/* Copyright 2020 University of California Santa Cruz
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
 * File         : frontend/pt_memtrace/memtrace_trace_reader_memtrace.h
 * Author       : Heiner Litz
 * Date         : 05/15/2020
 * Description  :
 ***************************************************************************************/

#include "frontend/pt_memtrace/memtrace_trace_reader_memtrace.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "pin/pin_lib/gather_scatter_addresses.h"
#include "pin/pin_lib/x86_decoder.h"

#include "compressed_file_reader.h"
#include "directory_iterator.h"
#include "file_reader.h"
#include "snappy_file_reader.h"
#include "zipfile_file_reader.h"

extern scatter_info_map scatter_info_storage;

#include "dr_api.h"
#include "dr_ir_instr.h"
#include "elf.h"

#define warn(...) printf(__VA_ARGS__)
#define panic(...) printf(__VA_ARGS__)

#define REG(x) SCARAB_REG_##x,
typedef enum Reg_Id_struct {
#include "isa/x86_regs.def"
  SCARAB_NUM_REGS
} Reg_Id;
#undef REG

using namespace dynamorio::drmemtrace;

namespace {

// Ensures instr_free on every exit from getNextInstruction__ (loop break, goto PATCH_REP, return).
struct DrInstProbe {
  void* dcontext;
  instr_t instr = {};
  explicit DrInstProbe(void* dc) : dcontext(dc) { instr_init(dc, &instr); }
  ~DrInstProbe() { instr_free(dcontext, &instr); }
  DrInstProbe(const DrInstProbe&) = delete;
  DrInstProbe& operator=(const DrInstProbe&) = delete;
};

}  // namespace

const char* trace_type_to_string(dynamorio::drmemtrace::trace_type_t type) {
  switch (type) {
    case TRACE_TYPE_INSTR:
      return "TRACE_TYPE_INSTR";
    case TRACE_TYPE_INSTR_NO_FETCH:
      return "TRACE_TYPE_INSTR_NO_FETCH";
    case TRACE_TYPE_INSTR_INDIRECT_CALL:
      return "TRACE_TYPE_INSTR_INDIRECT_CALL";
    case TRACE_TYPE_INSTR_DIRECT_CALL:
      return "TRACE_TYPE_INSTR_DIRECT_CALL";
    case TRACE_TYPE_INSTR_RETURN:
      return "TRACE_TYPE_INSTR_RETURN";
    case TRACE_TYPE_INSTR_CONDITIONAL_JUMP:
      return "TRACE_TYPE_INSTR_CONDITIONAL_JUMP";
    case TRACE_TYPE_INSTR_TAKEN_JUMP:
      return "TRACE_TYPE_INSTR_TAKEN_JUMP";
    case TRACE_TYPE_INSTR_UNTAKEN_JUMP:
      return "TRACE_TYPE_INSTR_UNTAKEN_JUMP";
    case TRACE_TYPE_INSTR_DIRECT_JUMP:
      return "TRACE_TYPE_INSTR_DIRECT_JUMP";
    case TRACE_TYPE_INSTR_INDIRECT_JUMP:
      return "TRACE_TYPE_INSTR_INDIRECT_JUMP";
    case TRACE_TYPE_INSTR_SYSENTER:
      return "TRACE_TYPE_INSTR_SYSENTER";
    case TRACE_TYPE_INSTR_FLUSH:
      return "TRACE_TYPE_INSTR_FLUSH";
    case TRACE_TYPE_READ:
      return "TRACE_TYPE_READ";
    case TRACE_TYPE_WRITE:
      return "TRACE_TYPE_WRITE";
    case TRACE_TYPE_DATA_FLUSH:
      return "TRACE_TYPE_DATA_FLUSH";
    case TRACE_TYPE_PREFETCH:
      return "TRACE_TYPE_PREFETCH";
    case TRACE_TYPE_PREFETCHT0:
      return "TRACE_TYPE_PREFETCHT0";
    case TRACE_TYPE_PREFETCHT1:
      return "TRACE_TYPE_PREFETCHT1";
    case TRACE_TYPE_PREFETCHT2:
      return "TRACE_TYPE_PREFETCHT2";
    case TRACE_TYPE_PREFETCHNTA:
      return "TRACE_TYPE_PREFETCHNTA";
    case TRACE_TYPE_PREFETCH_READ:
      return "TRACE_TYPE_PREFETCH_READ";
    case TRACE_TYPE_PREFETCH_WRITE:
      return "TRACE_TYPE_PREFETCH_WRITE";
    case TRACE_TYPE_PREFETCH_INSTR:
      return "TRACE_TYPE_PREFETCH_INSTR";
    case TRACE_TYPE_HARDWARE_PREFETCH:
      return "TRACE_TYPE_HARDWARE_PREFETCH";
    case TRACE_TYPE_MARKER:
      return "TRACE_TYPE_MARKER";
    case TRACE_TYPE_THREAD_EXIT:
      return "TRACE_TYPE_THREAD_EXIT";
    default:
      return "TRACE_TYPE_UNKNOWN";
  }
}

