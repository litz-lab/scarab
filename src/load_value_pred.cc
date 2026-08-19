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
 *   The recovery has no cross-op global state: on a wrong on-path prediction the
 *   frontend marks recover_at_exec on the mispredict's trigger uop; it then recovers
 *   at exec (op_set_exec_cycle -> predicted_load_schedule_recovery, which resolves
 *   the macro EOM via op_inst_eom, fills its recovery_info, and fires
 *   bp_sched_recovery on the EOM).
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
#include "general.param.h"
#include "memory/memory.param.h"

#include "bp/bp.h"
#include "memory/memory.h"

#include "statistics.h"
}

#include <memory>
#include <unordered_map>
#include <vector>

#include "isa/isa.h"

#include "frontend/frontend_intf.h"

#include "ft.h"

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
  if (dst_idx >= op->uop->num_dest_regs)
    return FALSE;
  *value = op->dst_val[dst_idx];
  return TRUE;
}

// Value-predict only architectural GP integer regs (RAX..R15), never the stack pointer; excludes
// flags, segment, RIP, and vector regs (all outside RAX..R15).
static inline Flag reg_is_value_predictable(uns reg_id) {
  return reg_id >= REG_RAX && reg_id <= REG_R15 && reg_id != REG_RSP;
}

/*
 * Mark a mispredicted (on-path) predicted load so it recovers at exec. The flag
 * lives on the load uop itself (any op may carry it); op_set_exec_cycle() fires
 * the recovery when the load's exec_cycle is set. bp_pred_main is the level
 * selected for on-path non-CF loads.
 */
void load_pred_mark_recovery(Op* op) {
  ASSERT(op->proc_id, !op->off_path);
  STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_LOADS_ON_PATH_MISPREDICTED);

  op_select_bp_pred_info(op, BP_PRED_MAIN);
  // addr-pred load recovers at operand wake; value/RFP-pred at exec.
  op->bp_pred_info->recovery_point = op->load_addr_predicted ? RECOVER_AT_WAKE : RECOVER_AT_EXEC;
}

/*
 * Fire the mispredicted load's squash at exec (from op_set_exec_cycle). Targets the macro's
 * END-OF-MACRO op (found via op_inst_eom) so bp_sched_recovery squashes the speculatively
 * woken consumers after the macro while the load's own uops survive. recovery_info is a NOT_CF
 * fall-through resteer, so bp_recover_op callers skip it and branch-predictor state is untouched.
 */
void predicted_load_schedule_recovery(Op* op, Counter recovery_cycle) {
  ASSERT(op->proc_id, !op->off_path);

  Op* eom = op_inst_eom(op);
  ASSERT(op->proc_id, eom);

  // op and eom are uops of the same macro, so all static inst_info (PC, size, cf_type) is read off
  // op; only eom's dynamic fields (op_num, oracle_info) and its role as the recovery target need the
  // sibling lookup.
  ASSERT(op->proc_id, op->uop->cf_type == NOT_CF);
  ASSERT(op->proc_id, eom->oracle_info.npc == ADDR_PLUS_OFFSET(op->inst->addr, op->inst->inst_size));

  op_select_bp_pred_info(eom, BP_PRED_MAIN);
  eom->recovery_info.proc_id = eom->proc_id;
  eom->recovery_info.op = eom;
  eom->recovery_info.op_num = eom->op_num;
  eom->recovery_info.PC = op->inst->addr;
  eom->recovery_info.cf_type = NOT_CF;
  eom->recovery_info.oracle_dir = eom->oracle_info.dir;
  eom->recovery_info.new_dir = eom->oracle_info.dir;
  eom->recovery_info.branchTarget = eom->oracle_info.target;
  eom->recovery_info.predict_cycle = cycle_count;

  bp_sched_recovery(bp_recovery_info, eom, recovery_cycle);
}

// At a value-predicted load's exec, count the cycles its value was available early (exec - predicted).
void load_pred_account_saved_cycles(Op* op) {
  Counter vp = op_get_value_predicted_cycle(op);
  Counter ex = op_get_exec_cycle(op);
  if (ex > vp)
    INC_STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_SAVED_CYCLES_ON_PATH, ex - vp);
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
 * The caller decides mispredict and sets up the exec recovery on the macro's EOM.
 */
static inline void load_pred_apply_early_result(Op* op, Counter ready_delay) {
  ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);

  op->load_value_predicted = TRUE;
  op->load_pred_ready_delay = ready_delay;
  if (ready_delay == 0)  // value prediction: value usable to consumers now; stamp for saved-cycles
    op_set_value_predicted_cycle(op, cycle_count);
}

