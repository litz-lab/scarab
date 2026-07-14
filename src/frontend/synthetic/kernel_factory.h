#include <map>

#include "isa/isa.h"

#include "ctype_pin_inst.h"
#include "kernel_params.h"
#include "sampler_params.h"

#ifndef KERNEL_FACTORY_H
#define KERNEL_FACTORY_H

class Kernel_Factory {
  // stores the synthetic kernel
  std::map<uns64, ctype_pin_inst> kernel_map;
  Kernel_Enum kernel;

  // starting pc and uid for generating a kernel
  uns64 start_pc = 0;
  uns64 start_uid = 0;

  // next pc to instruction to be read from a generated kernel
  uns64 next_pc;
  uns64 workload_length;

  // used by ubr and ibr workloads
  uns64 starting_target;
  uns64 target_stride = 0;
  uns64 target_pool_size = 0;

  // utilised in ibr_workloads
  uns num_of_ibr_ops_executed = 0;

  /* Dispatcher helpers */

  // dispatches next instruction to be executed the kernel
  ctype_pin_inst get_next_kernel_inst();
  // dipatches next cbr op and rerandomizes the cbr directions if we are at the end of kernel
  ctype_pin_inst get_next_cbr_kernel_type_inst(uns proc_id, bool offpath, double t_nt_ratio);
  // dispatches next ibr op and rerandomizes the ibr targets if they are supposed to be random
  ctype_pin_inst get_next_ibr_kernel_type_inst(uns proc_id, bool offpath, uns64 target_stride, uns64 num_of_targets,
                                               Sequence_Pick_Strategy target_strategy);

  // Constructor Helper
  std::map<uns64, ctype_pin_inst> generate_kernel_map(Sequence_Pick_Strategy branch_target_strategy,
                                                      Limit_Load_To level, double t_nt_ratio,
                                                      uns num_of_dependence_chains) const;

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