typedef enum {
  DR_ISA_REGDEPS_0,
  DR_ISA_REGDEPS_1,
  DR_ISA_REGDEPS_2,
  DR_ISA_REGDEPS_3,
  DR_ISA_REGDEPS_4,
  DR_ISA_REGDEPS_5,
  DR_ISA_REGDEPS_6,
  DR_ISA_REGDEPS_7,
  DR_ISA_REGDEPS_8,
  DR_ISA_REGDEPS_9,
  DR_ISA_REGDEPS_10,
  DR_ISA_REGDEPS_11,
  DR_ISA_REGDEPS_12,
  DR_ISA_REGDEPS_13,
  DR_ISA_REGDEPS_14,
  DR_ISA_REGDEPS_15,
  DR_ISA_REGDEPS_16,
  DR_ISA_REGDEPS_17,
  DR_ISA_REGDEPS_18,
  DR_ISA_REGDEPS_19,
  DR_ISA_REGDEPS_20,
  DR_ISA_REGDEPS_21,
  DR_ISA_REGDEPS_22,
  DR_ISA_REGDEPS_23,
  DR_ISA_REGDEPS_24,
  DR_ISA_REGDEPS_25,
  DR_ISA_REGDEPS_26,
  DR_ISA_REGDEPS_27,
  DR_ISA_REGDEPS_28,
  DR_ISA_REGDEPS_29,
  DR_ISA_REGDEPS_30,
  DR_ISA_REGDEPS_31,
  DR_ISA_REGDEPS_32,
  DR_ISA_REGDEPS_33,
  DR_ISA_REGDEPS_34,
  DR_ISA_REGDEPS_35,
  DR_ISA_REGDEPS_36,
  DR_ISA_REGDEPS_37,
  DR_ISA_REGDEPS_38,
  DR_ISA_REGDEPS_39,
  DR_ISA_REGDEPS_40,
  DR_ISA_REGDEPS_41,
  DR_ISA_REGDEPS_42,
  DR_ISA_REGDEPS_43,
  DR_ISA_REGDEPS_44,
  DR_ISA_REGDEPS_45,
  DR_ISA_REGDEPS_46,
  DR_ISA_REGDEPS_47,
  DR_ISA_REGDEPS_48,
  DR_ISA_REGDEPS_49,
  DR_ISA_REGDEPS_50,
  DR_ISA_REGDEPS_51,
  DR_ISA_REGDEPS_52,
  DR_ISA_REGDEPS_53,
  DR_ISA_REGDEPS_54,
  DR_ISA_REGDEPS_55,
  DR_ISA_REGDEPS_56,
  DR_ISA_REGDEPS_57,
  DR_ISA_REGDEPS_58,
  DR_ISA_REGDEPS_59,
  DR_ISA_REGDEPS_60,
  DR_ISA_REGDEPS_61,
  DR_ISA_REGDEPS_62,
  DR_ISA_REGDEPS_63,
  DR_ISA_REGDEPS_64,
  DR_ISA_REGDEPS_65,
  DR_ISA_REGDEPS_66,
  DR_ISA_REGDEPS_67,
  DR_ISA_REGDEPS_68,
  DR_ISA_REGDEPS_69,
  DR_ISA_REGDEPS_70,
  DR_ISA_REGDEPS_71,
  DR_ISA_REGDEPS_72,
  DR_ISA_REGDEPS_73,
  DR_ISA_REGDEPS_74,
  DR_ISA_REGDEPS_75,
  DR_ISA_REGDEPS_76,
  DR_ISA_REGDEPS_77,
  DR_ISA_REGDEPS_78,
  DR_ISA_REGDEPS_79,
  DR_ISA_REGDEPS_80,
  DR_ISA_REGDEPS_81,
  DR_ISA_REGDEPS_82,
  DR_ISA_REGDEPS_83,
  DR_ISA_REGDEPS_84,
  DR_ISA_REGDEPS_85,
  DR_ISA_REGDEPS_86,
  DR_ISA_REGDEPS_87
} dr_isa_reg_t;

#define NUM_DR_ISA_REGDEPS 88

// Positional init in the same order as the DR_ISA_REGDEPS_N enum (0..87).
// Was previously written with C99 array designators (GCC extension in C++),
// which clang flags as clang-diagnostic-c99-designator.
uint8_t DR_ISA_TO_SCARAB_REG_MAP[NUM_DR_ISA_REGDEPS] = {
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 0, 1
    SCARAB_REG_RAX,   SCARAB_REG_RBX,    // 2, 3
    SCARAB_REG_RCX,   SCARAB_REG_RDX,    // 4, 5
    SCARAB_REG_RSI,   SCARAB_REG_RDI,    // 6, 7
    SCARAB_REG_RBP,   SCARAB_REG_RSP,    // 8, 9
    SCARAB_REG_R8,    SCARAB_REG_R9,     // 10, 11
    SCARAB_REG_R10,   SCARAB_REG_R11,    // 12, 13
    SCARAB_REG_R12,   SCARAB_REG_R13,    // 14, 15
    SCARAB_REG_R14,   SCARAB_REG_R15,    // 16, 17
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 18, 19
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 20, 21
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 22, 23
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 24, 25
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 26, 27
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 28, 29
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 30, 31
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 32, 33
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 34, 35
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 36, 37
    SCARAB_REG_CS,    SCARAB_REG_INV,    // 38, 39
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 40, 41
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 42, 43
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 44, 45
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 46, 47
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 48, 49
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 50, 51
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 52, 53
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 54, 55
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 56, 57
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 58, 59
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 60, 61
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 62, 63
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 64, 65
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 66, 67
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 68, 69
    SCARAB_REG_INV,   SCARAB_REG_INV,    // 70, 71
    SCARAB_REG_ZMM0,  SCARAB_REG_ZMM1,   // 72, 73
    SCARAB_REG_ZMM2,  SCARAB_REG_ZMM3,   // 74, 75
    SCARAB_REG_ZMM4,  SCARAB_REG_ZMM5,   // 76, 77
    SCARAB_REG_ZMM6,  SCARAB_REG_ZMM7,   // 78, 79
    SCARAB_REG_ZMM8,  SCARAB_REG_ZMM9,   // 80, 81
    SCARAB_REG_ZMM10, SCARAB_REG_ZMM11,  // 82, 83
    SCARAB_REG_ZMM12, SCARAB_REG_ZMM13,  // 84, 85
    SCARAB_REG_ZMM14, SCARAB_REG_ZMM15   // 86, 87
};
static_assert(sizeof(DR_ISA_TO_SCARAB_REG_MAP) == NUM_DR_ISA_REGDEPS, "map must cover every DR_ISA_REGDEPS_N");

uint8_t dr_isa_to_scarab_reg(reg_id_t reg) {
  if (reg >= 0 && reg < NUM_DR_ISA_REGDEPS) {
    return DR_ISA_TO_SCARAB_REG_MAP[reg];
  }
  return SCARAB_REG_INV;  // Invalid or out-of-bounds
}

