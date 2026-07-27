/*
 * Copyright 2025 University of California Santa Cruz
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
 * File         : load_value_pred.cc
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 10/2025
 * Description  : Generalized speculative load-result predictor framework.
 *
 *   See load_value_pred.h for the category model.  This file provides:
 *     - the fetch-time recovery marker (load_pred_mark_recovery)
 *     - LoadPredictor       : fetch-time interface for value & address predictors
 *     - concrete predictors : last-value (scaffold), constant/stride address
 *     - a per-core registry holding one active scheme per category
 *
 *   The recovery has no cross-op global state: a wrong on-path prediction is
 *   recorded on the load (load_value_mispredicted) and its recovery_info is filled
 *   at predict time; the load then recovers at exec like a branch
 *   (op_set_exec_cycle sets recover_at_exec, exec_stage_bp_resolve fires it).
 ***************************************************************************************/

#include "load_value_pred.h"

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "core.param.h"
#include "memory/memory.param.h"

#include "bp/bp.h"
#include "memory/memory.h"

#include "statistics.h"
}

#include <unordered_map>
#include <vector>

/**************************************************************************************/
/* Shared pipeline effect / recovery helpers */

/*
 * Read one architectural destination value of the load.  Ops carry their
 * destination register values in op->dst_val[], indexed by destination register
 * (0 .. num_dest_regs-1) and populated at uop generation, so they are available
 * by the time predict_ft runs.  Returns FALSE when dst_idx is out of range (e.g.
 * a load form with no destination register), in which case the caller does not
 * speculate on that destination.
 */
static inline Flag load_pred_read_dest_value(Op* op, uns dst_idx, uns64* value) {
  if (dst_idx >= op->inst_info->table_info.num_dest_regs)
    return FALSE;
  *value = op->dst_val[dst_idx];
  return TRUE;
}

/*
 * Mark a mispredicted (on-path) load for recovery.  Like a branch, the load
 * recovers at exec: op_set_exec_cycle() sets recover_at_exec and
 * exec_stage_bp_resolve() fires bp_sched_recovery().  Here we only record the
 * misprediction and fill the op's recovery_info -- a non-CF, fall-through
 * (op->oracle_info.npc) resteer; cf_type = NOT_CF so bp_recover_op leaves
 * branch-predictor state untouched.  bp_sched_recovery squashes ops younger than
 * op, so op must be the macro's EOM; value/address prediction targets
 * single-destination loads whose load uop is its own EOM (asserted in ft.cc).
 * Off-path predicted loads are flushed and never recover.
 */
static inline void load_pred_mark_recovery(Op* op) {
  if (op->off_path)
    return;
  op->load_value_mispredicted = TRUE;
  STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_LOADS_ON_PATH_MISPREDICTED);

  op->recovery_info.proc_id = op->proc_id;
  op->recovery_info.op = op;
  op->recovery_info.op_num = op->op_num;
  op->recovery_info.PC = op->inst_info->addr;
  op->recovery_info.cf_type = NOT_CF;
  op->recovery_info.oracle_dir = op->oracle_info.dir;
  op->recovery_info.new_dir = op->oracle_info.dir;
  op->recovery_info.branchTarget = op->oracle_info.target;
  op->recovery_info.predict_cycle = cycle_count;
}

/*
 * "Early-result" effect: the load's result is made available to consumers before
 * the load itself finishes, so they may wake early (honored in op_sources_add,
 * which also applies ready_cycle as a wake-time floor).  Used by:
 *   - value prediction: the value is predicted, available immediately
 *     (ready_delay = 0);
 *   - RFP: the value is prefetched L1->register-file, so consumers wake after the
 *     prefetch latency (ready_delay = DCACHE_CYCLES).
 * The load still flows through the pipeline and accesses the dcache normally, so
 * its bandwidth/port contention is modeled; only its consumers are accelerated.
 * A wrong prediction is recorded via load_pred_mark_recovery and recovers at exec.
 */
static inline void load_pred_apply_early_result(Op* op, Flag is_mispred, Counter ready_delay) {
  ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);

  op->load_value_predicted = TRUE;
  op->load_pred_ready_delay = ready_delay;

  if (is_mispred)
    load_pred_mark_recovery(op);
}

/*
 * "Early-AGEN" effect (address prediction): only the *address* is known early,
 * not the value.  The load is allowed to issue without waiting for its
 * address-operand (REG_DATA_DEP) registers and to access the dcache at the
 * predicted address (op_sources_add honors load_addr_predicted; dcache_stage
 * uses load_pred_addr).  It then incurs the normal hit/miss latency and wakes its
 * consumers at its real completion - unlike wake-now, consumers are NOT resolved
 * at fetch.  A wrong predicted address is recorded via load_pred_mark_recovery
 * and recovers at exec (as for value/RFP mispredicts).
 */
