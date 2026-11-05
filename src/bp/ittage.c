/*
 * Copyright 2026 University of California Santa Cruz
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
 * File         : bp/ittage.c
 * Author       : Litz Lab
 * Description  : ITTAGE indirect branch target predictor (--ibtb_mech ittage),
 *                written from the description in A. Seznec, "A 64-Kbytes ITTAGE
 *                indirect branch predictor", JWAC-2 / CBP-3, 2011.
 *
 * Structure
 *   T0        tagless base table indexed by PC: {target, 2-bit confidence}
 *   T1..TN    tagged tables indexed by hash(PC, global history of length L(i),
 *             path history), L(i) geometric between ITTAGE_MIN_HIST and
 *             ITTAGE_MAX_HIST: {target, partial tag, 2-bit confidence, 1-bit useful}
 *   regions   targets are stored as {region pointer, low offset bits}; the
 *             region table holds the high target bits (as in the paper, this is
 *             what makes 64 KB feasible with 64-bit targets)
 *
 * Prediction: the longest matching tagged table provides the target; the next
 * matching table (or T0) is the alternate. A freshly allocated (confidence 0)
 * provider defers to the alternate when the USE_ALT_ON_NA counter says the
 * alternate is more often right in that situation.
 *
 * Update (at target resolution): confidence/target hysteresis on the provider,
 * useful bit set when the provider was right and the alternate wrong, cleared
 * when the reverse; on a misprediction a new entry is allocated in a longer
 * table whose useful bit is 0. Useful bits are cleared periodically and also
 * when allocations keep failing (TICK counter).
 *
 * History: one bit per conditional branch (direction) plus ITTAGE_TARG_HIST_BITS
 * bits of target per call / indirect branch / indirect call, kept in a circular
 * buffer with folded (compressed) copies per table. It is updated speculatively
 * at prediction time with the predicted outcome and checkpointed in each op's
 * Recovery_Info; on a recovery the checkpoint of the mispredicted op is
 * restored and that op's correct outcome is re-applied, which is the same
 * repair contract the other mechanisms follow for targ_hist/global_hist.
 *
 * Default storage (8 tagged tables, see bp.param.def):
 *   tagged: 8 x 2^10 x (7 region ptr + 20 offset + 16 tag + 2 conf + 1 u = 46 b) = 376,832 b
 *   base:       2^12 x (7 region ptr + 20 offset + 2 conf              = 29 b) = 118,784 b
 *   regions:     128 x (64 - 20 = 44 high target bits)                        =   5,632 b
 *   history + folded registers + counters                                    ~   1,000 b
 *   total ~ 502,248 bits = 62,781 bytes (~61.3 KiB). The exact figure for the
 *   configured parameters is printed at start-up.
 *   (Entry valid flags are simulation bookkeeping, equivalent to reset state.)
 ***************************************************************************************/

#include "bp/ittage.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "bp/bp.param.h"

#include "bp/bp.h"
#include "bp/bp_targ_mech.h"

#include "statistics.h"

/**************************************************************************************/
/* Constants */

#define ITTAGE_HIST_BUF_BITS 8192 /* circular history buffer, must be a power of 2 */
#define ITTAGE_HIST_BUF_MASK (ITTAGE_HIST_BUF_BITS - 1)
#define ITTAGE_CONF_MAX 3
#define ITTAGE_USE_ALT_MAX 15
#define ITTAGE_USE_ALT_INIT 8
#define ITTAGE_TICK_MAX 1023
#define ITTAGE_PATH_BITS 16

/**************************************************************************************/
/* Types */

typedef struct Ittage_Entry_struct {
  uns32 offset;  // low ITTAGE_OFFSET_BITS of the target
  uns16 region;  // pointer into the region table
  uns16 tag;
  uns8 conf;
  uns8 u;
  Flag valid;
} Ittage_Entry;

typedef struct Ittage_Region_struct {
  Addr high;  // target >> ITTAGE_OFFSET_BITS
  Counter last_use;
  Flag valid;
} Ittage_Region;

