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
 * File         : pcprof.cc
 * Author       : Litz Lab
 * Description  : AsmDB-style per-PC attribution (--pc_profile 1). See pcprof.h.
 *
 * Everything is keyed by the static instruction address (op->inst->addr, the raw
 * trace PC). Counts are taken over the measured region only: from the cycle full_warmup
 * is reached (warmup_dump_done[0]) to the end of the simulation, for on-path retired ops
 * unless a column says otherwise.
 *
 * ROB-head attribution (CPI-stack style): every node cycle in which nothing retires is
 * charged to exactly one PC and one category.
 *   - ROB empty (or only off-path ops) after an on-path recovery and before the first
 *     correct-path op arrives: charged to the recovering branch as st_bmisp (recovery at
 *     exec) or st_resteer (recovery at decode / fe).
 *   - ROB empty otherwise: st_fe, charged to the next on-path op that enters the ROB.
 *   - Head op not ready: charged to the head op, category decided each cycle
 *     (branch that will recover at exec -> st_bmisp; memory op that accessed the dcache
 *     -> memory; otherwise st_exec). Memory cycles are split at retire by the level that
 *     finally served the op: st_l1d (dcache hit / store forward), st_mlc, st_llc,
 *     st_dram, st_mem_unk (dcache miss whose fill level was not observed).
 *   Invariant: cycles = retiring_cycles + sum(all st_* columns) + fe_unattributed.
 ***************************************************************************************/

#include "pcprof.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "bp/bp.param.h"
#include "core.param.h"
#include "general.param.h"

#include "bp/bp.h"
#include "memory/mem_req.h"

#include "node_stage.h"
#include "op.h"
#include "table_info.h"
}

namespace {

enum Col {
  C_INSTS,
  C_UOPS,
  C_TAKEN,
  C_MISP_EXEC,
  C_MISP_DIR,
  C_MISP_TGT,
  C_MISP_BTB_MISS,
  C_MISP_BIM,
  C_MISP_TAGE_PROV,
  C_MISP_TAGE_ALT,
  C_MISP_LOOP,
  C_MISP_SC,
  C_MISP_ITT_PROV,
  C_MISP_ITT_ALT,
  C_MISP_ITT_BASE,
  C_MISP_BTB_TGT,
  C_MISP_NO_TGT,
  C_MISP_RAS,
  C_MISP_OTHER,
  C_RECOV_DECODE,
  C_RECOV_FE,
  C_LOADS,
  C_STORES,
  C_L1D_MISS,
  C_MLC_MISS,
  C_LLC_MISS,
  C_LD_MISS_CYCLES,
  C_ST_L1D_MISS,
  C_FT_FETCH,
  C_FT_UC_MISS,
  C_FT_IC_MISS,
  C_ST_BMISP,
  C_ST_RESTEER,
  C_ST_L1D,
  C_ST_MLC,
  C_ST_LLC,
  C_ST_DRAM,
  C_ST_MEM_UNK,
  C_ST_EXEC,
  C_ST_FE,
  C_ST_OTHER,
  NUM_COLS
};

const char* col_names[NUM_COLS] = {
    "insts",         "uops",           "taken",         "misp_exec", "misp_dir",   "misp_tgt",       "misp_btb_miss",
    "misp_bim",      "misp_tage_prov", "misp_tage_alt", "misp_loop", "misp_sc",    "misp_itt_prov",  "misp_itt_alt",
    "misp_itt_base", "misp_btb_tgt",   "misp_no_tgt",   "misp_ras",  "misp_other", "recov_decode",   "recov_fe",
    "loads",         "stores",         "l1d_miss",      "mlc_miss",  "llc_miss",   "ld_miss_cycles", "st_l1d_miss",
    "ft_fetch",      "ft_uc_miss",     "ft_ic_miss",    "st_bmisp",  "st_resteer", "st_l1d",         "st_mlc",
    "st_llc",        "st_dram",        "st_mem_unk",    "st_exec",   "st_fe",      "st_other"};

// exec-recovery source (Op::pcprof_misp)
enum Misp_Src {
  MS_NONE,
  MS_BIM,
  MS_TAGE_PROV,
  MS_TAGE_ALT,
  MS_LOOP,
  MS_SC,
  MS_ITT_PROV,
  MS_ITT_ALT,
  MS_ITT_BASE,
  MS_BTB_TGT,
  MS_NO_TGT,
  MS_RAS,
  MS_OTHER
};

const char* cf_names[NUM_CF_TYPES] = {"", "jmp", "cbr", "rep", "call", "ijmp", "icall", "ico", "ret", "sys"};

struct PcRec {
  uns64 c[NUM_COLS] = {};
  uns8 cf = 0;
};

// Exact LRU stack distance (number of distinct other keys touched since the previous
// touch of this key) via a Fenwick tree over access timestamps.
class StackDist {
 public:
  static const int NB = 42;  // bucket 0: d=0, bucket b>=1: 2^(b-1) <= d < 2^b
  uns64 hist[NB] = {0};
  uns64 cold = 0;
  uns64 accesses = 0;

  StackDist() { grow(1u << 20); }

  void access(uns64 key) {
    uns64 t = ++now;
    accesses++;
    if (t >= cap)
      grow(cap * 2);
    auto it = last.find(key);
    if (it == last.end()) {
      cold++;
      last.emplace(key, t);
    } else {
      uns64 t0 = it->second;
      uns64 d = prefix(t - 1) - prefix(t0);
      hist[bucket(d)]++;
      add(t0, -1);
      mark[t0] = 0;
      it->second = t;
    }
    add(t, 1);
    mark[t] = 1;
  }
  size_t distinct() const { return last.size(); }

