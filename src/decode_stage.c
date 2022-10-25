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
 * File         : decode_stage.c
 * Author       : HPS Research Group
 * Date         : 2/17/1999
 * Description  : simulates the latency due to decode stage. (actual uop decoding was
                    done in frontend)
 ***************************************************************************************/

#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "isa/isa_macros.h"

#include "bp/bp.h"
#include "decode_stage.h"
#include "op_pool.h"

#include "core.param.h"
#include "debug/debug.param.h"
#include "general.param.h"
#include "thread.h" /* for td */

#include "uop_cache.h"
#include "statistics.h"
#include "memory/memory.param.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECODE_STAGE, ##args)
#define STAGE_MAX_OP_COUNT ISSUE_WIDTH
#define STAGE_MAX_DEPTH DECODE_CYCLES


/**************************************************************************************/
/* Global Variables */

Decode_Stage* dec = NULL;

/**************************************************************************************/
/* Local prototypes */

static inline void update_cycles_stats(Stage_Data* src_sd, int empty_stage_idx);

/**************************************************************************************/
/* set_decode_stage: */

void set_decode_stage(Decode_Stage* new_dec) {
  dec = new_dec;
}


/**************************************************************************************/
/* init_decode_stage: */

void init_decode_stage(uns8 proc_id, const char* name) {
  char tmp_name[MAX_STR_LENGTH + 1];
  uns  ii;
  ASSERT(0, dec);
  ASSERT(0, STAGE_MAX_DEPTH > 0);
  DEBUG(proc_id, "Initializing %s stage\n", name);

  memset(dec, 0, sizeof(Decode_Stage));
  dec->proc_id = proc_id;

  dec->sds = (Stage_Data*)malloc(sizeof(Stage_Data) * STAGE_MAX_DEPTH);
  for(ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &dec->sds[ii];
    snprintf(tmp_name, MAX_STR_LENGTH, "%s %d", name, STAGE_MAX_DEPTH - ii - 1);
    cur->name         = (char*)strdup(tmp_name);
    cur->max_op_count = STAGE_MAX_OP_COUNT;
    cur->ops          = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);
  }
  dec->last_sd = &dec->sds[0];
  reset_decode_stage();
}


/**************************************************************************************/
/* reset_decode_stage: */

void reset_decode_stage() {
  uns ii, jj;
  ASSERT(0, dec);
  for(ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &dec->sds[ii];
    cur->op_count   = 0;
    for(jj = 0; jj < STAGE_MAX_OP_COUNT; jj++)
      cur->ops[jj] = NULL;
  }
}


/**************************************************************************************/
/* recover_decode_stage: */

void recover_decode_stage() {
  uns ii, jj;
  ASSERT(0, dec);
  for(ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &dec->sds[ii];
    cur->op_count   = 0;
    for(jj = 0; jj < STAGE_MAX_OP_COUNT; jj++) {
      if(cur->ops[jj]) {
        if(FLUSH_OP(cur->ops[jj])) {
          free_op(cur->ops[jj]);
          cur->ops[jj] = NULL;
        } else {
          cur->op_count++;
        }
      }
    }
  }
}


/**************************************************************************************/
/* debug_decode_stage: */

void debug_decode_stage() {
  uns ii;
  for(ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &dec->sds[STAGE_MAX_DEPTH - ii - 1];
    DPRINTF("# %-10s  op_count:%d\n", cur->name, cur->op_count);
    print_op_array(GLOBAL_DEBUG_STREAM, cur->ops, STAGE_MAX_OP_COUNT,
                   cur->op_count);
  }
}


/**************************************************************************************/
/* decode_cycle: Movement of cached uops to later stages assumes that with a stalled
 *               pipeline the same (modified src_sd is passed to update_decode_stage
 *               What if there is a branch mispred? are instr properly flushed?
 */

void update_decode_stage(Stage_Data* src_sd) {
  Flag        stall = (dec->last_sd->op_count > 0);
  Stage_Data *cur, *prev;
  Op**        temp;
  uns         ii;

  /* do all the intermediate stages */
  for(ii = 0; ii < STAGE_MAX_DEPTH - 1; ii++) {
    cur = &dec->sds[ii];
    if(cur->op_count)
      continue;
    prev           = &dec->sds[ii + 1];
    temp           = cur->ops;
    cur->ops       = prev->ops;
    prev->ops      = temp;
    cur->op_count  = prev->op_count;
    prev->op_count = 0;
  }

  /* do the first decode stage */
  /* Ops from the uop cache do not go to the decode stage. */
  Flag from_icache = src_sd->op_count && !src_sd->ops[0]->fetched_from_uop_cache;
  cur = &dec->sds[STAGE_MAX_DEPTH - 1];
  if(cur->op_count == 0 && from_icache) {
    prev           = src_sd;
    temp           = cur->ops;
    cur->ops       = prev->ops;
    prev->ops      = temp;
    cur->op_count  = prev->op_count;
    prev->op_count = 0;
  }

  /* if the last decode stage is stalled, don't re-process the ops  */
  if(stall)
    return;

  /* now check the ops in the last decode stage for BTB errors */
  for(ii = 0; ii < dec->last_sd->op_count; ii++) {
    Op* op = dec->last_sd->ops[ii];
    ASSERT(dec->proc_id, op != NULL);
    ASSERT(dec->proc_id, !op->fetched_from_uop_cache);
    decode_stage_process_op(op);
    accumulate_op(op);
  }
}