/* Shared tables (allocated by the primary BP, shared by the others) */
typedef struct Ittage_struct {
  uns num_tables;
  uns log_size;
  uns log_base;
  uns tag_bits;
  uns hist_len[ITTAGE_MAX_TABLES + 1];
  uns fold_len[ITTAGE_MAX_TABLES + 1][3];

  Ittage_Entry* base;
  Ittage_Entry* table[ITTAGE_MAX_TABLES + 1];

  Ittage_Region* regions;
  Counter region_clock;

  uns use_alt;  // 4-bit USE_ALT_ON_NA counter
  uns tick;     // allocation failure counter
  Counter num_updates;
  uns32 lfsr;
} Ittage;

/* Per-BP speculative history */
typedef struct Ittage_Hist_struct {
  uns8 buf[ITTAGE_HIST_BUF_BITS];
  Ittage_Ckpt st;
} Ittage_Hist;

/**************************************************************************************/
/* History maintenance */

static inline uns16 fold_push(uns16 comp, uns clen, uns olen, uns8 new_bit, uns8 old_bit) {
  uns32 c = ((uns32)comp << 1) | new_bit;
  c ^= (uns32)old_bit << (olen % clen);
  c ^= c >> clen;
  return (uns16)(c & N_BIT_MASK(clen));
}

static void hist_push_bit(const Ittage* it, Ittage_Hist* h, uns8 bit) {
  const uns32 pos = h->st.head;
  h->buf[pos & ITTAGE_HIST_BUF_MASK] = bit;
  for (uns i = 1; i <= it->num_tables; i++) {
    const uns len = it->hist_len[i];
    const uns8 old_bit = h->buf[(pos - len) & ITTAGE_HIST_BUF_MASK];
    for (uns k = 0; k < 3; k++)
      h->st.fold[i - 1][k] = fold_push(h->st.fold[i - 1][k], it->fold_len[i][k], len, bit, old_bit);
  }
  h->st.head = pos + 1;
}

/* Insert one control-flow op into the history. dir is used for conditional
 * branches, target for calls and indirect branches. Other types do not
 * change the history. */
static void hist_update(const Ittage* it, Ittage_Hist* h, Cf_Type cf_type, Addr pc, Flag dir, Addr target) {
  switch (cf_type) {
    case CF_CBR:
    case CF_REP:
      hist_push_bit(it, h, dir ? 1 : 0);
      break;
    case CF_CALL:
    case CF_ICALL:
    case CF_IBR: {
      const Addr t = target ^ (target >> 4) ^ (target >> 9);
      for (uns b = 0; b < ITTAGE_TARG_HIST_BITS; b++)
        hist_push_bit(it, h, (t >> b) & 1);
      break;
    }
    default:
      return;
  }
  h->st.path = (uns16)((h->st.path << 1) | ((pc ^ (pc >> 4)) & 1));
}

/**************************************************************************************/
/* Target compression through the region table */

static inline Addr decode_target(const Ittage* it, const Ittage_Entry* e) {
  const Ittage_Region* r = &it->regions[e->region];
  if (!e->valid || !r->valid)
    return 0;
  return (r->high << ITTAGE_OFFSET_BITS) | e->offset;
}

static uns16 find_or_alloc_region(Ittage* it, Addr target, uns proc_id) {
  const Addr high = target >> ITTAGE_OFFSET_BITS;
  int free_slot = -1;
  uns lru = 0;
  it->region_clock++;
  for (uns r = 0; r < ITTAGE_REGION_ENTRIES; r++) {
    if (!it->regions[r].valid) {
      if (free_slot < 0)
        free_slot = r;
      continue;
    }
    if (it->regions[r].high == high) {
      it->regions[r].last_use = it->region_clock;
      return (uns16)r;
    }
    if (it->regions[r].last_use < it->regions[lru].last_use || !it->regions[lru].valid)
      lru = r;
  }
  const uns victim = free_slot >= 0 ? (uns)free_slot : lru;
  if (it->regions[victim].valid)
    STAT_EVENT(proc_id, ITTAGE_REGION_REPLACE);
  it->regions[victim].valid = TRUE;
  it->regions[victim].high = high;
  it->regions[victim].last_use = it->region_clock;
  return (uns16)victim;
}

static void set_target(Ittage* it, Ittage_Entry* e, Addr target, uns proc_id) {
  e->region = find_or_alloc_region(it, target, proc_id);
  e->offset = (uns32)(target & N_BIT_MASK(ITTAGE_OFFSET_BITS));
}

/**************************************************************************************/
/* Index and tag hashing */

