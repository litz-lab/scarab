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
 *     - a shared squash/recovery helper (load_pred_schedule_squash)
 *     - LoadPredictor       : fetch-time interface for value & address predictors
 *     - concrete predictors : last-value (scaffold), constant/stride address
 *     - a per-core registry holding one active scheme per category
 *
 *   The recovery has no cross-op global state: a wrong prediction is recorded on
 *   the op (load_value_mispredicted) and the FT builder walks the macro's uops to
 *   stamp the squash on the EOM (see FT::predict_ft in ft.cc).
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
#include "memory/memory.h"
#include "memory/memory.param.h"

#include "bp/bp.h"
#include "statistics.h"
}

#include <unordered_map>
#include <vector>

/**************************************************************************************/
/* Shared pipeline effect / recovery helpers */

/*
 * Read the load's architectural result (the loaded value).  Ops carry their
 * destination register values in op->dst_val[], populated at uop generation, so
 * this is available by the time predict_ft runs.  A load writes a single
 * destination; dst_val[0] holds the loaded value.  Returns FALSE for the (rare)
 * load form with no destination register, in which case the value predictor
 * simply does not speculate.
 */
static inline Flag load_pred_read_dest_value(Op* op, uns64* value) {
  if (op->inst_info->table_info.num_dest_regs < 1)
    return FALSE;
  *value = op->dst_val[0];
  return TRUE;
}

/*
 * Route a squash through the existing branch-recovery path so a mispredicted
 * load re-issues.  The op is set up as a non-CF, on-path recovery at exec:
 * exec_stage_bp_resolve() sees bp_pred_info->recover_at_exec and calls
 * bp_sched_recovery(), which resteers to op->oracle_info.npc (the true
 * fall-through) - i.e. re-fetch the same correct path with the load resolved.
 * recovery_info.cf_type = NOT_CF suppresses branch-predictor state recovery in
 * bp_recover_op().  Must be called on the macro's EOM op (bp_sched_recovery
 * squashes ops younger than it), so sibling uops are not skipped on the resteer.
 */
void load_pred_schedule_squash(Op* op) {
  op->load_value_flush = TRUE;

  // On-path non-CF ops select bp_pred_main during predict_ft; set both levels so
  // the exec-stage recovery check fires regardless of which is selected.
  for (Bp_Pred_Info* bp : {&op->bp_pred_main, &op->bp_pred_l0}) {
    bp->recover_at_fe = FALSE;
    bp->recover_at_decode = FALSE;
    bp->recover_at_exec = TRUE;
  }

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
 * Value-prediction verification at load completion.  Called from the dcache stage
 * when a load finishes (its real value is now known).  A value predictor verifies
 * at completion rather than at AGEN, so if the predicted value was wrong the
 * squash is scheduled here, at the load's done_cycle - modelling that the
 * misprediction is only detected once the load returns its data.  The recovery is
 * stamped on the load itself and fires immediately (bp_sched_recovery), flushing
 * the speculatively-woken consumers so they re-execute with the correct value.
 */
void load_pred_verify_at_completion(Op* op) {
  if (!op->load_value_mispredicted || !op->load_pred_verify_at_done || op->off_path)
    return;

  op->load_value_mispredicted = FALSE;

  // Stamp the squash on the macro's EOM (recorded at FT in ft.cc), not on this
  // possibly mid-macro load uop. For single-uop loads the EOM is the load
  // itself. The EOM is a younger sibling on the same path, so it is still live
  // (not yet retired) at the load's completion.
  Op* eom = op->load_pred_squash_op;
  ASSERT(op->proc_id, eom && eom->op_pool_valid && eom->unique_num == op->load_pred_squash_unique);

  load_pred_schedule_squash(eom);
  bp_sched_recovery(bp_recovery_info, eom, op->done_cycle);
  eom->recovery_scheduled = TRUE;
}

/*
 * "Early-result" effect: the load's result is made available to consumers before
 * the load itself finishes, so they may wake early (honored in op_sources_add,
 * which also applies ready_cycle as a wake-time floor).  Used by:
 *   - value prediction: the value is predicted, available immediately
 *     (ready_cycle = now), and verified against the real value at load
 *     completion (verify_at_done = TRUE);
 *   - RFP: the value is prefetched L1->register-file, so consumers wake after the
 *     prefetch latency (ready_cycle = now + DCACHE_CYCLES); the predicted address
 *     is verified at AGEN (verify_at_done = FALSE).
 * The load still flows through the pipeline and accesses the dcache normally, so
 * its bandwidth/port contention is modeled; only its consumers are accelerated.
 * A wrong prediction is recorded on the op and squashed later (ft.cc for AGEN-
 * verified modes; dcache completion for value's verify-at-done).
 */
static inline void load_pred_apply_early_result(Op* op, Flag is_mispred, Counter ready_cycle, Flag verify_at_done) {
  ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);

  op->load_value_predicted = TRUE;
  op->load_pred_ready_cycle = ready_cycle;
  op->load_pred_verify_at_done = verify_at_done;

  if (is_mispred) {
    op->load_value_mispredicted = TRUE;
    if (!op->off_path) {
      STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_LOADS_ON_PATH_MISPREDICTED);
    }
  }
}

