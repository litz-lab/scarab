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
 * File         : libs/cache_lib.h
 * Author       : HPS Research Group
 * Date         : 2/6/1998
 * Description  : Header for libs/cache_lib.c
 ***************************************************************************************/

#ifndef __CACHE_LIB_H__
#define __CACHE_LIB_H__

#include "globals/global_defs.h"

#include "libs/list_lib.h"

/**************************************************************************************/

/* set data pointers to this initially */
#define INIT_CACHE_DATA_VALUE ((void*)0x8badbeef)

/**************************************************************************************/

typedef enum Repl_Policy_enum {
  REPL_TRUE_LRU,              /* actual least-recently-used replacement */
  REPL_RANDOM,                /* random replacement */
  REPL_NOT_MRU,               /* not-most-recently-used replacement */
  REPL_ROUND_ROBIN,           /* round-robin replacement */
  REPL_IDEAL,                 /* ideal replacement */
  REPL_ISO_PREF,              /* lru with some entries (isolated misses) higher priority */
  REPL_LOW_PREF,              /* prefetched data have lower priority */
  REPL_SHADOW_IDEAL,          /* ideal replacement with shadow cache */
  REPL_IDEAL_STORAGE,         /* if the data doesn't have a temporal locality then it
                                 isn't stored at the cache */
  REPL_MLP,                   /* mlp based replacement  -- uses MLP_REPL_POLICY */
  REPL_PARTITION,             /* Based on the partition*/
  REPL_RESTEER,               /* Prioritize the instr following a resteered branch or fetch barrier */
  REPL_STICKY_PRIORITY_LINES, /* Prioritize lines tagged with priority bit. */

  REPL_VOID,    /* void policy for loop ending and policy seperation */
  REPL_LRU_REF, /* least-recently-used replacement */
  REPL_NRU,     /* not recently used replacement */
  REPL_SRRIP,   /* static re-reference interval prediction */
  REPL_BRRIP,   /* bimodal re-reference interval prediction */
  REPL_DRRIP,   /* dynamic re-reference interval prediction */
  REPL_SHIP,    /* signature-based hit predictor */

  NUM_REPL
} Repl_Policy;

typedef enum Cache_Repl_Signiture_enum {
  CACHE_REPL_SIGH_PC,
  CACHE_REPL_SIGH_MEM,
  CACHE_REPL_SIGH_NUM
} Cache_Repl_Signiture;

typedef struct Cache_Entry_struct {
  uns8 proc_id;
  Flag valid;               /* valid bit for the line */
  Addr tag;                 /* tag for the line */
  Addr base;                /* address of first element */
  Counter last_access_time; /* for replacement policy */
  Counter insertion_time;   /* for replacement policy */
  void* data;               /* pointer to arbitrary data */
  Flag pref;                /* extra replacement info */
  Flag dirty;               /* Dirty bit should have been here, however this is used only in warmup now */
  Addr pw_start_addr;       /* for uop cache: start addr of prediction window */

  uns8 reference_val; /* for re-reference replacement policy */
  Flag outcome;       /* for replacement policy */
} Cache_Entry;

// DO NOT CHANGE THIS ORDER
typedef enum Cache_Insert_Repl_enum {
  INSERT_REPL_DEFAULT = 0, /* Insert with default replacement information */
  INSERT_REPL_LRU,         /* Insert into LRU position */
  INSERT_REPL_LOWQTR,      /* Insert such that it is Quarter(Roughly) of the repl order*/
  INSERT_REPL_MID,         /* Insert such that it is Middle(Roughly) of the repl order*/
  INSERT_REPL_MRU,         /* Insert into MRU position */
  NUM_INSERT_REPL
} Cache_Insert_Repl;

