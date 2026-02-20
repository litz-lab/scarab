// The uop queue is populated by uops from either the icache or the uop cache.

#ifndef __UOP_QUEUE_STAGE_H_
#define __UOP_QUEUE_STAGE_H_

#include "globals/global_types.h"
#include "decode_stage.h"
#include "op.h"
#include "stage_data.h"
#include "uop_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

void alloc_mem_uop_queue_stage(uns num_cores);
void set_uop_queue_stage(uns8 proc_id);
void init_uop_queue_stage(uns8 proc_id);
void update_uop_queue_stage(Stage_Data* src_sd);
void recover_uop_queue_stage(void);
Stage_Data* uop_queue_stage_get_latest_sd(void);
// Returns length of queue in terms of number of stages
int get_uop_queue_stage_length(void);
void stat_event_new_pw_accessed(Uop_Cache_Data* pw);

#ifdef __cplusplus
}
#endif

#endif
