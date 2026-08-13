#include <cassert>
#include <iostream>

#include "globals/global_types.h"

#include "memory/memory.param.h"

#include "kernel_factory.h"
#include "sampler.h"
#include "synthetic_basic_apis.h"

// Flushes the kernel buffer into a map at the end of kernel generation. **
void flush_kernel_buffer(std::vector<ctype_pin_inst>& buffer, std::map<uns64, ctype_pin_inst>& kernel_map) {
  for (auto item : buffer) {
    auto pc = item.instruction_addr;
    kernel_map.insert({pc, item});
  }
}

// Generates leading nops for CF workloads.
uns64 Kernel_Factory::generate_nop_sequence(std::vector<ctype_pin_inst>& kernel_buffer, Sampler& uid_sequence,
                                            uns num_of_nops, uns64 starting_pc) {
  Sampler nop_pcs(UNIFORM_SEQUENTIAL, 1, num_of_nops, starting_pc, get_nop_size());
  for (uns i{0}; i < num_of_nops - 1; i++) {
    auto current_pc{nop_pcs.get_next_element()};
    kernel_buffer.push_back(
        ctype_pin_inst{generate_nop(current_pc, uid_sequence.get_next_element(), get_nop_size(), false)});
  }
  return nop_pcs.get_next_element();
}

// Generates tail end unconditional branch and NOP PADs at the end of each kernel
void Kernel_Factory::generate_tail_end_insts(std::vector<ctype_pin_inst>& kernel_buffer, Sampler& uid_sequence,
                                             Sampler& targets_pool, uns64 pad_to_next_inst) {
  uns64 current_pc = targets_pool.get_last_element_in_sorted_order() + pad_to_next_inst;
  current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, ISSUE_WIDTH, current_pc);
  auto next_inst =
      generate_unconditional_branch(current_pc, uid_sequence.get_next_element(), get_start_pc(), get_inst_size());
  kernel_buffer.push_back(next_inst);
  current_pc += get_inst_size();
  generate_nop_sequence(kernel_buffer, uid_sequence, pad_length, current_pc);
}

/* ===============================================================================================
 * CBR_LIMITED KERNEL (includes cbr_20t, cbr_50t, cbr_80t)
 * -----------------------------------------------------------------------------------------------
 * The kernel consists of a sequence of conditional branches with fixed targets but randomized branch
 directions.
 * The direction_sampler generates the randomized sequence according to the weights assigned each
 unique sample; {0, 1}. 0 = not-taken, 1 = taken. The weight is defined using the t_nt_ratio.
 With t_nt_ratio = 0.5, weight = {50, 50} for unique samples {0, 1}.
 * To ensure topdown, reflects the bottleneck as Bad Speculation. Each CBR is preceeded by a sequence of
 NOPs such that, the frontend can reliably deliver the full issue width every cycle. In addition,
 the sequience of NOPs+CBR in each issue packet is sized to fit the ICACHE_LINE_SIZE.
 * The kernel is essentially an unrolled loop, that ends with backward unconditional branch back to the
 beginning of the program. The branch direction sequence is rerandomized when control returns to the beginning.
 * The target_pool is generated randomly initially. But every iteration of the loop uses the same fixed
 targets according to the same sequence.
 *---------------------------------KERNEL VARIANTS-------------------------------------------------*
 * CBR_20t - t_nt_ratio is 0.2. weights = {20, 80}
 * CBR_50t - t_nt_ratio is 0.2. weights = {50, 50}
 * CBR_50t - t_nt_ratio is 0.2. weights = {80, 20}
 * ================================================================================================*/

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_cbr_kernel() {
  inst_size = ICACHE_LINE_SIZE - (nop_size * (ISSUE_WIDTH - 1));
  uid_sequence = Sampler(UNIFORM_SEQUENTIAL, 1, ((2 * ISSUE_WIDTH * target_pool_size) + pad_length + 1), start_uid, 1);

  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;
  // branch direction distribution for both onpath and offpath branches
  uns64 taken_ratio = static_cast<uns64>(t_nt_ratio * 100);
  uns64 not_Taken_ratio = 100 - taken_ratio;
  Sampler direction_sequence(direction_strategy, 1, {0, 1}, ((target_pool_size)), {taken_ratio, not_Taken_ratio});
  uns64 current_pc{0}, current_uid{0}, _target{0};
  ctype_pin_inst next_inst;

  // Generate insts for every possible pc, includes possible offpath insts
  for (uns i{0}; i < (target_pool_size); i++) {
    // generate leading nops and return next pc
    current_pc = target_pool.get_next_element();
    current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, ISSUE_WIDTH, current_pc);
    current_uid = uid_sequence.get_next_element();
    // every taken target is the next element in the shuffled sequence
    _target = target_pool.peek_next_element();
    next_inst = generate_conditional_branch(current_pc, current_uid, _target, direction_sequence.get_next_element(),
                                            get_inst_size());
    kernel_buffer.push_back(next_inst);
    // generate unconditional brnach to target on fall through
    current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, ISSUE_WIDTH, (current_pc + get_inst_size()));
    current_uid = uid_sequence.get_next_element();
    next_inst = generate_unconditional_branch(current_pc, current_uid, _target, get_inst_size());
    kernel_buffer.push_back(next_inst);
  }
  generate_tail_end_insts(kernel_buffer, uid_sequence, target_pool, ICACHE_LINE_SIZE);
  flush_kernel_buffer(kernel_buffer, kernel_map);
  return kernel_map;
}

