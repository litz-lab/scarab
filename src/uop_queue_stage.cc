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
Stage_Data uop_queue_oldest_ops {};
std::queue<Op*> queue_tail {}; // The hidden, tail end of the queue
Counter next_op_num = 1;

void init_uop_queue_stage() {
  uop_queue_oldest_ops.name = (char*)strdup("Uop Queue Stage");
  uop_queue_oldest_ops.max_op_count = STAGE_MAX_OP_COUNT;
  uop_queue_oldest_ops.op_count = 0;
  uop_queue_oldest_ops.ops = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);
}

// Get ops from the decode_stage OR directly from the icache_stage when
// the ops are predecoded (fetched from the uop cache).
void update_uop_queue_stage(Stage_Data* src_sd) {
  // If the queue cannot accomodate more ops, stall.
  if ((int)queue_tail.size() + uop_queue_oldest_ops.op_count + src_sd->op_count > UOP_QUEUE_LENGTH) {
    return;
  }

  // Place new ops into the queue.
  int src_orig_op_count = src_sd->op_count;
  for (int ii = 0; ii < src_orig_op_count; ii++) {
    Op* op = src_sd->ops[ii];
    decode_stage_process_op(op);  // Op skipped decode stage.
    queue_tail.push(op);
    src_sd->ops[ii] = NULL;
    src_sd->op_count--;
    next_op_num++;
  }

  // If uop_queue_oldest_ops has space, fill from queue
  while (uop_queue_oldest_ops.op_count < (int)STAGE_MAX_OP_COUNT && !queue_tail.empty()) {
    uop_queue_oldest_ops.ops[uop_queue_oldest_ops.op_count++] = queue_tail.front();
    queue_tail.pop();
  }
}