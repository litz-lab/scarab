#include "kernel_factory.h"

#include <cassert>
#include <iostream>

#include "bp/bp.param.h"
#include "memory/memory.param.h"

#include "frontend/synthetic/kernel_params.h"
/* Bottleneck name strings */
const char* kernel_names[] = {
#define KERNEL_IMPL(id, name) name,
#include "kernel_table.def"
#undef KERNEL_IMPL
    "invalid"};

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_kernel() {
  switch (kernel) {
    case MEM_BANDWIDTH_LIMITED:
      return generate_load_kernel(NO_DEPENDENCE_CHAIN);
      break;
    case DCACHE_LIMITED:
    case MLC_LIMITED:
    case LLC_LIMITED:
    case MEM_LIMITED:
      return generate_load_kernel(DEPENDENCE_CHAIN);
      break;
    case CBR_LIMITED_20T:
    case CBR_LIMITED_50T:
    case CBR_LIMITED_80T:
      return generate_cbr_kernel();
      break;
    case BTB_LIMITED_FULL_ASSOC_SWEEP:
    case BTB_LIMITED_FULL_CAPACITY_SWEEP:
    case BTB_CONTAINED:
      return generate_ubr_kernel();
      break;
    case IBR_LIMITED_Random_2TGTS:
    case IBR_LIMITED_RANDOM_4TGTS:
    case IBR_LIMITED_ROUNDROBIN_4TGTS:
      return generate_ibr_kernel();
      break;
    case ICACHE_LIMITED:
      return generate_icache_kernel();
      break;
    case ILP_LIMITED_0_DEP_CHAIN:
    case ILP_LIMITED_1_DEP_CHAIN:
    case ILP_LIMITED_2_DEP_CHAIN:
    case ILP_LIMITED_4_DEP_CHAIN:
      return generate_ilp_kernel(num_of_dependence_chains, workload_length);
      break;
    default:
      assert(0 && "Invalid kernel");
  }
}

Kernel_Factory::Kernel_Factory(Kernel_Enum kernel, uns64 start_pc, uns64 start_uid, uns64 workloadlength)
    /*
      the ff are defaults, we edit them in the switch if we have to, for a particular workload.
      If a particular workload does not require a field, its benign
    */
    : kernel(kernel),
      start_pc(start_pc),
      start_uid(start_uid),
      workload_length(workloadlength),
      starting_target((get_start_pc())),
      target_stride(2 * ICACHE_LINE_SIZE),
      target_pool_size(workloadlength),
      t_nt_ratio(1),
      level(DCACHE_LEVEL),
      num_of_dependence_chains(0),
      inst_size(8),
      nop_size(ICACHE_LINE_SIZE / (ISSUE_WIDTH)),
      pad_length(500),
      target_strategy(UNIFORM_SEQUENTIAL),
      direction_strategy(DICRETE_RANDOM),
      uid_sequence(UNIFORM_SEQUENTIAL, 1, (workload_length + pad_length + 1), start_uid, 1),
      pc_sequence(UNIFORM_SEQUENTIAL, 1, (workload_length), start_pc, inst_size),
      target_pool(target_strategy, 1, target_pool_size, get_start_pc(), target_stride) {
  std::cout << "Generating " << kernel_names[kernel] << " kernel" << std::endl;
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
      target_strategy = UNIFORM_RANDOM;
      break;
    case CBR_LIMITED_50T:
      t_nt_ratio = 0.5;
      target_strategy = UNIFORM_RANDOM;
      break;
    case CBR_LIMITED_80T:
      t_nt_ratio = 0.8;
      target_strategy = UNIFORM_RANDOM;
      break;
    case IBR_LIMITED_Random_2TGTS:
      target_strategy = UNIFORM_RANDOM;
      target_pool_size = 2;
      break;
    case IBR_LIMITED_RANDOM_4TGTS:
      target_strategy = UNIFORM_RANDOM;
      target_pool_size = 4;
      break;
    case IBR_LIMITED_ROUNDROBIN_4TGTS:
      target_strategy = UNIFORM_SEQUENTIAL;
      target_pool_size = 4;
      break;
    case BTB_LIMITED_FULL_ASSOC_SWEEP:
      target_pool_size = BTB_ASSOC + 1;
      workload_length = BTB_ASSOC + 1;
      target_stride = BTB_ENTRIES;
      target_strategy = UNIFORM_SEQUENTIAL;
      break;
    case BTB_LIMITED_FULL_CAPACITY_SWEEP:
      target_stride = (ICACHE_LINE_SIZE);
      target_pool_size = BTB_ENTRIES + 1;
      workload_length = BTB_ENTRIES + 1;
      target_strategy = UNIFORM_RANDOM;
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
      workload_length = 2 * (ICACHE_SIZE / ICACHE_LINE_SIZE);
      inst_size = ICACHE_LINE_SIZE;
      break;
    default:
      assert(0 && "kernel is invalid");
      break;
  }
  pc_sequence = Sampler(UNIFORM_SEQUENTIAL, 1, (workload_length), start_pc, inst_size);
  target_pool = Sampler(target_strategy, 1, target_pool_size, starting_target, target_stride);
}
