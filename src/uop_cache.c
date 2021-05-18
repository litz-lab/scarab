/***************************************************************************************
 * File         : uop_cache.h
 * Author       : Peter Braun
 * Date         : 10.28.2020
 * Description  : Interface for interacting with uop cache object.
 *                  Following Kotra et. al.'s MICRO 2020 description of uop cache baseline
 ***************************************************************************************/

#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "isa/isa_macros.h"

#include "bp/bp.h"
#include "op_pool.h"

#include "core.param.h"
#include "debug/debug.param.h"
#include "general.param.h"
#include "statistics.h"

#include "libs/cache_lib.h"
#include "memory/memory.param.h"
#include "uop_cache.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_UOP_CACHE, ##args)

#define UOP_CACHE_LINE_SIZE       ICACHE_LINE_SIZE
#define UOP_QUEUE_SIZE            100 // at least UOP_CACHE_ASSOC * UOP_CACHE_MAX_UOPS_LINE
#define UOP_CACHE_LINE_DATA_SIZE  sizeof(Uop_Cache_Data)

/**************************************************************************************/
/* Local Prototypes */

static inline Flag insert_uop_cache(void);
static inline Flag in_uop_cache_search(Addr search_addr, Flag update_repl);

/**************************************************************************************/
/* Global Variables */

Cache uop_cache;

// uop trace/bbl accumulation
static Uop_Cache_Data accumulating_pw = {0};
static Op* uop_q[UOP_QUEUE_SIZE];

// k: instr addr
Hash_Table inf_size_uop_cache;

/**************************************************************************************/
/* init_uop_cache */

void init_uop_cache() {
  if (INF_SIZE_UOP_CACHE || INF_SIZE_UOP_CACHE_PW_SIZE_LIM) {
    init_hash_table(&inf_size_uop_cache, "infinite sized uop cache", 15000000, sizeof(int));
  }
  if (UOP_CACHE_SIZE == 0) {
    return;
  }
  init_cache(&uop_cache, "UOP_CACHE", UOP_CACHE_SIZE, UOP_CACHE_ASSOC, UOP_CACHE_LINE_SIZE,
             UOP_CACHE_LINE_DATA_SIZE, REPL_TRUE_LRU);
}

/**************************************************************************************/
/* insert_uop_cache: private method, only called by accumulate_op
 *                   Drain buffer and insert. Return whether inserted.
 */
Flag insert_uop_cache() {
  // PW may span multiple cache entries. 1 entry per line. Additional terminating conditions per line:
  // 1. max uops per line
  // 2. max imm/disp per line
  // 3. max micro-coded instr per line (not simulated)

  // Invalidate after hitting maximum allowed number of lines, as in gem5
  // Each entry/pw indexed by physical addr of 1st instr, so insert each line using same addr
  // Invalidation: Let LRU handle it by placing PW in one line at a time.   
  
  ASSERT(0, accumulating_pw.n_uops);

  Addr line_addr;  
  Addr repl_line_addr;
  Uop_Cache_Data* cur_line_data = NULL;
  Flag success = FALSE;

  int lines_needed = accumulating_pw.n_uops / UOP_CACHE_MAX_UOPS_LINE;
  if (accumulating_pw.n_uops % UOP_CACHE_MAX_UOPS_LINE) lines_needed++;

  if (INF_SIZE_UOP_CACHE || (INF_SIZE_UOP_CACHE_PW_SIZE_LIM 
      && accumulating_pw.n_uops <= INF_SIZE_UOP_CACHE_PW_SIZE_LIM)) {
    for (int ii = 0; ii < accumulating_pw.n_uops; ii++) {
      Op* op = uop_q[ii];
      Flag new_entry;
      hash_table_access_create(&inf_size_uop_cache, op->inst_info->addr, 
                                &new_entry);
    }
    success = TRUE;
  } else if (INF_SIZE_UOP_CACHE_PW_SIZE_LIM 
            && accumulating_pw.n_uops > INF_SIZE_UOP_CACHE_PW_SIZE_LIM) {
    success = FALSE;
  } else {
    // Is the PW too big?
    if (lines_needed > UOP_CACHE_ASSOC) {
      cache_invalidate(&uop_cache, accumulating_pw.first, &line_addr);
      success = FALSE;
    } else {
      // Insert it, taking appropriate number of lines
      for (int jj = 0; jj < lines_needed; jj++) {
        cur_line_data = (Uop_Cache_Data*) cache_insert(&uop_cache, 0, 
                        accumulating_pw.first, &line_addr, &repl_line_addr);
        // TODO: invalidate all lines with same pw start addr 
        // (not all with that have their PW start in this line)
        if (repl_line_addr) {
          cache_invalidate(&uop_cache, repl_line_addr, &line_addr);
        }
        memset(cur_line_data, 0, UOP_CACHE_LINE_DATA_SIZE);
        *cur_line_data = accumulating_pw;
      }
      success = TRUE;
    }

    if (success) {
      STAT_EVENT(0, UOP_CACHE_PWS_INSERTED);
      INC_STAT_EVENT(0, UOP_CACHE_LINES_INSERTED, lines_needed);
    }
    return success;
  }
}

