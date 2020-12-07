/* Copyright 2020 HPS/SAFARI Research Groups
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

#include <iostream>
#include <set>
#include <string>
#include <unordered_map>

#include "decoder.h"
#include "x87_stack_delta.h"

#include "pin_scarab_common_lib.h"

using namespace std;

KNOB<BOOL> Knob_debug(KNOB_MODE_WRITEONCE, "pintool", "debug", "0",
                      "always add instructions to print map");
KNOB<BOOL> Knob_translate_x87_regs(
  KNOB_MODE_WRITEONCE, "pintool", "translate_x87_regs", "1",
  "translate Pin's relative x87 regs to Scarab's absolute regs");

// the most recently filled instruction
static ctype_pin_inst* filled_inst_info;
static ctype_pin_inst  tmp_inst_info;


/************************** Data Type Definitions *****************************/
/*#define REG(x) SCARAB_REG_##x,
typedef enum Reg_Id_struct {
#include "../../isa/x86_regs.def"
  SCARAB_NUM_REGS
} Reg_Id;
#undef REG
*/

// Globals used for communication between analysis functions
uint32_t       glb_opcode, glb_actually_taken;
deque<ADDRINT> glb_ld_vaddrs, glb_st_vaddrs;

std::ostream*         glb_err_ostream;
bool                  glb_translate_x87_regs;
std::set<std::string> unknown_opcodes;
inst_info_map                                   inst_info_storage;

/********************* Private Functions Prototypes ***************************/
ctype_pin_inst* get_inst_info_obj(const INS& ins);
void     insert_analysis_functions(ctype_pin_inst* info, const INS& ins);
void     print_err_if_invalid(ctype_pin_inst* info, const INS& ins);

void get_opcode(UINT32 opcode);

void get_gather_scatter_eas(PIN_MULTI_MEM_ACCESS_INFO* mem_access_info);
void get_ld_ea(ADDRINT addr);
void get_ld_ea2(ADDRINT addr1, ADDRINT addr2);
void get_st_ea(ADDRINT addr);
void get_branch_dir(bool taken);
void create_compressed_op(ADDRINT iaddr);

/**************************** Public Functions ********************************/
void pin_decoder_init(bool translate_x87_regs, std::ostream* err_ostream) {
  init_reg_compress_map();
  init_pin_opcode_convert();
  init_x87_stack_delta();
  glb_translate_x87_regs = translate_x87_regs;
  if(err_ostream) {
    glb_err_ostream = err_ostream;
  } else {
    glb_err_ostream = &std::cout;
  }
}

void pin_decoder_insert_analysis_functions(const INS& ins) {
  ctype_pin_inst* info = get_inst_info_obj(ins);
  fill_in_basic_info(info, ins);
  uint32_t max_op_width = add_dependency_info(info, ins);
  fill_in_simd_info(info, ins, max_op_width);
  apply_x87_bug_workaround(info, ins);
  fill_in_cf_info(info, ins);
  insert_analysis_functions(info, ins);
  print_err_if_invalid(info, ins);
}

ctype_pin_inst* pin_decoder_get_latest_inst() {
  return filled_inst_info;
}

void pin_decoder_print_unknown_opcodes() {
  for(const auto opcode : unknown_opcodes) {
    (*glb_err_ostream) << opcode << std::endl;
  }
}

ctype_pin_inst create_sentinel() {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.op_type     = OP_INV;
  inst.is_sentinel = 1;
  strcpy(inst.pin_iclass, "SENTINEL");
  return inst;
}

ctype_pin_inst create_dummy_jump(uint64_t eip, uint64_t tgt) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.instruction_addr = eip;
  inst.size             = 1;
  inst.op_type          = OP_IADD;
  inst.cf_type          = CF_BR;
  inst.num_simd_lanes   = 1;
  inst.lane_width_bytes = 1;
  inst.branch_target    = tgt;
  inst.actually_taken   = 1;
  inst.fake_inst        = 1;
  strcpy(inst.pin_iclass, "DUMMY_JMP");
  return inst;
}

ctype_pin_inst create_dummy_nop(uint64_t                  eip,
                                Wrongpath_Nop_Mode_Reason reason) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.instruction_addr      = eip;
  inst.instruction_next_addr = eip + 1;
  inst.size                  = 1;
  inst.op_type               = OP_NOP;
  strcpy(inst.pin_iclass, "DUMMY_NOP");
  inst.fake_inst        = 1;
  inst.fake_inst_reason = reason;
  return inst;
}

/*************************** Private Functions  *******************************/
ctype_pin_inst* get_inst_info_obj(const INS& ins) {
  ctype_pin_inst* info = (ctype_pin_inst*)calloc(1, sizeof(ctype_pin_inst));
  inst_info_map_p lp   = inst_info_storage.find(INS_Address(ins));
  if(lp == inst_info_storage.end()) {
    inst_info_storage[INS_Address(ins)] = info;
  } else {
    // TODO_b: should we free the existing ctype_pin_inst at
    // inst_info_storage[INS_Address(ins)]?
  }
  return info;
}


