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

#include "pin/pin_lib/x86_decoder.h"

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

uint8_t DR_ISA_TO_SCARAB_REG_MAP[NUM_DR_ISA_REGDEPS] = {
    [DR_ISA_REGDEPS_0] = SCARAB_REG_INV,    [DR_ISA_REGDEPS_1] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_2] = SCARAB_REG_RAX,    [DR_ISA_REGDEPS_3] = SCARAB_REG_RBX,
    [DR_ISA_REGDEPS_4] = SCARAB_REG_RCX,    [DR_ISA_REGDEPS_5] = SCARAB_REG_RDX,
    [DR_ISA_REGDEPS_6] = SCARAB_REG_RSI,    [DR_ISA_REGDEPS_7] = SCARAB_REG_RDI,
    [DR_ISA_REGDEPS_8] = SCARAB_REG_RBP,    [DR_ISA_REGDEPS_9] = SCARAB_REG_RSP,
    [DR_ISA_REGDEPS_10] = SCARAB_REG_R8,    [DR_ISA_REGDEPS_11] = SCARAB_REG_R9,
    [DR_ISA_REGDEPS_12] = SCARAB_REG_R10,   [DR_ISA_REGDEPS_13] = SCARAB_REG_R11,
    [DR_ISA_REGDEPS_14] = SCARAB_REG_R12,   [DR_ISA_REGDEPS_15] = SCARAB_REG_R13,
    [DR_ISA_REGDEPS_16] = SCARAB_REG_R14,   [DR_ISA_REGDEPS_17] = SCARAB_REG_R15,
    [DR_ISA_REGDEPS_18] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_19] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_20] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_21] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_22] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_23] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_24] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_25] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_26] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_27] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_28] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_29] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_30] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_31] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_32] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_33] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_34] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_35] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_36] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_37] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_38] = SCARAB_REG_CS,    [DR_ISA_REGDEPS_39] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_40] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_41] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_42] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_43] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_44] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_45] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_46] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_47] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_48] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_49] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_50] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_51] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_52] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_53] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_54] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_55] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_56] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_57] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_58] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_59] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_60] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_61] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_62] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_63] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_64] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_65] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_66] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_67] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_68] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_69] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_70] = SCARAB_REG_INV,   [DR_ISA_REGDEPS_71] = SCARAB_REG_INV,
    [DR_ISA_REGDEPS_72] = SCARAB_REG_ZMM0,  [DR_ISA_REGDEPS_73] = SCARAB_REG_ZMM1,
    [DR_ISA_REGDEPS_74] = SCARAB_REG_ZMM2,  [DR_ISA_REGDEPS_75] = SCARAB_REG_ZMM3,
    [DR_ISA_REGDEPS_76] = SCARAB_REG_ZMM4,  [DR_ISA_REGDEPS_77] = SCARAB_REG_ZMM5,
    [DR_ISA_REGDEPS_78] = SCARAB_REG_ZMM6,  [DR_ISA_REGDEPS_79] = SCARAB_REG_ZMM7,
    [DR_ISA_REGDEPS_80] = SCARAB_REG_ZMM8,  [DR_ISA_REGDEPS_81] = SCARAB_REG_ZMM9,
    [DR_ISA_REGDEPS_82] = SCARAB_REG_ZMM10, [DR_ISA_REGDEPS_83] = SCARAB_REG_ZMM11,
    [DR_ISA_REGDEPS_84] = SCARAB_REG_ZMM12, [DR_ISA_REGDEPS_85] = SCARAB_REG_ZMM13,
    [DR_ISA_REGDEPS_86] = SCARAB_REG_ZMM14, [DR_ISA_REGDEPS_87] = SCARAB_REG_ZMM15};

uint8_t dr_isa_to_scarab_reg(reg_id_t reg) {
  if (reg >= 0 && reg < NUM_DR_ISA_REGDEPS) {
    return DR_ISA_TO_SCARAB_REG_MAP[reg];
  }
  return SCARAB_REG_INV;  // Invalid or out-of-bounds
}

// Trace Reader
TraceReaderMemtrace::TraceReaderMemtrace(const std::string& _trace, uint32_t _bufsize)
    : TraceReader(_trace, _bufsize),
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

