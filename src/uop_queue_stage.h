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

//Ops that are at the "head", i.e. the ops that can be consumed by the next stage
extern Stage_Data oldest_ops;

void init_uop_queue_stage(void);
void update_uop_queue_stage(Stage_Data* src_sd, Flag from_uop_cache);

#ifdef __cplusplus
}
#endif

#endif