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
#include <iostream>

#include "globals/global_types.h"

#include "memory/memory.param.h"

#include "kernel_factory.h"
#include "sampler.h"
#include "synthetic_basic_apis.h"

#define DEBUG(args...) _SDEBUG(DEBUG_SYNTHETIC_KERNEL_GENERATOR, ##args)

// Flushes the kernel buffer into a map at the end of kernel generation. **
void flush_kernel_buffer(std::vector<ctype_pin_inst>& buffer, std::map<uns64, ctype_pin_inst>& kernel_map) {
  for (auto item : buffer) {
    auto pc = item.st.instruction_addr;
    kernel_map.insert({pc, item});
  }
}

// Generates leading nops for CF workloads.
uns64 Kernel_Factory::generate_nop_sequence(std::vector<ctype_pin_inst>& kernel_buffer, Sampler& uid_sequence,
                                            uns num_of_nops, uns64 starting_pc) {
  Sampler nop_pcs(UNIFORM_SEQUENTIAL, 1, num_of_nops + 1, starting_pc, get_nop_size());
  for (uns i{0}; i < num_of_nops; i++) {
    auto current_pc{nop_pcs.get_next_element()};
    kernel_buffer.push_back(
        ctype_pin_inst{generate_nop(current_pc, uid_sequence.get_next_element(), get_nop_size(), false)});
  }
  return nop_pcs.get_next_element();
}

// Generates tail end unconditional branch and NOP PADs at the end of each kernel
void Kernel_Factory::generate_tail_end_insts(std::vector<ctype_pin_inst>& kernel_buffer, Sampler& uid_sequence,
                                             Sampler& targets_pool, uns pad_to_next_inst,
                                             uns unconditional_branch_size) {
  uns64 current_pc = targets_pool.get_last_element_in_sorted_order() + pad_to_next_inst;
  current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, DISPATCH_WIDTH, current_pc);
  auto next_inst = generate_unconditional_branch(current_pc, uid_sequence.get_next_element(), get_start_pc(),
                                                 unconditional_branch_size);
  kernel_buffer.push_back(next_inst);
  current_pc += unconditional_branch_size;
  generate_nop_sequence(kernel_buffer, uid_sequence, pad_length, current_pc);
}

/************  PreChecks For CF_Workloads ******************
These prechecks warns the user beforehand if
1. Issue packets will cross ICACHE_BOUNDARIES
2. The chosen size for NOPs is sufficient to fill out the remaining cache line given the size of the issue packet
3. If the configuration tuple; target_stride, nop_size and cf_instr_size (CF_BR, CF_CBR and CF_IBR0) are appopriate for
correct operation

Parameter Definitions
1. nop_count_in_packet: How many NOPS are in an issue_packet
2. cf_op_count_in_packet: How many cf ops are in an issue packet. CBR worklads include NOPS+CBR+BR, IBR and UBR consist
of NOPS+IBR, NOPS+UBR respectively
3. target_stride: This indicates the distance between taken targets. If the starting pc is a multiple of the
icache_line_size, and the target_stride is a factor of icache_line_size, then every issuepacket starts at an
ICACHE_BOUNDARY
4. cf_inst_size: Size of control flow instructions; CBR, UBR or IBR according to the chosen kernel
5. nop_inst_size: Size of a NOP
***************************************************************/

void prechecks_for_CF_kernels(uns nop_count_in_packet, uns cf_op_count_in_packet, uns target_stride, uns cf_inst_size,
                              uns nop_inst_size) {
  // total size of packet
  uns cf_kernel_issue_packet_size = (nop_count_in_packet * nop_inst_size) + (cf_op_count_in_packet * cf_inst_size);

  // if packet is greater than branch target distance, packets will overlap into each other
  assert((cf_kernel_issue_packet_size <= target_stride) &&
         "ERROR: Invalid tuple; target_stride, nop_size and cf_inst size, packets overwrite each other");

  if (cf_kernel_issue_packet_size > ICACHE_LINE_SIZE)
    std::cerr << "WARNING: CF Packets cross ICACHE_BOUNDARIES, Frontend Bound Atrribtion in TMA possible";
  else if (cf_kernel_issue_packet_size < ICACHE_LINE_SIZE)
    // The issue packet is not fully utilizing icahce lines but we need to make sure NOP size is appopriate to fill out
    // this gap so offpath execution will work properly
    assert((ICACHE_LINE_SIZE - cf_kernel_issue_packet_size) % nop_inst_size == 0 &&
           "Issue packets for this kernel < ICACHE_INE_SIZE but NOP size is inappopriate to fill out the leftover");
};