/**************************************************************************************/
/* process_decode_op: */

void decode_stage_process_op(Op* op) {
  Cf_Type cf = op->table_info->cf_type;

  if(cf) {
    Flag bf = op->table_info->bar_type & BAR_FETCH ? TRUE : FALSE;

    if(cf <= CF_CALL) {
      // it is a direct branch, so the target is now known
      bp_target_known_op(g_bp_data, op);

      // since it is not indirect, redirect the input stream if it was a btb
      // miss
      if(op->oracle_info.btb_miss && !bf) {
        // since this is direct, it can no longer a misfetch
        op->oracle_info.misfetch = FALSE;
        op->oracle_info.pred_npc = op->oracle_info.pred ?
                                     op->oracle_info.target :
                                     ADDR_PLUS_OFFSET(
                                       op->inst_info->addr,
                                       op->inst_info->trace_info.inst_size);
        ASSERT_PROC_ID_IN_ADDR(op->proc_id, op->oracle_info.pred_npc);
        // schedule a redirect using the predicted npc
        bp_sched_redirect(bp_recovery_info, op, cycle_count);
        // stats for the reason of resteer
        STAT_EVENT(dec->proc_id, RESTEER_BTB_MISS_CF_BR + cf);
      }
    } else {
      // the instruction is indirect, so we can only unstall the front end
      if(op->oracle_info.btb_miss && !op->oracle_info.no_target && !bf) {
        // schedule a redirect using the predicted npc
        bp_sched_redirect(bp_recovery_info, op, cycle_count);
        // stats for the reason of resteer
        STAT_EVENT(dec->proc_id, RESTEER_BTB_MISS_CF_BR + cf);
      }
    }
  }
}

static inline void update_cycles_stats(Stage_Data* src_sd, int empty_stage_idx) {
  static Op*  last_op = NULL;  // The most recent op that has entered the decode stage.

  Op* next_op = NULL;
  int decode_stages_empty = STAGE_MAX_DEPTH - empty_stage_idx;
  if (last_op && decode_stages_empty && src_sd->op_count) {
    next_op = src_sd->ops[0];
    int fetch_bubbles = (next_op->fetch_cycle - last_op->fetch_cycle - 1);
    int cycles_saved_with_uoc_fetch = (ICACHE_LATENCY - UOP_CACHE_LATENCY) + (decode_stages_empty - 1);
    Flag last_op_resteered = last_op->oracle_info.mispred || last_op->oracle_info.misfetch || last_op->oracle_info.btb_miss;

    if (last_op_resteered) {
      // UOP_CACHE_LATENCY is the minimum fetch latency after a resteer. 
      // It is added since it cannot be improved upon with a better uop cache.
      Counter resteer_cycles = fetch_bubbles + UOP_CACHE_LATENCY;
      if (last_op->oracle_info.mispred) {
        INC_STAT_EVENT(dec->proc_id, CYCLES_RESTEER_MISPRED, resteer_cycles);
        INC_STAT_EVENT(dec->proc_id, CYCLES_RESTEER_MISPRED_SAVED_UC_FETCH + !next_op->fetched_from_uop_cache, cycles_saved_with_uoc_fetch);
      } else if (last_op->oracle_info.misfetch) {
        INC_STAT_EVENT(dec->proc_id, CYCLES_RESTEER_MISFETCH, resteer_cycles);
        INC_STAT_EVENT(dec->proc_id, CYCLES_RESTEER_MISFETCH_SAVED_UC_FETCH + !next_op->fetched_from_uop_cache, cycles_saved_with_uoc_fetch);
      } else if (last_op->oracle_info.btb_miss) {
        INC_STAT_EVENT(dec->proc_id, CYCLES_RESTEER_BTB_MISS, resteer_cycles);
        INC_STAT_EVENT(dec->proc_id, CYCLES_RESTEER_BTB_MISS_SAVED_UC_FETCH + !next_op->fetched_from_uop_cache, cycles_saved_with_uoc_fetch);
      }
    } else if (last_op->fetched_from_uop_cache && !next_op->fetched_from_uop_cache) {
      INC_STAT_EVENT(dec->proc_id, CYCLES_UOC_IC_SWITCH, cycles_saved_with_uoc_fetch);
    }
  }
  // Only count stats if the icache is not stalled due to a full decode stage.
  if (decode_stages_empty && src_sd->op_count)
    last_op = src_sd->ops[src_sd->op_count - 1];
}