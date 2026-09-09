#include "kernel_factory.h"
#ifndef KERNEL_DISPATCHER_H
#define KERNEL_DISPATCHER_H

class Kernel_Dispatcher {
  Kernel_Factory factory;
  Kernel_Enum kernel;
  std::map<uns64, ctype_pin_inst> kernel_map;
  uns64 next_pc;
  uns64 num_of_ops_executed;

 public:
  Kernel_Dispatcher(Kernel_Enum kernel_, uns64 start_pc, uns64 start_uid, uns64 workload_length);
  // dispatches next instruction to be executed the kernel
  ctype_pin_inst get_next_kernel_inst();
  // dipatches next cbr op and rerandomizes the cbr directions if we are at the end of kernel
  ctype_pin_inst get_next_cbr_kernel_type_inst(uns proc_id, bool offpath);
  // dispatches next ibr op and rerandomizes the ibr targets if they are supposed to be random
  ctype_pin_inst get_next_ibr_kernel_type_inst(uns proc_id, bool offpath);

  // Instruction Dispatch: returns the next instruction, from the kernel to be executed
  ctype_pin_inst synthetic_fe_generate_next(uns proc_id, bool offpath);

  uns64 get_next_pc() const { return next_pc; }
  void redirect_next_pc(uns64 pc) { next_pc = pc; }
};

#endif