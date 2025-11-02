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
 * File         : node_stage.c
 * Author       : HPS Research Group
 * Date         : 1/28/1999
 * Description  :
 ***************************************************************************************/

#include "node_stage.h"

#include <unistd.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "debug/memview.h"

#include "bp/bp.param.h"
#include "core.param.h"
#include "memory/memory.param.h"

#include "bp/bp.h"
#include "bp/tagescl.h"
#include "frontend/frontend.h"
#include "memory/memory.h"

#include "decoupled_frontend.h"
#include "exec_ports.h"
#include "ft.h"
#include "lsq.h"
#include "map.h"
#include "map_rename.h"
#include "node_issue_queue.h"
#include "op_pool.h"
#include "sim.h"
#include "statistics.h"
#include "thread.h"
#include "xed-iclass-enum.h"

/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_NODE_STAGE, ##args)
#define PRINT_RETIRED_UOP(proc_id, args...) _DEBUG_LEAN(proc_id, DEBUG_RETIRED_UOPS, ##args)

#define DEBUG_NODE_WIDTH ISSUE_WIDTH
#define OP_IS_IN_RS(op) (op->state >= OS_IN_RS && op->state < OS_SCHEDULED)

/**************************************************************************************/
/* Global Variables */

Node_Stage* node = NULL;
Rob_Stall_Reason rob_stall_reason = ROB_STALL_NONE;
Rob_Block_Issue_Reason rob_block_issue_reason = ROB_BLOCK_ISSUE_NONE;

/**************************************************************************************/
/* Prototypes */

void debug_print_retired_uop(Op* op);
void flush_ready_list(void);
void flush_scheduling_buffer(void);
void flush_rs(void);
void flush_window(void);
void debug_print_node_table(void);
void debug_print_rs(void);
void debug_print_ready_list(void);
Flag op_not_ready_for_retire(Op* op);
Flag is_node_table_empty(void);
void collect_not_ready_to_retire_stats(Op* op);
Flag is_node_table_full(void);
void collect_node_table_full_stats(Op* op);

void node_fill_rob(Stage_Data*);
void node_retire(void);

void node_precommit_update(void);
void node_precommit_retire(Op* op);

void node_fuse_op(Op* op);

/**************************************************************************************/
/* set_node_stage:*/

void set_node_stage(Node_Stage* new_node) {
  node = new_node;
}

/**************************************************************************************/
/* init_node_stage:*/

void init_node_stage(uns8 proc_id, const char* name) {
  ASSERT(proc_id, node);
  DEBUG(proc_id, "Initializing %s stage\n", name);

  node->proc_id = proc_id;
  node->sd.name = (char*)strdup(name);

  // allocate wires to functional units
  node->sd.max_op_count = NUM_FUS;  // Bandwidth between schedule and FUS
  node->sd.ops = (Op**)malloc(sizeof(Op*) * node->sd.max_op_count);
  // allocate FU to RS mapping array
  node->fu_to_rs_map = (int32*)malloc(sizeof(int32) * NUM_FUS);

  reset_node_stage();
}

/**************************************************************************************/
/* reset_node_stage:*/

void reset_node_stage() {
  uns ii;
  for (ii = 0; ii < NUM_FUS; ii++) {
    node->sd.ops[ii] = NULL;
    node->fu_to_rs_map[ii] = -1;  // Initialize to invalid RS ID
  }
  node->sd.op_count = 0;

  node->node_head = NULL;
  node->node_tail = NULL;
  node->rdy_head = NULL;
  node->next_op_into_rs = NULL;

  node->node_count = 0;
  node->ret_op = 1;
  node->last_scheduled_opnum = 0;
  node->mem_blocked = FALSE;
  node->mem_block_length = 0;
  node->ret_stall_length = 0;

  node->node_precommit = NULL;
  node->prev_op_fusable = FALSE;
}

/**************************************************************************************/
/* reset_node_stage:*/
// CMP used for bogus run: may be combined with reset_node_stage
void reset_all_ops_node_stage() {
  /* TODO: re-consider for multi-core mode */
  reset_node_stage();
}

