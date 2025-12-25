#include "bp/bp.param.h"
#include "memory/memory.param.h"

#include "frontend/synthetic/synthetic_kernels.h"
#include "isa/isa.h"

/* Helper Function For Bottleneck Microkernels */
// Function to generate leading nops for CF workloads
uns64 gen_issue_width_lock_nops(std::map<uns64, ctype_pin_inst>& kernel_map,
                                Distribution<uns64, Sequences::Strided_Sequence<uns64>>& uid_sequence, uns num_of_nops,
                                uns64 starting_pc) {
  Distribution<uns64, Sequences::Strided_Sequence<uns64>> nop_pcs(NORMAL_ROUND_ROBIN, ISSUE_WIDTH, NOP_SIZE,
                                                                  starting_pc);
  for (uns i{0}; i < num_of_nops; i++) {
    auto current_pc{nop_pcs.get_next_element()};
    kernel_map.insert({current_pc, make_nop(current_pc, uid_sequence.get_next_element(), NOP_SIZE, false)});
  }
  return nop_pcs.get_next_element();
}

/*  Microkernel Definitions */
std::map<uns64, ctype_pin_inst> generate_cbr_kernel(Sequence_Pick_Strategy branch_direction_pick_strategy,
                                                    Sequence_Pick_Strategy branch_target_pick_strategy,
                                                    uns64 direction_pool_size, uns64 target_pool_size,
                                                    double branch_t_nt_ratio, uns64 workload_length, uns64 start_pc,
                                                    uns64 start_uid) {
  Distribution<uns64, Sequences::Strided_Sequence<uns64>> uid_sequence(NORMAL_ROUND_ROBIN, 2 * target_pool_size, 1,
                                                                       start_uid);

  Distribution<uns64, Sequences::Strided_Sequence<uns64>> targets_pool(
      branch_target_pick_strategy, 2 * target_pool_size, (ICACHE_LINE_SIZE), start_pc);

  Distribution<bool, Sequences::Boolean_Sequence> direction_sequence(branch_direction_pick_strategy,
                                                                     direction_pool_size, branch_t_nt_ratio);

  std::map<uns64, ctype_pin_inst> kernel_map;
  uns64 current_pc{start_pc}, _target{0};
  ctype_pin_inst next_inst;
  auto targets_copy{targets_pool};
  uns workload_count{0};
  for (uns i{0}; i < 2 * target_pool_size; i++) {
    /* generate leading nops and return next pc */
    current_pc = gen_issue_width_lock_nops(kernel_map, uid_sequence, ISSUE_WIDTH - 1, targets_copy.get_next_element());
    auto current_uid = uid_sequence.get_next_element();

    // if the current pc is the last element in the distribution branch back to start pc (unconditional)
    if (current_pc >= targets_copy.get_last_element())
      next_inst = generate_unconditional_branch(current_pc, current_uid, START_PC, BRANCH_SIZE);
    else {
      // generate next CBR
      _target = current_pc + BRANCH_SIZE + ICACHE_LINE_SIZE;
      next_inst = generate_conditional_branch(current_pc, current_uid, _target, direction_sequence.get_next_element(),
                                              BRANCH_SIZE);

      // if the generated CBR's target goes out of range, unconditional branch to beginning
      if (_target >= targets_copy.get_last_element())
        next_inst = generate_unconditional_branch(current_pc, current_uid, START_PC, BRANCH_SIZE);
      workload_count++;
    }
    kernel_map.insert({current_pc, next_inst});
  }

  return kernel_map;
}

