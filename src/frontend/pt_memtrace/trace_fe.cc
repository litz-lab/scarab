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

Flag ctype_pin_inst_same_mem_vaddr(ctype_pin_inst inst_a, ctype_pin_inst inst_b) {
  for (uns i = 0; i < MAX_LD_NUM; i++) {
    if (inst_a.ld_vaddr[i] != inst_b.ld_vaddr[i]) {
      return FALSE;
    }
  }

  for (uns i = 0; i < MAX_ST_NUM; i++) {
    if (inst_a.st_vaddr[i] != inst_b.st_vaddr[i]) {
      return FALSE;
    }
  }

  return TRUE;
}

void assert_ctype_pin_inst_same(uns proc_id, ctype_pin_inst inst_a, ctype_pin_inst inst_b) {
  // uint64_t inst_uid;  // unique ID produced by the frontend

  ASSERT(proc_id, inst_a.instruction_addr == inst_b.instruction_addr);
  ASSERT(proc_id, inst_a.size == inst_b.size);
  ASSERT(proc_id, inst_a.inst_binary_msb == inst_b.inst_binary_msb);
  ASSERT(proc_id, inst_a.inst_binary_lsb == inst_b.inst_binary_lsb);
  ASSERT(proc_id, inst_a.op_type == inst_b.op_type);
  ASSERT(proc_id, inst_a.cf_type == inst_b.cf_type);
  ASSERT(proc_id,  inst_a.is_fp == inst_b.is_fp);
  ASSERT(proc_id,  inst_a.true_op_type == inst_b.true_op_type);
  ASSERT(proc_id,  inst_a.num_src_regs == inst_b.num_src_regs);
  ASSERT(proc_id,  inst_a.num_dst_regs == inst_b.num_dst_regs);
  ASSERT(proc_id,  inst_a.num_ld1_addr_regs == inst_b.num_ld1_addr_regs);
  ASSERT(proc_id,  inst_a.num_ld2_addr_regs == inst_b.num_ld2_addr_regs);
  ASSERT(proc_id,  inst_a.num_st_addr_regs == inst_b.num_st_addr_regs);

  for (uns i = 0; i < MAX_SRC_REGS_NUM; i++) {
    ASSERT(proc_id, inst_a.src_regs[i] == inst_b.src_regs[i]);
  }

  for (uns i = 0; i < MAX_DST_REGS_NUM; i++) {
    ASSERT(proc_id, inst_a.dst_regs[i] == inst_b.dst_regs[i]);
  }

  for (uns i = 0; i < MAX_MEM_ADDR_REGS_NUM; i++) {
    ASSERT(proc_id, inst_a.ld1_addr_regs[i] == inst_b.ld1_addr_regs[i]);
  }

  for (uns i = 0; i < MAX_MEM_ADDR_REGS_NUM; i++) {
    ASSERT(proc_id, inst_a.ld2_addr_regs[i] == inst_b.ld2_addr_regs[i]);
  }

  for (uns i = 0; i < MAX_MEM_ADDR_REGS_NUM; i++) {
    ASSERT(proc_id, inst_a.st_addr_regs[i] == inst_b.st_addr_regs[i]);
  }

  ASSERT(proc_id, inst_a.num_simd_lanes == inst_b.num_simd_lanes);
  ASSERT(proc_id, inst_a.lane_width_bytes == inst_b.lane_width_bytes);
  ASSERT(proc_id, inst_a.num_ld == inst_b.num_ld);
  ASSERT(proc_id, inst_a.num_st == inst_b.num_st);
  ASSERT(proc_id, inst_a.has_immediate == inst_b.has_immediate);

  for (uns i = 0; i < MAX_LD_NUM; i++) {
    ASSERT(proc_id, inst_a.ld_vaddr[i] == inst_b.ld_vaddr[i]);
  }

  for (uns i = 0; i < MAX_ST_NUM; i++) {
    ASSERT(proc_id, inst_a.st_vaddr[i] == inst_b.st_vaddr[i]);
  }

  ASSERT(proc_id, inst_a.ld_size == inst_b.ld_size);
  ASSERT(proc_id, inst_a.st_size == inst_b.st_size);
  ASSERT(proc_id, inst_a.branch_target == inst_b.branch_target);
  ASSERT(proc_id, inst_a.actually_taken == inst_b.actually_taken);

  ASSERT(proc_id, inst_a.is_string == inst_b.is_string);
  ASSERT(proc_id, inst_a.is_call == inst_b.is_call);
  ASSERT(proc_id, inst_a.is_move == inst_b.is_move);
  ASSERT(proc_id, inst_a.is_prefetch == inst_b.is_prefetch);

  ASSERT(proc_id, inst_a.has_push == inst_b.has_push);
  ASSERT(proc_id, inst_a.has_pop == inst_b.has_pop);
  ASSERT(proc_id, inst_a.is_ifetch_barrier == inst_b.is_ifetch_barrier);
  ASSERT(proc_id, inst_a.is_lock == inst_b.is_lock);

  ASSERT(proc_id, inst_a.is_repeat == inst_b.is_repeat);
  ASSERT(proc_id, inst_a.is_simd == inst_b.is_simd);
  ASSERT(proc_id, inst_a.is_gather_scatter == inst_b.is_gather_scatter);
  ASSERT(proc_id, inst_a.is_sentinel == inst_b.is_sentinel);

  ASSERT(proc_id, inst_a.fake_inst == inst_b.fake_inst);
  ASSERT(proc_id, inst_a.exit == inst_b.exit);
  ASSERT(proc_id, inst_a.fake_inst_reason == inst_b.fake_inst_reason);
  ASSERT(proc_id, inst_a.instruction_next_addr == inst_b.instruction_next_addr);

  for (uns i = 0; i < 16; i++) {
    ASSERT(proc_id, inst_a.pin_iclass[i] == inst_b.pin_iclass[i]);
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
          STAT_EVENT(proc_id, INST_MAP_UPDATE_JITTED);
          pc_to_inst.erase(addr);
          pc_to_inst.insert(std::pair<uint64_t, ctype_pin_inst>(addr, next_onpath_pi[proc_id]));
        }
        else if (next_onpath_pi[proc_id].instruction_next_addr != find->second.instruction_next_addr) {
          ASSERT(proc_id, next_onpath_pi[proc_id].op_type == find->second.op_type);
          if (next_onpath_pi[proc_id].cf_type) {
            ASSERT(proc_id, next_onpath_pi[proc_id].cf_type == find->second.cf_type);
            ASSERT(proc_id, next_onpath_pi[proc_id].cf_type == CF_CBR ||
                            next_onpath_pi[proc_id].cf_type >= CF_IBR);
          }
          STAT_EVENT(proc_id, INST_MAP_UPDATE_NPC_INV + next_onpath_pi[proc_id].op_type);
          pc_to_inst.erase(addr);
          pc_to_inst.insert(std::pair<uint64_t, ctype_pin_inst>(addr, next_onpath_pi[proc_id]));
        }
        else if (!ctype_pin_inst_same_mem_vaddr(next_onpath_pi[proc_id], find->second)) {
          ASSERT(proc_id, next_onpath_pi[proc_id].op_type == find->second.op_type);
          STAT_EVENT(proc_id, INST_MAP_UPDATE_MEM_INV + next_onpath_pi[proc_id].op_type);
          pc_to_inst.erase(addr);
          pc_to_inst.insert(std::pair<uint64_t, ctype_pin_inst>(addr, next_onpath_pi[proc_id]));
        }
        else {
          assert_ctype_pin_inst_same(proc_id, next_onpath_pi[proc_id], find->second);
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
