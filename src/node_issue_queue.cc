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

/**************************************************************************************/
/* Issuers:
 *      The interface to the issue functions is that Scarab will pass the
 * function the op to be issued, and the issuer will return the RS id that the
 * op should be issued to, or -1 meaning that there is no RS for the op to
 * be issued to. See FIND_EMPTIEST_RS for an example.
 *
 *      +FIND_EMPTIEST_RS: will always select the RS with the most empty slots
 */
int64 node_dispatch_find_emptiest_rs(Op* op) {
  int64 emptiest_rs_id = -1;
  int64 emptiest_rs_slots = -1;

  /*Iterate through RSs looking for an available RS that is connected
    to an FU that can execute the OP.*/
  for (int64 rs_id = 0; rs_id < NUM_RS; ++rs_id) {
    Reservation_Station* rs = &node->rs[rs_id];
    ASSERT(node->proc_id, !rs->size || rs->rs_op_count <= rs->size);
    ASSERTM(node->proc_id, rs->size, "Infinite RS not suppoted by node_dispatch_find_emptiest_rs issuer.");
    for (uns32 i = 0; i < rs->num_fus; ++i) {
      Func_Unit* fu = rs->connected_fus[i];

      // This FU can execute this op
      if (get_fu_type(op->table_info->op_type, op->table_info->is_simd) & fu->type) {
        // Find the emptiest RS
        int32 num_empty_slots = rs->size - rs->rs_op_count;
        if (num_empty_slots != 0) {
          if (emptiest_rs_slots < num_empty_slots) {
            // Found a new emptiest rs
            emptiest_rs_id = rs_id;
            emptiest_rs_slots = num_empty_slots;
          }
        }
      }
    }
  }

  return emptiest_rs_id;
}

/**************************************************************************************/
/* Schedulers:
 *      The interface to the schedule functions is that Scarab will pass the
 * function the ready op, and the scheduler will return the selected ops in
 * node->sd. See OLDEST_FIRST_SCHED for an example. Note, it is not
 * necessary to look at FU availability in this stage, if the FU is busy,
 * then the op will be ignored and available to schedule again in the next
 * stage.
 *
 *      +OLDEST_FIRST_SCHED: will always select the oldest ready ops to schedule
 */

void node_schedule_oldest_first_sched(Op* op) {
  int32 youngest_slot_op_id = -1;  // -1 means not found

  // Iterate through the FUs that this RS is connected to.
  Reservation_Station* rs = &node->rs[op->rs_id];
  for (uns32 i = 0; i < rs->num_fus; ++i) {
    Func_Unit* fu = rs->connected_fus[i];
    uns32 fu_id = fu->fu_id;

    // check if this op can be executed by this FU
    if (get_fu_type(op->table_info->op_type, op->table_info->is_simd) & fu->type) {
      Op* s_op = node->sd.ops[fu_id];
      if (!s_op) {  // nobody has been scheduled to this FU yet
        DEBUG(node->proc_id, "Scheduler selecting    op_num:%s  fu_id:%d op:%s l1:%d\n", unsstr64(op->op_num), fu_id,
              disasm_op(op, TRUE), op->engine_info.l1_miss);
        ASSERT(node->proc_id, fu_id < (uns32)node->sd.max_op_count);
        op->fu_num = fu_id;
        node->sd.ops[op->fu_num] = op;
        node->last_scheduled_opnum = op->op_num;
        node->sd.op_count += !s_op;
        ASSERT(node->proc_id, node->sd.op_count <= node->sd.max_op_count);
        youngest_slot_op_id = -1;
        break;
      } else if (op->op_num < s_op->op_num) {
        // The slot is not empty, but we are older than the op that is in the
        // slot
        if (youngest_slot_op_id == -1) {
          youngest_slot_op_id = fu_id;
        } else {
          Op* youngest_op = node->sd.ops[youngest_slot_op_id];
          if (s_op->op_num > youngest_op->op_num) {
            // this slot is younger than the youngest known op
            youngest_slot_op_id = fu_id;
          }
        }
      }
    }
  }

  if (youngest_slot_op_id != -1) {
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
  } else {
    /*Did not find an empty slot or a slot that is younger than me, do nothing*/
  }
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
/* External Function */

int64 node_issue_queue_dispatch(Op* op) {
  ASSERT(node->proc_id, NODE_ISSUE_QUEUE_DISPATCH_SCHEME >= 0 &&
                            NODE_ISSUE_QUEUE_DISPATCH_SCHEME < NODE_ISSUE_QUEUE_DISPATCH_SCHEME_NUM);
  return dispatch_func_table[NODE_ISSUE_QUEUE_DISPATCH_SCHEME](op);
}

void node_issue_queue_schedule(Op* op) {
  ASSERT(node->proc_id, NODE_ISSUE_QUEUE_SCHEDULE_SCHEME >= 0 &&
                            NODE_ISSUE_QUEUE_SCHEDULE_SCHEME < NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_NUM);
  schedule_func_table[NODE_ISSUE_QUEUE_SCHEDULE_SCHEME](op);
}
