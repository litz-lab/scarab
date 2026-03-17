/* Copyright 2026 Litz Lab
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
 * File         : debug/cfg.h
 * Description  : Control Flow Graph collector.
 *
 * Activated by --debug_cfg 1. On each instruction retirement, tracks basic
 * block (BB) transitions: each BB node is identified by its start PC and
 * terminated by a control-flow instruction. Edges represent transitions
 * between BBs. On simulation end, dumps cfg_data.json to OUTPUT_DIR for
 * later aggregation across simpoints by scarab-infra's cfg_analyzer.py.
 ***************************************************************************************/

#ifndef __CFG_H__
#define __CFG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "globals/global_types.h"

/* Forward declaration — avoids including all of op.h from C compilation units */
struct Op_struct;

/* Initialize per-proc CFG state. Call once before simulation starts. */
void cfg_init(void);

/*
 * Track the start of a new instruction (call for every op with bom==TRUE).
 * Used only to initialise current_bb_start on the very first retirement.
 */
void cfg_track_inst(struct Op_struct* op);

/*
 * Count uops and instructions for the current BB (call for every retired op,
 * before cfg_retire_op).
 */
void cfg_accum_uop(struct Op_struct* op);

/*
 * Record a basic-block transition (call for every op with eom==TRUE and
 * table_info->cf_type != NOT_CF).  Updates the node for the current BB
 * (start_pc → end_pc) and the edge to the next BB (oracle_info.npc).
 * Accumulates the inter-BBL retire-cycle delta for the node.
 */
void cfg_retire_op(struct Op_struct* op);

/*
 * Record a BBL prediction event (call for the CF op that terminates the BBL,
 * using op->pred_cycle and the explicit bb_start of its enclosing BBL).
 * The inter-BBL predict delta captures BTB-miss and misprediction-recovery
 * overhead between consecutive BBL predictions.
 */
void cfg_predict_BBL(struct Op_struct* op, Addr bb_start);

/*
 * Record a BBL fetch event (call for the first op of a fetched BBL, using
 * op->fetch_cycle).  The inter-BBL fetch delta captures the icache miss stall
 * of the *preceding* BBL plus normal fetch throughput (off-by-one-BBL
 * attribution); complementary to fetch_latency_sum in cfg_track_inst which
 * attributes the stall to the BBL that caused it.
 */
void cfg_fetch_BBL(struct Op_struct* op);

/*
 * Write cfg_data.json into output_dir.  Call once at the end of simulation
 * for each core (proc_id 0..NUM_CORES-1).
 */
void cfg_dump(const char* output_dir);

#ifdef __cplusplus
}
#endif

#endif /* __CFG_H__ */