void Kernel_Factory::fill_remaining_cache_line(std::vector<ctype_pin_inst>& kernel_buffer, uns packet_size,
                                               uns current_pc) {
  int empty_distance = ICACHE_LINE_SIZE - packet_size;

  if ((target_stride == ICACHE_LINE_SIZE) && (empty_distance > 0)) {
    // we can fill spaces
    generate_nop_sequence(kernel_buffer, uid_sequence, empty_distance, current_pc);
  }
}

/* ####################################################################################
// [ Control Flow (CF) Kernels ]
// ....................................................................................
// Control flow kernels are designed to trigger Top-Down "Bad Speculation"
// bottlenecks.  Rather than strictly sizing packets to fill an entire cache line,
// issue packets are simply bounded by the ICACHE_LINE_SIZE to prevent
// boundary-crossing stalls, with any leftover dead space cleanly padded with NOPs.

//   CBR_LIMITED (cbr_limited)
//   - Generates conditional branches with fixed targets but randomized
//     directions.
//   - Directions (taken/not-taken) are weighted according to t_nt_ratio.
//   - The Kernel_Dispatcher rerandomizes the branch direction sequence
//     each time control returns to the beginning of the kernel.
//
// ********************* CBR  Considerations ***********************************
//   The cbr_limited kernel poses a unique structural risk: conditional branches
//   have both a Taken path and a Not-Taken (fall-through) path. Because the
//   branch direction is randomized to induce mispredictions, a branch that
//   correctly evaluates as Not-Taken would naturally fall through to the next
//   sequential instruction.
//
//   Without strict routing, a random fall-through could accidentally jump to a
//   previously visited target, creating a closed loop. If the program gets
//   trapped in a closed loop, it will iterate indefinitely among a small subset
//   of addresses and never traverse the full target pool or reach the end of
//   the workload.
//
//   To guarantee the master circular path is strictly traversed regardless of
//   branch direction, the cbr_limited kernel employs an Unconditional Branch
//   (UBR) safety net. Below is a sequence schema demonstrating this dual-entry routing.
//
//     // Packet A (Target N)
//        [64]   NOP  1
//        [65]   NOP  1
//        [66]   NOP  1
//        [67]   NOP  1
//        [68]   NOP  1
//        [69]   CBR  1 -> TAKEN: Tgt1
//        [70]   UBR  1 -> Tgt1
//        ... (nop dead space to end of cache line)
//
//     // Packet B (Target N+1)
//        [127]  Tgt1:     NOP 1  <-- Entry (if Packet A CBR is Taken or through UBR)
//        [128]            NOP 1
//        [129]            NOP 1
//        [130]            NOP 1
//        [131]            NOP 1
//        [132]            CBR 1 -> TAKEN: Tgt2
//        [133]            UBR 1 -> Tgt2
//        ... (nop dead space to end of cache line)
//
// * Taken Path: The CBR branches directly to the primary entry point of the
//   next randomized target block (e.g., Tgt1 at [128]).
// * Not-Taken Path: Execution falls through directly into the UBR, which
//   unconditionally routes control flow to a secondary entry point within
//   the exact same target block (e.g., Tgt1 + 1 at [129]).
//
//   Because the btb_limited (UBR) and ibr_limited kernels utilize absolute
//   jumps with no sequential fall-through paths, they are natively immune to
//   this closed-loop risk and do not require a safety net.
//
//   [ Dispatch Consistency & The NOP Balancing Act ]
//........................................................................
//    To ensure that Top-Down metric estimations perfectly match the simulation
//    results, the execution stream must process the exact same number of
//    micro-ops between branch evaluations. Fluctuating retire counts based on
//    branch direction would introduce structural noise.
//
//    The CBR kernel must offset the operational cost of the UBR safety net
//    triggered on the fall-through path. It does this by front-loading
//    DISPATCH_WIDTH NOPs + 1 CBR + 1 UBR and manipulating the jump targets to
//    balance the final retired operation count:
//     * When CBR is Taken: The branch jumps to the first NOP of the next target
//       block (Tgt1). The pipeline executes exactly DISPATCH_WIDTH NOPs + 1 CBR.
//     * When CBR is Not-Taken (Fall-through): The branch falls through to the UBR
//       (costing 1 operation). To offset this, the UBR is hardcoded to jump to
//       Tgt1 + 1, intentionally skipping the first NOP of the destination block.
//       The pipeline executes 1 UBR + (DISPATCH_WIDTH - 1) NOPs + 1 CBR.
//
//    Packet A (Target N)
//    [64]   NOP  1
//    [65]   NOP  1
//    [66]   NOP  1
//    [67]   NOP  1
//    [68]   NOP  1
//    [69]   NOP  1
//    [70]   CBR  1 -> TAKEN: Tgt1
//    [71]   UBR  1 -> Tgt1 + 1
//    ... (nop dead space to end of cache line)
//
//    // Packet B (Target N+1)
//    [128]  Tgt1:     NOP 1  <-- Primary Entry (if Packet A CBR is Taken)
//    [129]  Tgt1 + 1: NOP 1  <-- Secondary Entry (if Packet A CBR is Not-Taken)
//    [130]            NOP 1
//    [131]            NOP 1
//    [132]            NOP 1
//    [133]            NOP 1
//    [134]            CBR 1 -> TAKEN: Tgt2
//    [135]            UBR 1 -> Tgt2 + 1
//    ... (nop dead space to end of cache line)
//
//    In both scenarios, the pipeline executes the exact same number of operations.
//    While the very first issue packet of the program may execute one micro-op
//    more than the steady state, perfect dispatch symmetry is achieved for all
//    subsequent iterations.
//
//    Unconditional Kernels (btb_limited, ibr_limited):
//    Because there is no fall-through path to manage, the math is straightforward.
//    * The issue packet is constructed with exactly (DISPATCH_WIDTH - 1) NOPs
//      + 1 UBR/IBR.
//    * The pipeline flawlessly ingests and executes exactly one DISPATCH_WIDTH
//      worth of instructions per target.
######################################################################################*/

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_cbr_kernel() {
  uid_sequence =
      Sampler(UNIFORM_SEQUENTIAL, 1, ((2 * DISPATCH_WIDTH * target_pool_size) + pad_length + 1), start_uid, 1);

  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;
  // branch direction distribution for both onpath and offpath branches
  uns64 taken_ratio = static_cast<uns64>(t_nt_ratio * 100);
  uns64 not_Taken_ratio = 100 - taken_ratio;
  Sampler direction_sequence(direction_strategy, 1, {0, 1}, ((target_pool_size)), {taken_ratio, not_Taken_ratio});
  uns64 current_pc{0}, current_uid{0}, _target{0};
  ctype_pin_inst next_inst;
  uns packet_size = (DISPATCH_WIDTH * get_nop_size()) + (2 * get_inst_size());
  prechecks_for_CF_kernels(DISPATCH_WIDTH, 2, target_stride, get_inst_size(), get_nop_size());

  // Generate insts for every possible pc, includes possible offpath insts
  for (uns i{0}; i < (target_pool_size); i++) {
    // generate leading nops and return next pc
    current_pc = target_pool.get_next_element();
    current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, DISPATCH_WIDTH, current_pc);
    current_uid = uid_sequence.get_next_element();
    // every taken target is the next element in the shuffled sequence
    _target = target_pool.peek_next_element();
    next_inst = generate_conditional_branch(current_pc, current_uid, _target, direction_sequence.get_next_element(),
                                            get_inst_size());
    kernel_buffer.push_back(next_inst);

    // generate unconditional branch to cbr_target on fall through
    current_uid = uid_sequence.get_next_element();
    current_pc += get_inst_size();
    next_inst = generate_unconditional_branch(current_pc, current_uid, _target + get_nop_size(), get_inst_size());
    kernel_buffer.push_back(next_inst);

    current_pc += get_inst_size();
    // if the entire line was not used, fill with NOPs.
    fill_remaining_cache_line(kernel_buffer, packet_size, current_pc);
  }

  generate_tail_end_insts(kernel_buffer, uid_sequence, target_pool, packet_size, get_nop_size());
  flush_kernel_buffer(kernel_buffer, kernel_map);
  // at this stage inst_count[proc_id] and op_count[proc_id] are invalid hence cant use DEBUG macro
  if (DEBUG_SYNTHETIC_KERNEL_GENERATOR) {
    for (auto item : kernel_map) {
      printf("\nop_type:%d ip:%llu next_addr:%llu size:%d branch_target:%llu branch_dir:%d inst_uid:%llu",
             item.second.st.op_type, (unsigned long long)item.second.st.instruction_addr,
             (unsigned long long)item.second.instruction_next_addr, (uns32)item.second.st.size,
             (unsigned long long)item.second.st.branch_target, (uns32)item.second.actually_taken,
             (unsigned long long)item.second.inst_uid);
    }
  }
  return kernel_map;
}

