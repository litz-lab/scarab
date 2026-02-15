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
 * File         : bp.c
 * Author       : HPS Research Group
 * Date         : 12/9/1998
 * Description  :
 ***************************************************************************************/

#include "bp/bp.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "bp/bp.param.h"
#include "core.param.h"
#include "prefetcher/pref.param.h"

#include "bp//bp_conf.h"
#include "bp/bp_targ_mech.h"
#include "bp/cbp_to_scarab.h"
#include "bp/gshare.h"
#include "bp/hybridgp.h"
#include "bp/tagescl.h"
#include "frontend/pin_trace_fe.h"
#include "isa/isa_macros.h"
#include "libs/cache_lib.h"
#include "prefetcher/branch_misprediction_table.h"
#include "prefetcher/fdip.h"

#include "decoupled_frontend.h"
#include "icache_stage.h"
#include "model.h"
#include "sim.h"
#include "statistics.h"
#include "thread.h"
#include "uop_cache.h"

/******************************************************************************/
/* include the table of possible branch predictors */

#include "bp/bp_table.def"

/******************************************************************************/
/* Collect stats for tcache */

extern void tc_do_stat(Op*, Flag);

/******************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_BP, ##args)
#define DEBUG_BTB(proc_id, args...) _DEBUG(proc_id, DEBUG_BTB, ##args)

/******************************************************************************/
/* Global Variables */

Bp_Recovery_Info* bp_recovery_info = NULL;
Bp_Data* g_bp_data = NULL;
extern List op_buf;
extern uns operating_mode;


/******************************************************************************/
// Local prototypes

/******************************************************************************/
/* set_bp_data set the global bp_data pointer (so I don't have to pass it around
 * everywhere */
void set_bp_data(Bp_Data* new_bp_data) {
  g_bp_data = new_bp_data;
}

/******************************************************************************/
/* set_bp_recovery_info: set the global bp_data pointer (so I don't have to pass
 * it around everywhere */
void set_bp_recovery_info(Bp_Recovery_Info* new_bp_recovery_info) {
  bp_recovery_info = new_bp_recovery_info;
}

/******************************************************************************/
/*  init_bp_recovery_info */

void init_bp_recovery_info(uns8 proc_id, Bp_Recovery_Info* new_bp_recovery_info) {
  ASSERT(proc_id, new_bp_recovery_info);
  memset(new_bp_recovery_info, 0, sizeof(Bp_Recovery_Info));

  new_bp_recovery_info->proc_id = proc_id;

  new_bp_recovery_info->recovery_cycle = MAX_CTR;
  new_bp_recovery_info->redirect_cycle = MAX_CTR;

  bp_recovery_info = new_bp_recovery_info;

}

/******************************************************************************/
/* bp_sched_recover: called on a mispredicted op when it's misprediction is
   first realized */

void bp_sched_recovery(Bp_Recovery_Info* bp_recovery_info, Op* op, Counter cycle) {
  ASSERT(op->proc_id, bp_recovery_info->proc_id == op->proc_id);
  ASSERT(0, !op->off_path);
  if (op->bp_pred_info->recover_at_exec) {
    INC_STAT_EVENT(0, SCHEDULED_EXEC_LAT, cycle_count - op->recovery_info.predict_cycle);
    STAT_EVENT(0, SCHEDULED_EXEC_RECOVERIES);
  } else if (op->bp_pred_info->recover_at_decode) {
    INC_STAT_EVENT(0, SCHEDULED_DECODE_LAT, cycle_count - op->recovery_info.predict_cycle);
    STAT_EVENT(0, SCHEDULED_DECODE_RECOVERIES);
  }

  if (bp_recovery_info->recovery_cycle == MAX_CTR || op->op_num <= bp_recovery_info->recovery_op_num) {
    const Addr next_fetch_addr = op->oracle_info.npc;
    ASSERT(0, op->oracle_info.npc);
    const uns latency = 1;
    DEBUG(bp_recovery_info->proc_id, "Recovery signaled for op_num:%s @ 0x%s  next_fetch:0x%s offpath:%d\n",
          unsstr64(op->op_num), hexstr64s(op->inst_info->addr), hexstr64s(next_fetch_addr), op->off_path);
    ASSERT(op->proc_id, !op->bp_pred_info->recovery_sch);
    op->bp_pred_info->recovery_sch = TRUE;
    bp_recovery_info->recovery_cycle = cycle + latency;
    bp_recovery_info->recovery_fetch_addr = next_fetch_addr;
    if (op->proc_id)
      ASSERT(op->proc_id, bp_recovery_info->recovery_fetch_addr);

    bp_recovery_info->recovery_op_num = op->op_num;
    bp_recovery_info->recovery_cf_type = op->table_info->cf_type;
    bp_recovery_info->recovery_info = op->recovery_info;
    bp_recovery_info->recovery_info.op_num = op->op_num;
    bp_recovery_info->recovery_inst_info = op->inst_info;
    bp_recovery_info->recovery_force_offpath = op->off_path;
    bp_recovery_info->recovery_op = op;
    bp_recovery_info->oracle_cp_num = op->oracle_cp_num;
    bp_recovery_info->recovery_unique_num = op->unique_num;
    bp_recovery_info->recovery_inst_uid = op->inst_uid;
    bp_recovery_info->wpe_flag = FALSE;
  }
}

