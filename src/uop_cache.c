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
#include "memory/memory.h"
#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"
#include "uop_cache.h"
#include "icache_stage.h" //needed for get_pw_lookahead_buffer
#include "uop_cache_prefetch_decoder.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_UOP_CACHE, ##args)

// Uop cache is byte-addressable, so tag/set index are generated from full address (no offset)
// Uop cache uses icache tag + icache offset as full TAG
#define UOP_CACHE_LINE_SIZE       ICACHE_LINE_SIZE
#define UOP_QUEUE_SIZE            1000 // at least UOP_CACHE_ASSOC * UOP_CACHE_MAX_UOPS_LINE
#define UOP_CACHE_LINE_DATA_SIZE  sizeof(Uop_Cache_Data)

/**************************************************************************************/
/* Local Prototypes */

static inline Flag insert_uop_cache(void);
static inline Flag in_uop_cache_search(Addr search_addr, Flag update_repl);

/**************************************************************************************/
/* Global Variables */

Cache uop_cache;
uns8 proc_id;

// uop trace/bbl accumulation
static Uop_Cache_Data accumulating_pw = {0};
static Counter cons_op_num = 0;
static Op* uop_q[UOP_QUEUE_SIZE];

// k: instr addr
Hash_Table inf_size_uop_cache;
// indexed by start addr of PW
Hash_Table pc_to_pw;
Flag uoc_prefetching_enabled;

Addr addr_following_resteer_bf = 0;  // Addr that follows a resteer or fetch barrier

/**************************************************************************************/
/* init_uop_cache */

void init_uop_cache(uns8 pid) {
  proc_id = pid;
  if (!UOP_CACHE_ENABLE) {
    return;
  }
  if (INF_SIZE_UOP_CACHE || INF_SIZE_UOP_CACHE_PW_SIZE_LIM) {
    init_hash_table(&inf_size_uop_cache, "infinite sized uop cache", 15000000, sizeof(int));
  }

  uoc_prefetching_enabled = UOC_PREF || UOC_ORACLE_PREF || UOC_ZERO_LATENCY_PREF;
  if (uoc_prefetching_enabled) {
    init_hash_table(&pc_to_pw, "Log of all PWs decoded", 15000000,
                    sizeof(Uop_Cache_Data));
  }
  // The cache library computes the number of entries from cache_size_bytes/cache_line_size_bytes,
  uns uop_cache_lines = UOP_CACHE_UOP_CAPACITY / UOP_CACHE_MAX_UOPS_LINE;
  init_cache(&uop_cache, "UOP_CACHE", uop_cache_lines * UOP_CACHE_LINE_SIZE, UOP_CACHE_ASSOC,
             UOP_CACHE_LINE_SIZE, UOP_CACHE_LINE_DATA_SIZE, UOP_CACHE_REPL);
  uop_cache.tag_incl_offset = TRUE;
}

Flag pw_insert(Uop_Cache_Data pw) {
  Uop_Cache_Data* cur_line_data = NULL;
  Addr line_addr;  
  Addr repl_line_addr;
  pw.used = 0;

  int lines_needed = pw.n_uops / UOP_CACHE_MAX_UOPS_LINE;
  if (pw.n_uops % UOP_CACHE_MAX_UOPS_LINE) lines_needed++;
  ASSERT(0, lines_needed > 0);

  // Is the PW too big?
  if (lines_needed > UOP_CACHE_ASSOC) {
    STAT_EVENT(ic->proc_id, UOP_CACHE_PW_INSERT_FAILED_TOO_LONG + pw.prefetch);
    return FALSE;
  } else if (cache_access(&uop_cache, pw.first, &line_addr, FALSE)) {
    STAT_EVENT(ic->proc_id, UOP_CACHE_PW_INSERT_FAILED_CACHE_HIT + pw.prefetch);
    return FALSE;
  } else {
    // If REPL_RESTEER and not addr_following_resteer_bf, consider NOT inserting at all.
    // (here I insert into the LRU position) 
    Cache_Insert_Repl insert_repl = INSERT_REPL_DEFAULT;
    if (UOP_CACHE_REPL == REPL_RESTEER && pw.first != addr_following_resteer_bf) {
      insert_repl = INSERT_REPL_LRU;
    }
    DEBUG(ic->proc_id, "PW inserted. addr=0x%llx, set=%u, lines_needed=%i\n",
          pw.first, ext_cache_index(&uop_cache, pw.first, &line_addr, &line_addr), lines_needed);
    // FIRST preemptively evict the PWs to create space for the new PW. 
    // This is so that multiple lines from the same PW
    // inserted into the LRU position do not evict each other.
    while (cache_get_invalid_line_count(&uop_cache, pw.first) < lines_needed) {
      Uop_Cache_Data evict_pw = *(Uop_Cache_Data*)get_next_valid_repl_line(&uop_cache, ic->proc_id, pw.first);
      cache_invalidate(&uop_cache, evict_pw.first, &line_addr);
      DEBUG(ic->proc_id, "PW evicted. addr=0x%llx, set=%u\n",
            evict_pw.first, ext_cache_index(&uop_cache, evict_pw.first, &line_addr, &line_addr));
    }
    for (int jj = 0; jj < lines_needed; jj++) {
      cur_line_data = (Uop_Cache_Data*) cache_insert_replpos(&uop_cache, ic->proc_id,
                      pw.first, &line_addr, &repl_line_addr, insert_repl, pw.prefetch);
      ASSERT(ic->proc_id, !repl_line_addr);
      memset(cur_line_data, 0, UOP_CACHE_LINE_DATA_SIZE);
      *cur_line_data = pw;
    }
    STAT_EVENT(0, UOP_CACHE_PWS_INSERTED);
    INC_STAT_EVENT(0, UOP_CACHE_LINES_INSERTED, lines_needed);
  }
  return TRUE;
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
  Flag success = FALSE;
  Flag new_entry;

  if (uoc_prefetching_enabled) {
    Uop_Cache_Data* saved_pw = (Uop_Cache_Data*) hash_table_access_create(
                                &pc_to_pw, accumulating_pw.first, &new_entry);
    *saved_pw = accumulating_pw;
  }
  int lines_needed = accumulating_pw.n_uops / UOP_CACHE_MAX_UOPS_LINE;
  if (accumulating_pw.n_uops % UOP_CACHE_MAX_UOPS_LINE) lines_needed++;

  if (INF_SIZE_UOP_CACHE || (INF_SIZE_UOP_CACHE_PW_SIZE_LIM 
      && accumulating_pw.n_uops <= INF_SIZE_UOP_CACHE_PW_SIZE_LIM)) {
    for (int ii = 0; ii < accumulating_pw.n_uops; ii++) {
      Op* op = uop_q[ii];
      hash_table_access_create(&inf_size_uop_cache, op->inst_info->addr, 
                                &new_entry);
    }
    success = TRUE;
  } else if (INF_SIZE_UOP_CACHE_PW_SIZE_LIM 
            && accumulating_pw.n_uops > INF_SIZE_UOP_CACHE_PW_SIZE_LIM) {
    success = FALSE;
  } else {
    success = pw_insert(accumulating_pw);
  }

  return success;
}

