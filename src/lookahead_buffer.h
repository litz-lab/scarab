#ifndef __LOOKAHEAD_BUFFER_H__
#define __LOOKAHEAD_BUFFER_H__

#include <cstdint>
#include <map>

#include <stdbool.h>
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

/* Build a FT and insert it into the lookahead buffer*/
void FT_buffer_insert_FT(uns proc_id);

/* Returns a uop from the buffer to the DFE */
FT* FT_buffer_read_FT(uns proc_id);

/* Refills the lookahead buffer to the parameter size*/
void lookahead_buffer_refill(uns proc_id);

/* Pops an ft, Synchronizes lookahead buffer when FT is removed from FTQ */
void lookahead_buffer_pop_ft(uns proc_id, Addr addr, Flag from_lookahead_buffer, int n_uops, Flag off_path,
                             uint64_t pop_count);

/* Returns the next address for frontend fetching */
Addr lookahead_buffer_next_addr();

/* Returns whether lookahead buffer can currently provide an op */
Flag lookahead_buffer_can_fetch_op(uns proc_id);

/* Returns all FTs in the buffer matching given static FT info */
std::vector<FT*> find_FTs(const FT_Info_Static& target_info);

/* Returns the youngest FT matching given static FT info */
FT* find_youngest_FT(const FT_Info_Static& target_info);

/* Returns map of FT info to FTs for a given FT start address */
std::map<FT_Info_Static, std::vector<FT*>> find_FT_start(uint64_t FT_start);

/* Returns list of FTs containing the given PC */
std::vector<FT*> get_enclosing_FTs(Addr PC);

/* Returns list of FTs containing the given line address */
std::vector<FT*> get_enclosing_FTs_line_addr(Addr line_addr);

/* Searches for future access to a given FT; returns insertion order if found */
uint64_t lookahead_buffer_FT_search(FT_Info_Static static_info);

/* Returns buffer index of the oldest FT matching given info, if found */
uint64_t lookahead_buffer_FT_search_buf_pos(FT_Info_Static static_info);

/* Searches for FT static info by buffer position; used in scanning */
FT_Info_Static lookahead_buffer_ptr_search(uint64_t ptr_pos);

#endif