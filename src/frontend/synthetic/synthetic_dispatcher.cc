#include <iostream>

#include "bp/bp.param.h"
#include "memory/memory.param.h"

#include "frontend/synthetic/synthetic_kernels.h"

/* Bottleneck name strings */
const char* bottleneckNames[] = {
#define BOTTLENECK_IMPL(id, name) name,
#include "bottlenecks_table.def"
#undef BOTTLENECK_IMPL
    "invalid"};

BottleNeck_enum bottleneck;

/* Dispatcher helper Prototypes */
template <typename inst_gen>
ctype_pin_inst get_next_kernel_inst(uns64 ip, std::map<uns64, ctype_pin_inst>& old_kernel_map, inst_gen gen);
ctype_pin_inst get_next_mem_latency_kernel_type_inst(uns64 ip, Limit_Load_To load_level);
ctype_pin_inst get_next_cbr_kernel_type_inst(uns64 ip, uns64 uid, bool offpath, double t_nt_ratio);
ctype_pin_inst get_next_btb_kernel_type_inst(uns64 ip, uns64 uid, bool offpath, uns64 workload_length,
                                             uns64 target_pool_size, uns64 starting_target, uns64 target_stride);
ctype_pin_inst get_next_ibr_kernel_type_inst(uns64 ip, uns64 uid, bool offpath, uns64 target_stride,
                                             uns64 num_of_targets, Sequence_Pick_Strategy target_strategy);
ctype_pin_inst get_next_ilp_kernel_type_inst(uns64 ip, uns dependence_chain_length);

void synthetic_kernel_init() {
  std::cout << "Simulating synthetic " << bottleneckNames[BOTTLENECK] << " bottleneck" << std::endl;
}

