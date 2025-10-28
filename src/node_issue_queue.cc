/*
 * Copyright 2025 University of California Santa Cruz
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
 * File         : node_issue_queue.cc
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 4/15/2025
 * Description  :
 ***************************************************************************************/

#include "node_issue_queue.h"

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "memory/memory.h"

#include "exec_ports.h"
#include "node_stage.h"
}

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_NODE_STAGE, ##args)

/**************************************************************************************/
/* Prototypes */

int64 node_dispatch_find_emptiest_rs(Op*);
void node_schedule_oldest_first_sched(Op*);
void node_track_fu_idle_stats();

/**************************************************************************************/
/* Issuers:
 *      The interface to the issue functions is that Scarab will pass the
 * function the op to be issued, and the issuer will return the RS id that the
 * op should be issued to, or -1 meaning that there is no RS for the op to
 * be issued to.
 */

/*
 * FIND_EMPTIEST_RS: will always select the RS with the most empty slots
 */
int64 node_dispatch_find_emptiest_rs(Op* op) {
  int64 emptiest_rs_id = NODE_ISSUE_QUEUE_RS_SLOT_INVALID;
  uns emptiest_rs_slots = 0;

  /*
   * Iterate through RSs looking for an available RS that is connected to
   * an FU that can execute the OP.
   */
  for (int64 rs_id = 0; rs_id < NUM_RS; ++rs_id) {
    Reservation_Station* rs = &node->rs[rs_id];
    ASSERT(node->proc_id, !rs->size || rs->rs_op_count <= rs->size);

    /* TODO: support infinite RS for upper-bound expr */
    ASSERTM(node->proc_id, rs->size, "Infinite RS not suppoted by node_dispatch_find_emptiest_rs issuer.");

    for (uns32 i = 0; i < rs->num_fus; ++i) {
      // find the FU that can execute this op
      Func_Unit* fu = rs->connected_fus[i];
      if (!(get_fu_type(op->table_info->op_type, op->table_info->is_simd) & fu->type)) {
        continue;
      }

      ASSERT(node->proc_id, rs->size >= rs->rs_op_count);
      uns num_empty_slots = rs->size - rs->rs_op_count;
      if (num_empty_slots == 0) {
        continue;
      }

      // find the emptiest RS
      if (emptiest_rs_slots < num_empty_slots) {
        emptiest_rs_id = rs_id;
        emptiest_rs_slots = num_empty_slots;
      }
    }
  }

  return emptiest_rs_id;
}

/**************************************************************************************/
/* Schedulers:
 *      The interface to the schedule functions is that Scarab will pass the
 * function the ready op, and the scheduler will return the selected ops in
 * node->sd. See OLDEST_FIRST_SCHED for an example. Note, it is not necessary
 * to look at FU availability in this stage, if the FU is busy, then the op
 * will be ignored and available to schedule again in the next stage.
 */

/*
 * OLDEST_FIRST_SCHED: will always select the oldest ready ops to schedule
 */