// Trace Reader
TraceReaderMemtrace::TraceReaderMemtrace(const std::string& _trace, uint32_t _bufsize)
    : TraceReader(_trace, _bufsize),
      module_mapper_(nullptr),
      directory_(),
      dcontext_(nullptr),
      knob_verbose_(0),
      trace_has_encodings_(false),
      reader_at_eof_(false),
      reader_first_read_(true),
      roi_end_(0),
      mt_state_(MTState::INST),
      mt_use_next_ref_(true),
      mt_mem_ops_(0),
      mt_seq_(0),
      mt_prior_isize_(0),
      mt_using_info_a_(true),
      mt_warn_target_(0) {
  init(_trace);
}

TraceReaderMemtrace::~TraceReaderMemtrace() {
  if (mt_warn_target_ > 0) {
    warn("Set %lu conditional branches to 'not-taken' due to pid/tid gaps\n", mt_warn_target_);
  }
}

void TraceReaderMemtrace::init(const std::string& _trace) {
  mt_info_a_ = {};
  mt_info_b_ = {};
  mt_info_a_.custom_op = CustomOp::NONE;
  mt_info_a_.custom_op = CustomOp::NONE;
  mt_info_b_.custom_op = CustomOp::NONE;
  mt_info_a_.valid = true;
  mt_info_b_.valid = true;
  TraceReader::init(_trace);
}

// TODO: Detect memtrace/module.log type dynamically
#ifdef ZSIM_USE_YT
/* Below is required to parse Google Memtraces that contain an extra column */
const char* TraceReaderMemtrace::parse_buildid_string(const char* src, OUT void** data) {
  // We just skip the string.  We don't store it as we're not using it here.
  const char* comma = strchr(src, ',');
  if (comma == nullptr)
    return nullptr;
  return comma + 1;
}
#endif

static std::unique_ptr<reader_t> create_trace_reader(const std::string& path, int verbosity) {
  if (directory_iterator_t::is_directory(path)) {
    directory_iterator_t end;
    directory_iterator_t iter(path);
    for (; iter != end; ++iter) {
      const std::string fname = *iter;
      if (fname == "." || fname == "..")
        continue;
      if (ends_with(fname, ".sz"))
        return std::unique_ptr<reader_t>(new snappy_file_reader_t(path, verbosity));
      if (ends_with(fname, ".zip"))
        return std::unique_ptr<reader_t>(new zipfile_file_reader_t(path, verbosity));
    }
  } else {
    if (ends_with(path, ".sz"))
      return std::unique_ptr<reader_t>(new snappy_file_reader_t(path, verbosity));
    if (ends_with(path, ".zip"))
      return std::unique_ptr<reader_t>(new zipfile_file_reader_t(path, verbosity));
  }
  return std::unique_ptr<reader_t>(new compressed_file_reader_t(path, verbosity));
}

bool TraceReaderMemtrace::advanceReader() {
  if (reader_at_eof_)
    return false;
  if (reader_first_read_) {
    reader_first_read_ = false;
  } else {
    ++(*reader_);
  }
  if (*reader_ == *reader_end_) {
    reader_at_eof_ = true;
    return false;
  }
  // Allow one extra instruction past roi_end_ so the last ROI instruction
  // gets its target/npc set from the following instruction's PC.
  if (roi_end_ && reader_->get_instruction_ordinal() > roi_end_ + 1) {
    printf("advanceReader: ROI end reached (ordinal %lu > roi_end %lu)\n",
           (unsigned long)reader_->get_instruction_ordinal(), (unsigned long)roi_end_);
    fflush(stdout);
    reader_at_eof_ = true;
    return false;
  }
  mt_ref_ = **reader_;
  return true;
}

bool TraceReaderMemtrace::initTrace() {
  reader_ = create_trace_reader(trace_, 0);
  // EOF sentinel: default-constructed reader with at_eof_ = true
  reader_end_.reset(new compressed_file_reader_t());
  if (!reader_ || !reader_->init()) {
    panic("failed to initialize trace reader for: %s\n", trace_.c_str());
    return false;
  }

  // reader_t::init() only advances past the first visible marker (typically
  // VERSION).  We must continue advancing so the reader processes the FILETYPE
  // marker, which populates get_filetype().
  while (reader_->get_filetype() == 0) {
    if (!advanceReader())
      break;
  }
  auto type = reader_->get_filetype();
  ASSERT(0, type != 0 && "Filetype detection failed: got 0x0 (trace file is missing header)");

  if (dcontext_ == nullptr) {
    dcontext_ = dr_standalone_init();
  }

  trace_has_encodings_ = type & dynamorio::drmemtrace::OFFLINE_FILE_TYPE_ENCODINGS;
  if (type & dynamorio::drmemtrace::OFFLINE_FILE_TYPE_ARCH_REGDEPS) {
    dr_isa_mode_t dummy;
    dr_set_isa_mode(dcontext_, DR_ISA_REGDEPS, &dummy);
  } else {
    warn(
        "Warning: Scarab expects the trace file type to include OFFLINE_FILE_TYPE_ARCH_REGDEPS (0x%lx), but got "
        "type: 0x%lx\n",
        (unsigned long)dynamorio::drmemtrace::OFFLINE_FILE_TYPE_ARCH_REGDEPS, (unsigned long)type);
  }

  // Handle region of interest (instruction-count based)
  if (MEMTRACE_ROI_BEGIN) {
    ASSERT(0, MEMTRACE_ROI_BEGIN < MEMTRACE_ROI_END || MEMTRACE_ROI_END == 0);
    reader_->skip_instructions(static_cast<uint64_t>(MEMTRACE_ROI_BEGIN) - 1);
    roi_end_ = MEMTRACE_ROI_END ? static_cast<uint64_t>(MEMTRACE_ROI_END) : 0;
  }

  // Set info 'A' to the first complete instruction.
  // It will initially lack branch target information.
  getNextInstruction__(&mt_info_a_, &mt_info_b_);
  mt_using_info_a_ = false;
  return true;
}

std::unordered_map<uint64_t, std::tuple<int, bool, bool, bool, ctype_pin_inst>> ctype_inst_map;

