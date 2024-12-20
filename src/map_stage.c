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
 * Author       : HPS Research Group, Litz Lab
 * Date         : 2/4/1999, 12/2024
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
#include "map_rename.h"
#include "map_stage.h"
#include "model.h"
#include "thread.h"
#include "uop_cache.h"
#include "decode_stage.h"
#include "icache_stage.h"

#include "core.param.h"
#include "debug/debug.param.h"
#include "statistics.h"

/**************************************************************************************/
/* Macros */
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MAP_STAGE, ##args)
#define STAGE_MAX_OP_COUNT ISSUE_WIDTH
#define STAGE_MAX_DEPTH MAP_CYCLES


/**************************************************************************************/
/* Global Variables */

Map_Stage* map = NULL;
int map_off_path = 0;

/*
  The next op number is used when deciding whether to consume ops from the uop cache:
  i.e. check if any preceding instructions are still in the decoder.
*/
Counter map_fetch_next_op_num = 1;


/**************************************************************************************/
/* Local Methods */

/* each op in the map stage will allocate register file entries and set wakeup dependencies */
static inline void map_stage_process_op(Op* op) {
  // register renaming allocation
  reg_file_rename(op);

  // setting wake up lists
  add_to_wake_up_lists(op, &op->oracle_info, model->wake_hook);
}

/* TODO: update counters */
static inline void map_stage_update_stat(Flag last_stage_stall, Stage_Data* sd) {
  if (map_off_path) {
    STAT_EVENT(map->proc_id, MAP_STAGE_OFF_PATH);
    return;
  }

  if (last_stage_stall)
    STAT_EVENT(map->proc_id, MAP_STAGE_STALLED);
  else
    STAT_EVENT(map->proc_id, MAP_STAGE_NOT_STALLED);

  if (sd == NULL)
    STAT_EVENT(map->proc_id, MAP_STAGE_STARVED);
  else
    STAT_EVENT(map->proc_id, MAP_STAGE_NOT_STARVED);
}

/* shift the ops to the start of the stage_data op array */
static inline void map_stage_shift_ops(Stage_Data* sd) {
  if (!sd || sd->op_count == 0)
    return;

  int insert_idx = 0;
  for (int ii = 0; ii < sd->max_op_count; ii++) {
    if (!sd->ops[ii])
      continue;

    sd->ops[insert_idx] = sd->ops[ii];
    if (ii != insert_idx)
      sd->ops[ii] = NULL;
    insert_idx++;
  }
}

/* fill the map stage with op from src at fetch_idx */
static inline Flag map_stage_fetch_op(Stage_Data* src_sd, int* fetch_idx) {
  if (*fetch_idx == src_sd->max_op_count)
    return FALSE;

  Op* op = src_sd->ops[*fetch_idx];
  if (!op || op->op_num != map_fetch_next_op_num)
    return FALSE;

  DEBUG(map->proc_id, "Fetching opnum=%llu from %s at idx=%i\n", op->op_num, src_sd->name, *fetch_idx);
  if (!op->decode_cycle)
    decode_stage_process_op(op);

  op->map_cycle = cycle_count;
  map->first_sd->ops[map->first_sd->op_count++] = op;
  src_sd->ops[*fetch_idx] = NULL;
  src_sd->op_count--;
  map_fetch_next_op_num++;
  *fetch_idx = *fetch_idx + 1;
  return TRUE;
}