 private:
  std::unordered_map<uns64, uns64> last;
  std::vector<int32> bit;
  std::vector<uns8> mark;
  uns64 cap = 0;
  uns64 now = 0;

  static int bucket(uns64 d) {
    if (d == 0)
      return 0;
    int b = 64 - __builtin_clzll(d);
    return b < NB ? b : NB - 1;
  }
  void add(uns64 i, int v) {
    for (; i < cap; i += i & (~i + 1))
      bit[i] += v;
  }
  uns64 prefix(uns64 i) const {
    int64 s = 0;
    for (; i > 0; i -= i & (~i + 1))
      s += bit[i];
    return (uns64)s;
  }
  void grow(uns64 ncap) {
    mark.resize(ncap, 0);
    bit.assign(ncap, 0);
    cap = ncap;
    for (uns64 i = 1; i < cap; i++) {  // linear Fenwick build
      bit[i] += mark[i];
      uns64 j = i + (i & (~i + 1));
      if (j < cap)
        bit[j] += bit[i];
    }
  }
};

struct PairHash {
  size_t operator()(const std::pair<Addr, Addr>& p) const {
    return std::hash<uns64>()(p.first * 0x9E3779B97F4A7C15ull ^ p.second);
  }
};

struct PairHash64 {
  size_t operator()(const std::pair<Addr, int64>& p) const {
    return std::hash<uns64>()(p.first * 0x9E3779B97F4A7C15ull ^ (uns64)p.second);
  }
};

struct Prof {
  std::unordered_map<Addr, PcRec> pcs;

  // global counters (measured region)
  uns64 cycles = 0;
  uns64 retiring_cycles = 0;
  uns64 fe_unattributed = 0;  // pending st_fe cycles never claimed by an arriving op
  uns64 pending_fe = 0;

  Flag in_recovery = FALSE;
  Flag recovery_exec = FALSE;
  Addr recovery_pc = 0;

  uns retired_this_cycle = 0;
  Flag started = FALSE;
  uns64 start_cycle = 0;

  // footprints
  std::unordered_set<Addr> inst_pcs, icache_lines, icache_pages, data_pages, ind_pcs, ret_pcs;
  std::unordered_set<std::pair<Addr, Addr>, PairHash> ind_pairs;
  StackDist br_reuse;    // conditional branch PCs (cbr + rep), in conditional branches
  StackDist data_reuse;  // 64B data lines, in retired load/store uops
  uns64 cond_dyn = 0, ind_dyn = 0, mem_dyn = 0;

  // ---- phases ----
  // sys phase = number of the last syscall retired before the instruction (-1 = none seen)
  // mark phase = k for the k-th XCHG rbx,rbx begin marker until XCHG rdx,rdx (0 = unmarked)
  struct Ev {
    uns64 uid;
    int type;
    int64 value;
  };
  std::deque<Ev> events;
  int64 sys_phase = -1;
  int64 mark_phase = 0;
  int64 mark_count = 0;
  uns64 insts_since_sys = 0;
  std::unordered_map<std::pair<Addr, int64>, PcRec, PairHash64> sys_pcs, mark_pcs;
  struct PhaseTot {
    uns64 cycles = 0, retiring = 0, entries = 0;
  };
  std::unordered_map<int64, PhaseTot> sys_tot, mark_tot;
  PhaseTot *sys_cur = nullptr, *mark_cur = nullptr;  // current phases' entries; null after a phase change
  uns64 win_hist[64] = {0};  // log2 histogram of retired insts between consecutive syscalls
  uns64 win_count = 0, win_sum = 0;
  uns64 syscalls_in_region = 0;
  uns64 mark_begins_in_region = 0, mark_ends_in_region = 0;
  int64 sys_phase_at_start = -2, mark_phase_at_start = -2;
};

Prof* g = nullptr;

inline Flag measuring() {
  if (FULL_WARMUP && !warmup_dump_done[0])
    return FALSE;
  if (!g->started) {
    g->started = TRUE;
    g->start_cycle = cycle_count;
    g->sys_phase_at_start = g->sys_phase;
    g->mark_phase_at_start = g->mark_phase;
    g->sys_tot[g->sys_phase].entries++;  // the phase in progress counts as entered once
    g->mark_tot[g->mark_phase].entries++;
  }
  return TRUE;
}

// Apply frontend phase events up to and including this retiring instruction.
inline void apply_events(uns64 uid) {
  while (!g->events.empty() && g->events.front().uid <= uid) {
    const Prof::Ev e = g->events.front();
    g->events.pop_front();
    Flag m = measuring();
    if (e.type == PCPROF_EV_SYSCALL) {
      if (m) {
        g->syscalls_in_region++;
        uns64 w = g->insts_since_sys;
        g->win_hist[w ? 64 - __builtin_clzll(w) : 0]++;
        g->win_count++;
        g->win_sum += w;
      }
      g->sys_phase = e.value;
      g->sys_cur = nullptr;
      g->insts_since_sys = 0;
      if (m)
        g->sys_tot[g->sys_phase].entries++;
    } else if (e.type == PCPROF_EV_MARK_BEGIN) {
      g->mark_count++;
      g->mark_phase = g->mark_count;
      g->mark_cur = nullptr;
      if (m) {
        g->mark_begins_in_region++;
        g->mark_tot[g->mark_phase].entries++;
      }
    } else if (e.type == PCPROF_EV_MARK_END) {
      g->mark_phase = 0;
      g->mark_cur = nullptr;
      if (m) {
        g->mark_ends_in_region++;
        g->mark_tot[0].entries++;
      }
    }
  }
}

// A counter update goes to the whole-region table and to the current sys and mark phase tables.
struct R {
  PcRec* p[3];
  void add(int col, uns64 v = 1) {
    p[0]->c[col] += v;
    p[1]->c[col] += v;
    p[2]->c[col] += v;
  }
  void set_cf(uns8 cf) { p[0]->cf = p[1]->cf = p[2]->cf = cf; }
};

inline R recs(Addr pc) {
  R r = {};
  r.p[0] = &g->pcs[pc];
  r.p[1] = &g->sys_pcs[std::make_pair(pc, g->sys_phase)];
  r.p[2] = &g->mark_pcs[std::make_pair(pc, g->mark_phase)];
  return r;
}

inline void ensure() {
  if (!g)
    g = new Prof();
}

}  // namespace

