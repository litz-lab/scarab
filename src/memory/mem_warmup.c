/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
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
 * File         : memory/mem_warmup.c
 * Description  : functional warmup (--warmup) of the cache hierarchy
 *
 * The timing model moves lines with memory requests through bank ports, MSHRs and
 * writeback files; warmup has no time, so this file applies the same placement rules
 * directly to the caches. Each rule names the code it mirrors -- change them together.
 *
 * Demand miss at L0 (icache/dcache), looked up MLC then LLC:
 *  - Non-exclusive: the fill inserts at every level it came through, LLC first
 *    (fill_inserts_at_l1/fill_inserts_at_mlc are TRUE), and an LLC hit fills the MLC too
 *    (fill_mlc in mem_process_l1_hit_access).
 *  - Exclusive: the fill inserts only at the L0 that asked (EXCL_FILL_SKIPPED_LLC/_MLC).
 *    A hit below L0 takes the line away and its dirty bit moves up, except that a dirty
 *    line going to the icache keeps its lower copy (mem_invalidate_on_promote).
 * L0 victim: the dcache writes back a dirty victim, and when exclusive a clean one too
 *   (dcache_fill_get_cacheline); the icache demotes a victim only when exclusive
 *   (icache_demote_victim). Both go to the MLC, or to the LLC without one
 *   (new_mem_dc_wb_req, mem_ic_wb_file).
 * MLC victim: a dirty one, or any one when exclusive, goes to the LLC (mlc_fill_line).
 * LLC victim: leaves the chip; nothing to warm.
 * Writeback arriving at a level: a hit marks the line dirty (mem_process_*_hit_access);
 *   a miss inserts it, unless, when exclusive, the other uncore level holds it, which
 *   then takes the dirty bit (the dup checks in mlc_fill_line/l1_fill_line).
 *
 * Not warmed here: prefetchers, the L1/dcache prefetch caches (they take only prefetch
 * and off-path fills), and write-through levels (asserted off).
 ***************************************************************************************/

#include "memory/mem_warmup.h"

#include <string.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "libs/cache_lib.h"
#include "memory/cache_part.h"
#include "memory/memory.h"

#include "cmp_model.h"
#include "statistics.h"

typedef enum {
  WARM_MLC,
  WARM_LLC
} Warm_Level;

static inline Cache* warm_cache(uns proc_id, Warm_Level level) {
  return level == WARM_MLC ? &cmp_model.memory.uncores[proc_id].mlc->cache
                           : &cmp_model.memory.uncores[proc_id].l1->cache;
}

/* The level an L0 victim goes down to. */
static inline Warm_Level warm_below_l0(void) {
  return MLC_PRESENT ? WARM_MLC : WARM_LLC;
}

static void warm_writeback(uns proc_id, Warm_Level level, Addr addr, Flag dirty);

/* Insert a line that is not at this level, sending its victim down first. */
static void warm_uncore_insert(uns proc_id, Warm_Level level, Addr addr, Flag dirty) {
  Cache* cache = warm_cache(proc_id, level);
  Addr line_addr, repl_line_addr;
  Flag repl_line_valid;
  L1_Data* victim = (L1_Data*)get_next_repl_line(cache, proc_id, addr, &repl_line_addr, &repl_line_valid);
  if (repl_line_valid) {
    uns repl_proc_id = get_proc_id_from_cmp_addr(repl_line_addr);
    if (level == WARM_MLC) {
      if (victim->dirty || EXCLUSIVE_CACHES)
        warm_writeback(repl_proc_id, WARM_LLC, repl_line_addr, victim->dirty);
    } else {
      STAT_EVENT(repl_proc_id, NORESET_L1_EVICT);
      STAT_EVENT(repl_proc_id, NORESET_L1_EVICT_NONPREF);
    }
  }
  if (level == WARM_LLC) {
    STAT_EVENT(proc_id, NORESET_L1_FILL);
    STAT_EVENT(proc_id, NORESET_L1_FILL_NONPREF);
  }
  /* cache_insert evicts the victim chosen above: nothing touched this cache in between. */
  L1_Data* data = (L1_Data*)cache_insert(cache, proc_id, addr, &line_addr, &repl_line_addr);
  memset(data, 0, sizeof(L1_Data));
  data->proc_id = proc_id;
  data->dirty = dirty;
}

/* A writeback (dirty) or a clean demotion arriving at an uncore level. */
static void warm_writeback(uns proc_id, Warm_Level level, Addr addr, Flag dirty) {
  Addr line_addr;
  if (level == WARM_LLC && L1_PART_SHADOW_WARMUP)
    cache_part_l1_warmup(proc_id, addr);
  L1_Data* line = (L1_Data*)cache_access(warm_cache(proc_id, level), addr, &line_addr, TRUE);
  if (line) {
    line->dirty |= dirty;
    return;
  }
  if (EXCLUSIVE_CACHES && MLC_PRESENT) {
    Warm_Level other = level == WARM_MLC ? WARM_LLC : WARM_MLC;
    L1_Data* dup = (L1_Data*)cache_access(warm_cache(proc_id, other), addr, &line_addr, FALSE);
    if (dup) {
      dup->dirty |= dirty;
      return;
    }
  }
  warm_uncore_insert(proc_id, level, addr, dirty);
}

/* A demand hit below L0. Returns the dirty bit the line brings up. */
static Flag warm_promote(uns proc_id, Warm_Level level, L1_Data* line, Addr addr, Flag is_inst) {
  if (!EXCLUSIVE_CACHES || !EXCLUSIVE_PROMOTE_INVALIDATE)
    return FALSE;
  Flag dirty = line->dirty;
  if (dirty && is_inst)
    return FALSE; /* the icache has nowhere to carry the dirty bit: the lower copy stays */
  Addr line_addr;
  cache_invalidate(warm_cache(proc_id, level), addr, &line_addr);
  return dirty;
}