bool TraceReaderMemtrace::getNextInstruction__(InstInfo* _info, InstInfo* _prior) {
  uint32_t prior_isize = mt_prior_isize_;
  bool complete = false;

  DrInstProbe probe(dcontext_);

  if (mt_use_next_ref_) {
    if (!advanceReader())
      reader_at_eof_ = true;
  } else {
    assert(!reader_at_eof_);
    assert(mt_state_ == MTState::INST);
    mt_use_next_ref_ = true;
  }
  while (!reader_at_eof_) {
    // there can be mt_ref types other than inst and mem
    // the FSM will skip those if they appear within MTState::INST state
    switch (mt_state_) {
      case (MTState::INST):
        if (type_is_instr(mt_ref_.instr.type)) {
          instr_reset(dcontext_, &probe.instr);
          decode(dcontext_, mt_ref_.instr.encoding, &probe.instr);
          const bool is_regdeps = (instr_get_isa_mode(&probe.instr) == DR_ISA_REGDEPS);
          if (is_regdeps) {
            processDrIsaInst(_info, 0, &probe.instr);
          } else {
            processInst(_info, &probe.instr);
          }
          if (mt_mem_ops_ > 0) {
            mt_state_ = MTState::MEM1;
          } else {
            complete = true;
          }
        } else if (mt_ref_.instr.type == dynamorio::drmemtrace::TRACE_TYPE_INSTR_NO_FETCH) {
          // a repeated rep — MAP_REP is not set for DR_ISA_REGDEPS (see processDrIsaInst tuple), so only assert for XED
          // path
          if (!_prior->is_dr_ins) {
            bool is_rep = std::get<MAP_REP>(ctype_inst_map.at(_prior->pc));
            assert(is_rep && ((uint32_t)mt_ref_.instr.pid == _prior->pid) &&
                   ((uint32_t)mt_ref_.instr.tid == _prior->tid) && (mt_ref_.instr.addr == _prior->pc));
          }
          // do not need to re-process
          *_info = *_prior;
          // flag this instruction as non-fetched
          _info->fetched_instruction = false;
          // mt_mem_ops_ set by the first rep occurance
          if (mt_mem_ops_ > 0) {
            mt_state_ = MTState::MEM1;
          } else {
            complete = true;
          }
        } else if (typeIsMem(mt_ref_.data.type)) {
          // Skip flush and thread exit types and
          // silently ignore memory operands of unknown instructions
          if (!_prior->unknown_type) {
            if (skipped_ == 0) {
              warn(
                  "Stray memory record detected at seq. %lu: PC: 0x%lx, "
                  "PID: %lu, TID: %lu, Addr: 0x%lx. "
                  "Suppressing further messages.\n",
                  mt_seq_, mt_ref_.data.pc, mt_ref_.data.pid, mt_ref_.data.tid, mt_ref_.data.addr);
            }
            skipped_++;
          }
        }
        break;
      case (MTState::MEM1):
        if (typeIsMem(mt_ref_.data.type)) {
          if (((uint32_t)_info->pid == mt_ref_.data.pid) && ((uint32_t)_info->tid == mt_ref_.data.tid) &&
              (_info->pc == mt_ref_.data.pc)) {
            _info->mem_addr[0] = mt_ref_.data.addr;
            _info->mem_used[0] = true;
            _info->mem_is_rd[0] = mt_ref_.data.type == dynamorio::drmemtrace::TRACE_TYPE_READ;
            _info->mem_is_wr[0] = mt_ref_.data.type == dynamorio::drmemtrace::TRACE_TYPE_WRITE;
            if (mt_mem_ops_ > 1) {
              mt_state_ = MTState::MEM2;
            } else {
              mt_state_ = MTState::INST;
              complete = true;
            }
          } else {
            warn("Unexpected PID/TID/PC switch following 0x%lx\n", _info->pc);
            mt_state_ = MTState::INST;
          }
        } else {
          // REP Instructions with REP count 0
          mt_mem_ops_ = 0;
          mt_state_ = MTState::INST;
          complete = true;
          // If we have already fetched the next instr (this) skip fetching once
          if (type_is_instr(mt_ref_.instr.type)) {
            mt_use_next_ref_ = false;
          }
          goto PATCH_REP;
        }
        break;
      case (MTState::MEM2):
        if (typeIsMem(mt_ref_.data.type)) {
          if (((uint32_t)_info->pid == mt_ref_.data.pid) && ((uint32_t)_info->tid == mt_ref_.data.tid) &&
              (_info->pc == mt_ref_.data.pc)) {
            _info->mem_addr[1] = mt_ref_.data.addr;
            _info->mem_used[1] = true;
            _info->mem_is_rd[1] = mt_ref_.data.type == dynamorio::drmemtrace::TRACE_TYPE_READ;
            _info->mem_is_wr[1] = mt_ref_.data.type == dynamorio::drmemtrace::TRACE_TYPE_WRITE;
            assert(mt_mem_ops_ <= 2);
            mt_state_ = MTState::INST;
            complete = true;
          } else {
            warn("Unexpected PID/TID/PC switch following 0x%lx\n", _info->pc);
            mt_state_ = MTState::INST;
          }
        } else {
          warn("Expected data2 but found type '%s'\n", dynamorio::drmemtrace::trace_type_names[mt_ref_.data.type]);
          mt_state_ = MTState::INST;
        }
        break;
    }
    mt_seq_++;
    if (complete) {
      break;
    }
    // advance to the next entry if the instruction has not yet completed
    if (!advanceReader())
      reader_at_eof_ = true;
  }
PATCH_REP:
  _info->valid &= complete;
  // Compute the branch target information for the prior instruction
  if (_info->valid) {
    auto ctype_prior_iter = ctype_inst_map.find(_prior->pc);
    bool is_rep = (ctype_prior_iter != ctype_inst_map.end()) ? std::get<MAP_REP>(ctype_prior_iter->second) : false;
    bool non_seq = _info->pc != (_prior->pc + prior_isize);

    if (_prior->taken) {  // currently set iif branch
      bool new_gid = (_prior->tid != _info->tid) || (_prior->pid != _info->pid);
      if (new_gid) {
        // TODO(granta): If there are enough of these, it may make sense to
        // delay conditional branch instructions until the thread resumes even
        // though this alters the apparent order of the trace.
        // (Seeking ahead to resolve the branch info is a non-starter.)
        if (mt_warn_target_ == 0) {
          warn(
              "Detected a conditional branch preceding a pid/tid change "
              "at seq. %lu. Assuming not-taken. Suppressing further "
              "messages.\n",
              mt_seq_ - 1);
        }
        mt_warn_target_++;
        non_seq = false;
      }
      // non_seq tells us whether the branch was taken for conditional branches.
      // Unconditional branches are always taken even if the target equals fall-through.
      if (_prior->info && _prior->info->cf_type != CF_CBR && _prior->info->cf_type != CF_REP)
        _prior->taken = true;
      else
        _prior->taken = non_seq;
      // For REGDEPS traces the static branch target for direct CBRs is
      // unavailable from the synthetic encoding.  Learn it from the first
      // taken instance so that subsequent not-taken CBRs still expose the
      // correct target to the BTB.  Indirect branches are excluded because
      // their target varies per execution.

      if (non_seq && _prior->info && _prior->info->cf_type == CF_CBR) {
        if (_prior->info->branch_target != (Addr)_info->pc) {
          const_cast<ctype_pin_inst*>(_prior->info)->branch_target = _info->pc;
          const_cast<ctype_pin_inst*>(_prior->info)->encoding_is_new = true;
        }
      }
    } else if (_prior->pc && non_seq &&
               (!is_rep || (_prior->pc != _info->pc && (_prior->pc + prior_isize) != _info->pc))) {
      gap_patch_jmp_ = create_dummy_jump(_prior->pc, _info->pc, XED_ICLASS_JMP);
      _prior->info = &gap_patch_jmp_;
      _prior->target = _info->pc;
      _prior->taken = true;
      _prior->mem_addr[0] = 0;
      _prior->mem_addr[1] = 0;
      _prior->mem_used[0] = false;
      _prior->mem_used[1] = false;
      _prior->mem_is_rd[0] = false;
      _prior->mem_is_rd[1] = false;
      _prior->mem_is_wr[0] = false;
      _prior->mem_is_wr[1] = false;
      _prior->is_dr_ins = false;
      warn("Patching gap in trace by injecting a Jmp, prior PC: %lx next PC: %lx\n", _prior->pc, _info->pc);
    }
    _prior->target = _info->pc;  // TODO(granta): Invalid for pid/tid switch
  } else {
    // Last instruction of the trace (EOF or ROI): the next PC cannot be
    // derived from a following instruction.  Pretend the branch is not-taken
    // and set target to the fall-through address so the branch predictor
    // sees consistent npc == pc + size.
    _prior->last_inst_from_trace = true;
    _prior->taken = false;
    _prior->target = _prior->pc + prior_isize;
  }

  return complete;
}