/* Kernel Dispatcher */
ctype_pin_inst generate_synthetic_microkernel(uns proc_id, BottleNeck_enum bottleneck_type, uns64 ip, uns64 uid,
                                              bool offpath) {
  switch (bottleneck_type) {
    case MEM_BANDWIDTH_LIMITED: {
      static auto kernel_map{generate_load_kernel(NO_DEPENDENCE_CHAIN, 1000, NORMAL_ROUND_ROBIN, (2 * ICACHE_LINE_SIZE),
                                                  8, DCACHE_LEVEL, START_PC, UID_START)};
      return get_next_kernel_inst(ip, kernel_map, []() -> std::map<uns64, ctype_pin_inst> {
        return generate_load_kernel(NO_DEPENDENCE_CHAIN, 1000, NORMAL_ROUND_ROBIN, (2 * ICACHE_LINE_SIZE), 8,
                                    DCACHE_LEVEL,
                                    (kernel_map.rbegin()->second.instruction_addr + kernel_map.rbegin()->second.size),
                                    (kernel_map.rbegin()->second.inst_uid + 1));
      });
    }

    case DCACHE_LIMITED:
      return get_next_mem_latency_kernel_type_inst(ip, DCACHE_LEVEL);

    case MLC_LIMITED:
      return get_next_mem_latency_kernel_type_inst(ip, MLC_LEVEL);

    case LLC_LIMITED:
      return get_next_mem_latency_kernel_type_inst(ip, LLC_LEVEL);

    case MEM_LIMITED:
      return get_next_mem_latency_kernel_type_inst(ip, MEM_LEVEL);

    case CBR_LIMITED_20T:
      return get_next_cbr_kernel_type_inst(ip, uid, offpath, 0.2);

    case CBR_LIMITED_50T:
      return get_next_cbr_kernel_type_inst(ip, uid, offpath, 0.5);

    case CBR_LIMITED_80T:
      return get_next_cbr_kernel_type_inst(ip, uid, offpath, 0.8);

    case BTB_LIMITED_FULL_ASSOC_SWEEP: {
      const uns64 target_pool_size = BTB_ASSOC - 1;
      const uns64 workload_length = BTB_ASSOC - 1;
      const uns64 target_Stride = BTB_ENTRIES;

      return get_next_btb_kernel_type_inst(ip, uid, offpath, workload_length, target_pool_size,
                                           (START_PC + BTB_ENTRIES), target_Stride);
    }

    case BTB_LIMITED_FULL_CAPACITY_SWEEP: {
      const uns64 target_pool_size = BTB_ENTRIES + 2;
      const uns64 workload_length = BTB_ENTRIES + 2;
      const uns64 target_Stride = (2 * ICACHE_LINE_SIZE);
      return get_next_btb_kernel_type_inst(ip, uid, offpath, workload_length, target_pool_size,
                                           (START_PC + 2 * ICACHE_LINE_SIZE), target_Stride);
    }

    case IBR_LIMITED_ROUNDROBIN_4TGTS: {
      const uns target_pool_size{4};
      const uns64 target_stride{2 * ICACHE_LINE_SIZE};
      return get_next_ibr_kernel_type_inst(ip, uid, offpath, target_stride, target_pool_size, NORMAL_ROUND_ROBIN);
    }

    case IBR_LIMITED_Random_2TGTS: {
      const uns target_pool_size{2};
      const uns64 target_stride{2 * ICACHE_LINE_SIZE};
      return get_next_ibr_kernel_type_inst(ip, uid, offpath, target_stride, target_pool_size, UNIFORM_PROBABILITY);
    }

    case IBR_LIMITED_RANDOM_4TGTS: {
      const uns target_pool_size{4};
      const uns64 target_stride{2 * ICACHE_LINE_SIZE};
      return get_next_ibr_kernel_type_inst(ip, uid, offpath, target_stride, target_pool_size, UNIFORM_PROBABILITY);
    }

    case ICACHE_LIMITED: {
      static auto kernel_map{generate_icache_kernel(START_PC, UID_START)};
      return get_next_kernel_inst(ip, kernel_map, [&]() -> std::map<uns64, ctype_pin_inst> {
        return generate_icache_kernel((kernel_map.rbegin()->second.instruction_addr + kernel_map.rbegin()->second.size),
                                      (kernel_map.rbegin()->second.inst_uid + 1));
      });
    }

    case ILP_LIMITED_1_DEP_CHAIN: {
      return get_next_ilp_kernel_type_inst(ip, 1);
    }

    case ILP_LIMITED_2_DEP_CHAIN: {
      return get_next_ilp_kernel_type_inst(ip, 2);
    }

    case ILP_LIMITED_4_DEP_CHAIN: {
      return get_next_ilp_kernel_type_inst(ip, 4);
    }

    default:
      return make_nop(ip, uid, NOP_SIZE, true);
  }
}

/* Helper Definitions */
template <typename inst_gen>
ctype_pin_inst get_next_kernel_inst(uns64 ip, std::map<uns64, ctype_pin_inst>& old_kernel_map,
                                    inst_gen gen_new_kernel) {
  auto it = old_kernel_map.find(ip);
  if (it == old_kernel_map.end()) {
    auto kernel_extend = gen_new_kernel();
    old_kernel_map.insert(kernel_extend.begin(), kernel_extend.end());
    assert(old_kernel_map.size() == 2 * kernel_extend.size() && " extension of kernel did not work well ");
    it = old_kernel_map.find(ip);
    assert(it != old_kernel_map.end() && " extension worked but cannot still find an inst_entry ");
  }
  return it->second;
}

ctype_pin_inst get_next_mem_latency_kernel_type_inst(uns64 ip, Limit_Load_To load_level) {
  static auto kernel_map{generate_load_kernel(DEPENDENCE_CHAIN, 1000, NORMAL_ROUND_ROBIN, (2 * ICACHE_LINE_SIZE), 0,
                                              load_level, START_PC, UID_START)};

  return get_next_kernel_inst(ip, kernel_map, [load_level]() -> std::map<uns64, ctype_pin_inst> {
    return generate_load_kernel(DEPENDENCE_CHAIN, 1000, NORMAL_ROUND_ROBIN, (2 * ICACHE_LINE_SIZE), 0, load_level,
                                (kernel_map.rbegin()->second.instruction_addr + kernel_map.rbegin()->second.size),
                                (kernel_map.rbegin()->second.inst_uid + 1));
  });
}