/*
  - the next op to be consumed by the map stage is from either the decode stage or the uop cache source
  - only consume if older ops have already been consumed by this stage
*/
static inline void map_stage_decide_primary_src(Stage_Data* dec_src_sd, Stage_Data* uopq_src_sd,
                                                Stage_Data** primary_sd_ptr, Stage_Data** secondary_sd_ptr) {
  // when the uop cache is disabled, the next op to be consumed by the map stage is from the decode stage.
  if (!UOP_CACHE_ENABLE) {
    ASSERT(map->proc_id, uopq_src_sd == NULL);
    if (dec_src_sd->op_count == 0) {
      return;
    }

    ASSERT(map->proc_id, dec_src_sd->ops[0]->op_num == map_fetch_next_op_num);
    *primary_sd_ptr = dec_src_sd;
    return;
  }

  /*
    - The uop cache source is either the uop queue or the icache stage uopc stage data bypassing the uop queue
    - The map stage may consume multiple ops in one cycle from both the map stage and the uop cache source if allowed
    - Only consume all ops from this stage if the other sd has them ready; otherwise only the first few
  */
  ASSERT(map->proc_id, uopq_src_sd != NULL);
  if (dec_src_sd->op_count && dec_src_sd->ops[0]->op_num == map_fetch_next_op_num) {
    *primary_sd_ptr = dec_src_sd;
    *secondary_sd_ptr = uopq_src_sd;
    return;
  }

  if (uopq_src_sd->op_count && uopq_src_sd->ops[0]->op_num == map_fetch_next_op_num) {
    *primary_sd_ptr = uopq_src_sd;
    *secondary_sd_ptr = dec_src_sd;
    return;
  }
}

/* determine if consume ops from the secondary src stage_data */
static inline Flag map_stage_decide_fetch_both(Stage_Data* primary_sd, Stage_Data* secondary_sd) {
  if (!UOP_CACHE_ENABLE || !MAP_CONSUME_FROM_BOTH_SRCS)
    return FALSE;

  if (primary_sd == NULL || secondary_sd == NULL)
    return FALSE;

  return map->first_sd->max_op_count >= primary_sd->op_count + secondary_sd->op_count;
}

/* consume only from one src stage_data, unless if_fetch_both is active, then consume interleaved */
static inline void map_stage_consume_src(Stage_Data* primary_sd, Stage_Data* secondary_sd, Flag if_fetch_both) {
  if (map->first_sd->op_count != 0 || primary_sd == NULL)
    return;

  int fetch_idx_selected = 0;
  int fetch_idx_optional = 0;
  Flag if_fetched = FALSE;

  do {
    if_fetched = map_stage_fetch_op(primary_sd, &fetch_idx_selected);
    if (if_fetch_both) {
      if_fetched = map_stage_fetch_op(secondary_sd, &fetch_idx_optional) || if_fetched;
    }
  } while (if_fetched);

  for (int ii = 0; ii < map->first_sd->op_count; ii++) {
    Op* op = map->first_sd->ops[ii];
    if (op && op->off_path)
      map_off_path = 1;
  }

  // do not count number of on-path ops as any stage can receive a mix of on/off-path ops in a single cycle
  if (!map_off_path)
    STAT_EVENT(map->proc_id, MAP_STAGE_RECEIVED_OPS_0 + map->first_sd->op_count);
  ASSERT(map->proc_id, map->first_sd->op_count <= MAP_STAGE_RECEIVED_OPS_MAX);

  // shift ops to array start after fetching
  map_stage_shift_ops(primary_sd);
  if (if_fetch_both)
    map_stage_shift_ops(secondary_sd);
}


/**************************************************************************************/
/* External Methods */

void set_map_stage(Map_Stage* new_map) {
  map = new_map;
}


void init_map_stage(uns8 proc_id, const char* name) {
  ASSERT(proc_id, map && STAGE_MAX_DEPTH > 0);
  ASSERT(proc_id, STAGE_MAX_OP_COUNT >= IC_ISSUE_WIDTH);
  ASSERT(proc_id, !UOP_CACHE_ENABLE || STAGE_MAX_OP_COUNT >= UOPC_ISSUE_WIDTH);
  DEBUG(proc_id, "Initializing %s stage\n", name);

  memset(map, 0, sizeof(Map_Stage));
  map->proc_id = proc_id;

  map->sds = (Stage_Data*)malloc(sizeof(Stage_Data) * STAGE_MAX_DEPTH);
  for (uns ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    char tmp_name[MAX_STR_LENGTH + 1];
    snprintf(tmp_name, MAX_STR_LENGTH, "%s %d", name, STAGE_MAX_DEPTH - ii - 1);
    cur->name = (char*)strdup(tmp_name);
    cur->max_op_count = STAGE_MAX_OP_COUNT;
    cur->ops = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);
  }

  map->last_sd = &map->sds[0];
  map->first_sd = &map->sds[STAGE_MAX_DEPTH - 1];

  reset_map_stage();
}


