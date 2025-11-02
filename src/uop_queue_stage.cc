// The uop queue buffers ops fetched from the uop cache.

#include "uop_queue_stage.h"

#include <deque>

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "memory/memory.param.h"

#include "bp/bp.h"

#include "op_pool.h"
#include "statistics.h"
#include "uop_cache.h"
}
#include "ft.h"

// Macros
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_UOP_QUEUE_STAGE, ##args)
#define UOP_QUEUE_STAGE_LENGTH UOP_QUEUE_LENGTH
#define STAGE_MAX_OP_COUNT UOP_CACHE_WIDTH

// Uop Queue Variables
std::deque<Stage_Data*> q{};
std::deque<Stage_Data*> free_sds{};
bool uopq_off_path;

void init_uop_queue_stage() {
  char tmp_name[MAX_STR_LENGTH + 1];
  for (uns ii = 0; ii < UOP_QUEUE_STAGE_LENGTH; ii++) {
    Stage_Data* sd = (Stage_Data*)calloc(1, sizeof(Stage_Data));
    snprintf(tmp_name, MAX_STR_LENGTH, "UOP QUEUE STAGE %d", ii);
    sd->name = (char*)strdup(tmp_name);
    sd->max_op_count = STAGE_MAX_OP_COUNT;
    sd->op_count = 0;
    sd->ops = (Op**)calloc(STAGE_MAX_OP_COUNT, sizeof(Op*));
    free_sds.push_back(sd);
  }
}

// Get ops from the uop cache.
void update_uop_queue_stage(Stage_Data* src_sd) {
  if (!UOP_CACHE_ENABLE)
    return;

  // If the front of the queue was consumed, remove that stage.
  if (q.size() && q.front()->op_count == 0) {
    free_sds.push_back(q.front());
    q.pop_front();
    ASSERT(0, !q.size() || q.front()->op_count > 0);  // Only one stage is consumed per cycle
  }

  if (uopq_off_path) {
    STAT_EVENT(dec->proc_id, UOPQ_STAGE_OFF_PATH);
  }
  // If the queue cannot accomodate more ops, stall.
  if (q.size() >= UOP_QUEUE_STAGE_LENGTH) {
    // Backend stalls may force fetch to stall.
    if (!uopq_off_path) {
      STAT_EVENT(dec->proc_id, UOPQ_STAGE_STALLED);
    }
    return;
  } else if (!uopq_off_path) {
    STAT_EVENT(dec->proc_id, UOPQ_STAGE_NOT_STALLED);
  }

  // Build a new sd and place new ops into the queue.
  Stage_Data* new_sd = free_sds.front();
  ASSERT(0, src_sd->op_count <= (int)STAGE_MAX_OP_COUNT);
  if (src_sd->op_count) {
    if (!uopq_off_path) {
      STAT_EVENT(dec->proc_id, UOPQ_STAGE_NOT_STARVED);
    }
    for (int i = 0; i < src_sd->max_op_count; i++) {
      Op* src_op = src_sd->ops[i];
      if (src_op) {
        ASSERT(src_op->proc_id, src_op->fetched_from_uop_cache);
        new_sd->ops[new_sd->op_count] = src_op;
        src_sd->ops[i] = NULL;
        new_sd->op_count++;
        src_sd->op_count--;
        decode_stage_process_op(src_op);
        DEBUG(0, "Fetching opnum=%llu\n", src_op->op_num);
        if (src_op->off_path)
          uopq_off_path = true;
      }
    }
  } else if (!uopq_off_path) {
    STAT_EVENT(dec->proc_id, UOPQ_STAGE_STARVED);
  }

  if (new_sd->op_count > 0) {
    free_sds.pop_front();
    q.push_back(new_sd);
  }
}

void recover_uop_queue_stage(void) {
  uopq_off_path = false;
  for (std::deque<Stage_Data*>::iterator it = q.begin(); it != q.end();) {
    Stage_Data* sd = *it;
    sd->op_count = 0;
    for (uns op_idx = 0; op_idx < STAGE_MAX_OP_COUNT; op_idx++) {
      Op* op = sd->ops[op_idx];
      if (op && FLUSH_OP(op)) {
        ASSERT(op->proc_id, op->off_path);
        ft_free_op(op);
        sd->ops[op_idx] = NULL;
      } else if (op) {
        sd->op_count++;
      }
    }

    if (sd->op_count == 0) {  // entire stage data was off-path
      free_sds.push_back(sd);
      it = q.erase(it);
    } else {
      ++it;
    }
  }
}

Stage_Data* uop_queue_stage_get_latest_sd(void) {
  if (q.size()) {
    return q.front();
  }
  ASSERT(0, free_sds.size() == UOP_QUEUE_STAGE_LENGTH);
  return free_sds.front();
};

int get_uop_queue_stage_length(void) {
  return q.size();
}