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
 * File         : frontend/pt_memtrace/memtrace_fe.cc
 * Author       : Heiner Litz
 * Date         : 05/15/2020
 * Description  : Frontend to simulate traces in memtrace format
 ***************************************************************************************/

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
}

#include "bp/bp.param.h"

#include "bp/bp.h"
#include "frontend/pt_memtrace/memtrace_fe.h"
#include "isa/isa.h"
#include "pin/pin_lib/uop_generator.h"
#include "pin/pin_lib/x86_decoder.h"

#include "ctype_pin_inst.h"
#include "statistics.h"

#define REG(x) SCARAB_REG_##x,
typedef enum Scarab_Reg_Id_struct {
#include "isa/x86_regs.def"
  SCARAB_NUM_REGS
} Scarab_Reg_Id;
#undef REG

#define DR_DO_NOT_DEFINE_int64

#include <iostream>
#include <unordered_map>

#include "frontend/pt_memtrace/memtrace_trace_reader_memtrace.h"

/**************************************************************************************/
/* Global Variables */

static char* trace_files[MAX_NUM_PROCS];
TraceReader* trace_readers[MAX_NUM_PROCS];
// TODO: Make per proc?
uint64_t ins_id = 0;
uint64_t ins_id_fetched = 0;
uint64_t prior_tid = 0;
uint64_t prior_pid = 0;

Flag roi_dump_began = FALSE;
Counter roi_dump_ID = 0;

/**************************************************************************************/
/* Private Functions */

void fill_in_dynamic_info(ctype_pin_inst* info, const InstInfo* insi) {
  uint8_t ld = 0;
  uint8_t st = 0;

  info->instruction_addr = insi->pc;
  info->instruction_next_addr = insi->target;
  info->actually_taken = insi->taken;
  // For direct branches, fill_in_cf_info already set branch_target to the
  // static destination in the cached ctype_pin_inst.  Preserve it so the
  // BTB/predictor sees the correct target even for not-taken CBRs.
  // Indirect branches have a different target on every execution, so always
  // use the dynamic target from the trace.
  // For REGDEPS CBRs not yet observed taken, branch_target is still 0; mark
  // it ADDR_INVALID so downstream code never silently uses the fall-through.
  if (info->cf_type == CF_CBR) {
    // CBR: preserve static target from fill_in_cf_info / learning cache.
    // If still unknown (REGDEPS, never seen taken), mark ADDR_INVALID.
    if (info->branch_target == 0) {
      if (!insi->taken)
        info->branch_target = ADDR_INVALID;
      else
        info->branch_target = insi->target;
    }
  } else {
    // Everything else (BR, CALL, IBR, ICALL, RET, ICO, SYS, non-CF):
    // always use the dynamic target — covers indirect branches whose
    // target varies, and direct branches/jitted code whose target may change.
    info->branch_target = insi->target;
  }
  info->inst_uid = ins_id;
  info->last_inst_from_trace = insi->last_inst_from_trace;
  info->fetched_instruction = insi->fetched_instruction;

  if (info->cf_type == CF_RET)
    info->actually_taken = 1;

#ifdef PRINT_INSTRUCTION_INFO
  std::cout << std::hex << info->instruction_addr << " Next " << info->instruction_next_addr << " size "
            << (uint32_t)info->size << " taken " << (uint32_t)info->actually_taken << " target " << info->branch_target
            << " pid " << insi->pid << " tid " << insi->tid;
  std::cout << " uid " << std::dec << info->inst_uid << std::endl;
#endif

  assert(info->size);

  if (insi->mem_is_rd[0] || insi->mem_is_rd[1])
    assert(info->num_ld);
  if (insi->mem_is_wr[0] || insi->mem_is_wr[1])
    assert(info->num_st);

  if (!insi->mem_is_rd[0] && !insi->mem_is_rd[1]) {
    info->num_ld = 0;
  }
  if (!insi->mem_is_wr[0] && !insi->mem_is_wr[1]) {
    info->num_st = 0;
  }

  for (uint8_t op = 0; op < 2; op++) {
    if (!insi->mem_used[op])
      continue;
    if (insi->mem_is_rd[op]) {
      info->ld_vaddr[ld++] = insi->mem_addr[op];
    } else if (insi->mem_is_wr[op]) {
      info->st_vaddr[st++] = insi->mem_addr[op];
    }
  }
}

