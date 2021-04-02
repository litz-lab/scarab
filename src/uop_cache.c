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
#define UOP_CACHE_LINE_DATA_SIZE  UOP_CACHE_MAX_UOPS_LINE * sizeof(Addr)

/**************************************************************************************/
/* Local Prototypes */

static inline void insert_uop_cache(void);
static inline Flag in_uop_cache_search(Addr pw_start_addr, Addr search_addr, Flag update_repl);

/**************************************************************************************/
/* Global Variables */

Cache uop_cache;

// uop trace/bbl accumulation
static Op* uop_q[UOP_QUEUE_SIZE];
static int uop_q_len = 0;

// cache line of currently accumulating PW
static Addr cur_icache_line_addr = 0;

// start_addr of current PW/bbl
static Addr cur_pw_start_addr = 0;

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
 *                   Drain buffer and insert. 
 */

void insert_uop_cache() { 
  // PW may span multiple cache entries. 1 entry per line. Additional terminating conditions per line:
  // 1. max uops per line
  // 2. max imm/disp per line
  // 3. max micro-coded instr per line (not simulated)

  // Invalidate after hitting maximum allowed number of lines, as in gem5
  // Each entry/pw indexed by physical addr of 1st instr, so insert each line using same addr
  // Invalidation: Let LRU handle it by placing PW in one line at a time.   
  
  ASSERT(0, uop_q_len);

  Addr start_addr = uop_q[0]->inst_info->addr;
  Addr line_addr;  
  Addr repl_line_addr;
  Addr* cur_line_data = NULL;
        
  int n_lines_used = 0;
  int n_uops_line = 0;
  int n_imm_disp_line = 0;
 
  for (int ii = 0; ii < uop_q_len; ii++) {
    Op* op = uop_q[ii];
    if (INF_SIZE_UOP_CACHE || (INF_SIZE_UOP_CACHE_PW_SIZE_LIM && uop_q_len <= INF_SIZE_UOP_CACHE_PW_SIZE_LIM)) {
      Flag new_entry;
      hash_table_access_create(&inf_size_uop_cache, op->inst_info->addr, &new_entry);
      continue;
    } else if (INF_SIZE_UOP_CACHE_PW_SIZE_LIM && uop_q_len > INF_SIZE_UOP_CACHE_PW_SIZE_LIM) {
      break;
    }
    int imm_disp = (op->inst_info->lit > 0) + (op->inst_info->disp > 0);
    
    if (cur_line_data == NULL || n_uops_line == UOP_CACHE_MAX_UOPS_LINE || (n_imm_disp_line + imm_disp > UOP_CACHE_MAX_IMM_DISP_LINE)) {
      // insert a new line
      cur_line_data = (Addr*) cache_insert(&uop_cache, 0, start_addr, &line_addr, &repl_line_addr);
      // TODO: invalidate all lines with same start_addr (not necessarily all with that line!)
      if (repl_line_addr) {
        cache_invalidate(&uop_cache, repl_line_addr, &line_addr);
      }
      n_uops_line = 0;
      n_imm_disp_line = 0;
      n_lines_used++;
      memset(cur_line_data, 0, UOP_CACHE_LINE_DATA_SIZE);
    }

    //add data to line
    cur_line_data[n_uops_line] = op->inst_info->addr;
    n_uops_line++;
    n_imm_disp_line += imm_disp;

    // if we used up too many full cache lines, invalidate all of them
    if (n_lines_used >= UOP_CACHE_ASSOC) {
      cache_invalidate(&uop_cache, start_addr, &line_addr);
      break;
    }
  }

  uop_q_len = 0;
}

// TODO: optimize, very inefficient. Should store line data so multiple cache accesses not needed for same PW
// Access the PW, check if it has the search_addr in it. 
static inline Flag in_uop_cache_search(Addr pw_start_addr, Addr search_addr, Flag update_repl) {
  Addr* line_data[UOP_QUEUE_SIZE]; // way over-allocated
  Addr line_addr;
  int matched_lines = cache_access_all(&uop_cache, pw_start_addr, &line_addr, FALSE, (void*) line_data);

  for (int ii = 0; ii < matched_lines; ii++) {
    Addr* line = (Addr*) line_data[ii];
    for (int jj = 0; jj < UOP_CACHE_MAX_UOPS_LINE && line[jj]; jj++) {
      if (line[jj] == search_addr) {
        // actually update replacement since it is a real hit.
        cache_access_all(&uop_cache, pw_start_addr, &line_addr, update_repl, (void*) line_data);
        return TRUE;
      }
    }
  }
  return FALSE;
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
  
  Flag found = FALSE;
  // Is it in current PW that was fetched last?
  if (cur_pw_start_addr != 0 && in_uop_cache_search(cur_pw_start_addr, pc, FALSE)) {
    found = TRUE;
  }
  // Is it itself start of new PW?
  if (in_uop_cache_search(pc, pc, update_repl)) {
    cur_pw_start_addr = pc;
    found = TRUE;
  }

  if (update_repl) {
    STAT_EVENT(0, UOP_CACHE_MISS + found);

    if (op_num) {
      ASSERT(0, *op_num == next_op_num);
      next_op_num++;
    }

  }
  if (!found) {
    cur_pw_start_addr = 0;
  }
  
  return found;
}

void end_accumulate(void) {
  if (UOP_CACHE_SIZE == 0 && !INF_SIZE_UOP_CACHE && !INF_SIZE_UOP_CACHE_PW_SIZE_LIM) {
    return;
  }

  if (uop_q_len) {
    insert_uop_cache();
    cur_icache_line_addr = 0;
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

  Addr icache_line_addr = get_cache_line_addr(&uop_cache, op->inst_info->addr);

  if (!cur_icache_line_addr) {
    cur_icache_line_addr = icache_line_addr;
    cons_op_num = op->op_num + 1;
  } else {
    ASSERT(0, op->op_num == cons_op_num);
    cons_op_num++;
  }
  
  Flag end_of_icache_line = icache_line_addr != cur_icache_line_addr;
  Flag branch_pt = op->oracle_info.pred == TAKEN;
  Flag uop_q_full = (uop_q_len + 1 > UOP_QUEUE_SIZE);

  if (end_of_icache_line || branch_pt || uop_q_full) {
    end_accumulate();
  }

  if (!uop_q_len) {
    cur_icache_line_addr = icache_line_addr;
  }
  uop_q[uop_q_len] = op;
  uop_q_len++;
};