/******************************************************************************/
/* bp_sched_redirect: called on an op that caused the fetch stage to suspend
   (eg. a btb miss).  The pred_npc is what is used for the new pc. */

void bp_sched_redirect(Bp_Recovery_Info* bp_recovery_info, Op* op, Counter cycle) {
  if (bp_recovery_info->redirect_cycle == MAX_CTR || op->op_num < bp_recovery_info->redirect_op_num) {
    DEBUG(bp_recovery_info->proc_id, "Redirect signaled for op_num:%s @ 0x%s\n", unsstr64(op->op_num),
          hexstr64s(op->inst_info->addr));

    bp_recovery_info->redirect_cycle = cycle + 1 + (op->table_info->cf_type == CF_SYS ? EXTRA_CALLSYS_CYCLES : 0);
    bp_recovery_info->redirect_op = op;
    bp_recovery_info->redirect_op_num = op->op_num;
    bp_recovery_info->redirect_op->redirect_scheduled = TRUE;
    ASSERT(bp_recovery_info->proc_id, bp_recovery_info->proc_id == op->proc_id);
    ASSERT_PROC_ID_IN_ADDR(op->proc_id, bp_recovery_info->redirect_op->bp_pred_info->pred_npc);
  }
  ASSERT(bp_recovery_info->proc_id, bp_recovery_info->proc_id == op->proc_id);
  ASSERT_PROC_ID_IN_ADDR(op->proc_id, bp_recovery_info->redirect_op->bp_pred_info->pred_npc);
}

/******************************************************************************/
/* init_bp:  initializes all branch prediction structures */

void init_bp_data(uns8 proc_id, uns8 bp_id, Bp_Data* bp_data, Bp_Data* primary_bp_data) {
  uns ii;
  if (SPEC_LEVEL)
    ASSERT(proc_id, BP_MECH == TAGE64K_BP);
  ASSERT(bp_data->proc_id, bp_data);
  memset(bp_data, 0, sizeof(Bp_Data));

  if (!bp_id) {
    bp_data->btb = (Cache*)malloc(sizeof(Cache));
    bp_data->tc_tagged = (Cache*)malloc(sizeof(Cache));
  }
  bp_data->proc_id = proc_id;
  bp_data->bp_id = bp_id;
  /* initialize branch predictor */
  bp_data->bp = &bp_table[BP_MECH];
  bp_data->bp->init_func();

  /* init btb structure */
  bp_data->bp_btb = &bp_btb_table[BTB_MECH];
  bp_data->bp_btb->init_func(bp_data, primary_bp_data);

  /* init call-return stack */
  bp_data->crs.entries = (Crs_Entry*)malloc(sizeof(Crs_Entry) * CRS_ENTRIES * 2);
  bp_data->crs.off_path = (Flag*)malloc(sizeof(Flag) * CRS_ENTRIES);
  for (ii = 0; ii < CRS_ENTRIES * 2; ii++) {
    bp_data->crs.entries[ii].addr = 0;
    bp_data->crs.entries[ii].op_num = 0;
    bp_data->crs.entries[ii].nos = 0;
  }
  for (ii = 0; ii < CRS_ENTRIES; ii++) {
    bp_data->crs.off_path[ii] = FALSE;
  }

  /* initialize the indirect target branch predictor */
  bp_data->bp_ibtb = &bp_ibtb_table[IBTB_MECH];
  bp_data->bp_ibtb->init_func(bp_data, primary_bp_data);
  bp_data->target_bit_length = IBTB_HIST_LENGTH / TARGETS_IN_HIST;
  if (!USE_PAT_HIST)
    ASSERTM(bp_data->proc_id, bp_data->target_bit_length * TARGETS_IN_HIST == IBTB_HIST_LENGTH,
            "IBTB_HIST_LENGTH must be a multiple of TARGETS_IN_HIST\n");

  g_bp_data = bp_data;

  /* confidence */
  if (ENABLE_BP_CONF) {
    bp_data->br_conf = &br_conf_table[CONF_MECH];
    bp_data->br_conf->init_func();
  }
}

Flag bp_is_predictable(Bp_Data* bp_data) {
  return !bp_data->bp->full_func(bp_data);
}

/******************************************************************************/
/* bp_predict_op:  predicts the target of a control flow instruction */