static inline uns32 pc_hash(Addr pc) {
  return (uns32)(pc ^ (pc >> 21) ^ (pc >> 42));
}

static inline uns32 base_index(const Ittage* it, Addr pc) {
  const uns32 p = pc_hash(pc);
  return (p ^ (p >> it->log_base)) & N_BIT_MASK(it->log_base);
}

static inline uns32 tagged_index(const Ittage* it, const Ittage_Hist* h, Addr pc, uns i) {
  const uns ls = it->log_size;
  const uns32 p = pc_hash(pc);
  const uns plen = MIN2(it->hist_len[i], ITTAGE_PATH_BITS);
  uns32 path = h->st.path & N_BIT_MASK(plen);
  // fold the path into ls bits and rotate by the table number so that the
  // same path does not land on the same index in every table
  path = (path & N_BIT_MASK(ls)) ^ (path >> ls);
  const uns rot = i % ls;
  path = ((path << rot) | (path >> (ls - rot))) & N_BIT_MASK(ls);
  const uns shift = (ls > i ? ls - i : 1);
  return (p ^ (p >> shift) ^ h->st.fold[i - 1][0] ^ path) & N_BIT_MASK(ls);
}

static inline uns16 tagged_tag(const Ittage* it, const Ittage_Hist* h, Addr pc, uns i) {
  const uns32 p = pc_hash(pc);
  return (uns16)((p ^ h->st.fold[i - 1][1] ^ ((uns32)h->st.fold[i - 1][2] << 1)) & N_BIT_MASK(it->tag_bits));
}

static inline uns32 lfsr_next(Ittage* it) {
  // 32-bit Galois LFSR; used only to spread allocations between tables
  it->lfsr = (it->lfsr >> 1) ^ (-(it->lfsr & 1u) & 0xD0000001u);
  return it->lfsr;
}

/**************************************************************************************/
/* Storage accounting */

static uns64 ittage_storage_bits(const Ittage* it) {
  const uns ptr_bits = LOG2(ITTAGE_REGION_ENTRIES);
  const uns64 tagged_entry = ptr_bits + ITTAGE_OFFSET_BITS + it->tag_bits + 2 + 1;
  const uns64 base_entry = ptr_bits + ITTAGE_OFFSET_BITS + 2;
  uns64 bits = 0;
  bits += (uns64)it->num_tables * (1ull << it->log_size) * tagged_entry;
  bits += (1ull << it->log_base) * base_entry;
  bits += (uns64)ITTAGE_REGION_ENTRIES * (64 - ITTAGE_OFFSET_BITS);
  bits += it->hist_len[it->num_tables] + ITTAGE_PATH_BITS;  // global + path history
  for (uns i = 1; i <= it->num_tables; i++)
    bits += it->fold_len[i][0] + it->fold_len[i][1] + it->fold_len[i][2];
  bits += 4 + 10 + ITTAGE_U_RESET_LOG_PERIOD;  // USE_ALT_ON_NA, TICK, reset counter
  return bits;
}

/**************************************************************************************/
/* bp_ibtb_ittage_init */

