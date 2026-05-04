/* Copyright 2024 Litz Lab
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
 * File         : ft.cc
 * Author       : Mingsheng Xu, Yuanpeng Liao
 * Date         :
 * Description  : Fetch Target (FT) class implementation
 ***************************************************************************************/

#include "ft.h"

#include <cstdio>
#include <functional>
#include <iostream>

#include "globals/assert.h"
#include "globals/utils.h"

#include "bp/bp.param.h"
#include "memory/memory.param.h"

extern "C" {
#include "bp/bp_targ_mech.h"
#include "frontend/frontend.h"
}
#include "frontend/frontend_intf.h"
#include "isa/isa_macros.h"

#include "decoupled_frontend.h"
#include "op_pool.h"
#include "uop_cache.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

uint64_t FT_id_counter = 0;

static inline const Bp_Pred_Info* ft_active_or_main_bp_pred_info(const Op* op) {
  return op->bp_pred_info ? op->bp_pred_info : &op->bp_pred_main;
}

/* FT member functions */
FT::~FT() {
  ASSERT(proc_id, bp_id || !ops.empty());
  for (auto ft_op : ops) {
    if (!ft_op->parent_FT_off_path || ft_op->off_path) {
      ft_op->parent_FT = nullptr;
      free_op(ft_op);
    }
    if (ft_op->parent_FT_off_path) {
      ASSERT(proc_id, !ft_op->off_path);
      ASSERT(proc_id, ft_op->parent_FT_off_path == this);
      ft_op->parent_FT_off_path = nullptr;
    }
  }
}

FT::FT(uns _proc_id, uns _bp_id) : proc_id(_proc_id), bp_id(_bp_id) {
  ft_info.dynamic_info.FT_id = FT_id_counter++;
}

bool FT::can_fetch_op() {
  return op_pos < ops.size();
}

Op* FT::fetch_op() {
  ASSERT(proc_id, can_fetch_op());
  Op* op = ops[op_pos];
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->op_pool_valid);
  ASSERT(proc_id, op->inst_info);
  op_pos++;

  DEBUG(proc_id, "Fetch op from FT fetch_addr0x:%llx off_path:%i op_num:%llu\n", op->inst_info->addr, op->off_path,
        op->op_num);
  return op;
}

void FT::add_op(Op* op) {
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->op_pool_valid);
  ASSERT(proc_id, op->inst_info);
  if (!ops.empty()) {
    ASSERT(proc_id, ops.back());
    ASSERT(proc_id, ops.back()->op_pool_valid);
    ASSERT(proc_id, ops.back()->inst_info);
    if (op->bom) {
      // assert consecutivity
      DEBUG(proc_id, "back addr + size %llx fetch addr %llx\n",
            ops.back()->inst_info->addr + ops.back()->inst_info->trace_info.inst_size, op->inst_info->addr);
      ASSERT(proc_id, ops.back()->inst_info->addr + ops.back()->inst_info->trace_info.inst_size == op->inst_info->addr);
    } else {
      // assert all uops of the same inst share the same addr
      ASSERT(proc_id, ops.back()->inst_info->addr == op->inst_info->addr);
    }
  }
  op->parent_FT = this;

  ops.emplace_back(op);
}

void FT::trim_unread_tail(const std::function<bool(Op*)>& should_remove) {
  ASSERT(proc_id, op_pos <= ops.size());
  while (ops.size() > op_pos) {
    Op* op = ops.back();
    if (!should_remove(op))
      break;
    ops.pop_back();
    op->parent_FT = nullptr;
    op->parent_FT_off_path = nullptr;
    free_op(op);
  }
}

/* remove some pre-built ops after recovery in execution-driven (PIN) mode.
 * in PIN execution-driven mode, after recovery, some ops may have been already saved as
 * recovery FT so we delete those ops and refetch them to rebuild the FT.
 */
void FT::remove_op_after_exec_recover() {
  ASSERT(proc_id, FRONTEND == FE_PIN_EXEC_DRIVEN);
  trim_unread_tail([](Op*) { return true; });
  ASSERT(proc_id, get_end_reason() == FT_NOT_ENDED || get_end_reason() == FT_TAKEN_BRANCH);
}