Addr bp_predict_op(Bp_Data* bp_data, Op* op, uns br_num, Addr fetch_addr) {
  Addr* btb_target;
  Addr ibp_target;
  Addr pred_target;
  Flag btb_miss_nt = FALSE;
  const Addr pc_plus_offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size);

  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  ASSERT(bp_data->proc_id, op->table_info->cf_type);

  /* set address used to predict branch */
  // op->bp_pred_info->pred_addr         = addr;
  op->bp_pred_info->pred_addr = op->inst_info->addr;
  op->btb_pred_info->btb_miss_resolved = FALSE;
  op->cf_within_fetch = br_num;

  /* initialize recovery information---this stuff might be
     overwritten by a prediction function that uses and
     speculatively updates global history */
  op->recovery_info.proc_id = op->proc_id;
  op->recovery_info.bp_id = op->bp_id;
  op->recovery_info.pred_global_hist = bp_data->global_hist;
  op->recovery_info.targ_hist = bp_data->targ_hist;
  op->recovery_info.new_dir = op->oracle_info.dir;
  op->recovery_info.crs_next = bp_data->crs.next;
  op->recovery_info.crs_tos = bp_data->crs.tos;
  op->recovery_info.crs_depth = bp_data->crs.depth;
  op->recovery_info.op_num = op->op_num;
  op->recovery_info.PC = op->inst_info->addr;
  op->recovery_info.cf_type = op->table_info->cf_type;
  op->recovery_info.oracle_dir = op->oracle_info.dir;
  op->recovery_info.branchTarget = op->oracle_info.target;
  op->recovery_info.predict_cycle = cycle_count;

  bp_data->bp->timestamp_func(op);

  if (BP_HASH_TOS || IBTB_HASH_TOS) {
    Addr tos_addr;
    uns new_next = CIRC_DEC2(bp_data->crs.next, CRS_ENTRIES);
    uns new_tail = CIRC_DEC2(bp_data->crs.tail, CRS_ENTRIES);
    Flag flag = bp_data->crs.off_path[new_tail];
    switch (CRS_REALISTIC) {
      case 0:
        tos_addr = bp_data->crs.entries[new_tail << 1 | flag].addr;
        break;
      case 1:
        tos_addr = bp_data->crs.entries[bp_data->crs.tos].addr;
        break;
      case 2:
        tos_addr = bp_data->crs.entries[new_next].addr;
        break;
      default:
        tos_addr = 0;
        break;
    }
    op->recovery_info.tos_addr = tos_addr;
  }

  // {{{ special case--system calls
  if (op->table_info->cf_type == CF_SYS) {
    op->bp_pred_info->pred = TAKEN;
    op->bp_pred_info->misfetch = FALSE;
    op->bp_pred_info->mispred = FALSE;
    op->btb_pred_info->btb_miss = FALSE;
    op->btb_pred_info->no_target = FALSE;
    // Syscalls cause flush of later ops at decode
    op->bp_pred_info->recover_at_decode = TRUE;
    op->bp_pred_info->recover_at_exec = FALSE;
    ASSERT_PROC_ID_IN_ADDR(op->proc_id, op->oracle_info.npc);
    op->bp_pred_info->pred_npc = op->oracle_info.npc;
    op->btb_pred_info->pred_target = op->oracle_info.npc;
    bp_data->bp->spec_update_func(op);
    return op->oracle_info.npc;
  } else
    ASSERT(0, !(op->table_info->bar_type & BAR_FETCH));
  // }}}

  // {{{ access btb for branch information and target

  // we assume that some branch information is stored in the BTB.
  // In the event of a btb miss, the branch will predicted as
  // normal, but will incur the redirect penalty for missing in the
  // btb.  btb_miss and pred_target are set appropriately.
  op->btb_pred_info->no_target = TRUE;
  op->bp_pred_info->misfetch = FALSE;
  btb_target = bp_data->bp_btb->pred_func(bp_data, op);
  if (btb_target) {
    // btb hit
    op->btb_pred_info->btb_miss = FALSE;
    op->btb_pred_info->no_target = FALSE;
    pred_target = *btb_target;
    if (op->table_info->cf_type != CF_ICO && op->table_info->cf_type != CF_RET &&
        !(op->table_info->bar_type & BAR_FETCH)) {
      STAT_EVENT(op->proc_id, op->off_path ? BTB_CORRECT_OFF_PATH : BTB_CORRECT);
    }
  } else {
    // In case of BTB miss, execute fall-through
    pred_target = pc_plus_offset;
    if (op->table_info->cf_type != CF_ICO && op->table_info->cf_type != CF_RET &&
        !(op->table_info->bar_type & BAR_FETCH)) {
      STAT_EVENT(op->proc_id, op->off_path ? BTB_INCORRECT_OFF_PATH : BTB_INCORRECT);
    }

    // In the case where fall-through == branch target, ignore BTB miss
    // This almost never happes but if it does, without the fix below, it would cause
    // recovery where the recovery address is incorrect
    if (pc_plus_offset == op->oracle_info.target) {
      op->btb_pred_info->btb_miss = FALSE;
      op->btb_pred_info->no_target = FALSE;
      op->bp_pred_info->pred = TAKEN;
      btb_target = &pred_target;  // make !NULL
      if (op->table_info->cf_type != CF_ICO && op->table_info->cf_type != CF_RET &&
          !(op->table_info->bar_type & BAR_FETCH)) {
        STAT_EVENT(op->proc_id,
                   op->off_path ? BTB_INCORRECT_BUT_TARGET_CORRECT_OFF_PATH : BTB_INCORRECT_BUT_TARGET_CORRECT);
      }
    } else {
      // btb miss
      op->btb_pred_info->btb_miss = TRUE;
    }
  }
  // overwrite pred_target with indirect predictor
  if (ENABLE_IBP && (op->table_info->cf_type == CF_IBR || op->table_info->cf_type == CF_ICALL)) {
    ibp_target = bp_data->bp_ibtb->pred_func(bp_data, op);
    if (ibp_target) {
      pred_target = ibp_target;
      op->btb_pred_info->no_target = FALSE;
      op->btb_pred_info->ibp_miss = FALSE;
      STAT_EVENT(op->proc_id, op->off_path ? IBTB_CORRECT_OFF_PATH : IBTB_CORRECT);
    } else {
      op->btb_pred_info->ibp_miss = TRUE;
      STAT_EVENT(op->proc_id, op->off_path ? IBTB_INCORRECT_OFF_PATH : IBTB_INCORRECT);
    }
  }

  // }}}
  // {{{ handle predictions for individual cf types
  switch (op->table_info->cf_type) {
    case CF_BR:
      // BR will be predicted at decode, but fill in the info here
      op->bp_pred_info->pred_orig = TAKEN;
      // On BTB hit, ensure that target is correct (no aliasing or jitted code)
      if (btb_target && pred_target == op->oracle_info.npc) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred = TAKEN;
        op->bp_pred_info->pred_npc = pred_target;
        STAT_EVENT(op->proc_id, op->off_path ? BR_CORRECT_OFF_PATH : BR_CORRECT);
      } else {
        op->bp_pred_info->recover_at_decode = TRUE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred = NOT_TAKEN;
        op->bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT(op->proc_id, op->off_path ? BR_RECOVER_OFF_PATH : BR_RECOVER);
      }
      break;
    case CF_REP:
    case CF_CBR:
      // Branch predictors may use pred_global_hist as input.
      op->bp_pred_info->pred_global_hist = bp_data->global_hist;

      if (PERFECT_BP) {
        op->bp_pred_info->pred = op->oracle_info.dir;
        op->bp_pred_info->pred_orig = op->oracle_info.dir;
        op->btb_pred_info->no_target = FALSE;
      } else {
        ASSERT(op->proc_id, !PERFECT_NT_BTB);  // currently not supported
        op->bp_pred_info->pred = bp_data->bp->pred_func(op);
        op->bp_pred_info->pred_orig = op->bp_pred_info->pred;
      }
      // Update history used by the rest of Scarab.
      bp_data->global_hist = (bp_data->global_hist >> 1) | (op->bp_pred_info->pred << 31);

      if (op->btb_pred_info->btb_miss && op->bp_pred_info->pred == NOT_TAKEN)
        btb_miss_nt = TRUE;

      if (PERFECT_CBR_BTB || (PERFECT_NT_BTB && op->bp_pred_info->pred == NOT_TAKEN)) {
        pred_target = op->oracle_info.target;
        op->btb_pred_info->btb_miss = FALSE;
        op->btb_pred_info->no_target = FALSE;
      }

      // pred_target is set by BTB on hit. For CBR we may however, still want to execute fall-through
      if (op->bp_pred_info->pred == NOT_TAKEN) {
        pred_target = pc_plus_offset;
      }

      // Regular mispredict resolved at exec
      // On dir misprediction, treat as correctly predicted if fall-through happens to match target
      if (btb_target && op->oracle_info.dir != op->bp_pred_info->pred && pc_plus_offset != op->oracle_info.target) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = TRUE;
        op->bp_pred_info->pred_npc = pred_target;

        if (op->bp_pred_info->pred == TAKEN)
          ASSERT(0, pred_target != pc_plus_offset);
        if (op->bp_pred_info->pred == NOT_TAKEN)
          ASSERT(0, pred_target == pc_plus_offset);

        STAT_EVENT(op->proc_id, op->off_path ? CBR_RECOVER_MISPREDICT_OFF_PATH : CBR_RECOVER_MISPREDICT);
      }
      // Although the btb hits and cbr is correctly predicted, target address may be wrong (aliasing or jitted code)
      else if (btb_target && pred_target != op->oracle_info.npc) {
        op->bp_pred_info->recover_at_decode = TRUE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred_npc = pred_target;
        STAT_EVENT(op->proc_id, op->off_path ? CBR_RECOVER_MISFETCH_OFF_PATH : CBR_RECOVER_MISFETCH);
      }
      // Correctly predicted
      else if (btb_target) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred_npc = pred_target;
        STAT_EVENT(op->proc_id, op->off_path ? CBR_CORRECT_OFF_PATH : CBR_CORRECT);
      }
      // If BTB missed, the branch will be assumed not taken at fetch. At decode we detect
      // the branch and will predict. There are 4 outcomes:
      // 1. Branch is predicted taken, violating not-taken assumption, causing flush at decode
      else if (!btb_target && op->bp_pred_info->pred == TAKEN && op->oracle_info.dir == TAKEN) {
        op->bp_pred_info->recover_at_decode = TRUE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred = NOT_TAKEN;
        op->bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT(op->proc_id, op->off_path ? CBR_RECOVER_BTB_MISS_T_T_OFF_PATH : CBR_RECOVER_BTB_MISS_T_T);
      }
      // 2. Branch is predicted taken, violating not-taken asumption. This would flush at decode,
      // however, the branch will flush again at exec when it is determined that the prediction was wrong
      // Scarab does not support flushing twice per op. Flushing at exec should not introduce inaccuracy.
      else if (!btb_target && op->bp_pred_info->pred == TAKEN && op->oracle_info.dir == NOT_TAKEN) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = TRUE;
        op->bp_pred_info->pred = NOT_TAKEN;
        op->bp_pred_info->pred_npc = pred_target;  // Not accurate. At fetch it would execute pc_plus_offset, at decode
                                                   // would resteer frontend to pred_taken
        STAT_EVENT(op->proc_id, op->off_path ? CBR_RECOVER_BTB_MISS_T_NT_OFF_PATH : CBR_RECOVER_BTB_MISS_T_NT);
      }
      // 3. Branch is predicted not-taken causing branch to continue to exec where the flush is triggered
      else if (!btb_target && op->bp_pred_info->pred == NOT_TAKEN && op->oracle_info.dir == TAKEN) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = TRUE;
        op->bp_pred_info->pred = NOT_TAKEN;
        op->bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT(op->proc_id, op->off_path ? CBR_RECOVER_BTB_MISS_NT_T_OFF_PATH : CBR_RECOVER_BTB_MISS_NT_T);
      }
      // 4. Branch is predicted not-taken which is correct causing no flush
      else if (!btb_target && op->bp_pred_info->pred == NOT_TAKEN && op->oracle_info.dir == NOT_TAKEN) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred = NOT_TAKEN;
        op->bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT(op->proc_id, op->off_path ? CBR_CORRECT_BTB_MISS_NT_NT_OFF_PATH : CBR_CORRECT_BTB_MISS_NT_NT);
      } else {
        // We should have matched all cases by here
        ASSERT(op->proc_id, 0);
      }
      break;

    case CF_CALL:
      op->bp_pred_info->pred = TAKEN;
      op->bp_pred_info->pred_orig = TAKEN;
      if (ENABLE_CRS)
        CRS_REALISTIC ? bp_crs_realistic_push(bp_data, op) : bp_crs_push(bp_data, op);
      // On BTB hit, ensure that target is correct (no aliasing or jitted code)
      if (btb_target && pred_target == op->oracle_info.npc) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred = TAKEN;
        op->bp_pred_info->pred_npc = pred_target;
        DEBUG(bp_data->proc_id,
              "no flush BP:  op_num:%s  off_path:%d  cf_type:%s  addr:%s  p_npc:%s  "
              "t_npc:0x%s  btb_miss:%d  mispred:%d  misfetch:%d  no_tar:%d\n",
              unsstr64(op->op_num), op->off_path, cf_type_names[op->table_info->cf_type],
              hexstr64s(op->inst_info->addr), hexstr64s(op->bp_pred_info->pred_npc), hexstr64s(op->oracle_info.npc),
              op->btb_pred_info->btb_miss, op->bp_pred_info->mispred, op->bp_pred_info->recover_at_exec,
              op->bp_pred_info->recover_at_decode);

        ASSERT(0, op->bp_pred_info->pred == op->oracle_info.dir);
        STAT_EVENT(op->proc_id, op->off_path ? CALL_CORRECT_OFF_PATH : CALL_CORRECT);
      } else {
        DEBUG(bp_data->proc_id,
              "flush BP:  op_num:%s  off_path:%d  cf_type:%s  addr:%s  p_npc:%s  "
              "t_npc:0x%s  btb_miss:%d  mispred:%d  misfetch:%d  no_tar:%d predtarg %llx npc %llx\n",
              unsstr64(op->op_num), op->off_path, cf_type_names[op->table_info->cf_type],
              hexstr64s(op->inst_info->addr), hexstr64s(op->bp_pred_info->pred_npc), hexstr64s(op->oracle_info.npc),
              op->btb_pred_info->btb_miss, op->bp_pred_info->mispred, op->bp_pred_info->recover_at_exec,
              op->bp_pred_info->recover_at_decode, pred_target, op->oracle_info.npc);

        op->bp_pred_info->recover_at_decode = TRUE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred = NOT_TAKEN;
        op->bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT(op->proc_id, op->off_path ? CALL_RECOVER_OFF_PATH : CALL_RECOVER);
      }
      break;

    case CF_IBR:
      if (PERFECT_BP) {
        op->bp_pred_info->pred = op->oracle_info.dir;
        op->bp_pred_info->pred_orig = op->oracle_info.dir;
      } else {
        op->bp_pred_info->pred = TAKEN;
        op->bp_pred_info->pred_orig = TAKEN;
      }
      if (ENABLE_IBP && ibp_target) {
        ASSERT(op->proc_id, op->oracle_info.target == op->oracle_info.npc);
        if (op->oracle_info.target == pred_target) {
          op->bp_pred_info->recover_at_decode = FALSE;
          op->bp_pred_info->recover_at_exec = FALSE;
          op->bp_pred_info->pred_npc = pred_target;
          STAT_EVENT(op->proc_id, op->off_path ? IBR_CORRECT_IBTB_OFF_PATH : IBR_CORRECT_IBTB);
        } else {
          op->bp_pred_info->recover_at_decode = FALSE;
          op->bp_pred_info->recover_at_exec = TRUE;
          op->bp_pred_info->pred_npc = pred_target;
          STAT_EVENT(op->proc_id, op->off_path ? IBR_RECOVER_IBTB_MISFETCH_OFF_PATH : IBR_RECOVER_IBTB_MISFETCH);
        }
      } else if (btb_target) {
        if (op->oracle_info.target == pred_target) {
          op->bp_pred_info->recover_at_decode = FALSE;
          op->bp_pred_info->recover_at_exec = FALSE;
          op->bp_pred_info->pred_npc = pred_target;
          STAT_EVENT(op->proc_id, op->off_path ? IBR_CORRECT_BTB_OFF_PATH : IBR_CORRECT_BTB);
        } else {
          op->bp_pred_info->recover_at_decode = FALSE;
          op->bp_pred_info->recover_at_exec = TRUE;
          op->bp_pred_info->pred_npc = pred_target;
          op->bp_pred_info->misfetch = TRUE;
          STAT_EVENT(op->proc_id, op->off_path ? IBR_RECOVER_BTB_MISFETCH_OFF_PATH : IBR_RECOVER_BTB_MISFETCH);
        }
      }
      // If BTB and iBTB miss we can detect the mispredition at decode but we need to wait
      // until exec to resolve the branch target. We would not know which target to fetch
      // at decode so we can just recover at exec
      else {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = TRUE;
        op->bp_pred_info->pred = NOT_TAKEN;
        op->bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT(op->proc_id, op->off_path ? IBR_RECOVER_XBTB_MISS_OFF_PATH : IBR_RECOVER_XBTB_MISS);
      }

      break;

    case CF_ICALL:
      if (PERFECT_BP) {
        op->bp_pred_info->pred = op->oracle_info.dir;
        op->bp_pred_info->pred_orig = op->oracle_info.dir;
      } else {
        op->bp_pred_info->pred = TAKEN;
        op->bp_pred_info->pred_orig = TAKEN;
      }
      if (ENABLE_CRS)
        CRS_REALISTIC ? bp_crs_realistic_push(bp_data, op) : bp_crs_push(bp_data, op);

      if (ENABLE_IBP && ibp_target) {
        ASSERT(op->proc_id, op->oracle_info.target == op->oracle_info.npc);
        if (op->oracle_info.target == pred_target) {
          op->bp_pred_info->recover_at_decode = FALSE;
          op->bp_pred_info->recover_at_exec = FALSE;
          op->bp_pred_info->pred_npc = pred_target;
          STAT_EVENT(op->proc_id, op->off_path ? ICALL_CORRECT_IBTB_OFF_PATH : ICALL_CORRECT_IBTB);
        } else {
          op->bp_pred_info->recover_at_decode = FALSE;
          op->bp_pred_info->recover_at_exec = TRUE;
          op->bp_pred_info->pred_npc = pred_target;
          op->bp_pred_info->misfetch = TRUE;
          STAT_EVENT(op->proc_id, op->off_path ? ICALL_RECOVER_IBTB_MISFETCH_OFF_PATH : ICALL_RECOVER_IBTB_MISFETCH);
        }
      } else if (btb_target) {
        if (op->oracle_info.target == pred_target) {
          op->bp_pred_info->recover_at_decode = FALSE;
          op->bp_pred_info->recover_at_exec = FALSE;
          op->bp_pred_info->pred_npc = pred_target;
          STAT_EVENT(op->proc_id, op->off_path ? ICALL_CORRECT_BTB_OFF_PATH : ICALL_CORRECT_BTB);
        } else {
          op->bp_pred_info->recover_at_decode = FALSE;
          op->bp_pred_info->recover_at_exec = TRUE;
          op->bp_pred_info->pred_npc = pred_target;
          STAT_EVENT(op->proc_id, op->off_path ? ICALL_RECOVER_BTB_MISFETCH_OFF_PATH : ICALL_RECOVER_BTB_MISFETCH);
        }
      }
      // If BTB and iBTB miss we can detect the mispredition at decode but we need to wait
      // until exec to resolve the branch target. We would not know which target to fetch
      // at decode so we can just recover at exec
      else {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = TRUE;
        op->bp_pred_info->pred = NOT_TAKEN;
        op->bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT(op->proc_id, op->off_path ? ICALL_RECOVER_XBTB_MISS_OFF_PATH : ICALL_RECOVER_XBTB_MISS);
      }

      break;

    case CF_ICO:
      op->bp_pred_info->pred = TAKEN;
      op->bp_pred_info->pred_orig = TAKEN;
      if (ENABLE_CRS) {
        pred_target = CRS_REALISTIC ? bp_crs_realistic_pop(bp_data, op) : bp_crs_pop(bp_data, op);
        CRS_REALISTIC ? bp_crs_realistic_push(bp_data, op) : bp_crs_push(bp_data, op);
      }

      if (pred_target != op->oracle_info.npc) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = TRUE;
        op->bp_pred_info->pred_npc = pred_target;
        STAT_EVENT(op->proc_id, op->off_path ? ICO_RECOVER_OFF_PATH : ICO_RECOVER);
      } else {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred = NOT_TAKEN;
        op->bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT(op->proc_id, op->off_path ? ICO_CORRECT_OFF_PATH : ICO_CORRECT);
      }

      break;

    case CF_RET:
      if (PERFECT_BP) {
        op->bp_pred_info->pred = op->oracle_info.dir;
        op->bp_pred_info->pred_orig = op->oracle_info.dir;
      } else {
        op->bp_pred_info->pred = TAKEN;
        op->bp_pred_info->pred_orig = TAKEN;
      }
      if (ENABLE_CRS)
        pred_target = CRS_REALISTIC ? bp_crs_realistic_pop(bp_data, op) : bp_crs_pop(bp_data, op);
      if (pred_target == 0) {  // RAS Underflow
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = TRUE;
        op->bp_pred_info->pred_npc = pc_plus_offset;
        op->bp_pred_info->pred = NOT_TAKEN;
        STAT_EVENT(op->proc_id, op->off_path ? RET_RECOVER_UFLOW_OFF_PATH : RET_RECOVER_UFLOW);
      } else if (pred_target != op->oracle_info.npc) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = TRUE;
        op->bp_pred_info->pred_npc = pred_target;
        STAT_EVENT(op->proc_id, op->off_path ? RET_RECOVER_OFF_PATH : RET_RECOVER);
      } else {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->pred_npc = pred_target;
        STAT_EVENT(op->proc_id, op->off_path ? RET_CORRECT_OFF_PATH : RET_CORRECT);
      }
      break;

    default:
      ASSERT(op->proc_id, 0);  // should not happen
      op->bp_pred_info->pred = TAKEN;
      op->bp_pred_info->pred_orig = TAKEN;
      break;
  }
  // }}}

  pred_target = convert_to_cmp_addr(op->proc_id, pred_target);
  op->btb_pred_info->pred_target = pred_target;
  if (op->btb_pred_info->btb_miss && op->bp_pred_info->pred == NOT_TAKEN)
    btb_miss_nt = TRUE;

  bp_data->bp->spec_update_func(op);

  DEBUG(bp_data->proc_id,
        "BP:  op_num:%s  off_path:%d  cf_type:%s  addr:%s  p_npc:%s  "
        "t_npc:0x%s  btb_miss:%d  mispred:%d  misfetch:%d  no_tar:%d dir%d pred%d offset %llx target %llx\n",
        unsstr64(op->op_num), op->off_path, cf_type_names[op->table_info->cf_type], hexstr64s(op->inst_info->addr),
        hexstr64s(op->bp_pred_info->pred_npc), hexstr64s(op->oracle_info.npc), op->btb_pred_info->btb_miss,
        op->bp_pred_info->mispred, op->bp_pred_info->recover_at_exec, op->bp_pred_info->recover_at_decode,
        op->oracle_info.dir, op->bp_pred_info->pred, pc_plus_offset, op->oracle_info.target);

  ASSERT(op->proc_id, op->bp_pred_info->pred_npc);
  if (op->oracle_info.dir != op->bp_pred_info->pred && pc_plus_offset != op->oracle_info.target) {
    if (!(op->bp_pred_info->recover_at_exec || op->bp_pred_info->recover_at_decode))
      ASSERT(op->proc_id, op->bp_pred_info->recover_at_exec || op->bp_pred_info->recover_at_decode);
  }

  ASSERT_PROC_ID_IN_ADDR(op->proc_id, op->bp_pred_info->pred_npc);
  bp_predict_op_evaluate(bp_data, op, op->bp_pred_info->pred_npc);

  // The case where BTB-miss not-taken branch pollute global hist
  // mispred || misfetch will trigger a re-steer but no chance to fix the global hist
  if (btb_miss_nt &&
      (((op->bp_pred_info->pred != op->oracle_info.dir) && (op->bp_pred_info->pred_npc != op->oracle_info.npc)) ||
       (!op->bp_pred_info->mispred && op->bp_pred_info->pred_npc != op->oracle_info.npc)))
    STAT_EVENT(op->proc_id, FDIP_BTB_MISS_NT_RESTEER_ONPATH + op->off_path);

  if (!op->off_path) {
    if (op->bp_pred_info->recover_at_exec)
      STAT_EVENT(0, BP_EXEC_RECOVERIES);
    else if (op->bp_pred_info->recover_at_decode)
      STAT_EVENT(0, BP_DECODE_RECOVERIES);
  }
  return op->bp_pred_info->pred_npc;
}