/**************************************************************************************/

extern "C" void pcprof_fe_event(uns proc_id, uns64 inst_uid, int type, int64 value) {
  if (!PC_PROFILE || proc_id != 0)
    return;
  ensure();
  g->events.push_back(Prof::Ev{inst_uid, type, value});
}

extern "C" void pcprof_recovery(Op* op) {
  if (!PC_PROFILE || !op)
    return;
  ensure();
  Bp_Pred_Info* bpi = op->bp_pred_info;
  Flag at_exec = (bpi->recovery_point == RECOVER_AT_EXEC);
  Flag at_decode = (bpi->recovery_point == RECOVER_AT_DECODE);
  Flag at_fe = (bpi->recovery_point == RECOVER_AT_FE);
  op->pcprof_recov |= (at_exec ? 1 : 0) | (at_decode ? 2 : 0) | (at_fe ? 4 : 0);

  if (at_exec) {
    const Cf_Type cf = op->uop->cf_type;
    const Flag btb_miss = btb_pred_miss(op->btb_pred_info);
    op->pcprof_misp_btb_miss = btb_miss;
    op->pcprof_misp_dir = FALSE;
    uns8 src = MS_OTHER;
    if (cf == CF_CBR || cf == CF_REP) {
      if (op->bp_pred_main.pred_orig != op->oracle_info.dir) {
        op->pcprof_misp_dir = TRUE;
        switch (op->pcprof_tage_comp) {
          case 1:  // TAGE_SHORT
          case 2:  // TAGE_LONG
            src = op->pcprof_tage_used_alt ? MS_TAGE_ALT : MS_TAGE_PROV;
            break;
          case 3:
            src = MS_LOOP;
            break;
          case 4:
            src = MS_SC;
            break;
          default:  // TAGE_BASE: bimodal, either as the only hit or as the chosen alternate
            src = (op->pcprof_tage_hitbank > 0 && op->pcprof_tage_used_alt) ? MS_TAGE_ALT : MS_BIM;
            break;
        }
      } else {
        src = btb_miss ? MS_NO_TGT : MS_BTB_TGT;
      }
    } else if (cf == CF_IBR || cf == CF_ICALL) {
      if (ENABLE_IBP && !op->btb_pred_info->ibp_miss) {
        // the provider/alt/base split exists only for ITTAGE; other IBP mechanisms count as other
        if (IBTB_MECH == ITTAGE_IBTB) {
          const Ittage_Pred_Info* pi = &op->btb_pred_info->ittage;
          src = pi->used_alt ? MS_ITT_ALT : (pi->provider == 0 ? MS_ITT_BASE : MS_ITT_PROV);
        } else {
          src = MS_OTHER;
        }
      } else {
        src = btb_miss ? MS_NO_TGT : MS_BTB_TGT;
      }
    } else if (cf == CF_RET || cf == CF_ICO) {
      src = MS_RAS;
    }
    op->pcprof_misp = src;
  }

  // An exec recovery keeps the blame for the empty-ROB refill window even if a
  // decode/fe resteer fires on the refetched path before the first op arrives.
  if (g->in_recovery && g->recovery_exec && !at_exec)
    return;
  g->in_recovery = TRUE;
  g->recovery_exec = at_exec;
  g->recovery_pc = op->inst->addr;
}

extern "C" void pcprof_rob_insert(Op* op) {
  if (!PC_PROFILE || op->off_path)
    return;
  ensure();
  if (g->pending_fe) {
    recs(op->inst->addr).add(C_ST_FE, g->pending_fe);
    g->pending_fe = 0;
  }
  g->in_recovery = FALSE;
}

extern "C" void pcprof_dcache_fill(Op* op, struct Mem_Req_struct* req) {
  if (!PC_PROFILE)
    return;
  op->pcprof_mem_level = req->mlc_hit ? 2 : (req->l1_hit ? 3 : 4);
}

extern "C" void pcprof_fetch(Addr fetch_addr, Flag off_path, Flag uc_hit, Flag ic_hit) {
  if (!PC_PROFILE || off_path)
    return;
  ensure();
  if (!measuring())
    return;
  R r = recs(fetch_addr);
  r.add(C_FT_FETCH);
  if (!uc_hit) {
    r.add(C_FT_UC_MISS);
    if (!ic_hit)
      r.add(C_FT_IC_MISS);
  }
}