typedef struct Cache_struct {
  char name[MAX_STR_LENGTH + 1]; /* name to identify the cache (for debugging) */
  uns data_size;                 /* how big are the data items in each cache entry? (for malloc) */

  uns assoc;               /* associativity */
  uns num_lines;           /* number of lines in the cache */
  uns num_sets;            /* number of sets in the cache */
  uns line_size;           /* size in bytes of one line */
  Repl_Policy repl_policy; /* the replacement policy of the cache */

  uns set_bits;     /* number of bits used in the set mask */
  uns shift_bits;   /* number of bits to shift an address before using (assuming it is shifted) */
  Addr set_mask;    /* mask applied after shifting to get the index */
  Addr tag_mask;    /* mask used to get the tag after shifting */
  Addr offset_mask; /* mask used to get the line offset */

  uns* repl_ctrs; /* replacement info */

  /* A dynamically allocated array of all of the cache entries. The array is two-dimensional, sets are row major. */
  Cache_Entry** entries;

  /* A linked list for each set in the cache that is used when simulating ideal replacement policies */
  List* unsure_lists;

  Flag perfect;                 /* is the cache perfect (for henry mem system) */
  uns repl_pref_thresh;         /* threshhold for how many entries are high-priority. */
  Cache_Entry** shadow_entries; /* A dynamically allocated array for shadow cache */
  uns* queue_end;               /* queue pointer for ideal storage */

  Counter num_demand_access;
  Counter last_update; /* last update cycle */

  uns* num_ways_allocted_core; /* For cache partitioning */
  uns* num_ways_occupied_core; /* For cache partitioning */
  uns* lru_index_core;         /* For cache partitioning */
  Counter* lru_time_core;      /* For cache partitioning */

  Flag tag_incl_offset; /* The uop cache is byte-addressable, so the tag includes offset bits as well */

  /* For DRRIP repl */
  uns* dedicated_policy_set; /* For dedicated set map */
  Counter* miss_count;       /* For sampling */
  Counter bimodal_count;

  /* For repl with predictor */
  void* predictor;
} Cache;

/**************************************************************************************/
/* Strategy Design */
struct repl_policy_func {
  Repl_Policy repl_policy_type;

  void (*action_init)(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
  void (*action_repl)(Cache*, Cache_Entry*, uns8, Addr, Addr*, Addr*);

  void (*update_hit)(Cache*, uns, uns, void*);
  void (*update_insert)(Cache*, uns8, uns, uns, void*);
  Cache_Entry* (*update_evict)(Cache*, uns8, uns, uns*, void*, Flag);
};

/* Driven Table */
extern struct repl_policy_func repl_policy_func_table[NUM_REPL];

/* Strategy Function */
void init_cache_strategy(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
void* cache_insert_strategy(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr);
void* cache_access_strategy(Cache* cache, Addr addr, Addr* line_addr, Flag update_repl);
Cache_Entry* cache_evict_strategy(Cache* cache, uns8 proc_id, uns set, uns* way);

const static Flag CACHE_DEBUG_ENABLE = FALSE;  // To be Changed into DEBUG_PARA

/**************************************************************************************/
/* prototypes */

void init_cache(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
void* cache_access(Cache*, Addr, Addr*, Flag);
void* cache_insert(Cache*, uns8, Addr, Addr*, Addr*);
void* cache_insert_replpos(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr,
                           Cache_Insert_Repl insert_repl_policy, Flag isPrefetch);
void* cache_insert_lru(Cache*, uns8, Addr, Addr*, Addr*);
void cache_invalidate(Cache*, Addr, Addr*);
void cache_flush(Cache*);
void* get_next_repl_line(Cache*, uns8, Addr, Addr*, Flag*);
void* get_next_valid_repl_line(Cache* cache, uns8 proc_id, Addr addr);
uns ext_cache_index(Cache*, Addr, Addr*, Addr*);
Addr get_cache_line_addr(Cache*, Addr);
uns cache_get_invalid_line_count(Cache* cache, Addr addr);
void update_repl_resteer_policy(Cache*, Addr);

void* shadow_cache_insert(Cache* cache, uns set, Addr tag, Addr base);
void* access_shadow_lines(Cache* cache, uns set, Addr tag);
void* access_ideal_storage(Cache* cache, uns set, Addr tag, Addr addr);
void reset_cache(Cache*);
int cache_find_pos_in_lru_stack(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr);
void set_partition_allocate(Cache* cache, uns8 proc_id, uns num_ways);
uns get_partition_allocated(Cache* cache, uns8 proc_id);

/**************************************************************************************/

#endif /* #ifndef __CACHE_LIB_H__ */
