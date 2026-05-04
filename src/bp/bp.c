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
#include "bp/bimodal.h"
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

#define STAT_EVENT_BP(stat)                       \
  do {                                            \
    if (bp_id == MAIN_BP)                         \
      STAT_EVENT((op)->proc_id, stat);            \
    else                                          \
      ALT_STAT_EVENT((op)->proc_id, bp_id, stat); \
  } while (0)

#define STAT_EVENT_BP_SPLIT_PATH(op, stat)                                 \
  do {                                                                     \
    if ((pred_level) == BP_PRED_L0) {                                      \
      STAT_EVENT_BP((op)->off_path ? L0_##stat##_OFFPATH : L0_##stat);     \
    } else {                                                               \
      STAT_EVENT_BP((op)->off_path ? MAIN_##stat##_OFFPATH : MAIN_##stat); \
    }                                                                      \
  } while (0)

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
  if (op->bp_pred_info->recover_at_fe) {
    INC_STAT_EVENT(op->proc_id, SCHEDULED_L0_EARLY_LAT, cycle_count - op->recovery_info.predict_cycle);
    STAT_EVENT(op->proc_id, SCHEDULED_L0_EARLY_RECOVERIES);
  } else if (op->bp_pred_info->recover_at_exec) {
    INC_STAT_EVENT(op->proc_id, SCHEDULED_MAIN_EXEC_LAT, cycle_count - op->recovery_info.predict_cycle);
    STAT_EVENT(op->proc_id, SCHEDULED_MAIN_EXEC_RECOVERIES);
  } else if (op->bp_pred_info->recover_at_decode) {
    INC_STAT_EVENT(op->proc_id, SCHEDULED_MAIN_DECODE_LAT, cycle_count - op->recovery_info.predict_cycle);
    STAT_EVENT(op->proc_id, SCHEDULED_MAIN_DECODE_RECOVERIES);
  }

  if (bp_recovery_info->recovery_cycle == MAX_CTR || op->op_num <= bp_recovery_info->recovery_op_num) {
    const Addr next_fetch_addr = op->oracle_info.npc;
    ASSERT(0, op->oracle_info.npc);
    const uns latency = 1;
    ASSERT(op->proc_id, !op->bp_pred_info->recovery_sch);
    op->bp_pred_info->recovery_sch = TRUE;
    bp_recovery_info->recovery_cycle = cycle + latency;
    bp_recovery_info->recovery_fetch_addr = next_fetch_addr;
    if (op->proc_id)
      ASSERT(op->proc_id, bp_recovery_info->recovery_fetch_addr);

    bp_recovery_info->recovery_op_num = op->op_num;
    bp_recovery_info->recovery_cf_type = op->inst_info->table_info.cf_type;
    bp_recovery_info->recovery_info = op->recovery_info;
    bp_recovery_info->recovery_info.op_num = op->op_num;
    bp_recovery_info->recovery_inst_info = op->inst_info;
    bp_recovery_info->recovery_force_offpath = op->off_path;
    bp_recovery_info->recovery_op = op;
    bp_recovery_info->recovery_unique_num = op->unique_num;
    bp_recovery_info->recovery_inst_uid = op->inst_uid;
    bp_recovery_info->wpe_flag = FALSE;
    DEBUG(bp_recovery_info->proc_id,
          "Recovery scheduled op_num:%s @ 0x%s next_fetch:0x%s offpath:%d recovery_cycle:%s (now:%s)\n",
          unsstr64(op->op_num), hexstr64s(op->inst_info->addr), hexstr64s(next_fetch_addr), op->off_path,
          unsstr64(bp_recovery_info->recovery_cycle), unsstr64(cycle_count));
  }
}

/******************************************************************************/
/* bp_sched_redirect: called on an op that caused the fetch stage to suspend
   (eg. a btb miss).  The pred_npc is what is used for the new pc. */

void bp_sched_redirect(Bp_Recovery_Info* bp_recovery_info, Op* op, Counter cycle) {
  if (bp_recovery_info->redirect_cycle == MAX_CTR || op->op_num < bp_recovery_info->redirect_op_num) {
    DEBUG(bp_recovery_info->proc_id, "Redirect signaled for op_num:%s @ 0x%s\n", unsstr64(op->op_num),
          hexstr64s(op->inst_info->addr));

    bp_recovery_info->redirect_cycle =
        cycle + 1 + (op->inst_info->table_info.cf_type == CF_SYS ? EXTRA_CALLSYS_CYCLES : 0);
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
/* bp_stat_main_branch_resolve_latency: called when decode/exec resolves a MAIN
 * prediction that requires frontend recovery. */

void bp_stat_main_branch_resolve_latency(Op* op, Counter resolve_cycle, Flag recover_at_exec) {
  if (op->bp_pred_info != &op->bp_pred_main)
    return;
  if (!(op->bp_pred_main.recover_at_decode || op->bp_pred_main.recover_at_exec))
    return;

  ASSERT(op->proc_id, op->bp_cycle != MAX_CTR);
  Counter latency = resolve_cycle > op->bp_cycle ? resolve_cycle - op->bp_cycle : 1;
  uns bucket = latency > 100 ? 100 : (uns)(latency - 1);
  STAT_EVENT(op->proc_id,
             (recover_at_exec ? MAIN_BRANCH_RESOLVE_AT_EXEC_LAT_1 : MAIN_BRANCH_RESOLVE_AT_DECODE_LAT_1) + bucket);
}

/******************************************************************************/
/* init_bp:  initializes all branch prediction structures */

void init_bp_data(uns8 proc_id, uns8 bp_id, Bp_Data* bp_data, Bp_Data* primary_bp_data) {
  uns ii;
  if (SPEC_LEVEL)
    ASSERTM(proc_id, BP_MECH == TAGE64K_BP || BP_MECH == BIMODAL_BP,
            "SPEC_LEVEL currently supports BP_MECH=tage64k or bp_mech=bimodal\n");
  ASSERTM(proc_id, MAIN_ALL_PREDICTIONS == L0_ALL_PREDICTIONS + 1,
          "BP level stats must be contiguous: {L0,MAIN}_ALL_PREDICTIONS\n");
  ASSERTM(proc_id, MAIN_ALL_RECOVER_AT_EXEC == L0_ALL_RECOVER_AT_EXEC + 1,
          "BP level stats must be contiguous: {L0,MAIN}_ALL_RECOVER_AT_EXEC\n");
  ASSERTM(proc_id, MAIN_ALL_RECOVER_AT_DECODE == L0_ALL_RECOVER_AT_DECODE + 1,
          "BP level stats must be contiguous: {L0,MAIN}_ALL_RECOVER_AT_DECODE\n");
  ASSERTM(proc_id, MAIN_ALL_PREDICTIONS_OFFPATH == L0_ALL_PREDICTIONS_OFFPATH + 1,
          "BP level stats must be contiguous: {L0,MAIN}_ALL_PREDICTIONS_OFFPATH\n");
  ASSERTM(proc_id, MAIN_ALL_RECOVER_AT_EXEC_OFFPATH == L0_ALL_RECOVER_AT_EXEC_OFFPATH + 1,
          "BP level stats must be contiguous: {L0,MAIN}_ALL_RECOVER_AT_EXEC_OFFPATH\n");
  ASSERTM(proc_id, MAIN_ALL_RECOVER_AT_DECODE_OFFPATH == L0_ALL_RECOVER_AT_DECODE_OFFPATH + 1,
          "BP level stats must be contiguous: {L0,MAIN}_ALL_RECOVER_AT_DECODE_OFFPATH\n");
  ASSERT(bp_data->proc_id, bp_data);
  memset(bp_data, 0, sizeof(Bp_Data));

  if (!bp_id) {
    if (bp_l0_enabled()) {
      ASSERTM(proc_id, BP_MECH_L0 == BIMODAL_BP,
              "BP_MECH_L0 must be bimodal when L0 is enabled as the other BPs do not support off-path prediction\n");
      ASSERTM(proc_id, BP_L0_LATENCY == 1, "BP_L0_LATENCY must be 1 when L0 is enabled\n");
      ASSERTM(proc_id, BP_MAIN_LATENCY > 1, "BP_MAIN_LATENCY must be > 1 when L0 is enabled\n");
      ASSERTM(proc_id, BP_MAIN_LATENCY < DECODE_CYCLES, "BP_MAIN_LATENCY must be < DECODE_CYCLES\n");
      // To support a last-level BTB latency greater than BP_MAIN_LATENCY, add
      // a frontend recovery case for main-correct predictions with a late BTB target.
      ASSERTM(proc_id, BTB_MAIN_LATENCY <= BP_MAIN_LATENCY,
              "BTB_MAIN_LATENCY must be <= BP_MAIN_LATENCY when L0 is enabled\n");
    } else {
      ASSERTM(proc_id, BP_MAIN_LATENCY == 1, "BP_MAIN_LATENCY must be 1 when early predictor is disabled\n");
    }
    bp_data->btb = (Cache*)malloc(sizeof(Cache));
    if (BTB_L0_PRESENT)
      bp_data->btb_l0 = (Cache*)malloc(sizeof(Cache));
    if (BTB_L1_PRESENT)
      bp_data->btb_l1 = (Cache*)malloc(sizeof(Cache));
    bp_data->tc_tagged = (Cache*)malloc(sizeof(Cache));
  }
  bp_data->proc_id = proc_id;
  bp_data->bp_id = bp_id;
  /* initialize branch predictor */
  bp_data->bp = &bp_table[BP_MECH];
  bp_data->bp->init_func();
  if (bp_l0_enabled()) {
    bp_data->bp_l0 = &bp_table[BP_MECH_L0];
    bp_data->bp_l0->init_func();
  } else {
    bp_data->bp_l0 = NULL;
  }

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

static Addr bp_predict_op_impl(Bp_Data* bp_data, Op* op, uns bp_id, uns br_num, Addr fetch_addr,
                               Bp_Pred_Level pred_level) {
  Bp_Pred_Info* bp_pred_info = (pred_level == BP_PRED_L0) ? &op->bp_pred_l0 : &op->bp_pred_main;
  Bp* pred_bp = (pred_level == BP_PRED_L0) ? bp_data->bp_l0 : bp_data->bp;
  Addr pred_target;
  Flag btb_miss_nt = FALSE;
  const Addr pc_plus_offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size);

  (void)br_num;

  ASSERT(bp_data->proc_id, bp_id < MAX_NUM_BPS);
  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  ASSERT(bp_data->proc_id, op->inst_info->table_info.cf_type);
  ASSERT(bp_data->proc_id, op->btb_pred_info);  // must have been set by bp_predict_btb()

  /* initialize recovery information---this stuff might be
     overwritten by a prediction function that uses and
     speculatively updates global history */
  op->recovery_info.proc_id = op->proc_id;
  op->recovery_info.bp_id = bp_id;
  op->recovery_info.pred_global_hist = bp_data->global_hist;
  op->recovery_info.targ_hist = bp_data->targ_hist;
  op->recovery_info.new_dir = op->oracle_info.dir;
  op->recovery_info.crs_next = bp_data->crs.next;
  op->recovery_info.crs_tos = bp_data->crs.tos;
  op->recovery_info.crs_depth = bp_data->crs.depth;
  op->recovery_info.op_num = op->op_num;
  op->recovery_info.PC = op->inst_info->addr;
  op->recovery_info.op = op;
  op->recovery_info.cf_type = op->inst_info->table_info.cf_type;
  op->recovery_info.oracle_dir = op->oracle_info.dir;
  op->recovery_info.branchTarget = op->oracle_info.target;
  op->recovery_info.predict_cycle = cycle_count;

  pred_bp->timestamp_func(op);
  bp_pred_info->pred_branch_id = op->recovery_info.branch_id;
  bp_pred_info->bp_ready_cycle = cycle_count + (pred_level == BP_PRED_L0 ? BP_L0_LATENCY : BP_MAIN_LATENCY);

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
  if (op->inst_info->table_info.cf_type == CF_SYS) {
    bp_pred_info->pred = TAKEN;
    // Syscalls cause flush of later ops at decode
    bp_pred_info->recover_at_decode = TRUE;
    bp_pred_info->recover_at_exec = FALSE;
    ASSERT_PROC_ID_IN_ADDR(op->proc_id, op->oracle_info.npc);
    bp_pred_info->pred_npc = op->oracle_info.npc;
    pred_bp->spec_update_func(op, pred_level);
    return op->oracle_info.npc;
  } else
    ASSERT(0, !(op->inst_info->table_info.bar_type & BAR_FETCH));
  // }}}

  // {{{ read pre-computed BTB/IBP results from btb_pred_info

  // All BTB/IBP lookup results were computed once by bp_predict_btb() and
  // stored in op->btb_pred_info.  The BTB target is usable by this predictor
  // level only if the BTB level that supplied it is no slower than the BP.
  const uns bp_latency = pred_level == BP_PRED_L0 ? BP_L0_LATENCY : BP_MAIN_LATENCY;
  const Flag btb_hit = btb_pred_hit_by(op->btb_pred_info, bp_latency);
  if (btb_hit)
    pred_target = op->btb_pred_info->pred_target;
  else
    pred_target = pc_plus_offset;

  // }}}
  // {{{ handle predictions for individual cf types
  switch (op->inst_info->table_info.cf_type) {
    case CF_BR:
      // BR will be predicted at decode, but fill in the info here
      bp_pred_info->pred_orig = TAKEN;
      // On BTB hit, ensure that target is correct (no aliasing or jitted code)
      if (btb_hit && pred_target == op->oracle_info.npc) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred = TAKEN;
        bp_pred_info->pred_npc = pred_target;
        STAT_EVENT_BP_SPLIT_PATH(op, BR_CORRECT);
      } else {
        bp_pred_info->recover_at_decode = TRUE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred = NOT_TAKEN;
        bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT_BP_SPLIT_PATH(op, BR_RECOVER);
      }
      break;
    case CF_REP:
    case CF_CBR:
      // Branch predictors may use pred_global_hist as input.
      bp_pred_info->pred_global_hist = bp_data->global_hist;

      if (PERFECT_BP) {
        bp_pred_info->pred = op->oracle_info.dir;
        bp_pred_info->pred_orig = op->oracle_info.dir;
      } else {
        ASSERT(op->proc_id, !PERFECT_NT_BTB);  // currently not supported
        bp_pred_info->pred = pred_bp->pred_func(op, pred_level);
        bp_pred_info->pred_orig = bp_pred_info->pred;
      }
      // Update history used by the rest of Scarab.
      if (pred_level == BP_PRED_MAIN)
        bp_data->global_hist = (bp_data->global_hist >> 1) | (bp_pred_info->pred << 31);

      if (!btb_hit && bp_pred_info->pred == NOT_TAKEN)
        btb_miss_nt = TRUE;

      // pred_target is set by BTB on hit. For CBR we may however, still want to execute fall-through
      if (bp_pred_info->pred == NOT_TAKEN) {
        pred_target = pc_plus_offset;
      }

      // Regular mispredict resolved at exec
      // On dir misprediction, treat as correctly predicted if fall-through happens to match target
      if (btb_hit && op->oracle_info.dir != bp_pred_info->pred && pc_plus_offset != op->oracle_info.target) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = TRUE;
        bp_pred_info->pred_npc = pred_target;

        if (bp_pred_info->pred == TAKEN)
          ASSERT(0, pred_target != pc_plus_offset);
        if (bp_pred_info->pred == NOT_TAKEN)
          ASSERT(0, pred_target == pc_plus_offset);

        STAT_EVENT_BP_SPLIT_PATH(op, CBR_RECOVER_AT_EXEC);
      }
      // Although the btb hits and cbr is correctly predicted, target address may be wrong (aliasing or jitted code)
      else if (btb_hit && pred_target != op->oracle_info.npc) {
        bp_pred_info->recover_at_decode = TRUE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred_npc = pred_target;
        STAT_EVENT_BP_SPLIT_PATH(op, CBR_RECOVER_AT_DECODE);
      }
      // Correctly predicted
      else if (btb_hit) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred_npc = pred_target;
        STAT_EVENT_BP_SPLIT_PATH(op, CBR_CORRECT);
      }
      // If BTB missed, the branch will be assumed not taken at fetch. At decode we detect
      // the branch and will predict. There are 4 outcomes:
      // 1. Branch is predicted taken, violating not-taken assumption, causing flush at decode
      else if (!btb_hit && bp_pred_info->pred == TAKEN && op->oracle_info.dir == TAKEN) {
        bp_pred_info->recover_at_decode = TRUE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred = NOT_TAKEN;
        bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT_BP_SPLIT_PATH(op, CBR_RECOVER_BTB_MISS_T_T);
      }
      // 2. Branch is predicted taken, violating not-taken asumption. This would flush at decode,
      // however, the branch will flush again at exec when it is determined that the prediction was wrong
      // Scarab does not support flushing twice per op. Flushing at exec should not introduce inaccuracy.
      else if (!btb_hit && bp_pred_info->pred == TAKEN && op->oracle_info.dir == NOT_TAKEN) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = TRUE;
        bp_pred_info->pred = NOT_TAKEN;
        bp_pred_info->pred_npc = pred_target;  // Not accurate. At fetch it would execute pc_plus_offset, at decode
                                               // would resteer frontend to pred_taken
        STAT_EVENT_BP_SPLIT_PATH(op, CBR_RECOVER_BTB_MISS_T_NT);
      }
      // 3. Branch is predicted not-taken causing branch to continue to exec where the flush is triggered
      else if (!btb_hit && bp_pred_info->pred == NOT_TAKEN && op->oracle_info.dir == TAKEN) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = TRUE;
        bp_pred_info->pred = NOT_TAKEN;
        bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT_BP_SPLIT_PATH(op, CBR_RECOVER_BTB_MISS_NT_T);
      }
      // 4. Branch is predicted not-taken which is correct causing no flush
      else if (!btb_hit && bp_pred_info->pred == NOT_TAKEN && op->oracle_info.dir == NOT_TAKEN) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred = NOT_TAKEN;
        bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT_BP_SPLIT_PATH(op, CBR_CORRECT_BTB_MISS_NT_NT);
      } else {
        // We should have matched all cases by here
        ASSERT(op->proc_id, 0);
      }
      break;

    case CF_CALL:
      bp_pred_info->pred = TAKEN;
      bp_pred_info->pred_orig = TAKEN;
      if (ENABLE_CRS)
        CRS_REALISTIC ? bp_crs_realistic_push(bp_data, op) : bp_crs_push(bp_data, op);
      // On BTB hit, ensure that target is correct (no aliasing or jitted code)
      if (btb_hit && pred_target == op->oracle_info.npc) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred = TAKEN;
        bp_pred_info->pred_npc = pred_target;
        DEBUG(bp_data->proc_id,
              "no flush BP:  op_num:%s  off_path:%d  cf_type:%s  addr:%s  p_npc:%s  "
              "t_npc:0x%s  btb_miss:%d  recover_at_fe:%d recover_at_decode:%d recover_at_exec:%d no_tar:%d\n",
              unsstr64(op->op_num), op->off_path, cf_type_names[op->inst_info->table_info.cf_type],
              hexstr64s(op->inst_info->addr), hexstr64s(bp_pred_info->pred_npc), hexstr64s(op->oracle_info.npc),
              btb_pred_miss(op->btb_pred_info), bp_pred_info->recover_at_fe, bp_pred_info->recover_at_decode,
              bp_pred_info->recover_at_exec, op->btb_pred_info->no_target);

        ASSERT(0, bp_pred_info->pred == op->oracle_info.dir);
        STAT_EVENT_BP_SPLIT_PATH(op, CALL_CORRECT);
      } else {
        DEBUG(bp_data->proc_id,
              "flush BP:  op_num:%s  off_path:%d  cf_type:%s  addr:%s  p_npc:%s  "
              "t_npc:0x%s  btb_miss:%d  recover_at_fe:%d recover_at_decode:%d recover_at_exec:%d no_tar:%d "
              "predtarg %llx npc %llx\n",
              unsstr64(op->op_num), op->off_path, cf_type_names[op->inst_info->table_info.cf_type],
              hexstr64s(op->inst_info->addr), hexstr64s(bp_pred_info->pred_npc), hexstr64s(op->oracle_info.npc),
              btb_pred_miss(op->btb_pred_info), bp_pred_info->recover_at_fe, bp_pred_info->recover_at_decode,
              bp_pred_info->recover_at_exec, op->btb_pred_info->no_target, pred_target, op->oracle_info.npc);

        bp_pred_info->recover_at_decode = TRUE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred = NOT_TAKEN;
        bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT_BP_SPLIT_PATH(op, CALL_RECOVER);
      }
      break;

    case CF_IBR:
      if (PERFECT_BP) {
        bp_pred_info->pred = op->oracle_info.dir;
        bp_pred_info->pred_orig = op->oracle_info.dir;
      } else {
        bp_pred_info->pred = TAKEN;
        bp_pred_info->pred_orig = TAKEN;
      }
      if (ENABLE_IBP && !op->btb_pred_info->ibp_miss) {
        ASSERT(op->proc_id, op->oracle_info.target == op->oracle_info.npc);
        if (op->oracle_info.target == pred_target) {
          bp_pred_info->recover_at_decode = FALSE;
          bp_pred_info->recover_at_exec = FALSE;
          bp_pred_info->pred_npc = pred_target;
          STAT_EVENT_BP_SPLIT_PATH(op, IBR_CORRECT_IBTB);
        } else {
          bp_pred_info->recover_at_decode = FALSE;
          bp_pred_info->recover_at_exec = TRUE;
          bp_pred_info->pred_npc = pred_target;
          STAT_EVENT_BP_SPLIT_PATH(op, IBR_RECOVER_AT_EXEC_IBTB);
        }
      } else if (btb_hit) {
        if (op->oracle_info.target == pred_target) {
          bp_pred_info->recover_at_decode = FALSE;
          bp_pred_info->recover_at_exec = FALSE;
          bp_pred_info->pred_npc = pred_target;
          STAT_EVENT_BP_SPLIT_PATH(op, IBR_CORRECT_BTB);
        } else {
          bp_pred_info->recover_at_decode = FALSE;
          bp_pred_info->recover_at_exec = TRUE;
          bp_pred_info->pred_npc = pred_target;
          STAT_EVENT_BP_SPLIT_PATH(op, IBR_RECOVER_AT_EXEC_BTB);
        }
      }
      // If BTB and iBTB miss we can detect the mispredition at decode but we need to wait
      // until exec to resolve the branch target. We would not know which target to fetch
      // at decode so we can just recover at exec
      else {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = TRUE;
        bp_pred_info->pred = NOT_TAKEN;
        bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT_BP_SPLIT_PATH(op, IBR_RECOVER_AT_EXEC_XBTB_MISS);
      }

      break;

    case CF_ICALL:
      if (PERFECT_BP) {
        bp_pred_info->pred = op->oracle_info.dir;
        bp_pred_info->pred_orig = op->oracle_info.dir;
      } else {
        bp_pred_info->pred = TAKEN;
        bp_pred_info->pred_orig = TAKEN;
      }
      if (ENABLE_CRS)
        CRS_REALISTIC ? bp_crs_realistic_push(bp_data, op) : bp_crs_push(bp_data, op);

      if (ENABLE_IBP && !op->btb_pred_info->ibp_miss) {
        ASSERT(op->proc_id, op->oracle_info.target == op->oracle_info.npc);
        if (op->oracle_info.target == pred_target) {
          bp_pred_info->recover_at_decode = FALSE;
          bp_pred_info->recover_at_exec = FALSE;
          bp_pred_info->pred_npc = pred_target;
          STAT_EVENT_BP_SPLIT_PATH(op, ICALL_CORRECT_IBTB);
        } else {
          bp_pred_info->recover_at_decode = FALSE;
          bp_pred_info->recover_at_exec = TRUE;
          bp_pred_info->pred_npc = pred_target;
          STAT_EVENT_BP_SPLIT_PATH(op, ICALL_RECOVER_AT_EXEC_IBTB);
        }
      } else if (btb_hit) {
        if (op->oracle_info.target == pred_target) {
          bp_pred_info->recover_at_decode = FALSE;
          bp_pred_info->recover_at_exec = FALSE;
          bp_pred_info->pred_npc = pred_target;
          STAT_EVENT_BP_SPLIT_PATH(op, ICALL_CORRECT_BTB);
        } else {
          bp_pred_info->recover_at_decode = FALSE;
          bp_pred_info->recover_at_exec = TRUE;
          bp_pred_info->pred_npc = pred_target;
          STAT_EVENT_BP_SPLIT_PATH(op, ICALL_RECOVER_AT_EXEC_BTB);
        }
      }
      // If BTB and iBTB miss we can detect the mispredition at decode but we need to wait
      // until exec to resolve the branch target. We would not know which target to fetch
      // at decode so we can just recover at exec
      else {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = TRUE;
        bp_pred_info->pred = NOT_TAKEN;
        bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT_BP_SPLIT_PATH(op, ICALL_RECOVER_AT_EXEC_XBTB_MISS);
      }

      break;

    case CF_ICO:
      bp_pred_info->pred = TAKEN;
      bp_pred_info->pred_orig = TAKEN;
      if (ENABLE_CRS) {
        pred_target = CRS_REALISTIC ? bp_crs_realistic_pop(bp_data, op) : bp_crs_pop(bp_data, op);
        CRS_REALISTIC ? bp_crs_realistic_push(bp_data, op) : bp_crs_push(bp_data, op);
      }

      if (pred_target != op->oracle_info.npc) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = TRUE;
        bp_pred_info->pred_npc = pred_target;
        STAT_EVENT_BP_SPLIT_PATH(op, ICO_RECOVER);
      } else {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred = NOT_TAKEN;
        bp_pred_info->pred_npc = pc_plus_offset;
        STAT_EVENT_BP_SPLIT_PATH(op, ICO_CORRECT);
      }

      break;

    case CF_RET:
      if (PERFECT_BP) {
        bp_pred_info->pred = op->oracle_info.dir;
        bp_pred_info->pred_orig = op->oracle_info.dir;
      } else {
        bp_pred_info->pred = TAKEN;
        bp_pred_info->pred_orig = TAKEN;
      }
      if (ENABLE_CRS)
        pred_target = CRS_REALISTIC ? bp_crs_realistic_pop(bp_data, op) : bp_crs_pop(bp_data, op);
      if (pred_target == 0) {  // RAS Underflow
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = TRUE;
        bp_pred_info->pred_npc = pc_plus_offset;
        bp_pred_info->pred = NOT_TAKEN;
        STAT_EVENT_BP_SPLIT_PATH(op, RET_RECOVER_UFLOW);
      } else if (pred_target != op->oracle_info.npc) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = TRUE;
        bp_pred_info->pred_npc = pred_target;
        STAT_EVENT_BP_SPLIT_PATH(op, RET_RECOVER);
      } else {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->pred_npc = pred_target;
        STAT_EVENT_BP_SPLIT_PATH(op, RET_CORRECT);
      }
      break;

    default:
      ASSERT(op->proc_id, 0);  // should not happen
      bp_pred_info->pred = TAKEN;
      bp_pred_info->pred_orig = TAKEN;
      break;
  }
  // }}}

  if (!btb_hit && bp_pred_info->pred == NOT_TAKEN)
    btb_miss_nt = TRUE;

  if (pred_level == BP_PRED_L0) {
    ASSERT(op->proc_id, !bp_pred_info->recover_at_fe);
    bp_pred_info->recover_at_fe = bp_pred_info->recover_at_decode || bp_pred_info->recover_at_exec;
    if (bp_pred_info->recover_at_fe) {
      bp_pred_info->recover_at_decode = FALSE;
      bp_pred_info->recover_at_exec = FALSE;
    }
  }

  // Pre-spec-update hook for the alt-DFE machinery: alts with
  // ALTERNATE_ON_PREDICTION / ALTERNATE_ON_MISPREDICTION trigger want main's
  // PRE-spec-update bp_data state captured (via bp_sync) before the upcoming
  // spec_update applies main's direction, so the post-hook can re-apply
  // spec_update on alt's TAGE with alt's direction. Gated on
  // SIMULATION_MODE (warmup's bp_predict_op should not touch alt) and
  // on-path predictions only (off-path predictions don't drive alt events).
  if (bp_id == MAIN_BP && pred_level == BP_PRED_MAIN && operating_mode == SIMULATION_MODE && !op->off_path)
    decoupled_fe_capture_main_pre_state(op->proc_id, op);

  pred_bp->spec_update_func(op, pred_level);

  DEBUG(bp_data->proc_id,
        "BP[%s,%s]:  op_num:%s  off_path:%d  cf_type:%s  addr:%s  p_npc:%s  "
        "t_npc:0x%s  btb_miss:%d  recover_at_fe:%d recover_at_decode:%d recover_at_exec:%d no_tar:%d dir%d "
        "pred%d offset %llx target %llx\n",
        pred_bp->name, pred_level == BP_PRED_L0 ? "l0" : "main", unsstr64(op->op_num), op->off_path,
        cf_type_names[op->inst_info->table_info.cf_type], hexstr64s(op->inst_info->addr),
        hexstr64s(bp_pred_info->pred_npc), hexstr64s(op->oracle_info.npc), btb_pred_miss(op->btb_pred_info),
        bp_pred_info->recover_at_fe, bp_pred_info->recover_at_decode, bp_pred_info->recover_at_exec,
        op->btb_pred_info->no_target, op->oracle_info.dir, bp_pred_info->pred, pc_plus_offset, op->oracle_info.target);

  ASSERT(op->proc_id, bp_pred_info->pred_npc);
  if (op->oracle_info.dir != bp_pred_info->pred && pc_plus_offset != op->oracle_info.target) {
    ASSERT(op->proc_id,
           bp_pred_info->recover_at_fe || bp_pred_info->recover_at_exec || bp_pred_info->recover_at_decode);
  }

  ASSERT_PROC_ID_IN_ADDR(op->proc_id, bp_pred_info->pred_npc);

  op->bp_cycle = cycle_count;

  if (op->inst_info->table_info.cf_type == CF_CBR || op->inst_info->table_info.cf_type == CF_REP) {
    if (!op->off_path) {
      if (bp_pred_info->recover_at_exec)
        _DEBUGA(op->proc_id, 0, "ON PATH HW RECOVER_AT_EXEC  addr:0x%s  pghist:0x%s\n", hexstr64s(op->inst_info->addr),
                hexstr64s(bp_pred_info->pred_global_hist));
      else
        _DEBUGA(op->proc_id, 0, "ON PATH HW CORRECT  addr:0x%s  pghist:0x%s\n", hexstr64s(op->inst_info->addr),
                hexstr64s(bp_pred_info->pred_global_hist));
    }
  }

  DEBUG_BTB(bp_data->proc_id, "BTB:  op_num:%s  off_path:%d  cf_type:%s  addr:0x%s  btb_miss:%d\n",
            unsstr64(op->op_num), op->off_path, cf_type_names[op->inst_info->table_info.cf_type],
            hexstr64s(op->inst_info->addr), btb_pred_miss(op->btb_pred_info));

  DEBUG(bp_data->proc_id,
        "BP:  op_num:%s  off_path:%d  cf_type:%s  addr:%s  p_npc:%s  "
        "t_npc:0x%s  btb_miss:%d  recover_at_fe:%d  recover_at_decode:%d  recover_at_exec:%d  no_tar:%d\n",
        unsstr64(op->op_num), op->off_path, cf_type_names[op->inst_info->table_info.cf_type],
        hexstr64s(op->inst_info->addr), hexstr64s(bp_pred_info->pred_npc), hexstr64s(op->oracle_info.npc),
        btb_pred_miss(op->btb_pred_info), bp_pred_info->recover_at_fe, bp_pred_info->recover_at_decode,
        bp_pred_info->recover_at_exec, op->btb_pred_info->no_target);

  if (bp_id == MAIN_BP && ENABLE_BP_CONF && IS_CONF_CF(op)) {
    bp_data->br_conf->pred_func(op, pred_level);

    if (!(bp_pred_info->pred_conf))
      td->td_info.low_conf_count++;
    DEBUG(bp_data->proc_id, "low_conf_count:%d \n", td->td_info.low_conf_count);
  }

  {
    const Stat_Enum predictions_stat = (op->off_path ? L0_ALL_PREDICTIONS_OFFPATH : L0_ALL_PREDICTIONS) + pred_level;
    const Stat_Enum recover_at_exec_stat =
        (op->off_path ? L0_ALL_RECOVER_AT_EXEC_OFFPATH : L0_ALL_RECOVER_AT_EXEC) + pred_level;
    const Stat_Enum recover_at_decode_stat =
        (op->off_path ? L0_ALL_RECOVER_AT_DECODE_OFFPATH : L0_ALL_RECOVER_AT_DECODE) + pred_level;
    const Flag l0_or_exec_recovery =
        (pred_level == BP_PRED_L0) ? bp_pred_info->recover_at_fe : bp_pred_info->recover_at_exec;

    STAT_EVENT_BP(predictions_stat);
    if (l0_or_exec_recovery)
      STAT_EVENT_BP(recover_at_exec_stat);
    if (bp_pred_info->recover_at_decode)
      STAT_EVENT_BP(recover_at_decode_stat);
  }

  // The case where BTB-miss not-taken branch pollute global hist
  // a not-taken BTB-miss correction has no chance to fix the global hist
  if (bp_id == MAIN_BP && btb_miss_nt && (bp_pred_info->recover_at_fe || bp_pred_info->recover_at_exec) &&
      bp_pred_info->pred_npc != op->oracle_info.npc)
    STAT_EVENT(op->proc_id, FDIP_BTB_MISS_NT_RESTEER_ONPATH + op->off_path);

  if (bp_id == MAIN_BP && !op->off_path) {
    if (bp_pred_info->recover_at_fe) {
      STAT_EVENT(op->proc_id, BP_L0_EARLY_RECOVERIES);
    } else if (bp_pred_info->recover_at_exec) {
      STAT_EVENT(op->proc_id, BP_MAIN_EXEC_RECOVERIES);
    } else if (bp_pred_info->recover_at_decode) {
      STAT_EVENT(op->proc_id, BP_MAIN_DECODE_RECOVERIES);
    }
  }
  return bp_pred_info->pred_npc;
}

