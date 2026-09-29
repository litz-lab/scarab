/***************************************************************************************
 * File         : pcprof.cc
 * Description  : AsmDB-style per-PC attribution (--pc_profile 1). See pcprof.h.
 *
 * Everything is keyed by the static instruction address (op->inst_info->addr, the raw
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
    "insts",         "uops",          "taken",        "misp_exec",     "misp_dir",     "misp_tgt",
    "misp_btb_miss", "misp_bim",      "misp_tage_prov", "misp_tage_alt", "misp_loop",  "misp_sc",
    "misp_itt_prov", "misp_itt_alt",  "misp_itt_base", "misp_btb_tgt", "misp_no_tgt",  "misp_ras",
    "misp_other",    "recov_decode",  "recov_fe",     "loads",         "stores",       "l1d_miss",
    "mlc_miss",      "llc_miss",      "ld_miss_cycles", "st_l1d_miss", "ft_fetch",     "ft_uc_miss",
    "ft_ic_miss",    "st_bmisp",      "st_resteer",   "st_l1d",        "st_mlc",       "st_llc",
    "st_dram",       "st_mem_unk",    "st_exec",      "st_fe",         "st_other"};

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
  uns64 c[NUM_COLS];
  uns8 cf;
  PcRec() : cf(0) { std::fill(c, c + NUM_COLS, 0); }
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
};

Prof* g = nullptr;

inline Flag measuring() {
  if (FULL_WARMUP && !warmup_dump_done[0])
    return FALSE;
  if (!g->started) {
    g->started = TRUE;
    g->start_cycle = cycle_count;
  }
  return TRUE;
}

inline PcRec& rec(Addr pc) {
  return g->pcs[pc];
}

inline void ensure() {
  if (!g)
    g = new Prof();
}

}  // namespace

/**************************************************************************************/

