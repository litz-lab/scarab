/*
 * Copyright 2020 HPS/SAFARI Research Groups
 * Copyright 2025 Litz Lab
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
 * Author       : HPS Research Group, Litz Lab
 * Date         : 2/4/1999, 3/2025
 * Description  :
 ***************************************************************************************/

#include "map_stage.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "core.param.h"
#include "memory/memory.param.h"

#include "bp/bp.h"

#include "ft.h"
#include "map.h"
#include "map_rename.h"
#include "model.h"
#include "op_pool.h"
#include "statistics.h"
#include "thread.h"

/**************************************************************************************/
/* Macros */
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MAP_STAGE, ##args)
#define STAGE_MAX_OP_COUNT ISSUE_WIDTH
#define STAGE_MAX_DEPTH MAP_CYCLES

/**************************************************************************************/
/* Global Variables */

Map_Stage* map = NULL;

int map_off_path = 0;
Counter map_stage_next_op_num = 1;
/* The next op number is used when deciding whether to consume ops from the uop
 * cache: i.e. check if any preceding instructions are still in the decoder. */

/**************************************************************************************/
/* Local prototypes */

static inline void stage_process_op(Op*);
static inline void map_stage_collect_stat(Flag, Flag);
static inline void map_stage_fetch_op(Stage_Data*);

/**************************************************************************************/
/* set_map_stage: */

void set_map_stage(Map_Stage* new_map) {
  map = new_map;
}

/**************************************************************************************/
/* init_map_stage: */

void init_map_stage(uns8 proc_id, const char* name) {
  char tmp_name[MAX_STR_LENGTH + 1];
  uns ii;
  ASSERT(proc_id, map);
  ASSERT(proc_id, STAGE_MAX_DEPTH > 0);
  DEBUG(proc_id, "Initializing %s stage\n", name);

  memset(map, 0, sizeof(Map_Stage));
  map->proc_id = proc_id;

  map->sds = (Stage_Data*)malloc(sizeof(Stage_Data) * STAGE_MAX_DEPTH);
  for (ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    snprintf(tmp_name, MAX_STR_LENGTH, "%s %d", name, STAGE_MAX_DEPTH - ii - 1);
    cur->name = (char*)strdup(tmp_name);
    cur->max_op_count = STAGE_MAX_OP_COUNT;
    cur->ops = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);
  }
  map->last_sd = &map->sds[0];
  reset_map_stage();
}

/**************************************************************************************/
/* reset_map_stage: */

void reset_map_stage() {
  uns ii, jj;
  ASSERT(0, map);
  for (ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    cur->op_count = 0;
    for (jj = 0; jj < STAGE_MAX_OP_COUNT; jj++)
      cur->ops[jj] = NULL;
  }

  map->reg_file_stall = FALSE;
}

/**************************************************************************************/
/* recover_map_stage: */