void bp_ibtb_ittage_init(Bp_Data* bp_data, Bp_Data* primary_bp) {
  bp_data->ittage_hist = (Ittage_Hist*)calloc(1, sizeof(Ittage_Hist));

  if (bp_data->bp_id) {
    bp_data->ittage = primary_bp->ittage;
    return;
  }

  ASSERTM(bp_data->proc_id, ITTAGE_NUM_TABLES >= 1 && ITTAGE_NUM_TABLES <= ITTAGE_MAX_TABLES,
          "ITTAGE_NUM_TABLES must be in [1, %d]\n", ITTAGE_MAX_TABLES);
  ASSERTM(bp_data->proc_id, ITTAGE_LOG_TABLE_SIZE >= 2 && ITTAGE_LOG_TABLE_SIZE <= 16,
          "ITTAGE_LOG_TABLE_SIZE must be in [2, 16]\n");
  ASSERTM(bp_data->proc_id, ITTAGE_LOG_BASE_SIZE >= 2 && ITTAGE_LOG_BASE_SIZE <= 24,
          "ITTAGE_LOG_BASE_SIZE must be in [2, 24]\n");
  ASSERTM(bp_data->proc_id, ITTAGE_TAG_BITS >= 2 && ITTAGE_TAG_BITS <= 16, "ITTAGE_TAG_BITS must be in [2, 16]\n");
  ASSERTM(bp_data->proc_id, ITTAGE_MIN_HIST >= 1 && ITTAGE_MIN_HIST <= ITTAGE_MAX_HIST,
          "need 1 <= ITTAGE_MIN_HIST <= ITTAGE_MAX_HIST\n");
  ASSERTM(bp_data->proc_id, ITTAGE_MAX_HIST <= ITTAGE_HIST_BUF_BITS / 2, "ITTAGE_MAX_HIST must be <= %d\n",
          ITTAGE_HIST_BUF_BITS / 2);
  ASSERTM(bp_data->proc_id, ITTAGE_REGION_ENTRIES >= 1 && ITTAGE_REGION_ENTRIES <= 65536,
          "ITTAGE_REGION_ENTRIES must be in [1, 65536]\n");
  ASSERTM(bp_data->proc_id, ITTAGE_OFFSET_BITS >= 2 && ITTAGE_OFFSET_BITS <= 32,
          "ITTAGE_OFFSET_BITS must be in [2, 32]\n");

  Ittage* it = (Ittage*)calloc(1, sizeof(Ittage));
  it->num_tables = ITTAGE_NUM_TABLES;
  it->log_size = ITTAGE_LOG_TABLE_SIZE;
  it->log_base = ITTAGE_LOG_BASE_SIZE;
  it->tag_bits = ITTAGE_TAG_BITS;

  // geometric history lengths L(1) = min ... L(N) = max
  for (uns i = 1; i <= it->num_tables; i++) {
    if (it->num_tables == 1) {
      it->hist_len[i] = ITTAGE_MAX_HIST;
    } else {
      const double ratio =
          pow((double)ITTAGE_MAX_HIST / (double)ITTAGE_MIN_HIST, (double)(i - 1) / (double)(it->num_tables - 1));
      it->hist_len[i] = (uns)(ITTAGE_MIN_HIST * ratio + 0.5);
    }
    if (i > 1 && it->hist_len[i] <= it->hist_len[i - 1])
      it->hist_len[i] = it->hist_len[i - 1] + 1;
    it->fold_len[i][0] = it->log_size;
    it->fold_len[i][1] = it->tag_bits;
    it->fold_len[i][2] = it->tag_bits - 1;
  }

  it->base = (Ittage_Entry*)calloc(1ull << it->log_base, sizeof(Ittage_Entry));
  for (uns i = 1; i <= it->num_tables; i++)
    it->table[i] = (Ittage_Entry*)calloc(1ull << it->log_size, sizeof(Ittage_Entry));
  it->regions = (Ittage_Region*)calloc(ITTAGE_REGION_ENTRIES, sizeof(Ittage_Region));
  it->use_alt = ITTAGE_USE_ALT_INIT;
  it->lfsr = 0x2545F491u;

  bp_data->ittage = it;

  const uns64 bits = ittage_storage_bits(it);
  printf("ITTAGE: %u tagged tables x 2^%u entries, base 2^%u, %u-bit tags, %u regions x %u-bit offsets, hist",
         it->num_tables, it->log_size, it->log_base, it->tag_bits, (uns)ITTAGE_REGION_ENTRIES, (uns)ITTAGE_OFFSET_BITS);
  for (uns i = 1; i <= it->num_tables; i++)
    printf(" %u", it->hist_len[i]);
  printf("; storage %llu bits = %.1f KiB\n", (unsigned long long)bits, bits / 8.0 / 1024.0);
}

/**************************************************************************************/
/* bp_ibtb_ittage_pred: returns the predicted target, 0 = no prediction */

