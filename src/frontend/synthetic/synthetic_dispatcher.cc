#include <iostream>

#include "bp/bp.param.h"
#include "memory/memory.param.h"

#include "frontend/synthetic/kernel_params.h"
#include "frontend/synthetic/synthetic_kernels.h"

/* static globals */
uns64 synth_fe_curr_pc{START_PC};
uns64 synth_fe_curr_uid{UID_START};

/* Bottleneck name strings */
const char* kernel_names[] = {
#define KERNEL_IMPL(id, name) name,
#include "kernel_table.def"
#undef KERNEL_IMPL
    "invalid"};

Kernel_Enum kernel;

/* Dispatcher helper Prototypes */
ctype_pin_inst get_next_kernel_inst(const std::map<uns64, ctype_pin_inst>& kernel_map);
ctype_pin_inst get_next_mem_latency_kernel_type_inst(uns proc_id, Limit_Load_To load_level);
ctype_pin_inst get_next_cbr_kernel_type_inst(uns proc_id, bool offpath, double t_nt_ratio);
ctype_pin_inst get_next_ubr_kernel_type_inst(uns proc_id, bool offpath, uns64 workload_length, uns64 target_pool_size,
                                             uns64 starting_target, uns64 target_stride);
ctype_pin_inst get_next_ibr_kernel_type_inst(uns proc_id, bool offpath, uns64 target_stride, uns64 num_of_targets,
                                             Sequence_Pick_Strategy target_strategy);
ctype_pin_inst get_next_ilp_kernel_type_inst(uns proc_id, uns dependence_chain_length);

void synthetic_kernel_init() {
  std::cout << "Simulating " << kernel_names[KERNEL] << " synthetic kernel" << std::endl;
}

/* Kernel Dispatcher */
ctype_pin_inst synthetic_fe_generate_next(uns proc_id, bool offpath) {
  switch (kernel) {
    case MEM_BANDWIDTH_LIMITED: {
      static auto kernel_map{generate_load_kernel(NO_DEPENDENCE_CHAIN, 500, NORMAL_ROUND_ROBIN, (ICACHE_LINE_SIZE), 4,
                                                  DCACHE_LEVEL, START_PC, UID_START)};
      // get next inst
      return get_next_kernel_inst(kernel_map);
    }

    case DCACHE_LIMITED:
      return get_next_mem_latency_kernel_type_inst(proc_id, DCACHE_LEVEL);

    case MLC_LIMITED:
      return get_next_mem_latency_kernel_type_inst(proc_id, MLC_LEVEL);

    case LLC_LIMITED:
      return get_next_mem_latency_kernel_type_inst(proc_id, LLC_LEVEL);

    case MEM_LIMITED:
      return get_next_mem_latency_kernel_type_inst(proc_id, MEM_LEVEL);

    case CBR_LIMITED_20T:
      return get_next_cbr_kernel_type_inst(proc_id, offpath, 0.2);

    case CBR_LIMITED_50T:
      return get_next_cbr_kernel_type_inst(proc_id, offpath, 0.5);

    case CBR_LIMITED_80T:
      return get_next_cbr_kernel_type_inst(proc_id, offpath, 0.8);

    case BTB_LIMITED_FULL_ASSOC_SWEEP: {
      const uns64 target_pool_size = BTB_ASSOC + 1;
      const uns64 workload_length = BTB_ASSOC + 1;
      const uns64 target_Stride = BTB_ENTRIES;
      const uns64 starting_target = (START_PC + BTB_ENTRIES);

      return get_next_ubr_kernel_type_inst(proc_id, offpath, workload_length, target_pool_size, starting_target,
                                           target_Stride);
    }

    case BTB_LIMITED_FULL_CAPACITY_SWEEP: {
      const uns64 target_pool_size = BTB_ENTRIES + 1;
      const uns64 workload_length = BTB_ENTRIES + 1;
      const uns64 target_Stride = (2 * ICACHE_LINE_SIZE);
      const uns64 starting_target = (START_PC + 2 * ICACHE_LINE_SIZE);
      return get_next_ubr_kernel_type_inst(proc_id, offpath, workload_length, target_pool_size, starting_target,
                                           target_Stride);
    }

    case IBR_LIMITED_ROUNDROBIN_4TGTS: {
      const uns target_pool_size{4};
      const uns64 target_stride{2 * ICACHE_LINE_SIZE};
      return get_next_ibr_kernel_type_inst(proc_id, offpath, target_stride, target_pool_size, NORMAL_ROUND_ROBIN);
    }

    case IBR_LIMITED_Random_2TGTS: {
      const uns target_pool_size{2};
      const uns64 target_stride{2 * ICACHE_LINE_SIZE};
      return get_next_ibr_kernel_type_inst(proc_id, offpath, target_stride, target_pool_size, RANDOM);
    }

    case IBR_LIMITED_RANDOM_4TGTS: {
      const uns target_pool_size{4};
      const uns64 target_stride{2 * ICACHE_LINE_SIZE};
      return get_next_ibr_kernel_type_inst(proc_id, offpath, target_stride, target_pool_size, RANDOM);
    }

    case ICACHE_LIMITED: {
      static auto kernel_map{generate_icache_kernel(synth_fe_curr_pc, synth_fe_curr_uid)};
      return get_next_kernel_inst(kernel_map);
    }

    case ILP_LIMITED_1_DEP_CHAIN: {
      return get_next_ilp_kernel_type_inst(proc_id, 1);
    }

    case ILP_LIMITED_2_DEP_CHAIN: {
      return get_next_ilp_kernel_type_inst(proc_id, 2);
    }

    case ILP_LIMITED_4_DEP_CHAIN: {
      return get_next_ilp_kernel_type_inst(proc_id, 4);
    }

    default:
      return generate_nop(synth_fe_curr_pc++, synth_fe_curr_uid++, NOP_SIZE, false);
  }
}

