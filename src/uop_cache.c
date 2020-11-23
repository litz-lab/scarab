/***************************************************************************************
 * File         : decode_stage.c
 * Author       : HPS Research Group
 * Date         : 2/17/1999
 * Description  : simulates the latency due to decode stage. (actual uop decoding was
                    done earlier)
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
#include "thread.h" /* for td */

#include "libs/cache_lib.h"
#include "memory/memory.param.h"
#include "uop_cache.h"
// if i want to fetch an entire bbl i need to brainstorm a different technique.

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_UOP_CACHE, ##args)

#define UOP_CACHE_SIZE        32
#define UOP_CACHE_ASSOC       8
#define UOP_CACHE_LINE_SIZE   ICACHE_LINE_SIZE

#define MAX_UOPS_LINE         4
#define MAX_IMM_DISP_LINE     4

#define UOP_QUEUE_SIZE        120



/**************************************************************************************/
/* Global Variables */

Cache uop_cache;

// uop trace/bbl accumulation
Op* uop_q[UOP_QUEUE_SIZE];
int uop_q_len = 0;
int n_imm_disp = 0;
int n_uops = 0;
// cache line of currently accumulating PW
Addr cur_icache_line_addr = 0;


/**************************************************************************************/
/* init_uop_cache */

void init_uop_cache(uns8 proc_id) {
  int data_size = sizeof(Op*) * MAX_UOPS_LINE;
  init_cache(&uop_cache, "UOP_CACHE", UOP_CACHE_SIZE, UOP_CACHE_ASSOC, UOP_CACHE_LINE_SIZE,
             data_size, REPL_TRUE_LRU);
  
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
  Op** cur_line_data = NULL;
  
  // cache_access should NOT succeed because we shouldn't have started accumulating uops
  // if this PW was already in cache
  ASSERT(0, !cache_access(&uop_cache, start_addr, &line_addr, FALSE));
        
  int n_lines_used = 0;
  int n_uops_line = 0;
  int n_imm_disp_line = 0;
 
  for (int ii = 0; ii < uop_q_len; ii++) {
    Op* op = uop_q[ii];
    Addr inst_addr = op->inst_info->addr;
    int imm_disp = (op->inst_info->lit > 0) + (op->inst_info->disp > 0);
    
    if (cur_line_data == NULL || n_uops_line == MAX_UOPS_LINE || (n_imm_disp_line + imm_disp > MAX_IMM_DISP_LINE)) {
      // insert a new line
      cur_line_data = (Op**) cache_insert(&uop_cache, 0, inst_addr, &line_addr, &repl_line_addr);

      n_uops_line = 0;
      n_imm_disp_line = 0;
      n_lines_used++;
    }

    //add data to line
    cur_line_data[n_uops_line] = op;
    n_uops_line++;
    n_imm_disp_line += imm_disp;

    // if we used up too many full cache lines, invalidate all of them
    if (n_lines_used >= UOP_CACHE_ASSOC) {
      cache_invalidate(&uop_cache, inst_addr, &line_addr);
      break;
    }
  }

  uop_q_len = 0;
}

/**************************************************************************************/
/* in_uop_cache: return 0 or 1
 *                    Iterate over all possible ops and check if contained.
 *                    Other option: use cache to simulate capacity and maintain a map 
 *                      data structure for pcs
 */

  // A PW can span multiple cache entries. In this implementation, the next line used is physically
  // next in the set (use flag to indicate entry continues) or anywhere in the set
  // (use pointer for next entry).
  // Flag start_of_uop_entry

int in_uop_cache(Addr pc) {
  // cache_access returns first line that matches... create own cache_access method that returns an array + array_len
  
  // what if multiple lines have same tag? as they will -- need to check all for uop in question
  // cache_access();
  return 0;
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
  Addr icache_line_addr = get_cache_line_addr(&uop_cache, op->inst_info->addr);
  
  Flag end_of_icache_line = icache_line_addr != cur_icache_line_addr;
  Flag branch_pt = op->oracle_info.pred == TAKEN;
  Flag uop_q_full = (n_uops + 1 > UOP_QUEUE_SIZE);

  if (end_of_icache_line || branch_pt || uop_q_full) {
    insert_uop_cache();
  }

  uop_q[uop_q_len] = op;
  uop_q_len++;
};