extern "C" void pcprof_node_cycle(void) {
  if (!PC_PROFILE)
    return;
  ensure();
  uns retired = g->retired_this_cycle;
  g->retired_this_cycle = 0;
  if (!measuring())
    return;
  g->cycles++;
  if (!g->sys_cur)
    g->sys_cur = &g->sys_tot[g->sys_phase];
  if (!g->mark_cur)
    g->mark_cur = &g->mark_tot[g->mark_phase];
  Prof::PhaseTot& st = *g->sys_cur;
  Prof::PhaseTot& mk = *g->mark_cur;
  st.cycles++;
  mk.cycles++;
  if (retired) {
    g->retiring_cycles++;
    st.retiring++;
    mk.retiring++;
    return;
  }
  Op* h = node->node_head;
  if (!h || h->off_path) {
    if (g->in_recovery)
      recs(g->recovery_pc).add(g->recovery_exec ? C_ST_BMISP : C_ST_RESTEER);
    else
      g->pending_fe++;
    return;
  }
  uns cat;
  const Mem_Type mt = h->uop->mem_type;
  if (h->recovery_scheduled ||
      (h->uop->cf_type && (h->bp_pred_info->recovery_point == RECOVER_AT_EXEC) && !(h->state == OS_DONE || OP_DONE(h))))
    cat = PCPROF_OPC_BMISP;
  else if (h->redirect_scheduled || h->state == OS_DONE || OP_DONE(h))
    cat = PCPROF_OPC_OTHER;
  else if (mt != NOT_MEM && (op_get_dcache_cycle(h) != MAX_CTR || h->engine_info.dcmiss || h->state == OS_WAIT_DCACHE ||
                             h->state == OS_WAIT_MEM || h->state == OS_MISS))
    cat = PCPROF_OPC_MEM;
  else
    cat = PCPROF_OPC_EXEC;
  h->pcprof_stall[cat]++;
}

extern "C" void pcprof_retire(Op* op) {
  if (!PC_PROFILE)
    return;
  ensure();
  g->retired_this_cycle++;
  apply_events(op->inst_uid);
  if (op->eom)
    g->insts_since_sys++;
  if (!measuring())
    return;

  const Addr pc = op->inst->addr;
  R r = recs(pc);
  r.add(C_UOPS);
  if (op->eom) {
    r.add(C_INSTS);
    g->inst_pcs.insert(pc);
    g->icache_lines.insert(pc >> 6);
    g->icache_pages.insert(pc >> 12);
  }

  const Cf_Type cf = op->uop->cf_type;
  if (cf) {
    r.set_cf(cf);
    if (op->oracle_info.dir)
      r.add(C_TAKEN);
    if (cf == CF_CBR || cf == CF_REP) {
      g->cond_dyn++;
      g->br_reuse.access(pc);
    } else if (cf == CF_IBR || cf == CF_ICALL) {
      g->ind_dyn++;
      g->ind_pcs.insert(pc);
      g->ind_pairs.insert(std::make_pair(pc, (Addr)op->oracle_info.npc));
    } else if (cf == CF_RET) {
      g->ret_pcs.insert(pc);
    }
    if (op->pcprof_recov & 1) {
      r.add(C_MISP_EXEC);
      r.add(op->pcprof_misp_dir ? C_MISP_DIR : C_MISP_TGT);
      if (op->pcprof_misp_btb_miss)
        r.add(C_MISP_BTB_MISS);
      static const int src_col[] = {C_MISP_OTHER,  C_MISP_BIM,      C_MISP_TAGE_PROV, C_MISP_TAGE_ALT, C_MISP_LOOP,
                                    C_MISP_SC,     C_MISP_ITT_PROV, C_MISP_ITT_ALT,   C_MISP_ITT_BASE, C_MISP_BTB_TGT,
                                    C_MISP_NO_TGT, C_MISP_RAS,      C_MISP_OTHER};
      r.add(src_col[op->pcprof_misp]);
    }
    if (op->pcprof_recov & 2)
      r.add(C_RECOV_DECODE);
    if (op->pcprof_recov & 4)
      r.add(C_RECOV_FE);
  }

  const Mem_Type mt = op->uop->mem_type;
  if (mt == MEM_LD || mt == MEM_ST) {
    g->mem_dyn++;
    g->data_reuse.access(op->oracle_info.va >> 6);
    g->data_pages.insert(op->oracle_info.va >> 12);
    if (mt == MEM_LD) {
      r.add(C_LOADS);
      if (op->engine_info.dcmiss) {
        r.add(C_L1D_MISS);
        if (op->pcprof_mem_level >= 3)
          r.add(C_MLC_MISS);
        if (op->pcprof_mem_level == 4)
          r.add(C_LLC_MISS);
        const Counter dc = op_get_dcache_cycle(op), done = op_get_done_cycle(op);
        if (dc != MAX_CTR && done != MAX_CTR && done > dc)
          r.add(C_LD_MISS_CYCLES, done - dc);
      }
    } else {
      r.add(C_STORES);
      if (op->engine_info.dcmiss)
        r.add(C_ST_L1D_MISS);
    }
  }

  r.add(C_ST_BMISP, op->pcprof_stall[PCPROF_OPC_BMISP]);
  r.add(C_ST_EXEC, op->pcprof_stall[PCPROF_OPC_EXEC]);
  r.add(C_ST_OTHER, op->pcprof_stall[PCPROF_OPC_OTHER]);
  uns32 m = op->pcprof_stall[PCPROF_OPC_MEM];
  if (m) {
    int col = C_ST_L1D;
    if (op->engine_info.dcmiss)
      col = op->pcprof_mem_level == 2   ? C_ST_MLC
            : op->pcprof_mem_level == 3 ? C_ST_LLC
            : op->pcprof_mem_level == 4 ? C_ST_DRAM
                                        : C_ST_MEM_UNK;
    r.add(col, m);
  }
}

/**************************************************************************************/
/* dump */