static inline void load_pred_apply_early_agen(Op* op, Addr pred_addr, Flag is_mispred) {
  ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);

  op->load_addr_predicted = TRUE;
  op->load_pred_addr = pred_addr;

  if (is_mispred)
    load_pred_mark_recovery(op);
}

/*
 * Apply the configured address-prediction effect for a confident prediction.
 * Shared by all address predictor schemes (constant, stride, ...); the algorithm
 * only produces pred_addr, this decides how it acts on the pipeline.
 */
static inline void load_pred_apply_addr(Op* op, Addr pred_addr) {
  Flag is_mispred = (pred_addr != op->oracle_info.va);

  if (LOAD_ADDR_PRED_MODE == LOAD_ADDR_PRED_MODE_RFP) {
    // RFP prefetches the value L1->register file. Model it only when the
    // predicted line is L1-resident (bounded prefetch latency); consumers then
    // wake after the L1->RF transfer (now + DCACHE_CYCLES), NOT immediately. The
    // predicted address is verified at AGEN.
    if (!do_l1_access_addr(pred_addr))
      return;
    load_pred_apply_early_result(op, is_mispred, /*ready_delay=*/DCACHE_CYCLES);
  } else {
    // EARLY_AGEN: issue early and access the predicted address in the dcache
    // stage, which handles hit and miss (mem req) with normal latency.
    load_pred_apply_early_agen(op, pred_addr, is_mispred);
  }
}

static void load_pred_collect_predict_stat(Op* op) {
  ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
  if (op->off_path)
    return;

  STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_LOADS_ON_PATH);
  if (op->load_value_predicted || op->load_addr_predicted) {
    STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_LOADS_ON_PATH_PREDICTED);
  }
}

/**************************************************************************************/
/* Predictor interface */

// Internal linkage: these type names (PredictorEntry, LoadPredictor, ...) are
// generic and collide with identically-named types in other translation units
// (e.g. bp/cbp_tagescl_64k.h). An anonymous namespace keeps them TU-local and
// avoids C++ One-Definition-Rule violations.
namespace {

class PredictorEntry {
 public:
  virtual ~PredictorEntry() = default;
  virtual bool is_found() const = 0;
};

/* Fetch-time predictors that resolve a load early (value & address categories). */
class LoadPredictor {
 public:
  virtual ~LoadPredictor() = default;
  virtual void init(uns8 proc_id) = 0;
  virtual void recover() = 0;

  virtual PredictorEntry* lookup(Op* op) = 0;
  virtual void train(Op* op, PredictorEntry* entry) = 0;
  virtual void infer(Op* op, PredictorEntry* entry) = 0;
};

/* None predictor (category disabled). */
class NoneLoadPredictor : public LoadPredictor {
 public:
  void init(uns8 proc_id) override { return; }
  void recover() override { return; }
  PredictorEntry* lookup(Op* op) override { return nullptr; }
  void train(Op* op, PredictorEntry* entry) override { return; }
  void infer(Op* op, PredictorEntry* entry) override { return; }
};

/**************************************************************************************/
/* Value Predictor: Last Value
 *
 *   Predicts that a load produces the same value it produced last time.  Reads
 *   the load's architectural result via load_pred_read_dest_value() (op->dst_val)
 *   to both train and verify; once confidence exceeds the threshold it resolves
 *   the load early so consumers can wake before it executes.
 */

struct LastValuePredEntry : public PredictorEntry {
  uns64 last_value;
  uns confidence;
  bool found;

  LastValuePredEntry() : last_value(0), confidence(0), found(false) {}
  explicit LastValuePredEntry(uns64 value) : last_value(value), confidence(0), found(true) {}

  bool is_found() const override { return found; }
};

class LastValuePredictor : public LoadPredictor {
 private:
  uns8 proc_id;
  // A load may write more than one architectural destination, so the table is
  // keyed per (PC, destination index) rather than per PC.  MAX_DESTS == 8 fits in
  // the low 3 bits.
  std::unordered_map<uint64_t, LastValuePredEntry> prediction_table;

  static inline uint64_t dest_key(Addr pc, uns dst_idx) { return ((uint64_t)pc << 3) | (dst_idx & 0x7); }

 public:
  void init(uns8 proc_id) override {
    this->proc_id = proc_id;
    prediction_table.clear();
  }
  void recover() override { return; }

  // The per-destination table is consulted directly in infer()/train(); the
  // shared lookup()/entry indirection (used by single-target predictors) is not
  // needed here, so return a non-null sentinel to let predict_op proceed.
  PredictorEntry* lookup(Op* op) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    static LastValuePredEntry sentinel;
    return &sentinel;
  }

