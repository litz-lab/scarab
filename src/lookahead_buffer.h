#ifndef __LOOKAHEAD_BUFFER_H__
#define __LOOKAHEAD_BUFFER_H_
#include <cstdint>
#include <map>
#include <math.h>
#include <stdbool.h>
#include <vector>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "ft.h"
#include "ft_info.h"
#include "op.h"

// initialization api, used to initialize the lookahead buffer if buffer size is not 0
void init_lookahead_buffer();

// read from frontend till we have a complete FT
// then insert the FT into the lookahead buffer
// also called upon initialization to fill the buffer
void FT_buffer_insert_FT(uns8 proc_id);

// read the uops from the lookahead buffer
// called by dfe
Op* FT_buffer_read_Op(uns proc_id);

// output the uops in the lookahead buffer to the frontend
// will be called when the frontend is trying to fetch a new op (inside frontend_fetch_op(proc_id, op);)
// if we are in on path, we will read the ft and read pointer position and output the ops contained
// read pointer will be moved to the next FT when FT completely consumed
// if we are in off path, we will call APIs in frontend for offpath op generation and output that
Op* lookahead_buffer_fetch_op(uns proc_id, Op* new_op);

Flag lookahead_buffer_can_fetch_op(uns proc_id);

// look for the FT to see the if we have future accesses to the FT
// would also need to check if the FT is in the FTQ
// returns the FT inserting order/timestamp if found
uint64_t lookahead_buffer_FT_search(FT_Info_Static static_info);

// reuturns oldest FT buffer position if found
uint64_t lookahead_buffer_FT_search_buf_pos(FT_Info_Static static_info);

// returns the FT static info if found
// called for buffer scanning
FT_Info_Static lookahead_buffer_ptr_search(uint64_t ptr_pos);

// returns a vector of FTs of the static info in the lookahead buffer
std::vector<FT> find_FTs(const FT_Info_Static& target_info);

// returns the youngest FT of the static info in the lookahead buffer
FT find_youngest_FT(const FT_Info_Static& target_info);

// returns the FT info and FT vector of the FT start address
std::map<FT_Info_Static, std::vector<FT>> find_FT_start(uint64_t FT_start);

// returns teh FTs containing the PC
std::vector<FT*> get_enclosing_FTs(Addr PC);

// called when FT is removed from the FTQ to sync lookahead
void lookahead_buffer_ft_removed_from_ftq(uns proc_id, Addr addr);

// to match dfe api calls to frontend
Addr lookahead_buffer_next_addr();

// called when dfe goes off path and recover
void lookahead_buffer_redirect();
void lookahead_buffer_recover();

// returns the read pointer position included in the FTQ
uint64_t lookahead_buffer_rdptr();

#endif