void node_schedule_oldest_first_sched(Op* op) {
  int32 youngest_slot_op_id = NODE_ISSUE_QUEUE_FU_SLOT_INVALID;

  // Iterate through the FUs that this RS is connected to.
  Reservation_Station* rs = &node->rs[op->rs_id];
  for (uns32 i = 0; i < rs->num_fus; ++i) {
    // check if this op can be executed by this FU
    Func_Unit* fu = rs->connected_fus[i];
    if (!(get_fu_type(op->table_info->op_type, op->table_info->is_simd) & fu->type)) {
      continue;
    }

    uns32 fu_id = fu->fu_id;
    Op* s_op = node->sd.ops[fu_id];

    // nobody has been scheduled to this FU yet
    if (!s_op) {
      DEBUG(node->proc_id, "Scheduler selecting    op_num:%s  fu_id:%d op:%s l1:%d\n", unsstr64(op->op_num), fu_id,
            disasm_op(op, TRUE), op->engine_info.l1_miss);
      ASSERT(node->proc_id, fu_id < (uns32)node->sd.max_op_count);
      op->fu_num = fu_id;
      node->sd.ops[op->fu_num] = op;
      node->last_scheduled_opnum = op->op_num;
      node->sd.op_count += !s_op;
      ASSERT(node->proc_id, node->sd.op_count <= node->sd.max_op_count);
      return;
    }

    if (op->op_num >= s_op->op_num) {
      continue;
    }

    // The slot is not empty, but we are older than the op that is in the slot
    if (youngest_slot_op_id == NODE_ISSUE_QUEUE_FU_SLOT_INVALID) {
      youngest_slot_op_id = fu_id;
      continue;
    }

    // check if this slot is younger than the youngest known op
    Op* youngest_op = node->sd.ops[youngest_slot_op_id];
    if (s_op->op_num > youngest_op->op_num) {
      youngest_slot_op_id = fu_id;
    }
  }

  /* Did not find an empty slot or a slot that is younger than me, do nothing */
  if (youngest_slot_op_id == NODE_ISSUE_QUEUE_FU_SLOT_INVALID) {
    // Track statistics for ready ops that couldn't be issued
    STAT_EVENT(node->proc_id, RS_OP_READY_NOT_ISSUED_TOTAL);
    STAT_EVENT(node->proc_id, RS_0_OP_READY_NOT_ISSUED + (op->rs_id < 8 ? op->rs_id : 8));
    return;
  }

  /* Did not find an empty slot, but we did find a slot that is younger that us */
  uns32 fu_id = youngest_slot_op_id;
  DEBUG(node->proc_id, "Scheduler selecting    op_num:%s  fu_id:%d op:%s l1:%d\n", unsstr64(op->op_num), fu_id,
        disasm_op(op, TRUE), op->engine_info.l1_miss);
  ASSERT(node->proc_id, fu_id < (uns32)node->sd.max_op_count);
  op->fu_num = fu_id;
  node->sd.ops[op->fu_num] = op;
  node->last_scheduled_opnum = op->op_num;
  node->sd.op_count += 0;  // replacing an op, not adding a new one.
  ASSERT(node->proc_id, node->sd.op_count <= node->sd.max_op_count);
}

/**************************************************************************************/
/* Driven Table */

using Dispatch_Func = int64 (*)(Op*);
Dispatch_Func dispatch_func_table[NODE_ISSUE_QUEUE_DISPATCH_SCHEME_NUM] = {
    [NODE_ISSUE_QUEUE_DISPATCH_SCHEME_FIND_EMPTIEST_RS] = {node_dispatch_find_emptiest_rs},
};

using Schedule_Func = void (*)(Op*);
Schedule_Func schedule_func_table[NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_NUM] = {
    [NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_OLDEST_FIRST] = {node_schedule_oldest_first_sched},
};

/**************************************************************************************/

/*
 * Memory is blocked when there are no more MSHRs in the L1 Q
 * (i.e., there is no way to handle a D-Cache miss).
 * This function checks to see if any of the L1 MSHRs have become available.
 */
void node_issue_queue_check_mem() {
  /* if we are stalled due to lack of MSHRs to the L1, check to see if there is space now. */
  if (node->mem_blocked && mem_can_allocate_req_buffer(node->proc_id, MRT_DFETCH, FALSE)) {
    node->mem_blocked = FALSE;
    STAT_EVENT(node->proc_id, MEM_BLOCK_LENGTH_0 + MIN2(node->mem_block_length, 5000) / 100);
    if (DIE_ON_MEM_BLOCK_THRESH) {
      if (node->proc_id == DIE_ON_MEM_BLOCK_CORE) {
        ASSERTM(node->proc_id, node->mem_block_length < DIE_ON_MEM_BLOCK_THRESH,
                "Core blocked on memory for %u cycles (%llu--%llu)\n", node->mem_block_length,
                cycle_count - node->mem_block_length, cycle_count);
      }
    }
    node->mem_block_length = 0;
  }
  INC_STAT_EVENT(node->proc_id, CORE_MEM_BLOCKED, node->mem_blocked);
  node->mem_block_length += node->mem_blocked;
}

/*
 * Remove scheduled ops (i.e., going from RS to FUs) from the RS and ready queue
 */
