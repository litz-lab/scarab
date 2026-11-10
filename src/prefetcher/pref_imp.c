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
 * File         : prefetcher/pref_imp.c
 * Description  : Indirect Memory Prefetcher (Yu et al., MICRO 2015), A[B[i]] only.
 *
 *   ADDR(A[B[i]]) = (B[i] << shift) + base                          (paper Eq. 2)
 *
 *   1. Stream: a load PC that reads B one element at a time (PREF_IMP_STREAM_THRESH
 *      consecutive +size accesses) is an index stream. One PT entry per load PC.
 *   2. IPD: record two index values B[i], B[i+1] and the L1 misses (from any PC) that
 *      follow each. On the next index access, a base = miss - (B[k] << shift) shared by
 *      both (same shift) is the pattern; on failure, retry with the next two values.
 *   3. Prefetch: on each index access B[i], prefetch (B[i+dist] << shift) + base.
 *
 *   Index values: Scarab's caches hold no data, so values are read from Pin (pin_exec
 *   only), but B[i+dist] is used only once its line is in the L1, as in hardware.
 *   IMP never fetches index lines: other prefetchers (e.g., stream) or demand accesses
 *   bring them. Until then the A prefetch waits in a small pending table that is retried
 *   on every L1 access. A prefetch that does not fit in the queue (at most
 *   PREF_INSERT_PER_CYCLE per cycle) stays pending and is retried on a later access.
 *
 *   Simplified for an out-of-order core (the paper assumes in-order): a miss is paired
 *   with every recorded index value, since misses may arrive out of program order.
 * 
 *   Confidence (paper: hit cnt): an L1 access to base + (B[k] << shift) for one of the
 *   last IMP_CONF_WINDOW index values counts up; an index value pushed out without it
 *   counts down. IMP prefetches only at hit_cnt >= IMP_CONF_THRESH. The window (instead
 *   of the paper's single index) is needed out of order, where later index loads run
 *   ahead of A[B[i]].
 *   Differences from the paper: fixed prefetch distance, confidence window, no IPD
 *   back-off, and every load PC (not only streams) allocates a PT entry.
 ***************************************************************************************/

#include "prefetcher/pref_imp.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"

#include "general.param.h"
#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"

#include "frontend/frontend_intf.h"
#include "frontend/pin_exec_driven_fe.h"
#include "statistics.h"

#include "cmp_model.h"
#include "libs/cache_lib.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_PREF, ##args)

#define IMP_NSHIFT 3      // paper: {2, 3, 4, -3}; -3 (bit vectors) not modeled
#define IMP_IPD_MISSES 8  // misses recorded per index value
#define IMP_PENDING 64

#define IMP_CONF_WINDOW 8  // recent index values awaiting their A[B[i]] (OoO: may arrive late)
#define IMP_CONF_THRESH 4  // confirmations needed before prefetching

static const uns imp_shift[IMP_NSHIFT] = {2, 3, 4};

typedef struct Imp_Entry_struct {
  Addr pc;
  Counter last_use;  // LRU
  Addr last_va;  // stream
  uns stream_hits;
  Flag found;  // pattern
  uns shift;
  Addr base;
  uns nidx;  // IPD: index values B[i], B[i+1] and the misses after each
  uns64 idx[2];
  uns nmiss[2];
  Addr miss[2][IMP_IPD_MISSES];
  uns hit_cnt;                        // confidence (paper: hit cnt)
  uns64 recent[IMP_CONF_WINDOW];      // recent B[i] values
  Flag wait[IMP_CONF_WINDOW];         // still waiting for their A[B[i]]
  uns next;
} Imp_Entry;

static Imp_Entry* imp_pt;  // Prefetch Table, fully associative with LRU (paper: 16 entries); NULL when IMP is off
static HWP_Info* imp_hwp_info;
static HWP_Type imp_dest;


typedef struct { 
  Addr idx_va;
  uns size, shift;
  Addr base;
} Imp_Pending;  // idx_va == 0: empty

static Imp_Pending imp_pend[IMP_PENDING];  // A prefetches waiting for their index line
static uns imp_pend_next;

/* Read B[i+dist] from its (present) index line and prefetch A[B[i+dist]].
 * Returns FALSE only if the prefetch queue is full (retry later). */
static Flag imp_issue(uns8 proc_id, Imp_Pending p) {
  const Addr target = p.base + (pin_exec_driven_read_mem(proc_id, p.idx_va, p.size) << p.shift);

  if (target != convert_to_cmp_addr(proc_id, target & N_BIT_MASK(NUM_ADDR_NON_SIGN_EXTEND_BITS)))
    return TRUE;  // bad target: drop it

  if (!pref_addto_dest_req_queue(proc_id, imp_dest, target >> LOG2(DCACHE_LINE_SIZE), imp_hwp_info->id))
    return FALSE;

  STAT_EVENT(proc_id, PREF_IMP_PREF_ISSUED);
  return TRUE;
}

static Flag imp_idx_line_ready(uns8 proc_id, Addr idx_va) {
  Addr line;
  return cache_access(&cmp_model.dcache_stage[proc_id].dcache, idx_va, &line, FALSE) != NULL;
}

/* Issue waiting A prefetches whose index line has reached the L1, until the queue is full */
static void imp_retry_pending(uns8 proc_id) {
  for (uns ii = 0; ii < IMP_PENDING; ii++) {
    Imp_Pending* p = &imp_pend[ii];

    if (!p->idx_va || !imp_idx_line_ready(proc_id, p->idx_va))
      continue;

    if (!imp_issue(proc_id, *p))
      return;  // queue full this cycle: keep the rest for the next access

    p->idx_va = 0;
  }
}

void pref_imp_init(HWP* hwp) {
  if (!pref_hwp_instance_enabled(hwp, PREF_TRAIN_LEVEL_DCACHE))
    return;

  ASSERTM(0, FRONTEND == FE_PIN_EXEC_DRIVEN, "IMP reads index values from Pin: pin_exec only\n");

  imp_hwp_info = hwp->hwp_info;
  imp_dest = pref_hwp_instance_dest(hwp, PREF_TRAIN_LEVEL_DCACHE);
  imp_pt = (Imp_Entry*)calloc(PREF_IMP_PT_ENTRIES, sizeof(Imp_Entry));
  hwp->hwp_info->enabled = TRUE;
}

/* Base candidate from a miss address and an index value: base = addr - (idx << shift) */
static inline Addr imp_base_of(Addr miss_addr, uns64 idx, uns shift) {
  return miss_addr - (idx << shift);
}

/* IPD: a pattern is found when a miss after idx[0] and a miss after idx[1]
 * are explained by the same (shift, base). */
static Flag imp_ipd_match(Imp_Entry* e) {
  if (e->idx[0] == e->idx[1]) 
    return FALSE;  // identical indices match under every shift -> meaningless

  for (uns s = 0; s < IMP_NSHIFT; s++) {
    const uns shift = imp_shift[s];

    for (uns a = 0; a < e->nmiss[0]; a++) {
      const Addr base0 = imp_base_of(e->miss[0][a], e->idx[0], shift);

      for (uns b = 0; b < e->nmiss[1]; b++) {
        const Addr base1 = imp_base_of(e->miss[1][b], e->idx[1], shift);

        if (base0 != base1) 
          continue;

        e->shift = shift;
        e->base  = base0;
        return TRUE;
      }
    }
  }
  return FALSE;
}

void pref_imp_dl0_access(uns8 proc_id, Addr va, uns size, Addr load_PC, Flag hit) {
  if (!imp_pt)
    return;

  imp_retry_pending(proc_id);  // issue waiting A prefetches whose index line has arrived

  /* IPD: a miss may be A[B[k]] for any index value recorded so far */
  if (!hit) {
    for (uns ii = 0; ii < PREF_IMP_PT_ENTRIES; ii++) {
      Imp_Entry* e = &imp_pt[ii];

      for (uns k = 0; k < e->nidx; k++) {
        if (e->nmiss[k] >= IMP_IPD_MISSES)
          continue;

        e->miss[k][e->nmiss[k]] = va;
        e->nmiss[k]++;
      }
    }
  }

  /* Confidence: an access to A[B[k]] for a recent index value confirms the pattern */
  for (uns ii = 0; ii < PREF_IMP_PT_ENTRIES; ii++) {
    for (uns k = 0; imp_pt[ii].found && k < IMP_CONF_WINDOW; k++) {
      if (imp_pt[ii].wait[k] && va == imp_pt[ii].base + (imp_pt[ii].recent[k] << imp_pt[ii].shift)) {
        imp_pt[ii].wait[k] = FALSE;
        imp_pt[ii].hit_cnt += imp_pt[ii].hit_cnt < 7;
      }
    }
  }

  /* PT lookup: fully associative, LRU replacement */
  Imp_Entry* e = NULL;
  Imp_Entry* lru = &imp_pt[0];

  for (uns ii = 0; ii < PREF_IMP_PT_ENTRIES && !e; ii++) {
    if (imp_pt[ii].pc == load_PC)
      e = &imp_pt[ii];
    else if (imp_pt[ii].last_use < lru->last_use)
      lru = &imp_pt[ii];
  }

  /* replace according to LRU replacement*/
  if (!e) {
    e = lru;
    memset(e, 0, sizeof(*e));
    e->pc = load_PC;
  }

  e->last_use = cycle_count;

  /* 1. stream: same PC, next element */
  e->stream_hits = (va == e->last_va + size) ? e->stream_hits + 1 : 0;
  e->last_va = va;

  if ((!e->found && e->stream_hits < PREF_IMP_STREAM_THRESH) || size > 8)
    return;  // the stream check is only needed to find the pattern

  /* 2. IPD: record B[i], B[i+1]; on the next index access, match */
  if (!e->found) {
    if (e->nidx < 2) {
      e->idx[e->nidx++] = pin_exec_driven_read_mem(proc_id, va, size);
      return;
    }

    e->found = imp_ipd_match(e);
    if (e->found)
      fprintf(stderr, "IMP found pc:0x%llx shift:%u base:0x%llx\n", e->pc, e->shift, e->base);

    STAT_EVENT(proc_id, e->found ? PREF_IMP_PATTERN_FOUND : PREF_IMP_PATTERN_FAILED);
    DEBUG(proc_id, "pc:0x%llx pattern %s shift:%u base:0x%llx\n", e->pc, e->found ? "found" : "failed", e->shift, e->base);

    e->nidx = e->nmiss[0] = e->nmiss[1] = 0;  // on failure, retry with the next two index values

    return;
  }

  /* Confidence: record B[i]; the value it replaces never saw its A[B[i]] -> count down */
  const uns k = e->next++ % IMP_CONF_WINDOW;
  e->hit_cnt -= e->wait[k] && e->hit_cnt > 0;
  e->recent[k] = pin_exec_driven_read_mem(proc_id, va, size);
  e->wait[k] = TRUE;

  if (e->hit_cnt < IMP_CONF_THRESH)
    return;  // pattern not confirmed: train only

  /* 3. prefetch A[B[i + dist]] once B[i + dist] is in the L1 */
  const Imp_Pending p = {va + PREF_IMP_DIST * size, size, e->shift, e->base};
  const Flag line_ready    = imp_idx_line_ready(proc_id, p.idx_va);

  /* Index line in the L1 -> read B[i+dist] and prefetch now */
  if (line_ready) {
    const Flag queued = imp_issue(proc_id, p);
    if (queued)
      return;
  }

  /* Not issued yet (line missing or queue full) -> park it; imp_retry_pending() retries
   * once the index line is in the L1. IMP never fetches index lines itself. */
  imp_pend[imp_pend_next++ % IMP_PENDING] = p;
}