/* Separate performing branch prediction from evaluating the prediction into
 * two functions, enabling FDIP.
 */
Addr bp_predict_op_evaluate(Bp_Data* bp_data, Op* op, Addr prediction) {
  // If the direction prediction is wrong, but next address happens to be right
  // anyway, do not treat this as a misprediction.
  op->bp_pred_info->mispred = (op->bp_pred_info->pred != op->oracle_info.dir) && (prediction != op->oracle_info.npc);
  op->bp_pred_info->misfetch = !op->bp_pred_info->mispred && prediction != op->oracle_info.npc;

  op->bp_cycle = cycle_count;

  if (!op->off_path) {
    if (op->bp_pred_info->mispred)
      td->td_info.mispred_counter++;
    else
      td->td_info.corrpred_counter++;
  }

  if (op->table_info->cf_type == CF_CBR || op->table_info->cf_type == CF_REP) {
    if (!op->off_path) {
      if (op->bp_pred_info->mispred)
        _DEBUGA(op->proc_id, 0, "ON PATH HW MISPRED  addr:0x%s  pghist:0x%s\n", hexstr64s(op->inst_info->addr),
                hexstr64s(op->bp_pred_info->pred_global_hist));
      else
        _DEBUGA(op->proc_id, 0, "ON PATH HW CORRECT  addr:0x%s  pghist:0x%s\n", hexstr64s(op->inst_info->addr),
                hexstr64s(op->bp_pred_info->pred_global_hist));
    }
  }

  DEBUG_BTB(bp_data->proc_id, "BTB:  op_num:%s  off_path:%d  cf_type:%s  addr:0x%s  btb_miss:%d\n",
            unsstr64(op->op_num), op->off_path, cf_type_names[op->table_info->cf_type], hexstr64s(op->inst_info->addr),
            op->btb_pred_info->btb_miss);

  DEBUG(bp_data->proc_id,
        "BP:  op_num:%s  off_path:%d  cf_type:%s  addr:%s  p_npc:%s  "
        "t_npc:0x%s  btb_miss:%d  mispred:%d  misfetch:%d  no_tar:%d\n",
        unsstr64(op->op_num), op->off_path, cf_type_names[op->table_info->cf_type], hexstr64s(op->inst_info->addr),
        hexstr64s(prediction), hexstr64s(op->oracle_info.npc), op->btb_pred_info->btb_miss, op->bp_pred_info->mispred,
        op->bp_pred_info->misfetch, op->btb_pred_info->no_target);

  if (ENABLE_BP_CONF && IS_CONF_CF(op)) {
    bp_data->br_conf->pred_func(op);

    if (!(op->bp_pred_info->pred_conf))
      td->td_info.low_conf_count++;
    DEBUG(bp_data->proc_id, "low_conf_count:%d \n", td->td_info.low_conf_count);
  }

  return prediction;
}

