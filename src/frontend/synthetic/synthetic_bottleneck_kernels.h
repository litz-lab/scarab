#ifndef __SYNTHETIC_KERNELS_H__
#define __SYNTHETIC_KERNELS_H__
#include <map>

#include "globals/global_types.h"

#include "ctype_pin_inst.h"
#include "sampler.h"

/* Microkernels Init Utilities */
void synthetic_kernel_init();

/* Microkernel APIs */
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