static bool is_xchg_rcx_rcx(const ctype_pin_inst* pi) {
  return pi->true_op_type == XED_ICLASS_XCHG && pi->num_src_regs > 1 && pi->src_regs[0] == SCARAB_REG_RCX &&
         pi->src_regs[1] == SCARAB_REG_RCX;
}

int ffwd(const ctype_pin_inst* pi) {
  if (!FAST_FORWARD) {
    return 0;
  }
  if (is_xchg_rcx_rcx(pi)) {
    return 0;
  }
  if ((USE_FETCHED_COUNT ? ins_id_fetched : ins_id) == FAST_FORWARD_TRACE_INS) {
    return 0;
  }
  return 1;
}

int roi(const ctype_pin_inst* pi) {
  return is_xchg_rcx_rcx(pi) ? 1 : 0;
}

int memtrace_trace_read(int proc_id, ctype_pin_inst* next_onpath_pi) {
  InstInfo* insi;

  do {
    insi = const_cast<InstInfo*>(trace_readers[proc_id]->nextInstruction());

    if (prior_pid == 0) {
      ASSERT(proc_id, prior_tid == 0);
      ASSERT(proc_id, insi->valid);
      prior_pid = insi->pid;
      prior_tid = insi->tid;
      ASSERT(proc_id, prior_tid);
      ASSERT(proc_id, prior_pid);
    }
    if (insi->valid) {
      if (insi->last_inst_from_trace) {
        std::cout << "Reached end of trace (last_inst) pc=0x" << std::hex << insi->pc << std::dec << std::endl;
        return 0;  // don't simulate the sentinel instruction
      }
      ins_id++;
      if (insi->fetched_instruction) {
        ins_id_fetched++;
      }
    } else {
      std::cout << "Reached end of trace pc=0x" << std::hex << insi->pc << std::dec << std::endl;
      return 0;  // end of trace
    }
  } while (insi->pid != prior_pid || insi->tid != prior_tid);

  // Static info (basic_info, deps, simd, cf, etc.) is pre-built in
  // processInst / processDrIsaInst and cached via ctype_inst_map.
  assert(insi->info != nullptr);
  memcpy(next_onpath_pi, insi->info, sizeof(ctype_pin_inst));
  fill_in_dynamic_info(next_onpath_pi, insi);

  if (next_onpath_pi->scarab_marker_roi_begin == true) {
    assert(!roi_dump_began);
    // reset stats
    std::cout << "Reached roi dump begin marker, reset stats" << std::endl;
    reset_stats(TRUE);
    roi_dump_began = TRUE;
  } else if (next_onpath_pi->scarab_marker_roi_end == true) {
    assert(roi_dump_began);
    // dump stats
    std::cout << "Reached roi dump end marker, dump stats between" << std::endl;
    dump_stats(proc_id, TRUE, global_stat_array[proc_id], NUM_GLOBAL_STATS);
    roi_dump_began = FALSE;
    roi_dump_ID++;
  }

  // End of ROI
  if (roi(next_onpath_pi))
    return 0;

  return 1;
}

/**************************************************************************************/
/* trace_init() */

