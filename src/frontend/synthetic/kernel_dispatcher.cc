#include "kernel_dispatcher.h"

#include <cassert>

ctype_pin_inst Kernel_Dispatcher::synthetic_fe_generate_next(uns proc_id, bool offpath) {
  switch (kernel) {
    case MEM_BANDWIDTH_LIMITED:
    case DCACHE_LIMITED:
    case MLC_LIMITED:
    case LLC_LIMITED:
    case MEM_LIMITED:
    case BTB_LIMITED_FULL_ASSOC_SWEEP:
    case BTB_LIMITED_FULL_CAPACITY_SWEEP:
    case BTB_CONTAINED:
    case ICACHE_LIMITED:
    case ILP_LIMITED_0_DEP_CHAIN:
    case ILP_LIMITED_1_DEP_CHAIN:
    case ILP_LIMITED_2_DEP_CHAIN:
    case ILP_LIMITED_4_DEP_CHAIN:
      return get_next_kernel_inst();

    case CBR_LIMITED_20T:
    case CBR_LIMITED_50T:
    case CBR_LIMITED_80T:
      return get_next_cbr_kernel_type_inst(proc_id, offpath);

    case IBR_LIMITED_ROUNDROBIN_4TGTS:
    case IBR_LIMITED_Random_2TGTS:
    case IBR_LIMITED_RANDOM_4TGTS: {
      return get_next_ibr_kernel_type_inst(proc_id, offpath);
    }
    default:
      assert(0 && "Invalid Kernel");
  }
}

// dispatches next instruction to be executed the kernel
ctype_pin_inst Kernel_Dispatcher::get_next_kernel_inst() {
  auto it = kernel_map.find(get_next_pc());
  auto inst = it->second;
  assert(it != kernel_map.end() && "Every inst possible should be in the map");
  // update next_pc for next time
  next_pc = inst.instruction_next_addr;
  return it->second;
}

// dipatches next cbr op and rerandomizes the cbr directions if we are at the end of kernel
ctype_pin_inst Kernel_Dispatcher::get_next_cbr_kernel_type_inst(uns proc_id, bool offpath) {
  auto inst = Kernel_Dispatcher::get_next_kernel_inst();
  /* if we execute the workload length or return via the unconditional branch path  */
  if (!offpath && (inst.op_type == OP_CF && inst.cf_type == CF_CBR)) {
    num_of_ops_executed++;

    if (num_of_ops_executed == factory.get_target_pool_size()) {
      num_of_ops_executed = 0;
      kernel_map = factory.generate_kernel();
    }
  }
  return inst;
}

// dispatches next ibr op and rerandomizes the ibr targets if we are at the end of random targets kernel
ctype_pin_inst Kernel_Dispatcher::get_next_ibr_kernel_type_inst(uns proc_id, bool offpath) {
  auto inst = get_next_kernel_inst();
  if (!offpath && (inst.op_type == OP_CF && inst.cf_type == CF_IBR)) {
    num_of_ops_executed++;

    if (num_of_ops_executed == factory.get_target_pool_size()) {
      num_of_ops_executed = 0;
      factory.regenerate_target_pool();
      factory.reset_target_pool_pointer();
      kernel_map = factory.generate_kernel();
    }
  }
  return inst;
}

Kernel_Dispatcher::Kernel_Dispatcher(Kernel_Enum kernel_, uns64 start_pc, uns64 start_uid, uns64 workload_length)
    : factory(kernel_, start_pc, start_uid, workload_length),
      kernel(kernel_),
      kernel_map(factory.generate_kernel()),
      next_pc(start_pc),
      num_of_ops_executed{0} {
}
