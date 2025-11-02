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

#include "memory/memory.param.h"

#include "frontend/frontend_intf.h"
#include "isa/isa_macros.h"

#include "decoupled_frontend.h"
#include "op_pool.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

uint64_t FT_id_counter = 0;

/* FT member functions */
FT::~FT() {
  ASSERT(proc_id, !ops.empty());
  Flag all_on_path = !ops[0]->off_path && !ops.back()->off_path;
  for (auto ft_op : ops) {
    if (all_on_path || ft_op->off_path) {
      free_op(ft_op);
    }
  }
}

FT::FT(uns _proc_id) : proc_id(_proc_id) {
  ft_info.dynamic_info.FT_id = FT_id_counter++;
  op_pos = 0;
  ft_info.static_info.start = 0;
  ft_info.static_info.length = 0;
  ft_info.static_info.n_uops = 0;
  ft_info.dynamic_info.ended_by = FT_NOT_ENDED;
  ft_info.dynamic_info.first_op_off_path = FALSE;
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
      ASSERT(proc_id, ops.back()->inst_info->addr + ops.back()->inst_info->trace_info.inst_size == op->inst_info->addr);
    } else {
      // assert all uops of the same inst share the same addr
      ASSERT(proc_id, ops.back()->inst_info->addr == op->inst_info->addr);
    }
  }
  op->parent_FT = this;

  ops.emplace_back(op);
}

