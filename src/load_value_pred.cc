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
 * Author       : Yinyuan Zhao
 * Date         : 10/2025
 * Description  : Load Address Predictor
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

#include "memory/memory.h"

#include "statistics.h"
#include "xed-interface.h"
}

#include <unordered_map>
#include <vector>

/**************************************************************************************/
/* Global Values */

/*
 * When a load is mispredicted, the ending op of its corresponding macro-instruction
 * is marked as the recovery op.
 * This flag is to records the last mispredicted load operation to associate it with
 * its end-of-macro (EOM) op.
 */
Flag* per_core_load_value_mispredict;

/**************************************************************************************/
/* Static Methods */

static inline void load_value_predictor_debug_print_op(Op* op, int state) {
  ASSERT(op->proc_id, op != NULL);

  Inst_Info* inst_info = op->inst_info;
  Table_Info* table_info = inst_info->table_info;
  uns16 op_code = inst_info->table_info->true_op_type;
  const char* op_str = xed_iclass_enum_t2str((xed_iclass_enum_t)op_code);

  printf("--- %d ---\n", state);
  printf("[OP: %lld]\n", op->op_num);
  printf(" - pc: 0x%llx, opcode: 0x%x(%s)\n", inst_info->addr, op_code, op_str);
  printf(" - off_path: %d, cf: %d, mem: %d\n", op->off_path, table_info->cf_type, table_info->mem_type);
  printf(" - bom: %d, eom: %d\n", op->bom, op->eom);
  printf(" - predicted: %d, recover: %d\n", op->load_value_predicted, op->oracle_info.recover_at_exec);

  printf(" - src#%d: <", inst_info->table_info->num_src_regs);
  for (uns ii = 0; ii < inst_info->table_info->num_src_regs; ii++)
    printf("%d, ", inst_info->srcs[ii].id);
  printf(">, dest#%d: <", inst_info->table_info->num_dest_regs);
  for (uns ii = 0; ii < inst_info->table_info->num_dest_regs; ii++)
    printf("%d, ", inst_info->dests[ii].id);
  printf(">\n");
}

static inline void load_value_predictor_sched_recovery(Op* op) {
  if (!op->eom || !per_core_load_value_mispredict[op->proc_id])
    return;

  per_core_load_value_mispredict[op->proc_id] = FALSE;
  op->load_value_flush = TRUE;

  const Addr pc_plus_offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size);
  op->oracle_info.dir = NOT_TAKEN;
  op->oracle_info.pred = NOT_TAKEN;
  op->oracle_info.misfetch = FALSE;
  op->oracle_info.mispred = FALSE;
  op->oracle_info.late_misfetch = FALSE;
  op->oracle_info.late_mispred = FALSE;
  op->oracle_info.btb_miss = FALSE;
  op->oracle_info.no_target = FALSE;
  op->oracle_info.pred_npc = pc_plus_offset;
  op->oracle_info.late_pred_npc = pc_plus_offset;
  op->oracle_info.recover_at_exec = TRUE;

  op->recovery_info.proc_id = op->proc_id;
  op->recovery_info.new_dir = op->oracle_info.dir;
  op->recovery_info.op_num = op->op_num;
  op->recovery_info.PC = op->inst_info->addr;
  op->recovery_info.cf_type = NOT_CF;
  op->recovery_info.oracle_dir = op->oracle_info.dir;
  op->recovery_info.branchTarget = op->oracle_info.target;
  op->recovery_info.predict_cycle = cycle_count;
}

static inline void load_value_predictor_process_predict_op(Op* op, Flag is_mispred) {
  ASSERT(op->proc_id, op->table_info->mem_type == MEM_LD);

  // set the metadata to indicate bypassing
  op->load_value_predicted = TRUE;
  op->decode_cycle = cycle_count;
  op->exec_cycle = cycle_count;

  if (is_mispred) {
    per_core_load_value_mispredict[op->proc_id] = TRUE;
    if (!op->off_path) {
      STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_LOADS_ON_PATH_MISPREDICTED);
    }
  }
}

static void load_value_predictor_collect_stat(Op* op) {
  ASSERT(op->proc_id, op->table_info->mem_type == MEM_LD);
  if (op->off_path) {
    return;
  }

  STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_LOADS_ON_PATH);
  if (op->load_value_predicted) {
    STAT_EVENT(op->proc_id, LOAD_VALUE_PREDICT_LOADS_ON_PATH_PREDICTED);
  }
}

/**************************************************************************************/
/* Definition */

class PredictorEntry {
 public:
  virtual ~PredictorEntry() = default;
  virtual bool is_found() const = 0;
};

class LoadValuePredictor {
 public:
  virtual ~LoadValuePredictor() = default;
  virtual void init(uns8 proc_id) = 0;
  virtual void recover() = 0;

  virtual PredictorEntry* lookup(Op* op) = 0;
  virtual void train(Op* op, PredictorEntry* entry) = 0;
  virtual void infer(Op* op, PredictorEntry* entry) = 0;
};

