#include "trace_fe.h"
#include "bp/bp.h"
#include "bp/bp.param.h"
#include "ctype_pin_inst.h"
#include "frontend/pt_memtrace/memtrace_fe.h"
#include "isa/isa.h"
#include "pin/pin_lib/uop_generator.h"
#include "pin/pin_lib/x86_decoder.h"
#include "statistics.h"
#include <iostream>

#include "frontend/pt_memtrace/pt_fe.h"
#include "frontend/frontend_intf.h"

/**************************************************************************************/
/* Macros */

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "globals/assert.h"
//#include "globals/global_defs.h"
//#include "globals/global_types.h"
//#include "globals/global_vars.h"
//#include "globals/utils.h"
//#include "globals/global_types.h"


#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_TRACE_READ, ##args)

/* Globals */
static ctype_pin_inst next_onpath_pi[MAX_NUM_PROCS];
static ctype_pin_inst next_offpath_pi[MAX_NUM_PROCS];
static bool            off_path_mode[MAX_NUM_PROCS] = {false};
static uint64_t        off_path_addr[MAX_NUM_PROCS] = {0};
static std::unordered_map<uint64_t, ctype_pin_inst> pc_to_inst;


void off_path_generate_inst(uns proc_id, uint64_t *off_path_addr, ctype_pin_inst *inst) {
  auto op_iter = pc_to_inst.find(*off_path_addr);

  if (op_iter != pc_to_inst.end()) {
    *inst = op_iter->second;
    *off_path_addr += inst->size;
    DEBUG(proc_id, "Generate off-path inst:%lx inst_size:%i ",inst->instruction_addr, inst->size);
  }
  else {
    *inst = create_dummy_nop(*off_path_addr, WPNM_REASON_REDIRECT_TO_NOT_INSTRUMENTED);
    (*off_path_addr)++;
  }
}

void ext_trace_fetch_op(uns proc_id, Op* op) {
  if(uop_generator_get_bom(proc_id)) {
    if (!off_path_mode[proc_id]) {
      uop_generator_get_uop(proc_id, op, &next_onpath_pi[proc_id]);
    }
    else {
      uop_generator_get_uop(proc_id, op, &next_offpath_pi[proc_id]);
    }
  } else {
    uop_generator_get_uop(proc_id, op, NULL);
  }

  if(uop_generator_get_eom(proc_id)) {
    if (!off_path_mode[proc_id]) {

      int success = false;
      if (FRONTEND == FE_PT)
        success = pt_trace_read(proc_id, &next_onpath_pi[proc_id]);
      else if (FRONTEND == FE_MEMTRACE)
        success = memtrace_trace_read(proc_id, &next_onpath_pi[proc_id]);
      if(!success) {
        trace_read_done[proc_id] = TRUE;
        reached_exit[proc_id]    = TRUE;
        op->exit = TRUE;
      }
      else {
        uint64_t addr = next_onpath_pi[proc_id].instruction_addr;
        auto find = pc_to_inst.find(addr);
        if(find == pc_to_inst.end()) {
          pc_to_inst.insert(std::pair<uint64_t, ctype_pin_inst>(addr, next_onpath_pi[proc_id]));
        }
        else {
          // Check if the instruction of a PC has changed. If yes, sufficient to just replace it?
          ASSERT(proc_id, next_onpath_pi[proc_id].inst_binary_lsb == find->second.inst_binary_lsb);
          ASSERT(proc_id, next_onpath_pi[proc_id].inst_binary_msb == find->second.inst_binary_msb);
        }
      }
    }
    else {
      off_path_generate_inst(proc_id, &off_path_addr[proc_id], &next_offpath_pi[proc_id]);
    }
  }
  DEBUG(proc_id, "Fetch op is_on_path:%i on_path:%lx off_path:%lx\n", off_path_mode[proc_id], next_onpath_pi[proc_id].instruction_addr, next_offpath_pi[proc_id].instruction_addr);
}

Flag ext_trace_can_fetch_op(uns proc_id) {
  return !(uop_generator_get_eom(proc_id) && trace_read_done[proc_id]);
}

void ext_trace_redirect(uns proc_id, uns64 inst_uid, Addr fetch_addr) {
  off_path_mode[proc_id] = true;
  off_path_addr[proc_id] = fetch_addr;
  off_path_generate_inst(proc_id, &off_path_addr[proc_id], &next_offpath_pi[proc_id]);
  DEBUG(proc_id, "Redirect on-path:%lx off-path:%lx", next_onpath_pi[proc_id].instruction_addr, next_offpath_pi[proc_id].instruction_addr);
}

void ext_trace_recover(uns proc_id, uns64 inst_uid) {
  Op dummy_op;
  off_path_mode[proc_id] = false;
  // Finish decoding of the current off-path inst before switching to on-path
  while (!uop_generator_get_eom(proc_id)) {
    uop_generator_get_uop(proc_id, &dummy_op, &next_offpath_pi[proc_id]);
  }
  DEBUG(proc_id, "Recover CF:%lx ", next_onpath_pi[proc_id].instruction_addr);
}

void ext_trace_retire(uns proc_id, uns64 inst_uid) {
  // Trace frontend does not need to communicate to PIN which instruction are
  // retired.
}

Addr ext_trace_next_fetch_addr(uns proc_id) {
  return next_onpath_pi[proc_id].instruction_addr;
}

void ext_trace_init() {
  memset(next_offpath_pi, 0, sizeof(next_offpath_pi));
  memset(next_onpath_pi, 0, sizeof(next_onpath_pi));

  if (FRONTEND == FE_PT) {
    pt_init();
    for(uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
      pt_trace_read(proc_id, &next_onpath_pi[proc_id]);
    }
  }
  else if (FRONTEND == FE_MEMTRACE) {
    memtrace_init();
    for(uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
      memtrace_trace_read(proc_id, &next_onpath_pi[proc_id]);
    }
  }
}

void ext_trace_done() {

}