/*
 * "Early-AGEN" effect (address prediction): only the *address* is known early,
 * not the value.  The load is allowed to issue without waiting for its
 * address-operand (REG_DATA_DEP) registers and to access the dcache at the
 * predicted address (op_sources_add honors load_addr_predicted; dcache_stage
 * uses load_pred_addr).  It then incurs the normal hit/miss latency and wakes its
 * consumers at its real completion - unlike wake-now, consumers are NOT resolved
 * at fetch.  Returns whether the predicted address was wrong (recovers at exec,
 * as for value/RFP mispredicts).
 */
static inline Flag load_pred_apply_early_agen(Op* op, Addr pred_addr, Flag is_mispred) {
  ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);

  op->load_addr_predicted = TRUE;
  op->load_pred_addr = pred_addr;
  // Bind bp_pred_info so the operand-wake path can read recover_at_exec through the selected level
  // (mark_recovery sets it only on a mispredict; a correct prediction otherwise leaves it unset).
  op_select_bp_pred_info(op, BP_PRED_MAIN);
  return is_mispred;
}

/*
 * Apply the configured address-prediction effect for a confident prediction.
 * Shared by all address predictor schemes (constant, stride, ...); the algorithm
 * only produces pred_addr, this decides how it acts on the pipeline.  Returns
 * whether the prediction was wrong.
 */
static inline Flag load_pred_apply_addr(Op* op, Addr pred_addr) {
  Flag is_mispred = (pred_addr != op->oracle_info.va);

  if (LOAD_ADDR_PRED_MODE == LOAD_ADDR_PRED_MODE_RFP) {
    // RFP prefetches the value L1->register file. Model it only when the
    // predicted line is L1-resident (bounded prefetch latency); consumers then
    // wake after the L1->RF transfer (now + DCACHE_CYCLES), NOT immediately. The
    // predicted address is verified at AGEN.
    if (!do_l1_access_addr(pred_addr))
      return FALSE;
    load_pred_apply_early_result(op, /*ready_delay=*/DCACHE_CYCLES);
    return is_mispred;
  }
  // EARLY_AGEN: issue early and access the predicted address in the dcache
  // stage, which handles hit and miss (mem req) with normal latency.
  return load_pred_apply_early_agen(op, pred_addr, is_mispred);
}

static void load_pred_collect_predict_stat(Op* op) {
  ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
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
  // Returns TRUE if this predictor speculated and the prediction is wrong.
  virtual Flag infer(Op* op, PredictorEntry* entry) = 0;
};