void insert_analysis_functions(ctype_pin_inst* info, const INS& ins) {
  if(Knob_translate_x87_regs.Value()) {
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)get_opcode, IARG_UINT32,
                   INS_Opcode(ins), IARG_END);
  }

  if(INS_IsVgather(ins) || INS_IsVscatter(ins)) {
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)get_gather_scatter_eas,
                   IARG_MULTI_MEMORYACCESS_EA, IARG_END);
  } else {
    if(INS_IsMemoryRead(ins)) {
      if(INS_HasMemoryRead2(ins)) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)get_ld_ea2,
                       IARG_MEMORYREAD_EA, IARG_MEMORYREAD2_EA, IARG_END);
      } else {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)get_ld_ea,
                       IARG_MEMORYREAD_EA, IARG_END);
      }
    }

    if(INS_IsMemoryWrite(ins)) {
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)get_st_ea,
                     IARG_MEMORYWRITE_EA, IARG_END);
    }
  }

  if(info->cf_type) {
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)get_branch_dir,
                   IARG_BRANCH_TAKEN, IARG_END);
  }

  INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)create_compressed_op,
                 IARG_INST_PTR, IARG_END);
}

// int64_t heartbeat = 0;
void create_compressed_op(ADDRINT iaddr) {
  if(!fast_forward_count) {
    assert(inst_info_storage.count(iaddr) == 1);
    filled_inst_info = inst_info_storage[iaddr];
    if(glb_translate_x87_regs) {
      // copy ctype_pin_inst to avoid clobbering it
      memcpy(&tmp_inst_info, filled_inst_info, sizeof(ctype_pin_inst));
      filled_inst_info = &tmp_inst_info;
      // translate registers (no need to translate agen, since they won't be
      // FP)
      for(int i = 0; i < filled_inst_info->num_src_regs; ++i) {
        filled_inst_info->src_regs[i] = absolute_reg(
          filled_inst_info->src_regs[i], glb_opcode, false);
      }
      for(int i = 0; i < filled_inst_info->num_dst_regs; ++i) {
        filled_inst_info->dst_regs[i] = absolute_reg(
          filled_inst_info->dst_regs[i], glb_opcode, true);
      }
      // update x87 state
      update_x87_stack_state(glb_opcode);
    }

    uint num_lds = glb_ld_vaddrs.size();
    assert(num_lds <= MAX_LD_NUM);
    for(uint ld = 0; ld < num_lds; ld++) {
      filled_inst_info->ld_vaddr[ld] = glb_ld_vaddrs[ld];
    }

    uint num_sts = glb_st_vaddrs.size();
    assert(num_sts <= MAX_ST_NUM);
    for(uint st = 0; st < num_sts; st++) {
      filled_inst_info->st_vaddr[st] = glb_st_vaddrs[st];
    }

    filled_inst_info->actually_taken = glb_actually_taken;
  }
  glb_opcode = 0;
  glb_ld_vaddrs.clear();
  glb_st_vaddrs.clear();
  glb_actually_taken = 0;

  // if (heartbeat % 100000000 == 0) {
  //  (*glb_err_ostream) << "Heartbeat: " << heartbeat << std::endl <<
  //  std::flush;
  //}
  // heartbeat += 1;
}

void get_gather_scatter_eas(PIN_MULTI_MEM_ACCESS_INFO* mem_access_info) {
  UINT32 numMemOps = mem_access_info->numberOfMemops;
  for(UINT32 i = 0; i < numMemOps; i++) {
    ADDRINT        addr = mem_access_info->memop[i].memoryAddress;
    PIN_MEMOP_ENUM type = mem_access_info->memop[i].memopType;

    // only let Scarab know about it if the memop is not masked away
    if(mem_access_info->memop[i].maskOn) {
      if(PIN_MEMOP_LOAD == type) {
        // TODO: get rid of the print
        (*glb_err_ostream) << "load memop to address 0x" << std::hex << addr
                           << std::endl;
        glb_ld_vaddrs.push_back(addr);
      } else if(PIN_MEMOP_STORE == type) {
        // TODO: get rid of the print
        (*glb_err_ostream) << "store memop to address 0x" << std::hex << addr
                           << std::endl;
        glb_st_vaddrs.push_back(addr);
      } else {
        assert(false);  // unknown PIN_MEMOP_ENUM type
      }
    }
  }
}

void get_opcode(UINT32 opcode) {
  glb_opcode = opcode;
}

void get_ld_ea(ADDRINT addr) {
  glb_ld_vaddrs.push_back(addr);
}

void get_ld_ea2(ADDRINT addr1, ADDRINT addr2) {
  glb_ld_vaddrs.push_back(addr1);
  glb_ld_vaddrs.push_back(addr2);
}

void get_st_ea(ADDRINT addr) {
  glb_st_vaddrs.push_back(addr);
}

void get_branch_dir(bool taken) {
  glb_actually_taken = taken;
}


