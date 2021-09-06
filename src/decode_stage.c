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
#define STAGE_MAX_OP_COUNT ISSUE_WIDTH + 1
#define STAGE_MAX_DEPTH DECODE_CYCLES


/**************************************************************************************/
/* Global Variables */

Decode_Stage* dec = NULL;

/**************************************************************************************/
/* Local prototypes */

static inline void stage_process_op(Op*);


/**************************************************************************************/
/* set_decode_stage: */

void set_decode_stage(Decode_Stage* new_dec) {
  dec = new_dec;
}


/**************************************************************************************/
/* init_decode_stage: */

void init_decode_stage(uns8 proc_id, const char* name) {
  char tmp_name[MAX_STR_LENGTH];
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
  static Flag fetching_from_UC = FALSE;

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
  /* First, insert all cached uops into stage nearest last. 
   * Next, place the stage data minus cached instr into last stage 
   */

  /* find first available empty pipeline stage for cached uops */
  int empty_stage_idx = STAGE_MAX_DEPTH;
  for(int jj = STAGE_MAX_DEPTH - 1; jj >= 0; jj--) {
    cur = &dec->sds[jj];
    if(!cur->op_count) {
      empty_stage_idx = jj;
    } else {
      break;
    }
  }

  /* Move cached uops to later stage in pipeline if possible */
  for (int ii = 0; ii < src_sd->op_count; ii++) {

    // Cannot leave icache if not in uop cache, or no space in first stage.
    if (!src_sd->ops[ii]->fetched_from_uop_cache) {
      fetching_from_UC = FALSE;
      break;
    }
    if (dec->sds[STAGE_MAX_DEPTH - 1].op_count == STAGE_MAX_OP_COUNT) {
      break;
    }

    /* the next stage after the empty stage may have a few extra slots */
    // want to be able to append to first stage if there is a stall
    int append_to_sd = empty_stage_idx - 1 >= 0 
                       && dec->sds[empty_stage_idx - 1].op_count < STAGE_MAX_OP_COUNT;
    int insert_into_sd_num = -1;
    if (append_to_sd) {
      insert_into_sd_num = empty_stage_idx - 1;
    } else if (empty_stage_idx < STAGE_MAX_DEPTH - 1) {
      /* append to closest empty stage */
      insert_into_sd_num = empty_stage_idx;
    } else {
      /* No empty slots in later stages, Pipeline full. Done moving individual ops */
      continue;
    }

    // log cycles saved only for the FIRST op that is moved in seq of fetched instr from UC
    // if inserting into first stage when first stage is partially full still save 1 cycle
    if (!fetching_from_UC) {
      int cycles_saved = (STAGE_MAX_DEPTH - 1) - insert_into_sd_num;
      INC_STAT_EVENT(dec->proc_id, UOP_CACHE_CYCLES_SAVED, cycles_saved ? cycles_saved : 1);
      fetching_from_UC = TRUE;
    }

    /* stage to insert op into */
    Stage_Data* insert_stage = &dec->sds[insert_into_sd_num];

    Op* moved_op = src_sd->ops[ii];
    insert_stage->ops[insert_stage->op_count] = src_sd->ops[ii];
    src_sd->ops[ii] = NULL;
    insert_stage->op_count++;

    /* process op if appended to last dec stage. Fetched from uop cache, so do not accumulate for insert */
    ASSERT(dec->proc_id, moved_op != NULL);
    if (empty_stage_idx - 1 == 0 && append_to_sd) {
      stage_process_op(moved_op);
      end_accumulate();
    }
  }

  /* update src_sd->ops, op_count */
  int ops_moved = 0;
  for (int ii = 0; ii < src_sd->op_count; ii++) {
    if (src_sd->ops[ii] == NULL) {
      ops_moved++;
    } else if (ops_moved > 0) {
      src_sd->ops[ii - ops_moved] = src_sd->ops[ii];
      src_sd->ops[ii] = NULL;
    } else {
      break;
    }
  }
  src_sd->op_count -= ops_moved;

  INC_STAT_EVENT(dec->proc_id, N_UOPS_DEC, ops_moved);

  /* Place any remaining ops into first stage */
  cur = &dec->sds[STAGE_MAX_DEPTH - 1];
  if(cur->op_count == 0) {
    prev           = src_sd;
    temp           = cur->ops;
    cur->ops       = prev->ops;
    prev->ops      = temp;
    cur->op_count  = prev->op_count;
    prev->op_count = 0;
    INC_STAT_EVENT(dec->proc_id, N_UOPS_DEC, cur->op_count);
  } else {
    ASSERT(0, stall);
  }

  /* if the last decode stage is stalled, don't re-process the ops  */
  if(stall)
    return;

  /* now check the ops in the last decode stage for BTB errors */
  for(ii = 0; ii < dec->last_sd->op_count; ii++) {
    Op* op = dec->last_sd->ops[ii];
    ASSERT(dec->proc_id, op != NULL);
    stage_process_op(op);
    // Cache all uops being emitted from decode stage
    if (!op->fetched_from_uop_cache) {
      STAT_EVENT(dec->proc_id, UOP_ACCUMULATE);
      accumulate_op(op);
    } else {
      end_accumulate();
    }
  }
}


/**************************************************************************************/
/* process_decode_op: */

static inline void stage_process_op(Op* op) {
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
        // schedule a redirect using the predicted npc
        bp_sched_redirect(bp_recovery_info, op, cycle_count);
      }
    } else {
      // the instruction is indirect, so we can only unstall the front end
      if(op->oracle_info.btb_miss && !op->oracle_info.no_target && !bf) {
        // schedule a redirect using the predicted npc
        bp_sched_redirect(bp_recovery_info, op, cycle_count);
      }
    }
  }
}