#include "isa/isa.h"

#include "ctype_pin_inst.h"

/*                           Basic one line instruction APIs                                            */
ctype_pin_inst generate_generic_load(uns64 ip, uns64 uid, uns64 vaddr, uns8 inst_size, uns8 ld_addr_reg,
                                     uns8 dest_reg) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.inst_uid = uid;
  inst.instruction_addr = ip;
  inst.instruction_next_addr = ip + inst_size;
  inst.size = inst_size;
  inst.op_type = OP_ILD;
  strcpy(inst.pin_iclass, "DUMMY_LOAD_DC");
  inst.num_simd_lanes = 1;
  inst.lane_width_bytes = 1;
  inst.is_move = 1;
  inst.num_ld1_addr_regs = 1;
  inst.ld1_addr_regs[0] = ld_addr_reg;
  inst.ld_vaddr[0] = vaddr;
  inst.num_dst_regs = 1;
  inst.dst_regs[0] = dest_reg;
  inst.num_ld = 1;
  inst.ld_size = 8;
  return inst;
}

ctype_pin_inst generate_alu_type_inst(uns64 ip, uns64 uid, uns8 inst_size, uns8 regDest, uns8 regSrc1, uns8 regSrc2) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.inst_uid = uid;
  inst.instruction_addr = ip;
  inst.instruction_next_addr = ip + inst_size;
  inst.size = inst_size;
  inst.op_type = OP_IADD;
  strcpy(inst.pin_iclass, "DUMMY_IADD");
  inst.num_simd_lanes = 1;
  inst.lane_width_bytes = 1;
  inst.num_src_regs = 2;
  inst.num_dst_regs = 1;
  inst.src_regs[0] = regSrc1;
  inst.src_regs[1] = regSrc2;
  inst.dst_regs[0] = regDest;
  return inst;
}

ctype_pin_inst generate_conditional_branch(uns64 ip, uns64 uid, uns64 tgtAddr, bool direction, uns8 inst_size) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.inst_uid = uid;
  inst.instruction_addr = ip;
  inst.instruction_next_addr = direction ? tgtAddr : (ip + inst_size);
  inst.size = inst_size;
  inst.op_type = OP_CF;
  inst.cf_type = CF_CBR;
  inst.num_simd_lanes = 1;
  inst.lane_width_bytes = 1;
  inst.branch_target = tgtAddr;
  inst.actually_taken = direction ? TAKEN : NOT_TAKEN;
  strcpy(inst.pin_iclass, "DUMMY_CBR_JMP");
  return inst;
}

ctype_pin_inst generate_unconditional_branch(uns64 ip, uns64 uid, uns64 tgt, uns8 inst_size) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.instruction_addr = ip;
  inst.inst_uid = uid;
  inst.instruction_next_addr = tgt;
  inst.size = inst_size;
  inst.op_type = OP_CF;
  inst.cf_type = CF_BR;
  inst.num_simd_lanes = 1;
  inst.lane_width_bytes = 1;
  inst.branch_target = tgt;
  inst.actually_taken = TAKEN;
  strcpy(inst.pin_iclass, "DUMMY_UBR_JMP");
  return inst;
}

ctype_pin_inst generate_indirect_branch(uns64 ip, uns64 uid, uns64 tgtAddr, uns64 vaddr, uns8 inst_size) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.instruction_addr = ip;
  inst.inst_uid = uid;
  inst.instruction_next_addr = tgtAddr;
  inst.size = inst_size;
  inst.op_type = OP_CF;
  inst.cf_type = CF_IBR;
  inst.num_simd_lanes = 1;
  inst.lane_width_bytes = 1;
  inst.branch_target = tgtAddr;
  inst.actually_taken = 1;
  inst.num_ld1_addr_regs = 1;
  inst.ld1_addr_regs[0] = REG_RAX;
  inst.ld_vaddr[0] = vaddr;
  strcpy(inst.pin_iclass, "DUMMY_IBR_JUMP");
  return inst;
}

ctype_pin_inst generate_nop(uns64 ip, uns64 uid, uns64 inst_size, bool fake) {
  ctype_pin_inst inst = create_dummy_nop(ip, WPNM_NOT_IN_WPNM);
  inst.size = inst_size;
  inst.instruction_next_addr = ip + inst_size;
  inst.inst_uid = uid;
  inst.fake_inst = fake ? 0 : 1;
  return inst;
}