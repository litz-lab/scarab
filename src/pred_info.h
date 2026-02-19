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
  Addr pred_addr;          // address used to predict branch (might be fetch_addr)
  Addr pred_npc;           // predicted next pc field
  uns8 pred;               // predicted direction of branch, set by the branch predictor
  uns8 pred_orig;          // predicted direction of branch, not overwritten on BTB miss (for fdip)
  Flag misfetch;           // true if target address is the ONLY thing that was wrong
  Flag mispred;            // true if the direction of the branch was mispredicted and the
                           // branch should cause a recovery, set by the branch predictor
  Flag recovery_sch;       // true if this op has scheduled a recovery
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
  uns32 pred_global_hist;       // global history used to predict the branch
  uns32 pred_targ_hist;         // global history used to predict the indirect branch
  uns8 hybridgp_gpred;          // hybridgp's global prediction
  uns8 hybridgp_ppred;          // hybridgp's pred-address prediction
  uns8 pred_tc_selector_entry;  // which ibtb predicted this op?

  Flag pred_conf;
  Addr pred_conf_index;
  uns opc_index;

  int8 off_path_reason;  // reason this op will cause a recovery (Off_Path_Reason enum)
} Bp_Pred_Info;

typedef struct Btb_Pred_Info_struct {
  Flag btb_miss;           // true if the target is not known at prediction time
  Flag btb_miss_resolved;  // true if the btb miss is resolved by the pipeline.
  Flag no_target;          // true if there is no target for this branch at prediction time
  Flag ibp_miss;           // true if the target is not predicted by the indirect pred
  Addr pred_target;        // selected target from BTB/IBTB (if any)
} Btb_Pred_Info;

#endif /* #ifndef __PRED_INFO_H__ */
