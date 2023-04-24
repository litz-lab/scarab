#include "trace_fe.h"
#include "bp/bp.h"
#include "bp/bp.param.h"
#include "ctype_pin_inst.h"
#include "frontend/pt_memtrace/memtrace_fe.h"
#include "isa/isa.h"
#include "pin/pin_lib/uop_generator.h"
#include "pin/pin_lib/x86_decoder.h"
#include "pin/pin_lib/pin_api_to_xed.h"
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


InstInfo trace_fe_info;
xed_decoded_inst_t trace_fe_xed_inst_nop;
ctype_pin_inst trace_fe_inst_nop;

void generate_nop(uint64_t *pc, ctype_pin_inst *inst) {
  xed_state_t state;
  state.mmode = XED_MACHINE_MODE_LONG_64;
  uint8_t buf[10];
  xed_error_enum_t res = xed_encode_nop(buf, 1);
  if(res != XED_ERROR_NONE) {
    printf("Failed to encode due to %s\n", xed_error_enum_t2str(res));
  }
  xed_decoded_inst_zero_set_mode(&trace_fe_xed_inst_nop, &state);
  res = xed_decode(&trace_fe_xed_inst_nop, buf, sizeof(buf));
  if(res != XED_ERROR_NONE) {
    printf("XED NOP decode error! %s\n", xed_error_enum_t2str(res));
  }
  trace_fe_info.ins = &trace_fe_xed_inst_nop;
  trace_fe_info.pc = *pc;
  trace_fe_info.pid = 1;
  trace_fe_info.tid = 1;
  trace_fe_info.target = *pc + 1;
  trace_fe_info.static_target = 0;
  trace_fe_info.taken = 0;
  //trace_fe_info.unknown_type = unknown_type;
  trace_fe_info.valid = true;
  memset(inst, 0, sizeof(ctype_pin_inst));
  if (FRONTEND == FE_PT)
    pt_fill_in_dynamic_info(inst, &trace_fe_info);
  else if (FRONTEND == FE_MEMTRACE)
    memtrace_fill_in_dynamic_info(inst, &trace_fe_info);
  else
    assert(0);
  fill_in_basic_info(inst, trace_fe_info.ins);
  uint32_t max_op_width = add_dependency_info(inst, trace_fe_info.ins);
  fill_in_simd_info(inst, trace_fe_info.ins, max_op_width);
  apply_x87_bug_workaround(inst, trace_fe_info.ins);
  fill_in_cf_info(inst, trace_fe_info.ins);
  inst->actually_taken = false;
  print_err_if_invalid(inst, trace_fe_info.ins);
  (*pc)++;
}

void off_path_generate_inst(uns proc_id, uint64_t *off_path_addr, ctype_pin_inst *inst) {
  auto op_iter = pc_to_inst.find(*off_path_addr);

  if (op_iter != pc_to_inst.end()) {
    *inst = op_iter->second;
    *off_path_addr += inst->size;
    DEBUG(proc_id, "Generate off-path inst:%lx inst_size:%i ",inst->instruction_addr, inst->size);
  }
  else {
    *inst = trace_fe_inst_nop;
    inst->instruction_addr      = (*off_path_addr)++;
    inst->instruction_next_addr = *off_path_addr;
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
        else if (next_onpath_pi[proc_id].inst_binary_lsb != find->second.inst_binary_lsb ||
                 next_onpath_pi[proc_id].inst_binary_msb != find->second.inst_binary_msb) {
          DEBUG(proc_id, "Previously seen PC references new instruction addr:%lx inst_size:%i lsb:%lx msb:%lx\n ",
                addr, next_onpath_pi[proc_id].size, next_onpath_pi[proc_id].inst_binary_lsb,
                next_onpath_pi[proc_id].inst_binary_msb);
          // Handle jitted code
          pc_to_inst.erase(addr);
          pc_to_inst.insert(std::pair<uint64_t, ctype_pin_inst>(addr, next_onpath_pi[proc_id]));
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
  uint64_t pc = 0xCAFEBABE;
  generate_nop(&pc, &trace_fe_inst_nop);
}

void ext_trace_done() {

}