/**************************************************************************************/
/* recover_node_stage:*/

void recover_node_stage() {
  ASSERT(node->proc_id, node->proc_id == bp_recovery_info->proc_id);

  DEBUG(node->proc_id, "Recovering '%s' stage\n", node->sd.name);
  if (ENABLE_GLOBAL_DEBUG_PRINT && DEBUG_NODE_STAGE && DEBUG_RANGE_COND(node->proc_id))
    debug_node_stage();

  flush_ready_list();
  flush_scheduling_buffer();
  flush_rs();
  flush_window();

  // recover last_scheduled_opnum
  if (node->last_scheduled_opnum >= bp_recovery_info->recovery_op_num)
    node->last_scheduled_opnum = bp_recovery_info->recovery_op_num;

  if (ENABLE_GLOBAL_DEBUG_PRINT && DEBUG_NODE_STAGE && DEBUG_RANGE_COND(node->proc_id))
    debug_node_stage();
}

void flush_ready_list() {
  Op* op;
  Op** last;
  for (op = node->rdy_head, last = &node->rdy_head; op; op = op->next_rdy) {
    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    if (FLUSH_OP(op)) {
      ASSERT(node->proc_id, op->op_num > bp_recovery_info->recovery_op_num);
      *last = op->next_rdy;
      op->in_rdy_list = FALSE;
    } else
      last = &op->next_rdy;
  }
}

void flush_scheduling_buffer() {
  uns ii;
  for (ii = 0; ii < node->sd.max_op_count; ii++) {
    Op* op = node->sd.ops[ii];
    if (op && FLUSH_OP(op)) {
      ASSERT(node->proc_id, node->proc_id == op->proc_id);
      ASSERTM(node->proc_id, op->op_num > bp_recovery_info->recovery_op_num, "op_num:%s\n", unsstr64(op->op_num));

      node->sd.ops[ii] = NULL;
      node->sd.op_count--;

      ASSERT(node->proc_id, node->sd.op_count >= 0);
    }
  }
}

void flush_rs() {
  Op* op = node->next_op_into_rs;
  if (op && FLUSH_OP(op)) {
    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    ASSERTM(node->proc_id, op->op_num > bp_recovery_info->recovery_op_num, "op_num:%s\n", unsstr64(op->op_num));
    node->next_op_into_rs = NULL;  // all later ops will also be flushed
  }
}

void flush_window() {
  Op* op;
  Op** last;
  uns flush_ops = 0;
  uns keep_ops = 0;

  node->node_tail = NULL;
  for (op = node->node_head, last = &node->node_head; op; op = *last) {
    ASSERT(node->proc_id, node->proc_id == op->proc_id);

    if (FLUSH_OP(op)) {
      DEBUG(node->proc_id, "Node flushing  op:%s\n", unsstr64(op->op_num));
      if (!op->macro_fused)
        flush_ops++;
      ASSERT(node->proc_id, op->op_num > bp_recovery_info->recovery_op_num);
      op->in_node_list = FALSE;
      *last = op->next_node;
      if (op->state == OS_IN_RS || op->state == OS_READY || op->state == OS_WAIT_FWD) {
        ASSERT(op->proc_id, node->rs[op->rs_id].rs_op_count > 0);
        node->rs[op->rs_id].rs_op_count--;
      }
      ft_free_op(op);
    } else {
      /* Keep op */

      if (IS_FLUSHING_OP(op)) {
        /* Mark that the scheduled recovery has occurred */
        op->recovery_scheduled = FALSE;
      }
      DEBUG(node->proc_id, "Node keeping  op:%s node_id:%llu\n", unsstr64(op->op_num), op->node_id);
      if (!op->macro_fused)
        keep_ops++;
      last = &op->next_node;
      node->node_tail = op;
    }
  }

  ASSERT(node->proc_id, flush_ops + keep_ops == node->node_count);
  node->node_count = keep_ops;
  ASSERT(node->proc_id, node->node_count <= NODE_TABLE_SIZE);
}

/**************************************************************************************/
/* debug_node_stage:*/

