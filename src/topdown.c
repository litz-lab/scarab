/*
 * Copyright (c) 2025 University of California, Santa Cruz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : topdown.c
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 05/2025
 *    Implements the Top-Down performance analysis methodology based on:
 *      Yasin, A. "A Top-Down Method for Performance Analysis and Counters Architecture,"
 *      2014 IEEE International Symposium on Performance Analysis of Systems and Software.
 ***************************************************************************************/

#include "topdown.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "dcache_stage.h"
#include "lsq.h"
#include "map_stage.h"
#include "node_stage.h"
#include "op.h"

const static uns64 TOPDOWN_SCALE_FACTOR = 10000;

// Top-down pipeline width, measured at the decoupled-frontend fetch point: the uop-cache issue width
// (the widest fetch path). A fetch cycle served by the narrower icache/decode path therefore leaves
// UOP_CACHE_WIDTH - DECODE_WIDTH slots as frontend bubbles.
#define TOPDOWN_WIDTH UOP_CACHE_WIDTH

/**************************************************************************************/
/* Events Update */

/* ==================================================================================
 * Event Definitions
 * ----------------------------------------------------------------------------------
 * TotalSlots*            = Total number of issue-pipeline slots.
 * SlotsIssued*           = Utilized issue-pipeline slots to issue operations.
 * SlotsRetired*          = Utilized issue-pipeline slots to retire (complete) operations.
 * FetchBubbles           = Unutilized issue-pipeline slots while there is no backend-stall.
 * RecoveryBubbles        = Unutilized issue-pipeline slots due to recovery from earlier miss-speculation.
 * BrMispredRetired*      = Retired miss-predicted branch instructions.
 * MachineClears*         = Machine clear events (pipeline is flushed).
 * MsSlotsRetired*        = Retired pipeline slots supplied by the micro-sequencer fetch-unit.
 * OpsExecuted*           = Number of operations executed in a cycle.
 * MemStalls.AnyLoad      = Cycles with no uops executed and at least one in-flight load not completed.
 * MemStalls.L1miss       = Cycles with no uops executed and at least one in-flight load that missed the L1-cache.
 * MemStalls.L2miss       = Cycles with no uops executed and at least one in-flight load that missed the L2-cache.
 * MemStalls.L3miss       = Cycles with no uops executed and at least one in-flight load that missed the L3-cache.
 * MemStalls.Stores       = Cycles with few uops executed and no more stores can be issued.
 * ExtMemOutstanding      = Number of outstanding requests to the memory controller every cycle.
 *
 * -----------------------------------------------------------------------------------
 * [Defined in https://github.com/andikleen/pmu-tools/blob/master/skl_client_ratios.py]
 *
 * BackendStalls = CoreStalls + (∑OpsExecuted[= FEW]) + StoreStalls
 * =================================================================================== */

void topdown_bp_recovery(uns proc_id, Op* op) {
  ASSERT(op->proc_id, op->uop->cf_type);

  // Bad-speculation breakdown inputs; the bad-spec slots themselves are counted at the fetch point
  // (topdown_fetch_update) while the frontend fetches the wrong path.
  STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEAR_CYCLES);
  if (op->bp_pred_info->recovery_point == RECOVER_AT_EXEC) {
    ASSERT(op->proc_id, !op->off_path);
    STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_RETIRED_CYCLES);
  }
}

/*
 * Called once per cycle from the decoupled-frontend fetch point (execute_coupled_FSM), partitioning
 * TOPDOWN_WIDTH fetch slots. off_path is the icache stage's own wrong-path state, so bad speculation
 * covers the whole wrong-path fetch -- including BTB misses resolved at decode, which never reach the
 * backend. count_fetched is the ops delivered from the FT this cycle (<= TOPDOWN_WIDTH, since the
 * uop-cache path is the widest); a narrower icache/decode-path cycle leaves the rest as frontend.
 * on_path_fetched is the subset that is on-path, so a transition cycle (on-path ops served before the
 * wrong-path flip) credits those slots to retiring, not bad speculation.
 */