uint32_t TraceReaderMemtrace::add_dependency_info(ctype_pin_inst* info, instr_t* drinst) {
  uint32_t max_op_width = 1;
  info->ld_size = 8;
  info->st_size = 8;
  bool two_ops_first = true;
  uint32_t mem_regs = info->num_ld + info->num_st;

  assert(mem_regs < 3);
  assert(!info->num_src_regs);
  assert(!info->num_dst_regs);
  assert(!info->num_ld1_addr_regs);
  assert(!info->num_ld2_addr_regs);
  assert(!info->num_st_addr_regs);

  /* Handle register sources. The Google traces do not contain info about
     whether operands are memory operands or regular register ops. We do some
     guessing, ie. assign one source register to each memory operand. It should not matter
     much as all source dependencis should be considered (regular and mem regs) */
  for (int i = 0; i < instr_num_srcs(drinst); ++i) {
    opnd_t src = instr_get_src(drinst, i);
    if (opnd_is_reg(src)) {
      reg_id_t dr_reg = opnd_get_reg(src);
      uint8_t scarab_reg = dr_isa_to_scarab_reg(dr_reg);

      if (mem_regs == 0) {
        add_reg(info, SRC_REGS, scarab_reg);  // regular src register
      } else if (info->num_ld && info->num_st) {
        if (two_ops_first) {
          add_reg(info, (Reg_Array_Id)(LD1_ADDR_REGS), scarab_reg);
          two_ops_first = false;
          mem_regs--;
        } else {
          add_reg(info, (Reg_Array_Id)(ST_ADDR_REGS), scarab_reg);
          mem_regs--;
        }
      } else if (info->num_ld) {
        add_reg(info, (Reg_Array_Id)(LD1_ADDR_REGS), scarab_reg);
        mem_regs--;
      } else if (info->num_st) {
        add_reg(info, (Reg_Array_Id)(ST_ADDR_REGS), scarab_reg);
        mem_regs--;
      }
    }
  }

  // Handle register destinations
  for (int i = 0; i < instr_num_dsts(drinst); ++i) {
    opnd_t dst = instr_get_dst(drinst, i);
    if (opnd_is_reg(dst)) {
      reg_id_t dr_reg = opnd_get_reg(dst);
      uint8_t scarab_reg = dr_isa_to_scarab_reg(dr_reg);
      add_reg(info, DST_REGS, scarab_reg);
    }
  }

  return max_op_width;
}