/**************************************************************************************/
/* Load Value Predictor None */

class NoneLoadValuePredictor : public LoadValuePredictor {
 public:
  void init(uns8 proc_id) override { return; }
  void recover() override { return; }

  PredictorEntry* lookup(Op* op) override { return nullptr; }
  void train(Op* op, PredictorEntry* entry) override { return; }
  void infer(Op* op, PredictorEntry* entry) override { return; }
};

/**************************************************************************************/
/*
 * Constant Load Address Predictor
 *    The constant load address predictor accelerates critical instruction chains by
 *    resolving loads with a constant address early. In particular, it trains loads
 *    that frequently utilize the same address and executes them at fetch.
 */

struct ConstantLoadAddrPredEntry : public PredictorEntry {
  Addr oracle_address;
  uns confidence;
  bool found;

  ConstantLoadAddrPredEntry() : oracle_address(0), confidence(0), found(false) {}
  ConstantLoadAddrPredEntry(Addr addr) : oracle_address(addr), confidence(0), found(true) {}

  bool is_found() const override { return found; }
};

class ConstantLoadAddrPredictor : public LoadValuePredictor {
 private:
  uns8 proc_id;
  std::unordered_map<Addr, ConstantLoadAddrPredEntry> prediction_table;

 public:
  void init(uns8 proc_id) override;
  void recover() override;

  PredictorEntry* lookup(Op* op) override;
  void train(Op* op, PredictorEntry* entry) override;
  void infer(Op* op, PredictorEntry* entry) override;
};

void ConstantLoadAddrPredictor::init(uns8 proc_id) {
  this->proc_id = proc_id;
  prediction_table.clear();
}

void ConstantLoadAddrPredictor::recover() {
  return;
}

PredictorEntry* ConstantLoadAddrPredictor::lookup(Op* op) {
  ASSERT(op->proc_id, op->table_info->mem_type == MEM_LD);
  Addr pc = op->inst_info->addr;

  auto it = prediction_table.find(pc);
  if (it != prediction_table.end()) {
    it->second.found = true;
    return &(it->second);
  }

  // Return a new entry that represents "not found"
  static ConstantLoadAddrPredEntry not_found_entry;
  not_found_entry.found = false;
  return &not_found_entry;
}

void ConstantLoadAddrPredictor::train(Op* op, PredictorEntry* entry) {
  // If this PC has not been seen before
  //   → create a new prediction entry with current VA
  //   → store it in the prediction table
  //   → return

  // If predicted address matches actual address
  //   → increase confidence counter
  // Else
  //   → update stored address to new VA
  //   → reset confidence to zero
}

void ConstantLoadAddrPredictor::infer(Op* op, PredictorEntry* entry) {
  // If no valid entry exists for this PC
  //   → return (no prediction can be made)

  // Check confidence level
  // If confidence is below threshold
  //   → return (not confident enough to predict)

  // Attempt to access L1 cache using predicted address
  // l1_data = do_l1_access_addr(pred_addr)
  // If access fails (no cache data)
  //   → return (prediction not usable)

  // Flag is_mispred = (pred_addr != op->oracle_info.va);
  // load_value_predictor_process_predict_op(op, is_mispred);
}

/**************************************************************************************/
/* Global Values */

static std::vector<LoadValuePredictor*> per_core_load_value_pred;
static LoadValuePredictor* predictor = nullptr;

/**************************************************************************************/
/* External Vanilla Model Func */

void alloc_mem_load_value_predictor(uns num_cores) {
  ASSERT(0, LOAD_VALUE_PRED_SCHEME >= 0 && LOAD_VALUE_PRED_SCHEME < LOAD_VALUE_PRED_SCHEME_NUM);
  per_core_load_value_mispredict = (Flag*)malloc(num_cores * sizeof(Flag));

  for (uns ii = 0; ii < num_cores; ii++) {
    per_core_load_value_mispredict[ii] = FALSE;

    switch (LOAD_VALUE_PRED_SCHEME) {
      case LOAD_VALUE_PRED_SCHEME_NONE:
        per_core_load_value_pred.push_back(new NoneLoadValuePredictor());
        break;

      case LOAD_VALUE_PRED_SCHEME_CONST_ADDR_PRED:
        per_core_load_value_pred.push_back(new ConstantLoadAddrPredictor());
        break;

      default:
        ASSERT(0, 0);
        break;
    }
  }
}

void set_load_value_predictor(uns8 proc_id) {
  predictor = per_core_load_value_pred[proc_id];
}

void init_load_value_predictor(uns8 proc_id, const char* name) {
  predictor->init(proc_id);
}

void recover_load_value_predictor() {
  predictor->recover();
}

/**************************************************************************************/
/* External Methods */

void load_value_predictor_predict_op(Op* op) {
  if (op->table_info->mem_type == MEM_LD) {
    PredictorEntry* entry = predictor->lookup(op);
    predictor->infer(op, entry);
    predictor->train(op, entry);
    load_value_predictor_collect_stat(op);
  }

  load_value_predictor_sched_recovery(op);
}
