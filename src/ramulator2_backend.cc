/*
 * Copyright 2026 University of California Santa Cruz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : ramulator2_backend.cc
 * Author       : Litz Lab
 * Date         : 10/2026
 * Description  : DramBackend over Ramulator 2 (src/deps/ramulator2), configured by the
 *                YAML file in ramulator2_config (see src/ramulator2/gen_configs.py).
 ***************************************************************************************/

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ramulator/base/base.h"
#include "ramulator/base/config.h"
#include "ramulator/base/param.h"
#include "ramulator/base/request.h"
#include "ramulator/controller/controller_base.h"
#include "ramulator/controller/plugin/i_controller_plugin.h"
#include "ramulator/dram/dram_spec.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"

extern "C" {
#include "globals/assert.h"
#include "globals/utils.h"

#include "general.param.h"
#include "memory/memory.param.h"
#include "ramulator.param.h"

#include "ramulator.h"
#include "statistics.h"
}

#include "dram_backend.h"

namespace Ramulator {

/* Ramulator 2 needs a frontend to connect the memory system to, and its controllers size their
   per-core stats from get_num_cores(). Scarab sends requests to the memory system directly; this
   frontend only reports how many cores can appear as a request's source_id. */
class ScarabFrontEnd : public IFrontEnd, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, ScarabFrontEnd, "ScarabFrontEnd")

  int m_num_cores = 1;

 public:
  void init() override {
    RAMULATOR_PARSE_PARAM(m_clock_ratio, unsigned int, "clock_ratio").required();
    RAMULATOR_PARSE_PARAM(m_num_cores, int, "num_cores").required();
  }
  void tick() override {}
  bool is_finished() override { return false; }
  int get_num_cores() override { return m_num_cores; }
};

/* Counts every command a controller issues into POWER_DRAM_{ACTIVATE,PRECHARGE,READ,WRITE}, which
   the power model and DVFS read, classified as Ramulator 1 did (Controller.h issue_cmd): opening
   commands are activates; closing ones (PRE*, RDA, WRA) precharges; accessing commands named RD*
   reads and WR* writes. A command with no requesting core (refresh, row-policy closes) is
   counted on core 0, as in Ramulator 1. */
class ScarabCmdStats : public IControllerPlugin, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, ScarabCmdStats, "ScarabCmdStats")

  enum : int {
    ACTIVATE = 1,
    PRECHARGE = 2,
    READ = 4,
    WRITE = 8
  };
  std::vector<int> m_kind;  // command id -> ACTIVATE | PRECHARGE | READ | WRITE bits

 public:
  inline static int s_instances = 0;  // checked against the controller count after construction

  void init() override { s_instances++; }

  void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
    const DRAMSpec& spec = *cast_parent<ControllerBase>()->m_device.m_spec;
    m_kind.assign(spec.command_count, 0);
    for (int cmd = 0; cmd < spec.command_count; cmd++) {
      const DRAMCommandMeta& meta = spec.command_meta[cmd];
      const std::string& name = spec.command_names[cmd];
      m_kind[cmd] = (meta.is_opening ? ACTIVATE : 0) | (meta.is_closing ? PRECHARGE : 0) |
                    (meta.is_accessing && !name.compare(0, 2, "RD") ? READ : 0) |
                    (meta.is_accessing && !name.compare(0, 2, "WR") ? WRITE : 0);
    }
  }

  void on_issue(const Request& req) override {
    int kind = m_kind[req.command];
    uns proc_id = req.source_id >= 0 ? req.source_id : 0;
    if (kind & ACTIVATE)
      STAT_EVENT(proc_id, POWER_DRAM_ACTIVATE);
    if (kind & PRECHARGE)
      STAT_EVENT(proc_id, POWER_DRAM_PRECHARGE);
    if (kind & READ)
      STAT_EVENT(proc_id, POWER_DRAM_READ);
    if (kind & WRITE)
      STAT_EVENT(proc_id, POWER_DRAM_WRITE);
  }
};

}  // namespace Ramulator

