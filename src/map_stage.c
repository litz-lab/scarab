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
 * File         : map_stage.c
 * Author       : HPS Research Group
 * Date         : 2/4/1999
 * Description  :
 ***************************************************************************************/

#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "memory/memory.param.h"
#include "op_pool.h"

#include "bp/bp.h"
#include "map.h"
#include "map_stage.h"
#include "model.h"
#include "thread.h"
#include "uop_cache.h"

#include "core.param.h"
#include "debug/debug.param.h"
#include "statistics.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MAP_STAGE, ##args)
#define STAGE_MAX_OP_COUNT ISSUE_WIDTH + UOP_CACHE_ADDITIONAL_ISSUE_BANDWIDTH
// Since update_map_stage is called twice per cycle, STAGE_MAX_DEPTH should be double
// the number of cycles.
#define STAGE_MAX_DEPTH (MAP_CYCLES * 2)


/**************************************************************************************/
/* Global Variables */

Map_Stage* map = NULL;
// The next op number is used when deciding whether to consume ops from the
// uop cache: i.e. check if any preceding instructions are still in the decoder.
Counter map_stage_next_op_num = 1;

/**************************************************************************************/
/* Local prototypes */

static inline void stage_process_op(Op*);


/**************************************************************************************/
/* set_map_stage: */

void set_map_stage(Map_Stage* new_map) {
  map = new_map;
}


/**************************************************************************************/
/* init_map_stage: */

void init_map_stage(uns8 proc_id, const char* name) {
  char tmp_name[MAX_STR_LENGTH + 1];
  uns  ii;
  ASSERT(proc_id, map);
  ASSERT(proc_id, STAGE_MAX_DEPTH > 0);
  DEBUG(proc_id, "Initializing %s stage\n", name);

  memset(map, 0, sizeof(Map_Stage));
  map->proc_id = proc_id;

  map->sds = (Stage_Data*)malloc(sizeof(Stage_Data) * STAGE_MAX_DEPTH);
  for(ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    snprintf(tmp_name, MAX_STR_LENGTH, "%s %d", name, STAGE_MAX_DEPTH - ii - 1);
    cur->name         = (char*)strdup(tmp_name);
    cur->max_op_count = STAGE_MAX_OP_COUNT;
    cur->ops          = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);
  }
  map->last_sd = &map->sds[0];
  reset_map_stage();
}


/**************************************************************************************/
/* reset_map_stage: */

void reset_map_stage() {
  uns ii, jj;
  ASSERT(0, map);
  for(ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    cur->op_count   = 0;
    for(jj = 0; jj < STAGE_MAX_OP_COUNT; jj++)
      cur->ops[jj] = NULL;
  }
}


/**************************************************************************************/
/* recover_map_stage: */

void recover_map_stage() {
  uns ii, jj, kk;
  ASSERT(0, map);
  for(ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    cur->op_count   = 0;

    for(jj = 0, kk = 0; jj < STAGE_MAX_OP_COUNT; jj++) {
      if(cur->ops[jj]) {
        if(FLUSH_OP(cur->ops[jj])) {
          free_op(cur->ops[jj]);
          cur->ops[jj] = NULL;
        } else {
          Op* op = cur->ops[jj];
          cur->op_count++;
          cur->ops[jj]   = NULL;  // collapse the ops
          cur->ops[kk++] = op;
        }
      }
    }
  }

  if (map_stage_next_op_num > bp_recovery_info->recovery_op_num) {
    map_stage_next_op_num = bp_recovery_info->recovery_op_num + 1;
    DEBUG(map->proc_id, "Recovering map_stage_next_op_num to %llu\n", map_stage_next_op_num);
  }
}


/**************************************************************************************/
/* debug_map_stage: */

void debug_map_stage() {
  uns ii;
  for(ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[STAGE_MAX_DEPTH - ii - 1];
    DPRINTF("# %-10s  op_count:%d\n", cur->name, cur->op_count);
    print_op_array(GLOBAL_DEBUG_STREAM, cur->ops, STAGE_MAX_OP_COUNT,
                   STAGE_MAX_OP_COUNT);
  }
}


/**************************************************************************************/
/* map_cycle: */

void update_map_stage(Stage_Data* src_sd) {
  // Last cycle that map stage consumed ops.
  // This prevents map stage from accidentally consuming ops twice per cycle
  // (since update_map_stage is called twice per cycle).
  static Counter last_cycle_consumed = 0;
  Flag        stall = (map->last_sd->op_count > 0);
  Stage_Data *cur, *prev;
  Op**        temp;
  uns         ii;

  /* do all the intermediate stages */
  for(ii = 0; ii < STAGE_MAX_DEPTH - 1; ii++) {
    cur = &map->sds[ii];
    if(cur->op_count)
      continue;
    prev           = &map->sds[ii + 1];
    temp           = cur->ops;
    cur->ops       = prev->ops;
    prev->ops      = temp;
    cur->op_count  = prev->op_count;
    prev->op_count = 0;
  }

  /* do the first map stage */
  // Uops can be received from either the decoder or directly from the uop cache
  // via the uop queue.
  // Only consume if older ops have already been consumed by this stage.
  Flag consume_ops = src_sd->op_count && src_sd->ops[0]->op_num == map_stage_next_op_num;
  cur = &map->sds[STAGE_MAX_DEPTH - 1];
  DEBUG(map->proc_id, "src_sd_correct=%u, expected_next_op_num=%llu, peeked_op_num=%llu\n",
                      consume_ops, map_stage_next_op_num,
                      src_sd->op_count ? src_sd->ops[0]->op_num : 0);
  if (cur->op_count == 0 && src_sd->op_count == 0 && last_cycle_consumed < cycle_count) {
    // Inaccurate: This would trigger once per cycle even if fetching steady state from one source (the other will be empty)
    STAT_EVENT(map->proc_id, MAP_STAGE_STARVED_2X);
  }
  if(cur->op_count == 0 && consume_ops && last_cycle_consumed < cycle_count) {
    /* call the fetch fill unit */
    prev           = src_sd;
    temp           = cur->ops;
    cur->ops       = prev->ops;
    prev->ops      = temp;
    cur->op_count  = prev->op_count;
    prev->op_count = 0;

    for(ii = 0; ii < cur->op_count; ii++) {
      Op* op = cur->ops[ii];
      ASSERT(map->proc_id, op != NULL);
      op->map_cycle = cycle_count;
    }
    map_stage_next_op_num += cur->op_count;
    last_cycle_consumed = cycle_count;

    STAT_EVENT(map->proc_id, MAP_STAGE_RECEIVED_OPS_0 + cur->op_count);
    ASSERT(map->proc_id, cur->op_count <= 8);
  }

  /* if the last map stage is stalled, don't re-process the ops  */
  if(stall)
    return;

  /* now map the ops in the last map stage */
  for(ii = 0; ii < map->last_sd->op_count; ii++) {
    Op* op = map->last_sd->ops[ii];
    ASSERT(map->proc_id, op != NULL);
    stage_process_op(op);
  }
}


/**************************************************************************************/
/* map_process_op: */

static inline void stage_process_op(Op* op) {
  /* the map stage is currently responsible only for setting wake up lists */
  add_to_wake_up_lists(op, &op->oracle_info, model->wake_hook);
}