/* ===============================================================================================
 * UBR_LIMITED KERNEL (includes cbr_20t, cbr_50t, cbr_80t)
 * -----------------------------------------------------------------------------------------------
 * The kernel consists of a sequence of unconditional branches with a set of targets.
 * The target pool is generated uniformly, shuffled and sampled for each UBR in the program.
 * Depending on the target pool pattern, the kernel ensures either 100% HIT or MISS or in between.
 * To ensure topdown, reflects the bottleneck as Bad Speculation. Each UBR is preceeded by a sequence of
 NOPs such that, the frontend can reliably deliver the full issue width every cycle. In addition,
 the sequience of NOPs+UBR in each issue packet is sized to fit the ICACHE_LINE_SIZE.
 * The kernel is a loop, that ends with backward unconditional branch to the beginning of the program.
 *---------------------------------KERNEL VARIANTS-------------------------------------------------*
 * btb_full_capacity_sweep - sweeps the full_capacity+1 to ensure 100% BTB MISS.
 * btb_assoc_sweep         - sweeps all the "way" entries of a chosen set to ensure 100% BTB MISS
 * btb_contained           - sweeps less than the capcity of the BTB to ensure 100% HIT
 * ================================================================================================*/

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_ubr_kernel() {
  inst_size = ICACHE_LINE_SIZE - (nop_size * (ISSUE_WIDTH - 1));
  starting_target = get_start_pc();
  uid_sequence = Sampler(UNIFORM_SEQUENTIAL, 1, ((2 * ISSUE_WIDTH * target_pool_size) + pad_length + 1), start_uid, 1);
  target_pool = Sampler(target_strategy, 1, target_pool_size, starting_target, target_stride);

  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;
  ctype_pin_inst next_inst;

  uns64 current_pc{start_pc}, current_uid{0};
  for (uns i{0}; i < (2 * target_pool_size); i++) {
    // generate leading nops
    current_pc = target_pool.get_next_element();
    current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, ISSUE_WIDTH, current_pc);
    current_uid = uid_sequence.get_next_element();

    // target is the next element in shuffled sequence
    uns64 next_target = target_pool.peek_next_element();

    next_inst = generate_unconditional_branch(current_pc, current_uid, next_target, get_inst_size());
    kernel_buffer.push_back(next_inst);
  }
  generate_tail_end_insts(kernel_buffer, uid_sequence, target_pool, ICACHE_LINE_SIZE);
  flush_kernel_buffer(kernel_buffer, kernel_map);
  return kernel_map;
}

/* ===========================================================================================================
 * IBR_LIMITED KERNEL (includes IBR_RR_4TGT, IBR_RANDOM_2TGT, IBR_RANDOM_4TGT)
 * ------------------------------------------------------------------------------------------------------------
 * The kernel consists of a sequence of indirect branches that branch to a set of indirect targets.
 * The targets are obtained from a fixed memory address sourced indirectly from a fixed source register operand.
 * The target pool is generated uniformly, shuffled and sampled for each IBR in the program.
 * Depending on the target pool pattern, the kernel ensures either 100% HIT or MISS or in between.
 * To ensure topdown, reflects the bottleneck as Bad Speculation. Each IBR is preceeded by a sequence of
 NOPs such that, the frontend can reliably deliver the full issue width every cycle. In addition,
 the sequience of NOPs+IBR in each issue packet is sized to fit the ICACHE_LINE_SIZE.
 * The kernel is a loop, that ends with backward unconditional branch to the beginning of the program.
 *---------------------------------KERNEL VARIANTS-------------------------------------------------*
 * IBR_RR_4TGT             - sweeps four targets in a round robin fashion. 100% IBR_BTB_HIT.
 * IBR_RANDOM_4TGT         - sweeps four targets in a random fashion. 25% IBR_BTB_HIT.
 * IBR_RANDOM_2TGT         - sweeps two  targets in a round robin fashion. 50% IBR_BTB_HIT.
 *--------------------
 * The RANDOM variants are randomized whenever control returns to the beginning of the program
 * ===========================================================================================================*/
