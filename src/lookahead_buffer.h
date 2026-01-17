#ifndef __LOOKAHEAD_BUFFER_H__
#define __LOOKAHEAD_BUFFER_H__

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "ft.h"
#include "ft_info.h"
#include "op.h"

/* Initialization API, used when buffer size is non-zero */
void init_lookahead_buffer();

/* Refills lookahead buffer to the parameter size */
void lookahead_buffer_refill(uns proc_id);

/* Pops an ft, Synchronizes lookahead buffer when FT is removed from FTQ */
FT* lookahead_buffer_pop_ft(uns proc_id);

/* Returns FT_Info for the FT at read pointer */
FT_Info lookahead_buffer_peek();

/* Returns whether lookahead buffer can currently provide an op */
Flag lookahead_buffer_can_fetch_op(uns proc_id);

/* Returns all FTs in the buffer matching given static FT info */
std::vector<FT*> lookahead_buffer_find_FTs_by_ft_info(const FT_Info_Static& target_info);

/* find youngest FT by static info
   built from find_by_ft_info primitive */
FT* lookahead_buffer_find_youngest_FT_by_static_info(const FT_Info_Static& target_info);

/* Returns map of FT info to FTs for a given FT start address */
std::map<FT_Info_Static, std::vector<FT*>> lookahead_buffer_find_FTs_by_start_addr(uint64_t FT_start_addr);

/* Returns list of FTs containing the given PC */
std::deque<FT*> lookahead_buffer_find_FTs_enclosing_PC(Addr PC);

/* Returns list of FTs containing the given line address */
std::deque<FT*> lookahead_buffer_find_FTs_enclosing_line_addr(Addr line_addr);

/* returns oldest FT of given FT info */
FT* lookahead_buffer_find_oldest_FT_by_FT_info(FT_Info_Static static_info);

/* Searches for FT static info by buffer position; used in scanning */
FT* lookahead_buffer_get_FT_at_buf_pos(uint64_t ptr_pos);

/* Returns current read pointer position included in FTQ */
uint64_t lookahead_buffer_rdptr();

/* Returns number of FTs in buffer */
uint64_t lookahead_buffer_count();

#endif