/******************************************************************************/
/* bp_target_known_op: called on cf ops when the real target is known
   (either decode time or execute time) */

void bp_target_known_op(Bp_Data* bp_data, Op* op) {
  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  ASSERT(bp_data->proc_id, op->table_info->cf_type);

  // if it was a btb miss, it is time to write it into the btb
  if (op->btb_pred_info->btb_miss && op->oracle_info.dir == TAKEN) {
    bp_data->bp_btb->update_func(bp_data, op);
  } else if (op->btb_pred_info->btb_miss == FALSE && op->oracle_info.dir == TAKEN) {
    // For jitted CF we want to update the BTB if the target changes, even on btb hit
    // or For indirects we want to update the BTB if the target changes, even on btb hit
    // The detection relies on the target stored in the btb
    Addr line_addr;
    Addr* btb_entry = (Addr*)cache_access(bp_data->btb, op->bp_pred_info->pred_addr, &line_addr, FALSE);
    // The following assertion can fail (due to eviction?)
    // ASSERT(bp_data->proc_id, btb_entry);
    if (btb_entry && *btb_entry != op->oracle_info.target) {
      bp_data->bp_btb->update_func(bp_data, op);
      STAT_EVENT(bp_data->proc_id, BTB_UPDATE_BTB_HIT_JITTED_NOT_CF + op->table_info->cf_type);
    }
  }

  // special case updates
  switch (op->table_info->cf_type) {
    case CF_ICALL:  // fall through
    case CF_IBR:
      if (ENABLE_IBP) {
        if (IBTB_OFF_PATH_WRITES || !op->off_path) {
          bp_data->bp_ibtb->update_func(bp_data, op);
        }
      }
      break;
    default:
      break;  // do nothing
  }
}