std::map<uns64, ctype_pin_inst> generate_ubr_kernel(Sequence_Pick_Strategy branch_target_pick_strategy,
                                                    uns64 target_pool_size, uns64 workload_length, uns64 start_pc,
                                                    uns64 start_uid, uns64 starting_target, uns64 target_stride) {
  assert(target_pool_size <= workload_length && "workload_length must be less than or equal to target_pool size ");
  Distribution<uns64, Sequences::Strided_Sequence<uns64>> uid_sequence(NORMAL_ROUND_ROBIN, target_pool_size, 1,
                                                                       start_uid);

  Distribution<uns64, Sequences::Strided_Sequence<uns64>> targets_pool(branch_target_pick_strategy, target_pool_size,
                                                                       target_stride, starting_target);

  std::map<uns64, ctype_pin_inst> kernel_map;
  ctype_pin_inst next_inst;
  uns64 current_pc{start_pc}, current_uid{0};
  for (uns i{0}; i < workload_length; i++) {
    // generate leading nops
    current_pc = gen_issue_width_lock_nops(kernel_map, uid_sequence, ISSUE_WIDTH - 1, current_pc);
    current_uid = uid_sequence.get_next_element();

    // generate forward UBR using next target in distribution
    next_inst = generate_unconditional_branch(current_pc, current_uid, targets_pool.get_next_element(), BRANCH_SIZE);
    kernel_map.insert({current_pc, next_inst});

    // get next pc after generated UBR and check if we have exhausted targets in the ditribution, if so next inst
    // branches to beginning
    current_pc = next_inst.instruction_next_addr;
    if (current_pc == targets_pool.get_last_element()) {
      current_pc = gen_issue_width_lock_nops(kernel_map, uid_sequence, ISSUE_WIDTH - 1, current_pc);
      current_uid = uid_sequence.get_next_element();
      next_inst = generate_unconditional_branch(current_pc, current_uid, START_PC, BRANCH_SIZE);
      kernel_map.insert({current_pc, next_inst});
      break;
    }
  }
  return kernel_map;
}

std::map<uns64, ctype_pin_inst> generate_ibr_kernel(Sequence_Pick_Strategy branch_target_pick_strategy,
                                                    uns64 target_pool_size, uns64 start_pc, uns64 start_uid,
                                                    uns64 target_stride, uns64 starting_target) {
  Distribution<uns64, Sequences::Strided_Sequence<uns64>> uid_sequence(NORMAL_ROUND_ROBIN,
                                                                       target_pool_size * ISSUE_WIDTH, 1, start_uid);

  static Distribution<uns64, Sequences::Strided_Sequence<uns64>> targets_pool(
      branch_target_pick_strategy, target_pool_size, target_stride, starting_target);

  static uns64 memaddress = Sequences::genRandomNum((uns64)1, (uns64)0x00007fffffffffff);
  std::map<uns64, ctype_pin_inst> kernel_map;
  ctype_pin_inst next_inst;
  uns64 current_pc{start_pc}, current_uid{0};

  // For Round Robin ibr kernel, we want to have multiple static instructions, For Randdom targets we want to have a
  // single static branch
  uns _loop_count = (branch_target_pick_strategy == NORMAL_ROUND_ROBIN) ? 1 : 0;
  uns i{0};
  do {
    /* generate leading nops */
    current_pc = gen_issue_width_lock_nops(kernel_map, uid_sequence, ISSUE_WIDTH - 1, current_pc);
    current_uid = uid_sequence.get_next_element();

    /* generate indirect branch that goes to next target in the ditribution */
    next_inst =
        generate_indirect_branch(current_pc, current_uid, targets_pool.get_next_element(), memaddress, BRANCH_SIZE);
    kernel_map.insert({current_pc, next_inst});
    current_pc = next_inst.instruction_next_addr;
    i++;
  } while (i < (_loop_count * target_pool_size));

  /* gen backward branch to beginning */
  current_pc = gen_issue_width_lock_nops(kernel_map, uid_sequence, ISSUE_WIDTH - 1, current_pc);
  next_inst = generate_unconditional_branch(current_pc, current_uid, START_PC, BRANCH_SIZE);
  kernel_map.insert({current_pc, next_inst});

  return kernel_map;
}