//------------------------------------------------------------------------//
// > UBR_LIMITED / BTB_LIMITED (btb_limited)
//------------------------------------------------------------------------//
//   - Generates a sequence of unconditional branches using a set of
//     uniformly generated, shuffled targets.
//   - Depending on the configured target pool pattern, this kernel can
//     guarantee either a 100% BTB hit rate, a 100% miss rate, or a
//     specific ratio in between.
std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_ubr_kernel() {
  uid_sequence = Sampler(UNIFORM_SEQUENTIAL, 1, (target_pool_size + pad_length + 1), start_uid, 1);
  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;
  ctype_pin_inst next_inst;
  prechecks_for_CF_kernels(DISPATCH_WIDTH - 1, 1, target_stride, get_inst_size(), get_nop_size());
  uns packet_size = ((DISPATCH_WIDTH - 1) * NOP_SIZE) + (get_inst_size());
  uns64 current_pc = 0, current_uid = 0;
  for (uns i{0}; i < (target_pool_size); i++) {
    // generate leading nops
    current_pc = target_pool.get_next_element();
    current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, DISPATCH_WIDTH - 1, current_pc);
    current_uid = uid_sequence.get_next_element();

    // target is the next element in shuffled sequence
    uns64 next_target = target_pool.peek_next_element();

    next_inst = generate_unconditional_branch(current_pc, current_uid, next_target, get_inst_size());
    kernel_buffer.push_back(next_inst);

    current_pc += get_inst_size();
    // if the entire line was not used, fill with NOPs.
    fill_remaining_cache_line(kernel_buffer, packet_size, current_pc);
  }

  generate_tail_end_insts(kernel_buffer, uid_sequence, target_pool, packet_size, get_inst_size());

  flush_kernel_buffer(kernel_buffer, kernel_map);
  if (DEBUG_SYNTHETIC_KERNEL_GENERATOR) {
    for (auto item : kernel_map) {
      printf("\nop_type:%d ip:%llu next_addr:%llu size:%d branch_target:%llu branch_dir:%d inst_uid:%llu",
             item.second.st.op_type, (unsigned long long)item.second.st.instruction_addr,
             (unsigned long long)item.second.instruction_next_addr, (uns32)item.second.st.size,
             (unsigned long long)item.second.st.branch_target, (uns32)item.second.actually_taken,
             (unsigned long long)item.second.inst_uid);
    }
  }
  return kernel_map;
}

