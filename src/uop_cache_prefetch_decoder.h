// This module simulates the decoder latency for uop cache prefetches

#ifndef __UOP_QUEUE_STAGE_H_
#define __UOP_QUEUE_STAGE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "uop_cache.h"

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