/* Helper Definitions */

ctype_pin_inst get_next_kernel_inst(const std::map<uns64, ctype_pin_inst>& kernel_map) {
  auto it = kernel_map.find(synth_fe_curr_pc);
  auto inst = it->second;
  assert(it != kernel_map.end() && "Every inst possible should be in the map");
  synth_fe_curr_pc = inst.instruction_next_addr;
  return it->second;
}

ctype_pin_inst get_next_mem_latency_kernel_type_inst(uns proc_id, Limit_Load_To load_level) {
  // generate map that contains the entire workload
  static auto kernel_map{generate_load_kernel(DEPENDENCE_CHAIN, 1000, NORMAL_ROUND_ROBIN, (2 * ICACHE_LINE_SIZE), 0,
                                              load_level, START_PC, UID_START)};
  return get_next_kernel_inst(kernel_map);
}

ctype_pin_inst get_next_cbr_kernel_type_inst(uns proc_id, bool offpath, double t_nt_ratio) {
  const uns64 workload_length = 100;
  // create map that contains the entire cbr workload
  static std::map<uns64, ctype_pin_inst> kernel_map = generate_cbr_kernel(
      RANDOM, NORMAL_ROUND_ROBIN, workload_length, t_nt_ratio, workload_length, START_PC, UID_START);

  auto inst = get_next_kernel_inst(kernel_map);
  // if we are at the tail end of program regenerate the kernel - this generates new random branch directions
  if (!offpath && inst.instruction_next_addr == START_PC) {
    kernel_map = generate_cbr_kernel(RANDOM, NORMAL_ROUND_ROBIN, workload_length, t_nt_ratio, workload_length, START_PC,
                                     UID_START);
  }
  return inst;
}

ctype_pin_inst get_next_ubr_kernel_type_inst(uns proc_id, bool offpath, uns64 workload_length, uns64 target_pool_size,
                                             uns64 starting_target, uns64 target_stride) {
  // create map that contains the entire btb workload
  static std::map<uns64, ctype_pin_inst> kernel_map = generate_ubr_kernel(
      NORMAL_ROUND_ROBIN, target_pool_size, workload_length, START_PC, UID_START, starting_target, target_stride);

  return get_next_kernel_inst(kernel_map);
}

ctype_pin_inst get_next_ibr_kernel_type_inst(uns proc_id, bool offpath, uns64 target_stride, uns64 num_of_targets,
                                             Sequence_Pick_Strategy target_strategy) {
  static uint _insts_executed{0};
  /* randomising the targets can be tricky if the starting pc is part of the distribution
     thus the distribution should be safely away from START_PC */
  static const uns64 starting_target{(START_PC + 2 * ICACHE_LINE_SIZE)};

  // create map that contains the entire ibr workload
  static std::map<uns64, ctype_pin_inst> kernel_map =
      generate_ibr_kernel(target_strategy, num_of_targets, START_PC, UID_START, target_stride, starting_target);

  auto inst = get_next_kernel_inst(kernel_map);

  if (!offpath)
    _insts_executed++;

  /* for random ibr, if the number of insts executed equals the required random targets we re-randomize targets
     by generating new kernel */
  if (!offpath && (_insts_executed == num_of_targets) && target_strategy == RANDOM) {
    kernel_map =
        generate_ibr_kernel(target_strategy, num_of_targets, START_PC, UID_START, target_stride, starting_target);
    _insts_executed = 0;
  }
  return inst;
}

ctype_pin_inst get_next_ilp_kernel_type_inst(uns proc_id, uns dependence_chain_length) {
  // create map that contains the entire ilp workload
  static auto kernel_map{generate_ilp_kernel(dependence_chain_length, 1200, START_PC, UID_START)};
  // return next inst
  return get_next_kernel_inst(kernel_map);
}