//--------------------------------------------------------------------------//
// > IBR_LIMITED (ibr_limited)
//------------------------------------------------------------------------//
//   - Produces indirect branches branching to targets obtained indirectly
//     from a fixed memory address via a source register.
//   - If a randomized target pool is used, the Kernel_Dispatcher
//     rerandomizes the targets when execution loops back to the start.
// The following API is called for the random case

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_random_ibr_kernel() {
  assert(starting_target <= get_start_pc() && "Random IBR Targets must not include starting_pc");
  uid_sequence =
      Sampler(UNIFORM_SEQUENTIAL, 1, ((2 * DISPATCH_WIDTH * target_pool_size + pad_length) + 1), start_uid, 1);

  prechecks_for_CF_kernels(DISPATCH_WIDTH - 1, 1, target_stride, get_inst_size(), get_nop_size());

  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;
  // fixed mem address
  uns64 memaddress = 0x123456;
  ctype_pin_inst next_inst;
  uns64 current_pc = start_pc, current_uid = 0;
  // generate leading nops
  current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, DISPATCH_WIDTH - 1, current_pc);
  current_uid = uid_sequence.get_next_element();
  // target is the next element in shuffled sequence
  uns64 next_target = target_pool.get_next_element();
  next_inst = generate_indirect_branch(current_pc, current_uid, next_target, memaddress, get_inst_size());
  kernel_buffer.push_back(next_inst);
  // generate tail-end instructions
  current_pc += get_inst_size();
  for (uns i = 0; i < pad_length; i++) {
    current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, DISPATCH_WIDTH - 1, current_pc);
    current_uid = uid_sequence.get_next_element();
    next_inst = generate_unconditional_branch(current_pc, current_uid, start_pc, get_inst_size());
    kernel_buffer.push_back(next_inst);
    current_pc += get_inst_size();
  }
  flush_kernel_buffer(kernel_buffer, kernel_map);
  if (DEBUG_SYNTHETIC_KERNEL_GENERATOR)
    for (auto item : kernel_map) {
      printf("\nop_type:%d ip:%llu next_addr:%llu size:%d branch_target:%llu branch_dir:%d inst_uid:%llu",
             item.second.st.op_type, (unsigned long long)item.second.st.instruction_addr,
             (unsigned long long)item.second.instruction_next_addr, (uns32)item.second.st.size,
             (unsigned long long)item.second.st.branch_target, (uns32)item.second.actually_taken,
             (unsigned long long)item.second.inst_uid);
    }
  return kernel_map;
}
//------------------------------------------------------------------------//
// > IBR_LIMITED (ibr_limited)
//------------------------------------------------------------------------//
//   - Produces indirect branches branching to targets obtained indirectly
//     from a fixed memory address via a source register.
//   - If a randomized target pool is used, the Kernel_Dispatcher
//     rerandomizes the targets when execution loops back to the start.
// The following API is called for the round-robin case

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_round_robin_ibr_kernel() {
  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;
  // fixed mem address
  uns64 memaddress = 0x123456;
  ctype_pin_inst next_inst;
  uns packet_size = ((DISPATCH_WIDTH - 1) * NOP_SIZE) + (get_inst_size());
  uns64 current_uid = 0, current_pc = 0;
  for (uns i{0}; i < target_pool_size; i++) {
    // generate leading nops
    current_pc = target_pool.get_next_element();
    current_pc = generate_nop_sequence(kernel_buffer, uid_sequence, DISPATCH_WIDTH - 1, current_pc);

    current_uid = uid_sequence.get_next_element();
    // target is the next element in shuffled sequence
    uns64 next_target = target_pool.peek_next_element();
    next_inst = generate_indirect_branch(current_pc, current_uid, next_target, memaddress, get_inst_size());
    kernel_buffer.push_back(next_inst);

    // if the entire line was not used, fill with NOPs.
    fill_remaining_cache_line(kernel_buffer, packet_size, current_pc);
  }
  generate_tail_end_insts(kernel_buffer, uid_sequence, target_pool, packet_size, get_inst_size());
  flush_kernel_buffer(kernel_buffer, kernel_map);
  if (DEBUG_SYNTHETIC_KERNEL_GENERATOR)
    for (auto item : kernel_map) {
      printf("\nop_type:%d ip:%llu next_addr:%llu size:%d branch_target:%llu branch_dir:%d inst_uid:%llu",
             item.second.st.op_type, (unsigned long long)item.second.st.instruction_addr,
             (unsigned long long)item.second.instruction_next_addr, (uns32)item.second.st.size,
             (unsigned long long)item.second.st.branch_target, (uns32)item.second.actually_taken,
             (unsigned long long)item.second.inst_uid);
    }
  return kernel_map;
}

