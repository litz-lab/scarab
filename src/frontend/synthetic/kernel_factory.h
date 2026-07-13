#include <map>

#include "isa/isa.h"

#include "ctype_pin_inst.h"
#include "kernel_params.h"
#include "sampler.h"

#ifndef KERNEL_FACTORY_H
#define KERNEL_FACTORY_H

class Kernel_Factory {
  // stores the synthetic kernel
  std::map<uns64, ctype_pin_inst> kernel_map;
  Kernel_Enum kernel;

  // starting pc and uid for generating a kernel
  uns64 start_pc;
  uns64 start_uid;

  // next pc to instruction to be read from a generated kernel
  uns64 next_pc;
  uns64 workload_length;

  // preserved so that control flow workloads can be rerandomized
  uns64 starting_target;
  uns64 target_stride;
  uns64 target_pool_size;
  double t_nt_ratio;
  Sequence_Pick_Strategy target_strategy;
  Sequence_Pick_Strategy direction_strategy;
  // counts the number of ibr ops executed
  uns num_of_ibr_ops_executed = 0;

  /* Dispatch helpers */
  // dispatches next instruction to be executed the kernel
  ctype_pin_inst get_next_kernel_inst();
  // dipatches next cbr op and rerandomizes the cbr directions if we are at the end of kernel
  ctype_pin_inst get_next_cbr_kernel_type_inst(uns proc_id, bool offpath);
  // dispatches next ibr op and rerandomizes the ibr targets if they are supposed to be random
  ctype_pin_inst get_next_ibr_kernel_type_inst(uns proc_id, bool offpath);

  /* Constructor Helpers */

  // Top level helper
  std::map<uns64, ctype_pin_inst> generate_kernel_map(Limit_Load_To level = DCACHE_LEVEL,
                                                      uns num_of_dependence_chains = 1);

  /* Downstream configurable helpers */
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

  // generates a sequence of nops and returns the location the next instruction starts from
  uns64 generate_nop_sequence(std::map<uns64, ctype_pin_inst>& kernel_map, Sampler& uid_sequence, uns num_of_nops,
                              uns64 starting_pc);

 public:
  // Constructor
  Kernel_Factory(Kernel_Enum kernel, uns64 start_pc, uns64 start_uid, uns64 workload_length);
  // Instruction Dispatch: returns the next instruction, from the kernel to be executed
  ctype_pin_inst synthetic_fe_generate_next(uns proc_id, bool offpath);

  uns64 get_start_pc() const { return start_pc; }
  uns64 get_start_uid() const { return start_uid; }
  uns64 get_next_pc() const { return next_pc; }

  void redirect_next_pc(uns64 pc) { next_pc = pc; }
};

#endif