void reset_map_stage() {
  ASSERT(0, map);
  for (uns ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    cur->op_count = 0;
    for (uns jj = 0; jj < STAGE_MAX_OP_COUNT; jj++)
      cur->ops[jj] = NULL;
  }
}


void recover_map_stage() {
  ASSERT(0, map);
  map_off_path = 0;

  for (uns ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    cur->op_count = 0;

    for (uns jj = 0, kk = 0; jj < STAGE_MAX_OP_COUNT; jj++) {
      if (cur->ops[jj] == NULL)
        continue;

      if (FLUSH_OP(cur->ops[jj])) {
        free_op(cur->ops[jj]);
        cur->ops[jj] = NULL;
        continue;
      }

      // collapse the ops
      Op* op = cur->ops[jj];
      cur->op_count++;
      cur->ops[jj] = NULL;
      cur->ops[kk++] = op;
    }
  }

  if (map_fetch_next_op_num > bp_recovery_info->recovery_op_num) {
    map_fetch_next_op_num = bp_recovery_info->recovery_op_num + 1;
    DEBUG(map->proc_id, "Recovering map_fetch_next_op_num to %llu\n", map_fetch_next_op_num);
  }
}


void debug_map_stage() {
  for (uns ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[STAGE_MAX_DEPTH - ii - 1];
    DPRINTF("# %-10s  op_count:%d\n", cur->name, cur->op_count);
    print_op_array(GLOBAL_DEBUG_STREAM, cur->ops, STAGE_MAX_OP_COUNT, STAGE_MAX_OP_COUNT);
  }
}


void update_map_stage(Stage_Data* dec_src_sd, Stage_Data* uopq_src_sd) {
  /* stall if the renaming table is full */
  if (!reg_file_available(STAGE_MAX_OP_COUNT)) {
    STAT_EVENT(map->proc_id, MAP_STAGE_STALL_ITSELF);
    return;
  }
  STAT_EVENT(map->proc_id, MAP_STAGE_NOT_STALL_ITSELF);


  /* do all the intermediate stages */
  // get the last stage stall flag before shifting
  Flag last_stage_stall = (map->last_sd->op_count > 0);

  // shift ops from previous stages to the current stage if available
  for (uns ii = 0; ii < STAGE_MAX_DEPTH - 1; ii++) {
    Stage_Data *curr = &map->sds[ii];
    if (curr->op_count)
      continue;

    Stage_Data *prev = &map->sds[ii + 1];
    Op** temp = curr->ops;
    curr->ops = prev->ops;
    prev->ops = temp;
    curr->op_count = prev->op_count;
    prev->op_count = 0;
  }


  /* do the first map stage */
  // uops can be received from either the decoder or directly from the uop cache via the uop queue
  Stage_Data* primary_sd = NULL;
  Stage_Data* secondary_sd = NULL;

  // determine which src stage_data to be the primary src being consumed
  map_stage_decide_primary_src(dec_src_sd, uopq_src_sd, &primary_sd, &secondary_sd);

  // determine if consume ops from the secondary src stage_data
  Flag if_fetch_both = map_stage_decide_fetch_both(primary_sd, secondary_sd);

  // update the stall stat
  map_stage_update_stat(last_stage_stall, primary_sd);

  // move ops from the src stage_data to the first stage
  map_stage_consume_src(primary_sd, secondary_sd, if_fetch_both);


  /* do the last map stage */
  // if the last map stage is stalled, don't re-process the ops
  if (last_stage_stall) {
    return;
  }

  // now map dependencies and allocate registers for the ops in the last map stage
  for (uns ii = 0; ii < map->last_sd->op_count; ii++) {
    Op* op = map->last_sd->ops[ii];
    ASSERT(map->proc_id, op != NULL);
    map_stage_process_op(op);
  }
}