Addr bp_predict_op(Bp_Data* bp_data, Op* op, uns bp_id, uns br_num, Addr fetch_addr, Bp_Pred_Level pred_level) {
  return bp_predict_op_impl(bp_data, op, bp_id, br_num, fetch_addr, pred_level);
}

/******************************************************************************/
/* bp_target_known_op: called on cf ops when the real target is known
   (either decode time or execute time) */

void bp_target_known_op(Bp_Data* bp_data, Op* op) {
  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  ASSERT(bp_data->proc_id, op->inst_info->table_info.cf_type);

  bp_data->bp_btb->update_func(bp_data, op);

  // special case updates
  switch (op->inst_info->table_info.cf_type) {
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
  // Always train both predictors regardless of which one made the active prediction.
  op->recovery_info.branch_id = op->bp_pred_main.pred_branch_id;
  bp_data->bp->update_func(op, BP_PRED_MAIN);
  if (bp_data->bp_l0)
    bp_data->bp_l0->update_func(op, BP_PRED_L0);

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
  // Always retire both predictors regardless of which one made the active prediction.
  op->recovery_info.branch_id = op->bp_pred_main.pred_branch_id;
  bp_data->bp->retire_func(op);
  if (bp_data->bp_l0)
    bp_data->bp_l0->retire_func(op);
}

/******************************************************************************/
/* bp_recover_op: called on the last mispredicted op when the recovery happens
 */

void bp_recover_op(Bp_Data* bp_data, Cf_Type cf_type, Recovery_Info* info) {
  STAT_EVENT(0, PERFORMED_RECOVERIES);
  INC_STAT_EVENT(0, PERFORMED_RECOVERY_LAT, cycle_count - info->predict_cycle);
  /* always recover the global history */
  if (cf_type == CF_CBR || cf_type == CF_REP) {
    bp_data->global_hist = (info->pred_global_hist >> 1) | (info->new_dir << 31);
  } else {
    bp_data->global_hist = info->pred_global_hist;
  }
  bp_data->targ_hist = info->targ_hist;

  /* this event counts updates to BP, so it's really branch resolutions */
  STAT_EVENT(bp_data->proc_id, POWER_BRANCH_RECOVER_AT_EXEC);
  STAT_EVENT(bp_data->proc_id, POWER_BTB_WRITE);

  bp_data->bp_btb->recover_func(bp_data, info);

  /* type-specific recovery */
  if (cf_type == CF_ICALL || cf_type == CF_IBR) {
    bp_data->bp_ibtb->recover_func(bp_data, info);
  }
  bp_data->bp->recover_func(info);
  if (bp_data->bp_l0)
    bp_data->bp_l0->recover_func(info);

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
