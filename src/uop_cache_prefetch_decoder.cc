// This module simulates the decoder latency for uop cache prefetches
//
//

#include "uop_cache_prefetch_decoder.h"
#include <queue>
// #include <memory>

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
#include "uop_cache.h"
}

std::queue<Prefetch_Decoder_Entry> prefetch_decoder_queue;

void start_decoding_uop_cache_prefetch(Uop_Cache_Data pw) {
    Prefetch_Decoder_Entry pde = {pw, cycle_count + DECODE_CYCLES};
    prefetch_decoder_queue.emplace(pde);
}

void insert_decoded_uop_cache_prefetch(void) {
    // Insert into the uop cache up to once per cycle.
    if (prefetch_decoder_queue.empty()) return;
    Prefetch_Decoder_Entry oldest_entry = prefetch_decoder_queue.front();
    if (oldest_entry.ready_cycle <= cycle_count) {
        prefetch_decoder_queue.pop();
        Flag inserted = pw_insert(oldest_entry.pw);
        INC_STAT_EVENT(0, UOP_CACHE_PREFETCH, inserted);
        INC_STAT_EVENT(0, UOP_CACHE_PREFETCH_UOP_COUNT, oldest_entry.pw.n_uops);
        STAT_EVENT(0, UOP_CACHE_PREFETCH_PW_LENGTH_1 + oldest_entry.pw.n_uops - 1);
    }
}