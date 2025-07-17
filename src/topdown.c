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

#include "dcache_stage.h"
#include "idq_stage.h"
#include "lsq.h"
#include "node_stage.h"
#include "op.h"

/**************************************************************************************/
/* Events Update */

/*
 * Event Definitions
 *
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
 */

void topdown_bp_recovery(uns proc_id, Op* op) {
  ASSERT(op->proc_id, op->table_info->cf_type);

  idq_stage_set_recovery_cycle(DECODE_CYCLES);

  STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEARS);
  if (op->oracle_info.recover_at_exec)
    STAT_EVENT(proc_id, TOPDOWN_BR_MISPRED_RETIRED);
}

void topdown_idq_update(uns proc_id, int count_available, int count_issued, int count_issued_on_path) {
  INC_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS, ISSUE_WIDTH);
  INC_STAT_EVENT(proc_id, TOPDOWN_SLOTS_ISSUED, count_issued);
  INC_STAT_EVENT(proc_id, TOPDOWN_SLOTS_RETIRED, count_issued_on_path);

  int recovery_cycle = idq_stage_get_recovery_cycle();
  if (recovery_cycle != 0) {
    ASSERT(proc_id, recovery_cycle > 0);
    idq_stage_set_recovery_cycle(recovery_cycle - 1);
    INC_STAT_EVENT(proc_id, TOPDOWN_RECOVERY_BUBBLES, ISSUE_WIDTH - count_available);
    return;
  }

  // only increment frontend-stall when there is no backend-stall
  if (node->node_stall) {
    return;
  }

  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES, ISSUE_WIDTH - count_available);
  if (count_available == 0)
    STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_GREATER_THAN_MIW);
}

void topdown_exec_update(uns proc_id, uns8 fus_busy) {
  if (fus_busy <= TOPDOWN_FU_EXEC_FEW && node->node_count != 0) {
    STAT_EVENT(proc_id, TOPDOWN_EXEC_STALLS);
    if (lsq_get_load_num() > 0 && fus_busy == 0) {
      STAT_EVENT(proc_id, TOPDOWN_MEM_STALLS_LOAD);
    } else if (lsq_get_unready_store_num() > 0) {
      STAT_EVENT(proc_id, TOPDOWN_MEM_STALLS_STORE);
    }
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
  INC_STAT_EVENT(proc_id, TOPDOWN_FRONTEND_BOUND, GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES));
  INC_STAT_EVENT(proc_id, TOPDOWN_BAD_SPEC,
                 GET_STAT_EVENT(proc_id, TOPDOWN_SLOTS_ISSUED) - GET_STAT_EVENT(proc_id, TOPDOWN_SLOTS_RETIRED) +
                     GET_STAT_EVENT(proc_id, TOPDOWN_RECOVERY_BUBBLES));
  INC_STAT_EVENT(proc_id, TOPDOWN_RETIRING, GET_STAT_EVENT(proc_id, TOPDOWN_SLOTS_RETIRED));
  INC_STAT_EVENT(proc_id, TOPDOWN_BACKEND_BOUND,
                 GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS) - GET_STAT_EVENT(proc_id, TOPDOWN_FRONTEND_BOUND) -
                     GET_STAT_EVENT(proc_id, TOPDOWN_BAD_SPEC) - GET_STAT_EVENT(proc_id, TOPDOWN_RETIRING));

  /* Backend Breakdown */
  if (GET_STAT_EVENT(proc_id, TOPDOWN_EXEC_STALLS) == 0)
    STAT_EVENT(proc_id, TOPDOWN_EXEC_STALLS);
  INC_STAT_EVENT(
      proc_id, TOPDOWN_MEM_BOUND,
      (GET_STAT_EVENT(proc_id, TOPDOWN_MEM_STALLS_LOAD) + GET_STAT_EVENT(proc_id, TOPDOWN_MEM_STALLS_STORE)) *
          GET_STAT_EVENT(proc_id, TOPDOWN_BACKEND_BOUND) / GET_STAT_EVENT(proc_id, TOPDOWN_EXEC_STALLS));
  INC_STAT_EVENT(proc_id, TOPDOWN_CORE_BOUND,
                 GET_STAT_EVENT(proc_id, TOPDOWN_BACKEND_BOUND) - GET_STAT_EVENT(proc_id, TOPDOWN_MEM_BOUND));

  /* Retiring Breakdown */
  // TODO: need more metadata from the simulation frontend to determine if an operand requires MicroSequencer

  /* Front-End Breakdown */
  ASSERT(proc_id, GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS) != 0);
  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_LATENCY, GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_GREATER_THAN_MIW));
  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_BANDWIDTH,
                 GET_STAT_EVENT(proc_id, TOPDOWN_FRONTEND_BOUND) * GET_STAT_EVENT(proc_id, NODE_CYCLE) /
                         GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS) -
                     GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_LATENCY));

  /* Bad Spec Breakdown */
  // TODO: off-path flush is not enabling currently
}