const char* category_to_str(uint cat) {
  static char result[256];
  result[0] = '\0';  // clear buffer

  if (cat & DR_INSTR_CATEGORY_UNCATEGORIZED)
    strcat(result, "UNCATEGORIZED ");
  if (cat & DR_INSTR_CATEGORY_FP)
    strcat(result, "FP ");
  if (cat & DR_INSTR_CATEGORY_LOAD)
    strcat(result, "LOAD ");
  if (cat & DR_INSTR_CATEGORY_STORE)
    strcat(result, "STORE ");
  if (cat & DR_INSTR_CATEGORY_BRANCH)
    strcat(result, "BRANCH ");
  if (cat & DR_INSTR_CATEGORY_SIMD)
    strcat(result, "SIMD ");
  if (cat & DR_INSTR_CATEGORY_STATE)
    strcat(result, "STATE ");
  if (cat & DR_INSTR_CATEGORY_MOVE)
    strcat(result, "MOVE ");
  if (cat & DR_INSTR_CATEGORY_CONVERT)
    strcat(result, "CONVERT ");
  if (cat & DR_INSTR_CATEGORY_MATH)
    strcat(result, "MATH ");
  if (cat & DR_INSTR_CATEGORY_OTHER)
    strcat(result, "OTHER ");

  if (result[0] == '\0')
    return "UNKNOWN";

  // remove trailing space
  result[strlen(result) - 1] = '\0';
  return result;
}

const uint CAT_SIMD_LOAD = DR_INSTR_CATEGORY_SIMD | DR_INSTR_CATEGORY_LOAD;
const uint CAT_SIMD_STORE = DR_INSTR_CATEGORY_SIMD | DR_INSTR_CATEGORY_STORE;
const uint CAT_LOAD_STORE = DR_INSTR_CATEGORY_LOAD | DR_INSTR_CATEGORY_STORE;
const uint CAT_LOAD = DR_INSTR_CATEGORY_LOAD;
const uint CAT_STORE = DR_INSTR_CATEGORY_STORE;
const uint CAT_FP_LOAD = DR_INSTR_CATEGORY_FP | DR_INSTR_CATEGORY_LOAD;
const uint CAT_FP_STORE = DR_INSTR_CATEGORY_FP | DR_INSTR_CATEGORY_STORE;
const uint CAT_FP_LOAD_SIMD = DR_INSTR_CATEGORY_FP | DR_INSTR_CATEGORY_LOAD | DR_INSTR_CATEGORY_SIMD;
const uint CAT_FP_STORE_SIMD = DR_INSTR_CATEGORY_FP | DR_INSTR_CATEGORY_STORE | DR_INSTR_CATEGORY_SIMD;
const uint CAT_MOVE = DR_INSTR_CATEGORY_MOVE;
const uint CAT_SIMD = DR_INSTR_CATEGORY_SIMD;

void TraceReaderMemtrace::fill_in_basic_info(ctype_pin_inst* info, instr_t* drinst, size_t size, trace_type_t type) {
  uint cat = instr_get_category(drinst);
  xed_iclass_enum_t iclass = XED_ICLASS_INVALID;
  info->cf_type = NOT_CF;
  info->lane_width_bytes = instr_get_operation_size(drinst);
  info->num_simd_lanes = 4;  // Guess
  info->num_st = (cat & DR_INSTR_CATEGORY_STORE) ? 1 : 0;
  info->num_ld = (cat & DR_INSTR_CATEGORY_LOAD) ? 1 : 0;
  info->is_fp = (cat & DR_INSTR_CATEGORY_FP) ? 1 : 0;
  info->is_simd = (cat & DR_INSTR_CATEGORY_SIMD) ? 1 : 0;

  /* Reverse-engineer XED_ICLASS from the limited amount of information
     that the filtered Google traces provide */
  if (cat & DR_INSTR_CATEGORY_BRANCH) {
    switch (type) {
      case TRACE_TYPE_INSTR_DIRECT_JUMP:
        info->cf_type = CF_BR;
        iclass = XED_ICLASS_JMP;
        break;
      case TRACE_TYPE_INSTR:  // For some branches, DRINST does not state type, they seem to be direct jumps
      case TRACE_TYPE_INSTR_INDIRECT_JUMP:
        info->cf_type = CF_IBR;
        iclass = XED_ICLASS_JMP;
        break;
      case TRACE_TYPE_INSTR_CONDITIONAL_JUMP:
      case TRACE_TYPE_INSTR_TAKEN_JUMP:
      case TRACE_TYPE_INSTR_UNTAKEN_JUMP:
        info->cf_type = CF_CBR;
        iclass = XED_ICLASS_JNZ;
        break;
      case TRACE_TYPE_INSTR_DIRECT_CALL:
        info->cf_type = CF_CALL;
        iclass = XED_ICLASS_CALL_NEAR;
        break;
      case TRACE_TYPE_INSTR_INDIRECT_CALL:
        info->cf_type = CF_ICALL;
        iclass = XED_ICLASS_CALL_NEAR;
        break;
      case TRACE_TYPE_INSTR_RETURN:
        info->cf_type = CF_RET;
        iclass = XED_ICLASS_RET_NEAR;
        break;
      default:
        printf("fill_in_basic_info: branch with unknown trace type %d at pc=0x%lx cat=0x%x size=%u\n", (int)type,
               (unsigned long)info->instruction_addr, (unsigned)cat, (unsigned)size);
        fflush(stdout);
        info->cf_type = CF_CBR;
        iclass = XED_ICLASS_JNZ;
        assert(0);
        break;
    }
  } else if (cat & DR_INSTR_CATEGORY_MATH) {
    auto op_size = instr_get_operation_size(drinst);
    if (cat & DR_INSTR_CATEGORY_FP && cat & DR_INSTR_CATEGORY_SIMD) {
      switch (op_size) {
        case OPSZ_4:
          iclass = XED_ICLASS_ADDPS;
          break;
        case OPSZ_8:
          iclass = XED_ICLASS_ADDPD;
          break;
        default:
          iclass = XED_ICLASS_ADDPS;
          // TODO: Handle other sizes explictly
          break;
      }
    } else if (cat & DR_INSTR_CATEGORY_FP) {
      switch (op_size) {
        case OPSZ_4:
          iclass = XED_ICLASS_ADDSS;
          break;
        case OPSZ_8:
          iclass = XED_ICLASS_ADDSD;
          break;
        default:
          iclass = XED_ICLASS_ADDSS;
          // TODO: Handle other sizes explictly
          break;
      }
    } else if (cat & DR_INSTR_CATEGORY_SIMD) {
      switch (op_size) {
        case 1:
          iclass = XED_ICLASS_PADDB;
          break;
        case 2:
          iclass = XED_ICLASS_PADDW;
          break;
        case 4:
          iclass = XED_ICLASS_PADDD;
          break;
        case 8:
          iclass = XED_ICLASS_PADDQ;
          break;
        default:
          iclass = XED_ICLASS_PADDQ;
          // TODO: Handle other sizes explictly
          break;
      }
    } else {
      iclass = XED_ICLASS_ADD;
    }
  } else {
    switch (cat) {
      case CAT_SIMD_LOAD:
      case CAT_SIMD_STORE:
        iclass = XED_ICLASS_MOVAPS;
        break;

      case CAT_FP_LOAD_SIMD:
      case CAT_FP_STORE_SIMD:
        iclass = XED_ICLASS_MOVAPS;
        break;

      case CAT_FP_LOAD:
      case CAT_FP_STORE:
        iclass = XED_ICLASS_MOVSS;
        break;

      case CAT_LOAD_STORE:
      case CAT_LOAD:
      case CAT_STORE:
      case CAT_MOVE:
        iclass = XED_ICLASS_MOV;
        break;

      case CAT_SIMD:
        iclass = XED_ICLASS_PSHUFD;
        break;

      default:
        iclass = XED_ICLASS_ADD;
        break;
    }
  }

  info->size = size;
  assert(info->size);
  info->true_op_type = 0;
  info->op_type = iclass_to_scarab(iclass).opcode;
  info->is_string = false;
  info->is_call = false;
  info->is_move = (iclass == XED_ICLASS_MOV);
  info->is_prefetch = false;
  info->has_push = false;
  info->has_pop = false;
  info->is_lock = false;
  info->is_repeat = false;
  info->is_gather_scatter = false;

  // Note that for DR_ISA the instr_length (encoding bytes) is unequal to size (nextPC-PC)
  for (int ii = 0; (ii < 8) && (ii < instr_length(dcontext_, drinst)); ii++) {
    info->inst_binary_lsb = (info->inst_binary_lsb << 8) + instr_get_raw_byte(drinst, ii);
  }
  for (int ii = 8; (ii < 16) && (ii < instr_length(dcontext_, drinst)); ii++) {
    info->inst_binary_msb = (info->inst_binary_msb << 8) + instr_get_raw_byte(drinst, ii);
  }

  info->scarab_marker_roi_begin = false;
  info->scarab_marker_roi_end = false;
}

