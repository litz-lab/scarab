#include <cassert>
#include <iostream>

#include "bp/bp.param.h"
#include "memory/memory.param.h"

#include "frontend/synthetic/synthetic_kernels.h"
#include "isa/isa.h"

// pad length is the number of off-path insts to be padded to the backward branch at the tail end of every kernel
#define PAD_LENGTH 300

/* Helper Functions For Microkernels */

// Function to generate leading nops for CF workloads
uns64 gen_issue_width_lock_nops(std::map<uns64, ctype_pin_inst>& kernel_map, Sampler& uid_sequence, uns num_of_nops,
                                uns64 starting_pc) {
  Sampler nop_pcs(UNIFORM_SEQUENTIAL, ISSUE_WIDTH, starting_pc, NOP_SIZE, 1);
  for (uns i{0}; i < num_of_nops; i++) {
    auto current_pc{nop_pcs.get_next_element()};
    kernel_map.insert({current_pc, generate_nop(current_pc, uid_sequence.get_next_element(), NOP_SIZE, false)});
  }
  return nop_pcs.get_next_element();
}

/*  Microkernel Definitions */

// CBR
std::map<uns64, ctype_pin_inst> generate_cbr_kernel(Sequence_Pick_Strategy branch_direction_pick_strategy,
                                                    Sequence_Pick_Strategy branch_target_pick_strategy,
                                                    uns64 target_pool_size, double branch_t_nt_ratio,
                                                    uns64 workload_length, uns64 start_pc, uns64 start_uid) {
  Sampler uid_sequence(UNIFORM_SEQUENTIAL, ((2 * target_pool_size) + PAD_LENGTH), start_uid, 1, 1);

  // distribution for every possibe pc; onpath+offpath
  Sampler combined_targets_pool(branch_target_pick_strategy, ((2 * target_pool_size) + PAD_LENGTH), start_pc,
                                (ICACHE_LINE_SIZE), 1);

  // distribution for only taken targets
  const uint target_stride = (2 * ICACHE_LINE_SIZE);
  const uint starting_target = (start_pc + 2 * ICACHE_LINE_SIZE);
  Sampler targets_pool(branch_target_pick_strategy, target_pool_size, starting_target, target_stride, 1);

  // branch direction distribution for both onpath and offpath branches
  uns64 taken_ratio = static_cast<uns64>(branch_t_nt_ratio * 100);
  uns64 not_Taken_ratio = 100 - taken_ratio;
  Sampler direction_sequence({0, 1}, branch_direction_pick_strategy, 1, ((2 * target_pool_size) + PAD_LENGTH),
                             {taken_ratio, not_Taken_ratio});

  std::map<uns64, ctype_pin_inst> kernel_map;
  uns64 current_pc{start_pc}, _target{0};
  ctype_pin_inst next_inst;

  // Generate insts for every possible pc, includes possible offpath insts
  for (uns i{0}; i < (2 * target_pool_size + PAD_LENGTH); i++) {
    // generate leading nops and return next pc
    current_pc = gen_issue_width_lock_nops(kernel_map, uid_sequence, ISSUE_WIDTH - 1, current_pc);
    auto current_uid = uid_sequence.get_next_element();

    // every taken-target is 2 cachelines away.
    _target = combined_targets_pool.peek_element_following_next();
    next_inst = generate_conditional_branch(current_pc, current_uid, _target, direction_sequence.get_next_element(),
                                            BRANCH_SIZE);

    /* if the generated CBR's target goes out of range of target_pool, overwrite with unconditional branch to go back to
     beginning */
    if (_target > targets_pool.get_last_element())
      next_inst = generate_unconditional_branch(current_pc, current_uid, START_PC, BRANCH_SIZE);

    kernel_map.insert({current_pc, next_inst});
    // set up for next pc
    current_pc = combined_targets_pool.get_next_element();
  }

  return kernel_map;
}

// UBR
std::map<uns64, ctype_pin_inst> generate_ubr_kernel(Sequence_Pick_Strategy branch_target_pick_strategy,
                                                    uns64 target_pool_size, uns64 workload_length, uns64 start_pc,
                                                    uns64 start_uid, uns64 starting_target, uns64 target_stride) {
  assert(target_pool_size <= workload_length && "workload_length must be less than or equal to target_pool size ");

  Sampler uid_sequence(UNIFORM_SEQUENTIAL, (2 * target_pool_size + PAD_LENGTH), start_uid, 1, 1);

  // distribution for offpath+onpath targets
  Sampler combined_target_pool(branch_target_pick_strategy, (2 * target_pool_size + PAD_LENGTH), start_pc,
                               ICACHE_LINE_SIZE, 1);

  Sampler targets_pool(branch_target_pick_strategy, target_pool_size, starting_target, target_stride, 1);

  std::map<uns64, ctype_pin_inst> kernel_map;
  ctype_pin_inst next_inst;

  uns64 current_pc{start_pc}, current_uid{0};
  for (uns i{0}; i < ((2 * target_pool_size) + PAD_LENGTH); i++) {
    // generate leading nops
    current_pc = gen_issue_width_lock_nops(kernel_map, uid_sequence, ISSUE_WIDTH - 1, current_pc);
    current_uid = uid_sequence.get_next_element();

    // every taken target is 2 cachelines away
    uns64 next_target = combined_target_pool.peek_element_following_next();

    // if the generated target exceeds the taken targets distribution we set next target to beginning pc
    if (next_target > targets_pool.get_last_element())
      next_target = start_pc;

    next_inst = generate_unconditional_branch(current_pc, current_uid, next_target, BRANCH_SIZE);
    kernel_map.insert({current_pc, next_inst});
    // setup for next possibe pc
    current_pc = combined_target_pool.get_next_element();
  }
  return kernel_map;
}

