/* Copyright 2020 University of Michigan (implemented by Nathan Brown)
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
 * File         : frontend/pt_fe.cc
 * Author       : Nathan Brown
 * Date         : 02/24/2021
 * Description  : Interface to simulate ChampSim Simulator traces
 ***************************************************************************************/
#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "bp/bp.h"
#include "statistics.h"
#include "bp/bp.param.h"
#include "ctype_pin_inst.h"
#include "frontend/champsim_fe.h"
#include "frontend/champsim_inst.hpp"
#include "isa/isa.h"
#include "./pin/pin_lib/uop_generator.h"
#include "./pin/pin_lib/x86_decoder.h"

#define DR_DO_NOT_DEFINE_int64
#include "frontend/champsim_trace_reader.hpp"

#include <assert.h>
#include <string>
#include <iostream>

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_TRACE_READ, ##args)


/**************************************************************************************/
/* Global Variables for Champsim */

char* champsim_trace_files[MAX_NUM_PROCS];
ChampsimTraceReader_t *champsim_trace_readers[MAX_NUM_PROCS];
ctype_pin_inst* champsim_next_pi;
uint64_t champsim_ins_id = 0;
uint64_t champsim_prior_tid = 0;
uint64_t champsim_prior_pid = 0;

/**************************************************************************************/
/* Private Functions for Champsim */

void champsim_fill_in_dynamic_info(ctype_pin_inst* info, const champsim_instruction_info* insn) {
    info->actually_taken = insn->branch_taken;
    info->branch_target = insn->next_ip; // need to find a way to fake the static info
    // only an issue for conditional branches that aren't taken
    // can probably set to 0 for indirect branches / calls
    info->inst_uid = champsim_ins_id++;
    info->actually_taken = insn->actually_taken;

    // fill in load / store info
    info->ld_size = 0;
    info->st_size = 0;
    // TODO: handle ld/st size. Infer from which registers are used?
    auto& num_ld = info->num_ld;
    auto& num_st = info->num_st;
    for(num_ld = 0; num_ld < NUM_INSTR_SOURCES && insn->source_memory[num_ld]; ++num_ld) {
        info->ld_vaddr[num_ld] = insn->source_memory[num_ld];
    }
    for(num_st = 0; num_st < NUM_INSTR_DESTINATIONS && insn->destination_memory[num_st]; ++num_st) {
        info->st_vaddr[num_st] = insn->destination_memory[num_st];
    }
    /* std::cout << *insn << std::endl; */
    assert(num_st <= MAX_ST_NUM && "Cannot represent num stores in a ctype_pin_inst!");
    assert(num_ld <= MAX_LD_NUM && "Too many loads for a ctype pin inst");
}

int champsim_ffwd() {
  if (!FAST_FORWARD) {
    return 0;
  }
  return 1;
}

void fill_in_cf_info(ctype_pin_inst* info, const champsim_instruction_info& insn) {
    info->cf_type = NOT_CF;
    switch(insn.is_branch) {
        case NOT_BRANCH:
            break;
        case BRANCH_DIRECT_JUMP:
            info->cf_type = CF_BR;
            break;
        case BRANCH_INDIRECT:
            info->cf_type = CF_IBR;
            break;
        case BRANCH_CONDITIONAL:
            info->cf_type = CF_CBR;
            break;
        case BRANCH_DIRECT_CALL:
            info->cf_type = CF_CALL;
            break;
        case BRANCH_INDIRECT_CALL:
            info->cf_type = CF_ICALL;
            break;
        case BRANCH_RETURN:
            info->cf_type = CF_RET;
            break;
        case BRANCH_OTHER:
            info->cf_type = CF_BR; // TODO: is this right?
            break;
    }
}