namespace {

uns log2_exact(uint64_t value, const char* what) {
  ASSERTM(0, value && !(value & (value - 1)), "Ramulator 2: %s (%llu) must be a power of two\n", what,
          (unsigned long long)value);
  uns bits = 0;
  while ((1ull << bits) < value)
    bits++;
  return bits;
}

class Ramulator2Backend : public DramBackend {
 public:
  explicit Ramulator2Backend(Read_Done_Func read_done) : read_done(read_done) {
    ASSERTM(0, RAMULATOR2_CONFIG, "dram_backend ramulator2 needs --ramulator2_config <file.yaml>\n");
    ASSERTM(0, std::ifstream(RAMULATOR2_CONFIG).good(), "Cannot read ramulator2_config %s\n", RAMULATOR2_CONFIG);
    ASSERTM(0, !strcmp(RAMULATOR_SCHEDULING_POLICY, "FRFCFS"),
            "Ramulator 2 implements FRFCFS only; ramulator_scheduling_policy is %s\n", RAMULATOR_SCHEDULING_POLICY);
    try {
      init();
    } catch (const std::exception& e) {
      FATAL_ERROR(0, "Ramulator 2 could not load %s: %s\n", RAMULATOR2_CONFIG, e.what());
    }
  }

  ~Ramulator2Backend() override {
    delete memory_system;
    delete frontend;
  }

  bool send(long addr, bool is_write, int proc_id) override {
    long dram_addr = fold(addr);
#ifndef NO_DEBUG
    count_collision(addr, dram_addr);
#endif
    Ramulator::Request req(dram_addr, is_write ? Ramulator::Request::Type::Write : Ramulator::Request::Type::Read,
                           proc_id, nullptr);
    req.size_bytes = DCACHE_LINE_SIZE;
    /* Writes get no callback: Ramulator 2 would call it when the WR issues, or inside send when
       the write coalesces, and the glue acknowledges a write as soon as send accepts it. The
       callback carries the unfolded address, because two lines can fold to the same one. */
    if (!is_write)
      req.callback = [this, addr](Ramulator::Request&) { read_done(addr); };
    bool accepted = memory_system->send(req);
    if (accepted)
      (is_write ? writes_by_core : reads_by_core)[proc_id]++;
    return accepted;
  }

  void tick() override { memory_system->tick(); }

  void finish() override {
    frontend->finalize();
    memory_system->finalize();
    std::string path = std::string(OUTPUT_DIR) + "/ramulator2.stats.yaml";
    std::ofstream out(path);
    ASSERTM(0, out.good(), "Cannot write %s\n", path.c_str());
    memory_system->print_stats(out);
    write_ramulator1_stats();
#ifndef NO_DEBUG
    DPRINTF(
        "Ramulator 2 address fold: capacity 2^%u bytes, row field %u bits; %zu DRAM lines used, %llu "
        "lines above capacity, %zu lines folded onto a DRAM line another line already used\n",
        capacity_bits, row_bits, first_line_at.size(), lines_above_capacity, colliding_lines.size());
#endif
  }

  void get_config(Ramulator_Config* config) const override { *config = dram_config; }