void TraceReaderMemtrace::processDrIsaInst(InstInfo* _info, bool has_another_mem, instr_t* predecoded) {
  assert(mt_ref_.instr.size);
  bool unknown_type, cond_branch;
  _info->pc = mt_ref_.instr.addr;
  if (mt_ref_.instr.encoding_is_new) {
    ctype_inst_map.erase(mt_ref_.instr.addr);
  }
  auto ctype_inst_iter = ctype_inst_map.find(mt_ref_.instr.addr);
  if (mt_ref_.instr.encoding_is_new) {
    assert(predecoded != nullptr);
    ctype_pin_inst cinst = {};

    fill_in_basic_info(&cinst, predecoded, mt_ref_.instr.size, mt_ref_.instr.type);
    add_dependency_info(&cinst, predecoded);
    cinst.encoding_is_new = mt_ref_.instr.encoding_is_new;
    ctype_inst_map.emplace(mt_ref_.instr.addr,
                           std::make_tuple(cinst.num_ld + cinst.num_st, false, cinst.cf_type, false, cinst));
    ctype_inst_iter = ctype_inst_map.find(mt_ref_.instr.addr);
  } else {
    assert(ctype_inst_iter != ctype_inst_map.end());
    std::get<MAP_XED>(ctype_inst_iter->second).encoding_is_new = mt_ref_.instr.encoding_is_new;
  }

  tie(mt_mem_ops_, unknown_type, cond_branch, std::ignore, std::ignore) = ctype_inst_iter->second;
  mt_prior_isize_ = mt_ref_.instr.size;
  _info->is_dr_ins = true;
  _info->info = &(std::get<MAP_XED>(ctype_inst_iter->second));
  _info->pid = mt_ref_.instr.pid;
  _info->tid = mt_ref_.instr.tid;
  _info->target = 0;  // Set when the next instruction is evaluated
  // Set as taken if it's a branch.
  // Conditional branches are patched when the next instruction is evaluated.
  _info->taken = std::get<MAP_XED>(ctype_inst_iter->second).cf_type;
  _info->mem_addr[0] = 0;
  _info->mem_addr[1] = 0;
  _info->mem_used[0] = false;
  _info->mem_used[1] = false;
  _info->mem_is_rd[0] = false;
  _info->mem_is_rd[1] = false;
  _info->mem_is_wr[0] = false;
  _info->mem_is_wr[1] = false;
  _info->unknown_type = unknown_type;
  // correct this later at getNextInstruction if it is the last instruction
  _info->last_inst_from_trace = false;
  // non-fetched instructions will be set within FSM
  _info->fetched_instruction = true;
  _info->dr_trace_type = (int)mt_ref_.instr.type;
}

