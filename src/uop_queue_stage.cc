// The uop queue buffers ops fetched from the uop cache.

#include "uop_queue_stage.h"

#include <deque>
#include <vector>

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

// Per-core Uop Queue State
struct Uop_Queue_Stage_Data {
  std::deque<Stage_Data*> q;
  std::deque<Stage_Data*> free_sds;
  bool off_path;
  uns8 proc_id;
};

static std::vector<Uop_Queue_Stage_Data> per_core_uop_queue;
static Uop_Queue_Stage_Data* uopq = nullptr;

void alloc_mem_uop_queue_stage(uns num_cores) {
  per_core_uop_queue.resize(num_cores);
  for (uns i = 0; i < num_cores; i++) {
    per_core_uop_queue[i].proc_id = i;
    per_core_uop_queue[i].off_path = false;
  }
}

void set_uop_queue_stage(uns8 proc_id) {
  uopq = &per_core_uop_queue[proc_id];
}

void init_uop_queue_stage(uns8 proc_id) {
  char tmp_name[MAX_STR_LENGTH + 1];
  Uop_Queue_Stage_Data& state = per_core_uop_queue[proc_id];
  state.off_path = false;
  for (uns ii = 0; ii < UOP_QUEUE_STAGE_LENGTH; ii++) {
    Stage_Data* sd = (Stage_Data*)calloc(1, sizeof(Stage_Data));
    snprintf(tmp_name, MAX_STR_LENGTH, "UOP QUEUE STAGE %d", ii);
    sd->name = (char*)strdup(tmp_name);
    sd->max_op_count = STAGE_MAX_OP_COUNT;
    sd->op_count = 0;
    sd->ops = (Op**)calloc(STAGE_MAX_OP_COUNT, sizeof(Op*));
    state.free_sds.push_back(sd);
  }
}

// Get ops from the uop cache.
void update_uop_queue_stage(Stage_Data* src_sd) {
  if (!UOP_CACHE_ENABLE)
    return;

  ASSERT(0, uopq);

  // If the front of the queue was consumed, remove that stage.
  if (uopq->q.size() && uopq->q.front()->op_count == 0) {
    uopq->free_sds.push_back(uopq->q.front());
    uopq->q.pop_front();
    ASSERT(0, !uopq->q.size() || uopq->q.front()->op_count > 0);  // Only one stage is consumed per cycle
  }

  if (uopq->off_path) {
    STAT_EVENT(uopq->proc_id, UOPQ_STAGE_OFF_PATH);
  }
  // If the queue cannot accomodate more ops, stall.
  if (uopq->q.size() >= UOP_QUEUE_STAGE_LENGTH) {
    // Backend stalls may force fetch to stall.
    if (!uopq->off_path) {
      STAT_EVENT(uopq->proc_id, UOPQ_STAGE_STALLED);
    }
    return;
  } else if (!uopq->off_path) {
    STAT_EVENT(uopq->proc_id, UOPQ_STAGE_NOT_STALLED);
  }

  // If src_sd is NULL (when UOP_CACHE_ENABLE but called with NULL), just return
  if (!src_sd)
    return;

  // Build a new sd and place new ops into the queue.
  Stage_Data* new_sd = uopq->free_sds.front();
  ASSERT(0, src_sd->op_count <= (int)STAGE_MAX_OP_COUNT);
  if (src_sd->op_count) {
    if (!uopq->off_path) {
      STAT_EVENT(uopq->proc_id, UOPQ_STAGE_NOT_STARVED);
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
        DEBUG(uopq->proc_id, "Fetching opnum=%llu\n", src_op->op_num);
        if (src_op->off_path)
          uopq->off_path = true;
      }
    }
  } else if (!uopq->off_path) {
    STAT_EVENT(uopq->proc_id, UOPQ_STAGE_STARVED);
  }

  if (new_sd->op_count > 0) {
    uopq->free_sds.pop_front();
    uopq->q.push_back(new_sd);
  }
}

void recover_uop_queue_stage(void) {
  ASSERT(0, uopq);
  uopq->off_path = false;
  for (std::deque<Stage_Data*>::iterator it = uopq->q.begin(); it != uopq->q.end();) {
    Flag flushed = FALSE;
    Stage_Data* sd = *it;
    sd->op_count = 0;
    for (uns op_idx = 0; op_idx < STAGE_MAX_OP_COUNT; op_idx++) {
      Op* op = sd->ops[op_idx];
      if (op && FLUSH_OP(op)) {
        DEBUG(op->proc_id, "UopQ flushing op_num:%llu off_path:%u\n", (unsigned long long)op->op_num, op->off_path);
        flushed = TRUE;
        ASSERT(op->proc_id, op->off_path);
        if (op->parent_FT)
          ft_free_op(op);
        sd->ops[op_idx] = NULL;
      } else if (op) {
        sd->op_count++;
      }
    }

    if (sd->op_count > 0 && flushed) {
      Op* op = sd->ops[sd->op_count - 1];
      assert_ft_after_recovery(dec->proc_id, op, bp_recovery_info->recovery_fetch_addr);
    }

    if (sd->op_count == 0) {  // entire stage data was off-path
      uopq->free_sds.push_back(sd);
      it = uopq->q.erase(it);
    } else {
      ++it;
    }
  }
}

Stage_Data* uop_queue_stage_get_latest_sd(void) {
  ASSERT(0, uopq);
  if (uopq->q.size()) {
    return uopq->q.front();
  }
  ASSERT(0, uopq->free_sds.size() == UOP_QUEUE_STAGE_LENGTH);
  return uopq->free_sds.front();
};

int get_uop_queue_stage_length(void) {
  ASSERT(0, uopq);
  return uopq->q.size();
}