void node_issue_queue_clear() {
  // TODO: make this traversal more efficient since we know what ops we tried to schedule last cycle
  Op** last = &node->rdy_head;
  for (Op* op = node->rdy_head; op; op = op->next_rdy) {
    if (op->state != OS_SCHEDULED && op->state != OS_MISS) {
      last = &op->next_rdy;
      continue;
    }

    DEBUG(node->proc_id, "Removing from RS (and ready list)  op_num:%s op:%s l1:%d\n", unsstr64(op->op_num),
          disasm_op(op, TRUE), op->engine_info.l1_miss);
    *last = op->next_rdy;
    op->in_rdy_list = FALSE;
    ASSERT(node->proc_id, node->rs[op->rs_id].rs_op_count > 0);
    node->rs[op->rs_id].rs_op_count--;
    STAT_EVENT(node->proc_id, OP_ISSUED);
  }
}

/*
 * Fill the scheduling window (RS) with oldest available ops.
 * For each available op:
 *  - Allocate it to its designated reservation station.
 *  - If all source operands are ready, insert it into the ready list.
 */
void node_issue_queue_dispatch() {
  Op* op = NULL;
  uns32 num_fill_rs = 0;

  /* Scan through dispatched nodes in node table that have not been filled to RS yet. */
  for (op = node->next_op_into_rs; op; op = op->next_node) {
    int64 rs_id = dispatch_func_table[NODE_ISSUE_QUEUE_DISPATCH_SCHEME](op);
    if (rs_id == NODE_ISSUE_QUEUE_RS_SLOT_INVALID)
      break;
    ASSERT(node->proc_id, rs_id >= 0 && rs_id < NUM_RS);

    Reservation_Station* rs = &node->rs[rs_id];
    ASSERTM(node->proc_id, !rs->size || rs->rs_op_count < rs->size,
            "There must be at least one free space in selected RS!\n");

    ASSERT(node->proc_id, op->state == OS_IN_ROB);
    op->state = OS_IN_RS;
    op->rs_id = (Counter)rs_id;
    rs->rs_op_count++;
    num_fill_rs++;

    DEBUG(node->proc_id, "Filling %s with op_num:%s (%d)\n", rs->name, unsstr64(op->op_num), rs->rs_op_count);

    if (op->srcs_not_rdy_vector == 0) {
      /* op is ready to issue right now */
      DEBUG(node->proc_id, "Adding to ready list  op_num:%s op:%s l1:%d\n", unsstr64(op->op_num), disasm_op(op, TRUE),
            op->engine_info.l1_miss);
      op->state = (cycle_count + 1 >= op->rdy_cycle ? OS_READY : OS_WAIT_FWD);
      op->next_rdy = node->rdy_head;
      node->rdy_head = op;
      op->in_rdy_list = TRUE;
    }

    // maximum number of operations to fill into the RS per cycle (0 = unlimited)
    if (RS_FILL_WIDTH && (num_fill_rs == RS_FILL_WIDTH)) {
      op = op->next_node;
      break;
    }
  }

  // mark the next node to continue filling in the next cycle
  node->next_op_into_rs = op;
}

/*
 * Schedule ready ops (ops that are currently in the ready list).
 *
 * Input:  node->rdy_head, containing all ops that are ready to issue from each of the RSs.
 * Output: node->sd, containing ops thats being passed to the FUs.
 *
 * If a functional unit is available, it will accept the scheduled operation,
 * which is then removed from the ready list. If no FU is available, the
 * operation remains in the ready list to be considered in the next
 * scheduling cycle.
 */