/********************************************************************************/
// [ Instruction-Level Parallelism (ILP) Kernels ]
// .............................................................................//
// > ILP_LIMITED (ilp_limited)
//   - Constructs sequences of ALU instructions mapped to source and
//     destination registers to create strict dependence chains.
//   - If dependence_chain_count is 1, each instruction stalls until the
//     previous one completes.
//   - If dependence_chain_count > 1, the factory generates mutually
//     exclusive chains, allowing parallel execution up to the configured
//     chain limit.
// ############################################################################## //

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
  generate_tail_end_insts(kernel_buffer, uid_sequence, pc_sequence, get_inst_size(), get_inst_size());
  flush_kernel_buffer(kernel_buffer, kernel_map);
  if (DEBUG_SYNTHETIC_KERNEL_GENERATOR)
    for (auto item : kernel_map) {
      printf("\nop_type:%d ip:%llu next_addr:%llu size:%d src_reg1:%d src_reg2:%d dst_regs:%d inst_uid:%llu",
             (uns32)item.second.st.op_type, (unsigned long long)item.second.st.instruction_addr,
             (unsigned long long)item.second.instruction_next_addr, (uns32)item.second.st.size,
             (uns32)item.second.st.src_regs[0], (uns32)item.second.st.src_regs[1], (uns32)item.second.st.dst_regs[0],
             (unsigned long long)item.second.inst_uid);
    }
  return kernel_map;
}