  /* ramulator.stat.out in Ramulator 1's format, for scripts that read it (scarab-infra loads every
     "ramulator.*" line). A name is reused only for a quantity that means the same thing; the queue
     lengths are the exception, and their descriptions say how Ramulator 2 counts them. Ramulator 1
     stats Ramulator 2 has no counterpart for (active cycles, useless activates) are left out. */
  void write_ramulator1_stats() const {
    std::string path = std::string(OUTPUT_DIR) + "/ramulator.stat.out";
    FILE* out = fopen(path.c_str(), "w");
    ASSERTM(0, out, "Cannot write %s\n", path.c_str());
    /* Counts print as integers, averages with 6 decimals, like Ramulator 1. */
    auto text = [](double value) {
      char buf[64];
      if (value == std::floor(value) && std::fabs(value) < 1e18)
        snprintf(buf, sizeof(buf), "%lld", (long long)value);
      else
        snprintf(buf, sizeof(buf), "%f", value);
      return std::string(buf);
    };
    auto line = [out, &text](const std::string& name, double value, const char* desc) {
      fprintf(out, "%40s %20s    # %s\n", ("ramulator." + name).c_str(), text(value).c_str(), desc);
    };
    auto elem = [out, &text](size_t i, double value) {
      fprintf(out, "%40s %20s    # \n", ("[" + std::to_string(i) + "]").c_str(), text(value).c_str());
    };

    Ramulator::ConfigNode stats = memory_system->collect_stats();
    Ramulator::ConfigNode controller_stats = stats["controller"];
    std::vector<Ramulator::ConfigNode> channels;
    if (controller_stats.is_sequence())
      channels.assign(controller_stats.seq().begin(), controller_stats.seq().end());
    else
      channels.push_back(controller_stats);
    auto get = [](const Ramulator::ConfigNode& node, const std::string& name) { return node[name].as<double>(); };
    double tx_bytes = memory_system->get_tx_bytes();

    line("dram_cycles", get(channels[0], "cycles"), "Number of DRAM cycles simulated");
    line("incoming_requests", get(stats, "total_num_read_requests") + get(stats, "total_num_write_requests"),
         "Number of incoming requests to DRAM");
    line("read_requests", get(stats, "total_num_read_requests"), "Number of incoming read requests to DRAM per core");
    for (size_t i = 0; i < reads_by_core.size(); i++)
      elem(i, reads_by_core[i]);
    line("write_requests", get(stats, "total_num_write_requests"),
         "Number of incoming write requests to DRAM per core");
    for (size_t i = 0; i < writes_by_core.size(); i++)
      elem(i, writes_by_core[i]);
    double incoming = 0, incoming_reads = 0;
    for (auto& ch : channels) {
      incoming += get(ch, "num_read_reqs") + get(ch, "num_write_reqs");
      incoming_reads += get(ch, "num_read_reqs");
    }
    line("incoming_requests_per_channel", incoming, "Number of incoming requests to each DRAM channel");
    for (size_t c = 0; c < channels.size(); c++)
      elem(c, get(channels[c], "num_read_reqs") + get(channels[c], "num_write_reqs"));
    line("incoming_read_reqs_per_channel", incoming_reads, "Number of incoming read requests to each DRAM channel");
    for (size_t c = 0; c < channels.size(); c++)
      elem(c, get(channels[c], "num_read_reqs"));

    for (size_t c = 0; c < channels.size(); c++) {
      const Ramulator::ConfigNode& ch = channels[c];
      std::string id = std::to_string(c);
      for (const char* res : {"hits", "misses", "conflicts"}) {
        line(std::string("row_") + res + "_channel_" + id + "_core", get(ch, std::string("row_") + res),
             "Number of row hits/misses/conflicts per channel per core");
        line(std::string("read_row_") + res + "_channel_" + id + "_core", get(ch, std::string("read_row_") + res),
             "Number of row hits/misses/conflicts for read requests per channel per core");
        for (uns core = 0; core < NUM_CORES; core++)
          elem(core, get(ch, std::string("read_row_") + res + "_core_" + std::to_string(core)));
        line(std::string("write_row_") + res + "_channel_" + id + "_core", get(ch, std::string("write_row_") + res),
             "Number of row hits/misses/conflicts for write requests per channel");
      }
      /* Ramulator 1 left reads forwarded from the write queue out of the latency sum; Ramulator 2
         counts each of them as exactly 1 cycle. */
      double latency_sum = get(ch, "read_latency") - get(ch, "num_read_reqs_forwarded");
      double reads = get(ch, "num_read_reqs");
      line("read_latency_avg_" + id, reads ? latency_sum / reads : 0,
           "The average memory latency cycles (in memory time domain) per request for all read requests in this "
           "channel");
      line("read_latency_sum_" + id, latency_sum,
           "The memory latency cycles (in memory time domain) sum for all read requests in this channel");
      double cycles = get(ch, "cycles");
      const char* how =
          "Ramulator 2: requests in its read/write/priority buffers, which exclude rows being opened "
          "and reads waiting for data";
      for (auto& [r1_name, r2_name] : {std::pair<std::string, std::string>{"req", "queue_len"},
                                       {"read_req", "read_queue_len"},
                                       {"write_req", "write_queue_len"}}) {
        line(r1_name + "_queue_length_avg_" + id, cycles ? get(ch, r2_name) / cycles : 0, how);
        line(r1_name + "_queue_length_sum_" + id, get(ch, r2_name), how);
      }
      line("read_transaction_bytes_" + id, get(ch, "num_read_reqs_served") * tx_bytes,
           "The total byte of read transaction per channel");
      line("write_transaction_bytes_" + id, get(ch, "num_write_reqs_served") * tx_bytes,
           "The total byte of write transaction per channel");
    }
    line("maximum_bandwidth", peak_bandwidth_Bps, "The theoretical maximum bandwidth (Bps)");
    fclose(out);
  }

