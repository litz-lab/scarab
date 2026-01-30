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

// DFEx_RECOVERY_POLICY param
typedef enum DFE_Recovery_Policy_enum {
  PRIMARY_DFE,
  CONTINUE_ON_RECOVERY,
  CONTINUE_ON_PREDICTION,
} DFE_Recovery_Policy;

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
void decoupled_fe_pop_ft(FT* ft);
bool decoupled_fe_is_off_path();
void decoupled_fe_retire(Op* op, int proc_id, uns64 inst_uid);
FT* decoupled_fe_get_ft();
// FTQ API
void decoupled_fe_set_ftq_num(uint64_t ftq_ft_num);
uint64_t decoupled_fe_get_ftq_num();
uint64_t decoupled_fe_get_next_on_path_op_num();
uint64_t decoupled_fe_get_next_off_path_op_num();
Op* decoupled_fe_get_cur_op();
uns decoupled_fe_get_conf();
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
#ifdef __cplusplus
}  // extern "C"
#endif

#ifdef __cplusplus

// C++-only includes
#include <deque>
#include <memory>
#include <vector>

#include "confidence/conf.hpp"

class FT;
struct FT_PredictResult;

class Decoupled_FE {
 public:
  Decoupled_FE()
      : off_path(0),
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
        state(INACTIVE),
        next_state(INACTIVE) {}
  void init(uns proc_id, uns bp_id, Bp_Data* bp_data, uns dfe_recovery_policy);
  int is_off_path() { return is_off_path_state(); }
  void recover(Cf_Type cf_type, Recovery_Info* info);
  void update();
  FT* get_ft();
  void pop_ft(FT* ft);
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
  uns get_dfe_recovery_policy() { return dfe_recovery_policy; }

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
  uns dfe_recovery_policy;

  // Per core fetch target queue:
  // Each core has a queue of FTs,
  // where each FT contains a queue of micro instructions.
  std::deque<FT*> ftq;
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
  std::vector<std::unique_ptr<decoupled_fe_iter>> ftq_iterators;
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