void debug_node_stage() {
  DPRINTF("# %-10s  node_count:%d\n", node->sd.name, node->node_count);

  debug_print_node_table();
  debug_print_rs();
  debug_print_ready_list();
}

void debug_print_node_table() {
  Op* op;

  Counter row = 0;
  Flag empty = TRUE;
  uns32 slot_num = 0;
  uns printed = 0;

  Op** temp = (Op**)calloc(DEBUG_NODE_WIDTH, sizeof(Op*));

  for (op = node->node_head; op; op = op->next_node, ++row) {
    slot_num = row % DEBUG_NODE_WIDTH;
    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    ASSERT(node->proc_id, temp[slot_num] == NULL);
    temp[slot_num] = op;
    printed++;
    empty = FALSE;

    // we have populated entire row, print and reinitialize
    if (slot_num == DEBUG_NODE_WIDTH - 1) {
      if (!empty) {
        print_open_op_array(GLOBAL_DEBUG_STREAM, temp, DEBUG_NODE_WIDTH, DEBUG_NODE_WIDTH);
      }
      // For some reason this does not zero out the entire array.
      // (Assert fails and verified in gdb).
      // memset(temp, 0, DEBUG_NODE_WIDTH * sizeof(Op*));
      for (int i = 0; i < DEBUG_NODE_WIDTH; i++)
        temp[i] = 0;
      empty = TRUE;
    }
  }

  ASSERTM(node->proc_id, printed == node->node_count, "printed=%d, node_count=%d", printed, node->node_count);

  // If node table is empty, print a blank row. Or if there is a remainder, print that too
  if (printed == 0 || slot_num < DEBUG_NODE_WIDTH - 1)
    print_open_op_array(GLOBAL_DEBUG_STREAM, temp, DEBUG_NODE_WIDTH, DEBUG_NODE_WIDTH);

  print_open_op_array_end(GLOBAL_DEBUG_STREAM, DEBUG_NODE_WIDTH);

  free(temp);
}

void debug_print_rs() {
  Op* op;
  uns printed = 0;
  int32 i, j;

  ASSERT(node->proc_id, node->rs);

  for (i = 0; i < NUM_RS; ++i) {
    Reservation_Station* rs = &node->rs[i];
    printed = 0;
    DPRINTF("%s (%d/%s): ", rs->name, rs->rs_op_count, rs->size == 0 ? "inf" : unsstr64((uns64)rs->size));

    for (j = 0; j < rs->num_fus; ++j) {
      DPRINTF("%s, ", rs->connected_fus[j]->name);
    }

    DPRINTF("\n");

    for (op = node->node_head; op && op != node->next_op_into_rs; op = op->next_node) {
      if (op->rs_id == i && OP_IS_IN_RS(op)) {
        // Op belongs to this RS
        DPRINTF("%lld ", op->op_num);
        printed++;
        if (printed % 8 == 0)
          DPRINTF("\n");
      }
    }

    if (printed % 8)
      DPRINTF("\n");

    ASSERTM(node->proc_id, printed == rs->rs_op_count, "printed=%d, rs_op_count=%d\n", printed, rs->rs_op_count);
  }
}

void debug_print_ready_list() {
  Op* op;

  DPRINTF("Ready list:");

  for (op = node->rdy_head; op; op = op->next_rdy) {
    DPRINTF(" %s", unsstr64(op->op_num));
  }

  DPRINTF("\n");

  print_op_array(GLOBAL_DEBUG_STREAM, node->sd.ops, node->sd.max_op_count, node->sd.max_op_count);
}

/**************************************************************************************/
/* node_cycle: */

void update_node_stage(Stage_Data* src_sd) {
  DEBUG(node->proc_id, "Beginning '%s' stage\n", node->sd.name);
  STAT_EVENT(node->proc_id, NODE_CYCLE);
  STAT_EVENT(node->proc_id, POWER_CYCLE);

  /* insert ops coming from the previous stage*/
  node_fill_rob(src_sd);

  /* update the precommit pointer in the ROB */
  node_precommit_update();

  node_issue_queue_update();

  /* get rid of the ops that are finished */
  node_retire();

  memview_core_stall(node->proc_id, is_node_stage_stalled(), node->mem_blocked);
}