FT_Event FT::build(std::function<bool(uns8, uns8)> can_fetch_op_fn, std::function<bool(uns8, uns8, Op*)> fetch_op_fn,
                   bool off_path, bool conf_off_path, std::function<uint64_t()> get_next_op_id_fn) {
  FT_Event event = FT_EVENT_NONE;
  do {
    if (!can_fetch_op_fn(proc_id, bp_id)) {
      std::cout << "Warning could not fetch inst from frontend" << std::endl;
      delete this;
      return FT_EVENT_BUILD_FAIL;
    }
    Op* op = alloc_op(proc_id);
    fetch_op_fn(proc_id, bp_id, op);
    op->off_path = off_path;
    op->conf_off_path = conf_off_path;
    collect_op_stats(op);
    op->op_num = get_next_op_id_fn();
    op->bp_pred_main.pred_npc = op->oracle_info.npc;
    op->bp_pred_main.pred = op->oracle_info.dir;  // for prebuilt, pred is same as dir
    add_op(op);
    if (off_path) {
      bp_predict_btb(g_bp_data, op);
      if (bp_l0_enabled())
        predict_op_ft_event(op, BP_PRED_L0);
      event = predict_op_ft_event(op, BP_PRED_MAIN);
      op_select_bp_pred_info(op, BP_PRED_MAIN);
      if (op->inst_info->table_info.cf_type)
        g_bp_data->prev_cf_pred = op->bp_pred_info->pred;  // for next BTB access
    }
    if (op->inst_info->fake_inst == 1) {
      ft_info.dynamic_info.contains_fake_nop = TRUE;
    }
    if ((event == FT_EVENT_MISPREDICT || event == FT_EVENT_FETCH_BARRIER) && off_path) {
      generate_ft_info();
      return event;
    }
    STAT_EVENT(proc_id, FTQ_FETCHED_INS_ONPATH + off_path);
  } while (get_end_reason() == FT_NOT_ENDED);

  generate_ft_info();

  return event;
}

// will extract ops from 0 to index and form a new FT as off-path FT,
// the original FT have op_pos moved and modify ft_info to truncated version
// returns off_path and original FT
/***************************************************************************************
 * redirect_to_off_path() Cases Documentation
 *
 * This function handles branch misprediction by transitioning to off-path execution.
 * The behavior depends on where the misprediction occurs and the current (off path) FT state.
 *
 *
 * Code Flow:
 * 1. Extract on-path op from current FT at misprediction index as the current (off path) FT
 * 2. Set up recovery FT (either use trailing_ft or build new one)
 * 3. Transition to off-path state and redirect frontend
 * 4. Continue building off-path FT if needed (cases 2)
 *
 * - split_last:      Misprediction occurred at the last operation of the FT
 * - ft_ended:        splitted front part FT was already terminated before misprediction
 * - generate off FT: Need to building/padding the current off-path FT
 * - trailing_ft:     Remaining on-path operations after misprediction point
 *
 *
 * +------+------------+----------+-----------------+----------------------------------+
 * | Case | Split Last | FT Ended | Generate Off FT | Description                      |
 * +------+------------+----------+-----------------+----------------------------------+
 * |  1   |    Yes     |   Yes    |       No        | Mispred branch at end of line    |
 * |      |            |          |                 | - Misprediction at last op       |
 * |      |            |          |                 | - FT already ended               |
 * |      |            |          |                 | - Build New recovery FT          |
 * +------+------------+----------+-----------------+----------------------------------+
 * |  2   |    Yes     |   No     |      Yes        | Last op mispred not-taken        |
 * |      |            |          |                 | - Misprediction at last op       |
 * |      |            |          |                 | - FT not ended (pred not-taken)  |
 * |      |            |          |                 | - Need to pad/continue off-path  |
 * |      |            |          |                 | - Build New recovery FT          |
 * +------+------------+----------+-----------------+----------------------------------+
 * |  3   |    No      |   Yes    |      Yes        | Mispred in middle as taken       |
 * |      |            |          |                 | - Misprediction in middle of FT  |
 * |      |            |          |                 | - FT ends (pred taken branch)    |
 * |      |            |          |                 | - needs new off-path FT          |
 * |      |            |          |                 | - No need to pad off-path        |
 * |      |            |          |                 | - Use trailing_ft for recovery   |
 * +------+------------+----------+-----------------+----------------------------------+
 * |  4   |    No      |   No     |      Yes        | Mispred in middle not taken (btb)|
 * |      |            |          |                 | - btb miss result in mispred     |
 * |      |            |          |                 | - Misprediction in middle of FT  |
 * |      |            |          |                 | - FT not end                     |
 * |      |            |          |                 | - Need to pad off-path           |
 * |      |            |          |                 | - Use trailing_ft for recovery   |
 * +------+------------+----------+-----------------+----------------------------------+
 ***************************************************************************************/