namespace {

typedef bool (*Filter)(const PcRec&);
bool f_all(const PcRec&) {
  return true;
}
bool f_cond(const PcRec& r) {
  return r.cf == CF_CBR || r.cf == CF_REP;
}
bool f_ind(const PcRec& r) {
  return r.cf == CF_IBR || r.cf == CF_ICALL;
}
bool f_ret(const PcRec& r) {
  return r.cf == CF_RET || r.cf == CF_ICO;
}

void coverage(FILE* f, const char* name, const std::vector<const PcRec*>& all, int col, Filter filt,
              uns64 (*val)(const PcRec&) = nullptr) {
  std::vector<uns64> v;
  uns64 tot = 0;
  for (const PcRec* r : all) {
    if (!filt(*r))
      continue;
    uns64 x = val ? val(*r) : r->c[col];
    if (x) {
      v.push_back(x);
      tot += x;
    }
  }
  std::sort(v.begin(), v.end(), std::greater<uns64>());
  uns64 s10 = 0, s100 = 0, s1000 = 0, s = 0;
  size_t n50 = 0, n90 = 0;
  for (size_t i = 0; i < v.size(); i++) {
    s += v[i];
    if (i < 10)
      s10 = s;
    if (i < 100)
      s100 = s;
    if (i < 1000)
      s1000 = s;
    if (!n50 && 2 * s >= tot)
      n50 = i + 1;
    if (!n90 && 10 * s >= 9 * tot)
      n90 = i + 1;
  }
  auto fr = [&](uns64 x) { return tot ? (double)x / tot : 0.0; };
  fprintf(f, "%-22s %14llu %9zu %7.4f %7.4f %7.4f %8zu %8zu\n", name, (unsigned long long)tot, v.size(), fr(s10),
          fr(s100), fr(s1000), n50, n90);
}

uns64 v_stall_total(const PcRec& r) {
  uns64 s = 0;
  for (int c = C_ST_BMISP; c <= C_ST_OTHER; c++)
    s += r.c[c];
  return s;
}
uns64 v_stall_mem(const PcRec& r) {
  return r.c[C_ST_L1D] + r.c[C_ST_MLC] + r.c[C_ST_LLC] + r.c[C_ST_DRAM] + r.c[C_ST_MEM_UNK];
}

void print_hist(FILE* f, const char* name, const StackDist& sd) {
  fprintf(f, "\n# %s: LRU stack distance histogram (distinct other keys between consecutive touches)\n", name);
  fprintf(f, "%s_accesses %llu\n%s_cold %llu\n", name, (unsigned long long)sd.accesses, name,
          (unsigned long long)sd.cold);
  fprintf(f, "%-12s %-24s %14s %8s %8s\n", "bucket", "range", "count", "frac", "cumfrac");
  uns64 reuse = sd.accesses - sd.cold, cum = 0;
  int last = 0;
  for (int b = 0; b < StackDist::NB; b++)
    if (sd.hist[b])
      last = b;
  for (int b = 0; b <= last; b++) {
    char range[64];
    if (b == 0)
      snprintf(range, sizeof(range), "0");
    else
      snprintf(range, sizeof(range), "[%llu,%llu)", 1ull << (b - 1), 1ull << b);
    cum += sd.hist[b];
    fprintf(f, "%s_sd_%02d %-24s %14llu %8.5f %8.5f\n", name, b, range, (unsigned long long)sd.hist[b],
            reuse ? (double)sd.hist[b] / reuse : 0.0, reuse ? (double)cum / reuse : 0.0);
  }
}

struct SysName {
  int nr;
  const char* name;
  const char* cat;
};
// x86-64 Linux syscall numbers. Categories: net_recv, read, net_send (includes write),
// wait_child, poll, proc_create, file, futex, other.
const SysName sys_names[] = {
    {0, "read", "read"},
    {1, "write", "net_send"},
    {2, "open", "file"},
    {3, "close", "file"},
    {4, "stat", "file"},
    {5, "fstat", "file"},
    {6, "lstat", "file"},
    {7, "poll", "poll"},
    {8, "lseek", "file"},
    {9, "mmap", "file"},
    {10, "mprotect", "other"},
    {11, "munmap", "file"},
    {12, "brk", "other"},
    {13, "rt_sigaction", "other"},
    {14, "rt_sigprocmask", "other"},
    {15, "rt_sigreturn", "other"},
    {16, "ioctl", "other"},
    {17, "pread64", "read"},
    {18, "pwrite64", "other"},
    {19, "readv", "read"},
    {20, "writev", "net_send"},
    {21, "access", "other"},
    {22, "pipe", "other"},
    {23, "select", "poll"},
    {24, "sched_yield", "other"},
    {25, "mremap", "other"},
    {28, "madvise", "other"},
    {32, "dup", "other"},
    {33, "dup2", "other"},
    {35, "nanosleep", "other"},
    {39, "getpid", "other"},
    {41, "socket", "other"},
    {42, "connect", "other"},
    {43, "accept", "other"},
    {44, "sendto", "net_send"},
    {45, "recvfrom", "net_recv"},
    {46, "sendmsg", "net_send"},
    {47, "recvmsg", "net_recv"},
    {48, "shutdown", "other"},
    {49, "bind", "other"},
    {50, "listen", "other"},
    {51, "getsockname", "other"},
    {52, "getpeername", "other"},
    {53, "socketpair", "other"},
    {54, "setsockopt", "other"},
    {55, "getsockopt", "other"},
    {56, "clone", "proc_create"},
    {57, "fork", "proc_create"},
    {58, "vfork", "proc_create"},
    {59, "execve", "proc_create"},
    {60, "exit", "other"},
    {61, "wait4", "wait_child"},
    {62, "kill", "other"},
    {63, "uname", "other"},
    {72, "fcntl", "other"},
    {74, "fsync", "other"},
    {77, "ftruncate", "other"},
    {78, "getdents", "file"},
    {79, "getcwd", "other"},
    {80, "chdir", "other"},
    {82, "rename", "other"},
    {83, "mkdir", "other"},
    {84, "rmdir", "other"},
    {87, "unlink", "other"},
    {89, "readlink", "other"},
    {90, "chmod", "other"},
    {95, "umask", "other"},
    {96, "gettimeofday", "other"},
    {97, "getrlimit", "other"},
    {102, "getuid", "other"},
    {104, "getgid", "other"},
    {107, "geteuid", "other"},
    {108, "getegid", "other"},
    {110, "getppid", "other"},
    {131, "sigaltstack", "other"},
    {137, "statfs", "file"},
    {138, "fstatfs", "file"},
    {157, "prctl", "other"},
    {158, "arch_prctl", "other"},
    {186, "gettid", "other"},
    {200, "tkill", "other"},
    {202, "futex", "futex"},
    {204, "sched_getaffinity", "other"},
    {213, "epoll_create", "other"},
    {217, "getdents64", "file"},
    {218, "set_tid_address", "other"},
    {228, "clock_gettime", "other"},
    {230, "clock_nanosleep", "other"},
    {231, "exit_group", "other"},
    {232, "epoll_wait", "poll"},
    {233, "epoll_ctl", "other"},
    {234, "tgkill", "other"},
    {247, "waitid", "wait_child"},
    {257, "openat", "file"},
    {262, "newfstatat", "file"},
    {263, "unlinkat", "other"},
    {267, "readlinkat", "other"},
    {269, "faccessat", "other"},
    {270, "pselect6", "poll"},
    {271, "ppoll", "poll"},
    {273, "set_robust_list", "other"},
    {281, "epoll_pwait", "poll"},
    {288, "accept4", "other"},
    {290, "eventfd2", "other"},
    {291, "epoll_create1", "other"},
    {292, "dup3", "other"},
    {293, "pipe2", "other"},
    {299, "recvmmsg", "net_recv"},
    {302, "prlimit64", "other"},
    {307, "sendmmsg", "net_send"},
    {318, "getrandom", "other"},
    {332, "statx", "file"},
    {334, "rseq", "other"},
    {434, "pidfd_open", "other"},
    {435, "clone3", "proc_create"},
    {436, "close_range", "file"},
    {439, "faccessat2", "other"},
    {441, "epoll_pwait2", "poll"},
};

void sys_label(int64 nr, std::string* name, std::string* cat) {
  if (nr < 0) {
    *name = "none";
    *cat = "none";
    return;
  }
  for (const SysName& sn : sys_names)
    if (sn.nr == nr) {
      *name = sn.name;
      *cat = sn.cat;
      return;
    }
  *name = "sys" + std::to_string(nr);
  *cat = "other";
}

void write_phase_pcs(const std::string& path, const std::unordered_map<std::pair<Addr, int64>, PcRec, PairHash64>& m) {
  std::vector<std::pair<std::pair<Addr, int64>, const PcRec*>> rows;
  rows.reserve(m.size());
  for (auto& kv : m) {
    Flag nz = FALSE;
    for (int c = 0; c < NUM_COLS && !nz; c++)
      nz = kv.second.c[c] != 0;
    if (nz)
      rows.emplace_back(kv.first, &kv.second);
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.first.second != b.first.second ? a.first.second < b.first.second : a.first.first < b.first.first;
  });
  FILE* f = fopen(path.c_str(), "w");
  ASSERTM(0, f, "cannot open %s\n", path.c_str());
  fprintf(f, "pc,phase,cf");
  for (int c = 0; c < NUM_COLS; c++)
    fprintf(f, ",%s", col_names[c]);
  fprintf(f, "\n");
  for (auto& row : rows) {
    fprintf(f, "0x%llx,%lld,%s", (unsigned long long)row.first.first, (long long)row.first.second,
            cf_names[row.second->cf]);
    for (int c = 0; c < NUM_COLS; c++)
      fprintf(f, ",%llu", (unsigned long long)row.second->c[c]);
    fprintf(f, "\n");
  }
  fclose(f);
}