static inline Flag in_uop_cache_search(Addr search_addr, Flag update_repl) {
  static Uop_Cache_Data cur_pw = {0};
  Addr line_addr;
  Uop_Cache_Data* uoc_data = NULL;
  Flag found = FALSE;

  // First check if current pw has this search addr
  if (cur_pw.first && search_addr >= cur_pw.first
                   && search_addr <= cur_pw.last) {
    found = TRUE;
  } else {
    // Next try to access a new PW starting at this addr
    if (cache_access_all(&uop_cache, search_addr, &line_addr, update_repl, 
                          (void**) &uoc_data)) {
      cur_pw = *uoc_data;
      found = TRUE;
    } else {
      memset(&cur_pw, 0, sizeof(cur_pw));
      found = FALSE;
    }
  }

  return found;
}

/**************************************************************************************/
/* in_uop_cache: Iterate over all possible ops and check if contained.
 *                    Other option: use cache to simulate capacity and maintain a map 
 *                      data structure for pcs
 */
Flag in_uop_cache(Addr pc, const Counter* op_num, Flag update_repl) {
  // A PW can span multiple cache entries. The next line used is either physically 
  // next in the set (use flag) or anywhere in the set (use pointer), depending on impl.
  // Here, don't care about order, just search all lines for pc in question
  STAT_EVENT(0, IN_UOP_CACHE_CALLED);

  if (ORACLE_PERFECT_UOP_CACHE) {
    if (update_repl) {
      STAT_EVENT(0, UOP_CACHE_HIT);
    }
    return TRUE;
  } else if (INF_SIZE_UOP_CACHE || INF_SIZE_UOP_CACHE_PW_SIZE_LIM) {
    return hash_table_access(&inf_size_uop_cache, pc) != NULL;
  }
  if (UOP_CACHE_SIZE == 0) {
    return FALSE;
  }

  static Counter next_op_num = 1;

  Flag found = in_uop_cache_search(pc, update_repl);

  if (update_repl) {
    STAT_EVENT(0, UOP_CACHE_MISS + found);
    if (op_num) {
      ASSERT(0, *op_num == next_op_num);
      next_op_num++;
    }
  }
  
  return found;
}

void end_accumulate(void) {
  if (UOP_CACHE_SIZE == 0 && !INF_SIZE_UOP_CACHE && !INF_SIZE_UOP_CACHE_PW_SIZE_LIM) {
    return;
  }

  if (accumulating_pw.n_uops > 0) {
    insert_uop_cache();
    memset(&accumulating_pw, 0, sizeof(accumulating_pw));
  }
}

/**************************************************************************************/
/* accumulate_op: accumulate into buffer. Insert into cache at end of PW. */
void accumulate_op(Op* op) {
  // Prediction Window termination conditions:
  // 1. end of icache line
  // 2. branch predicted taken
  // 3. predetermined number of branch NT (no limit in implementation)
  // 4. uop queue full
  // 5. too many uops to fit in entire set, even after evicting all entries

  // it is possible for an instr to be partially in 2 lines. 
  // For pw termination purposes, assume it is in first line.
  static Counter cons_op_num = 0;

  if ((UOP_CACHE_SIZE == 0 && !INF_SIZE_UOP_CACHE && !INF_SIZE_UOP_CACHE_PW_SIZE_LIM) 
      || ORACLE_PERFECT_UOP_CACHE) {
    return;
  }

  Addr cur_icache_line_addr = get_cache_line_addr(&uop_cache,
                                                  accumulating_pw.first);
  Addr icache_line_addr = get_cache_line_addr(&uop_cache, op->inst_info->addr);

  if (!cur_icache_line_addr) {
    accumulating_pw.first = op->inst_info->addr;
    cur_icache_line_addr = icache_line_addr;
    cons_op_num = op->op_num + 1;
  } else {
    ASSERT(0, op->op_num == cons_op_num);
    cons_op_num++;
  }
  
  Flag end_of_icache_line = icache_line_addr != cur_icache_line_addr;
  Flag branch_pt = op->oracle_info.pred == TAKEN;
  Flag uop_q_full = (accumulating_pw.n_uops + 1 > UOP_QUEUE_SIZE);

  if (end_of_icache_line || branch_pt || uop_q_full) {
    end_accumulate();
  }

  if (accumulating_pw.n_uops == 0) {
    // occurs when THIS fxn call drains the uop queue (insertion into uop cache)
    accumulating_pw.first = op->inst_info->addr;
  }
  uop_q[accumulating_pw.n_uops] = op;
  accumulating_pw.last = op->inst_info->addr;
  accumulating_pw.n_uops++;
};