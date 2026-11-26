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

/* Memory hierachy level name strings */
const char* mem_hierachy_level_names[] = {
#define MEMORY_HIERACHY_LEVEL_IMPL(id, name) name,
#include "mem_hierachy_level_table.def"
#undef MEMORY_HIERACHY_LEVEL_IMPL
    "invalid"};

const char* sampling_strategy_names[] = {
#define SAMPLING_STRATEGY_IMPL(id, name) name,
#include "sampling_strategy_table.def"
#undef SAMPLING_STRATEGY_IMPL
    "invalid"};

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_kernel() {
  switch (kernel) {
    case MEM_FU_LIMITED:
      return generate_load_kernel(BANDWIDTH);
    case LOAD_LATENCY_LIMITED:
      return generate_load_kernel(LATENCY);
    case CBR_LIMITED:
      return generate_cbr_kernel();
    case BTB_LIMITED:
      return generate_ubr_kernel();
    case IBR_LIMITED: {
      if (target_strategy == UNIFORM_SEQUENTIAL)
        return generate_round_robin_ibr_kernel();
      else
        return generate_random_ibr_kernel();
    }
    case ICACHE_LIMITED:
      return generate_icache_kernel();
    case ILP_LIMITED:
      return generate_ilp_kernel(num_of_dependence_chains, workload_length);
    default:
      assert(0 && "Invalid kernel");
  }
}

Kernel_Factory::Kernel_Factory(Kernel_Enum kernel, uns64 start_pc, uns64 start_uid)
    : kernel(kernel),
      start_pc(start_pc),
      start_uid(start_uid),
      workload_length(WORKLOAD_LENGTH),
      starting_target(STARTING_TARGET),
      target_stride(TARGET_STRIDE),
      target_pool_size(TARGET_POOL_SIZE),
      t_nt_ratio(T_NT_RATIO),
      level(static_cast<Limit_Load_To>(MEM_HIERACHY_LEVEL)),
      num_of_dependence_chains(DEPENDENCE_CHAIN_COUNT),
      inst_size(INST_SIZE),
      nop_size(NOP_SIZE),
      pad_length(PAD_LENGTH),
      target_strategy(static_cast<Sampling_Strategy>(TARGET_SAMPLING_STRATEGY)),
      direction_strategy(static_cast<Sampling_Strategy>(DIRECTION_SAMPLING_STRATEGY)),
      uid_sequence(UNIFORM_SEQUENTIAL, 1, (WORKLOAD_LENGTH + PAD_LENGTH + 1), start_uid, 1),
      pc_sequence(UNIFORM_SEQUENTIAL, 1, WORKLOAD_LENGTH, start_pc, INST_SIZE),
      target_pool(static_cast<Sampling_Strategy>(TARGET_SAMPLING_STRATEGY), 1, TARGET_POOL_SIZE, get_start_pc(),
                  TARGET_STRIDE) {
  std::cout << "Generating " << kernel_names[kernel] << " kernel" << std::endl;
}
