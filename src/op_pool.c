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
#include <stdlib.h>
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

#include "dyn_inst.h"
#include "map.h"
#include "model.h"
#include "op_info.h"
#include "sim.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_OP_POOL, ##args)
#define DEBUGU(proc_id, args...) _DEBUGU(proc_id, DEBUG_OP_POOL, ##args)

// TODO: it should be increased to 512 to use more than 50,000 FDIP lookahead buffer entries
// Also need to increase if enabling large lookahead buffer
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
/* Dynamic_Inst pool: per-dynamic-macro grouping of uop ops, recycled through a simple free list
   (mirrors the op pool's reuse pattern). The instance is released when its eom uop is freed. */

static Dynamic_Inst* dyn_inst_free_list = NULL;

Dynamic_Inst* alloc_dyn_inst(void) {
  Dynamic_Inst* di = dyn_inst_free_list;
  if (di) {
    dyn_inst_free_list = di->free_list_next;
    memset(di, 0, sizeof(*di));
  } else {
    di = (Dynamic_Inst*)calloc(1, sizeof(Dynamic_Inst));
  }
  return di;
}

void dyn_inst_attach(Dynamic_Inst* di, Op* op) {
  ASSERT(op->proc_id, di && op->uop && op->uop->uop_seq_num < op->inst->num_uop);
  op->dyn_inst = di;
  di->uops[op->uop->uop_seq_num] = op;
}

void free_dyn_inst(Dynamic_Inst* di) {
  ASSERT(0, di);
  di->free_list_next = dyn_inst_free_list;
  dyn_inst_free_list = di;
}

/**************************************************************************************/
/* Prediction/recovery state. Ops that can resteer (see op_needs_pred_state) get a pooled instance,
   cleared on hand-out; every other op points at OP_PRED_ZERO and so reads zeros without anything
   being allocated or cleared per op. Nothing may store prediction state through OP_PRED_ZERO --
   one op doing so would hand every other op stale state, so the debug build checks it. */

#ifdef SCARAB_PRED_ZERO_GUARD
/* Build with -DSCARAB_PRED_ZERO_GUARD to map the shared instance read-only: a write through it
   then faults with a live stack, which also catches stores of zero the memcmp check cannot see. */
#include <sys/mman.h>
static Op_Pred_State* op_pred_zero_p = NULL;
static void op_pred_zero_init(void) {
  size_t sz = (sizeof(Op_Pred_State) + 4095) & ~(size_t)4095;
  op_pred_zero_p = (Op_Pred_State*)mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT(0, op_pred_zero_p != MAP_FAILED);
  ASSERT(0, !mprotect(op_pred_zero_p, sz, PROT_READ));
}
#define OP_PRED_ZERO (op_pred_zero_p ? op_pred_zero_p : (op_pred_zero_init(), op_pred_zero_p))
#else
static Op_Pred_State op_pred_zero_storage; /* permanently zero; never written through */
#define OP_PRED_ZERO (&op_pred_zero_storage)
#endif
static Op_Pred_State* op_pred_free_list = NULL;

void op_pred_attach(Op* op) {
  if (!op_needs_pred_state(op)) {
#ifndef NO_DEBUG
    static const Op_Pred_State all_zero;
    ASSERTM(op->proc_id, !memcmp(OP_PRED_ZERO, &all_zero, sizeof(all_zero)),
            "op_pred_zero was written through: an op without prediction state stored into it\n");
#endif
    op->pred = OP_PRED_ZERO;
    return;
  }

  Op_Pred_State* st = op_pred_free_list;
  if (st) {
    op_pred_free_list = st->free_list_next;
    memset(st, 0, sizeof(*st));
  } else {
    st = (Op_Pred_State*)calloc(1, sizeof(*st));
  }
  op->pred = st;
}

static inline void op_pred_release(Op* op) {
  if (op->pred == OP_PRED_ZERO)
    return;
  op->pred->free_list_next = op_pred_free_list;
  op_pred_free_list = op->pred;
  op->pred = OP_PRED_ZERO;
}

/* alloc_op:  returns a pointer to the next available op */