bool TraceReaderMemtrace::initTrace() {
  {
    // temporary scope only for reading filetype
    std::vector<dynamorio::drmemtrace::scheduler_t::input_workload_t> sched_inputs;
    sched_inputs.emplace_back(trace_);  // None ROI for scanning the filetype first

    dynamorio::drmemtrace::scheduler_t tmp_scheduler;
    if (tmp_scheduler.init(sched_inputs, 1, dynamorio::drmemtrace::scheduler_t::make_scheduler_serial_options()) !=
        dynamorio::drmemtrace::scheduler_t::STATUS_SUCCESS) {
      panic("failed to initialize tmp scheduler: %s", tmp_scheduler.get_error_string().c_str());
      return false;
    }

    // Detect trace type
    auto* tmp_stream = tmp_scheduler.get_stream(0);
    auto type = tmp_stream->get_filetype();
    ASSERT(0, type != 0 && "Filetype detection failed: got 0x0 (trace file is missing header)");

    trace_has_encodings_ = type & dynamorio::drmemtrace::OFFLINE_FILE_TYPE_ENCODINGS;
    if (type & dynamorio::drmemtrace::OFFLINE_FILE_TYPE_ARCH_REGDEPS) {
      dr_isa_mode_t dummy;
      dcontext_ = dr_standalone_init();
      dr_set_isa_mode(dcontext_, DR_ISA_REGDEPS, &dummy);
    } else {
      warn(
          "Warning: Scarab expects the trace file type to include OFFLINE_FILE_TYPE_ARCH_REGDEPS (0x%lx), but got "
          "type: 0x%lx\n",
          (unsigned long)dynamorio::drmemtrace::OFFLINE_FILE_TYPE_ARCH_REGDEPS, (unsigned long)type);
    }
  }

  std::vector<dynamorio::drmemtrace::scheduler_t::input_workload_t> sched_inputs;
  // memtrace region of interest provides a view of the trace only of interest
  // inst count satrt with 1
  // begin 0 is invalid
  // end is inclusive
  // end 0 is end of trace
  if (MEMTRACE_ROI_BEGIN) {
    ASSERT(0, MEMTRACE_ROI_BEGIN < MEMTRACE_ROI_END || MEMTRACE_ROI_END == 0);
    dynamorio::drmemtrace::scheduler_t::range_t roi(static_cast<uint64_t>(MEMTRACE_ROI_BEGIN),
                                                    static_cast<uint64_t>(MEMTRACE_ROI_END));
    sched_inputs.emplace_back(trace_, std::vector<dynamorio::drmemtrace::scheduler_t::range_t>{roi});
  } else {
    sched_inputs.emplace_back(trace_);
  }

  if (scheduler.init(sched_inputs, 1, dynamorio::drmemtrace::scheduler_t::make_scheduler_serial_options()) !=
      dynamorio::drmemtrace::scheduler_t::STATUS_SUCCESS) {
    panic("failed to initialize scheduler: %s", scheduler.get_error_string().c_str());
    return false;
  }

  // Set info 'A' to the first complete instruction.
  // It will initially lack branch target information.
  getNextInstruction__(&mt_info_a_, &mt_info_b_);
  mt_using_info_a_ = false;
  return true;
}

