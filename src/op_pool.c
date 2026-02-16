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
* File         : op_pool.c
* Author       : HPS Research Group
* Date         : 1/28/1998
* Description  : This file contains functions for maintaining a pool of active
Ops, thus eliminating dynamic allocation all over the place.  Basically, it
allocates them once and then hands out pointers every time 'alloc_op' is called.
***************************************************************************************/

#include "op_pool.h"

#include <stddef.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/pipeview.h"

#include "bp/bp.param.h"
#include "core.param.h"
#include "general.param.h"

#include "bp/bp.h"
#include "frontend/frontend_intf.h"
#include "frontend/pin_trace_fe.h"

#include "map.h"
#include "model.h"
#include "sim.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_OP_POOL, ##args)
#define DEBUGU(proc_id, args...) _DEBUGU(proc_id, DEBUG_OP_POOL, ##args)

// TODO: it should be increased to 512 to use more than 50,000 FDIP lookahead buffer entries
#define OP_POOL_ENTRIES_INC 128 /* default 128 */

/**************************************************************************************/
/* Global variables */

uns op_pool_entries = 0;
uns op_pool_active_ops = 0;
static Op* op_pool_free_head;

Op invalid_op;

/**************************************************************************************/
/* Prototypes */

static inline void expand_op_pool(void);

/**************************************************************************************/
/* init_op_pool: */

void init_op_pool() {
  DEBUGU(0, "Initializing op pool...\n");

  /* set up invalid op (for use as default value various places) */
  op_pool_init_op(&invalid_op);
  invalid_op.op_pool_valid = FALSE;
  invalid_op.op_num = 0;
  invalid_op.unique_num = 0;

  /* clear counters */
  reset_op_pool();

  /* allocate memory for op pool */
  expand_op_pool();
}

/**************************************************************************************/
/* reset_op_pool:  */

void reset_op_pool() {
  DEBUGU(0, "Resetting op pool...\n");
  op_pool_entries = 0;
  op_pool_active_ops = 0;
}

/**************************************************************************************/
/* alloc_op:  returns a pointer to the next available op */

Op* alloc_op(uns proc_id, uns bp_id) {
  Op* new_op;

  if (op_pool_free_head == NULL) {
    ASSERT(0, op_pool_active_ops == op_pool_entries);
    expand_op_pool();
  }

  new_op = op_pool_free_head;
  ASSERT(0, !new_op->op_pool_valid);
  new_op->op_pool_valid = TRUE;

  op_pool_setup_op(proc_id, bp_id, new_op);

  op_pool_active_ops++;
  DEBUG(0, "Allocating op  id:%u  op_pool_active_ops:%u  op_pool_entries:%d\n", new_op->op_pool_id, op_pool_active_ops,
        op_pool_entries);
  op_pool_free_head = new_op->op_pool_next;

  return new_op;
}

/**************************************************************************************/
/* free_op:  "frees" an op */

void free_op(Op* op) {
  ASSERT(0, op);
  ASSERT(0, op->op_pool_valid);
  ASSERT(0, !op->marked);

  if (PIPEVIEW)
    pipeview_print_op(op);

  op->op_pool_valid = FALSE;
  op_pool_active_ops--;
  ASSERTM(0, op_pool_active_ops >= 0, "op_pool_active_ops:%u\n", op_pool_active_ops);
  DEBUG(0, "Freed op  id:%u  op_pool_active_ops: %u\n", op->op_pool_id, op_pool_active_ops);

  if (op->sched_info)
    free(op->sched_info);

  if (op->table_info->mem_type == MEM_ST)
    delete_store_hash_entry(op);

  if (op->inst_info && op->inst_info->fake_inst) {
    ASSERT(0, op->table_info == op->inst_info->table_info);
    // we no longer allocate memory for fake nops
    // free(op->inst_info->table_info);
    free(op->inst_info);
    op->inst_info = NULL;
  }

  op->op_pool_next = op_pool_free_head;
  op_pool_free_head = op;
  free_wake_up_list(op);
}

/**************************************************************************************/
/* op_pool_init_op: this function is called only once per op
   struct---when it is first allocated.  Intialization put in here
   should be for things that never change. */

void op_pool_init_op(Op* op) {
  op->oracle_info.mispred = FALSE;
  op->oracle_info.misfetch = FALSE;
}

/**************************************************************************************/
/* op_pool_init_op: this function is called every time an op is
   taken from the pool to be used */

void op_pool_setup_op(uns proc_id, uns bp_id, Op* op) {
  uns ii, jj;
  /* only initialize here what is independent of the engine (the
     rest should be in the fetch stage) */
  size_t clear_off = offsetof(Op, proc_id);
  memset((char*)op + clear_off, 0, sizeof(*op) - clear_off);
  op->op_num = op_count[proc_id];
  op->unique_num = unique_count;
  op->unique_num_per_proc = unique_count_per_core[proc_id];
  op->proc_id = proc_id;
  op->bp_id = bp_id;
  op->state = OS_FETCHED;
  op->fu_num = -1;
  op->fetch_cycle = MAX_CTR;
  op->bp_cycle = MAX_CTR;
  op->issue_cycle = MAX_CTR;
  op->map_cycle = MAX_CTR;
  op->rdy_cycle = 1;
  op->sched_cycle = MAX_CTR;
  op->exec_cycle = MAX_CTR;
  op->dcache_cycle = MAX_CTR;
  op->done_cycle = MAX_CTR;
  op->retire_cycle = MAX_CTR;
  op->replay_cycle = MAX_CTR;
  op->pred_cycle = MAX_CTR;
  op->precommit_cycle = MAX_CTR;
  op->wake_cycle = MAX_CTR;

  /* pipelined scheduler fields */
  op->chkpt_num = MAX_CTR;
  op->node_id = MAX_CTR;
  op->rs_id = MAX_CTR;

  op->oracle_cp_num = -1;

  for (ii = 0; ii < MAX_SRCS; ++ii) {
    for (jj = 0; jj < REG_TABLE_TYPE_NUM; ++jj) {
      op->src_reg_id[ii][jj] = -1;
    }
  }

  for (ii = 0; ii < MAX_DESTS; ++ii) {
    for (jj = 0; jj < REG_TABLE_TYPE_NUM; ++jj) {
      op->dst_reg_id[ii][jj] = -1;
      op->prev_dst_reg_id[ii][jj] = -1;
    }
  }
}

/**************************************************************************************/
/* expand_op_pool: */

static inline void expand_op_pool() {
  Op* new_pool = (Op*)calloc(OP_POOL_ENTRIES_INC, sizeof(Op));
  uns ii;

  DEBUGU(0, "Expanding op pool to size %d\n", op_pool_entries + OP_POOL_ENTRIES_INC);
  for (ii = 0; ii < OP_POOL_ENTRIES_INC - 1; ii++) {
    new_pool[ii].op_pool_valid = FALSE;
    new_pool[ii].op_pool_next = &new_pool[ii + 1];
    new_pool[ii].op_pool_id = op_pool_entries++;
    op_pool_init_op(&new_pool[ii]);
  }
  new_pool[ii].op_pool_valid = FALSE;
  new_pool[ii].op_pool_next = op_pool_free_head;
  new_pool[ii].op_pool_id = op_pool_entries++;
  op_pool_init_op(&new_pool[ii]);

  op_pool_free_head = &new_pool[0];
  ASSERT(0, op_pool_entries <= OP_POOL_ENTRIES_INC * 128);
}