void champsim_fill_in_basic_info(ctype_pin_inst* pin_inst, champsim_instruction_info& insn) {
    pin_inst->instruction_addr = insn.ip;
    uint8_t size = insn.next_ip - insn.ip;
    /* if(insn.ip == 0xffffb7bc80f4) { */
    /*     std::cout << "@@@insn of interest has size: " << +size << " and the next insn ip is: " << insn.next_ip << " and the difference is: " << insn.next_ip - insn.ip << std::endl; */
    /* } */
    if(size != insn.next_ip - insn.ip && !insn.is_branch) {
        if(insn.ip == 0xffffb7bc80f4)
            std::cout << "@@@ marking this as a jmp instead!" << std::endl;
        insn.is_branch = BRANCH_DIRECT_JUMP;
        insn.branch_taken = insn.actually_taken = true;
        // direct branches have no sources
        memset(insn.source_registers, 0, NUM_INSTR_SOURCES * sizeof(insn.source_registers[0]));
        memset(insn.source_memory, 0, NUM_INSTR_SOURCES * sizeof(insn.source_memory[0]));
        std::cout << +insn.source_memory[0] << ' ' << +insn.source_registers[0] << std::endl;
        fill_in_cf_info(pin_inst, insn);
    }
    if(insn.branch_taken) {
        switch(insn.is_branch) {
            // TODO: actually make better guesses at the sizes
            case BRANCH_DIRECT_JUMP:
                if(!insn.source_memory[0] && !insn.source_registers[0]) {
                    // size depends on immediate value
                    size = LOG2(size) + 1; // 1 comes from opcode
                } else {
                    assert(0 && "direct jump has indirect soures!"); // not direct!
                }
                /* if(insn.ip == 0xffffb7bc80f4) */
                /*     std::cout << "@@@ size is now: " << std::dec << size << std::hex  << ' ' << size << std::endl; */
                break;
            case BRANCH_INDIRECT:
                assert([&](){
                        int num_sources = 0;
                        for(int i = 0; i < NUM_INSTR_SOURCES; ++i)
                        if(insn.source_registers[i] || insn.source_memory[i])
                            return true;
                        return false;
                        }() || "indirect does not have any sources!");
                size = 5; // TODO: figure out what this should be. I think reg will be smaller than mem
                break;
            case BRANCH_CONDITIONAL:
                // TODO
                size = 6;
                break;
            case BRANCH_DIRECT_CALL:
                size = 7;
                break;
            case BRANCH_INDIRECT_CALL:
                size = 8;
                break;
            case BRANCH_RETURN:
                size = 1;
                break;
        }
    }
    pin_inst->size = size;
    pin_inst->op_type = insn.is_branch ? OP_CF : OP_NOP;
    pin_inst->instruction_next_addr = insn.next_ip;
    pin_inst->pin_iclass[0] = 0; // empty string
    pin_inst->is_fp = false;
    pin_inst->is_string = false;
    pin_inst->is_call = false;
    pin_inst->is_move = false;
    pin_inst->is_prefetch =  false;
    pin_inst->has_push = false;
    pin_inst->has_pop = false;
    pin_inst->is_lock = false;
    pin_inst->is_repeat = false;
}

int champsim_trace_read(int proc_id, ctype_pin_inst* champsim_next_pi) {
  champsim_instruction_info *insi;

  insi = const_cast<champsim_instruction_info *>(champsim_trace_readers[proc_id]->nextInstruction());
  if (!insi)
   return 0; //end of trace

  /* if(insi->ip == 0xffffb7bc80f4) { */
  /*     assert(insi->is_branch); */
  /* } */
  memset(champsim_next_pi, 0, sizeof(ctype_pin_inst));
  fill_in_cf_info(champsim_next_pi, *insi); // helpful for everything else
  champsim_fill_in_basic_info(champsim_next_pi, *insi);
  assert(champsim_next_pi->instruction_next_addr && "instruction_next_addr not set");
  champsim_fill_in_dynamic_info(champsim_next_pi, insi);
  champsim_next_pi->num_ld = champsim_next_pi->num_st = champsim_next_pi->ld_size = champsim_next_pi->st_size = 0; // for now, NOP / CF won't need these
  add_dependency_info(champsim_next_pi, *insi);
  champsim_next_pi->num_simd_lanes = 1;
  if(insi->is_branch) {
      champsim_next_pi->lane_width_bytes = 8; // max, for indirect jump to 64 bit address
  } else {
      champsim_next_pi->lane_width_bytes = 1; // might do 0 for nop
  }
  /* fill_in_simd_info(champsim_next_pi, *insi, max_op_width); */
  /* apply_x87_bug_workaround(champsim_next_pi, *insi); */
  print_err_if_invalid(champsim_next_pi, *insi);

  std::cout << "fetched IP: " << std::hex << champsim_next_pi->instruction_addr << " next ip: " << champsim_next_pi->instruction_next_addr << std::endl;
  //End of ROI
  /* if (pt_roi(*insi)) */
  /*   return 0; */

  return 1;
}

void champsim_init(void) {
  /*ASSERTM(0, !FETCH_OFF_PATH_OPS,
          "Trace frontend does not support wrong path. Turn off "
          "FETCH_OFF_PATH_OPS\n");
  */

  uop_generator_init(NUM_CORES);
  init_pin_opcode_convert();
  init_reg_compress_map();
  init_x87_stack_delta();

  champsim_next_pi = (ctype_pin_inst*)malloc(NUM_CORES * sizeof(ctype_pin_inst));

  /* temp variable needed for easy initialization syntax */
  char* tmp_trace_files[MAX_NUM_PROCS] = {
    CBP_TRACE_R0,  CBP_TRACE_R1,  CBP_TRACE_R2,  CBP_TRACE_R3,  CBP_TRACE_R4,
    CBP_TRACE_R5,  CBP_TRACE_R6,  CBP_TRACE_R7,  CBP_TRACE_R8,  CBP_TRACE_R9,
    CBP_TRACE_R10, CBP_TRACE_R11, CBP_TRACE_R12, CBP_TRACE_R13, CBP_TRACE_R14,
    CBP_TRACE_R15, CBP_TRACE_R16, CBP_TRACE_R17, CBP_TRACE_R18, CBP_TRACE_R19,
    CBP_TRACE_R20, CBP_TRACE_R21, CBP_TRACE_R22, CBP_TRACE_R23, CBP_TRACE_R24,
    CBP_TRACE_R25, CBP_TRACE_R26, CBP_TRACE_R27, CBP_TRACE_R28, CBP_TRACE_R29,
    CBP_TRACE_R30, CBP_TRACE_R31, CBP_TRACE_R32, CBP_TRACE_R33, CBP_TRACE_R34,
    CBP_TRACE_R35, CBP_TRACE_R36, CBP_TRACE_R37, CBP_TRACE_R38, CBP_TRACE_R39,
    CBP_TRACE_R40, CBP_TRACE_R41, CBP_TRACE_R42, CBP_TRACE_R43, CBP_TRACE_R44,
    CBP_TRACE_R45, CBP_TRACE_R46, CBP_TRACE_R47, CBP_TRACE_R48, CBP_TRACE_R49,
    CBP_TRACE_R50, CBP_TRACE_R51, CBP_TRACE_R52, CBP_TRACE_R53, CBP_TRACE_R54,
    CBP_TRACE_R55, CBP_TRACE_R56, CBP_TRACE_R57, CBP_TRACE_R58, CBP_TRACE_R59,
    CBP_TRACE_R60, CBP_TRACE_R61, CBP_TRACE_R62, CBP_TRACE_R63,
  };
  if(DUMB_CORE_ON) {
    // avoid errors by specifying a trace known to be good
    tmp_trace_files[DUMB_CORE] = tmp_trace_files[0];
  }

  for(uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    champsim_trace_files[proc_id] = tmp_trace_files[proc_id];
  }
  for(uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    champsim_setup(proc_id);
  }
}