/**************************************************************************************/
/* node_fill_rob: This function takes ops from the map stage and allocates them into the node table.
 *    Note, this function does not place the Op in the RS, that is done later.*/

void node_fill_rob(Stage_Data* src_sd) {
  Flag on_path = FALSE;
  uns ii;

  /* if nothing to process, return */
  if (src_sd->op_count == 0)
    return;

  // Go through all the ops in the issue buffer and stick them into the Node Table.
  // We will stick them into the RS later
  for (ii = 0; ii < src_sd->max_op_count; ii++) {
    /* if node table is full, stall */
    if (is_node_table_full()) {
      collect_node_table_full_stats(node->node_head);
      rob_block_issue_reason = ROB_BLOCK_ISSUE_FULL;
      return;
    }
    rob_block_issue_reason = ROB_BLOCK_ISSUE_NONE;

    // If it is not full, issue the next op
    Op* op = src_sd->ops[ii];
    if (!op)
      continue;

    if (op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_ST) {
      if (!lsq_available(op->table_info->mem_type)) {
        STAT_EVENT(op->proc_id, LSQ_FULL_TOTAL);
        STAT_EVENT(op->proc_id, LSQ_FULL_TOTAL + op->table_info->mem_type);
        return;
      }

      lsq_dispatch(op);
    }

    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    /* check if it's a synchronizing op that can't issue  */
    if ((op->table_info->bar_type & BAR_ISSUE) && (node->node_count > 0))
      break;

    /* remove op from previous stage */
    src_sd->ops[ii] = NULL;
    src_sd->op_count--;
    ASSERT(node->proc_id, src_sd->op_count >= 0);

    /* set op fields */
    op->node_id = node->node_count;
    op->issue_cycle = cycle_count;

    /* add to node list & update node state*/
    ASSERT(node->proc_id, !op->in_node_list);
    if (node->node_tail)
      node->node_tail->next_node = op;
    if (node->node_head == NULL)
      node->node_head = op;
    op->next_node = NULL;
    op->in_node_list = TRUE;
    node->node_tail = op;

    if (!node->next_op_into_rs)   /* if there are no ops waiting to enter RS */
      node->next_op_into_rs = op; /* this will be the first one */

    // Jump uop after CMP or TEST will be fused into one uop
    node_fuse_op(op);
    if (!op->macro_fused)
      node->node_count++;

    ASSERTM(node->proc_id, node->node_count <= NODE_TABLE_SIZE,
            "node_count: %d src_max_op_count: %d src_op_count: %d\n", node->node_count, src_sd->max_op_count,
            src_sd->op_count);

    on_path |= !op->off_path;

    DEBUG(node->proc_id, "Issuing the op op_num:%s off_path:%d\n", unsstr64(op->op_num), op->off_path);

    op->state = OS_IN_ROB;

    /* always stop issuing after a synchronizing op */
    if (op->table_info->bar_type & BAR_ISSUE)
      break;
  }
}

/**************************************************************************************/
/* node_retire:*/