Addr bp_ibtb_ittage_pred(Bp_Data* bp_data, Op* op) {
  if (PERFECT_IBP)
    return op->oracle_info.target;

  const Ittage* it = bp_data->ittage;
  const Ittage_Hist* h = bp_data->ittage_hist;
  const Addr pc = op->inst->addr;
  Ittage_Pred_Info* pi = &op->btb_pred_info->ittage;

  pi->provider = -1;
  pi->alt = -1;
  pi->provider_weak = FALSE;
  pi->used_alt = FALSE;
  pi->provider_target = 0;
  pi->alt_target = 0;
  pi->pred_target = 0;

  pi->index[0] = base_index(it, pc);
  pi->tag[0] = 0;
  for (uns i = 1; i <= it->num_tables; i++) {
    pi->index[i] = tagged_index(it, h, pc, i);
    pi->tag[i] = tagged_tag(it, h, pc, i);
  }

  for (int i = it->num_tables; i >= 1; i--) {
    const Ittage_Entry* e = &it->table[i][pi->index[i]];
    if (e->valid && e->tag == pi->tag[i]) {
      if (pi->provider < 0) {
        pi->provider = i;
      } else {
        pi->alt = i;
        break;
      }
    }
  }

  const Ittage_Entry* be = &it->base[pi->index[0]];
  const Addr base_target = decode_target(it, be);
  if (pi->provider < 0) {
    if (base_target) {
      pi->provider = 0;
      pi->provider_target = base_target;
    }
  } else {
    const Ittage_Entry* pe = &it->table[pi->provider][pi->index[pi->provider]];
    pi->provider_target = decode_target(it, pe);
    pi->provider_weak = pe->conf == 0;
    if (pi->alt > 0) {
      pi->alt_target = decode_target(it, &it->table[pi->alt][pi->index[pi->alt]]);
    } else if (base_target) {
      pi->alt = 0;
      pi->alt_target = base_target;
    }
  }

  if (pi->provider_weak && pi->alt >= 0 && pi->alt_target && it->use_alt >= ITTAGE_USE_ALT_INIT) {
    pi->used_alt = TRUE;
    pi->pred_target = pi->alt_target;
  } else {
    pi->pred_target = pi->provider_target;
  }

  if (ITTAGE_BASE_BTB && pi->provider == 0 && !pi->used_alt)
    pi->pred_target = 0;

  if (!op->off_path) {
    if (pi->provider < 0)
      STAT_EVENT(op->proc_id, ITTAGE_NO_PRED);
    else
      STAT_EVENT(op->proc_id, ITTAGE_PROVIDER_BASE + pi->provider);
    if (pi->used_alt)
      STAT_EVENT(op->proc_id, ITTAGE_USED_ALT);
    if (pi->pred_target)
      STAT_EVENT(op->proc_id, pi->pred_target == op->oracle_info.target ? ITTAGE_CORRECT : ITTAGE_INCORRECT);
  }

  return pi->pred_target;
}

/**************************************************************************************/
/* Entry training helpers */

/* confidence / target hysteresis: replace the target only when confidence is 0 */
static void train_entry(Ittage* it, Ittage_Entry* e, Addr correct, uns proc_id) {
  if (decode_target(it, e) == correct) {
    if (e->conf < ITTAGE_CONF_MAX)
      e->conf++;
  } else if (e->conf > 0) {
    e->conf--;
  } else {
    set_target(it, e, correct, proc_id);
  }
}

static void reset_useful(Ittage* it) {
  for (uns i = 1; i <= it->num_tables; i++)
    for (uns32 j = 0; j < (1u << it->log_size); j++)
      it->table[i][j].u = 0;
}

/**************************************************************************************/
/* bp_ibtb_ittage_update: called when the target is known (see bp_target_known_op) */

