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
 *   1. Stream: a load PC that reads B one element at a time is an index stream.
 *   2. IPD: take two index values B[i], B[i+1] and the misses that follow each.
 *      A base = miss - (B[k] << shift) shared by both (same shift) is the pattern.
 *   3. Prefetch: on each index access B[i], prefetch (B[i+dist] << shift) + base.
 *
 *   Simplified for an out-of-order core (the paper assumes in-order): misses are paired with
 *   every recorded index value, and the distance is fixed (no confidence counter).
 *   Not modeled: multi-way / multi-level indirection, partial cachelines, shift -3.
 *
 *   Index values are read from Pin (pin_exec only): in hardware they come from the index
 *   line in the L1, but Scarab's caches hold no data.
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

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_PREF, ##args)

#define IMP_NSHIFT 3      // paper: {2, 3, 4, -3}; -3 (bit vectors) not modeled
#define IMP_IPD_MISSES 8  // misses recorded per index value

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
} Imp_Entry;

static Imp_Entry* imp_pt;  // Prefetch Table, fully associative with LRU (paper: 16 entries); NULL when IMP is off
static HWP_Info* imp_hwp_info;
static HWP_Type imp_dest;

void pref_imp_init(HWP* hwp) {
  if (!pref_hwp_instance_enabled(hwp, PREF_TRAIN_LEVEL_DCACHE))
    return;
  ASSERTM(0, FRONTEND == FE_PIN_EXEC_DRIVEN, "IMP reads index values from Pin: pin_exec only\n");
  imp_hwp_info = hwp->hwp_info;
  imp_dest = pref_hwp_instance_dest(hwp, PREF_TRAIN_LEVEL_DCACHE);
  imp_pt = (Imp_Entry*)calloc(PREF_IMP_PT_ENTRIES, sizeof(Imp_Entry));
  hwp->hwp_info->enabled = TRUE;
}

/* IPD: a base shared by both index values (same shift) is the pattern. */
static Flag imp_ipd_match(Imp_Entry* e) {
  for (uns s = 0; s < IMP_NSHIFT; s++)
    for (uns a = 0; a < e->nmiss[0]; a++)
      for (uns b = 0; b < e->nmiss[1]; b++) {
        const Addr base = e->miss[0][a] - (e->idx[0] << imp_shift[s]);
        if (base == e->miss[1][b] - (e->idx[1] << imp_shift[s])) {
          e->shift = imp_shift[s];
          e->base = base;
          return TRUE;
        }
      }
  return FALSE;
}

void pref_imp_dl0_access(uns8 proc_id, Addr va, uns size, Addr load_PC, Flag hit) {
  if (!imp_pt)
    return;

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

    STAT_EVENT(proc_id, e->found ? PREF_IMP_PATTERN_FOUND : PREF_IMP_PATTERN_FAILED);
    DEBUG(proc_id, "pc:0x%llx pattern %s shift:%u base:0x%llx\n", e->pc, e->found ? "found" : "failed", e->shift, e->base);

    e->nidx = e->nmiss[0] = e->nmiss[1] = 0;  // on failure, retry with the next two index values

    return;
  }

  /* 3. prefetch A[B[i + dist]] */
  const uns64 idx = pin_exec_driven_read_mem(proc_id, va + PREF_IMP_DIST * size, size);
  const Addr target = e->base + (idx << e->shift);

  if (target != convert_to_cmp_addr(proc_id, target & N_BIT_MASK(NUM_ADDR_NON_SIGN_EXTEND_BITS)))
    return;

  if (pref_addto_dest_req_queue(proc_id, imp_dest, target >> LOG2(DCACHE_LINE_SIZE), imp_hwp_info->id))
    STAT_EVENT(proc_id, PREF_IMP_PREF_ISSUED);
}