void node_issue_queue_schedule() {
  /*
   * the next stage is supposed to clear them out,
   * regardless of whether they are actually sent to a functional unit
   */
  ASSERT(node->proc_id, node->sd.op_count == 0);

  // Check to see if the L1 Q is (still) full
  node_issue_queue_check_mem();

  for (Op* op = node->rdy_head; op; op = op->next_rdy) {
    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    ASSERTM(node->proc_id, op->in_rdy_list, "op_num %llu\n", op->op_num);
    if (op->state == OS_WAIT_MEM) {
      if (node->mem_blocked)
        continue;
      else
        op->state = OS_READY;
    }

    if (op->state == OS_TENTATIVE || op->state == OS_WAIT_DCACHE)
      continue;

    ASSERTM(node->proc_id, op->state == OS_IN_RS || op->state == OS_READY || op->state == OS_WAIT_FWD,
            "op_num: %llu, op_state: %s\n", op->op_num, Op_State_str(op->state));
    DEBUG(node->proc_id, "Scheduler examining    op_num:%s op:%s l1:%d st:%s rdy:%s exec:%s done:%s\n",
          unsstr64(op->op_num), disasm_op(op, TRUE), op->engine_info.l1_miss, Op_State_str(op->state),
          unsstr64(op->rdy_cycle), unsstr64(op->exec_cycle), unsstr64(op->done_cycle));

    /* op will be ready next cycle, try to schedule */
    if (cycle_count >= op->rdy_cycle - 1) {
      ASSERT(node->proc_id, op->srcs_not_rdy_vector == 0x0);
      DEBUG(node->proc_id, "Scheduler considering  op_num:%s op:%s l1:%d\n", unsstr64(op->op_num), disasm_op(op, TRUE),
            op->engine_info.l1_miss);

      schedule_func_table[NODE_ISSUE_QUEUE_SCHEDULE_SCHEME](op);
    }
  }
  // Track statistics for idle FUs when no ready ops are available
  node_track_fu_idle_stats();
}

/**************************************************************************************/
/* FU idle tracking function */
/*
 * Track statistics for FUs that are idle when no ready ops are available
 * This function checks each FU to see if it's idle and whether there are
 * ready ops that could potentially be executed on it.
 */
void node_track_fu_idle_stats(void) {
  extern Exec_Stage* exec;  // Access to FUs through exec stage

  // Create ready_type vector for each RS to track which op types have ready ops
  uns64 ready_type_per_rs[NUM_RS] = {0};

  // Iterate over ready ops and set bits in ready_type corresponding to FU_TYPE of each op
  for (Op* op = node->rdy_head; op; op = op->next_rdy) {
    if ((op->state == OS_READY || op->state == OS_WAIT_FWD) && cycle_count >= op->rdy_cycle - 1) {
      uns64 op_fu_type = get_fu_type(op->table_info->op_type, op->table_info->is_simd);
      ready_type_per_rs[op->rs_id] |= op_fu_type;
    }
  }

  // Iterate over FUs and check if FU can handle any ready op types from its connected RS
  for (uns32 fu_id = 0; fu_id < NUM_FUS; ++fu_id) {
    // Check if this FU is currently idle (no op scheduled to it and FU is available)
    if (node->sd.ops[fu_id] != NULL)
      continue;  // FU has op scheduled to it, skip

    // Also check if the FU itself is available (not held by memory or still busy from previous op)
    Func_Unit* fu = &exec->fus[fu_id];
    if (fu->avail_cycle > cycle_count || fu->held_by_mem)
      continue;  // FU is not available, skip

    // FU is available - use global mapping to find connected RS
    int32 rs_id = node->fu_to_rs_map[fu_id];
    ASSERT(0, rs_id != NODE_ISSUE_QUEUE_RS_SLOT_INVALID);

    // Check if this FU can handle any ready op types from its connected RS
    if ((ready_type_per_rs[rs_id] & fu->type) == 0) {
      STAT_EVENT(node->proc_id, FU_IDLE_NO_READY_OPS_TOTAL);
      // Per-FU counter for idle cycles with no ready ops
      STAT_EVENT(node->proc_id, FU_0_IDLE_NO_READY_OPS + (fu_id < 32 ? fu_id : 32));
    }
  }
}

/**************************************************************************************/
/* External Function */

void node_issue_queue_update() {
  /* remove scheduled ops from RS and ready list */
  node_issue_queue_clear();

  /* fill RS with oldest ops waiting for it */
  node_issue_queue_dispatch();

  /* first schedule 1 ready op per NUM_FUS  */
  node_issue_queue_schedule();
}