void node_retire() {
  uns ret_count = 0;
  Op* op = NULL;

  // If node table is empty, then there is nothing to retire
  if (is_node_table_empty())
    return;

  // Iterate through the first NODE_RET_WIDTH number of ops and try to retire them
  for (op = node->node_head; op && ret_count < NODE_RET_WIDTH; op = op->next_node) {
    ASSERT(node->proc_id, node->proc_id == op->proc_id);

    // check to see if the head of the node table is ready to retire
    if (op_not_ready_for_retire(op)) {
      // op is not ready to retire
      collect_not_ready_to_retire_stats(op);
      break;
    }

    rob_stall_reason = ROB_STALL_NONE;

    /**op is ready to retire**/
    ASSERTM(node->proc_id, op->state != OS_TENTATIVE, "op_num: %llu\n", op->op_num);
    ret_count++;
    DEBUG(node->proc_id, "Retiring op:%llu\n", op->op_num);

    // Debug prints mainly used for testing the uop generation of PIN frontend
    debug_print_retired_uop(op);

    // count number of stall cycles
    STAT_EVENT(node->proc_id, RET_STALL_LENGTH_0 + MIN2(node->ret_stall_length, 5000) / 100);
    if (DIE_ON_RET_STALL_THRESH) {
      // time out code
      if (node->proc_id == DIE_ON_RET_STALL_CORE) {
        ASSERTM(node->proc_id, node->ret_stall_length < DIE_ON_RET_STALL_THRESH,
                "Retire stalled for %u cycles (%llu--%llu)\n", node->ret_stall_length,
                cycle_count - node->ret_stall_length, cycle_count);
      }
    }
    node->ret_stall_length = 0;

    // retire the ops
    Counter real_rdy_cycle = MAX2(op->rdy_cycle, op->issue_cycle);

    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    ASSERT(node->proc_id, op->in_node_list);
    ASSERT(node->proc_id, !op->off_path);
    STAT_EVENT(op->proc_id, OP_WAIT_0 + MIN2(op->sched_cycle - real_rdy_cycle, 31));
    STAT_EVENT(op->proc_id, OP_RETIRED);  // Counts all ops retired, not just those in primary thread

    DEBUG(node->proc_id, "Retiring op_num:%s\n", unsstr64(op->op_num));

    ASSERTM(node->proc_id, op->op_num == node->ret_op, "op_num=%s  ret_op=%s\n", unsstr64(op->op_num),
            unsstr64(node->ret_op));

    if (op->eom) {
      /* We need to retire sys calls, bar fetch instructions, and the last instruction.
       * All other retires are "optional" to release resources in the PIN frontend */
      inst_count[node->proc_id]++;
      STAT_EVENT(op->proc_id, NODE_INST_COUNT);

      if (op->fetched_instruction) {
        inst_count_fetched[node->proc_id]++;
        STAT_EVENT(op->proc_id, NODE_INST_COUNT_FETCHED);
      }

      Flag retire_op = IS_CALLSYS(op->table_info) || op->table_info->bar_type & BAR_FETCH ||
                       (inst_count[node->proc_id] % NODE_RETIRE_RATE == 0);

      if (op->exit) {
        retired_exit[op->proc_id] = TRUE;
        decoupled_fe_retire(op, op->proc_id, -1);
      } else if (retire_op) {
        decoupled_fe_retire(op, op->proc_id, op->inst_uid);
      }
    }
    uop_count[node->proc_id]++;
    STAT_EVENT(op->proc_id, NODE_UOP_COUNT);
    ASSERTM(node->proc_id, uop_count[node->proc_id] == node->ret_op, "%s  %s op_num: %s\n",
            unsstr64(uop_count[node->proc_id]), unsstr64(node->ret_op), unsstr64(op->op_num));

    node->ret_op++;

    STAT_EVENT(op->proc_id, RET_ALL_INST);

    remove_from_seq_op_list(td, op);

    if (op->table_info->cf_type) {
      if (BP_UPDATE_AT_RETIRE) {
        // this code updates the branch prediction structures
        if (op->table_info->cf_type >= CF_IBR)
          bp_target_known_op(g_bp_data, op);

        bp_resolve_op(g_bp_data, op);
      }
      bp_retire_op(g_bp_data, op);
    }

    if (op->table_info->mem_type == MEM_LD && (op->done_cycle - op->sched_cycle) < 5) {
      STAT_EVENT(op->proc_id, LD_EXEC_CYCLES_0 + (op->done_cycle - op->sched_cycle));
    }
    if (op->table_info->mem_type == MEM_LD) {
      STAT_EVENT(op->proc_id, LD_NO_DEPENDENTS + (op->wake_up_head ? 1 : 0));
    }
    STAT_EVENT(op->proc_id, RET_OP_EXEC_COUNT_0 + MIN2(32, op->exec_count));

    op->retire_cycle = cycle_count;

    // free the previous register entries with same architectural destination
    reg_file_commit(op);

    node_precommit_retire(op);

    if (op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_ST) {
      lsq_commit(op);
    }

    if (model->op_retired_hook)
      model->op_retired_hook(op);
    else
      ft_free_op(op);

    // the fused op does not occupy the ROB entry
    if (!op->macro_fused)
      node->node_count--;

    ASSERT(node->proc_id, node->node_count >= 0);
  }

  STAT_EVENT(node->proc_id, ROW_SIZE_0 + ret_count);

  // op should be pointing to first op that was not retired because of the above for-loop
  node->node_head = op;
  if (node->node_head)
    DEBUG(node->proc_id, "Op op_num:%s is now head of the node table\n", unsstr64(node->node_head->op_num));
  if (op == NULL) {
    node->node_tail = NULL;
    ASSERTM(node->proc_id, node->node_count == 0, "Node table must be empty if next node is null!\n");
  }
}