/* A demand miss at L0 looked up below it. Returns the dirty bit the line brings up. */
static Flag warm_uncore_demand(uns proc_id, Addr addr, Flag is_inst) {
  Addr line_addr;
  if (MLC_PRESENT) {
    L1_Data* mlc_line = (L1_Data*)cache_access(warm_cache(proc_id, WARM_MLC), addr, &line_addr, TRUE);
    if (mlc_line)
      return warm_promote(proc_id, WARM_MLC, mlc_line, addr, is_inst);
  }
  if (L1_PART_SHADOW_WARMUP)
    cache_part_l1_warmup(proc_id, addr);
  L1_Data* llc_line = (L1_Data*)cache_access(warm_cache(proc_id, WARM_LLC), addr, &line_addr, TRUE);
  if (llc_line) {
    Flag dirty = warm_promote(proc_id, WARM_LLC, llc_line, addr, is_inst);
    if (!EXCLUSIVE_CACHES && MLC_PRESENT)
      warm_uncore_insert(proc_id, WARM_MLC, addr, FALSE);
    return dirty;
  }
  if (!EXCLUSIVE_CACHES) {
    warm_uncore_insert(proc_id, WARM_LLC, addr, FALSE);
    if (MLC_PRESENT)
      warm_uncore_insert(proc_id, WARM_MLC, addr, FALSE);
  }
  return FALSE;
}

static void mem_warmup_asserts(void) {
  ASSERTM(0, !MLC_WRITE_THROUGH && !L1_WRITE_THROUGH, "WARMUP does not model write-through caches\n");
}

Flag mem_warmup_icache(uns proc_id, Addr ia, Addr* line_addr) {
  mem_warmup_asserts();
  Cache* icache = &cmp_model.icache_stage[proc_id].icache;
  Addr repl_line_addr;
  if (cache_access(icache, ia, line_addr, TRUE))
    return TRUE;

  warm_uncore_demand(proc_id, ia, TRUE);
  if (EXCLUSIVE_CACHES) {
    Flag repl_line_valid;
    get_next_repl_line(icache, proc_id, ia, &repl_line_addr, &repl_line_valid);
    if (repl_line_valid)
      warm_writeback(get_proc_id_from_cmp_addr(repl_line_addr), warm_below_l0(), repl_line_addr, FALSE);
  }
  cache_insert(icache, proc_id, ia, line_addr, &repl_line_addr);
  return FALSE;
}

void mem_warmup_dcache(uns proc_id, Addr va, Flag is_store) {
  mem_warmup_asserts();
  Cache* dcache = &cmp_model.dcache_stage[proc_id].dcache;
  Addr line_addr, repl_line_addr;
  Dcache_Data* data = (Dcache_Data*)cache_access(dcache, va, &line_addr, TRUE);
  if (data) {
    data->dirty |= is_store;
    data->read_count[0] += !is_store;
    data->write_count[0] += is_store;
    return;
  }

  Flag dirty_up = warm_uncore_demand(proc_id, va, FALSE);
  Flag repl_line_valid;
  Dcache_Data* victim = (Dcache_Data*)get_next_repl_line(dcache, proc_id, va, &repl_line_addr, &repl_line_valid);
  if (repl_line_valid && (victim->dirty || EXCLUSIVE_CACHES))
    warm_writeback(get_proc_id_from_cmp_addr(repl_line_addr), warm_below_l0(), repl_line_addr, victim->dirty);
  data = (Dcache_Data*)cache_insert(dcache, proc_id, va, &line_addr, &repl_line_addr);
  memset(data, 0, sizeof(Dcache_Data));
  data->dirty = dirty_up || is_store;
  data->read_count[0] = !is_store;
  data->write_count[0] = is_store;
}

/**************************************************************************************/
/* mem_warmup_check */

/* Every valid line of src that is also at dst, other than an icache line whose lower copy
   is dirty (warm_promote keeps those). */
static uns count_shared_lines(Cache* src, Cache* dst, Flag src_is_icache) {
  uns shared = 0;
  for (uns set = 0; set < src->num_sets; set++) {
    for (uns way = 0; way < src->assoc; way++) {
      Cache_Entry* e = &src->entries[set][way];
      if (!e->valid)
        continue;
      Addr line_addr;
      L1_Data* lower = (L1_Data*)cache_access(dst, e->base, &line_addr, FALSE);
      if (lower && !(src_is_icache && lower->dirty))
        shared++;
    }
  }
  return shared;
}

void mem_warmup_check(void) {
  if (!EXCLUSIVE_CACHES || !EXCLUSIVE_PROMOTE_INVALIDATE)
    return;
  for (uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    Cache* dcache = &cmp_model.dcache_stage[proc_id].dcache;
    Cache* icache = &cmp_model.icache_stage[proc_id].icache;
    Cache* llc = warm_cache(proc_id, WARM_LLC);
    uns shared = count_shared_lines(dcache, llc, FALSE) + count_shared_lines(icache, llc, TRUE);
    if (MLC_PRESENT) {
      Cache* mlc = warm_cache(proc_id, WARM_MLC);
      shared += count_shared_lines(dcache, mlc, FALSE) + count_shared_lines(icache, mlc, TRUE) +
                count_shared_lines(mlc, llc, FALSE);
    }
    ASSERTM(proc_id, shared == 0, "Exclusive hierarchy: %u lines at two levels after warmup\n", shared);
  }
}