void recover_map_stage() {
  uns ii, jj, kk;
  map_off_path = 0;
  ASSERT(0, map);
  for (ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    cur->op_count = 0;

    for (jj = 0, kk = 0; jj < STAGE_MAX_OP_COUNT; jj++) {
      if (cur->ops[jj]) {
        if (FLUSH_OP(cur->ops[jj])) {
          ft_free_op(cur->ops[jj]);
          cur->ops[jj] = NULL;
        } else {
          Op* op = cur->ops[jj];
          cur->op_count++;
          cur->ops[jj] = NULL;  // collapse the ops
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
  for (ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[STAGE_MAX_DEPTH - ii - 1];
    DPRINTF("# %-10s  op_count:%d\n", cur->name, cur->op_count);
    print_op_array(GLOBAL_DEBUG_STREAM, cur->ops, STAGE_MAX_OP_COUNT, STAGE_MAX_OP_COUNT);
  }
}

/**************************************************************************************/
/* map_cycle: */

void update_map_stage(Stage_Data* src_sd) {
  /* stall if the renaming table is full */
  if (!reg_file_available(STAGE_MAX_OP_COUNT)) {
    map->reg_file_stall = TRUE;
    STAT_EVENT(map->proc_id, MAP_STAGE_STALL_ITSELF);
    return;
  }
  map->reg_file_stall = FALSE;
  STAT_EVENT(map->proc_id, MAP_STAGE_NOT_STALL_ITSELF);

  Flag stall = (map->last_sd->op_count > 0);
  Flag starved = (src_sd->op_count == 0);
  map_stage_collect_stat(stall, starved);

  /* do all the intermediate stages */
  for (int ii = 0; ii < STAGE_MAX_DEPTH - 1; ii++) {
    Stage_Data* cur = &map->sds[ii];
    Stage_Data* prev = &map->sds[ii + 1];

    if (cur->op_count)
      continue;

    Op** temp = cur->ops;
    cur->ops = prev->ops;
    prev->ops = temp;
    cur->op_count = prev->op_count;
    prev->op_count = 0;
  }

  /* do the first map stage */
  if (map->sds[STAGE_MAX_DEPTH - 1].op_count == 0 && !starved) {
    map_stage_fetch_op(src_sd);
  }

  /* if the last map stage is stalled, don't re-process the ops  */
  if (stall) {
    return;
  }

  /* now map the ops in the last map stage */
  for (int ii = 0; ii < map->last_sd->op_count; ii++) {
    Op* op = map->last_sd->ops[ii];
    ASSERT(map->proc_id, op != NULL);
    stage_process_op(op);
  }
}

/**************************************************************************************/
/* Local methods */

static inline void stage_process_op(Op* op) {
  ASSERT(map->proc_id, map->proc_id == td->proc_id);

  /* add to sequential op list */
  add_to_seq_op_list(td, op);
  ASSERT(map->proc_id, td->seq_op_list.count <= op_pool_active_ops);

  /* map the op based on true dependencies & set information in op->oracle_info */
  thread_map_op(op);
  thread_map_mem_dep(op);

  /* register renaming allocation */
  reg_file_rename(op);

  /* setting wake up lists */
  add_to_wake_up_lists(op, &op->oracle_info, model->wake_hook);
}

static inline void map_stage_collect_stat(Flag stall, Flag starved) {
  if (map_off_path) {
    STAT_EVENT(map->proc_id, MAP_STAGE_OFF_PATH);
    return;
  }

  if (stall)
    STAT_EVENT(map->proc_id, MAP_STAGE_STALLED);
  else
    STAT_EVENT(map->proc_id, MAP_STAGE_NOT_STALLED);

  if (starved)
    STAT_EVENT(map->proc_id, MAP_STAGE_STARVED);
  else
    STAT_EVENT(map->proc_id, MAP_STAGE_NOT_STARVED);
}

static inline void map_stage_fetch_op(Stage_Data* src_sd) {
  Stage_Data* first_sd = &map->sds[STAGE_MAX_DEPTH - 1];
  int op_count_before_fetch = src_sd->op_count;

  for (int ii = 0; ii < op_count_before_fetch; ii++) {
    Op* op = src_sd->ops[ii];
    ASSERT(map->proc_id, op->op_num == map_stage_next_op_num);
    DEBUG(map->proc_id, "Fetching opnum=%llu at idx=%i\n", op->op_num, ii);

    op->map_cycle = cycle_count;
    first_sd->ops[ii] = op;
    first_sd->op_count++;

    src_sd->ops[ii] = NULL;
    src_sd->op_count--;

    map_stage_next_op_num++;
    if (op->off_path) {
      map_off_path = 1;
    }
  }

  // TODO: probably should count number of on-path ops.
  if (!map_off_path)
    STAT_EVENT(map->proc_id, MAP_STAGE_RECEIVED_OPS_0 + first_sd->op_count);

  // Any stage can receive a mix of on/off-path ops in a single cycle.
  ASSERT(map->proc_id, first_sd->op_count <= MAP_STAGE_RECEIVED_OPS_MAX);
}