/**************************************************************************************/
/* is_node_stage_stalled: returns TRUE if node table is full and there are no
 * ready ops */

Flag is_node_stage_stalled() {
  return (node->node_count == NODE_TABLE_SIZE) && /* node table is full */
         !node->rdy_head &&                       /* no ready ops */
         !node->next_op_into_rs;                  /* no ops waiting to enter RS */
}

void debug_print_retired_uop(Op* op) {
  PRINT_RETIRED_UOP(node->proc_id, "============================\n");
  PRINT_RETIRED_UOP(node->proc_id, "EIP: 0x%llx\n", op->inst_info->addr);
  PRINT_RETIRED_UOP(node->proc_id, "Op Type: %s\n", Op_Type_str(op->table_info->op_type));
  PRINT_RETIRED_UOP(node->proc_id, "Mem Type: %d\n", op->table_info->mem_type);
  PRINT_RETIRED_UOP(node->proc_id, "CF Type: %d\n", op->table_info->cf_type);
  PRINT_RETIRED_UOP(node->proc_id, "Barrier Type: %d\n", op->table_info->bar_type);
  PRINT_RETIRED_UOP(node->proc_id, "Is SIMD: %d\n", op->table_info->is_simd);
  PRINT_RETIRED_UOP(node->proc_id, "Srcs: ");
  for (uns i = 0; i < op->table_info->num_src_regs; ++i) {
    PRINT_RETIRED_UOP(node->proc_id, "%s ", disasm_reg(op->inst_info->srcs[i].id));
  }
  PRINT_RETIRED_UOP(node->proc_id, "\n");
  PRINT_RETIRED_UOP(node->proc_id, "Dests: ");
  for (uns i = 0; i < op->table_info->num_dest_regs; ++i) {
    PRINT_RETIRED_UOP(node->proc_id, "%s ", disasm_reg(op->inst_info->dests[i].id));
  }
  PRINT_RETIRED_UOP(node->proc_id, "\n");
}

Flag op_not_ready_for_retire(Op* op) {
  return !(op->state == OS_DONE || OP_DONE(op)) || op->off_path || op->recovery_scheduled || op->redirect_scheduled;
}

Flag is_node_table_empty() {
  if (node->node_count == 0) {
    if (node->node_head != NULL) {
      ASSERT(node->proc_id, node->node_head->macro_fused);
      return FALSE;
    }

    ASSERT(node->proc_id, node->node_head == NULL);
    ASSERT(node->proc_id, node->node_tail == NULL);
    return TRUE;
  }

  ASSERT(node->proc_id, node->node_head != NULL);
  ASSERT(node->proc_id, node->node_tail != NULL);
  return FALSE;
}

