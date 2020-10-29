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

#include "uop_cache.h"
#include <unordered_set> //assuming I just want to know if the instr is cached (I already know the uops from memtrace)
// if i want to fetch an entire bbl i need to brainstorm a different technique.

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECODE_STAGE, ##args)
#define STAGE_MAX_OP_COUNT ISSUE_WIDTH
#define STAGE_MAX_DEPTH DECODE_CYCLES


/**************************************************************************************/
/* Global Variables */

/* set of pcs that are found in uop cache */
std::unordered_set<Addr> uc_set{};

/**************************************************************************************/
/* insert_uop_cache: */

void insert_uop_cache(Addr pc) {
  auto res = uc_set.insert(pc);
  // ASSERT(0, res.second);
}

/**************************************************************************************/
/* insert_uop_cache: */

Op** get_ops_uop_cache() {
  return NULL;
}