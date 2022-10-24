// The uop queue is populated by uops from either the icache or the uop cache.

#ifndef __UOP_QUEUE_STAGE_H_
#define __UOP_QUEUE_STAGE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "op.h"
#include "globals/global_types.h"
#include "stage_data.h"
#include "decode_stage.h"
#include "uop_cache.h"

void init_uop_queue_stage(void);
void update_uop_queue_stage(Stage_Data* src_sd);
Stage_Data* uop_queue_stage_get_latest_sd(void);

#ifdef __cplusplus
}
#endif

#endif