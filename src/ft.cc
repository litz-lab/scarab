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

#include <functional>
#include <iostream>

#include "globals/assert.h"
#include "globals/utils.h"

#include "bp/bp.param.h"
#include "memory/memory.param.h"

#include "frontend/frontend_intf.h"
#include "isa/isa_macros.h"

#include "decoupled_frontend.h"
#include "op_pool.h"
#include "uop_cache.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

uint64_t FT_id_counter = 0;

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
  op_pos = 0;
  ft_info.static_info.start = 0;
  ft_info.static_info.length = 0;
  ft_info.static_info.n_uops = 0;
  ft_info.dynamic_info.ended_by = FT_NOT_ENDED;
  ft_info.dynamic_info.first_op_off_path = FALSE;
  is_prebuilt = false;
}

bool FT::can_fetch_op() {
  return op_pos < ops.size();
}

Op* FT::fetch_op() {
  ASSERT(proc_id, can_fetch_op());
  Op* op = ops[op_pos];
  op_pos++;

  DEBUG(proc_id, "Fetch op from FT fetch_addr0x:%llx off_path:%i op_num:%llu\n", op->inst_info->addr, op->off_path,
        op->op_num);
  return op;
}

void FT::add_op(Op* op) {
  if (!ops.empty()) {
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

void FT::recover_ft() {
  // This path is for an FT already popped from FTQ and currently in-flight
  // (e.g., ic/uc current_ft). We only trim unread tail ops that are newer
  // than recovery_op_num. Older/read ops are handled by stage-data recovery.
  FT_Info before_info = get_ft_info();
  size_t before_ops = ops.size();
  uint64_t before_op_pos = op_pos;
  Op* before_last = ops.empty() ? nullptr : ops.back();
  UNUSED(before_info);
  UNUSED(before_ops);
  UNUSED(before_op_pos);
  UNUSED(before_last);
  DEBUG(proc_id,
        "FT recover start: ft_id:%llu start:0x%llx len:%llu n_uops:%llu op_pos:%llu ops:%zu end_reason:%d last_op:%s "
        "last_addr:0x%llx last_eom:%u\n",
        (unsigned long long)before_info.dynamic_info.FT_id, (unsigned long long)before_info.static_info.start,
        (unsigned long long)before_info.static_info.length, (unsigned long long)before_info.static_info.n_uops,
        (unsigned long long)before_op_pos, before_ops, (int)get_end_reason(), before_last ? "yes" : "no",
        (unsigned long long)(before_last ? before_last->inst_info->addr : 0),
        (unsigned)(before_last ? before_last->eom : 0));

  trim_unread_tail([&](Op* op) {
    if (!FLUSH_OP(op))
      return false;
    DEBUG(proc_id, "FT recovery flushing unread op_num:%llu off_path:%u\n", (unsigned long long)op->op_num,
          op->off_path);
    ASSERT(proc_id, op->off_path);
    ASSERT(proc_id, op->parent_FT == this);
    return true;
  });

  FT_Info after_info = get_ft_info();
  Op* after_last = ops.empty() ? nullptr : ops.back();
  UNUSED(after_info);
  UNUSED(after_last);
  DEBUG(proc_id,
        "FT recover end: ft_id:%llu start:0x%llx len:%llu n_uops:%llu op_pos:%llu ops:%zu end_reason:%d last_op:%s "
        "last_addr:0x%llx last_eom:%u\n",
        (unsigned long long)after_info.dynamic_info.FT_id, (unsigned long long)after_info.static_info.start,
        (unsigned long long)after_info.static_info.length, (unsigned long long)after_info.static_info.n_uops,
        (unsigned long long)op_pos, ops.size(), (int)get_end_reason(), after_last ? "yes" : "no",
        (unsigned long long)(after_last ? after_last->inst_info->addr : 0),
        (unsigned)(after_last ? after_last->eom : 0));
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
    Op* op = alloc_op(proc_id, bp_id);
    fetch_op_fn(proc_id, bp_id, op);
    op->off_path = off_path;
    op->conf_off_path = conf_off_path;
    op->op_num = get_next_op_id_fn();
    op->bp_pred_info->pred_npc = op->oracle_info.npc;
    op->bp_pred_info->pred = op->oracle_info.dir;  // for prebuilt, pred is same as dir
    if (off_path)
      event = predict_one_cf_op(op);
    if (op->inst_info->fake_inst == 1)
      ft_info.dynamic_info.contains_fake_nop = TRUE;
    add_op(op);
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

FT_Event FT::predict_one_cf_op(Op* op) {
  bool trace_mode = false;

#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  if (op->table_info->cf_type) {
    ASSERT(proc_id, op->eom);
    bp_predict_op(g_bp_data, op, 1, op->inst_info->addr);
    const Addr pc_plus_offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size);
    if ((op->table_info->bar_type & BAR_FETCH) || IS_CALLSYS(op->table_info)) {
      op->bp_pred_info->recover_at_decode = FALSE;
      op->bp_pred_info->recover_at_exec = FALSE;
      op->bp_pred_info->recover_at_fe = FALSE;
      return FT_EVENT_FETCH_BARRIER;
    }
    if (op->bp_pred_info->recover_at_fe || op->bp_pred_info->recover_at_decode || op->bp_pred_info->recover_at_exec) {
      ASSERT(0, !(op->bp_pred_info->recover_at_decode && op->bp_pred_info->recover_at_exec));

      if (op->off_path) {
        op->bp_pred_info->recover_at_decode = FALSE;
        op->bp_pred_info->recover_at_exec = FALSE;
        op->bp_pred_info->recover_at_fe = FALSE;
      }
      return FT_EVENT_MISPREDICT;
    } else if (trace_mode && op->off_path && op->bp_pred_info->pred == TAKEN) {
      // in this case, not a misprediction pred is taken so oracle info should be taken
      // and this should be last op in the FT
      // no misprediction, just manually redirect
      if (pc_plus_offset != op->oracle_info.target)
        ASSERT(proc_id, op->oracle_info.dir == TAKEN);
      return FT_EVENT_OFFPATH_TAKEN_REDIRECT;
    }

  } else if (op->table_info->bar_type & BAR_FETCH) {
    ASSERT(0, !(op->bp_pred_info->recover_at_fe | op->bp_pred_info->recover_at_decode |
                op->bp_pred_info->recover_at_exec));
    return FT_EVENT_FETCH_BARRIER;
  }

  return FT_EVENT_NONE;
}

FT_PredictResult FT::predict_ft() {
  for (size_t idx = op_pos; idx < ops.size(); idx++) {
    Op* op = ops[idx];
    FT_Event event = FT_EVENT_NONE;
    if (op->table_info->cf_type) {
      ASSERT(proc_id, op->eom);
      const Flag l0_enabled = (bp_id == 0 && bp_l0_enabled());
      if (l0_enabled) {
        INC_STAT_EVENT(proc_id, DFE_L0_ENABLED_PREDICTIONS, 1);

        op->bp_pred_level = BP_PRED_L0;
        op_select_bp_pred_info(op, op->bp_pred_level);
        const FT_Event l0_event = predict_one_cf_op(op);
        const Flag l0_wrong = op->bp_pred_l0.mispred || op->bp_pred_l0.misfetch;

        op->bp_pred_level = BP_PRED_MAIN;
        op_select_bp_pred_info(op, op->bp_pred_level);
        const FT_Event main_event = predict_one_cf_op(op);
        const Flag main_wrong = op->bp_pred_main.mispred || op->bp_pred_main.misfetch;

        if (l0_wrong && !main_wrong) {
          STAT_EVENT(proc_id, DFE_L0_WRONG_MAIN_CORRECT);
          if (!op->off_path) {
            op->bp_pred_level = BP_PRED_L0;
            op_select_bp_pred_info(op, op->bp_pred_level);
            event = l0_event;
          } else {
            event = main_event;
          }
        } else if (l0_wrong && main_wrong) {
          STAT_EVENT(proc_id, DFE_L0_WRONG_MAIN_WRONG);
          event = main_event;
        } else if (!l0_wrong && main_wrong) {
          STAT_EVENT(proc_id, DFE_L0_CORRECT_MAIN_WRONG);
          event = main_event;
        } else {
          STAT_EVENT(proc_id, DFE_L0_CORRECT_MAIN_CORRECT);
          event = main_event;
        }
      } else {
        op->bp_pred_level = BP_PRED_MAIN;
        op_select_bp_pred_info(op, op->bp_pred_level);
        event = predict_one_cf_op(op);
      }

      DEBUG(
          proc_id,
          "[DFE%u] Predict CF fetch_addr:%llx true_npc:%llx pred_npc:%llx mispred:%i misfetch:%i btb miss:%i taken:%i "
          "recover_at_fe:%i recover_at_decode:%i recover_at_exec:%i, bar_fetch:%i\n",
          bp_id, op->inst_info->addr, op->oracle_info.npc, op->bp_pred_info->pred_npc, op->bp_pred_info->mispred,
          op->bp_pred_info->misfetch, op->btb_pred_info->btb_miss, op->bp_pred_info->pred == TAKEN,
          op->bp_pred_info->recover_at_fe, op->bp_pred_info->recover_at_decode, op->bp_pred_info->recover_at_exec,
          op->table_info->bar_type & BAR_FETCH);
    } else {
      event = predict_one_cf_op(op);
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
  Addr pred_npc = last_op->bp_pred_info->pred_npc;
  Addr npc = last_op->oracle_info.npc;
  Addr end_addr = last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size;
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
  if (op->eom) {
    uns offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size) -
                 ROUND_DOWN(op->inst_info->addr, ICACHE_LINE_SIZE);
    bool end_of_icache_line = offset >= ICACHE_LINE_SIZE;
    bool cf_taken = (op->table_info->cf_type && op->bp_pred_info->pred == TAKEN);
    bool bar_fetch = IS_CALLSYS(op->table_info) || op->table_info->bar_type & BAR_FETCH;

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

void FT::regenerate_ft_info_after_recovery(Counter recovery_op_num, Bp_Pred_Level bp_pred_level) {
  if (ops.empty()) {
    DEBUG(proc_id, "FT regenerate info: found_recovery:0 recovery_op_is_last:0 (empty FT) recovery_op_num:%llu\n",
          (unsigned long long)recovery_op_num);
    return;
  }

  bool found_recovery = false;
  for (auto* op : ops) {
    if (op && op->op_num == recovery_op_num) {
      found_recovery = true;
      break;
    }
  }
  UNUSED(found_recovery);

  Op* last = get_last_op();
  ASSERT(proc_id, last);
  bool recovery_op_is_last = (last->op_num == recovery_op_num);

  DEBUG(proc_id,
        "FT regenerate info: found_recovery:%u recovery_op_is_last:%u recovery_op_num:%llu ft_id:%llu op_pos:%llu "
        "ops:%zu last_op_num:%llu last_addr:0x%llx last_eom:%u last_cf:%u level:%u pred:%u pred_npc:0x%llx "
        "oracle_npc:0x%llx main_pred:%u l0_pred:%u\n",
        (unsigned)found_recovery, (unsigned)recovery_op_is_last, (unsigned long long)recovery_op_num,
        (unsigned long long)ft_info.dynamic_info.FT_id, (unsigned long long)op_pos, ops.size(),
        (unsigned long long)last->op_num, (unsigned long long)last->inst_info->addr, (unsigned)last->eom,
        (unsigned)last->table_info->cf_type, (unsigned)bp_pred_level, (unsigned)last->bp_pred_info->pred,
        (unsigned long long)last->bp_pred_info->pred_npc, (unsigned long long)last->oracle_info.npc,
        (unsigned)last->bp_pred_main.pred, (unsigned)last->bp_pred_l0.pred);

  if (recovery_op_is_last) {
    op_select_bp_pred_info(last, bp_pred_level);
  }

  uint64_t saved_op_pos = op_pos;
  op_pos = 0;
  generate_ft_info();
  DEBUG(proc_id, "FT regenerate result: ft_id:%llu start:0x%llx len:%llu n_uops:%llu ended_by:%d\n",
        (unsigned long long)ft_info.dynamic_info.FT_id, (unsigned long long)ft_info.static_info.start,
        (unsigned long long)ft_info.static_info.length, (unsigned long long)ft_info.static_info.n_uops,
        (int)ft_info.dynamic_info.ended_by);
  op_pos = saved_op_pos;
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

uint64_t ft_get_num_unread_ops(FT* ft) {
  ASSERT(0, ft);
  return ft->get_ops().size() - ft->get_op_pos();
}

bool ft_recovery_addr_is_consecutive(FT* ft, Addr next_start, Counter recovery_op_num) {
  ASSERT(0, ft);

  Op* recovery_op = nullptr;
  for (auto* ft_op : ft->get_ops()) {
    if (ft_op && ft_op->op_num == recovery_op_num) {
      recovery_op = ft_op;
      break;
    }
  }

  if (!recovery_op) {
    Op* last_op = ft->get_last_op();
    ASSERT(0, last_op);
    DEBUG(last_op->proc_id, "FT recovery consecutivity skipped: recovery_op_num:%llu not in FT[start:%llx]\n",
          (unsigned long long)recovery_op_num, (unsigned long long)ft->get_ft_info().static_info.start);
    return TRUE;
  }

  Addr npc = recovery_op->oracle_info.npc;
  DEBUG(recovery_op->proc_id, "FT recovery consecutivity: winner:%s op_num:%llu npc:%llx next_start:%llx\n",
        (recovery_op->bp_pred_level == BP_PRED_L0) ? "l0" : "main", (unsigned long long)recovery_op->op_num,
        (unsigned long long)npc, (unsigned long long)next_start);
  return npc == next_start;
}

void assert_ft_after_recovery(uns8 proc_id, Op* op, Addr recovery_fetch_addr, Counter recovery_op_num) {
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->parent_FT);
  ASSERT(proc_id, ft_recovery_addr_is_consecutive(op->parent_FT, recovery_fetch_addr, recovery_op_num));
  ASSERT(proc_id, IS_FLUSHING_OP(op));
  ASSERT(proc_id, op->eom);
}

void recover_ft(FT* ft) {
  ASSERT(0, ft);
  ft->recover_ft();
}

void ft_regenerate_info_after_recovery(FT* ft, Counter recovery_op_num, Bp_Pred_Level bp_pred_level) {
  if (!ft)
    return;
  ft->regenerate_ft_info_after_recovery(recovery_op_num, bp_pred_level);
}

/* retire and flush, free all ops in a FT when last op is freed */
void ft_free_op(Op* op, FT** ft_ref0, FT** ft_ref1) {
  ASSERT(0, op);
  ASSERT(0, op->parent_FT);

  FT* primary_ft = op->parent_FT;
  FT* off_path_ft = op->parent_FT_off_path;
  const bool delete_off_path_ft = (off_path_ft && off_path_ft->get_last_op() == op);
  bool delete_primary_ft = (primary_ft && primary_ft->get_last_op() == op);

  // If the op is still shared with an off-path FT that is not ending now,
  // primary FT deletion must be deferred.
  if (delete_primary_ft && off_path_ft && !delete_off_path_ft) {
    delete_primary_ft = false;
  }

  // Clear only refs to FT objects that are actually deleted.
  auto clear_if_deleted = [&](FT** ft_ref) {
    if (!ft_ref || !*ft_ref)
      return;
    if (delete_off_path_ft && *ft_ref == off_path_ft) {
      *ft_ref = NULL;
      return;
    }
    if (delete_primary_ft && *ft_ref == primary_ft)
      *ft_ref = NULL;
  };
  clear_if_deleted(ft_ref0);
  clear_if_deleted(ft_ref1);

  // Delete off-path FT first so shared ops clear parent_FT_off_path.
  if (delete_off_path_ft)
    delete off_path_ft;
  if (delete_primary_ft)
    delete primary_ft;
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