  int get_chip_width() const override { return chip_width; }
  int get_chip_size() const override { return chip_size_mbit; }
  int get_num_chips() const override { return num_chips; }
  int get_chip_row_buffer_size() const override { return chip_row_buffer_bits; }

 private:
  void init() {
    Ramulator::ConfigNode config = Ramulator::Config::parse_config_file(RAMULATOR2_CONFIG);

    Ramulator::ConfigNode fe(Ramulator::ConfigNode::Map{});
    fe.set("impl", "ScarabFrontEnd");
    fe.set("clock_ratio", 1);
    fe.set("num_cores", (int)NUM_CORES);
    config.set("frontend", fe);

    /* The controllers' request buffers are the ramulator_*q_entries params; L1_MSHRS <= the read
       buffer is what guarantees a read is never refused (checked in ramulator_init). */
    Ramulator::ConfigNode memory = config["memory_system"];
    Ramulator::ConfigNode controller_list = memory["controllers"];  // operator[] returns a copy
    const auto& controllers = controller_list.seq();
    ASSERTM(0, !controllers.empty(), "%s has no controllers\n", RAMULATOR2_CONFIG);
    Ramulator::ConfigNode sized;
    for (Ramulator::ConfigNode controller : controllers) {
      controller.set("read_buffer_size", (int)RAMULATOR_READQ_ENTRIES);
      controller.set("write_buffer_size", (int)RAMULATOR_WRITEQ_ENTRIES);
      Ramulator::ConfigNode plugins;  // Ramulator 2 reads a child list from "<interface name>s"
      if (controller["controller_plugins"])
        plugins = controller["controller_plugins"];
      Ramulator::ConfigNode cmd_stats(Ramulator::ConfigNode::Map{});
      cmd_stats.set("impl", "ScarabCmdStats");
      plugins.push_back(cmd_stats);
      controller.set("controller_plugins", plugins);
      sized.push_back(controller);
    }
    memory.set("controllers", sized);
    config.set("memory_system", memory);

    check_org(controllers);

    frontend = Ramulator::Factory::create_frontend(config);
    memory_system = Ramulator::Factory::create_memory_system(config);
    frontend->connect_memory_system(memory_system);
    memory_system->connect_frontend(frontend);
    ASSERTM(0, Ramulator::ScarabCmdStats::s_instances == (int)controllers.size(),
            "Ramulator 2 created %d ScarabCmdStats plugins for %zu controllers\n",
            Ramulator::ScarabCmdStats::s_instances, controllers.size());

    /* Scarab's memory clock is RAMULATOR_TCK (freq.c), fixed before the backend exists. Ramulator 2
       stores tCK in whole picoseconds (833 for DDR4-2400), so allow less than 1 ps of difference. */
    long long tck_fs = std::llround(memory_system->get_tCK() * 1e6);
    ASSERTM(0, std::llabs((long long)RAMULATOR_TCK - tck_fs) < 1000, "ramulator_tCK is %u fs but %s has tCK %lld fs\n",
            RAMULATOR_TCK, RAMULATOR2_CONFIG, tck_fs);
    ASSERTM(0, (uns)memory_system->get_tx_bytes() == DCACHE_LINE_SIZE,
            "%s moves %d bytes per request, but a Scarab line is %u bytes\n", RAMULATOR2_CONFIG,
            memory_system->get_tx_bytes(), DCACHE_LINE_SIZE);
  }