std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_ibr_kernel() {
  inst_size = ICACHE_LINE_SIZE - (nop_size * (ISSUE_WIDTH - 1));
  starting_target = get_start_pc() + ICACHE_LINE_SIZE;
  uid_sequence = Sampler(UNIFORM_SEQUENTIAL, 1, ((2 * ISSUE_WIDTH * target_pool_size) + pad_length + 1), start_uid, 1);

  uns last_target = starting_target + (target_pool_size - 1) * target_stride;
  uns pool_size = ((last_target - start_pc) / ICACHE_LINE_SIZE) + 1;
  Sampler combined_target_pool(UNIFORM_SEQUENTIAL, 1, pool_size, start_pc, ICACHE_LINE_SIZE);
  target_pool = Sampler(target_strategy, 1, target_pool_size, starting_target, target_stride);

  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;
  // fixed mem address
  uns64 memaddress = 0x123456;
  ctype_pin_inst next_inst;
  uns64 current_pc = start_pc, current_uid = 0;
  for (uns i{0}; i < pool_size; i++) {
    // generate leading nops
    current_pc = combined_target_pool.get_next_element();
    current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, ISSUE_WIDTH, current_pc);
    current_uid = uid_sequence.get_next_element();
    // target is the next element in shuffled sequence
    uns64 next_target = target_pool.get_next_element();
    next_inst = generate_indirect_branch(current_pc, current_uid, next_target, memaddress, get_inst_size());
    kernel_buffer.push_back(next_inst);
  }
  generate_tail_end_insts(kernel_buffer, uid_sequence, target_pool, ICACHE_LINE_SIZE);
  flush_kernel_buffer(kernel_buffer, kernel_map);
  return kernel_map;
}

/* ===========================================================================================================
 * ILP_LIMITED KERNEL (includes ILP_DEP_0_CHAIN, ILP_DEP_1_CHAIN, ILP_DEP_2_CHAIN, ILP_DEP_4_CHAIN,)
 * ------------------------------------------------------------------------------------------------------------
 * The kernel consists of a sequence of alu_type instructions that are orchestrated to generate ilp bottlenecks.
 * The sequence of instructions create a dependence chain through source and destination operands.
 * The number of dependence chains is configurable. With 1 dependence chain, every succeeding instruction is
 orchestrated to stall before execute so that the preceeding one completes.
 * Configuring > 1 dependence chains, generates a set of mutually exclusive dependence chain. Thus, an instruction
 from each dependence chain can be in execute concurrently.
 * The kernel is an unrolled loop, that ends with backward unconditional branch to the beginning of the program.
 *---------------------------------KERNEL VARIANTS-------------------------------------------------*
 * ILP_DEP_0_CHAIN         - All instructions are independent of each other.
 * ILP_DEP_1_CHAIN         - 1 dependence chain.  Expected IPC = 1, TopDown:Retiring = 1/ISSUEWIDTH.
 * ILP_DEP_2_CHAIN         - 2 dependence chains. Expected IPC = 2, TopDown:Retiring = 2/ISSUEWIDTH.
 * ILP_DEP_4_CHAIN         - 4 dependence chains. Expected IPC = 2, TopDown:Retiring = 4/ISSUEWIDTH.
 * ===========================================================================================================*/
std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_ilp_kernel(uns dependence_chain_length, uns workload_length) {
  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;
  if (dependence_chain_length != 0)
    assert((workload_length % dependence_chain_length) == 0 &&
           "workload_length must be a multiple of dependence chain length");
  uns64 current_pc = 0;
  // zero dependence chain length means no carried loop dependence
  if (dependence_chain_length == 0) {
    for (uns i{0}; i < (workload_length); i++) {
      current_pc = pc_sequence.get_next_element();
      kernel_buffer.push_back(
          generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), get_inst_size(), 1, 2, 3));
      // append unconditional branch to end of the kernel
    }
  } else {
    for (uns i{0}; i < ((workload_length / dependence_chain_length)); i++) {
      for (uns j{0}; j < dependence_chain_length; j++) {
        current_pc = pc_sequence.get_next_element();
        kernel_buffer.push_back(
            generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), get_inst_size(), j + 1, j + 1, j + 1));
      }
    }
  }
  generate_tail_end_insts(kernel_buffer, uid_sequence, pc_sequence, get_inst_size());
  flush_kernel_buffer(kernel_buffer, kernel_map);
  return kernel_map;
}