static inline Flag in_uop_cache_search(Addr search_addr, Flag update_repl) {
  static Uop_Cache_Data cur_pw = {0};
  Addr line_addr;
  Uop_Cache_Data* uoc_data = NULL;

  // First check if current pw has this search addr
  if (cur_pw.first && search_addr >= cur_pw.first
                   && search_addr <= cur_pw.last) {
    uoc_data = &cur_pw;
    if (uoc_data)
      DEBUG(ic->proc_id, "UOC hit (cur_pw). addr=0x%llx, set=%u\n",
            search_addr, ext_cache_index(&uop_cache, search_addr, &line_addr, &line_addr));
  } else {
    // Next try to access a new PW starting at this addr
    uoc_data = cache_access(&uop_cache, search_addr, &line_addr, update_repl);
    // Only update state if this access should change state
    if (update_repl && uoc_data) {
      if (uoc_data->prefetch && !uoc_data->used) {
        STAT_EVENT(0, UOP_CACHE_PREFETCH_USED);
      }
      if (!uoc_data->used) {
        STAT_EVENT(0, UOP_CACHE_LINES_USED);
      }
      uoc_data->used += 1;
      cur_pw = *uoc_data;
      DEBUG(ic->proc_id, "UOC hit (new PW). addr=0x%llx, set=%u\n",
            search_addr, ext_cache_index(&uop_cache, search_addr, &line_addr, &line_addr));
    } else if (update_repl) {
      memset(&cur_pw, 0, sizeof(cur_pw));
      DEBUG(ic->proc_id, "UOC miss. addr=0x%llx, set=%u\n",  
            search_addr, ext_cache_index(&uop_cache, search_addr, &line_addr, &line_addr));
      // maybe also add uop granularity, to make sure ending conditions are OK
    }
  }

  return uoc_data != NULL;
}

/**************************************************************************************/
/* in_uop_cache: Iterate over all possible ops and check if contained.
 *                    Other option: use cache to simulate capacity and maintain a map 
 *                      data structure for pcs
 */
Flag in_uop_cache(Addr pc, Flag update_repl) {
  // A PW can span multiple cache entries. The next line used is either physically 
  // next in the set (use flag) or anywhere in the set (use pointer), depending on impl.
  // Here, don't care about order, just search all lines for pc in question
  STAT_EVENT(0, IN_UOP_CACHE_CALLED);
  if (!UOP_CACHE_ENABLE) {
    return FALSE;
  }

  if (ORACLE_PERFECT_UOP_CACHE) {
    if (update_repl) {
      STAT_EVENT(0, UOP_CACHE_HIT);
    }
    return TRUE;
  } else if (INF_SIZE_UOP_CACHE || INF_SIZE_UOP_CACHE_PW_SIZE_LIM) {
    return hash_table_access(&inf_size_uop_cache, pc) != NULL;
  }

  Flag found = in_uop_cache_search(pc, update_repl);
  if (update_repl) {
    STAT_EVENT(0, UOP_CACHE_MISS + found);
  }
  
  return found;
}

