#ifndef __SYNTHETIC_KERNELS_H__
#define __SYNTHETIC_KERNELS_H__
#include <map>

#include "globals/global_types.h"

#include "ctype_pin_inst.h"
#include "distributions.h"

/* Definies */

#define NOP_SIZE ICACHE_LINE_SIZE / (ISSUE_WIDTH)
#define BRANCH_SIZE ICACHE_LINE_SIZE - (NOP_SIZE * (ISSUE_WIDTH - 1))
#define ALU_ADD_SIZE 8
#define LOAD_INST_SIZE 8
#define START_PC 256
#define UID_START 1000

/* static globals */
extern uns64 synth_fe_curr_pc;
extern uns64 synth_fe_curr_uid;

/* Enum for Kernels */
typedef enum Load_Kernel_Type_Enum {
  DEPENDENCE_CHAIN,
  NO_DEPENDENCE_CHAIN
} Load_Kernel_Type;

typedef enum Limit_Load_To_Enum {
  MLC_LEVEL,
  LLC_LEVEL,
  MEM_LEVEL,
  DCACHE_LEVEL
} Limit_Load_To;

/* Microkernels Init Utilities */
void synthetic_kernel_init();

/* Kernel dispatcher */
ctype_pin_inst synthetic_fe_generate_next(uns proc_id, bool offpath);

/*  Basic one Line APIs */
ctype_pin_inst generate_generic_load(uns64 ip, uns64 uid, uns64 vaddr, uns8 inst_size, uns8 ld_addr_reg, uns8 dest_reg);
ctype_pin_inst generate_alu_type_inst(uns64 ip, uns64 uid, uns8 inst_size, uns8 regDest, uns8 regSrc1, uns8 regSrc2);
ctype_pin_inst generate_conditional_branch(uns64 ip, uns64 uid, uns64 tgtAddr, bool direction, uns8 inst_size);
ctype_pin_inst generate_unconditional_branch(uns64 ip, uns64 uid, uns64 tgt, uns8 inst_size);
ctype_pin_inst generate_indirect_branch(uns64 ip, uns64 uid, uns64 tgtAddr, uns64 vaddr, uns8 inst_size);
ctype_pin_inst generate_nop(uns64 ip, uns64 uid, uns64 inst_size, bool fake);

/*  Parameteized Microkernel APIs */
std::map<uns64, ctype_pin_inst> generate_ubr_kernel(Sequence_Pick_Strategy branch_target_pick_strategy,
                                                    uns64 target_pool_size, uns64 workload_length, uns64 start_pc,
                                                    uns64 start_uid, uns64 starting_target, uns64 target_stride);

std::map<uns64, ctype_pin_inst> generate_ilp_kernel(uns dependence_chain_length, uns workload_length, uns64 start_pc,
                                                    uns64 start_uid);

std::map<uns64, ctype_pin_inst> generate_load_kernel(Load_Kernel_Type type, uns workload_length,
                                                     Sequence_Pick_Strategy mem_address_pick_srategy,
                                                     uns64 start_mem_address, uns64 mem_addresses_stride,
                                                     Limit_Load_To level, uns64 start_pc, uns64 start_uid);

std::map<uns64, ctype_pin_inst> generate_ibr_kernel(Sequence_Pick_Strategy branch_target_pick_strategy,
                                                    uns64 target_pool_size, uns64 start_pc, uns64 start_uid,
                                                    uns64 target_stride, uns64 starting_target);

std::map<uns64, ctype_pin_inst> generate_icache_kernel(uns64 start_pc, uns64 start_uid);

std::map<uns64, ctype_pin_inst> generate_cbr_kernel(Sequence_Pick_Strategy branch_direction_pick_strategy,
                                                    Sequence_Pick_Strategy branch_target_pick_strategy,
                                                    uns64 direction_pool_size, double branch_t_nt_ratio,
                                                    uns64 workload_length, uns64 start_pc, uns64 start_uid);
#endif