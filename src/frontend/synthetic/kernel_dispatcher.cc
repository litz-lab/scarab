extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"
}
#include <cassert>

#include "kernel_dispatcher.h"

#define DEBUG(args...) _SDEBUG(DEBUG_SYNTHETIC_KERNEL_DISPATCHER, ##args)

ctype_pin_inst Kernel_Dispatcher::synthetic_fe_generate_next(uns proc_id, bool offpath) {
  switch (kernel) {
    case MEM_FU_LIMITED:
    case LOAD_LATENCY_LIMITED:
    case BTB_LIMITED:
    case ICACHE_LIMITED:
    case ILP_LIMITED:
      return get_next_kernel_inst();
    case CBR_LIMITED:
      return get_next_cbr_kernel_type_inst(proc_id, offpath);

    case IBR_LIMITED: {
      if (TARGET_SAMPLING_STRATEGY == UNIFORM_SEQUENTIAL)
        return get_next_kernel_inst();
      else
        return get_next_ibr_kernel_type_inst(proc_id, offpath);
    }

    default:
      assert(0 && "Invalid Kernel");
  }
}

// dispatches next instruction to be executed the kernel
ctype_pin_inst Kernel_Dispatcher::get_next_kernel_inst(uns proc_id) {
  auto inst_pc = get_next_pc();
  auto it = kernel_map.find(inst_pc);
  // at this stage inst_count[proc_id] and op_count[proc_id] are invalid hence cant use DEBUG macro
  if (DEBUG_SYNTHETIC_KERNEL_DISPATCHER) {
    if (it != kernel_map.end())
      printf("\nop_type:%d ip:%llu next_addr:%llu size:%d branch_target:%llu branch_dir:%d inst_uid:%llu",
             (uns32)it->second.st.op_type, (unsigned long long)it->second.st.instruction_addr,
             (unsigned long long)it->second.instruction_next_addr, (uns32)it->second.st.size,
             (unsigned long long)it->second.st.branch_target, (uns32)it->second.actually_taken,
             (unsigned long long)it->second.inst_uid);
    else {
      printf("Missing inst at pc: %llu", inst_pc);
     
    }
  }
  ASSERT(proc_id, it != kernel_map.end());
  auto inst = it->second;
  next_pc = inst.instruction_next_addr;
  return inst;
}

// dipatches next cbr op and rerandomizes the cbr directions if we are at the end of kernel
ctype_pin_inst Kernel_Dispatcher::get_next_cbr_kernel_type_inst(uns proc_id, bool offpath) {
  auto inst = Kernel_Dispatcher::get_next_kernel_inst();
  /* if we execute the workload length or return via the unconditional branch path  */
  if (!offpath && (inst.st.op_type == OP_CF && inst.st.cf_type == CF_CBR)) {
    num_of_ops_executed++;

    if (num_of_ops_executed == factory.get_target_pool_size()) {
      num_of_ops_executed = 0;
      factory.reset_target_pool_pointer();
      kernel_map = factory.generate_kernel();
    }
  }
  return inst;
}

// dispatches next ibr op and rerandomizes the ibr targets if we are at the end of random targets kernel
ctype_pin_inst Kernel_Dispatcher::get_next_ibr_kernel_type_inst(uns proc_id, bool offpath) {
  auto inst = get_next_kernel_inst();
  if (!offpath && (inst.st.op_type == OP_CF && (inst.st.cf_type == CF_IBR))) {
    num_of_ops_executed++;
    // rerandomize the target pool if we reach the end
    if (num_of_ops_executed == TARGET_POOL_SIZE - 1) {
      num_of_ops_executed = 0;
      factory.regenerate_target_pool();
      factory.reset_target_pool_pointer();
    }
    // for the random ibr kernel, this moves the pointer to the next indirect branch target
    kernel_map = factory.generate_kernel();
  }
  return inst;
}

Kernel_Dispatcher::Kernel_Dispatcher(Kernel_Enum kernel_, uns64 start_pc, uns64 start_uid)
    : factory(kernel_, start_pc, start_uid),
      kernel(kernel_),
      kernel_map(factory.generate_kernel()),
      next_pc(start_pc),
      num_of_ops_executed{0} {
}