struct PhaseAgg {
  uns64 c[NUM_COLS] = {0};
  uns64 cond = 0, ind = 0, ret = 0;
  Prof::PhaseTot t;
};

void aggregate(const std::unordered_map<std::pair<Addr, int64>, PcRec, PairHash64>& m,
               const std::unordered_map<int64, Prof::PhaseTot>& tots, std::map<int64, PhaseAgg>* out) {
  for (auto& kv : tots)
    (*out)[kv.first].t = kv.second;
  for (auto& kv : m) {
    PhaseAgg& a = (*out)[kv.first.second];
    const PcRec& r = kv.second;
    for (int c = 0; c < NUM_COLS; c++)
      a.c[c] += r.c[c];
    if (r.cf == CF_CBR || r.cf == CF_REP)
      a.cond += r.c[C_MISP_EXEC];
    else if (r.cf == CF_IBR || r.cf == CF_ICALL)
      a.ind += r.c[C_MISP_EXEC];
    else if (r.cf == CF_RET || r.cf == CF_ICO)
      a.ret += r.c[C_MISP_EXEC];
  }
}

void phase_row(FILE* f, const char* dim, const std::string& phase, const std::string& name, const std::string& cat,
               const PhaseAgg& a) {
  fprintf(f, "%s,%s,%s,%s,%llu,%llu,%llu,%llu,%llu,%llu", dim, phase.c_str(), name.c_str(), cat.c_str(),
          (unsigned long long)a.t.entries, (unsigned long long)a.t.cycles, (unsigned long long)a.t.retiring,
          (unsigned long long)a.cond, (unsigned long long)a.ind, (unsigned long long)a.ret);
  for (int c = 0; c < NUM_COLS; c++)
    fprintf(f, ",%llu", (unsigned long long)a.c[c]);
  fprintf(f, "\n");
}

