// --------------------------------------------------------------------------------
// 2. KERNEL PROFILES & BEHAVIOR
// --------------------------------------------------------------------------------
// The generator maps the selected `kernel` enum to specific implementation logic
// that synthesizes bottleneck-inducing instruction streams.

//     [ 2.1 Control Flow (CF) Kernels ]
//     ........................................................................
//     Control flow kernels are designed to trigger Top-Down "Bad Speculation"
//     bottlenecks.  Rather than strictly sizing packets to fill an entire cache line,
//     issue packets are simply bounded by the ICACHE_LINE_SIZE to prevent
//     boundary-crossing stalls, with any leftover dead space cleanly padded with NOPs.

//     > CBR_LIMITED (cbr_limited)
//       - Generates conditional branches with fixed targets but randomized
//         directions.
//       - Directions (taken/not-taken) are weighted according to t_nt_ratio.
//       - The Kernel_Dispatcher rerandomizes the branch direction sequence
//         each time control returns to the beginning of the kernel.

//     > UBR_LIMITED / BTB_LIMITED (btb_limited)
//       - Generates a sequence of unconditional branches using a set of
//         uniformly generated, shuffled targets.
//       - Depending on the configured target pool pattern, this kernel can
//         guarantee either a 100% BTB hit rate, a 100% miss rate, or a
//         specific ratio in between.

//     > IBR_LIMITED (ibr_limited)
//       - Produces indirect branches branching to targets obtained indirectly
//         from a fixed memory address via a source register.
//       - If a randomized target pool is used, the Kernel_Dispatcher
//         rerandomizes the targets when execution loops back to the start.

//     [ 2.2 Memory Kernels ]
//     ........................................................................
//     These orchestrate latency or bandwidth bottlenecks in the memory
//     subsystem.

//     > LOAD_LATENCY_LIMITED (load_latency_limited)
//       - Limits IPC using the latency of specific cache hierarchy levels.
//       - Uses an unrolled, loop-carried dependence chain of load instructions.
//       - Dynamically configures target_stride and target_pool_size so each
//         load hits the targeted cache level but causes 100% conflict misses
//         in all preceding levels. (e.g., MLC_LIMITED misses in DCACHE but
//         hits in MLC).

//     > MEM_FU_LIMITED (mem_fu_limited)
//       - Stresses memory bandwidth and load execution units.
//       - Generates a stream of completely independent loads without
//         loop-carried dependencies, allowing concurrent execution limited
//         only by the available Load Functional Units.

//     [ 2.3 Instruction-Level Parallelism (ILP) Kernels ]
//     ........................................................................
//     > ILP_LIMITED (ilp_limited)
//       - Constructs sequences of ALU instructions mapped to source and
//         destination registers to create strict dependence chains.
//       - If dependence_chain_count is 1, each instruction stalls until the
//         previous one completes.
//       - If dependence_chain_count > 1, the factory generates mutually
//         exclusive chains, allowing parallel execution up to the configured
//         chain limit.

//     [ 2.4 Instruction Cache Kernels ]
//     ........................................................................
//     > ICACHE_LIMITED (icache_limited)
//       - Stresses frontend cache capacity.
//       - Forces the workload loop size to explicitly exceed ICACHE_SIZE,
//         guaranteeing a 100% I-Cache miss rate upon looping.
//       - Requires frontend prefetchers (FDIP) to be disabled for accurate
//         results, and sizes all generated instructions to the ICACHE_LINE_SIZE.

#include <map>

#include "isa/isa.h"

#include "ctype_pin_inst.h"
#include "kernel_params.h"
#include "sampler.h"

#ifndef KERNEL_FACTORY_H
#define KERNEL_FACTORY_H

class Kernel_Factory {
  Kernel_Enum kernel;

  // starting pc and uid for generating a kernel
  uns64 start_pc;
  uns64 start_uid;
  // preserved so that control flow workloads can be rerandomized
  uns64 workload_length;
  uns64 starting_target;
  uns64 target_stride;
  uns64 target_pool_size;
  double t_nt_ratio;
  Limit_Load_To level;
  uns num_of_dependence_chains;
  uns inst_size;
  uns nop_size;
  uns pad_length;
  Sampling_Strategy target_strategy;
  Sampling_Strategy direction_strategy;
  Sampler uid_sequence;
  Sampler pc_sequence;
  Sampler target_pool;

  /* Constructor Helpers */

  /* Downstream configurable helpers */
  std::map<uns64, ctype_pin_inst> generate_ubr_kernel();

  std::map<uns64, ctype_pin_inst> generate_ilp_kernel(uns dependence_chain_length, uns workload_length);

  std::map<uns64, ctype_pin_inst> generate_load_kernel(Load_Kernel_Type type);

  std::map<uns64, ctype_pin_inst> generate_round_robin_ibr_kernel();

  std::map<uns64, ctype_pin_inst> generate_random_ibr_kernel();

  std::map<uns64, ctype_pin_inst> generate_icache_kernel();

  std::map<uns64, ctype_pin_inst> generate_cbr_kernel();

  // generates a sequence of nops and returns the location the next instruction starts from
  uns64 generate_nop_sequence(std::vector<ctype_pin_inst>&, Sampler& uid_sequence, uns num_of_nops, uns64 starting_pc);

  void generate_tail_end_insts(std::vector<ctype_pin_inst>&, Sampler& uid_sequence, Sampler& target_sequence,
                               uns pad_to_next_inst, uns unconditional_branch_size);

  // Fills the remaining cache line if a cf kernel packet does not fill the entire line
  void fill_remaining_cache_line(std::vector<ctype_pin_inst>& kernel_buffer, uns packet_size, uns current_pc);

 public:
  // Constructor
  Kernel_Factory(Kernel_Enum kernel, uns64 start_pc, uns64 start_uid);
  // General kernel generator
  std::map<uns64, ctype_pin_inst> generate_kernel();

  uns64 get_start_pc() const { return start_pc; }
  uns64 get_start_uid() const { return start_uid; }
  uns64 get_target_pool_size() const { return target_pool_size; }
  uns get_inst_size() const { return inst_size; }
  uns get_nop_size() const { return nop_size; }

  void regenerate_target_pool() {
    target_pool.randomize_sequence_vector();
    target_pool.reset_next_pointer();
  }
  void reset_target_pool_pointer() { target_pool.reset_next_pointer(); }
};

#endif