Addr champsim_next_fetch_addr(uns proc_id) {
  return champsim_next_pi[proc_id].instruction_addr;
}

Flag pt_can_fetch_op(uns proc_id) {
  assert(proc_id == 0);
  return !(uop_generator_get_eom(proc_id) && trace_read_done[proc_id]);
}

void champsim_fetch_op(uns proc_id, Op *op) {
    assert(champsim_next_pi[proc_id].instruction_addr != 0x3ff000008083c80);
  if(uop_generator_get_bom(proc_id)) {
      /* std::cout << "bom was true" << std::endl; */
    uop_generator_get_uop(proc_id, op, &champsim_next_pi[proc_id]);
  } else {
      /* std::cout << "bom was false" << std::endl; */
    uop_generator_get_uop(proc_id, op, NULL);
  }

  if(uop_generator_get_eom(proc_id)) {
    int success = champsim_trace_read(proc_id, &champsim_next_pi[proc_id]);
    assert(champsim_next_pi[proc_id].instruction_addr != 0x3ff000008083c80);
    static int ins = 0;
    ins++;
    if(!success) {
      trace_read_done[proc_id] = TRUE;
      reached_exit[proc_id]    = TRUE;
    }
  }
}

void pt_redirect(uns proc_id, uns64 inst_uid, Addr fetch_addr) {
  assert(0);
}

void pt_recover(uns proc_id, uns64 inst_uid) {
  assert(0);
}

void pt_retire(uns proc_id, uns64 inst_uid) {
  // Similar to memtrace and PT, Champsim frontend does not need to communicate to PIN to
  // determine which instruction are retired.
}

void pt_close_trace_file(uns proc_id) {
  printf("Closing PT file for %u\n", proc_id);
}

void pt_done() {
  printf("Frontend simulation finished for all PTs\n");
}

void champsim_setup(uns proc_id) {
  std::string trace(champsim_trace_files[proc_id]);

  champsim_trace_readers[proc_id] = new ChampsimTraceReader_t(std::move(trace));

  //FFWD
  champsim_ins_id++;

  if(FAST_FORWARD) {
    std::cout << "fast forward not supported! " << champsim_ins_id << std::endl;
    return;
  }

  // TODO: revisit ffwd logic
  /* while (!insi->valid || champsim_ffwd(*insi)) { */
  /*   insi = pt_trace_readers[proc_id]->nextInstruction(); */
  /*   pt_ins_id++; */
  /*   if ((pt_ins_id % 10000000) == 0) */
  /*     std::cout << "Fast forwarded " << pt_ins_id << " instructions." << std::endl; */
  /* } */

  /* if(FAST_FORWARD) { */
  /*   std::cout << "Exit fast forward " << pt_ins_id << std::endl; */
  /* } */

  champsim_prior_pid = 1;
  champsim_prior_tid = 1;
  assert(champsim_prior_tid);
  assert(champsim_prior_pid);
  champsim_trace_read(proc_id, &champsim_next_pi[proc_id]);
}

Flag champsim_can_fetch_op(uns proc_id) {
    assert(!proc_id);
  return !(uop_generator_get_eom(proc_id) && trace_read_done[proc_id]);
}

void champsim_redirect(uns proc_id, uns64 inst_uid, Addr fetch_addr) {
  assert(0);
}

void champsim_recover(uns proc_id, uns64 inst_uid) {
  assert(0);
}

void champsim_retire(uns proc_id, uns64 inst_uid) {
  // Similar to memtrace annd PT, champsim frontend does not need to communicate to PIN to
  // determine which instruction are retired.
}

void champsim_done() {
    printf("Frontend simulation finished for all Champsims\n");
}