bool TraceReaderMemtrace::getNextInstruction__(InstInfo* _info, InstInfo* _prior) {
  static bool first_instr = true;
  uint32_t prior_isize = mt_prior_isize_;
  bool complete = false;

  auto* stream = scheduler.get_stream(0);

  if (mt_use_next_ref_) {
    // start with the next entry
    mt_status_ = stream->next_record(mt_ref_);
  } else {
    // mt_use_next_ref_ is false following a REP BUG
    // start with the current entry because it needs to be processed
    // otherwise it will be skipped
    assert(mt_status_ != dynamorio::drmemtrace::scheduler_t::STATUS_EOF);
    assert(mt_state_ == MTState::INST);
    // will use the next entry the next time if not set to false again
    mt_use_next_ref_ = true;
  }
  while (mt_status_ != dynamorio::drmemtrace::scheduler_t::STATUS_EOF) {
    if (mt_status_ != dynamorio::drmemtrace::scheduler_t::STATUS_OK) {
      panic("scheduler failed to advance: %d", mt_status_);
    }
    // there can be mt_ref types other than inst and mem
    // the FSM will skip those if they appear within MTState::INST state
    switch (mt_state_) {
      case (MTState::INST):
        if (type_is_instr(mt_ref_.instr.type)) {
          if (first_instr) {
            // if this is the first instruction ever,
            // the file type marker of the trace should have been processed internally by DynamoRIO.
            // it is time to see if encodings are available.
            first_instr = false;
            instr_t drinst;
            instr_init(dcontext_, &drinst);
            decode(dcontext_, mt_ref_.instr.encoding, &drinst);
            if (instr_get_isa_mode(&drinst) == DR_ISA_REGDEPS) {
              is_dr_isa = true;
            } else {
              is_dr_isa = false;
            }
          }
          if (is_dr_isa) {
            processDrIsaInst(_info, 0);
          } else {
            processInst(_info);
          }
          if (mt_mem_ops_ > 0) {
            mt_state_ = MTState::MEM1;
          } else {
            complete = true;
          }
        } else if (mt_ref_.instr.type == dynamorio::drmemtrace::TRACE_TYPE_INSTR_NO_FETCH) {
          // a repeated rep
          if (!is_dr_isa) {  // impossible to check DR_ISA_REGDEPS for REP
            bool is_rep = std::get<MAP_REP>(xed_map_.at(_prior->pc));
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
    mt_status_ = stream->next_record(mt_ref_);
  }
PATCH_REP:
  _info->valid &= complete;
  // Compute the branch target information for the prior instruction
  if (_info->valid) {
    bool is_rep = xed_map_.find(_prior->pc) != xed_map_.end() ? std::get<MAP_REP>(xed_map_.at(_prior->pc)) : 0;
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
      _prior->taken = non_seq;
    } else if (_prior->pc && non_seq &&
               (!is_rep || (_prior->pc != _info->pc && (_prior->pc + prior_isize) != _info->pc))) {
      _prior->ins = createJmp(_info->pc - _prior->pc);
      _prior->target = _info->pc;
      _prior->taken = true;
      _prior->mem_used[0] = false;
      _prior->mem_used[1] = false;
      _prior->is_dr_ins = false;
      ;
      warn("Patching gap in trace by injecting a Jmp, prior PC: %lx next PC: %lx\n", _prior->pc, _info->pc);
    }
    _prior->target = _info->pc;  // TODO(granta): Invalid for pid/tid switch
  } else {
    // for the last instruction of the trace, the npc cannot be set by the next one
    // set the npc to itself because of the frontend assertion
    // bp_recovery_info->recovery_fetch_addr == frontend_next_fetch_addr(proc_id)
    // at /home/mxu61_bak/scarab_hlitz/src/decoupled_frontend.cc
    _prior->last_inst_from_trace = true;
    _prior->target = _prior->pc;
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
  info->encoding_is_new = true;
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
      case TRACE_TYPE_INSTR_INDIRECT_JUMP:
        info->cf_type = CF_IBR;
        iclass = XED_ICLASS_JMP;
        break;
      case TRACE_TYPE_INSTR_CONDITIONAL_JUMP:
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
        info->cf_type = CF_CBR;
        iclass = XED_ICLASS_JNZ;
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

std::unordered_map<uint64_t, std::tuple<int, bool, bool, bool, ctype_pin_inst>> ctype_inst_map;


void TraceReaderMemtrace::processDrIsaInst(InstInfo* _info, bool has_another_mem) {
  assert(mt_ref_.instr.size);
  instr_t drinst;
  bool unknown_type, cond_branch;
  instr_init(dcontext_, &drinst);
  _info->pc = mt_ref_.instr.addr;
  auto ctype_inst_iter = ctype_inst_map.find(mt_ref_.instr.addr);
  if (mt_ref_.instr.encoding_is_new) {
    ctype_pin_inst cinst;
    memset(&cinst, 0, sizeof(cinst));
    decode(dcontext_, mt_ref_.instr.encoding, &drinst);

    fill_in_basic_info(&cinst, &drinst, mt_ref_.instr.size, mt_ref_.instr.type);
    add_dependency_info(&cinst, &drinst);
    ctype_inst_map.erase(mt_ref_.instr.addr);
    ctype_inst_map.emplace(mt_ref_.instr.addr,
                           std::make_tuple(cinst.num_ld + cinst.num_st, false, cinst.cf_type, false, cinst));
    ctype_inst_iter = ctype_inst_map.find(mt_ref_.instr.addr);
    // printf("PC %lx cat %i, str:%s ld %i st %i type %s \n", mt_ref_.instr.addr, instr_get_category(&drinst),
    // category_to_str(instr_get_category(&drinst)), cinst.num_ld, cinst.num_st,
    // trace_type_to_string(mt_ref_.instr.type));
  } else {
    assert(ctype_inst_iter != ctype_inst_map.end());
    std::get<MAP_XED>(ctype_inst_iter->second).encoding_is_new = false;
  }

  tie(mt_mem_ops_, unknown_type, cond_branch, std::ignore, std::ignore) = ctype_inst_iter->second;
  mt_prior_isize_ = mt_ref_.instr.size;
  _info->is_dr_ins = is_dr_isa;
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
}

void TraceReaderMemtrace::processInst(InstInfo* _info) {
  // Get the XED info from the cache, creating it if needed
  auto xed_map_iter = xed_map_.find(mt_ref_.instr.addr);
  if (xed_map_iter == xed_map_.end()) {
    if (trace_has_encodings_)
      fillCache(mt_ref_.instr.addr, mt_ref_.instr.size, mt_ref_.instr.encoding);
    else
      fillCache(mt_ref_.instr.addr, mt_ref_.instr.size);
    xed_map_iter = xed_map_.find(mt_ref_.instr.addr);
    assert((xed_map_iter != xed_map_.end()));
  }
  bool unknown_type, cond_branch;
  xed_decoded_inst_t* xed_ins;
  auto& xed_tuple = (*xed_map_iter).second;

  tie(mt_mem_ops_, unknown_type, cond_branch, std::ignore, std::ignore) = xed_tuple;
  mt_prior_isize_ = mt_ref_.instr.size;
  xed_ins = std::get<MAP_XED>(xed_tuple).get();

  xed_category_enum_t category = xed_decoded_inst_get_category(xed_ins);
  _info->is_dr_ins = false;
  _info->pc = mt_ref_.instr.addr;
  _info->ins = xed_ins;
  _info->pid = mt_ref_.instr.pid;
  _info->tid = mt_ref_.instr.tid;
  _info->target = 0;  // Set when the next instruction is evaluated
  // Set as taken if it's a branch.
  // Conditional branches are patched when the next instruction is evaluated.
  _info->taken = category == XED_CATEGORY_UNCOND_BR || category == XED_CATEGORY_COND_BR ||
                 category == XED_CATEGORY_CALL || category == XED_CATEGORY_RET;
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