std::pair<FT*, FT*> FT::extract_off_path_ft(uns split_index) {
  uns index_uns = static_cast<uns>(split_index);
  ASSERT(proc_id, index_uns < ops.size() && index_uns >= 0);
  ASSERT(proc_id, get_is_prebuilt());

  // if split at the last op and FT already ended, no need to create new FT, just update end condition
  if (split_index == ops.size() - 1 && get_end_reason() != FT_NOT_ENDED) {
    generate_ft_info();
    return {this, nullptr};
  }
  // Initialize off-path FT that will contain off-path ops after split position
  FT* off_path_ft = new FT(proc_id, bp_id);

  bool has_trailing_ops = (index_uns + 1 < ops.size());

  for (uns i = op_pos; i <= split_index; i++) {
    off_path_ft->ops.push_back(ops[i]);
    ops[i]->parent_FT_off_path = off_path_ft;
  }

  ASSERT(proc_id, off_path_ft->ops.size() == index_uns + 1 - op_pos);
  op_pos = index_uns + 1;
  if (has_trailing_ops) {
    // Inline move_over_ft logic
    ASSERT(0, (index_uns + 1 <= ops.size() - 1 && ops.size() - 1 < ops.size() && index_uns + 1 >= 0));

    generate_ft_info();

    ASSERT(proc_id, ft_info.static_info.start && ft_info.static_info.length && ops.size());
    ASSERT(proc_id, ops.size() == (ops.size() - ops.size()) + ops.size());
    ASSERT(proc_id, ft_info.dynamic_info.first_op_off_path == 0);
    ASSERT(proc_id, ft_info.dynamic_info.ended_by != FT_NOT_ENDED);
    ASSERT(proc_id, get_end_reason() != FT_NOT_ENDED);
  } else {
    off_path_ft->is_prebuilt = 1;
    this->is_prebuilt = 0;
  }

  ASSERT(proc_id, this->is_prebuilt || off_path_ft->is_prebuilt);
  ASSERT(proc_id, !(this->is_prebuilt && off_path_ft->is_prebuilt));

  off_path_ft->generate_ft_info();

  return {off_path_ft, this};
}

FT_Event FT::predict_op_ft_event(Op* op, Bp_Pred_Level pred_level) {
  Bp_Pred_Info* bp_pred_info = (pred_level == BP_PRED_L0) ? &op->bp_pred_l0 : &op->bp_pred_main;
  bool trace_mode = false;

#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  if (op->inst_info->table_info.cf_type) {
    ASSERT(proc_id, op->eom);
    bp_predict_op(g_bp_data, op, op->parent_FT->bp_id, 1, op->inst_info->addr, pred_level);
    const Addr pc_plus_offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size);

    DEBUG(proc_id,
          "[DFE%u] Predict CF fetch_addr:%llx true_npc:%llx pred_npc:%llx recover_at_fe:%i btb miss:%i taken:%i "
          "recover_at_decode:%i recover_at_exec:%i, bar_fetch:%i\n",
          bp_id, op->inst_info->addr, op->oracle_info.npc, bp_pred_info->pred_npc, bp_pred_info->recover_at_fe,
          btb_pred_miss(op->btb_pred_info), bp_pred_info->pred == TAKEN, bp_pred_info->recover_at_decode,
          bp_pred_info->recover_at_exec, op->inst_info->table_info.bar_type & BAR_FETCH);
    if ((op->inst_info->table_info.bar_type & BAR_FETCH) || IS_CALLSYS(&op->inst_info->table_info)) {
      bp_pred_info->recover_at_decode = FALSE;
      bp_pred_info->recover_at_exec = FALSE;
      STAT_EVENT(proc_id, op->off_path ? FTQ_SAW_BAR_FETCH_OFFPATH : FTQ_SAW_BAR_FETCH_ONPATH);
      return FT_EVENT_FETCH_BARRIER;
    }
    if (bp_pred_info->recover_at_fe || bp_pred_info->recover_at_decode || bp_pred_info->recover_at_exec) {
      ASSERT(0, !(bp_pred_info->recover_at_decode && bp_pred_info->recover_at_exec));

      if (op->off_path) {
        bp_pred_info->recover_at_decode = FALSE;
        bp_pred_info->recover_at_exec = FALSE;
        bp_pred_info->recover_at_fe = FALSE;
      }
      return FT_EVENT_MISPREDICT;
    } else if (trace_mode && op->off_path && bp_pred_info->pred == TAKEN) {
      // in this case, not a misprediction pred is taken so oracle info should be taken
      // and this should be last op in the FT
      // no misprediction, just manually redirect
      if (pc_plus_offset != op->oracle_info.target)
        ASSERT(proc_id, op->oracle_info.dir == TAKEN);
      return FT_EVENT_OFFPATH_TAKEN_REDIRECT;
    }

  } else if (op->inst_info->table_info.bar_type & BAR_FETCH) {
    ASSERT(0, !(bp_pred_info->recover_at_fe | bp_pred_info->recover_at_decode | bp_pred_info->recover_at_exec));
    return FT_EVENT_FETCH_BARRIER;
  }

  return FT_EVENT_NONE;
}

