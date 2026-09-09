/* ===============================================================================================
 * Kernel Definitions - Also Defined in Kernel Factory.cc and Synthetic_Bottleneck_kernels.cc
 * -----------------------------------------------------------------------------------------------
      ==========================================================================================================
      * CBR_LIMITED KERNEL (includes cbr_20t, cbr_50t, cbr_80t)
      * ---------------------------------------------------------------------------------------------------------
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
      *---------------------------------KERNEL VARIANTS-------------------------------------------------
      * CBR_20t - t_nt_ratio is 0.2. weights = {20, 80}
      * CBR_50t - t_nt_ratio is 0.2. weights = {50, 50}
      * CBR_50t - t_nt_ratio is 0.2. weights = {80, 20}
      =============================================================================================================

      ==========================================================================================================
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
      ==============================================================================================================

      ===========================================================================================================
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
      ===========================================================================================================

      ===============================================================================================================
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
      * ==============================================================================================================

    =================================================================================================================
      * MEMORY KERNELS (LATENCY, BANDWIDTH)
          * LATENCY_KERNELS (DCACHE_LIMITED, MLC_LIMITED, LLC_LIMITED, MAIN_MEM_LIMITED)
          * BANDWIDTH_KERNELS (MEM_BAND_1FU, MEM_BAND_2FU, MEM_BAND_4FU,)
      * ------------------------------------------------------------------------------------------------------------
      * The memory kernels orchestrate memory latency and bandwidth bottlenecks.

      * The latency workloads are intended to limit IPC through the latency of the various levels in the cache hierachy.
      * To enable latency bottlenecks, an unrolled loop-carried dependence chain including load instructions is
      utilised.
      * Each subsequent Load accesses a memory address that is gauranteed to hit at a particular level but miss in all
      preceeding levels, if any. The memory address pattern is defined in a target_pool and generated by the sampler.
      * The level is configurable as any level in the memory hierachy.

      * The bandwidth kernels orchestrate memory bandwidth bottlenecks.
      * Here the instructions stream consists of a sequence of independent loads that can execute concurrently.However,
      the actual number of loads that can actually execute is limited by the number of available load FUs
      which is configurable in PARAMS.in

      *---------------------------------------Latency-Limited KERNELVARIANTS---------------------------------------*
      * Loop Carried Dependence Chain
      * DCACHE_LIMITED          - All Load accesses hit in Dcache..
      * MLC_LIMITED             - All Load accesses hit in MLC, but miss in DCACHE.
      * LLC_LIMITED             - All Load accesses hit in LLC, but miss in DCACHE and MLC.
      * MAIN_MEMORY_LIMITED     - All Load accesses hit in MAIN_MEMORY but miss in DCACHE, LLC and MLC.


      *-------------------------------------Bandwidth-Limited KERNEL VARIANTS-----------------------------------------*
      * No Loop Carried Dependence Chain, all loads are independent.
      * MEM_BANDWIDTH_1FU             - Single load FU. IPC approx 1. Retiring = 1 / ISSUE_WIDTH
      * MEM_BANDWIDTH_2FU             - two load FUs. IPC approx 2. Retiring = 2 / ISSUE_WIDTH.
      * MEM_BANDWIDTH_4FU             - four Load FUs, IPC approx 4. Retiring = 4 / ISSUE_WIDTH..
      * =============================================================================================================

      ===============================================================================================================
        * ICACHE_LIMITED
      ------------------------------------------------------------------------------------------------------------
      * The kernel stresses the capacity of the ICACHE. By ensuring the loop to be run exceeds ICACHE_SIZE, the return
      to the start of the program is gauranteed 100% ICACHE_MISS.
      * The size for every instruction = ICACHE_LINE_SIZE, moreover, the workload required FDIP/frontend prefetcher be
      turned off.
      ================================================================================================================
*/
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
  Sequence_Pick_Strategy target_strategy;
  Sequence_Pick_Strategy direction_strategy;
  Sampler uid_sequence;
  Sampler pc_sequence;
  Sampler target_pool;

  /* Constructor Helpers */

  /* Downstream configurable helpers */
  std::map<uns64, ctype_pin_inst> generate_ubr_kernel();

  std::map<uns64, ctype_pin_inst> generate_ilp_kernel(uns dependence_chain_length, uns workload_length);

  std::map<uns64, ctype_pin_inst> generate_load_kernel(Load_Kernel_Type type);

  std::map<uns64, ctype_pin_inst> generate_ibr_kernel();

  std::map<uns64, ctype_pin_inst> generate_icache_kernel();

  std::map<uns64, ctype_pin_inst> generate_cbr_kernel();

  // generates a sequence of nops and returns the location the next instruction starts from
  uns64 generate_nop_sequence(std::vector<ctype_pin_inst>&, Sampler& uid_sequence, uns num_of_nops, uns64 starting_pc);

  void generate_tail_end_insts(std::vector<ctype_pin_inst>&, Sampler& uid_sequence, Sampler& target_sequence,
                               uns64 pad_to_next_inst);

  // void flush_kernel_buffer(std::vector<ctype_pin_inst>&, std::map<uns64, ctype_pin_inst>&);

 public:
  // Constructor
  Kernel_Factory(Kernel_Enum kernel, uns64 start_pc, uns64 start_uid, uns64 workload_length);
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