void dump_phases(const std::string& dir) {
  write_phase_pcs(dir + "/pcprof_phase_sys.csv", g->sys_pcs);
  write_phase_pcs(dir + "/pcprof_phase_mark.csv", g->mark_pcs);

  std::map<int64, PhaseAgg> sys, mark;
  aggregate(g->sys_pcs, g->sys_tot, &sys);
  aggregate(g->mark_pcs, g->mark_tot, &mark);
  std::map<std::string, PhaseAgg> cats;
  for (auto& kv : sys) {
    std::string name, cat;
    sys_label(kv.first, &name, &cat);
    PhaseAgg& c = cats[cat];
    for (int i = 0; i < NUM_COLS; i++)
      c.c[i] += kv.second.c[i];
    c.cond += kv.second.cond;
    c.ind += kv.second.ind;
    c.ret += kv.second.ret;
    c.t.cycles += kv.second.t.cycles;
    c.t.retiring += kv.second.t.retiring;
    c.t.entries += kv.second.t.entries;
  }

  FILE* f = fopen((dir + "/pcprof_phases.csv").c_str(), "w");
  ASSERTM(0, f, "cannot open pcprof_phases.csv\n");
  fprintf(f, "dim,phase,name,category,entries,cycles,retiring_cycles,misp_exec_cond,misp_exec_ind,misp_exec_ret");
  for (int c = 0; c < NUM_COLS; c++)
    fprintf(f, ",%s", col_names[c]);
  fprintf(f, "\n");
  for (auto& kv : sys) {
    std::string name, cat;
    sys_label(kv.first, &name, &cat);
    phase_row(f, "sys", std::to_string(kv.first), name, cat, kv.second);
  }
  for (auto& kv : cats)
    phase_row(f, "syscat", kv.first, kv.first, kv.first, kv.second);
  for (auto& kv : mark)
    phase_row(f, "mark", std::to_string(kv.first), kv.first ? "marked_" + std::to_string(kv.first) : "unmarked",
              kv.first ? "marked" : "unmarked", kv.second);
  fclose(f);
}

}  // namespace

