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

#define MAX_UOPS_ENTRY        4
#define MAX_IMM_DISP_ENTRY    4

#define UOP_QUEUE_SIZE        120



/**************************************************************************************/
/* Global Variables */

// keep track of cache line of currently accumulating PW (end of CL is terminating condition)
// dependent on icache LINE SIZE
// cache_line = 0;  // tag+index

// Cache line should be superclass of base line. incl counters of imm/disp, n_uops, etc
Cache uop_cache;

// uop trace/bbl accumulation before insertion
Op* uop_q[UOP_QUEUE_SIZE];
int uop_q_len = 0;
int n_imm_disp = 0;
int n_uops = 0;



/**************************************************************************************/
/* init_uop_cache */

void init_uop_cache(uns8 proc_id) {
  init_cache(&uop_cache, "UOP_CACHE", UOP_CACHE_SIZE, UOP_CACHE_ASSOC, UOP_CACHE_LINE_SIZE,
             0, REPL_TRUE_LRU);
  
}

/**************************************************************************************/
/* */


/**************************************************************************************/
/* insert_uop_cache: private method, only called by accumulate_op */

void insert_uop_cache(Op** pw) {
  return;
}

/**************************************************************************************/
/* get_ops_uop_cache: return 0 or 1*/

int in_uop_cache(Addr pc) {
  return 0;
}

/**************************************************************************************/
/* accumulate_op: accumulate into buffer. Insert into cache at end of PW. */
void accumulate_op(Op* op) {
  // it is possible for an instr to be partially in 2 lines. For termination purposes,
  // it is fine to assume it is in first line.
  Addr icache_line_addr = ROUND_DOWN(op->inst_info->addr, ICACHE_LINE_SIZE);
  
  if (icache_line_addr) {

  }
};
