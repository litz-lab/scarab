/*
 * Copyright 2025 University of California Santa Cruz
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
 * File         : load_value_pred.h
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 10/2025
 * Description  : Generalized speculative load-*result* predictor framework.
 *
 *   Two independent predictor *categories* are supported and may be enabled at
 *   the same time (one active scheme per category, per core):
 *
 *     1. LOAD_PRED_CAT_VALUE - predicts the loaded value so consumers can execute
 *                              before the load resolves (reads the architectural
 *                              result via op->dst_val, see the .cc).
 *     2. LOAD_PRED_CAT_ADDR  - predicts the load's effective address so the access
 *                              can resolve ahead of address generation.
 *
 *   A memory-dependence (store-to-load forwarding/bypass) category is planned as
 *   a future addition; it will slot in as a third category here (ideally hooking
 *   at fetch so it can reuse this same LoadPredictor interface and EOM handling).
 *
 *   Mechanism vs. policy: the pipeline *mechanism* lives on the op
 *   (op->load_value_predicted lets consumers wake early; op->load_value_mispredicted
 *   flags a wrong prediction, and bp_pred_info->recover_at_agen /
 *   recover_at_load_completion route the squash through the branch-recovery path
 *   from the dcache stage).  The *policy* - which predictor, how it trains, when
 *   it speculates - lives entirely in this module.
 ***************************************************************************************/

#ifndef __LOAD_VALUE_PRED_H__
#define __LOAD_VALUE_PRED_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "op.h"

/**************************************************************************************/
/* Predictor categories */

typedef enum Load_Pred_Category_enum {
  LOAD_PRED_CAT_VALUE,  // predicts the loaded value
  LOAD_PRED_CAT_ADDR,   // predicts the load's effective address
  LOAD_PRED_CAT_NUM
} Load_Pred_Category;

/**************************************************************************************/
/* Per-category schemes.  Scheme 0 is always "none" (category disabled). */

/* Value predictors (selected by LOAD_VALUE_PRED_SCHEME). */
typedef enum Load_Value_Pred_Scheme_enum {
  LOAD_VALUE_PRED_SCHEME_NONE,
  LOAD_VALUE_PRED_SCHEME_LAST_VALUE,  // last-value predictor (scaffold; see .cc)
  LOAD_VALUE_PRED_SCHEME_NUM
} Load_Value_Pred_Scheme;

/* Address predictors (selected by LOAD_ADDR_PRED_SCHEME). */
typedef enum Load_Addr_Pred_Scheme_enum {
  LOAD_ADDR_PRED_SCHEME_NONE,
  LOAD_ADDR_PRED_SCHEME_CONST,   // constant-address predictor
  LOAD_ADDR_PRED_SCHEME_STRIDE,  // stride-address predictor
  LOAD_ADDR_PRED_SCHEME_NUM
} Load_Addr_Pred_Scheme;

/*
 * How a correct address prediction is applied (selected by LOAD_ADDR_PRED_MODE):
 *   EARLY_AGEN - the load issues without waiting for its address-operand registers
 *                and accesses the dcache at the predicted address, incurring normal
 *                hit/miss latency; consumers wake at the load's real completion.
 *   RFP        - Register File Prefetching (Shukla et al., ISCA 2022): the value is
 *                prefetched into the register file, so consumers wake early as if the
 *                load-use latency were hidden. Address is verified at exec; a wrong
 *                address squashes the speculative consumers.
 */
typedef enum Load_Addr_Pred_Mode_enum {
  LOAD_ADDR_PRED_MODE_EARLY_AGEN,
  LOAD_ADDR_PRED_MODE_RFP,
  LOAD_ADDR_PRED_MODE_NUM
} Load_Addr_Pred_Mode;

/**************************************************************************************/
/* Lifecycle (per-core).  C linkage: called from the C cmp_model files. */

void alloc_mem_load_predictors(uns num_cores);
void set_load_predictors(uns8 proc_id);
void init_load_predictors(uns8 proc_id, const char* name);
void recover_load_predictors(void);

/*
 * Called from the dcache stage, gated by the load's recover_at_agen /
 * recover_at_load_completion flag, to schedule the squash-and-resteer recovery
 * for a mispredicted predicted load at the given cycle (AGEN cycle for address
 * prediction; the load's done_cycle for value prediction).
 */
void predicted_load_schedule_recovery(Op* op, Counter recover_cycle);

#ifdef __cplusplus
}
#endif

/**************************************************************************************/
/* Pipeline hooks (C++ only: called from ft.cc, which owns the FT op vector). */

#ifdef __cplusplus

/*
 * Fetch-time hook.  Runs the value and address predictors on a load: predicts,
 * applies the early-resolve effect, and trains.  On a wrong prediction it sets
 * op->load_value_mispredicted; the FT builder (which can see the whole macro)
 * then stamps the squash recovery on the macro's EOM op via the call below.
 */
void load_pred_predict_op(Op* op);

/*
 * Fill the load's recovery_info and fire bp_sched_recovery at recover_cycle,
 * reusing the branch-recovery path (non-CF, fall-through resteer) so a
 * mispredicted load re-issues its consumers.  op must be on-path.
 */
void load_pred_schedule_squash(Op* op, Counter recover_cycle);

#endif

#endif /* #ifndef __LOAD_VALUE_PRED_H__ */