extern "C" void pcprof_dump(uns proc_id) {
  if (!PC_PROFILE || proc_id != 0)
    return;
  ensure();
  g->fe_unattributed += g->pending_fe;
  g->pending_fe = 0;

  std::vector<std::pair<Addr, const PcRec*>> rows;
  rows.reserve(g->pcs.size());
  for (auto& kv : g->pcs)
    rows.emplace_back(kv.first, &kv.second);
  std::sort(
      rows.begin(), rows.end(),
      [](const std::pair<Addr, const PcRec*>& a, const std::pair<Addr, const PcRec*>& b) { return a.first < b.first; });

  std::string dir = OUTPUT_DIR;
  FILE* f = fopen((dir + "/pcprof.csv").c_str(), "w");
  ASSERTM(0, f, "cannot open pcprof.csv\n");
  std::unordered_map<Addr, uns32> ntgt;
  for (auto& p : g->ind_pairs)
    ntgt[p.first]++;
  fprintf(f, "pc,cf,ntgt");
  for (int c = 0; c < NUM_COLS; c++)
    fprintf(f, ",%s", col_names[c]);
  fprintf(f, "\n");
  std::vector<const PcRec*> all;
  uns64 tot[NUM_COLS] = {0};
  for (auto& row : rows) {
    const PcRec& r = *row.second;
    all.push_back(&r);
    auto nt = ntgt.find(row.first);
    fprintf(f, "0x%llx,%s,%u", (unsigned long long)row.first, cf_names[r.cf], nt == ntgt.end() ? 0u : nt->second);
    for (int c = 0; c < NUM_COLS; c++) {
      fprintf(f, ",%llu", (unsigned long long)r.c[c]);
      tot[c] += r.c[c];
    }
    fprintf(f, "\n");
  }
  fclose(f);

  f = fopen((dir + "/pcprof_summary.txt").c_str(), "w");
  ASSERTM(0, f, "cannot open pcprof_summary.txt\n");
  fprintf(f, "# pc_profile summary (measured region: from full_warmup to end; on-path retired unless noted)\n");
  fprintf(f, "start_cycle %llu\n", (unsigned long long)g->start_cycle);
  fprintf(f, "cycles %llu\n", (unsigned long long)g->cycles);
  fprintf(f, "retiring_cycles %llu\n", (unsigned long long)g->retiring_cycles);
  uns64 st_sum = 0;
  for (int c = C_ST_BMISP; c <= C_ST_OTHER; c++)
    st_sum += tot[c];
  fprintf(f, "stall_cycles_attributed %llu\n", (unsigned long long)st_sum);
  fprintf(f, "stall_fe_unattributed %llu\n", (unsigned long long)g->fe_unattributed);
  fprintf(f, "cycle_check_residual %lld\n",
          (long long)g->cycles - (long long)(g->retiring_cycles + st_sum + g->fe_unattributed));
  fprintf(f, "\n# column totals\n");
  for (int c = 0; c < NUM_COLS; c++)
    fprintf(f, "total_%s %llu\n", col_names[c], (unsigned long long)tot[c]);
  fprintf(f, "\n# stall category share of all cycles\n");
  for (int c = C_ST_BMISP; c <= C_ST_OTHER; c++)
    fprintf(f, "frac_cycles_%s %.5f\n", col_names[c], g->cycles ? (double)tot[c] / g->cycles : 0.0);
  fprintf(f, "frac_cycles_retiring %.5f\n", g->cycles ? (double)g->retiring_cycles / g->cycles : 0.0);

  fprintf(f, "\n# concentration: fraction of each event covered by the top-N PCs\n");
  fprintf(f, "%-22s %14s %9s %7s %7s %7s %8s %8s\n", "event", "total", "npcs", "top10", "top100", "top1000", "pcs50%",
          "pcs90%");
  coverage(f, "insts", all, C_INSTS, f_all);
  coverage(f, "misp_exec_all", all, C_MISP_EXEC, f_all);
  coverage(f, "misp_exec_cond", all, C_MISP_EXEC, f_cond);
  coverage(f, "misp_exec_indirect", all, C_MISP_EXEC, f_ind);
  coverage(f, "misp_exec_ret", all, C_MISP_EXEC, f_ret);
  coverage(f, "recov_decode", all, C_RECOV_DECODE, f_all);
  coverage(f, "l1d_miss", all, C_L1D_MISS, f_all);
  coverage(f, "mlc_miss", all, C_MLC_MISS, f_all);
  coverage(f, "llc_miss", all, C_LLC_MISS, f_all);
  coverage(f, "ld_miss_cycles", all, C_LD_MISS_CYCLES, f_all);
  coverage(f, "st_l1d_miss", all, C_ST_L1D_MISS, f_all);
  coverage(f, "ft_uc_miss", all, C_FT_UC_MISS, f_all);
  coverage(f, "ft_ic_miss", all, C_FT_IC_MISS, f_all);
  coverage(f, "stall_total", all, 0, f_all, v_stall_total);
  coverage(f, "stall_mem", all, 0, f_all, v_stall_mem);
  for (int c = C_ST_BMISP; c <= C_ST_OTHER; c++)
    coverage(f, col_names[c], all, c, f_all);

  fprintf(f, "\n# static footprints (measured region)\n");
  size_t cond_pcs = 0, uc_pcs = 0;
  for (const PcRec* r : all)
    if (f_cond(*r) && r->c[C_UOPS])
      cond_pcs++;
  for (const PcRec* r : all)
    if (r->c[C_FT_FETCH])
      uc_pcs++;
  fprintf(f, "distinct_inst_pcs %zu\n", g->inst_pcs.size());
  fprintf(f, "distinct_cond_branch_pcs %zu\n", cond_pcs);
  fprintf(f, "distinct_indirect_pcs %zu\n", g->ind_pcs.size());
  fprintf(f, "distinct_indirect_pc_target_pairs %zu\n", g->ind_pairs.size());
  fprintf(f, "distinct_return_pcs %zu\n", g->ret_pcs.size());
  fprintf(f, "distinct_icache_lines_64B %zu\n", g->icache_lines.size());
  fprintf(f, "distinct_icache_pages_4K %zu\n", g->icache_pages.size());
  fprintf(f, "distinct_fetch_target_starts %zu\n", uc_pcs);
  fprintf(f, "distinct_data_lines_64B %zu\n", g->data_reuse.distinct());
  fprintf(f, "distinct_data_pages_4K %zu\n", g->data_pages.size());
  fprintf(f, "dyn_cond_branches %llu\n", (unsigned long long)g->cond_dyn);
  fprintf(f, "dyn_indirect_branches %llu\n", (unsigned long long)g->ind_dyn);
  fprintf(f, "dyn_mem_uops %llu\n", (unsigned long long)g->mem_dyn);

  print_hist(f, "cond_branch_pc", g->br_reuse);
  print_hist(f, "data_line", g->data_reuse);

  fprintf(f, "\n# phases (details in pcprof_phases.csv, pcprof_phase_sys.csv, pcprof_phase_mark.csv)\n");
  std::string n0, c0;
  sys_label(g->sys_phase_at_start, &n0, &c0);
  fprintf(f, "syscalls_in_region %llu\n", (unsigned long long)g->syscalls_in_region);
  fprintf(f, "sys_phase_at_start %lld %s\n", (long long)g->sys_phase_at_start, n0.c_str());
  fprintf(f, "mark_phase_at_start %lld\n", (long long)g->mark_phase_at_start);
  fprintf(f, "mark_begins_total_including_warmup %lld\n", (long long)g->mark_count);
  fprintf(f, "mark_begins_in_region %llu\nmark_ends_in_region %llu\n", (unsigned long long)g->mark_begins_in_region,
          (unsigned long long)g->mark_ends_in_region);
  fprintf(f, "\n# inter-syscall window length (retired instructions between consecutive syscalls ending in region)\n");
  fprintf(f, "syscall_windows %llu\nsyscall_window_mean %.1f\n", (unsigned long long)g->win_count,
          g->win_count ? (double)g->win_sum / g->win_count : 0.0);
  for (int b = 0; b < 64; b++) {
    if (!g->win_hist[b])
      continue;
    char range[64];
    if (b == 0)
      snprintf(range, sizeof(range), "0");
    else
      snprintf(range, sizeof(range), "[%llu,%llu)", 1ull << (b - 1), 1ull << b);
    fprintf(f, "syscall_window_%02d %-24s %llu\n", b, range, (unsigned long long)g->win_hist[b]);
  }
  fclose(f);

  dump_phases(dir);
}
