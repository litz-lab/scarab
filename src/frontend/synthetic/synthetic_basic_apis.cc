#include "isa/isa.h"

#include "ctype_pin_inst.h"

/*                           Basic one line instruction APIs                                            */
ctype_pin_inst generate_generic_load(uns64 ip, uns64 uid, uns64 vaddr, uns8 inst_size, uns8 ld_addr_reg,
                                     uns8 dest_reg) {
  ctype_pin_inst inst;
  init_ctype_pin_inst(&inst);
  inst.inst_uid = uid;
  inst.st.instruction_addr = ip;
  inst.instruction_next_addr = ip + inst_size;
  inst.st.size = inst_size;
  inst.st.op_type = OP_ILD;
  strcpy(inst.st.pin_iclass, "SYNTH_LOAD");
  inst.st.num_simd_lanes = 1;
  inst.st.lane_width_bytes = 1;
  inst.st.is_move = 1;
  inst.st.num_ld1_addr_regs = 1;
  inst.st.ld1_addr_regs[0] = ld_addr_reg;
  inst.ld_vaddr[0] = vaddr;
  inst.st.num_dst_regs = 1;
  inst.st.dst_regs[0] = dest_reg;
  inst.st.num_ld = 1;
  inst.st.ld_size = 8;
  return inst;
}

ctype_pin_inst generate_alu_type_inst(uns64 ip, uns64 uid, uns8 inst_size, uns8 regDest, uns8 regSrc1, uns8 regSrc2) {
  ctype_pin_inst inst;
  init_ctype_pin_inst(&inst);
  inst.inst_uid = uid;
  inst.st.instruction_addr = ip;
  inst.instruction_next_addr = ip + inst_size;
  inst.st.size = inst_size;
  inst.st.op_type = OP_IADD;
  strcpy(inst.st.pin_iclass, "SYNTH_IADD");
  inst.st.num_simd_lanes = 1;
  inst.st.lane_width_bytes = 1;
  inst.st.num_src_regs = 2;
  inst.st.num_dst_regs = 1;
  inst.st.src_regs[0] = regSrc1;
  inst.st.src_regs[1] = regSrc2;
  inst.st.dst_regs[0] = regDest;
  return inst;
}

ctype_pin_inst generate_conditional_branch(uns64 ip, uns64 uid, uns64 tgtAddr, bool direction, uns8 inst_size) {
  ctype_pin_inst inst;
  init_ctype_pin_inst(&inst);
  inst.inst_uid = uid;
  inst.st.instruction_addr = ip;
  inst.instruction_next_addr = direction ? tgtAddr : (ip + inst_size);
  inst.st.size = inst_size;
  inst.st.op_type = OP_CF;
  inst.st.cf_type = CF_CBR;
  inst.st.num_simd_lanes = 1;
  inst.st.lane_width_bytes = 1;
  inst.st.branch_target = tgtAddr;
  inst.actually_taken = direction ? TAKEN : NOT_TAKEN;
  strcpy(inst.st.pin_iclass, "SYNTH_CBR");
  return inst;
}

ctype_pin_inst generate_unconditional_branch(uns64 ip, uns64 uid, uns64 tgt, uns8 inst_size) {
  ctype_pin_inst inst;
  init_ctype_pin_inst(&inst);
  inst.st.instruction_addr = ip;
  inst.inst_uid = uid;
  inst.instruction_next_addr = tgt;
  inst.st.size = inst_size;
  inst.st.op_type = OP_CF;
  inst.st.cf_type = CF_BR;
  inst.st.num_simd_lanes = 1;
  inst.st.lane_width_bytes = 1;
  inst.st.branch_target = tgt;
  inst.actually_taken = TAKEN;
  strcpy(inst.st.pin_iclass, "SYNTH_UBR");
  return inst;
}

ctype_pin_inst generate_indirect_branch(uns64 ip, uns64 uid, uns64 tgtAddr, uns64 vaddr, uns8 inst_size) {
  ctype_pin_inst inst;
  init_ctype_pin_inst(&inst);
  inst.st.instruction_addr = ip;
  inst.inst_uid = uid;
  inst.instruction_next_addr = tgtAddr;
  inst.st.size = inst_size;
  inst.st.op_type = OP_CF;
  inst.st.cf_type = CF_IBR;
  inst.st.num_simd_lanes = 1;
  inst.st.lane_width_bytes = 1;
  inst.st.branch_target = tgtAddr;
  inst.actually_taken = 1;
  inst.st.num_ld1_addr_regs = 1;
  inst.st.ld1_addr_regs[0] = REG_RAX;
  inst.ld_vaddr[0] = vaddr;
  strcpy(inst.st.pin_iclass, "SYNTH_IBR");
  return inst;
}

ctype_pin_inst generate_nop(uns64 ip, uns64 uid, uns64 inst_size, bool fake) {
  ctype_pin_inst inst = create_dummy_nop(ip, WPNM_NOT_IN_WPNM, inst_size);
  inst.st.size = inst_size;
  inst.instruction_next_addr = ip + inst_size;
  inst.inst_uid = uid;
  inst.st.fake_inst = fake ? 0 : 1;
  return inst;
}