#ifndef __LOOKAHEAD_BUFFER_H__
#define __LOOKAHEAD_BUFFER_H__

#include <stdint.h>

#include "globals/global_types.h"

#include "ft.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocation API for per-core lookahead buffer instances */
void alloc_mem_lookahead_buffer(uns num_cores);

/* Pops an FT and synchronizes lookahead buffer state */
FT* lookahead_buffer_pop_ft(uns proc_id);

/* Refills lookahead buffer up to configured size */
void lookahead_buffer_refill(uns proc_id);

/* Returns whether lookahead buffer can currently provide an op */
Flag lookahead_buffer_can_fetch_op(uns proc_id);

/* Searches for FT static info by buffer position; used in scanning */
FT* lookahead_buffer_get_FT(uns proc_id, uint64_t ptr_pos);

/* Returns current read pointer position included in FTQ */
uint64_t lookahead_buffer_rdptr(uns proc_id);

/* Returns number of FTs in buffer */
uint64_t lookahead_buffer_count(uns proc_id);

#ifdef __cplusplus
}
#endif

#endif