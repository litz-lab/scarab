/* Copyright 2024 Litz Lab
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
 * File         : decoupled_frontend.h
 * Author       : Surim Oh, Mingsheng Xu, Yuanpeng Liao, Heiner Litz
 * Date         :
 * Description  : Decoupled Frontend (Decoupled_FE) header
 ***************************************************************************************/

#ifndef __DECOUPLED_FE_H__
#define __DECOUPLED_FE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <math.h>
#include <stdbool.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "bp/bp.param.h"

#include "bp/bp.h"
#include "frontend/frontend.h"
#include "frontend/pin_trace_fe.h"

#include "stage_data.h"

typedef enum OFF_PATH_REASON_enum {
  REASON_NOT_IDENTIFIED,
  REASON_IBTB_MISS,
  REASON_BTB_MISS,
  // op that misses in the BTB and the BP incorrectly predicts not taken
  REASON_BTB_MISS_MISPRED,
  REASON_MISPRED,
  REASON_MISFETCH,
  REASON_LATE_BTB_HIT,
} Off_Path_Reason;

typedef enum CONF_OFF_PATH_REASON_enum {
  REASON_IBTB_MISS_BP_TAKEN,
  REASON_BTB_MISS_BP_TAKEN_CONF_0,
  REASON_BTB_MISS_BP_TAKEN_CONF_1,
  REASON_BTB_MISS_BP_TAKEN_CONF_2,
  REASON_BTB_MISS_BP_TAKEN_CONF_3,
  REASON_BTB_MISS_RATE,
  REASON_IBTB_MISS_RATE,
  REASON_MISFETCH_RATE,
  REASON_MISPRED_RATE,
  REASON_CONF_THRESHOLD,
  REASON_PERFECT_CONF,
  REASON_CONF_NOT_IDENTIFIED,
} Conf_Off_Path_Reason;

// DFEx_TRIGGER_POLICY param: when alt DFE is activated.
//   PRIMARY_DFE                - reserved for MAIN_BP.
//   CONTINUE_ON_RECOVERY       - activate at every main recovery, continuing
//                                main's just-abandoned off-path stream.
//   ALTERNATE_ON_PREDICTION    - activate on every CF main predicts (when alt
//                                is inactive). Realistic: real hardware can
//                                observe "BP just emitted a prediction" but
//                                cannot tell if that prediction was wrong.
//   ALTERNATE_ON_MISPREDICTION - NOT REALISTIC. Activate only on simulator-
//                                detected mispredictions (oracle-aware).
//                                Useful for upper-bound studies of an alt-BP
//                                mechanism that perfectly knows when main is
//                                wrong; not modelable in real hardware.
typedef enum DFE_Trigger_Policy_enum {
  PRIMARY_DFE,
  CONTINUE_ON_RECOVERY,
  ALTERNATE_ON_PREDICTION,
  ALTERNATE_ON_MISPREDICTION,
} DFE_Trigger_Policy;

// DFEx_STOP_POLICY param: when alt DFE is preempted/deactivated.
//   PRIMARY_DFE_STOP      - sentinel for MAIN_BP; the primary DFE is never
//                           subject to alt stop logic. init() asserts this is
//                           used iff the trigger policy is PRIMARY_DFE.
//   STOP_ON_RECOVERY      - alt deactivates at the next main recovery
//                           (default for alt BPs).
//   STOP_ON_PREDICTION    - alt is preempted on every CF main predicts (while
//                           alt is active). Realistic counterpart to
//                           ALTERNATE_ON_PREDICTION: real hardware can
//                           observe a prediction event but not its
//                           correctness.
//   STOP_ON_MISPREDICTION - NOT REALISTIC. Preempt only on simulator-detected
//                           mispredictions (oracle-aware). Useful for
//                           upper-bound studies; not modelable in real
//                           hardware.
typedef enum DFE_Stop_Policy_enum {
  PRIMARY_DFE_STOP,
  STOP_ON_RECOVERY,
  STOP_ON_PREDICTION,
  STOP_ON_MISPREDICTION,
} DFE_Stop_Policy;

typedef enum BpId_enum {
  MAIN_BP = 0,
  ALT_BP_1,
  ALT_BP_2,
  ALT_BP_3,
  ALT_BP_4,
} BpId;

