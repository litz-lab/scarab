// The uop queue buffers ops fetched from the uop cache.

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
// TODO(peterbraun): Check if the ISSUE_WIDTH can be less than the uop cache issue bandwidth

// Uop Queue Variables
std::queue<Stage_Data*> q {};
std::queue<Stage_Data*> free_sds {};

void init_uop_queue_stage() {
  for (uns ii = 0; ii < UOP_QUEUE_LENGTH; ii++) {
    Stage_Data* sd = (Stage_Data*)malloc(sizeof(Stage_Data));
    sd->name = (char*)strdup("Uop Queue Stage");
    sd->max_op_count = STAGE_MAX_OP_COUNT;
    sd->op_count = 0;
    sd->ops = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);
    free_sds.push(sd);
  }
}

// Get ops from the uop cache.
void update_uop_queue_stage(Stage_Data* src_sd) {
  // If the front of the queue was consumed, remove that stage.
  while (q.size() && q.front()->op_count == 0) {
    free_sds.push(q.front());
    q.pop();
  }

  // Check if ops are from the uop cache.
  Flag from_icache = src_sd->op_count && !src_sd->ops[0]->fetched_from_uop_cache;
  if (from_icache) {
    return;
  }
  // If the queue cannot accomodate more ops, stall.
  if (q.size() >= UOP_QUEUE_LENGTH) {
    // Backend stalls may force fetch to stall.
    return;
  }

  // Build a new sd and place new ops into the queue.
  Stage_Data* new_sd = free_sds.front();
  free_sds.pop();
  ASSERT(0, src_sd->op_count <= (int)STAGE_MAX_OP_COUNT);
  std::swap(new_sd->op_count, src_sd->op_count);
  std::swap(new_sd->ops, src_sd->ops);
  
  for (int ii = 0; ii < new_sd->op_count; ii++) {
    Op* op = new_sd->ops[ii];
    decode_stage_process_op(op);  // Op skipped decode stage.
    ASSERT(0, op->fetched_from_uop_cache);
  }
  q.push(new_sd);
}

Stage_Data* uop_queue_stage_get_latest_sd(void) {
  if (q.size()) {
    return q.front();
  }
  ASSERT(0, free_sds.size() == UOP_QUEUE_LENGTH);
  return free_sds.front();
};