Flag FT::build(std::function<bool(uns8)> can_fetch_op_fn, std::function<bool(uns8, Op*)> fetch_op_fn, bool off_path,
               std::function<uint64_t()> get_next_op_id_fn) {
  do {
    if (!can_fetch_op_fn(proc_id)) {
      std::cout << "Warning could not fetch inst from frontend" << std::endl;
      delete this;
      return false;
    }
    Op* op = alloc_op(proc_id);
    fetch_op_fn(proc_id, op);
    op->off_path = off_path;
    op->op_num = get_next_op_id_fn();
    op->oracle_info.pred_npc = op->oracle_info.npc;
    op->oracle_info.pred = op->oracle_info.dir;  // for prebuilt, pred is same as dir
    if (off_path)
      predict_one_cf_op(op);
    add_op(op);
    STAT_EVENT(proc_id, FTQ_FETCHED_INS_ONPATH + off_path);
  } while (get_end_reason() == FT_NOT_ENDED);

  generate_ft_info();

  return true;
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

  // if split at the last op and FT already ended, no need to create new FT, just update end condition
  if (split_index == ops.size() - 1 && get_end_reason() != FT_NOT_ENDED) {
    generate_ft_info();
    return {this, nullptr};
  }
  // Initialize off-path FT that will contain off-path ops after split position
  FT* off_path_ft = new FT(proc_id);

  for (uns i = 0; i <= split_index; i++) {
    off_path_ft->ops.push_back(ops[i]);
  }
  off_path_ft->op_pos = op_pos;

  // Only save original FT as recovery FT if there are ops after the split point
  op_pos = index_uns + 1;
  bool has_trailing_ops = (op_pos < ops.size());
  if (has_trailing_ops) {
    // Inline move_over_ft logic
    ASSERT(0, (index_uns + 1 <= ops.size() - 1 && ops.size() - 1 < ops.size() && index_uns + 1 >= 0));

    generate_ft_info();

    ASSERT(proc_id, ft_info.static_info.start && ft_info.static_info.length && ops.size());
    ASSERT(proc_id, ops.size() == (ops.size() - ops.size()) + ops.size());
    ASSERT(proc_id, ft_info.dynamic_info.first_op_off_path == 0);
    ASSERT(proc_id, ft_info.dynamic_info.ended_by != FT_NOT_ENDED);
    ASSERT(proc_id, get_end_reason() != FT_NOT_ENDED);
  }

  // Reset the 'end' part of ft_info before possible rebuilding
  ASSERT(proc_id, off_path_ft->ops.size() == index_uns + 1);
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

    DEBUG(proc_id,
          "Predict CF fetch_addr:%llx true_npc:%llx pred_npc:%llx mispred:%i misfetch:%i btb miss:%i taken:%i "
          "recover_at_decode:%i recover_at_exec:%i, bar_fetch:%i\n",
          op->inst_info->addr, op->oracle_info.npc, op->oracle_info.pred_npc, op->oracle_info.mispred,
          op->oracle_info.misfetch, op->oracle_info.btb_miss, op->oracle_info.pred == TAKEN,
          op->oracle_info.recover_at_decode, op->oracle_info.recover_at_exec, op->table_info->bar_type & BAR_FETCH);
    if ((op->table_info->bar_type & BAR_FETCH) || IS_CALLSYS(op->table_info)) {
      {
        op->oracle_info.recover_at_decode = FALSE;
        op->oracle_info.recover_at_exec = FALSE;
        STAT_EVENT(proc_id, op->off_path ? FTQ_SAW_BAR_FETCH_OFFPATH : FTQ_SAW_BAR_FETCH_ONPATH);
        return FT_EVENT_FETCH_BARRIER;
      }
    }
    if (op->oracle_info.recover_at_decode || op->oracle_info.recover_at_exec) {
      ASSERT(0, !(op->oracle_info.recover_at_decode && op->oracle_info.recover_at_exec));

      if (op->off_path) {
        op->oracle_info.recover_at_decode = FALSE;
        op->oracle_info.recover_at_exec = FALSE;
      }
      if (op->off_path && op->oracle_info.pred == NOT_TAKEN) {
        // if off path and not taken no redirect needed
        // just return FT_EVENT_NONE
        return FT_EVENT_NONE;
      }
      return FT_EVENT_MISPREDICT;
    } else if (trace_mode && op->off_path && op->oracle_info.pred == TAKEN) {
      // in this case, not a misprediction pred is taken so oracle info should be taken
      // and this should be last op in the FT
      // no misprediction, just manually redirect
      if (pc_plus_offset != op->oracle_info.target)
        ASSERT(proc_id, op->oracle_info.dir == TAKEN);
      return FT_EVENT_OFFPATH_TAKEN_REDIRECT;
    }

  } else if (op->table_info->bar_type & BAR_FETCH) {
    ASSERT(0, !(op->oracle_info.recover_at_decode | op->oracle_info.recover_at_exec));
    if (op->off_path)
      STAT_EVENT(proc_id, FTQ_SAW_BAR_FETCH_OFFPATH);
    else
      STAT_EVENT(proc_id, FTQ_SAW_BAR_FETCH_ONPATH);
    return FT_EVENT_FETCH_BARRIER;
  }

  return FT_EVENT_NONE;
}

FT_PredictResult FT::predict_ft() {
  for (size_t idx = op_pos; idx < ops.size(); idx++) {
    Op* op = ops[idx];
    FT_Event event = predict_one_cf_op(op);
    if (event != FT_EVENT_NONE) {
      uint64_t return_idx = (event == FT_EVENT_MISPREDICT) ? (idx) : 0;
      Addr pred_addr = op->oracle_info.pred_npc;
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
  Addr pred_npc = last_op->oracle_info.pred_npc;
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
    bool cf_taken = (op->table_info->cf_type && op->oracle_info.pred == TAKEN);
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
  for (uns i = op_pos; i <= ops.size() - 1; ++i) {
    ops[i]->ft_info = ft_info;
  }
  ASSERT(proc_id, ft_info.static_info.start && ft_info.static_info.length && ft_info.static_info.n_uops);
  STAT_EVENT(proc_id, POWER_BTB_READ);
}

void FT::clear_recovery_info() {
  for (auto op : ops) {
    op->oracle_info.recover_at_decode = FALSE;
    op->oracle_info.recover_at_exec = FALSE;
  }
}

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

/* retire and flush, free all ops in a FT when last op is freed */
void ft_free_op(Op* op) {
  ASSERT(0, op->parent_FT);
  if (op->parent_FT->get_last_op() == op) {
    delete op->parent_FT;
  }
}