void TraceReaderMemtrace::processInst(InstInfo* _info, [[maybe_unused]] instr_t* predecoded_dr) {
  // Same bytes as predecoded_dr; XED decode below is still required for mem-op/REP/SIMD/x87/cf helpers.
  _info->pc = mt_ref_.instr.addr;
  mt_prior_isize_ = mt_ref_.instr.size;

  if (mt_ref_.instr.encoding_is_new) {
    ctype_inst_map.erase(mt_ref_.instr.addr);
  }
  auto ctype_inst_iter = ctype_inst_map.find(mt_ref_.instr.addr);
  if (ctype_inst_iter == ctype_inst_map.end()) {
    // XED decode into a stack-local inst (only the ctype_pin_inst is cached)
    xed_decoded_inst_t xed_inst;
    xed_decoded_inst_zero_set_mode(&xed_inst, &xed_state_);
    uint8_t* loc = nullptr;
    bool unknown_type = false;
    if (trace_has_encodings_) {
      loc = mt_ref_.instr.encoding;
    } else {
      uint64_t size;
      if (!locationForVAddr(mt_ref_.instr.addr, &loc, &size)) {
        unknown_type = true;
      }
    }

    uint8_t isize = static_cast<uint8_t>(mt_ref_.instr.size);
    if (!unknown_type) {
      xed_error_enum_t xed_decode_res = xed_decode(&xed_inst, loc, isize);
      if (xed_decode_res != XED_ERROR_NONE) {
        assert(!trace_has_encodings_ || !"decode should not fail with trace-provided encodings");
        warn("XED decode error for 0x%lx: %s %u, replacing with nop\n", mt_ref_.instr.addr,
             xed_error_enum_t2str(xed_decode_res), isize);
        xed_inst = *makeNop(isize);
      }
    } else {
      assert(!trace_has_encodings_ || !"should not reach unknown_type path with trace-provided encodings");
      xed_inst = *makeNop(isize);
    }

    xed_decoded_inst_t* xed_ins = &xed_inst;

    // Count memory ops (same logic as fillCache)
    int n_used_mem_ops = 0;
    uint32_t n_mem_ops = xed_decoded_inst_number_of_memory_operands(xed_ins);
    if (n_mem_ops > 0) {
      xed_category_enum_t category = xed_decoded_inst_get_category(xed_ins);
      if (category != XED_CATEGORY_NOP && category != XED_CATEGORY_WIDENOP) {
        for (uint32_t i = 0; i < n_mem_ops; i++) {
          if (xed_decoded_inst_mem_read(xed_ins, i))
            n_used_mem_ops++;
          if (xed_decoded_inst_mem_written(xed_ins, i))
            n_used_mem_ops++;
        }
      }
    }

    bool is_rep = xed_decoded_inst_get_attribute(xed_ins, XED_ATTRIBUTE_REP) > 0;

    // Build the ctype_pin_inst
    ctype_pin_inst cinst;
    init_ctype_pin_inst(&cinst);
    cinst.instruction_addr = mt_ref_.instr.addr;

    ::fill_in_basic_info(&cinst, xed_ins);
    if (XED_INS_IsVgather(xed_ins) || XED_INS_IsVscatter(xed_ins)) {
      xed_category_enum_t cat = XED_INS_Category(xed_ins);
      scatter_info_storage[mt_ref_.instr.addr] = add_to_gather_scatter_info_storage(
          mt_ref_.instr.addr, XED_INS_IsVgather(xed_ins), XED_INS_IsVscatter(xed_ins), cat);
    }
    uint32_t max_op_width = ::add_dependency_info(&cinst, xed_ins);
    fill_in_simd_info(&cinst, xed_ins, max_op_width);
    apply_x87_bug_workaround(&cinst, xed_ins);
    fill_in_cf_info(&cinst, xed_ins);
    cinst.encoding_is_new = mt_ref_.instr.encoding_is_new;

    ctype_inst_map.emplace(mt_ref_.instr.addr,
                           std::make_tuple(n_used_mem_ops, unknown_type, cinst.cf_type != NOT_CF, is_rep, cinst));
    ctype_inst_iter = ctype_inst_map.find(mt_ref_.instr.addr);
  } else {
    std::get<MAP_XED>(ctype_inst_iter->second).encoding_is_new = mt_ref_.instr.encoding_is_new;
  }

  bool unknown_type, cond_branch;
  tie(mt_mem_ops_, unknown_type, cond_branch, std::ignore, std::ignore) = ctype_inst_iter->second;

  _info->is_dr_ins = false;
  _info->info = &(std::get<MAP_XED>(ctype_inst_iter->second));
  _info->pid = mt_ref_.instr.pid;
  _info->tid = mt_ref_.instr.tid;
  _info->target = 0;
  _info->taken = std::get<MAP_XED>(ctype_inst_iter->second).cf_type != NOT_CF;
  _info->mem_addr[0] = 0;
  _info->mem_addr[1] = 0;
  _info->mem_used[0] = false;
  _info->mem_used[1] = false;
  _info->mem_is_rd[0] = false;
  _info->mem_is_rd[1] = false;
  _info->mem_is_wr[0] = false;
  _info->mem_is_wr[1] = false;
  _info->unknown_type = unknown_type;
  _info->last_inst_from_trace = false;
  _info->fetched_instruction = true;
  _info->dr_trace_type = (int)mt_ref_.instr.type;
}

bool TraceReaderMemtrace::typeIsMem(dynamorio::drmemtrace::trace_type_t _type) {
  return ((_type == dynamorio::drmemtrace::TRACE_TYPE_READ) || (_type == dynamorio::drmemtrace::TRACE_TYPE_WRITE) ||
          type_is_prefetch(_type));
}

const InstInfo* TraceReaderMemtrace::getNextInstruction() {
  InstInfo& info = (mt_using_info_a_ ? mt_info_a_ : mt_info_b_);
  InstInfo& prior = (mt_using_info_a_ ? mt_info_b_ : mt_info_a_);
  mt_using_info_a_ = !mt_using_info_a_;
  if (getNextInstruction__(&info, &prior)) {
    return &prior;
  } else if (prior.valid) {
    // the last instruction's npc cannot be set by the trace
    // but it is a valid instruction
    ASSERT(0, !info.valid);
    return &prior;
  } else {
    return &invalid_info_;
  }
}

bool TraceReaderMemtrace::locationForVAddr(uint64_t _vaddr, uint8_t** _loc, uint64_t* _size) {
  assert(module_mapper_ != nullptr && "Module mapper is not initialized");

  app_pc module_start;
  size_t module_size;

  *_loc = module_mapper_->find_mapped_trace_bounds(reinterpret_cast<app_pc>(_vaddr), &module_start, &module_size);
  *_size = reinterpret_cast<uint64_t>(module_size) -
           (reinterpret_cast<uint64_t>(*_loc) - reinterpret_cast<uint64_t>(module_start));
  if (!module_mapper_->get_last_error().empty()) {
    std::cout << "Failed to find mapped address: " << std::hex << _vaddr
              << " Error: " << module_mapper_->get_last_error() << std::endl;
    return false;
  }
  return true;
}