extern "C" void pcprof_recovery(Op* op) {
  if (!PC_PROFILE || !op)
    return;
  ensure();
  Bp_Pred_Info* bpi = op->bp_pred_info;
  Flag at_exec = bpi->recover_at_exec;
  Flag at_decode = bpi->recover_at_decode;
  Flag at_fe = bpi->recover_at_fe;
  op->pcprof_recov |= (at_exec ? 1 : 0) | (at_decode ? 2 : 0) | (at_fe ? 4 : 0);

  if (at_exec) {
    const Cf_Type cf = op->inst_info->table_info.cf_type;
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
        const Ittage_Pred_Info* pi = &op->btb_pred_info->ittage;
        src = pi->used_alt ? MS_ITT_ALT : (pi->provider == 0 ? MS_ITT_BASE : MS_ITT_PROV);
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
  g->recovery_pc = op->inst_info->addr;
}

extern "C" void pcprof_rob_insert(Op* op) {
  if (!PC_PROFILE || op->off_path)
    return;
  ensure();
  if (g->pending_fe) {
    rec(op->inst_info->addr).c[C_ST_FE] += g->pending_fe;
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
  PcRec& r = rec(fetch_addr);
  r.c[C_FT_FETCH]++;
  if (!uc_hit) {
    r.c[C_FT_UC_MISS]++;
    if (!ic_hit)
      r.c[C_FT_IC_MISS]++;
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
  if (retired) {
    g->retiring_cycles++;
    return;
  }
  Op* h = node->node_head;
  if (!h || h->off_path) {
    if (g->in_recovery)
      rec(g->recovery_pc).c[g->recovery_exec ? C_ST_BMISP : C_ST_RESTEER]++;
    else
      g->pending_fe++;
    return;
  }
  uns cat;
  const Mem_Type mt = h->inst_info->table_info.mem_type;
  if (h->recovery_scheduled ||
      (h->inst_info->table_info.cf_type && h->bp_pred_info->recover_at_exec && !(h->state == OS_DONE || OP_DONE(h))))
    cat = PCPROF_OPC_BMISP;
  else if (h->redirect_scheduled || h->state == OS_DONE || OP_DONE(h))
    cat = PCPROF_OPC_OTHER;
  else if (mt != NOT_MEM && (h->dcache_cycle != 0 || h->engine_info.dcmiss || h->state == OS_WAIT_DCACHE ||
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
  if (!measuring())
    return;

  const Addr pc = op->inst_info->addr;
  PcRec& r = rec(pc);
  r.c[C_UOPS]++;
  if (op->eom) {
    r.c[C_INSTS]++;
    g->inst_pcs.insert(pc);
    g->icache_lines.insert(pc >> 6);
    g->icache_pages.insert(pc >> 12);
  }

  const Cf_Type cf = op->inst_info->table_info.cf_type;
  if (cf) {
    r.cf = cf;
    if (op->oracle_info.dir)
      r.c[C_TAKEN]++;
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
      r.c[C_MISP_EXEC]++;
      r.c[op->pcprof_misp_dir ? C_MISP_DIR : C_MISP_TGT]++;
      if (op->pcprof_misp_btb_miss)
        r.c[C_MISP_BTB_MISS]++;
      static const int src_col[] = {C_MISP_OTHER,   C_MISP_BIM,     C_MISP_TAGE_PROV, C_MISP_TAGE_ALT, C_MISP_LOOP,
                                    C_MISP_SC,      C_MISP_ITT_PROV, C_MISP_ITT_ALT,  C_MISP_ITT_BASE, C_MISP_BTB_TGT,
                                    C_MISP_NO_TGT,  C_MISP_RAS,     C_MISP_OTHER};
      r.c[src_col[op->pcprof_misp]]++;
    }
    if (op->pcprof_recov & 2)
      r.c[C_RECOV_DECODE]++;
    if (op->pcprof_recov & 4)
      r.c[C_RECOV_FE]++;
  }

  const Mem_Type mt = op->inst_info->table_info.mem_type;
  if (mt == MEM_LD || mt == MEM_ST) {
    g->mem_dyn++;
    g->data_reuse.access(op->oracle_info.va >> 6);
    g->data_pages.insert(op->oracle_info.va >> 12);
    if (mt == MEM_LD) {
      r.c[C_LOADS]++;
      if (op->engine_info.dcmiss) {
        r.c[C_L1D_MISS]++;
        if (op->pcprof_mem_level >= 3)
          r.c[C_MLC_MISS]++;
        if (op->pcprof_mem_level == 4)
          r.c[C_LLC_MISS]++;
        if (op->done_cycle > op->dcache_cycle)
          r.c[C_LD_MISS_CYCLES] += op->done_cycle - op->dcache_cycle;
      }
    } else {
      r.c[C_STORES]++;
      if (op->engine_info.dcmiss)
        r.c[C_ST_L1D_MISS]++;
    }
  }

  r.c[C_ST_BMISP] += op->pcprof_stall[PCPROF_OPC_BMISP];
  r.c[C_ST_EXEC] += op->pcprof_stall[PCPROF_OPC_EXEC];
  r.c[C_ST_OTHER] += op->pcprof_stall[PCPROF_OPC_OTHER];
  uns32 m = op->pcprof_stall[PCPROF_OPC_MEM];
  if (m) {
    int col = C_ST_L1D;
    if (op->engine_info.dcmiss)
      col = op->pcprof_mem_level == 2 ? C_ST_MLC
                                      : op->pcprof_mem_level == 3 ? C_ST_LLC
                                                                  : op->pcprof_mem_level == 4 ? C_ST_DRAM : C_ST_MEM_UNK;
    r.c[col] += m;
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
  std::sort(rows.begin(), rows.end(),
            [](const std::pair<Addr, const PcRec*>& a, const std::pair<Addr, const PcRec*>& b) {
              return a.first < b.first;
            });

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
  fprintf(f, "%-22s %14s %9s %7s %7s %7s %8s %8s\n", "event", "total", "npcs", "top10", "top100", "top1000",
          "pcs50%", "pcs90%");
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
  fclose(f);
}