// IBR
std::map<uns64, ctype_pin_inst> generate_ibr_kernel(Sequence_Pick_Strategy branch_target_pick_strategy,
                                                    uns64 target_pool_size, uns64 start_pc, uns64 start_uid,
                                                    uns64 target_stride, uns64 starting_target) {
  Sampler uid_sequence(UNIFORM_SEQUENTIAL, (2 * target_pool_size + PAD_LENGTH), start_uid, 1, 1);

  Sampler targets_pool(branch_target_pick_strategy, target_pool_size, starting_target, target_stride, 1);

  Sampler combined_target_pool(UNIFORM_SEQUENTIAL, (2 * target_pool_size + PAD_LENGTH), start_pc, ICACHE_LINE_SIZE, 1);
  // fixed mem address
  static std::uniform_int_distribution<uns64> uns64_dist{1, 0x00007fffffffffff};
  static uns64 memaddress = uns64_dist(Sampler::rng_engine);
  std::map<uns64, ctype_pin_inst> kernel_map;
  ctype_pin_inst next_inst;
  uns64 current_pc{start_pc}, current_uid{0};

  for (uns i{0}; i < (2 * target_pool_size) + PAD_LENGTH; i++) {
    // generate leading nops
    current_pc = gen_issue_width_lock_nops(kernel_map, uid_sequence, ISSUE_WIDTH - 1, current_pc);
    current_uid = uid_sequence.get_next_element();

    // the target of every branch is 2 cachelines away, for Round Robin
    uns64 next_target = combined_target_pool.peek_element_following_next();

    // for round robin ibr, if we exhaust our targets we go back to beginning
    if (next_target >= targets_pool.get_last_element())
      next_target = start_pc;

    // for random IBR the target is next element from the random distribution
    if (branch_target_pick_strategy == UNIFORM_RANDOM)
      next_target = targets_pool.get_next_element();

    next_inst = generate_indirect_branch(current_pc, current_uid, next_target, memaddress, BRANCH_SIZE);
    kernel_map.insert({current_pc, next_inst});
    // setup for next possibe pc
    current_pc = combined_target_pool.get_next_element();
  }

  return kernel_map;
}

// ILP
std::map<uns64, ctype_pin_inst> generate_ilp_kernel(uns dependence_chain_length, uns workload_length, uns64 start_pc,
                                                    uns64 start_uid) {
  /* Using 3x pad length because the dummy offpath insts after the workload have the same dependence chain structure
     as the on-path insts.
  */
  Sampler uid_sequence(UNIFORM_SEQUENTIAL, (workload_length + (3 * PAD_LENGTH)), start_uid, 1, 1);
  Sampler pc_sequence(UNIFORM_SEQUENTIAL, (workload_length + (3 * PAD_LENGTH)), start_pc, ALU_ADD_SIZE, 1);

  if (dependence_chain_length != 0)
    assert((workload_length % dependence_chain_length) == 0 &&
           "workload_length must be a multiple of dependence chain length");
  std::map<uns64, ctype_pin_inst> kernel_map;

  // zero dependence chain length means no carried loop dependence
  if (dependence_chain_length == 0) {
    for (uns i{0}; i < (workload_length + (3 * PAD_LENGTH)); i++) {
      auto current_pc{pc_sequence.get_next_element()};

      // if i=workload_length insert branch back to beginning. The following iterations are offpath pads for the branch
      if (i == workload_length) {
        kernel_map.insert({current_pc, generate_unconditional_branch(current_pc, uid_sequence.get_next_element(),
                                                                     START_PC, ALU_ADD_SIZE)});
        continue;
      }
      kernel_map.insert(
          {current_pc, generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), ALU_ADD_SIZE, 1, 2, 3)});

      // append unconditional branch to end of the kernel
    }
  } else {
    for (uns i{0}; i < ((workload_length / dependence_chain_length) + (3 * PAD_LENGTH)); i++) {
      for (uns j{0}; j < dependence_chain_length; j++) {
        auto current_pc{pc_sequence.get_next_element()};
        if ((i * dependence_chain_length) + j == workload_length) {
          kernel_map.insert({current_pc, generate_unconditional_branch(current_pc, uid_sequence.get_next_element(),
                                                                       START_PC, ALU_ADD_SIZE)});
          continue;
        }
        kernel_map.insert({current_pc, generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), ALU_ADD_SIZE,
                                                              j + 1, j + 1, j + 1)});
      }
    }
  }

  return kernel_map;
}