  // Speculate only if EVERY destination is trained and confident; a wrong
  // prediction on any destination makes the load a mispredict.  Reads each
  // destination's true value (op->dst_val[i]) to decide correctness.
  void infer(Op* op, PredictorEntry* /*entry*/) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    uns ndest = op->inst_info->table_info.num_dest_regs;
    if (ndest < 1)
      return;

    Flag any_mispred = FALSE;
    for (uns i = 0; i < ndest; i++) {
      uns64 actual_value = 0;
      if (!load_pred_read_dest_value(op, i, &actual_value))
        return;  // value unavailable -> do not speculate
      auto it = prediction_table.find(dest_key(op->inst_info->addr, i));
      if (it == prediction_table.end() || it->second.confidence <= LOAD_VALUE_PRED_THRESHOLD)
        return;  // not trained / not confident -> do not speculate
      if (it->second.last_value != actual_value)
        any_mispred = TRUE;
    }

    // value known now; verified against the real value at load completion.
    load_pred_apply_early_result(op, any_mispred, /*ready_delay=*/0);
  }

  void train(Op* op, PredictorEntry* /*entry*/) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    if (op->off_path)
      return;

    for (uns i = 0; i < op->inst_info->table_info.num_dest_regs; i++) {
      uns64 actual_value = 0;
      if (!load_pred_read_dest_value(op, i, &actual_value))
        continue;
      uint64_t key = dest_key(op->inst_info->addr, i);
      auto it = prediction_table.find(key);
      if (it == prediction_table.end()) {
        prediction_table[key] = LastValuePredEntry(actual_value);
      } else if (it->second.last_value == actual_value) {
        it->second.confidence++;
      } else {
        it->second.last_value = actual_value;
        it->second.confidence = 0;
      }
    }
  }
};

/**************************************************************************************/
/* Address Predictor: Constant
 *
 *   Accelerates critical instruction chains by resolving loads whose effective
 *   address is constant.  Trains loads that repeatedly use the same address and,
 *   once confident and the line is present in the L1, resolves them at fetch.
 */

struct ConstantLoadAddrPredEntry : public PredictorEntry {
  Addr oracle_address;
  uns confidence;
  bool found;

  ConstantLoadAddrPredEntry() : oracle_address(0), confidence(0), found(false) {}
  explicit ConstantLoadAddrPredEntry(Addr addr) : oracle_address(addr), confidence(0), found(true) {}

  bool is_found() const override { return found; }
};

class ConstantLoadAddrPredictor : public LoadPredictor {
 private:
  uns8 proc_id;
  std::unordered_map<Addr, ConstantLoadAddrPredEntry> prediction_table;

 public:
  void init(uns8 proc_id) override {
    this->proc_id = proc_id;
    prediction_table.clear();
  }
  void recover() override { return; }

  PredictorEntry* lookup(Op* op) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    auto it = prediction_table.find(op->inst_info->addr);
    if (it != prediction_table.end()) {
      it->second.found = true;
      return &(it->second);
    }
    static ConstantLoadAddrPredEntry not_found_entry;
    not_found_entry.found = false;
    return &not_found_entry;
  }

  void train(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    if (op->off_path)
      return;

    Addr pc = op->inst_info->addr;
    Addr va = op->oracle_info.va;

    auto* pred_entry = static_cast<ConstantLoadAddrPredEntry*>(entry);
    if (!pred_entry->is_found()) {
      prediction_table[pc] = ConstantLoadAddrPredEntry(va);
      return;
    }
    if (pred_entry->oracle_address == va) {
      pred_entry->confidence++;
    } else {
      pred_entry->oracle_address = va;
      pred_entry->confidence = 0;
    }
  }

  void infer(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    auto* pred_entry = static_cast<ConstantLoadAddrPredEntry*>(entry);
    if (!pred_entry->is_found())
      return;
    if (pred_entry->confidence <= CONST_LOAD_ADDR_PRED_THRESHOLD)
      return;

    load_pred_apply_addr(op, pred_entry->oracle_address);
  }
};

/**************************************************************************************/
/* Address Predictor: Stride
 *
 *   Predicts addr = last_addr + stride for loads that walk memory with a stable
 *   stride.  Confidence is gained when the observed stride repeats and reset when
 *   it changes.  Like the constant predictor, it only resolves early when the
 *   predicted line is present in the L1.
 */

struct StrideLoadAddrPredEntry : public PredictorEntry {
  Addr last_address;
  int64 stride;
  uns confidence;
  bool found;

  StrideLoadAddrPredEntry() : last_address(0), stride(0), confidence(0), found(false) {}
  explicit StrideLoadAddrPredEntry(Addr addr) : last_address(addr), stride(0), confidence(0), found(true) {}

  bool is_found() const override { return found; }
};

class StrideLoadAddrPredictor : public LoadPredictor {
 private:
  uns8 proc_id;
  std::unordered_map<Addr, StrideLoadAddrPredEntry> prediction_table;

