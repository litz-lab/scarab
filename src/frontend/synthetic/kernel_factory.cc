#include "kernel_factory.h"

#include <cassert>
#include <iostream>

#include "bp/bp.param.h"
#include "memory/memory.param.h"

#include "frontend/synthetic/kernel_params.h"
#include "frontend/synthetic/synthetic_bottleneck_kernels.h"
/* Bottleneck name strings */
const char* kernel_names[] = {
#define KERNEL_IMPL(id, name) name,
#include "kernel_table.def"
#undef KERNEL_IMPL
    "invalid"};

// Instruction Dispatch: returns the next instruction, from the kernel to be executed
ctype_pin_inst Kernel_Factory::synthetic_fe_generate_next(uns proc_id, bool offpath) {
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
      return get_next_cbr_kernel_type_inst(proc_id, offpath, 0.2);

    case CBR_LIMITED_50T:
      return get_next_cbr_kernel_type_inst(proc_id, offpath, 0.5);

    case CBR_LIMITED_80T:
      return get_next_cbr_kernel_type_inst(proc_id, offpath, 0.8);

    case IBR_LIMITED_ROUNDROBIN_4TGTS: {
      return get_next_ibr_kernel_type_inst(proc_id, offpath, target_stride, target_pool_size, UNIFORM_SEQUENTIAL);
    }

    case IBR_LIMITED_Random_2TGTS: {
      return get_next_ibr_kernel_type_inst(proc_id, offpath, target_stride, target_pool_size, UNIFORM_RANDOM);
    }

    case IBR_LIMITED_RANDOM_4TGTS: {
      return get_next_ibr_kernel_type_inst(proc_id, offpath, target_stride, target_pool_size, UNIFORM_RANDOM);
    }
    default:
      assert(0 && "Invalid Kernel");
  }
}

/* Helper Definitions */

// dispatches next instruction to be executed the kernel
ctype_pin_inst Kernel_Factory::get_next_kernel_inst() {
  auto it = kernel_map.find(get_next_pc());
  auto inst = it->second;
  assert(it != kernel_map.end() && "Every inst possible should be in the map");
  // update next_pc for next time
  next_pc = inst.instruction_next_addr;
  return it->second;
}

// dipatches next cbr op and rerandomizes the cbr directions if we are at the end of kernel
ctype_pin_inst Kernel_Factory::get_next_cbr_kernel_type_inst(uns proc_id, bool offpath, double t_nt_ratio) {
  auto inst = Kernel_Factory::get_next_kernel_inst();
  /* if we are at the tail end of program regenerate the kernel - this generates new random branch directions, without
  this predictor may predict the intial pattern  */
  if (!offpath && inst.instruction_next_addr == get_start_pc()) {
    kernel_map = generate_cbr_kernel(Sequence_Pick_Strategy::DICRETE_RANDOM, UNIFORM_SEQUENTIAL, workload_length,
                                     t_nt_ratio, workload_length, get_start_pc(), get_start_uid());
  }
  return inst;
}

// dispatches next ibr op and rerandomizes the ibr targets if we are at the end of random targets kernel
ctype_pin_inst Kernel_Factory::get_next_ibr_kernel_type_inst(uns proc_id, bool offpath, uns64 target_stride,
                                                             uns64 num_of_targets,
                                                             Sequence_Pick_Strategy target_strategy) {
  auto inst = get_next_kernel_inst();

  if (!offpath)
    num_of_ibr_ops_executed++;

  /* for random ibr, if the number of ibr ops executed equals the required number of random targets we re-randomize
     targets by generating new kernel */
  if (!offpath && (num_of_ibr_ops_executed == num_of_targets) && target_strategy == UNIFORM_RANDOM) {
    kernel_map = generate_ibr_kernel(target_strategy, num_of_targets, get_start_pc(), get_start_uid(), target_stride,
                                     starting_target);
    num_of_ibr_ops_executed = 0;
  }
  return inst;
}

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_kernel_map(Sequence_Pick_Strategy branch_target_strategy,
                                                                    Limit_Load_To level, double t_nt_ratio,
                                                                    uns num_of_dependence_chains) const {
  switch (kernel) {
    case MEM_BANDWIDTH_LIMITED:
      return generate_load_kernel(NO_DEPENDENCE_CHAIN, workload_length, UNIFORM_SEQUENTIAL, (ICACHE_LINE_SIZE), 4,
                                  DCACHE_LEVEL, get_start_pc(), get_start_uid());
    case DCACHE_LIMITED:
    case MLC_LIMITED:
    case LLC_LIMITED:
    case MEM_LIMITED:
      return generate_load_kernel(DEPENDENCE_CHAIN, workload_length, UNIFORM_SEQUENTIAL, (2 * ICACHE_LINE_SIZE), 0,
                                  level, get_start_pc(), get_start_uid());

    case CBR_LIMITED_20T:
    case CBR_LIMITED_50T:
    case CBR_LIMITED_80T:
      return generate_cbr_kernel(Sequence_Pick_Strategy::DICRETE_RANDOM, UNIFORM_SEQUENTIAL, workload_length,
                                 t_nt_ratio, workload_length, get_start_pc(), get_start_uid());

    case BTB_LIMITED_FULL_ASSOC_SWEEP:
    case BTB_LIMITED_FULL_CAPACITY_SWEEP:
    case BTB_CONTAINED:
      return generate_ubr_kernel(UNIFORM_SEQUENTIAL, target_pool_size, workload_length, get_start_pc(), get_start_uid(),
                                 starting_target, target_stride);

    case IBR_LIMITED_Random_2TGTS:
    case IBR_LIMITED_RANDOM_4TGTS:
    case IBR_LIMITED_ROUNDROBIN_4TGTS:
      return generate_ibr_kernel(branch_target_strategy, target_pool_size, get_start_pc(), get_start_uid(),
                                 target_stride, starting_target);

    case ICACHE_LIMITED:
      return generate_icache_kernel(get_start_pc(), get_start_uid());

    case ILP_LIMITED_0_DEP_CHAIN:
    case ILP_LIMITED_1_DEP_CHAIN:
    case ILP_LIMITED_2_DEP_CHAIN:
    case ILP_LIMITED_4_DEP_CHAIN:
      return generate_ilp_kernel(num_of_dependence_chains, 1200, get_start_pc(), get_start_uid());

    default:
      assert(0 && "Invalid kernel");
  }
}