void topdown_fetch_update(uns proc_id, Flag off_path, Flag backend_stall, int count_fetched, int on_path_fetched) {
  INC_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS, TOPDOWN_WIDTH);

  if (off_path) {
    // off_path is stamped at fetch from bp.c's recovery-point classification (icache_stage.c:971), so
    // it covers uopc-served wrong-path too. Credit on-path ops served before the flip to retiring; the
    // rest of the cycle (off-path ops + off-path bubbles) is bad speculation.
    ASSERT(proc_id, on_path_fetched <= TOPDOWN_WIDTH);
    INC_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS, on_path_fetched);
    INC_STAT_EVENT(proc_id, TOPDOWN_BAD_SPEC_SLOTS, TOPDOWN_WIDTH - on_path_fetched);
  } else if (backend_stall) {
    // Fetch stalled because the backend cannot accept ops (downstream full).
    INC_STAT_EVENT(proc_id, TOPDOWN_BACKEND_STALL_SLOTS, TOPDOWN_WIDTH);
    STAT_EVENT(proc_id, TOPDOWN_BACKEND_STALLS_CYCLES);
    if (lsq_get_in_flight_load_num() > 0) {
      STAT_EVENT(proc_id, TOPDOWN_MEM_LOAD_STALLS_CYCLES);
    } else if (!lsq_available(MEM_ST)) {
      STAT_EVENT(proc_id, TOPDOWN_MEM_STORE_STALLS_CYCLES);
    }
  } else {
    ASSERT(proc_id, count_fetched <= TOPDOWN_WIDTH);
    INC_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS, count_fetched);
    INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_SLOTS, TOPDOWN_WIDTH - count_fetched);
    if (count_fetched == 0)
      STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_GT_MIW_CYCLES);
  }

  // Every fetch slot is attributed to exactly one bucket, so the four must sum to the total.
  ASSERT(proc_id, GET_STAT_EVENT(proc_id, TOPDOWN_BAD_SPEC_SLOTS) +
                          GET_STAT_EVENT(proc_id, TOPDOWN_BACKEND_STALL_SLOTS) +
                          GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_SLOTS) +
                          GET_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS) ==
                      GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS));
}

void topdown_exec_update(uns proc_id, uns8 fus_busy) {
  if (fus_busy <= TOPDOWN_FU_EXEC_FEW && node->node_count != 0) {
    STAT_EVENT(proc_id, TOPDOWN_EXEC_STALLS_CYCLES);
  }
}

/**************************************************************************************/
/*
 * Metrics Update
 *  At the end of simulation, the events can be directly used to calculate the
 *  metrics using the following formulas.
 */

/*
 * Top-Down Metrics Formulas
 *
 * Frontend Bound       = FetchBubbles / TotalSlots
 * Bad Speculation      = (SlotsIssued - SlotsRetired + RecoveryBubbles) / TotalSlots
 * Retiring             = SlotsRetired / TotalSlots
 * Backend Bound        = 1 - (Frontend Bound + Bad Speculation + Retiring)
 *
 * Fetch Latency Bound  = FetchBubbles[≥ #MIW] / Clocks
 * Fetch Bandwidth Bound = Frontend Bound - Fetch Latency Bound
 *
 * #BrMispredFraction   = BrMispredRetired / (BrMispredRetired + MachineClears)
 * Branch Mispredicts   = #BrMispredFraction * Bad Speculation
 * Machine Clears       = Bad Speculation - Branch Mispredicts
 *
 * MicroSequencer       = MsSlotsRetired / TotalSlots
 * BASE                 = Retiring - MicroSequencer
 *
 * #ExecutionStalls     = (∑OpsExecuted[= FEW]) / Clocks
 *
 * Memory Bound         = (MemStalls.AnyLoad + MemStalls.Stores) / Clocks
 * Core Bound           = #ExecutionStalls - Memory Bound
 *
 * L1 Bound             = (MemStalls.AnyLoad - MemStalls.L1miss) / Clocks
 * L2 Bound             = (MemStalls.L1miss - MemStalls.L2miss) / Clocks
 * L3 Bound             = (MemStalls.L2miss - MemStalls.L3miss) / Clocks
 * Ext. Memory Bound    = MemStalls.L3miss / Clocks
 *
 * MEM Bandwidth        = ExtMemOutstanding[≥ THRESHOLD] / ExtMemOutstanding[≥ 1]
 * MEM Latency          = (ExtMemOutstanding[≥ 1] / Clocks) - MEM Bandwidth
 */

