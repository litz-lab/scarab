#include "uop_queue_stage.h"
#include <queue>

extern "C" {
#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "globals/assert.h"
#include "statistics.h"
#include "memory/memory.param.h"
}

// Macros
#define STAGE_MAX_OP_COUNT ISSUE_WIDTH  // The bandwidth of the next, consuming stage (map stage)

// Global Variables
Stage_Data oldest_ops {};
std::queue<Op*> queue_tail {}; // The hidden, tail end of the queue
Counter next_op_num = 1;

void init_uop_queue_stage() {
  oldest_ops.name = (char*)strdup("Uop Queue Stage");
  oldest_ops.max_op_count = STAGE_MAX_OP_COUNT;
  oldest_ops.op_count = 0;
  oldest_ops.ops = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);
}

// Get ops from the decode_stage OR directly from the icache_stage when
// the ops are predecoded (fetched from the uop cache).
void update_uop_queue_stage(Stage_Data* src_sd, Flag from_uop_cache) {
  // If the queue cannot accomodate more ops, stall.
  if ((int)queue_tail.size() + oldest_ops.op_count + src_sd->op_count > UOP_QUEUE_LENGTH) {
    return;
  }

  // Place new ops into the queue.
  int src_orig_op_count = src_sd->op_count;
  for (int ii = 0; ii < src_orig_op_count; ii++) {
    Op* op = src_sd->ops[ii];
    ASSERT(0, op->op_num >= next_op_num);
    if (op->op_num > next_op_num) {
      ASSERT(0, from_uop_cache);
      return;  // The decode stage is not empty - ops in flight.
    } else if ((from_uop_cache && !op->fetched_from_uop_cache) || (!from_uop_cache && op->fetched_from_uop_cache)) {
      for (int jj = 0; ii > 0 && ii < src_orig_op_count; ii++, jj++) {
        src_sd->ops[jj] = src_sd->ops[ii];  // Shift ops in src_sd to front.
        src_sd->ops[ii] = NULL;
      }
      break;
    }

    if (from_uop_cache) {
      decode_stage_process_op(op);  // Op skipped decode stage.
      // If we just switched from icache fetch to uop cache fetch, a PW
      // will exist and the end of it has just been reached.
      end_accumulate();
    } else {
      // Cache all uops being emitted from decode stage
      STAT_EVENT(dec->proc_id, UOP_ACCUMULATE);
      accumulate_op(op);
    }
    queue_tail.push(op);
    src_sd->ops[ii] = NULL;
    src_sd->op_count--;
    next_op_num++;
  }

  // If oldest_ops has space, fill from queue
  while (oldest_ops.op_count < (int)STAGE_MAX_OP_COUNT && !queue_tail.empty()) {
    oldest_ops.ops[oldest_ops.op_count++] = queue_tail.front();
    queue_tail.pop();
  }
}