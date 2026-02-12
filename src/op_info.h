/* Copyright 2020 HPS/SAFARI Research Groups
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
 * File         : op_info.h
 * Author       : HPS Research Group
 * Date         : 2/19/2001
 * Description  :
 ***************************************************************************************/

#ifndef __OP_INFO_H__
#define __OP_INFO_H__

#include "globals/global_types.h"
#include "inst_info.h"
#include "table_info.h"

/**************************************************************************************/
// Defines

#define MAX_DEPS 128
#define MAX_OUTS 3

/**************************************************************************************/

typedef struct Generic_Op_Info_struct {
  Counter unique_num;
  Addr addr; /* pc */
  Op* op;
  Counter fetch_cycle;
} Generic_Op_Info;

typedef enum Dep_Type_enum {
  REG_DATA_DEP,
  MEM_ADDR_DEP,
  MEM_DATA_DEP,
  NUM_DEP_TYPES,
} Dep_Type;

typedef struct Src_Info_struct {
  Dep_Type type;
  struct Op_struct* op;
  Counter op_num;
  Counter unique_num;
  Quad val;
} Src_Info;

/**************************************************************************************/
/* The 'Op_Info' struct holds information that is unique to the
 * current instance of the instruction (data values, etc.)
 * typedef in globals/global_types.h */

typedef struct Bp_PredictResult_struct {
  // Core prediction results (from predictor)
  uns8 pred_dir;
  uns8 pred_orig;
  Addr pred_npc;
  Addr pred_addr;
  Flag recover_at_decode;
  Flag recover_at_exec;
  Flag btb_miss_nt;
  Flag local_btb_miss;
  Flag local_no_target;
  Flag local_misfetch;

  // Derived/evaluated results (after oracle comparison)
  Flag misfetch;
  Flag mispred;
  Flag btb_miss;
  Flag btb_miss_resolved;
  Flag no_target;
  Flag recovery_sch;
  Flag use_late_pred_for_ft;
  int8 off_path_reason;

  // Predictor state/history and confidence bookkeeping
  uns64 pred_perceptron_global_hist;
  uns64 pred_conf_perceptron_global_hist;
  uns64 pred_conf_perceptron_global_misp_hist;
  uns8* pred_gpht_entry;
  uns8* pred_ppht_entry;
  uns8* pred_spht_entry;
  uns32 pred_local_hist;
  uns32 pred_global_hist;
  uns32 pred_targ_hist;
  uns8 pred_tc_selector_entry;
  uns8 hybridgp_gpred;
  uns8 hybridgp_ppred;
  Flag ibp_miss;
  Flag pred_conf;
  Addr pred_conf_index;
} Bp_PredictResult;

typedef struct Btb_PredictResult_struct {
  Flag btb_miss;
  Flag btb_miss_nt;
  Flag btb_miss_resolved;
  Flag btb_miss_but_target_correct;
  Flag no_target;
  Flag ibp_miss;
  Flag btb_target_valid;
  Addr btb_target;
  Addr ibp_target;
  Addr pred_target;
} Btb_PredictResult;

struct Op_Info_struct {
  struct Table_Info_struct* table_info;  // copy of op->table_info
  struct Inst_Info_struct* inst_info;    // copy of op->inst_info

  uns num_srcs;                 // number of dependencies to obey
  Src_Info src_info[MAX_DEPS];  // information about each source
  Flag update_fpcr;             // need to update the fpcr
  UQuad new_fpcr;               // fpcr value resulting from this op

  // mem op fields
  Addr va;       // virtual address for memory instructions
  uns mem_size;  // memory data size now became dynamic property due to REP STRING

  // all op fields
  Addr npc;  // the true next pc after the instruction
  Addr pc_plus_offset;  // addr + inst size (fall-through)

  // control flow fields
  Addr target;             // decoded target of branch, set by oracle
  uns8 dir;                // true direction of branch, set by oracle
  Flag main_recover_at_decode;  // recovery-at-decode as set by the main predictor
  Flag main_recover_at_exec;    // recovery-at-exec as set by the main predictor
  // prediction state moved to Bp_PredictResult

  Flag dcmiss;  // dcache miss has occurred

  uns opc_index;

  Counter inst_sim_cycle;  // cycle oracle executes op

  Quad old_mem_value;
  Quad new_mem_value;
  Flag mlc_miss;            // is this op an MLC data miss?
  Flag mlc_miss_satisfied;  // mlc miss caused by this op is already satisfied
  Flag l1_miss;             // is this op an L1 data miss?
  Flag l1_miss_satisfied;   // l1 miss caused by this op is already satisfied
  Flag dep_on_l1_miss;      // op is waiting for an l1_miss to be satisfied
  Flag was_dep_on_l1_miss;  // op was waiting for an l1_miss to be satisfied, but not any more

  uns32 error_event;  // bit vector for the unexpected events generated by this op (error_event.h)
};

/**************************************************************************************/

#endif /* #ifndef __OP_INFO_H__ */