typedef struct FT FT;
typedef struct Decoupled_FE Decoupled_FE;

typedef struct decoupled_fe_iter decoupled_fe_iter;

struct decoupled_fe_iter {
  // the ft index
  uint64_t ft_pos;
  // the op index
  uint64_t op_pos;
  // the flattened op index, as if the ftq is an 1-d array
  uint64_t flattened_op_pos;
};

// C-compatible API
// Simulator API
void alloc_mem_decoupled_fe(uns numProcs, uns numBPs);
void init_decoupled_fe(uns proc_id, uns bp_id, Bp_Data* bp_data);
void set_decoupled_fe(uns proc_id, uns bp_id);
void reset_decoupled_fe();
void debug_decoupled_fe();
void update_decoupled_fe(uns proc_id, uns bp_id);
// Icache/Core API
void recover_decoupled_fe(uns proc_id, uns bp_id, Cf_Type cf_type, Recovery_Info* info);
FT* decoupled_fe_pop_ft();
bool decoupled_fe_is_off_path();
void decoupled_fe_retire(Op* op, int proc_id, uns64 inst_uid);
// FT::predict_ft calls this after each CF main predicts (per-CF prediction
// event). Drives ALTERNATE_ON_PREDICTION trigger and STOP_ON_PREDICTION stop
// across alt DFEs at the moment main's bp_data has just spec-updated for op.
void decoupled_fe_on_main_prediction(uns proc_id, Op* op);
// bp_predict_op (in bp.c) calls this just before main's spec_update_func, so
// any alt that's about to be (re-)triggered by the next main prediction event
// can capture main's pre-spec-update bp_data via bp_sync. The alt-event
// dispatcher then re-applies spec_update on alt's TAGE with alt's direction.
// Skipped during warmup and for off-path predictions.
void decoupled_fe_capture_main_pre_state(uns proc_id, Op* op);
// FTQ API
void decoupled_fe_set_ftq_num(uint64_t ftq_ft_num);
uint64_t decoupled_fe_get_ftq_num();
uint64_t decoupled_fe_get_next_on_path_op_num();
uint64_t decoupled_fe_get_next_off_path_op_num();
Op* decoupled_fe_get_cur_op();
Off_Path_Reason decoupled_fe_get_off_path_reason();
Conf_Off_Path_Reason decoupled_fe_get_conf_off_path_reason();
void decoupled_fe_conf_resovle_cf(Op* op);
void decoupled_fe_print_conf_data();
// FTQ API
Decoupled_FE* decoupled_fe_new_ftq_iter(uns proc_id, uns bp_id, uns* ftq_idx);
/* Returns the Op at current iterator position or NULL if FTQ is empty or the end of FTQ was reached
   if end_of_ft is true the Op is the last one in a fetch target (cache-line boundary of taken branch)*/
Op* decoupled_fe_ftq_iter_get(Decoupled_FE* dfe, uns iter_idx, bool* end_of_ft);
/* Increments iterator and returns the Op at iterator position or NULL if FTQ is empty or the end of FTQ was reached
   if end_of_ft is true the Op is the last one in a fetch target (cache-line boundary of taken branch)*/