/******************************************************************************/
/* bp_resolve_op: called on cf ops when they complete in the functional unit */

void bp_resolve_op(Bp_Data* bp_data, Op* op) {
  if (!UPDATE_BP_OFF_PATH && op->off_path) {
    return;
  }
  bp_data->bp->update_func(op);

  if (ENABLE_BP_CONF && IS_CONF_CF(op)) {
    bp_data->br_conf->update_func(op);
  }
  if (CONFIDENCE_ENABLE)
    decoupled_fe_conf_resovle_cf(op);
}

/******************************************************************************/
/* bp_retire_op: called to update critical branch predictor state that should
 * only be updated on the right path and retire the timestamp of the branch.
 */

void bp_retire_op(Bp_Data* bp_data, Op* op) {
  bp_data->bp->retire_func(op);
}

/******************************************************************************/
/* bp_recover_op: called on the last mispredicted op when the recovery happens
 */

void bp_recover_op(Bp_Data* bp_data, Cf_Type cf_type, Recovery_Info* info) {
  STAT_EVENT(0, PERFORMED_EXEC_RECOVERIES);
  INC_STAT_EVENT(0, PERFORMED_RECOVERY_LAT, cycle_count - info->predict_cycle);
  /* always recover the global history */
  if (cf_type == CF_CBR || cf_type == CF_REP) {
    bp_data->global_hist = (info->pred_global_hist >> 1) | (info->new_dir << 31);
  } else {
    bp_data->global_hist = info->pred_global_hist;
  }
  bp_data->targ_hist = info->targ_hist;

  /* this event counts updates to BP, so it's really branch resolutions */
  STAT_EVENT(bp_data->proc_id, POWER_BRANCH_MISPREDICT);
  STAT_EVENT(bp_data->proc_id, POWER_BTB_WRITE);

  /* type-specific recovery */
  if (cf_type == CF_ICALL || cf_type == CF_IBR) {
    bp_data->bp_ibtb->recover_func(bp_data, info);
  }
  bp_data->bp->recover_func(info);

  /* always recover the call return stack */
  CRS_REALISTIC ? bp_crs_realistic_recover(bp_data, info) : bp_crs_recover(bp_data);

  if (ENABLE_BP_CONF && bp_data->br_conf->recover_func)
    bp_data->br_conf->recover_func();

  if (FDIP_DUAL_PATH_PREF_UOC_ONLINE_ENABLE)
    increment_branch_mispredictions(info->PC);
}

void bp_sync(Bp_Data* bp_data_src, Bp_Data* bp_data_dst) {
  bp_data_dst->global_hist = bp_data_src->global_hist;
  bp_data_dst->targ_hist = bp_data_src->targ_hist;
  bp_data_dst->targ_index = bp_data_src->targ_index;
  bp_data_dst->target_bit_length = bp_data_src->target_bit_length;
  bp_data_dst->on_path_pred = bp_data_src->on_path_pred;
  bp_crs_sync(bp_data_src, bp_data_dst);
  bp_predictors_sync(bp_data_src, bp_data_dst);
}