ctype_pin_inst get_next_cbr_kernel_type_inst(uns64 ip, uns64 uid, bool offpath, double t_nt_ratio) {
  const uns64 workload_length = 100;
  static std::map<uns64, ctype_pin_inst> kernel_map =
      generate_cbr_kernel(BOOLEAN_PROBABILITY, NORMAL_ROUND_ROBIN, workload_length, workload_length, t_nt_ratio,
                          workload_length, START_PC, UID_START);
  if (offpath) {
    return make_nop(ip, uid, NOP_SIZE, false);
  } else {
    auto it = kernel_map.find(ip);
    assert(it != kernel_map.end() && "cannot happen");
    ctype_pin_inst inst = it->second;
    if (inst.instruction_next_addr == START_PC) {
      auto new_map = generate_cbr_kernel(BOOLEAN_PROBABILITY, NORMAL_ROUND_ROBIN, workload_length, workload_length,
                                         t_nt_ratio, workload_length, START_PC, UID_START);
      kernel_map.swap(new_map);
    }
    return inst;
  }
}

ctype_pin_inst get_next_btb_kernel_type_inst(uns64 ip, uns64 uid, bool offpath, uns64 workload_length,
                                             uns64 target_pool_size, uns64 starting_target, uns64 target_stride) {
  static std::map<uns64, ctype_pin_inst> kernel_map = generate_ubr_kernel(
      NORMAL_ROUND_ROBIN, target_pool_size, workload_length, START_PC, UID_START, starting_target, (target_stride));
  if (offpath) {
    return make_nop(ip, uid, NOP_SIZE, false);
  } else {
    auto it = kernel_map.find(ip);
    const ctype_pin_inst inst = it->second;
    assert(it != kernel_map.end() && "cannot happen");
    return inst;
  }
}

ctype_pin_inst get_next_ibr_kernel_type_inst(uns64 ip, uns64 uid, bool offpath, uns64 target_stride,
                                             uns64 num_of_targets, Sequence_Pick_Strategy target_strategy) {
  static std::map<uns64, ctype_pin_inst> kernel_map = generate_ibr_kernel(
      target_strategy, num_of_targets, START_PC, UID_START, (2 * ICACHE_LINE_SIZE), (START_PC + 6 * ICACHE_LINE_SIZE));
  if (offpath) {
    return make_nop(ip, uid, NOP_SIZE, false);
  } else {
    auto it = kernel_map.find(ip);
    ctype_pin_inst inst = it->second;
    assert(it != kernel_map.end() && "cannot happen");
    // in the random case we must reinitialise kernel_map, for normal round-robin kernel_map remains the same
    if (target_strategy != NORMAL_ROUND_ROBIN && inst.instruction_next_addr == START_PC) {
      kernel_map = generate_ibr_kernel(target_strategy, num_of_targets, START_PC, UID_START, (2 * ICACHE_LINE_SIZE),
                                       (START_PC + 6 * ICACHE_LINE_SIZE));
    }
    return inst;
  }
}

ctype_pin_inst get_next_ilp_kernel_type_inst(uns64 ip, uns dependence_chain_length) {
  static auto kernel_map{generate_ilp_kernel(dependence_chain_length, 1200, START_PC, UID_START)};
  return get_next_kernel_inst(ip, kernel_map, [&]() -> std::map<uns64, ctype_pin_inst> {
    return generate_ilp_kernel(dependence_chain_length, 1200,
                               (kernel_map.rbegin()->second.instruction_addr + kernel_map.rbegin()->second.size),
                               (kernel_map.rbegin()->second.inst_uid + 1));
  });
}