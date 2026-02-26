extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "bp/bp.param.h"
#include "memory/memory.param.h"
}
#include <iostream>

#include "bp/bp.h"
#include "frontend/synthetic/synth_fe.h"
#include "frontend/synthetic/synthetic_kernels.h"
#include "pin/pin_lib/uop_generator.h"

#include "ctype_pin_inst.h"
#include "kernel_params.h"
// #define PRINT_INSTRUCTION_INFO
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_SYNTHETIC_INST, ##args)

/* intrinsic frontend variables */
static ctype_pin_inst next_onpath_pi[MAX_NUM_PROCS];
static ctype_pin_inst next_offpath_pi[MAX_NUM_PROCS][MAX_NUM_BPS];
static bool off_path_mode[MAX_NUM_PROCS][MAX_NUM_BPS] = {false};
static uint64_t off_path_addr[MAX_NUM_PROCS][MAX_NUM_BPS] = {0};

void synth_init() {
  kernel = static_cast<Kernel_Enum>(KERNEL);
  uop_generator_init(NUM_CORES);
  synthetic_kernel_init();

  for (uns proc_id{0}; proc_id < NUM_CORES; proc_id++) {
    next_onpath_pi[proc_id] = synthetic_fe_generate_next(proc_id, false);
  }
}

void synth_done() {
}

Addr synth_next_fetch_addr(uns proc_id) {
  return next_onpath_pi[proc_id].instruction_addr;
}

Flag synth_can_fetch_op(uns proc_id, uns bp_id) {
  return !(uop_generator_get_eom(proc_id) && trace_read_done[proc_id]);
}

void synth_fetch_op(uns proc_id, uns bp_id, struct Op_struct* op) {
  bool off_path_mode_ = off_path_mode[proc_id][bp_id];
  // uns64 off_path_addr_ = off_path_addr[proc_id][bp_id];
  ctype_pin_inst* next_offpath_pi_ = &next_offpath_pi[proc_id][bp_id];

  if (uop_generator_get_bom(proc_id)) {
    if (!off_path_mode_) {
      uop_generator_get_uop(proc_id, op, &next_onpath_pi[proc_id]);
    } else {
      uop_generator_get_uop(proc_id, op, next_offpath_pi_);
    }
#ifdef PRINT_INSTRUCTION_INFO
    ctype_pin_inst next_pi = off_path_mode_ ? *next_offpath_pi_ : next_onpath_pi[proc_id];
    std::cout << disasm_op(op, TRUE) << ": ip " << next_pi.instruction_addr << " Next " << next_pi.instruction_next_addr
              << " size " << (uint32_t)next_pi.size << " target " << next_pi.branch_target << " size "
              << (uint32_t)next_pi.size << " taken " << (uint32_t)next_pi.actually_taken << " uid " << next_pi.inst_uid
              << " uid " << next_pi.inst_uid << " mem_addr " << next_pi.ld_vaddr[0] << std::endl;
#endif
  } else {
    uop_generator_get_uop(proc_id, op, NULL);
  }

  if (uop_generator_get_eom(proc_id)) {
    if (!off_path_mode_) {
      next_onpath_pi[proc_id] = synthetic_fe_generate_next(proc_id, off_path_mode_);
    } else {
      *next_offpath_pi_ = synthetic_fe_generate_next(proc_id, off_path_mode_);
    }
  }
}

void synth_redirect(uns proc_id, uns bp_id, uns64 inst_uid, Addr fetch_addr) {
  if (!bp_id)
    ASSERT(proc_id, fetch_addr);
  if (!fetch_addr)
    off_path_mode[proc_id][bp_id] = false;
  else
    off_path_mode[proc_id][bp_id] = true;
  off_path_addr[proc_id][bp_id] = fetch_addr;
  // synthetic kernel manages PCs internally using synth_fe_curr_pc variable
  // on redirect we modify synth_fe_curr_pc accordingly
  synth_fe_curr_pc = off_path_addr[proc_id][bp_id];
  next_offpath_pi[proc_id][bp_id] = synthetic_fe_generate_next(proc_id, off_path_mode[proc_id]);
#ifdef PRINT_INSTRUCTION_INFO
  std::cout << " Redirect happened here, pred addr is " << fetch_addr << std::endl;
#endif
  DEBUG(proc_id, "Redirect on-path:%lx off-path:%lx", next_onpath_pi[proc_id].instruction_addr,
        next_offpath_pi[proc_id][bp_id].instruction_addr);
}

void synth_recover(uns proc_id, uns bp_id, uns64 inst_uid) {
  Op dummy_op;
  if (bp_id) {
    off_path_addr[proc_id][bp_id] = 0;
    memset(&next_offpath_pi[proc_id][bp_id], 0, sizeof(next_offpath_pi[proc_id][bp_id]));
  } else
    ASSERT(proc_id, off_path_mode[proc_id][bp_id]);
  off_path_mode[proc_id][bp_id] = false;
  // Finish decoding of the current off-path inst before switching to on-path
  while (!uop_generator_get_eom(proc_id)) {
    uop_generator_get_uop(proc_id, &dummy_op, &next_offpath_pi[proc_id][bp_id]);
  }
  // restore synthetic frontend pc using the state that was stored before redirect
  synth_fe_curr_pc = next_onpath_pi->instruction_next_addr;
#ifdef PRINT_INSTRUCTION_INFO
  std::cout << " Recover happened here " << std::endl;
#endif
  DEBUG(proc_id, "Recover CF:%lx ", next_onpath_pi[proc_id].instruction_addr);
}

void synth_retire(uns proc_id, uns64 inst_uid) {
}