/* ===========================================================================================================
 * MEMORY KERNELS (LATENCY, BANDWIDTH)
     * LATENCY_KERNELS (DCACHE_LIMITED, MLC_LIMITED, LLC_LIMITED, MAIN_MEM_LIMITED)
     * BANDWIDTH_KERNELS (MEM_BAND_1FU, MEM_BAND_2FU, MEM_BAND_4FU,)
 * ------------------------------------------------------------------------------------------------------------
 * The memory kernels orchestrate memory latency and bandwidth bottlenecks.

 * The latency workloads are intended to limit IPC through the latency of the various levels in the cache hierachy.
 * To enable latency bottlenecks, an unrolled loop-carried dependence chain including load instructions is utilised.
 * Each subsequent Load accesses a memory address that is gauranteed to hit at a particular level but miss in all
 preceeding levels, if any. The memory address pattern is defined in a target_pool and generated by the sampler.
 * The level is configurable as any level in the memory hierachy.

 * The bandwidth kernels orchestrate memory bandwidth bottlenecks.
 * Here the instructions stream consists of a sequence of independent loads that can execute concurrently.However,
 the actual number of loads that can actually execute is limited by the number of available load FUs
 which is configurable in PARAMS.in

 *------------------------------------------Latency-Limited KERNEL VARIANTS----------------------------------*
 * Loop Carried Dependence Chain
 * DCACHE_LIMITED          - All Load accesses hit in Dcache..
 * MLC_LIMITED             - All Load accesses hit in MLC, but miss in DCACHE.
 * LLC_LIMITED             - All Load accesses hit in LLC, but miss in DCACHE and MLC.
 * MAIN_MEMORY_LIMITED     - All Load accesses hit in MAIN_MEMORY but miss in DCACHE, LLC and MLC.


*-------------------------------------Bandwidth-Limited KERNEL VARIANTS----------------------------------------*
 * No Loop Carried Dependence Chain, all loads are independent.
 * MEM_BANDWIDTH_1FU             - Single load FU. IPC approx 1. Retiring = 1 / ISSUE_WIDTH
 * MEM_BANDWIDTH_2FU             - two load FUs. IPC approx 2. Retiring = 2 / ISSUE_WIDTH.
 * MEM_BANDWIDTH_4FU             - four Load FUs, IPC approx 4. Retiring = 4 / ISSUE_WIDTH..
 * ===========================================================================================================*/
std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_load_kernel(Load_Kernel_Type type) {
  // stride should be enough to cause hits at a level but misses in the precceding levels if any
  starting_target = 2 * ICACHE_LINE_SIZE;
  target_stride = [&]() -> uns64 {
    switch (level) {
      case DCACHE_LEVEL: {
        return 0;
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
        return 0;
    }
  }();

  /*
  The target_pool consists of a set of load addresses that map to the same set and are enough to cause 100%
  conflict misses in all preceeding levels.
  */
  target_pool_size = [&]() -> uns64 {
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

  target_pool = Sampler(target_strategy, 1, target_pool_size, starting_target, target_stride);

  std::vector<ctype_pin_inst> kernel_buffer;
  std::map<uns64, ctype_pin_inst> kernel_map;
  for (uns i{0}; i < (workload_length); i++) {
    auto current_pc = pc_sequence.get_next_element();
    auto mem_addr = target_pool.get_next_element();
    switch (type) {
      case DEPENDENCE_CHAIN: {
        kernel_buffer.push_back(generate_generic_load(current_pc, uid_sequence.get_next_element(), mem_addr,
                                                      get_inst_size(), Reg_Id::REG_RAX, Reg_Id::REG_RAX));
        break;
      }
      case NO_DEPENDENCE_CHAIN: {
        kernel_buffer.push_back(generate_generic_load(current_pc, uid_sequence.get_next_element(), mem_addr,
                                                      get_inst_size(), Reg_Id::REG_RAX, Reg_Id::REG_RBX));
        break;
      }
      default:
        break;
    }
  }
  generate_tail_end_insts(kernel_buffer, uid_sequence, pc_sequence, get_inst_size());
  flush_kernel_buffer(kernel_buffer, kernel_map);
  return kernel_map;
}

/* ===========================================================================================================
 * ICACHE_LIMITED
 * ------------------------------------------------------------------------------------------------------------
 * The kernel stresses the capacity of the ICACHE. By ensuring the loop to be run exceeds ICACHE_SIZE, the return
 to the start of the program is gauranteed 100% ICACHE_MISS.
 * The size for every instruction = ICACHE_LINE_SIZE, moreover, the workload required FDIP/frontend prefetcher be
 turned off.
 * ===========================================================================================================*/
std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_icache_kernel() {
  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;

  for (uns64 i{0}; i < workload_length; i++) {
    auto current_pc = pc_sequence.get_next_element();

    kernel_buffer.push_back(
        generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), ICACHE_LINE_SIZE, 1, 2, 3));
  }
  generate_tail_end_insts(kernel_buffer, uid_sequence, pc_sequence, ICACHE_LINE_SIZE);
  flush_kernel_buffer(kernel_buffer, kernel_map);
  return kernel_map;
}