// LOAD
std::map<uns64, ctype_pin_inst> generate_load_kernel(Load_Kernel_Type type, uns workload_length,
                                                     Sequence_Pick_Strategy mem_address_pick_srategy,
                                                     uns64 start_mem_address, uns64 mem_addresses_stride,
                                                     Limit_Load_To level, uns64 start_pc, uns64 start_uid) {
  // stride should be enough to cause hits at a level but misses in the precceding levels if any
  uns64 stride = [&]() -> uns64 {
    switch (level) {
      case DCACHE_LEVEL: {
        return mem_addresses_stride;
      };
      case MLC_LEVEL: {
        return DCACHE_SIZE / (DCACHE_ASSOC);
      };
      case LLC_LEVEL: {
        return MLC_SIZE / (MLC_ASSOC);
      };
      case MEM_LEVEL: {
        return L1_SIZE / L1_ASSOC;
      }
      default:
        return mem_addresses_stride;
    }
  }();

  // The distribution size is the number of accesses that will fully replace a cache_line of a preceeding level
  // Beyond the dcache_limited workload, we cause conflict misses for each preceeding level, simplifies things
  uns64 distribution_size = [&]() -> uns64 {
    switch (level) {
      case DCACHE_LEVEL: {
        return workload_length;
      };
      case MLC_LEVEL: {
        return 2 * DCACHE_ASSOC;
      };

      case LLC_LEVEL: {
        return 2 * MLC_ASSOC;
      };

      case MEM_LEVEL: {
        return 2 * L1_ASSOC;
      };

      default:
        return workload_length;
    }
  }();

  Sampler uid_sequence(UNIFORM_SEQUENTIAL, (workload_length + PAD_LENGTH), start_uid, 1, 1);

  Sampler mem_address_sequence(mem_address_pick_srategy, distribution_size, start_mem_address, stride, 1);

  Sampler pc_sequence(UNIFORM_SEQUENTIAL, (workload_length + PAD_LENGTH), start_pc, LOAD_INST_SIZE, 1);

  std::map<uns64, ctype_pin_inst> kernel_map;

  for (uns i{0}; i < (workload_length + PAD_LENGTH); i++) {
    auto current_pc{pc_sequence.get_next_element()};

    if (i == workload_length) {
      // append unconditional branch to end of kernel
      kernel_map.insert({current_pc, generate_unconditional_branch(current_pc, uid_sequence.get_next_element(),
                                                                   START_PC, LOAD_INST_SIZE)});
      continue;
    }
    auto mem_addr = mem_address_sequence.get_next_element();
    switch (type) {
      // generate load
      case DEPENDENCE_CHAIN: {
        kernel_map.insert({current_pc, generate_generic_load(current_pc, uid_sequence.get_next_element(), mem_addr,
                                                             LOAD_INST_SIZE, Reg_Id::REG_RAX, Reg_Id::REG_RAX)});
        break;
      }

      case NO_DEPENDENCE_CHAIN: {
        kernel_map.insert({current_pc, generate_generic_load(current_pc, uid_sequence.get_next_element(), mem_addr,
                                                             LOAD_INST_SIZE, Reg_Id::REG_RAX, Reg_Id::REG_RBX)});
        break;
      }
      default:
        break;
    }
  }
  return kernel_map;
}

// ICACHE
std::map<uns64, ctype_pin_inst> generate_icache_kernel(uns64 start_pc, uns64 start_uid) {
  // generate 2*ICACHE_DEPTH worth of instructions, so entries are always replaced
  uns64 workload_length = 2 * (ICACHE_SIZE / ICACHE_LINE_SIZE);
  Sampler pc_sequence(UNIFORM_SEQUENTIAL, (workload_length + PAD_LENGTH), start_pc, ICACHE_LINE_SIZE, 1);

  Sampler uid_sequence(UNIFORM_SEQUENTIAL, (workload_length + PAD_LENGTH), start_uid, 1, 1);
  std::map<uns64, ctype_pin_inst> kernel_map;
  for (uns64 i{0}; i < workload_length + PAD_LENGTH; i++) {
    auto current_pc{pc_sequence.get_next_element()};
    if (i == workload_length) {
      kernel_map.insert({current_pc, generate_unconditional_branch(current_pc, uid_sequence.get_next_element(),
                                                                   START_PC, ICACHE_LINE_SIZE)});
      continue;
    }

    kernel_map.insert(
        {current_pc, generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), ICACHE_LINE_SIZE, 1, 2, 3)});
  }

  return kernel_map;
}