FT_PredictResult FT::predict_ft() {
  for (size_t idx = op_pos; idx < ops.size(); idx++) {
    Op* op = ops[idx];
    bp_predict_btb(g_bp_data, op);
    FT_Event event = FT_EVENT_NONE;
    if (bp_l0_enabled()) {
      INC_STAT_EVENT(proc_id, DFE_L0_ENABLED_PREDICTIONS, 1);

      const FT_Event l0_event = predict_op_ft_event(op, BP_PRED_L0);
      const Flag l0_wrong = op->bp_pred_l0.recover_at_fe;

      const FT_Event main_event = predict_op_ft_event(op, BP_PRED_MAIN);
      const Flag main_wrong = op->bp_pred_main.recover_at_decode || op->bp_pred_main.recover_at_exec;

      if (l0_wrong && !main_wrong) {
        STAT_EVENT(proc_id, DFE_L0_WRONG_MAIN_CORRECT);
        if (!op->off_path) {
          if (ended_by_exit()) {
            op_select_bp_pred_info(op, BP_PRED_MAIN);
            event = FT_EVENT_NONE;
          } else {
            op_select_bp_pred_info(op, BP_PRED_L0);

            const Counter fetch_cycle = op->bp_pred_main.bp_ready_cycle - BP_MAIN_LATENCY;
            const Flag l0_dir_wrong = op->bp_pred_l0.pred_orig != op->oracle_info.dir;
            const uns recovery_latency = l0_dir_wrong ? BP_MAIN_LATENCY : op->btb_pred_info->btb_pred_latency;
            ASSERT(proc_id, recovery_latency > BP_L0_LATENCY);
            const Counter recovery_cycle = fetch_cycle + recovery_latency - BP_L0_LATENCY;

            bp_sched_recovery(bp_recovery_info, op, recovery_cycle);
            event = l0_event;
          }
        } else {
          event = main_event;
        }
      } else if (l0_wrong && main_wrong) {
        STAT_EVENT(proc_id, DFE_L0_WRONG_MAIN_WRONG);
        op_select_bp_pred_info(op, BP_PRED_MAIN);
        event = main_event;
      } else if (!l0_wrong && main_wrong) {
        STAT_EVENT(proc_id, DFE_L0_CORRECT_MAIN_WRONG);
        op_select_bp_pred_info(op, BP_PRED_MAIN);
        event = main_event;
      } else {
        STAT_EVENT(proc_id, DFE_L0_CORRECT_MAIN_CORRECT);
        op_select_bp_pred_info(op, BP_PRED_MAIN);
        event = main_event;
      }
    } else {
      event = predict_op_ft_event(op, BP_PRED_MAIN);
      op_select_bp_pred_info(op, BP_PRED_MAIN);
    }
    if (op->inst_info->table_info.cf_type) {
      g_bp_data->prev_cf_pred = op->bp_pred_info->pred;  // for next BTB access
      // Per-CF prediction event: now that main's bp_pred_info is finalized for
      // op (and main's bp_data has just been spec-updated), fire alt-DFE
      // _ON_PREDICTION dispatch. Single-op rollback in trigger_alt_with_rewind
      // works because main's state hasn't advanced past op yet. Off-path
      // predictions (FT::build with off_path=true) intentionally do not fire
      // this; alt _ON_PREDICTION semantics only cover main's on-path/recovery
      // predict_ft pass.
      if (bp_id == MAIN_BP)
        decoupled_fe_on_main_prediction(proc_id, op);
    }

    if (event == FT_EVENT_FETCH_BARRIER) {
      STAT_EVENT(proc_id, op->off_path ? FTQ_SAW_BAR_FETCH_OFFPATH : FTQ_SAW_BAR_FETCH_ONPATH);
    }

    if (event != FT_EVENT_NONE) {
      uint64_t return_idx = (event == FT_EVENT_MISPREDICT) ? (idx) : 0;
      Addr pred_addr = op->bp_pred_info->pred_npc;
      if (!ended_by_exit())
        return {return_idx, event, op, pred_addr};
    }
  }
  return {0, FT_EVENT_NONE, nullptr, 0};
}