Op* alloc_op(uns proc_id) {
  Op* new_op;

  if (op_pool_free_head == NULL) {
    ASSERT(0, op_pool_active_ops == op_pool_entries);
    expand_op_pool();
  }

  new_op = op_pool_free_head;
  ASSERT(0, !new_op->op_pool_valid);
  new_op->op_pool_valid = TRUE;

  op_pool_setup_op(proc_id, new_op);

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

  if (op->inst && op->uop->mem_type == MEM_ST)
    delete_store_hash_entry(op);

  if (op->inst && op->inst->fake_inst) {
    // fake ops get their own (non-interned) static structs; free both
    free(op->inst);
    free(op->uop);
    op->inst = NULL;
    op->uop = NULL;
  }

  // The macro's dynamic instance is shared by all its uops; release it once, when its eom (the last
  // uop to be freed, since uops retire/free in order) goes. Other uops just drop their pointer.
  if (op->dyn_inst) {
    if (op->eom)
      free_dyn_inst(op->dyn_inst);
    op->dyn_inst = NULL;
  }

  op_sources_free(op);
  op_pred_release(op);

  op->op_pool_next = op_pool_free_head;
  op_pool_free_head = op;
  free_wake_up_list(op);
}

/**************************************************************************************/
/* op_pool_init_op: this function is called only once per op
   struct---when it is first allocated.  Intialization put in here
   should be for things that never change. */

void op_pool_init_op(Op* op) {
}

/**************************************************************************************/
/* op_pool_init_op: this function is called every time an op is
   taken from the pool to be used */

void op_pool_setup_op(uns proc_id, Op* op) {
  uns ii, jj;
  /* only initialize here what is independent of the engine (the
     rest should be in the fetch stage) */
  size_t clear_off = offsetof(Op, proc_id);
  memset((char*)op + clear_off, 0, sizeof(*op) - clear_off);
  op->op_num = op_count[proc_id];
  op->unique_num = unique_count;
  op->unique_num_per_proc = unique_count_per_core[proc_id];
  op->proc_id = proc_id;
  op->state = OS_FETCHED;
  op->fu_num = -1;
  /* reset the per-op cycle counters to their sentinels (op_set_<name>_cycle
   * asserts against MAX_CTR; rdy_cycle is the accumulator and starts at 1). */
  op->cycles.fetch_cycle = MAX_CTR;
  op->cycles.bp_cycle = MAX_CTR;
  op->cycles.issue_cycle = MAX_CTR;
  op->cycles.map_cycle = MAX_CTR;
  op->cycles.rdy_cycle = 1;
  op->cycles.sched_cycle = MAX_CTR;
  op->cycles.exec_cycle = MAX_CTR;
  op->cycles.dcache_cycle = MAX_CTR;
  op->cycles.done_cycle = MAX_CTR;
  op->cycles.retire_cycle = MAX_CTR;
  op->cycles.replay_cycle = MAX_CTR;
  op->cycles.pred_cycle = MAX_CTR;
  op->cycles.precommit_cycle = MAX_CTR;
  op->cycles.decode_cycle = MAX_CTR;
  op->cycles.wake_cycle = MAX_CTR;

  /* pipelined scheduler fields */
  op->chkpt_num = MAX_CTR;
  op->node_id = MAX_CTR;
  op->queue_id = MAX_UNS16;
  op->queue_entry_id = MAX_UNS16;

  op->bp_pred_info = NULL;
  op->btb_pred_info = NULL;
  op->pred = OP_PRED_ZERO; /* until op_pred_attach() decides, reads see zeros */

  for (ii = 0; ii < MAX_SRCS; ++ii) {
    for (jj = 0; jj < REG_TABLE_TYPE_NUM; ++jj) {
      op->src_reg_id[ii][jj] = OP_REG_ID_INVALID;
    }
  }

  for (ii = 0; ii < MAX_DESTS; ++ii) {
    for (jj = 0; jj < REG_TABLE_TYPE_NUM; ++jj) {
      op->dst_reg_id[ii][jj] = OP_REG_ID_INVALID;
      op->prev_dst_reg_id[ii][jj] = OP_REG_ID_INVALID;
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