void topdown_done(uns proc_id) {
  /* Top-Level Breakdown */
  uns64 frontend_bound = GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_SLOTS) * TOPDOWN_SCALE_FACTOR /
                         GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
  INC_STAT_EVENT(proc_id, TOPDOWN_FRONTEND_BOUND, frontend_bound);

  uns64 bad_spec_bound = GET_STAT_EVENT(proc_id, TOPDOWN_BAD_SPEC_SLOTS) * TOPDOWN_SCALE_FACTOR /
                         GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
  INC_STAT_EVENT(proc_id, TOPDOWN_BAD_SPEC_BOUND, bad_spec_bound);

  uns64 retiring_bound = GET_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS) * TOPDOWN_SCALE_FACTOR /
                         GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
  INC_STAT_EVENT(proc_id, TOPDOWN_RETIRING_BOUND, retiring_bound);

  uns64 backend_bound = TOPDOWN_SCALE_FACTOR - frontend_bound - bad_spec_bound - retiring_bound;
  INC_STAT_EVENT(proc_id, TOPDOWN_BACKEND_BOUND, backend_bound);

  /* Backend Breakdown */
  // prevent division by zero
  uns64 backend_stalls_cycles = GET_STAT_EVENT(proc_id, TOPDOWN_BACKEND_STALLS_CYCLES);
  if (backend_stalls_cycles == 0) {
    backend_stalls_cycles++;
  }

  uns64 mem_stalls_cycles = (GET_STAT_EVENT(proc_id, TOPDOWN_MEM_LOAD_STALLS_CYCLES) +
                             GET_STAT_EVENT(proc_id, TOPDOWN_MEM_STORE_STALLS_CYCLES));
  uns64 mem_bound = backend_bound * mem_stalls_cycles / backend_stalls_cycles;
  INC_STAT_EVENT(proc_id, TOPDOWN_MEM_BOUND, mem_bound);
  INC_STAT_EVENT(proc_id, TOPDOWN_CORE_BOUND, backend_bound - mem_bound);

  /* Retiring Breakdown */
  // TODO: need more metadata from the simulation frontend to determine if an operand requires MicroSequencer

  /* Front-End Breakdown */
  ASSERT(proc_id, GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS) != 0);
  uns64 latency_bound = GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_GT_MIW_CYCLES) * TOPDOWN_SCALE_FACTOR /
                        GET_STAT_EVENT(proc_id, NODE_CYCLE);
  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_LATENCY_BOUND, latency_bound);
  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_BANDWIDTH_BOUND, frontend_bound - latency_bound);

  /* Bad Spec Breakdown */
  // prevent division by zero
  uns64 bad_spec_cycles = GET_STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_RETIRED_CYCLES) +
                          GET_STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEAR_CYCLES);
  if (bad_spec_cycles == 0) {
    bad_spec_cycles++;
  }

  INC_STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_BOUND,
                 bad_spec_bound * GET_STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_RETIRED_CYCLES) / bad_spec_cycles);
  INC_STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEARS_BOUND,
                 bad_spec_bound - GET_STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_BOUND));
}
