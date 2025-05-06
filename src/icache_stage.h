/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : icache_stage.h
 * Author       : HPS Research Group
 * Date         : 12/7/1998
 * Description  :
 ***************************************************************************************/

#ifndef __ICACHE_STAGE_H__
#define __ICACHE_STAGE_H__

#include "globals/global_types.h"

#include "libs/cache_lib.h"

#include "decoupled_frontend.h"
#include "stage_data.h"

#define IC_ISSUE_WIDTH DECODE_WIDTH
#define UOPC_ISSUE_WIDTH UOP_CACHE_WIDTH

/**************************************************************************************/
/* Forward Declarations */

struct Inst_Info_struct;
struct Mem_Req_struct;

/**************************************************************************************/
/* Types */

/* name strings are in debug/debug_print.c */
typedef enum Icache_State_enum {
  ICACHE_STAGE_RESTEER,
  ICACHE_MEM_REQ,
  ICACHE_WAIT_FOR_MISS,
  ICACHE_SERVING,
  UOP_CACHE_SERVING
} Icache_State;

// don't change this order without fixing stats in fetch.stat.def
typedef enum Break_Reason_enum {
  BREAK_DONT,  // don't break fetch yet
  BREAK_ICACHE_STAGE_RESTEER,
  BREAK_FT_UNAVAILABLE,           // break because the ft queue of the decoupled front-end is empty
  BREAK_ICACHE_MISS_REQ_SUCCESS,  // break because of an icache miss where the mem req succeeds
  BREAK_ICACHE_MISS_REQ_FAILURE,  // break because of an icache miss where the mem req fails
  BREAK_ICACHE_WAIT_FOR_MISS,
  BREAK_ICACHE_STALLED,
  BREAK_ICACHE_TO_UOP_CACHE_SWITCH,  // break because in the same cycle switched to fetching from uop cache
  BREAK_ICACHE_READ_LIMIT,           // break because the uop cache has limited read capability
  BREAK_ICACHE_ISSUE_WIDTH,
  BREAK_UOP_CACHE_STALLED,
  BREAK_UOP_CACHE_TO_ICACHE_SWITCH,
  BREAK_UOP_CACHE_READ_LIMIT,
  BREAK_UOP_CACHE_ISSUE_WIDTH
} Break_Reason;

typedef enum FT_Arbitration_Result_enum {
  FT_UNAVAILABLE,
  FT_MISS_BOTH,
  FT_HIT_ICACHE,
  FT_HIT_UOP_CACHE
} FT_Arbitration_Result;

typedef struct Icache_Stage_struct {
  uns8 proc_id;
  /* two data paths: */
  /* uops fetched from uop cache go to uopc_sd, otherwise sd */
  Stage_Data sd; /* stage interface data */
  uns8 lookups_per_cycle_count;

  Icache_State state;           /* state that the ICACHE is in */
  Icache_State next_state;      /* state that the ICACHE is going to be in next cycle */
  uint64_t wait_for_miss_start; /* time when cache miss was observed */
  Flag icache_miss_fulfilled;
  Flag icache_stage_resteer_signaled;

  Inst_Info** line; /* pointer to current line on a hit */
  Addr line_addr;   /* address of the last cache line hit */
  Addr fetch_addr;  /* address to fetch or fetching */
  // keep track of the current FT being used by the icache / uop cache
  FT* current_ft;
  Flag off_path;     /* is the icache fetching on the correct path? */
  Flag back_on_path; /* did a recovery happen to put the machine back on path? */

  Counter rdy_cycle; /* cycle that the henry icache will return data (only used in henry model) */

  Cache icache;           /* the cache storage structure (caches Inst_Info *) */
  Cache icache_line_info; /* contains info about the icache lines */
  Cache pref_icache;      /* Prefetcher cache storage structure (caches Inst_Info *) */
  char rand_wb_state[31]; /* State of random number generator for random writeback */
} Icache_Stage;

typedef struct Icache_Data_struct {
  Flag fetched_by_offpath;   /* fetched by an off_path op? */
  Addr offpath_op_addr;      /* PC of the off path op that fetched this line */
  Counter offpath_op_unique; /* unique of the off path op that fetched this line */
  uns read_count[2];
  Flag HW_prefetch;
  uns FDIP_prefetch;
  uint64_t ghist;

  Counter fetch_cycle;
  Counter onpath_use_cycle;
} Icache_Data;

/**************************************************************************************/
/* External Variables */

extern Icache_Stage* ic;

/**************************************************************************************/
/* Prototypes */

/* vanilla hps model */
void set_icache_stage(Icache_Stage*);
void init_icache_stage(uns8, const char*);
Stage_Data* get_current_stage_data(void);
void reset_icache_stage(void);
void reset_all_ops_icache_stage(void);
void recover_icache_stage(void);
void redirect_icache_stage(void);
void debug_icache_stage(void);
void update_icache_stage(void);

Flag icache_fill_line(Mem_Req*);
Flag icache_off_path(void);
Flag instr_fill_line(Mem_Req* req);
Flag in_icache(Addr addr);  // For branch stat collection

/**************************************************************************************/

#endif /* #ifndef __ICACHE_STAGE_H__ */