void collect_not_ready_to_retire_stats(Op* op) {
  rob_stall_reason = ROB_STALL_OTHER;
  if (op->recovery_scheduled) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_RECOVERY;
  } else if (op->redirect_scheduled) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_REDIRECT;
  }

  if (op->engine_info.l1_miss) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_L1_MISS;
    STAT_EVENT(op->proc_id, RET_BLOCKED_L1_MISS);
    Flag bw_prefetch = !op->engine_info.l1_miss_satisfied &&  // op->req is OK to use
                       op->req->demand_match_prefetch && op->req->bw_prefetch;
    Flag bw_prefetchable = !op->engine_info.l1_miss_satisfied &&  // op->req is OK to use
                           !op->req->demand_match_prefetch && op->req->bw_prefetchable;
    if (bw_prefetch || bw_prefetchable)
      STAT_EVENT(op->proc_id, RET_BLOCKED_L1_MISS_BW_PREF);
  }

  if (op->engine_info.l1_miss || op->state == OS_WAIT_MEM) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_MEMORY;
    STAT_EVENT(op->proc_id, RET_BLOCKED_MEM_STALL);
    if (num_offchip_stall_reqs(op->proc_id) > 0) {
      STAT_EVENT(op->proc_id, RET_BLOCKED_OFFCHIP_DEMAND);
    }
  }

  if (op->engine_info.dcmiss) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_DC_MISS;
    STAT_EVENT(op->proc_id, RET_BLOCKED_DC_MISS);
    if (!op->engine_info.l1_miss)
      STAT_EVENT(op->proc_id, RET_BLOCKED_L1_ACCESS);
  }

  node->ret_stall_length++;
}

Flag is_node_table_full() {
  ASSERT(node->proc_id, node->node_count <= NODE_TABLE_SIZE);
  return (node->node_count == NODE_TABLE_SIZE);
}

void collect_node_table_full_stats(Op* op) {
  if (!(op->state == OS_DONE || OP_DONE(op))) {
    if (op->table_info->op_type == OP_ILD || op->table_info->op_type == OP_IST || op->table_info->op_type == OP_FLD ||
        op->table_info->op_type == OP_FST) {
      STAT_EVENT(node->proc_id, FULL_WINDOW_MEM_OP);
    } else if (op->table_info->op_type >= OP_FCVT && op->table_info->op_type <= OP_FCMOV) {
      STAT_EVENT(node->proc_id, FULL_WINDOW_FP_OP);
    } else {
      STAT_EVENT(node->proc_id, FULL_WINDOW_OTHER_OP);
    }
  }

  STAT_EVENT(node->proc_id, FULL_WINDOW_STALL);
}

/**************************************************************************************/
/* node precommit mechanism */

void node_precommit_update(void) {
  Op* op = node->node_head;
  if (node->node_precommit)
    op = node->node_precommit;

  // scan the node table to update the precommit pointer
  uns precommit_count = 0;
  for (; op != NULL && precommit_count < NODE_RET_WIDTH; op = op->next_node) {
    // wait until the results usable for branches
    if (op->table_info->cf_type && op->exec_cycle > cycle_count)
      return;

    // wait until looking up the d-cache for memory operands
    if ((op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_ST) && op->dcache_cycle > cycle_count)
      return;

    if (op->off_path)
      return;

    // avoid multiple precommit for the precommit head
    if (op->precommitted)
      continue;

    precommit_count++;
    node->node_precommit = op;
    op->precommitted = TRUE;
    op->precommit_cycle = cycle_count;

    reg_file_precommit(op);
  }
}

void node_precommit_retire(Op* op) {
  ASSERT(node->proc_id, op->precommitted);
  ASSERT(node->proc_id, op->precommit_cycle <= op->retire_cycle);

  if (!node->node_precommit)
    return;
  ASSERT(node->proc_id, node->node_precommit->op_num >= op->op_num);

  /* clear the precommit pointer when it commits, which indicates that
   * the ROB is empty or all in-flight ops are off-path */
  if (node->node_precommit->op_num != op->op_num)
    return;
  node->node_precommit = NULL;
}

/* Marcro-Fusion op */
void node_fuse_op(Op* op) {
  uns16 op_code = op->inst_info->table_info->true_op_type;

  if (op_code == XED_ICLASS_CMP || op_code == XED_ICLASS_TEST) {
    node->prev_op_fusable = TRUE;
    return;
  }

  if (op_code >= XED_ICLASS_JB && op_code <= XED_ICLASS_JZ) {
    if (node->prev_op_fusable) {
      op->macro_fused = TRUE;
      STAT_EVENT(op->proc_id, OP_MACRO_FUSION_ONPATH + op->off_path);
    }
  }

  node->prev_op_fusable = FALSE;
}