FT_Info FT::get_ft_info() const {
  return ft_info;
}

std::vector<Op*>& FT::get_ops() {
  return ops;
}

Op* FT::get_last_op() const {
  return ops.back();
}
Op* FT::get_first_op() const {
  return ops.front();
}

Addr FT::get_start_addr() const {
  return ft_info.static_info.start;
}

bool FT::is_consecutive(const FT& previous_ft) const {
  ASSERT(0, previous_ft.get_last_op());
  Op* last_op = previous_ft.get_last_op();
  if (!last_op)
    return false;
  FT_Ended_By prev_end_type = previous_ft.get_ft_info().dynamic_info.ended_by;
  Addr start_addr = ft_info.static_info.start;
  const Bp_Pred_Info* bp_pred_info = ft_active_or_main_bp_pred_info(last_op);
  Addr pred_npc = bp_pred_info->pred_npc;
  Addr npc = last_op->oracle_info.npc;
  Addr end_addr = (FRONTEND == FE_PIN_EXEC_DRIVEN)
                      ? ADDR_PLUS_OFFSET(last_op->inst_info->addr, last_op->inst_info->trace_info.inst_size)
                      : last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size;
  bool matches = false;
  switch (prev_end_type) {
    case FT_TAKEN_BRANCH:
      // Next FT must start at the predicted or actual NPC.
      matches = (pred_npc == start_addr) || (npc == start_addr);
      break;
    case FT_BAR_FETCH:
      // Barrier-fetch allows either fall-through to end_addr or NPC.
      matches = (npc == start_addr) || (end_addr == start_addr);
      break;
    default:
      // Normal fall-through: next start must equal the end of last instruction.
      matches = (end_addr == start_addr);
      break;
  }
  return matches;
}

FT_Ended_By FT::get_end_reason() const {
  if (ops.empty()) {
    return FT_NOT_ENDED;
  }

  Op* op = ops.back();  // Get the last op
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->op_pool_valid);
  ASSERT(proc_id, op->inst_info);
  if (op->eom) {
    const Bp_Pred_Info* bp_pred_info = ft_active_or_main_bp_pred_info(op);
    uns offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size) -
                 ROUND_DOWN(op->inst_info->addr, ICACHE_LINE_SIZE);
    bool end_of_icache_line = offset >= ICACHE_LINE_SIZE;
    bool cf_taken = (op->inst_info->table_info.cf_type && bp_pred_info->pred == TAKEN);
    bool bar_fetch = IS_CALLSYS(&op->inst_info->table_info) || op->inst_info->table_info.bar_type & BAR_FETCH;

    if (op->exit) {
      return FT_APP_EXIT;
    } else if (bar_fetch) {
      return FT_BAR_FETCH;
    } else if (cf_taken) {
      return FT_TAKEN_BRANCH;
    } else if (end_of_icache_line) {
      return FT_ICACHE_LINE_BOUNDARY;
    }
  }

  return FT_NOT_ENDED;
}

void FT::generate_ft_info() {
  // first op to be read at op_pos
  auto op = ops[op_pos];
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->op_pool_valid);
  ASSERT(proc_id, op->inst_info);
  ASSERT(proc_id, get_last_op());
  ASSERT(proc_id, get_last_op()->op_pool_valid);
  ASSERT(proc_id, get_last_op()->inst_info);
  ASSERT(proc_id, op->bom && get_last_op()->eom);

  ft_info.static_info.start = op->inst_info->addr;
  ft_info.dynamic_info.first_op_off_path = op->off_path;
  ft_info.dynamic_info.ended_by = get_end_reason();
  ft_info.static_info.n_uops = ops.size() - op_pos;
  ft_info.static_info.length =
      ops.back()->inst_info->addr + ops.back()->inst_info->trace_info.inst_size - ft_info.static_info.start;
  ASSERT(proc_id, ft_info.static_info.start && ft_info.static_info.length && ft_info.static_info.n_uops);
  STAT_EVENT(proc_id, POWER_BTB_READ);
}