void end_accumulate(void) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  if (accumulating_pw.n_uops > 0) {
    STAT_EVENT(ic->proc_id, UOP_CACHE_PW_LENGTH_1 + (accumulating_pw.n_uops-1));
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

  // It is possible for an instr to be partially in 2 lines;
  // for pw termination purposes, assume it is in first line.
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  Addr cur_icache_line_addr = get_cache_line_addr(&ic->icache,
                                                  accumulating_pw.first);
  Addr icache_line_addr = get_cache_line_addr(&ic->icache, op->inst_info->addr);

  // Skipped ops means that there was a uop cache hit, so the accumulating_pw
  // may need to be flushed. This must be done in the same place accumulation is done.
  if (op->op_num > cons_op_num) {
    STAT_EVENT(0, UOP_CACHE_ICACHE_SWITCH);
    end_accumulate();
    cur_icache_line_addr = 0;
  }
  ASSERT(proc_id, op->op_num >= cons_op_num);  // At recovery cons_op_num should be reset

  if (!cur_icache_line_addr) {
    accumulating_pw.first = op->inst_info->addr;
    cur_icache_line_addr = icache_line_addr;
    cons_op_num = op->op_num + 1;
  } else {
    ASSERT(0, op->op_num == cons_op_num);
    cons_op_num++;
  }
  
  Flag end_of_icache_line = icache_line_addr != cur_icache_line_addr;
  Flag uop_q_full = (accumulating_pw.n_uops + 1 == UOP_QUEUE_SIZE);
  if (end_of_icache_line || uop_q_full) {
    end_accumulate();
  }

  if (accumulating_pw.n_uops == 0) {
    // occurs when THIS fxn call drains the uop queue (insertion into uop cache)
    accumulating_pw.first = op->inst_info->addr;
  }
  uop_q[accumulating_pw.n_uops] = op;
  accumulating_pw.last = op->inst_info->addr;
  accumulating_pw.last_op_num = op->op_num;
  accumulating_pw.n_uops++;

  Flag branch_pt = op->table_info->cf_type && op->oracle_info.pred == TAKEN;
  if (branch_pt) {
    end_accumulate();
  }
};

Flag uop_cache_fill_prefetch(Addr pw_start_addr, Flag fdip_on_path) {
  printf("Reimplement here using decoupled_fe API\n");
  ASSERT(proc_id, 0);
  ASSERT(proc_id, uoc_prefetching_enabled);
  Uop_Cache_Data pw;
  if (!UOP_CACHE_ENABLE) {
    return FALSE;
  }

  // on-path / off-path is not working, even for correct-path prefetching.
  fdip_on_path = FALSE; // just use legacy method.

  if (fdip_on_path) {
    pw = get_pw_lookahead_buffer(pw_start_addr);
  } else {
    Uop_Cache_Data* pw_p = (Uop_Cache_Data*) hash_table_access(&pc_to_pw, pw_start_addr);
    // if PW has not been decoded before, do nothing. Hopefully this is uncommon.
    if (pw_p == NULL) {
      STAT_EVENT(0, UOP_CACHE_PREFETCH_FAILED_PW_NEVER_SEEN);
      return FALSE;
    }
    pw = *pw_p;
  }
  ASSERT(0, pw.first == pw_start_addr);
  pw.prefetch = TRUE;
  start_decoding_uop_cache_prefetch(pw);
  return TRUE;
}

Flag uop_cache_issue_prefetch(Addr pw_start_addr, Flag on_path) {
  int prefetch_success = FALSE;

  if (UOC_ZERO_LATENCY_PREF) {
    prefetch_success = uop_cache_fill_prefetch(pw_start_addr, on_path);
  } else if (in_uop_cache(pw_start_addr, FALSE)) {
    STAT_EVENT(ic->proc_id, UOP_CACHE_HIT_NO_PREFETCH);
  } else {
    // If no op is provided, on_path is assumed.
    // The delay is set to the decode time to allow instr to decode into uops.
    prefetch_success = new_mem_req(MRT_UOCPRF, proc_id, pw_start_addr,
              ICACHE_LINE_SIZE, DECODE_CYCLES, NULL, instr_fill_line,
              unique_count,
              0);
    if(!prefetch_success) {
      STAT_EVENT(0, UOP_CACHE_PREFETCH_FAILED_MEMREQ_FAILED);
    }
  }

  return prefetch_success;
}

// This is called after a resteer is resolved or fetch barrier identified.
void set_addr_following_resteer_bf(Addr addr) {
  addr_following_resteer_bf = addr;
  if (uop_cache.repl_policy == REPL_RESTEER) {
    update_repl_resteer_policy(&uop_cache, addr);
  }
}

Uop_Cache_Data get_pw_lookahead_buffer(Addr addr) {
  printf("Reimplement here using decoupled_fe API\n");
  ASSERT(0,0);
}

void recover_uop_cache(void) {
  if (accumulating_pw.last_op_num > bp_recovery_info->recovery_op_num) {
    memset(&accumulating_pw, 0, sizeof(accumulating_pw));
  } else {
    end_accumulate();
  }
  cons_op_num = bp_recovery_info->recovery_op_num + 1;
}