void bp_ibtb_ittage_update(Bp_Data* bp_data, Op* op) {
  Ittage* it = bp_data->ittage;
  const Ittage_Pred_Info* pi = &op->btb_pred_info->ittage;
  const Addr correct = op->oracle_info.target;
  const uns proc_id = op->proc_id;

  ASSERT(bp_data->proc_id, !bp_data->bp_id);
  if (PERFECT_IBP || correct == 0 || correct == ADDR_INVALID)
    return;

  const Flag mispred = pi->pred_target != correct;

  // USE_ALT_ON_NA: learn whether a weak provider or its alternate is more reliable
  if (pi->provider > 0 && pi->provider_weak && pi->alt >= 0 && pi->alt_target != pi->provider_target) {
    if (pi->alt_target == correct) {
      if (it->use_alt < ITTAGE_USE_ALT_MAX)
        it->use_alt++;
    } else if (pi->provider_target == correct) {
      if (it->use_alt > 0)
        it->use_alt--;
    }
  }

  // allocate a new entry in a longer table on a misprediction
  if (mispred && pi->provider < (int)it->num_tables) {
    const uns start = pi->provider < 1 ? 1 : pi->provider + 1;
    uns cand[ITTAGE_MAX_TABLES];
    uns num_cand = 0;
    for (uns i = start; i <= it->num_tables; i++)
      if (it->table[i][pi->index[i]].u == 0)
        cand[num_cand++] = i;
    if (num_cand) {
      // mostly the shortest free table, sometimes the next one (spreads allocations)
      const uns pick = (num_cand > 1 && (lfsr_next(it) % 3) == 0) ? 1 : 0;
      const uns i = cand[pick];
      Ittage_Entry* e = &it->table[i][pi->index[i]];
      e->valid = TRUE;
      e->tag = pi->tag[i];
      e->conf = 0;
      e->u = 0;
      set_target(it, e, correct, proc_id);
      if (it->tick > 0)
        it->tick--;
      STAT_EVENT(proc_id, ITTAGE_ALLOC);
    } else {
      STAT_EVENT(proc_id, ITTAGE_ALLOC_FAIL);
      if (++it->tick >= ITTAGE_TICK_MAX) {
        reset_useful(it);
        it->tick = 0;
        STAT_EVENT(proc_id, ITTAGE_U_RESET);
      }
    }
  }

  // train the provider (and the alternate when the provider was not yet confident)
  if (pi->provider > 0) {
    Ittage_Entry* pe = &it->table[pi->provider][pi->index[pi->provider]];
    if (pe->valid && pe->tag == pi->tag[pi->provider]) {
      train_entry(it, pe, correct, proc_id);
      if (pi->alt >= 0 && pi->provider_target != pi->alt_target) {
        if (pi->provider_target == correct)
          pe->u = 1;
        else if (pi->alt_target == correct)
          pe->u = 0;
      }
    }
    if (pi->provider_weak && pi->alt >= 0) {
      Ittage_Entry* ae = pi->alt > 0 ? &it->table[pi->alt][pi->index[pi->alt]] : &it->base[pi->index[0]];
      if (pi->alt == 0 || (ae->valid && ae->tag == pi->tag[pi->alt]))
        train_entry(it, ae, correct, proc_id);
    }
  } else {
    Ittage_Entry* be = &it->base[pi->index[0]];
    if (!be->valid) {
      be->valid = TRUE;
      be->conf = 0;
      set_target(it, be, correct, proc_id);
    } else {
      train_entry(it, be, correct, proc_id);
    }
  }

  // periodic useful reset
  it->num_updates++;
  if ((it->num_updates & N_BIT_MASK(ITTAGE_U_RESET_LOG_PERIOD)) == 0) {
    reset_useful(it);
    STAT_EVENT(proc_id, ITTAGE_U_RESET);
  }
}

/**************************************************************************************/
/* bp_ibtb_ittage_recover: restore the history checkpoint of the op that caused
 * the recovery and re-apply that op with its correct outcome. Called for every
 * recovery (not only indirect ones) because the history covers all branches. */

void bp_ibtb_ittage_recover(Bp_Data* bp_data, Recovery_Info* info) {
  Ittage_Hist* h = bp_data->ittage_hist;
  h->st = info->ittage_ckpt;
  hist_update(bp_data->ittage, h, info->cf_type, info->PC, info->new_dir, info->branchTarget);
}

/**************************************************************************************/
/* bp_ittage_checkpoint / bp_ittage_spec_update: called from bp_predict_op for
 * every control-flow op. The checkpoint is the pre-op state; the speculative
 * update inserts the op with its predicted outcome. */

void bp_ittage_checkpoint(Bp_Data* bp_data, Op* op) {
  op->recovery_info.ittage_ckpt = bp_data->ittage_hist->st;
}

void bp_ittage_spec_update(Bp_Data* bp_data, Op* op, const Bp_Pred_Info* bp_pred_info) {
  hist_update(bp_data->ittage, bp_data->ittage_hist, op->uop->cf_type, op->inst->addr, bp_pred_info->pred_orig == TAKEN,
              bp_pred_info->pred_npc);
}

/**************************************************************************************/
/* bp_ittage_sync: copy the speculative history from one BP to another */

void bp_ittage_sync(Bp_Data* src, Bp_Data* dst) {
  if (src->ittage_hist && dst->ittage_hist)
    memcpy(dst->ittage_hist, src->ittage_hist, sizeof(Ittage_Hist));
}