void FT::clear_recovery_info() {
  for (auto op : ops) {
    op->bp_pred_info->recover_at_decode = FALSE;
    op->bp_pred_info->recover_at_exec = FALSE;
    op->bp_pred_info->recover_at_fe = FALSE;
  }
}

// generate_uop_cache_data was moved to uop_cache.cc as
// generate_uop_cache_data_from_FT(FT*, std::vector<Uop_Cache_Data>&)
// to keep FT class focused on FT semantics. Implementation lives in
// uop_cache.cc and is declared a friend of FT (see ft.h).

/* FT wrappers */
bool ft_can_fetch_op(FT* ft) {
  return ft->can_fetch_op();
}

Op* ft_fetch_op(FT* ft) {
  return ft->fetch_op();
}

FT_Info ft_get_ft_info(FT* ft) {
  return ft->get_ft_info();
}

static bool ft_op_recovery_addr_is_consecutive(Op* op, Addr next_start) {
  ASSERT(0, op);

  const Bp_Pred_Info* bp_pred_info = ft_active_or_main_bp_pred_info(op);
  Addr pred_npc = bp_pred_info->pred_npc;
  Addr npc = op->oracle_info.npc;
  Addr end_addr = (FRONTEND == FE_PIN_EXEC_DRIVEN)
                      ? ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size)
                      : op->inst_info->addr + op->inst_info->trace_info.inst_size;

  return (pred_npc == next_start) || (npc == next_start) || (end_addr == next_start);
}

bool ft_recovery_addr_is_consecutive(FT* ft, Addr next_start) {
  ASSERT(0, ft);
  return ft_op_recovery_addr_is_consecutive(ft->get_last_op(), next_start);
}

void assert_ft_after_recovery(uns8 proc_id, Op* op, Addr recovery_fetch_addr) {
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->parent_FT);
  const bool consecutive = ft_op_recovery_addr_is_consecutive(op, recovery_fetch_addr);
  ASSERT(proc_id, consecutive);
  ASSERT(proc_id, IS_FLUSHING_OP(op));
  ASSERT(proc_id, op->eom);
}

/* retire and flush, free all ops in a FT when last op is freed */
void ft_free_op(Op* op) {
  ASSERT(0, op->parent_FT);
  if (op->parent_FT_off_path && op->parent_FT_off_path->get_last_op() == op)
    delete op->parent_FT_off_path;
  if (!op->parent_FT_off_path && op->parent_FT->get_last_op() == op) {
    FT* ft = op->parent_FT;
    std::vector<Op*>& ft_ops = ft->get_ops();

    Flag has_on_path = FALSE;
    Flag has_off_path = FALSE;
    for (Op* ft_op : ft_ops) {
      has_off_path |= ft_op->off_path;
      has_on_path |= !ft_op->off_path;
      if (has_on_path && has_off_path)
        break;
    }

    // Mixed-path FT: drop all off-path ops so recovery sees a pure on-path FT.
    if (has_on_path && has_off_path) {
      DEBUG(op->proc_id, "[DFE%u] ft_free_op mixed FT cleanup: ft_id:%llu trigger_op:%llu total_ops:%zu\n",
            ft->get_bp_id(), (unsigned long long)ft->get_ft_info().dynamic_info.FT_id, (unsigned long long)op->op_num,
            ft_ops.size());
      while (!ft_ops.empty() && ft_ops.back()->off_path) {
        Op* tail = ft_ops.back();
        DEBUG(op->proc_id, "[DFE%u] ft_free_op removing off-path op_num:%llu addr:0x%llx\n", ft->get_bp_id(),
              (unsigned long long)tail->op_num, (unsigned long long)tail->inst_info->addr);
        ft_ops.pop_back();
        free_op(tail);
      }
      // Regenerate ft_info to reflect the on-path-only ops, then mark all as fetched.
      ft->op_pos = 0;
      ft->generate_ft_info();
      ft->op_pos = ft_ops.size();
      return;
    }

    // Uniform-path FT (all on-path or all off-path): delete FT as before.
    delete ft;
  }
}

/* use set to avoid duplicates and keep PC order */
std::set<Addr> FT::get_pcs() {
  std::set<Addr> pc_set;
  for (auto op : ops) {
    if (op && op->inst_info) {
      pc_set.insert(op->inst_info->addr);
    }
  }
  return pc_set;
}
