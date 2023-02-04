// This module simulates the decoder latency for uop cache prefetches

#ifndef __UOP_QUEUE_STAGE_H_
#define __UOP_QUEUE_STAGE_H_

#ifdef __cplusplus
extern "C" {
#endif

// extern "C" {
// #include "debug/debug_macros.h"
// #include "debug/debug_print.h"
// #include "globals/assert.h"
// #include "globals/global_defs.h"
// #include "globals/global_types.h"
// #include "globals/global_vars.h"
// #include "globals/utils.h"

// #include "globals/assert.h"
// #include "statistics.h"
// #include "memory/memory.param.h"
#include "uop_cache.h"
// }

struct Prefetch_Decoder_Entry {
  Uop_Cache_Data pw;
  Counter ready_cycle;
};

void start_decoding_uop_cache_prefetch(Uop_Cache_Data pw);
void insert_decoded_uop_cache_prefetch(void);

#ifdef __cplusplus
}
#endif

#endif