Kernel_Factory::Kernel_Factory(Kernel_Enum kernel, uns64 start_pc, uns64 start_uid, uns64 workload_length)
    : kernel(kernel),
      start_pc(start_pc),
      start_uid(start_uid),
      next_pc(start_pc),
      workload_length(workload_length),
      target_stride(0),
      target_pool_size(workload_length),
      num_of_ibr_ops_executed(0) {
  /*
    the ff are defaults, we edit them in the switch if we have to, for a particular workload.
    If a particular workload does not require a field, its benign
  */
  std::cout << "Generating " << kernel_names[kernel] << " kernel" << std::endl;
  Limit_Load_To level = DCACHE_LEVEL;
  double t_nt_ratio = 1;
  Sequence_Pick_Strategy branch_target_strategy = UNIFORM_SEQUENTIAL;
  target_stride = 2 * ICACHE_LINE_SIZE;
  starting_target = (get_start_pc() + 2 * ICACHE_LINE_SIZE);
  uns num_of_dependence_chains = 0;

  switch (kernel) {
    // empty cases are intentional to enable assertion of invalid kernels on default case
    case MEM_BANDWIDTH_LIMITED:
      break;
    case DCACHE_LIMITED:
      level = DCACHE_LEVEL;
      break;
    case MLC_LIMITED:
      level = MLC_LEVEL;
      break;
    case LLC_LIMITED:
      level = LLC_LEVEL;
      break;
    case MEM_LIMITED:
      level = MEM_LEVEL;
      break;
    case CBR_LIMITED_20T:
      t_nt_ratio = 0.2;
      break;
    case CBR_LIMITED_50T:
      t_nt_ratio = 0.5;
      break;
    case CBR_LIMITED_80T:
      t_nt_ratio = 0.8;
      break;
    case IBR_LIMITED_Random_2TGTS:
      branch_target_strategy = UNIFORM_RANDOM;
      target_pool_size = 2;
      break;
    case IBR_LIMITED_RANDOM_4TGTS:
      branch_target_strategy = UNIFORM_RANDOM;
      target_pool_size = 4;
      break;
    case IBR_LIMITED_ROUNDROBIN_4TGTS:
      branch_target_strategy = UNIFORM_SEQUENTIAL;
      target_pool_size = 4;
      break;

    case BTB_LIMITED_FULL_ASSOC_SWEEP:
      target_pool_size = BTB_ASSOC + 1;
      workload_length = BTB_ASSOC + 1;
      target_stride = BTB_ENTRIES;
      starting_target = (get_start_pc() + BTB_ENTRIES);
      break;

    case BTB_LIMITED_FULL_CAPACITY_SWEEP:
      target_pool_size = BTB_ENTRIES + 1;
      workload_length = BTB_ENTRIES + 1;
      break;

    case BTB_CONTAINED:
      target_pool_size = BTB_ENTRIES;
      workload_length = BTB_ENTRIES;
      break;

    case ILP_LIMITED_0_DEP_CHAIN:
      break;

    case ILP_LIMITED_1_DEP_CHAIN:
      num_of_dependence_chains = 1;
      break;

    case ILP_LIMITED_2_DEP_CHAIN:
      num_of_dependence_chains = 2;
      break;

    case ILP_LIMITED_4_DEP_CHAIN:
      num_of_dependence_chains = 4;
      break;

    case ICACHE_LIMITED:
      break;

    default:
      assert(0 && "kernel is invalid");
      break;
  }

  kernel_map = generate_kernel_map(branch_target_strategy, level, t_nt_ratio, num_of_dependence_chains);
}