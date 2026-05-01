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
 * File         : pred_info.h
 * Author       : Surim Oh <soh31@ucsc.edu>
 * Date         : 2/15/2026
 * Description  : Branch prediction and BTB prediction info structs
 ***************************************************************************************/

#ifndef __PRED_INFO_H__
#define __PRED_INFO_H__

#include "globals/global_types.h"

typedef struct Bp_Pred_Info_struct {
  Addr pred_npc;           // predicted next pc field
  Counter bp_ready_cycle;  // cycle when this level's prediction becomes available
  int64 pred_branch_id;    // predictor-local branch id for speculative checkpoint/recover
  uns8 pred;               // predicted direction of branch, set by the branch predictor
  uns8 pred_orig;          // predicted direction of branch, not overwritten on BTB miss (for fdip)
  Flag recovery_sch;       // true if this op has scheduled a recovery
  Flag recover_at_fe;      // op will schedule recovery in frontend (early correction)
  Flag recover_at_decode;  // op will schedule recovery at decode
  Flag recover_at_exec;    // op will schedule recovery at exec

  // Only for perceptron
  uns64 pred_perceptron_global_hist;            // global history used to predict the branch
  uns64 pred_conf_perceptron_global_hist;       // global history used to confidence predict the branch
  uns64 pred_conf_perceptron_global_misp_hist;  // global history used to confidence predict the branch

  uns8* pred_gpht_entry;        // entry used for interference free pred
  uns8* pred_ppht_entry;        // entry used for interference free pred
  uns8* pred_spht_entry;        // entry used for interference free pred
  uns32 pred_local_hist;        // local history used to predict the branch
  uns32 pred_global_hist;       // global history used to predict the branch (CBR/REP direction)
  uns8 hybridgp_gpred;          // hybridgp's global prediction
  uns8 hybridgp_ppred;          // hybridgp's pred-address prediction

  Flag pred_conf;
  Addr pred_conf_index;
  uns opc_index;

  int8 off_path_reason;  // reason this op will cause a recovery (Off_Path_Reason enum)
} Bp_Pred_Info;

typedef struct Btb_Pred_Info_struct {
  Flag btb_miss_resolved;  // true if the btb miss is resolved by the pipeline.
  Flag no_target;          // true if there is no target for this branch at prediction time
  Flag ibp_miss;           // true if the target is not predicted by the indirect pred
  Addr pred_target;        // selected target from BTB/IBTB (if any)

  // Pre-computed BTB lookup result, populated once by bp_predict_btb() before
  // bp_predict_op() is called.  Both BP_PRED_L0 and BP_PRED_MAIN read from
  // these fields instead of querying the BTB cache directly.
  Flag btb_l0_hit;     // TRUE if L0 BTB holds an entry for this branch
  Addr btb_l0_target;  // target stored in L0 BTB (valid when btb_l0_hit)

  Flag btb_l1_hit;     // TRUE if L1 BTB holds an entry for this branch
  Addr btb_l1_target;  // target stored in L1 BTB (valid when btb_l1_hit)

  Flag btb_main_hit;     // TRUE if the main BTB holds an entry for this branch
  Addr btb_main_target;  // branch target stored in the BTB (valid when btb_main_hit)
  Addr btb_index_addr;   // address used to look up btb for prediction

  uns btb_pred_latency;  // latency of pred_target; MAX_UNS means no prediction-time target

  // IBP-specific history saved during bp_predict_btb() for use in the
  // corresponding update call.  Kept here (not Bp_Pred_Info) so that they are
  // available before bp_predict_op() populates bp_pred_l0/bp_pred_main.
  uns32 ibp_pred_targ_hist;         // target history used by tc_tagged / tc_tagless IBP
  uns32 ibp_pred_global_hist;       // global history used by tc_hybrid IBP
  uns8 ibp_pred_tc_selector_entry;  // selector entry saved by tc_hybrid IBP
} Btb_Pred_Info;

static inline Flag btb_pred_miss(const Btb_Pred_Info* btb_pred_info) {
  return btb_pred_info->btb_pred_latency == MAX_UNS;
}

static inline Flag btb_pred_hit_by(const Btb_Pred_Info* btb_pred_info, uns latency) {
  return !btb_pred_miss(btb_pred_info) && btb_pred_info->btb_pred_latency <= latency;
}

// Prediction levels for the two-level (L0 + main) branch predictor hierarchy.
//
// IMPORTANT: The numeric values (L0=0, MAIN=1) must remain contiguous and match
// the order of the per-level aggregate stat pairs in bp.stat.def (e.g.
// L0_ALL_PREDICTIONS / MAIN_ALL_PREDICTIONS and their OFFPATH variants).
typedef enum Bp_Pred_Level_enum {
  BP_PRED_L0 = 0,
  BP_PRED_MAIN = 1,
} Bp_Pred_Level;

#endif /* #ifndef __PRED_INFO_H__ */