/********************************************************************************/
// [ Memory Kernels ]
// .............................................................................//
// These orchestrate latency or bandwidth bottlenecks in the memory
// subsystem.

// > LOAD_LATENCY_LIMITED (load_latency_limited)
//   - Limits IPC using the latency of specific cache hierarchy levels.
//   - Uses an unrolled, loop-carried dependence chain of load instructions.
//   - Dynamically configures target_stride and target_pool_size so each
//     load hits the targeted cache level but causes 100% conflict misses
//     in all preceding levels. (e.g., MLC_LIMITED misses in DCACHE but
//     hits in MLC).

// > MEM_FU_LIMITED (mem_fu_limited)
//   - Stresses memory bandwidth and load execution units.
//   - Generates a stream of completely independent loads without
//     loop-carried dependencies, allowing concurrent execution limited
//     only by the available Load Functional Units.
/********************************************************************************/

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_load_kernel(Load_Kernel_Type type) {
  // stride should be enough to cause hits at a level but misses in the precceding levels if any
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
        return target_stride;
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
      case LATENCY: {
        kernel_buffer.push_back(generate_generic_load(current_pc, uid_sequence.get_next_element(), mem_addr,
                                                      get_inst_size(), Reg_Id::REG_RAX, Reg_Id::REG_RAX));
        break;
      }
      case BANDWIDTH: {
        kernel_buffer.push_back(generate_generic_load(current_pc, uid_sequence.get_next_element(), mem_addr,
                                                      get_inst_size(), Reg_Id::REG_RAX, Reg_Id::REG_RBX));
        break;
      }
      default:
        break;
    }
  }
  generate_tail_end_insts(kernel_buffer, uid_sequence, pc_sequence, get_inst_size(), get_inst_size());
  flush_kernel_buffer(kernel_buffer, kernel_map);
  if (DEBUG_SYNTHETIC_KERNEL_GENERATOR)
    for (auto item : kernel_map) {
      printf("\nop_type:%d ip:%llu next_addr:%llu size:%d src_reg1:%d ld_vaddr:%llu dst_regs:%d inst_uid:%llu",
             (uns32)item.second.st.op_type, (unsigned long long)item.second.st.instruction_addr,
             (unsigned long long)item.second.instruction_next_addr, (uns32)item.second.st.size,
             (uns32)item.second.st.ld1_addr_regs[0], (uns64)item.second.ld_vaddr, (uns32)item.second.st.dst_regs[0],
             (unsigned long long)item.second.inst_uid);
    }

  return kernel_map;
}
//------------------------------------------------------------------------//
// [Instruction Cache Kernels ]
//........................................................................//
//     > ICACHE_LIMITED (icache_limited)
//       - Stresses frontend cache capacity.
//       - Forces the workload loop size to explicitly exceed ICACHE_SIZE,
//         guaranteeing a 100% I-Cache miss rate upon looping.
//       - Requires frontend prefetchers (FDIP) to be disabled for accurate
//         results, and sizes all generated instructions to the ICACHE_LINE_SIZE.

std::map<uns64, ctype_pin_inst> Kernel_Factory::generate_icache_kernel() {
  std::map<uns64, ctype_pin_inst> kernel_map;
  std::vector<ctype_pin_inst> kernel_buffer;
  uns64 current_pc = 0;
  for (uns64 i{0}; i < workload_length; i++) {
    current_pc = pc_sequence.get_next_element();

    kernel_buffer.push_back(
        generate_alu_type_inst(current_pc, uid_sequence.get_next_element(), get_inst_size(), 1, 2, 3));
  }
  generate_tail_end_insts(kernel_buffer, uid_sequence, pc_sequence, get_inst_size(), get_inst_size());
  flush_kernel_buffer(kernel_buffer, kernel_map);
  if (DEBUG_SYNTHETIC_KERNEL_GENERATOR)
    for (auto item : kernel_map) {
      printf("\nop_type:%d ip:%llu next_addr:%llu size:%d src_reg1:%d src_reg2:%d dst_regs:%d inst_uid:%llu",
             (uns32)item.second.st.op_type, (unsigned long long)item.second.st.instruction_addr,
             (unsigned long long)item.second.instruction_next_addr, (uns32)item.second.st.size,
             (uns32)item.second.st.src_regs[0], (uns32)item.second.st.src_regs[1], (uns32)item.second.st.dst_regs[0],
             (unsigned long long)item.second.inst_uid);
    }
  return kernel_map;
}