  /* The ramulator_* organization params must describe the same DRAM as the YAML, so a PARAMS file
     cannot silently disagree with the config it names. */
  void check_org(const Ramulator::ConfigNode::Seq& controllers) {
    const Ramulator::ConfigNode& controller = controllers[0];
    std::string standard = controller["dram"]["impl"].as<std::string>();
    ASSERTM(0, standard == "DDR4", "Ramulator 2 backend: only DDR4 is supported so far, %s is %s\n", RAMULATOR2_CONFIG,
            standard.c_str());
    std::unique_ptr<Ramulator::DRAMSpec> spec = Ramulator::DRAMSpec::create(standard, controller);

    struct {
      const char* param;
      uns expected;
      int actual;
    } org[] = {
        {"ramulator_channels", RAMULATOR_CHANNELS, (int)controllers.size()},
        {"ramulator_ranks", RAMULATOR_RANKS, spec->get_level_size("Rank")},
        {"ramulator_bankgroups", RAMULATOR_BANKGROUPS, spec->get_level_size("BankGroup")},
        {"ramulator_banks", RAMULATOR_BANKS, spec->get_level_size("Bank")},
        {"ramulator_rows", RAMULATOR_ROWS, spec->get_level_size("Row")},
        {"ramulator_cols", RAMULATOR_COLS, spec->get_level_size("Column")},
        {"ramulator_chip_width", RAMULATOR_CHIP_WIDTH, spec->organization.dq},
        {"bus_width_in_bytes * 8", BUS_WIDTH_IN_BYTES * 8, spec->channel_width},
    };
    for (auto& o : org)
      ASSERTM(0, (int)o.expected == o.actual, "%s is %u but %s has %d\n", o.param, o.expected, RAMULATOR2_CONFIG,
              o.actual);
    for (const Ramulator::ConfigNode& other : controllers)
      ASSERTM(0, same_dram(controller["dram"], other["dram"]), "All channels in %s must use the same DRAM\n",
              RAMULATOR2_CONFIG);

    uint64_t ranks = spec->get_level_size("Rank"), bankgroups = spec->get_level_size("BankGroup");
    uint64_t banks = spec->get_level_size("Bank"), rows = spec->get_level_size("Row");
    uint64_t cols = spec->get_level_size("Column"), dq = spec->organization.dq;
    uint64_t channel_bytes = ranks * bankgroups * banks * rows * cols * spec->channel_width / 8;
    capacity_bits = log2_exact(channel_bytes * controllers.size(), "DRAM capacity in bytes");
    row_bits = log2_exact(rows, "row count");

    peak_bandwidth_Bps = controllers.size() * spec->get_timing_value("rate") * 1e6 * spec->channel_width / 8;

    chip_width = dq;
    chip_size_mbit = bankgroups * banks * rows * cols * dq / (1024 * 1024);
    num_chips = controllers.size() * ranks * spec->channel_width / dq;
    chip_row_buffer_bits = cols * dq;

    dram_config.tck_fs = RAMULATOR_TCK;
    dram_config.channels = controllers.size();
    dram_config.ranks = ranks;
    dram_config.banks = banks;
    dram_config.readq_entries = RAMULATOR_READQ_ENTRIES;
    dram_config.writeq_entries = RAMULATOR_WRITEQ_ENTRIES;
    dram_config.tCL = spec->get_timing_value("nCL");
    dram_config.tCWL = spec->get_timing_value("nCWL");
    dram_config.tBL = spec->get_timing_value("nBL");
    dram_config.tRCD = spec->get_timing_value("nRCD");
    dram_config.tRP = spec->get_timing_value("nRP");
    dram_config.tRAS = spec->get_timing_value("nRAS");
    dram_config.tCCD = spec->get_timing_value("nCCDL");
    dram_config.tWTR = spec->get_timing_value("nWTRL");
  }