void memtrace_init(void) {
  uop_generator_init(NUM_CORES);
  init_x86_decoder(nullptr);
  init_x87_stack_delta();

  // next_onpath_pi = (ctype_pin_inst*)malloc(NUM_CORES * sizeof(ctype_pin_inst));

  /* temp variable needed for easy initialization syntax */
  char* tmp_trace_files[MAX_NUM_PROCS] = {
      CBP_TRACE_R0,  CBP_TRACE_R1,  CBP_TRACE_R2,  CBP_TRACE_R3,  CBP_TRACE_R4,  CBP_TRACE_R5,  CBP_TRACE_R6,
      CBP_TRACE_R7,  CBP_TRACE_R8,  CBP_TRACE_R9,  CBP_TRACE_R10, CBP_TRACE_R11, CBP_TRACE_R12, CBP_TRACE_R13,
      CBP_TRACE_R14, CBP_TRACE_R15, CBP_TRACE_R16, CBP_TRACE_R17, CBP_TRACE_R18, CBP_TRACE_R19, CBP_TRACE_R20,
      CBP_TRACE_R21, CBP_TRACE_R22, CBP_TRACE_R23, CBP_TRACE_R24, CBP_TRACE_R25, CBP_TRACE_R26, CBP_TRACE_R27,
      CBP_TRACE_R28, CBP_TRACE_R29, CBP_TRACE_R30, CBP_TRACE_R31, CBP_TRACE_R32, CBP_TRACE_R33, CBP_TRACE_R34,
      CBP_TRACE_R35, CBP_TRACE_R36, CBP_TRACE_R37, CBP_TRACE_R38, CBP_TRACE_R39, CBP_TRACE_R40, CBP_TRACE_R41,
      CBP_TRACE_R42, CBP_TRACE_R43, CBP_TRACE_R44, CBP_TRACE_R45, CBP_TRACE_R46, CBP_TRACE_R47, CBP_TRACE_R48,
      CBP_TRACE_R49, CBP_TRACE_R50, CBP_TRACE_R51, CBP_TRACE_R52, CBP_TRACE_R53, CBP_TRACE_R54, CBP_TRACE_R55,
      CBP_TRACE_R56, CBP_TRACE_R57, CBP_TRACE_R58, CBP_TRACE_R59, CBP_TRACE_R60, CBP_TRACE_R61, CBP_TRACE_R62,
      CBP_TRACE_R63,
  };
  if (DUMB_CORE_ON) {
    // avoid errors by specifying a trace known to be good
    tmp_trace_files[DUMB_CORE] = tmp_trace_files[0];
  }

  for (uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    trace_files[proc_id] = tmp_trace_files[proc_id];
  }
  for (uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    memtrace_setup(proc_id);
  }
}

void memtrace_setup(uns proc_id) {
  std::string path(trace_files[proc_id]);
  std::string trace(path);

  trace_readers[proc_id] = new TraceReaderMemtrace(trace, 1);

  if (FAST_FORWARD) {
    ASSERT(proc_id, !MEMTRACE_ROI_BEGIN && !MEMTRACE_ROI_END);
    uint64_t inst_count_to_use = USE_FETCHED_COUNT ? ins_id_fetched : ins_id;
    std::cout << "Enter fast forward " << inst_count_to_use << std::endl;
    // FFWD the first instruction and as many as later ffwding parameters specify.
    // insi is invalid once end of trace is reached.
    // Reaching the end of the trace breaks out of the loop and segfaults later in this function.
    const InstInfo* insi;
    do {
      insi = trace_readers[proc_id]->nextInstruction();
      if (insi->valid) {
        ins_id++;
        if (insi->fetched_instruction) {
          ins_id_fetched++;
        }
      }

      inst_count_to_use = USE_FETCHED_COUNT ? ins_id_fetched : ins_id;

      if ((inst_count_to_use % 10000000) == 0)
        std::cout << "Fast forwarded " << inst_count_to_use << " instructions." << (insi->valid ? " Valid" : " Invalid")
                  << " instr." << std::endl;
    } while (ffwd(insi->info));
    std::cout << "Exit fast forward " << inst_count_to_use << std::endl;
  }
}
