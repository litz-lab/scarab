#ifndef __SYNTHETIC_KERNELS_H__
#define __SYNTHETIC_KERNELS_H__
#include <stdbool.h>

#include "globals/global_types.h"

#include "isa/isa.h"

#include "ctype_pin_inst.h"

/* Bottleneck Enum */
typedef enum BottleNeck_Id_enum {
#define BOTTLENECK_IMPL(id, name) id,
#include "frontend/synthetic/bottlenecks_table.def"
#undef BOTTLENECK_IMPL
  INVALID
} BottleNeck_enum;

/************************** Microkernels Init Utilities *************************************/
void synthetic_kernel_init();
extern const char* bottleneckNames[];
extern BottleNeck_enum bottleneck;

/* top_level dispatccher */
ctype_pin_inst generate_synthetic_microkernel(uns, BottleNeck_enum, uns64, uns64, bool);

/************************** Microkernels*****************************************************/
ctype_pin_inst create_ILP_limited_microkernel(uns64, uns64, bool);
ctype_pin_inst generate_icache_limited_microkernel(uns64, uns64, bool);
ctype_pin_inst generate_mem_latency_limited_microkernel(uns64, uns64, bool);
ctype_pin_inst generate_mem_bandwidth_limited_microkernel(uns64, uns64, bool);
ctype_pin_inst generate_cbr_limited_microkernel(uns64, uns64, bool);
ctype_pin_inst generate_btb_limited_microkernel(uns64, uns64, bool);
ctype_pin_inst generate_btb_assoc_limited_microkernel(uns64, uns64, bool);
ctype_pin_inst generate_ibr_limited_microkernel(uns64, uns64, uns64, bool);

/************************** Basic one line instruction Definitions **************************/
inline ctype_pin_inst generate_loop_carried_dependence_load(uns64 ip, uns64 uid, uns64 vaddr, uns8 inst_size) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.inst_uid = uid;
  inst.instruction_addr = ip;
  inst.instruction_next_addr = ip + inst_size;
  inst.size = inst_size;
  inst.op_type = OP_MOV;
  strcpy(inst.pin_iclass, "DUMMY_LOAD_DC");
  inst.num_simd_lanes = 1;
  inst.lane_width_bytes = 1;
  inst.is_move = 1;
  inst.num_ld1_addr_regs = 1;
  inst.ld1_addr_regs[0] = REG_RAX;
  inst.ld_vaddr[0] = vaddr;
  inst.num_dst_regs = 1;
  inst.dst_regs[0] = REG_RAX;
  inst.num_ld = 1;
  inst.ld_size = 8;
  return inst;
}

inline ctype_pin_inst generate_independent_operand_load(uns64 ip, uns64 uid, uns64 vaddr, uns8 inst_size) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.inst_uid = uid;
  inst.instruction_addr = ip;
  inst.instruction_next_addr = ip + inst_size;
  inst.size = inst_size;
  inst.op_type = OP_MOV;
  strcpy(inst.pin_iclass, "DUMMY_LOAD");
  inst.num_simd_lanes = 1;
  inst.lane_width_bytes = 1;
  inst.is_move = 1;
  inst.num_ld1_addr_regs = 1;
  inst.ld1_addr_regs[0] = REG_RBX;
  inst.ld_vaddr[0] = vaddr;
  inst.num_dst_regs = 1;
  inst.dst_regs[0] = REG_RAX;
  inst.num_ld = 1;
  inst.ld_size = 8;
  return inst;
}

inline ctype_pin_inst generate_alu_type_inst(uns64 ip, uns64 uid, uns8 inst_size) {
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
  inst.src_regs[0] = REG_RAX;
  inst.src_regs[1] = REG_RBX;
  inst.dst_regs[0] = REG_RAX;
  return inst;
}

inline ctype_pin_inst generate_no_dependence_alu_type_inst(uns64 ip, uns64 uid, uns8 inst_size) {
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
  inst.src_regs[0] = REG_RAX;
  inst.src_regs[1] = REG_RBX;
  inst.dst_regs[0] = REG_RCX;
  return inst;
}

inline ctype_pin_inst generate_conditional_branch(uns64 ip, uns64 uid, uns64 tgtAddr, bool direction, uns8 inst_size) {
  ctype_pin_inst inst;
  memset(&inst, 0, sizeof(inst));
  inst.inst_uid = uid;
  inst.instruction_addr = ip;
  inst.instruction_next_addr = direction ? tgtAddr : (ip + inst_size);
  inst.size = inst_size;
  inst.op_type = OP_IADD;
  inst.cf_type = CF_CBR;
  inst.num_simd_lanes = 1;
  inst.lane_width_bytes = 1;
  inst.branch_target = tgtAddr;
  inst.actually_taken = direction ? TAKEN : NOT_TAKEN;
  strcpy(inst.pin_iclass, "DUMMY_CBR_JMP");
  return inst;
}

inline ctype_pin_inst generate_unconditional_branch(uns64 ip, uns64 uid, uns64 tgt, uns8 inst_size) {
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

inline ctype_pin_inst generate_indirect_branch(uns64 ip, uns64 uid, uns64 tgtAddr, uns64 vaddr, uns8 inst_size) {
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
  inst.num_ld = 1;
  inst.ld_vaddr[0] = vaddr;
  strcpy(inst.pin_iclass, "DUMMY_IBR_JUMP");
  return inst;
}

inline ctype_pin_inst make_nop(uns64 ip, uns64 uid, uns64 inst_size, bool fake) {
  ctype_pin_inst inst = create_dummy_nop(ip, WPNM_NOT_IN_WPNM);
  inst.size = inst_size;
  inst.instruction_next_addr = ip + inst_size;
  inst.inst_uid = uid;
  inst.fake_inst = fake ? 0 : 1;
  return inst;
}
#endif