  static std::string scalars(const Ramulator::ConfigNode& seq) {
    std::string out;
    for (const Ramulator::ConfigNode& v : seq.seq())
      out += v.as<std::string>() + ",";
    return out;
  }

  static bool same_dram(const Ramulator::ConfigNode& a, const Ramulator::ConfigNode& b) {
    return a["impl"].as<std::string>() == b["impl"].as<std::string>() &&
           a["org"]["dq"].as<int>() == b["org"]["dq"].as<int>() &&
           scalars(a["org"]["count"]) == scalars(b["org"]["count"]) && scalars(a["timing"]) == scalars(b["timing"]) &&
           a["channel_width"].as<int>() == b["channel_width"].as<int>();
  }

  /* Ramulator 2 drops address bits above the DRAM capacity, so pages that differ only there would
     share rows. Scarab's physical addresses keep page identity in exactly those bits (addr_trans.c),
     so mix them into the top row_bits below the capacity, which is the row field of RoBaRaCoCh.
     The bits are hashed rather than XORed in directly: with a plain XOR fold, 1-6% of the lines of
     the regression traces landed on another line's DRAM line, far above what a random mapping gives
     (addr_translate already XORs part of the page index into both halves); hashed, it is at that
     level. Addresses below the capacity are unchanged. */
  long fold(long addr) const {
    uint64_t a = (uint64_t)addr;
    uint64_t low = a & ((1ull << capacity_bits) - 1);
    uint64_t high = a >> capacity_bits;
    if (!high)
      return (long)low;
    uint64_t z = high + 0x9e3779b97f4a7c15ull;  // splitmix64 finalizer
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    uint64_t row_mask = (1ull << row_bits) - 1;
    return (long)(low ^ ((z & row_mask) << (capacity_bits - row_bits)));
  }

#ifndef NO_DEBUG
  void count_collision(long addr, long dram_addr) {
    uint64_t line = (uint64_t)addr / DCACHE_LINE_SIZE, dram_line = (uint64_t)dram_addr / DCACHE_LINE_SIZE;
    auto it = first_line_at.find(dram_line);
    if (it == first_line_at.end()) {
      first_line_at.emplace(dram_line, line);
      lines_above_capacity += line != dram_line;
    } else if (it->second != line) {
      colliding_lines.insert(line);
    }
  }
  std::unordered_map<uint64_t, uint64_t> first_line_at;  // folded line -> first full line sent there
  std::unordered_set<uint64_t> colliding_lines;          // full lines that landed on another's DRAM line
  unsigned long long lines_above_capacity = 0;
#endif

  Read_Done_Func read_done;
  Ramulator::IFrontEnd* frontend = nullptr;
  Ramulator::IMemorySystem* memory_system = nullptr;
  uns capacity_bits = 0, row_bits = 0;
  int chip_width = 0, chip_size_mbit = 0, num_chips = 0, chip_row_buffer_bits = 0;
  Ramulator_Config dram_config = {};
  double peak_bandwidth_Bps = 0;
  std::vector<unsigned long long> reads_by_core = std::vector<unsigned long long>(NUM_CORES);
  std::vector<unsigned long long> writes_by_core = std::vector<unsigned long long>(NUM_CORES);
};

}  // namespace

DramBackend* ramulator2_backend_create(DramBackend::Read_Done_Func read_done) {
  return new Ramulator2Backend(read_done);
}
