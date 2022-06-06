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

void init_uop_queue_stage() {
  oldest_ops.name = (char*)strdup("Uop Queue Stage");
  oldest_ops.max_op_count = STAGE_MAX_OP_COUNT;
  oldest_ops.op_count = 0;
  oldest_ops.ops = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);
}

void update_uop_queue_stage(Stage_Data* src_sd) {
  // If the queue cannot accomodate more ops, stall.
  if ((int)queue_tail.size() + oldest_ops.op_count + src_sd->op_count > UOP_QUEUE_LENGTH) {
    return;
  }

  // Place new ops into the queue.
  for (int src_sd_op_idx = 0; src_sd_op_idx < src_sd->op_count; src_sd_op_idx++) {
    queue_tail.push(src_sd->ops[src_sd_op_idx]);
    src_sd->ops[src_sd_op_idx] = NULL;
  }
  src_sd->op_count = 0;

  // If oldest_ops has space, fill from queue
  while (oldest_ops.op_count < (int)STAGE_MAX_OP_COUNT && !queue_tail.empty()) {
    oldest_ops.ops[oldest_ops.op_count++] = queue_tail.front();
    queue_tail.pop();
  }
}