/* None predictor (category disabled). */
class NoneLoadPredictor : public LoadPredictor {
 public:
  void init(uns8 proc_id) override { return; }
  void recover() override { return; }
  PredictorEntry* lookup(Op* op) override { return nullptr; }
  void train(Op* op, PredictorEntry* entry) override { return; }
  Flag infer(Op* op, PredictorEntry* entry) override { return FALSE; }
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
  uns8 proc_id = 0;
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
    ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
    static LastValuePredEntry sentinel;
    return &sentinel;
  }

  // Speculate only if EVERY destination is trained and confident; a wrong
  // prediction on any destination makes the load a mispredict.  Reads each
  // destination's true value (op->dst_val[i]) to decide correctness.
  Flag infer(Op* op, PredictorEntry* /*entry*/) override {
    ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
    uns ndest = op->uop->num_dest_regs;
    if (ndest < 1)
      return FALSE;

    Flag any_mispred = FALSE;
    for (uns i = 0; i < ndest; i++) {
      if (!reg_is_value_predictable(op->uop->dests[i].id))
        return FALSE;  // only architectural GP value regs (no RSP/flags/segment/vector)
      uns64 actual_value = 0;
      if (!load_pred_read_dest_value(op, i, &actual_value))
        return FALSE;  // value unavailable -> do not speculate
      auto it = prediction_table.find(dest_key(op->inst->addr, i));
      if (it == prediction_table.end() || it->second.confidence <= LOAD_VALUE_PRED_THRESHOLD)
        return FALSE;  // not trained / not confident -> do not speculate
      if (it->second.last_value != actual_value)
        any_mispred = TRUE;
    }

    // value known now; verified against the real value at load completion.
    load_pred_apply_early_result(op, /*ready_delay=*/0);
    return any_mispred;
  }

  void train(Op* op, PredictorEntry* /*entry*/) override {
    ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
    if (op->off_path)
      return;

    for (uns i = 0; i < op->uop->num_dest_regs; i++) {
      uns64 actual_value = 0;
      if (!load_pred_read_dest_value(op, i, &actual_value))
        continue;
      uint64_t key = dest_key(op->inst->addr, i);
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
  uns8 proc_id = 0;
  std::unordered_map<Addr, ConstantLoadAddrPredEntry> prediction_table;

 public:
  void init(uns8 proc_id) override {
    this->proc_id = proc_id;
    prediction_table.clear();
  }
  void recover() override { return; }

  PredictorEntry* lookup(Op* op) override {
    ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
    auto it = prediction_table.find(op->inst->addr);
    if (it != prediction_table.end()) {
      it->second.found = true;
      return &(it->second);
    }
    static ConstantLoadAddrPredEntry not_found_entry;
    not_found_entry.found = false;
    return &not_found_entry;
  }

  void train(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
    if (op->off_path)
      return;

    Addr pc = op->inst->addr;
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

  Flag infer(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
    auto* pred_entry = static_cast<ConstantLoadAddrPredEntry*>(entry);
    if (!pred_entry->is_found())
      return FALSE;
    if (pred_entry->confidence <= CONST_LOAD_ADDR_PRED_THRESHOLD)
      return FALSE;

    return load_pred_apply_addr(op, pred_entry->oracle_address);
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
  uns8 proc_id = 0;
  std::unordered_map<Addr, StrideLoadAddrPredEntry> prediction_table;

 public:
  void init(uns8 proc_id) override {
    this->proc_id = proc_id;
    prediction_table.clear();
  }
  void recover() override { return; }

  PredictorEntry* lookup(Op* op) override {
    ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
    auto it = prediction_table.find(op->inst->addr);
    if (it != prediction_table.end()) {
      it->second.found = true;
      return &(it->second);
    }
    static StrideLoadAddrPredEntry not_found_entry;
    not_found_entry.found = false;
    return &not_found_entry;
  }

  void train(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
    if (op->off_path)
      return;

    Addr pc = op->inst->addr;
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

  Flag infer(Op* op, PredictorEntry* entry) override {
    ASSERT(op->proc_id, op->uop->mem_type == MEM_LD);
    auto* pred_entry = static_cast<StrideLoadAddrPredEntry*>(entry);
    if (!pred_entry->is_found())
      return FALSE;
    if (pred_entry->confidence <= LOAD_ADDR_PRED_STRIDE_THRESHOLD)
      return FALSE;

    Addr pred_addr = (Addr)((int64)pred_entry->last_address + pred_entry->stride);
    return load_pred_apply_addr(op, pred_addr);
  }
};

/**************************************************************************************/
/* Per-core registry: one active scheme per category */

struct LoadPredictorSet {
  std::unique_ptr<LoadPredictor> value_pred;
  std::unique_ptr<LoadPredictor> addr_pred;
};

static std::vector<LoadPredictorSet> per_core_predictors;
static LoadPredictorSet* active = nullptr;

static std::unique_ptr<LoadPredictor> make_value_predictor() {
  // Value prediction reads the load's produced value (op->dst_val), which is only supplied by the
  // exec-driven PIN frontend; trace/memtrace frontends do not carry destination register values.
  ASSERTM(0, LOAD_VALUE_PRED_SCHEME == LOAD_VALUE_PRED_SCHEME_NONE || FRONTEND == FE_PIN_EXEC_DRIVEN,
          "LOAD_VALUE_PRED_SCHEME requires --frontend pin_exec_driven (load values unavailable otherwise)\n");
  switch (LOAD_VALUE_PRED_SCHEME) {
    case LOAD_VALUE_PRED_SCHEME_NONE:
      return std::make_unique<NoneLoadPredictor>();
    case LOAD_VALUE_PRED_SCHEME_LAST_VALUE:
      return std::make_unique<LastValuePredictor>();
    default:
      ASSERT(0, 0);
      return std::make_unique<NoneLoadPredictor>();
  }
}

static std::unique_ptr<LoadPredictor> make_addr_predictor() {
  switch (LOAD_ADDR_PRED_SCHEME) {
    case LOAD_ADDR_PRED_SCHEME_NONE:
      return std::make_unique<NoneLoadPredictor>();
    case LOAD_ADDR_PRED_SCHEME_CONST:
      return std::make_unique<ConstantLoadAddrPredictor>();
    case LOAD_ADDR_PRED_SCHEME_STRIDE:
      return std::make_unique<StrideLoadAddrPredictor>();
    default:
      ASSERT(0, 0);
      return std::make_unique<NoneLoadPredictor>();
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
    per_core_predictors.push_back(std::move(set));
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

// Returns TRUE if a category speculated on this load and its prediction is wrong;
// the decoupled frontend then sets up the exec recovery on the macro's EOM.
Flag load_pred_predict_op(Op* op) {
  if (op->uop->mem_type != MEM_LD)
    return FALSE;

  Flag is_mispred = FALSE;

  // Value category: predicts the loaded value.
  PredictorEntry* v_entry = active->value_pred->lookup(op);
  if (v_entry) {
    is_mispred |= active->value_pred->infer(op, v_entry);
    active->value_pred->train(op, v_entry);
  }

  // Address category. Always train, but only apply its effect if the value
  // predictor did not already resolve this load (avoid double speculation).
  PredictorEntry* a_entry = active->addr_pred->lookup(op);
  if (a_entry) {
    if (!op->load_value_predicted)
      is_mispred |= active->addr_pred->infer(op, a_entry);
    active->addr_pred->train(op, a_entry);
  }

  load_pred_collect_predict_stat(op);
  return is_mispred;
}