Op* decoupled_fe_ftq_iter_get_next(Decoupled_FE* dfe, uns iter_idx, bool* end_of_ft);
/* Returns iter flattened offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_offset(Decoupled_FE* dfe, uns iter_idx);
/* Returns iter ft offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_ft_offset(Decoupled_FE* dfe, uns iter_idx);
uint64_t decoupled_fe_ftq_num_ops(Decoupled_FE* dfe);
uint64_t decoupled_fe_ftq_num_fts(Decoupled_FE* dfe);
Flag lookahead_buffer_can_fetch_op(uns proc_id);
FT* lookahead_buffer_get_FT(uns proc_id, uint64_t ptr_pos);
uint64_t lookahead_buffer_rdptr(uns proc_id);
uint64_t lookahead_buffer_count(uns proc_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#ifdef __cplusplus

// C++-only includes
#include <deque>
#include <memory>
#include <vector>

#include "confidence/conf.hpp"

// Note: FT and Decoupled_FE are both forward-declared in the C-visible section
// above via `typedef struct FT FT;` / `typedef struct Decoupled_FE Decoupled_FE;`.
// We keep the `struct` tag here to match (C++ struct and class differ only in
// default access, and both definitions use explicit `public:`).
struct FT;
struct FT_PredictResult;

struct Decoupled_FE {
 public:
  Decoupled_FE()
      : proc_id(0),
        bp_id(0),
        bp_data(nullptr),
        dfe_trigger_policy(0),
        dfe_stop_policy(0),
        current_ft_to_push(nullptr),
        saved_recovery_ft(nullptr),
        off_path(0),
        conf_off_path(0),
        sched_off_path(0),
        stalled(false),
        exit_on_off_path(false),
        op_num(1),
        current_off_path_op_num(0),
        recovery_addr(0),
        redirect_cycle(0),
        ftq_ft_num(FE_FTQ_BLOCK_NUM),
        trace_mode(false),
        cur_op(nullptr),
        conf(nullptr),
        state(INACTIVE),
        next_state(INACTIVE) {}
  ~Decoupled_FE();
  void init(uns proc_id, uns bp_id, Bp_Data* bp_data, uns dfe_trigger_policy, uns dfe_stop_policy);
  int is_off_path() { return is_off_path_state(); }
  // Same predicate as is_off_path(), exposed under the alt-DFE-friendly name.
  // For an alt DFE the SERVING_OFF_PATH state is precisely the "alt has been
  // triggered and is in flight" condition, so calling this is_active() at the
  // alt-iteration call sites reads more clearly than is_off_path().
  bool is_active() { return is_off_path_state(); }
  void recover(Cf_Type cf_type, Recovery_Info* info);
  void update();
  FT* pop_ft();
  uns new_ftq_iter();
  Op* ftq_iter_get(uns iter_idx, bool* end_of_ft);
  Op* ftq_iter_get_next(uns iter_idx, bool* end_of_ft);
  uint64_t ftq_iter_offset(uns iter_idx);
  uint64_t ftq_iter_ft_offset(uns iter_idx);
  uint64_t ftq_num_ops();
  uint64_t ftq_num_fts() { return ftq.size(); }
  void stall(Op* op);
  void retire(Op* op, int op_proc_id, uns64 inst_uid);
  void set_ftq_num(uint64_t set_ftq_ft_num) { ftq_ft_num = set_ftq_ft_num; }
  uint64_t get_ftq_num() { return ftq_ft_num; }
  uns get_proc_id() { return proc_id; }
  uns get_bp_id() { return bp_id; }
  Bp_Data* get_bp_data() { return bp_data; }
  Op* get_cur_op() { return cur_op; }
  Conf* get_conf() { return conf; }
  uns get_conf_off_path() { return conf->get_conf(); }
  Off_Path_Reason get_off_path_reason() { return conf->get_off_path_reason(); }
  Conf_Off_Path_Reason get_conf_off_path_reason() { return conf->get_conf_off_path_reason(); }
  void conf_resolve_cf(Op* op) { conf->resolve_cf(op); }
  Off_Path_Reason eval_off_path_reason(Op* op);
  void print_conf_data() { conf->print_data(); }
  uint64_t get_next_on_path_op_num() { return op_num++; }
  uint64_t get_next_off_path_op_num() { return current_off_path_op_num++; }
  Op* get_last_fetch_op();
  uns get_dfe_trigger_policy() { return dfe_trigger_policy; }
  uns get_dfe_stop_policy() { return dfe_stop_policy; }
  // Copy main's bp_data into this (alt) DFE's bp_data (history, predictor
  // tables, CRS, ...). Standalone of activate_off_path so the per-CF/per-mispred
  // dispatch can capture main's PRE-spec-update state via the bp.c pre-hook
  // and then drive the trigger phase later without re-syncing.
  void bp_sync_from_main();
  // Frontend redirect + state transition to SERVING_OFF_PATH (alt-only). No
  // bp_sync. Use after capture_main_pre_state_for_alts has put alt at main's
  // pre-state and apply_alt_spec_update has redone the trigger spec_update
  // with alt's direction.
  void activate_off_path_only(uns64 inst_uid, Addr fetch_addr);
  // bp_sync_from_main + activate_off_path_only. Used by recover()
  // (CONTINUE_ON_RECOVERY) where we just want main's current state.
  void activate_off_path(uns64 inst_uid, Addr fetch_addr);
  // (Alt DFE) Re-do the trigger op's spec_update on alt's TAGE with alt's
  // direction (opposite of main's predicted direction). Caller must have
  // already captured main's pre-spec-update state on alt's bp_data via the
  // bp.c pre-hook (capture_main_pre_state_for_alts). After this, alt sees
  // "main's pre-trigger state + alt's direction at the trigger op".
  void apply_alt_spec_update(Op* trigger_op);
  // (Alt DFE) apply_alt_spec_update + activate_off_path_only at
  // alt_direction_target. Caller must have checked alt is inactive,
  // ftq is empty, and alt_direction_target(trigger_op) is non-zero.
  void trigger_alt(Op* trigger_op);
  // (MAIN_BP) Pre-spec-update hook: for every alt DFE that's about to be
  // (re-)triggered by the upcoming prediction event, bp_sync_from_main to
  // capture main's pre-spec-update state on alt's bp_data.
  void capture_main_pre_state_for_alts(Op* trigger_op);
  // (Alt DFE) Stop a running alt episode: clear FTQ, redirect frontend to 0,
  // transition to INACTIVE.
  void stop_alt_episode();
  // (MAIN_BP) Shared per-event alt-DFE dispatcher. For each alt: stop any
  // running alt whose stop_policy == match_stop, then (re-)trigger any
  // currently-inactive alt whose trigger_policy == match_trigger. The pair
  // (match_trigger, match_stop) identifies one event class; the named
  // wrappers below pass the values for the prediction or misprediction event
  // they represent. Mutually-exclusive trigger/stop axes mean alt has at most
  // one matching policy per axis.
  void drive_alt_on_event(Op* trigger_op, DFE_Trigger_Policy match_trigger, DFE_Stop_Policy match_stop);
  // (MAIN_BP) Misprediction-event entry point.
  void drive_alt_on_misprediction(Op* trigger_op);
  // (MAIN_BP) Per-CF prediction-event entry point. Called per CF from
  // FT::predict_ft via the decoupled_fe_on_main_prediction C wrapper.
  // Off-path predictions (FT::build with off_path=true) intentionally don't
  // fire this; alt _ON_PREDICTION semantics only cover main's on-path /
  // recovery predict_ft pass.
  void drive_alt_on_prediction(Op* trigger_op);

  // FSM states for DFE
  enum DFE_STATE {
    INACTIVE,
    SERVING_ON_PATH,
    SERVING_OFF_PATH,
    RECOVERING
    // Add more states as needed
  };

 protected:
  void set_conf_off_path() { conf_off_path = true; }
  void dfe_recover_op();
  bool is_off_path_state() const { return state == SERVING_OFF_PATH; }
  void check_consecutivity_and_push_to_ftq();
  void redirect_to_off_path(FT_PredictResult result);
  inline uint64_t ftq_max_size() { return ftq_ft_num; }
  void set_off_path_op_num(uint64_t op_num) { current_off_path_op_num = op_num; }
  void set_on_path_op_num(uint64_t op_num) { this->op_num = op_num; }

  uns proc_id;
  uns bp_id;
  Bp_Data* bp_data;
  uns dfe_trigger_policy;
  uns dfe_stop_policy;

  // Per core fetch target queue:
  // Each core has a queue of FTs,
  // where each FT contains a queue of micro instructions.
  std::deque<FT*> ftq = {};
  // keep track of the current FT to be pushed next
  FT* current_ft_to_push;
  FT* saved_recovery_ft;

  int off_path;
  int conf_off_path;
  int sched_off_path;
  bool stalled;
  bool exit_on_off_path;
  uint64_t op_num;
  uint64_t current_off_path_op_num;
  std::vector<std::unique_ptr<decoupled_fe_iter>> ftq_iterators = {};
  uint64_t recovery_addr;
  uint64_t redirect_cycle;
  uint64_t ftq_ft_num;
  bool trace_mode;
  Op* cur_op;
  Conf* conf;

  DFE_STATE state;       // FSM state (applied each update)
  DFE_STATE next_state;  // requested next state, applied inside update()
};

#endif  // __cpluscplus
#endif  // __DECOUPLED_FE_H__