/*
 * "Early-AGEN" effect (address prediction): only the *address* is known early,
 * not the value.  The load is allowed to issue without waiting for its
 * address-operand (REG_DATA_DEP) registers and to access the dcache at the
 * predicted address (op_sources_add honors load_addr_predicted; dcache_stage
 * uses load_pred_addr).  It then incurs the normal hit/miss latency and wakes its
 * consumers at its real completion - unlike wake-now, consumers are NOT resolved
 * at fetch.  The predicted address is verified against the true VA at exec; a
 * mismatch squashes via the macro's EOM (as for value/RFP mispredicts).
 */
static inline void load_pred_apply_early_agen(Op* op, Addr pred_addr, Flag is_mispred) {
  ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);

  op->load_addr_predicted = TRUE;
  op->load_pred_addr = pred_addr;

  if (is_mispred) {
    op->load_value_mispredicted = TRUE;
    if (!op->off_path) {
      STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_LOADS_ON_PATH_MISPREDICTED);
    }
  }
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
    load_pred_apply_early_result(op, is_mispred, cycle_count + DCACHE_CYCLES, /*verify_at_done=*/FALSE);
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
  std::unordered_map<Addr, LastValuePredEntry> prediction_table;

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
    static LastValuePredEntry not_found_entry;
    not_found_entry.found = false;
    return &not_found_entry;
  }

  void infer(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    auto* pred_entry = static_cast<LastValuePredEntry*>(entry);
    if (!pred_entry->is_found())
      return;
    if (pred_entry->confidence <= LOAD_VALUE_PRED_THRESHOLD)
      return;

    // SEAM: requires the load's true destination value to verify the prediction.
    uns64 actual_value = 0;
    if (!load_pred_read_dest_value(op, &actual_value))
      return;  // values unavailable -> never speculate (sound no-op today)

    Flag is_mispred = (pred_entry->last_value != actual_value);
    // value known now; verified against the real value at load completion.
    load_pred_apply_early_result(op, is_mispred, cycle_count, /*verify_at_done=*/TRUE);
  }

  void train(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->inst_info->table_info.mem_type == MEM_LD);
    if (op->off_path)
      return;

    // SEAM: requires the load's true destination value to update the table.
    uns64 actual_value = 0;
    if (!load_pred_read_dest_value(op, &actual_value))
      return;

    auto* pred_entry = static_cast<LastValuePredEntry*>(entry);
    if (!pred_entry->is_found()) {
      prediction_table[op->inst_info->addr] = LastValuePredEntry(actual_value);
      return;
    }
    if (pred_entry->last_value == actual_value) {
      pred_entry->confidence++;
    } else {
      pred_entry->last_value = actual_value;
      pred_entry->confidence = 0;
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