 public:
  void init(uns8 proc_id) override {
    this->proc_id = proc_id;
    prediction_table.clear();
  }
  void recover() override { return; }

  PredictorEntry* lookup(Op* op) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    auto it = prediction_table.find(op->inst_info->addr);
    if (it != prediction_table.end()) {
      it->second.found = true;
      return &(it->second);
    }
    static StrideLoadAddrPredEntry not_found_entry;
    not_found_entry.found = false;
    return &not_found_entry;
  }

  void train(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    if (op->off_path)
      return;

    Addr pc = op->inst_info->addr;
    Addr va = op->oracle_info.va;

    auto* pred_entry = static_cast<StrideLoadAddrPredEntry*>(entry);
    if (!pred_entry->is_found()) {
      prediction_table[pc] = StrideLoadAddrPredEntry(va);
      return;
    }

    int64 observed_stride = (int64)va - (int64)pred_entry->last_address;
    if (observed_stride == pred_entry->stride) {
      pred_entry->confidence++;
    } else {
      pred_entry->stride = observed_stride;
      pred_entry->confidence = 0;
    }
    pred_entry->last_address = va;
  }

  void infer(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    auto* pred_entry = static_cast<StrideLoadAddrPredEntry*>(entry);
    if (!pred_entry->is_found())
      return;
    if (pred_entry->confidence <= LOAD_ADDR_PRED_STRIDE_THRESHOLD)
      return;

    Addr pred_addr = (Addr)((int64)pred_entry->last_address + pred_entry->stride);
    load_pred_apply_addr(op, pred_addr);
  }
};

/**************************************************************************************/
/* Per-core registry: one active scheme per category */

struct LoadPredictorSet {
  LoadPredictor* value_pred;
  LoadPredictor* addr_pred;
};

static std::vector<LoadPredictorSet> per_core_predictors;
static LoadPredictorSet* active = nullptr;

static LoadPredictor* make_value_predictor() {
  switch (LOAD_VALUE_PRED_SCHEME) {
    case LOAD_VALUE_PRED_SCHEME_NONE:
      return new NoneLoadPredictor();
    case LOAD_VALUE_PRED_SCHEME_LAST_VALUE:
      return new LastValuePredictor();
    default:
      ASSERT(0, 0);
      return new NoneLoadPredictor();
  }
}

static LoadPredictor* make_addr_predictor() {
  switch (LOAD_ADDR_PRED_SCHEME) {
    case LOAD_ADDR_PRED_SCHEME_NONE:
      return new NoneLoadPredictor();
    case LOAD_ADDR_PRED_SCHEME_CONST:
      return new ConstantLoadAddrPredictor();
    case LOAD_ADDR_PRED_SCHEME_STRIDE:
      return new StrideLoadAddrPredictor();
    default:
      ASSERT(0, 0);
      return new NoneLoadPredictor();
  }
}

}  // anonymous namespace

/**************************************************************************************/
/* Lifecycle */

void alloc_mem_load_predictors(uns num_cores) {
  ASSERT(0, LOAD_VALUE_PRED_SCHEME >= 0 && LOAD_VALUE_PRED_SCHEME < LOAD_VALUE_PRED_SCHEME_NUM);
  ASSERT(0, LOAD_ADDR_PRED_SCHEME >= 0 && LOAD_ADDR_PRED_SCHEME < LOAD_ADDR_PRED_SCHEME_NUM);

  for (uns ii = 0; ii < num_cores; ii++) {
    LoadPredictorSet set;
    set.value_pred = make_value_predictor();
    set.addr_pred = make_addr_predictor();
    per_core_predictors.push_back(set);
  }
}

void set_load_predictors(uns8 proc_id) {
  active = &per_core_predictors[proc_id];
}

void init_load_predictors(uns8 proc_id, const char* name) {
  active->value_pred->init(proc_id);
  active->addr_pred->init(proc_id);
}

void recover_load_predictors(void) {
  active->value_pred->recover();
  active->addr_pred->recover();
}

/**************************************************************************************/
/* Pipeline hook */

void load_pred_predict_op(Op* op) {
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;

  // Value category: predicts the loaded value.
  PredictorEntry* v_entry = active->value_pred->lookup(op);
  if (v_entry) {
    active->value_pred->infer(op, v_entry);
    active->value_pred->train(op, v_entry);
  }

  // Address category. Always train, but only apply its effect if the value
  // predictor did not already resolve this load (avoid double speculation).
  PredictorEntry* a_entry = active->addr_pred->lookup(op);
  if (a_entry) {
    if (!op->load_value_predicted)
      active->addr_pred->infer(op, a_entry);
    active->addr_pred->train(op, a_entry);
  }

  load_pred_collect_predict_stat(op);
}