std::map<uns64, ctype_pin_inst> generate_ilp_kernel(uns dependence_chain_length, uns workload_length, uns64 start_pc,
                                                    uns64 start_uid) {
  Distribution<uns64, Sequences::Strided_Sequence<uns64>> uid_sequence(NORMAL_ROUND_ROBIN, workload_length + 1, 1,
                                                                       start_uid);

  Distribution<uns64, Sequences::Strided_Sequence<uns64>> pc_sequence(NORMAL_ROUND_ROBIN, workload_length + 1,
                                                                      ALU_ADD_SIZE, start_pc);
  if (dependence_chain_length != 0)
    assert((workload_length % dependence_chain_length) == 0 &&
           "workload_length must be a multiple of dependence chain length");
  std::map<uns64, ctype_pin_inst> kernel_map;

  // zero dependence chain length means no carried loop dependence
  if (dependence_chain_length == 0) {
    for (uns i{0}; i < workload_length; i++) {
      auto current_pc{pc_sequence.get_next_element()};
      kernel_map.insert(
          {current_pc, generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), ALU_ADD_SIZE, 1, 2, 3)});
    }
  } else {
    for (uns i{0}; i < workload_length / dependence_chain_length; i++) {
      for (uns j{0}; j < dependence_chain_length; j++) {
        auto current_pc{pc_sequence.get_next_element()};
        kernel_map.insert({current_pc, generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), ALU_ADD_SIZE,
                                                              j + 1, j + 1, j + 1)});
      }
    }
  }

  // append unconditional branch to end of the kernel
  auto current_pc{pc_sequence.get_next_element()};
  kernel_map.insert(
      {current_pc, generate_unconditional_branch(current_pc, uid_sequence.get_next_element(), START_PC, BRANCH_SIZE)});

  return kernel_map;
}

std::map<uns64, ctype_pin_inst> generate_load_kernel(Load_Kernel_Type type, uns workload_length,
                                                     Sequence_Pick_Strategy mem_address_pick_srategy,
                                                     uns64 start_mem_address, uns64 mem_addresses_stride,
                                                     Limit_Load_To level, uns64 start_pc, uns64 start_uid) {
  // stride should be enough to cause hits at a level but misses in the precceding level
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

  uns64 num_of_mem_addresses = [&]() -> uns64 {
    switch (level) {
      case DCACHE_LEVEL: {
        return workload_length;
      };

      // beyond the dcache level we cause conflict misses for the preceeding level, simplifies things
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

  Distribution<uns64, Sequences::Strided_Sequence<uns64>> uid_sequence(NORMAL_ROUND_ROBIN, workload_length + 1, 1,
                                                                       start_uid);

  Distribution<uns64, Sequences::Strided_Sequence<uns64>> mem_address_sequence(
      mem_address_pick_srategy, num_of_mem_addresses, stride, start_mem_address);

  Distribution<uns64, Sequences::Strided_Sequence<uns64>> pc_sequence(NORMAL_ROUND_ROBIN, workload_length + 1,
                                                                      LOAD_INST_SIZE, start_pc);

  std::map<uns64, ctype_pin_inst> kernel_map;

  for (uns i{0}; i < workload_length; i++) {
    auto current_pc{pc_sequence.get_next_element()};
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
  // append unconditional branch to end of kernel
  auto current_pc{pc_sequence.get_next_element()};
  kernel_map.insert(
      {current_pc, generate_unconditional_branch(current_pc, uid_sequence.get_next_element(), START_PC, BRANCH_SIZE)});
  return kernel_map;
}

std::map<uns64, ctype_pin_inst> generate_icache_kernel(uns64 start_pc, uns64 start_uid) {
  // generate 2x the size of the Icache, so entries are always replaced
  uns64 workload_length = 2 * (ICACHE_SIZE / ICACHE_LINE_SIZE);
  Distribution<uns64, Sequences::Strided_Sequence<uns64>> pc_sequence(NORMAL_ROUND_ROBIN, workload_length + 1,
                                                                      ICACHE_LINE_SIZE, start_pc);

  Distribution<uns64, Sequences::Strided_Sequence<uns64>> uid_sequence(NORMAL_ROUND_ROBIN, workload_length + 1, 1,
                                                                       start_uid);
  std::map<uns64, ctype_pin_inst> kernel_map;
  for (uns64 i{0}; i < workload_length; i++) {
    auto current_pc{pc_sequence.get_next_element()};
    kernel_map.insert(
        {current_pc, generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), ICACHE_LINE_SIZE, 1, 2, 3)});
  }
  auto current_pc{pc_sequence.get_next_element()};
  kernel_map.insert(
      {current_pc, generate_unconditional_branch(current_pc, uid_sequence.get_next_element(), START_PC, BRANCH_SIZE)});

  return kernel_map;
}