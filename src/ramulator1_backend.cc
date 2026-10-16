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
 * File         : ramulator1_backend.cc
 * Author       : Litz Lab
 * Date         : 10/2026
 * Description  : DramBackend over the vendored Ramulator 1.0 (src/ramulator/): builds
 *                its Config from the ramulator_* params, and turns its per-command
 *                callbacks into the POWER_DRAM_* stats.
 ***************************************************************************************/

#include <string>

#include "ramulator/Config.h"
#include "ramulator/Request.h"
#include "ramulator/ScarabWrapper.h"

extern "C" {
#include "globals/assert.h"

#include "general.param.h"
#include "memory/memory.param.h"
#include "ramulator.param.h"

#include "statistics.h"
}

#include "dram_backend.h"

using namespace ramulator;

namespace {

void stats_callback(int coreid, int type) {
  switch (type) {
    case int(StatCallbackType::DRAM_ACT):
      STAT_EVENT(coreid, POWER_DRAM_ACTIVATE);
      break;
    case int(StatCallbackType::DRAM_PRE):
      STAT_EVENT(coreid, POWER_DRAM_PRECHARGE);
      break;
    case int(StatCallbackType::DRAM_READ):
      STAT_EVENT(coreid, POWER_DRAM_READ);
      break;
    case int(StatCallbackType::DRAM_WRITE):
      STAT_EVENT(coreid, POWER_DRAM_WRITE);
      break;
  }
}

void init_configs(Config* configs) {
  configs->set_core_num(NUM_CORES);  // This has to be 1. We do not instantiate
                                     // CPU cores in Ramulator when using it
                                     // with Scarab as opposed to when using
                                     // Ramulator standalone. So, this should
                                     // not have any effect other than printing
                                     // "per-core" statistics in the output
                                     // file.

  configs->add("standard", RAMULATOR_STANDARD);
  configs->add("speed", RAMULATOR_SPEED);
  configs->add("org", RAMULATOR_ORG);

  configs->add("channels", std::to_string(RAMULATOR_CHANNELS));
  configs->add("ranks", std::to_string(RAMULATOR_RANKS));
  configs->add("bank_groups", std::to_string(RAMULATOR_BANKGROUPS));
  configs->add("banks", std::to_string(RAMULATOR_BANKS));
  configs->add("rows", std::to_string(RAMULATOR_ROWS));
  configs->add("columns", std::to_string(RAMULATOR_COLS));

  configs->add("chip_width", std::to_string(RAMULATOR_CHIP_WIDTH));
  configs->add("channel_width", std::to_string(BUS_WIDTH_IN_BYTES * 8));

  configs->add("record_cmd_trace", RAMULATOR_REC_CMD_TRACE);
  configs->add("print_cmd_trace", RAMULATOR_PRINT_CMD_TRACE);
  configs->add("use_rest_of_addr_as_row_addr", RAMULATOR_USE_REST_OF_ADDR_AS_ROW_ADDR);

  configs->add("scheduling_policy", RAMULATOR_SCHEDULING_POLICY);
  configs->add("readq_entries", std::to_string(RAMULATOR_READQ_ENTRIES));
  configs->add("writeq_entries", std::to_string(RAMULATOR_WRITEQ_ENTRIES));
  configs->add("output_dir", OUTPUT_DIR);

  // TODO: make these optional and use the preset values specified by
  // RAMULATOR_SPEED for timings that are not explicitly provided in
  // ramulator.param.def
  configs->add("tCK", std::to_string(RAMULATOR_TCK));
  configs->add("tCL", std::to_string(RAMULATOR_TCL));
  configs->add("tCCD", std::to_string(RAMULATOR_TCCD));
  configs->add("tCCDS", std::to_string(RAMULATOR_TCCDS));
  configs->add("tCCDL", std::to_string(RAMULATOR_TCCDL));
  configs->add("tCWL", std::to_string(RAMULATOR_TCWL));
  configs->add("tBL", std::to_string(RAMULATOR_TBL));
  configs->add("tWTR", std::to_string(RAMULATOR_TWTR));
  configs->add("tWTRS", std::to_string(RAMULATOR_TWTRS));
  configs->add("tWTRL", std::to_string(RAMULATOR_TWTRL));
  configs->add("tRP", std::to_string(RAMULATOR_TRP));
  configs->add("tRPpb", std::to_string(RAMULATOR_TRPpb));
  configs->add("tRPab", std::to_string(RAMULATOR_TRPab));
  configs->add("tRCD", std::to_string(RAMULATOR_TRCD));
  configs->add("tRCDR", std::to_string(RAMULATOR_TRCDR));
  configs->add("tRCDW", std::to_string(RAMULATOR_TRCDW));
  configs->add("tRAS", std::to_string(RAMULATOR_TRAS));
}

class Ramulator1Backend : public DramBackend {
 public:
  explicit Ramulator1Backend(Read_Done_Func read_done) : read_done(read_done) {
    init_configs(&configs);
    wrapper = new ScarabWrapper(configs, DCACHE_LINE_SIZE, &stats_callback);
  }

  ~Ramulator1Backend() override { delete wrapper; }

  bool send(long addr, bool is_write, int proc_id) override {
    Request req;
    req.type = is_write ? Request::Type::WRITE : Request::Type::READ;
    req.addr = addr;
    req.coreid = proc_id;
    /* The controller calls back only for reads; writes are acknowledged by the glue. */
    req.callback = [this](Request& done) {
      ASSERTM(0, done.type == Request::Type::READ, "ERROR: Responses should be sent only for read requests! \n");
      read_done(done.addr);
    };
    return wrapper->send(req);
  }

  void tick() override { wrapper->tick(); }

  void finish() override { wrapper->finish(); }

  int get_chip_width() const override { return wrapper->get_chip_width(); }
  int get_chip_size() const override { return wrapper->get_chip_size(); }
  int get_num_chips() const override { return wrapper->get_num_chips(); }
  int get_chip_row_buffer_size() const override { return wrapper->get_chip_row_buffer_size(); }

 private:
  Read_Done_Func read_done;
  Config configs;
  ScarabWrapper* wrapper = NULL;
};

}  // namespace

DramBackend* ramulator1_backend_create(DramBackend::Read_Done_Func read_done) {
  return new Ramulator1Backend(read_done);
}
