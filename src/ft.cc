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

#include "op_pool.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

uint64_t FT_id_count = 0;

/* FT member functions */
FT::FT(uns _proc_id) : proc_id(_proc_id), consumed(false) {
  free_ops_and_clear();
}

void FT::free_ops_and_clear() {
  while (op_pos < ops.size()) {
    free_op(ops[op_pos]);
    op_pos++;
  }

  ops.clear();
  op_pos = 0;
  ft_info.static_info.start = 0;
  ft_info.static_info.length = 0;
  ft_info.static_info.n_uops = 0;
  ft_info.dynamic_info.started_by = FT_NOT_STARTED;
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

void FT::set_per_op_ft_info() {
  for (auto op : ops) {
    op->ft_info = ft_info;
  }
}

void FT::set_ft_started_by(FT_Started_By ft_started_by) {
  ft_info.dynamic_info.started_by = ft_started_by;
}

void FT::add_op(Op* op, FT_Ended_By ft_ended_by) {
  if (ops.empty()) {
    ASSERT(proc_id, op->bom && !ft_info.static_info.start);
    ft_info.static_info.start = op->inst_info->addr;
    ft_info.dynamic_info.first_op_off_path = op->off_path;
  } else {
    if (op->bom) {
      ASSERT(proc_id, ops.back()->inst_info->addr + ops.back()->inst_info->trace_info.inst_size == op->inst_info->addr);
    } else {
      // assert all uops of the same inst share the same addr
      ASSERT(proc_id, ops.back()->inst_info->addr == op->inst_info->addr);
    }
  }
  ops.emplace_back(op);
  ft_info.dynamic_info.ended_by = ft_ended_by;
  if (ft_ended_by != FT_NOT_ENDED) {
    ASSERT(proc_id, op->eom && !ft_info.static_info.length);
    ASSERT(proc_id, ft_info.static_info.start);
    ft_info.static_info.n_uops = ops.size();
    ft_info.static_info.length = op->inst_info->addr + op->inst_info->trace_info.inst_size - ft_info.static_info.start;

    // counting extremely short FT reason
    if (!ft_info.dynamic_info.first_op_off_path) {
      if (ft_info.static_info.n_uops <= (int)UOP_CACHE_WIDTH) {
        if (ft_info.dynamic_info.started_by == FT_STARTED_BY_ICACHE_LINE_BOUNDARY) {
          STAT_EVENT(proc_id, FT_SHORT_ICACHE_LINE_BOUNDARY_ICACHE_LINE_BOUNDARY + ft_ended_by - 1);
          INC_STAT_EVENT(proc_id, FT_SHORT_UOP_LOST_ICACHE_LINE_BOUNDARY_ICACHE_LINE_BOUNDARY + ft_ended_by - 1,
                         UOP_CACHE_WIDTH - ft_info.static_info.n_uops);
        } else if (ft_info.dynamic_info.started_by == FT_STARTED_BY_TAKEN_BRANCH) {
          STAT_EVENT(proc_id, FT_SHORT_TAKEN_BRANCH_ICACHE_LINE_BOUNDARY + ft_ended_by - 1);
          INC_STAT_EVENT(proc_id, FT_SHORT_UOP_LOST_TAKEN_BRANCH_ICACHE_LINE_BOUNDARY + ft_ended_by - 1,
                         UOP_CACHE_WIDTH - ft_info.static_info.n_uops);
        } else if (ft_info.dynamic_info.started_by == FT_STARTED_BY_RECOVERY) {
          STAT_EVENT(proc_id, FT_SHORT_RECOVERY_ICACHE_LINE_BOUNDARY + ft_ended_by - 1);
          INC_STAT_EVENT(proc_id, FT_SHORT_UOP_LOST_RECOVERY_ICACHE_LINE_BOUNDARY + ft_ended_by - 1,
                         UOP_CACHE_WIDTH - ft_info.static_info.n_uops);
        } else {
          STAT_EVENT(proc_id, FT_SHORT_OTHER + ft_ended_by - 1);
          INC_STAT_EVENT(proc_id, FT_SHORT_UOP_LOST_OTHER, UOP_CACHE_WIDTH - ft_info.static_info.n_uops);
        }
      } else {
        STAT_EVENT(proc_id, FT_NOT_SHORT);
      }
    }
  }
}

FT_BuildResult FT::build_full_ft(uns start_index, std::function<bool(uns8)> can_fetch_op_fn,
                                 std::function<bool(uns8, Op*)> fetch_op_fn, FT last_ft, Flag off_path, Flag use_pred,
                                 uint64_t& dfe_op_count) {
  FT_BuildResult result;
  result.build_complete = false;
  result.redirect_needed = false;
  result.fetch_bar_needed = false;
  result.trigger_op = nullptr;
  result.redirect_uid = 0;
  result.redirect_addr = 0;

  if (!can_fetch_op_fn(proc_id)) {
    std::cout << "Warning could not fetch inst from frontend" << std::endl;
    *this = FT(proc_id);
    return result;
  }
  if (last_ft.ft_info.dynamic_info.ended_by == FT_APP_EXIT) {
    *this = FT(proc_id);
    return result;
  }

  FT_Ended_By ft_ended_by = FT_NOT_ENDED;
  if (start_index == 0) {
    ft_info.dynamic_info.FT_id = FT_id_count++;
  }

  if (start_index > 0) {
    ft_ended_by = ft_get_ended_by(ops[start_index - 1], use_pred);
  }
  while (ft_ended_by == FT_NOT_ENDED) {
    Op* op = alloc_op(proc_id);
    bool fetched = fetch_op_fn(proc_id, op);
    if (!fetched)
      return result;
    op->off_path = off_path;
    FT_Event event = FT_EVENT_NONE;
    if (op->off_path && start_index != 0) {
      event = predict_one_cf_op(op, dfe_op_count);
    }
    ft_ended_by = ft_get_ended_by(op, use_pred);
    add_op(op, ft_ended_by);
    if (event == FT_EVENT_MISPREDICT || event == FT_EVENT_OFFPATH_TAKEN_REDIRECT) {
      result.redirect_needed = true;
      result.trigger_op = op;
      result.redirect_uid = op->inst_uid;
      result.redirect_addr = op->oracle_info.pred_npc;
      return result;
    } else if (event == FT_EVENT_FETCH_BARRIER) {
      result.trigger_op = op;
      result.fetch_bar_needed = true;
      return result;
    }
    if (off_path) {
      STAT_EVENT(proc_id, FTQ_FETCHED_INS_OFFPATH);
    } else {
      STAT_EVENT(proc_id, FTQ_FETCHED_INS_ONPATH);
    }
  }
  // If no new ops were added in the loop, but FT ended at start_index, update ft_info accordingly
  if (ft_ended_by != FT_NOT_ENDED || (start_index > 0 && ops.size() == start_index)) {
    // If we didn't add a new op, but FT ended at the previous op, update ft_info
    if (ops.size() > 0) {
      ft_info.static_info.n_uops = ops.size();
      Op* last_op = ops.back();
      ft_info.static_info.length =
          last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size - ft_info.static_info.start;
    }
    ft_info.dynamic_info.ended_by = ft_ended_by;
    set_per_op_ft_info();
    if (last_ft.get_ft_info().static_info.start) {
      Op* last_op = last_ft.peek_last_op();
      ASSERT(proc_id, last_op);
      FT_Ended_By prev_end_type = last_ft.get_ft_info().dynamic_info.ended_by;
      Addr start_addr = ft_info.static_info.start;
      if (prev_end_type == FT_TAKEN_BRANCH) {
        ASSERT(proc_id, last_op->oracle_info.pred_npc == start_addr || last_op->oracle_info.npc == start_addr);
      } else if (prev_end_type == FT_BAR_FETCH) {
        ASSERT(proc_id, last_op->oracle_info.npc == start_addr ||
                            last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size == start_addr);
      } else {
        if (last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size != start_addr)
          ASSERT(proc_id, last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size == start_addr);
      }
    }
    if (ft_ended_by == FT_ICACHE_LINE_BOUNDARY) {
      set_ft_started_by(FT_STARTED_BY_ICACHE_LINE_BOUNDARY);
    } else if (ft_ended_by == FT_TAKEN_BRANCH) {
      set_ft_started_by(FT_STARTED_BY_TAKEN_BRANCH);
    } else if (ft_ended_by == FT_BAR_FETCH) {
      set_ft_started_by(FT_STARTED_BY_BAR_FETCH);
    }
    result.build_complete = true;
  }
  return result;
}

// will split the FT into two parts, the first part contains ops from 0 to index,
// the second part contains ops from index + 1 to the end of the FT for now to keep ft_info same as before
// can change to save the old ft and move read pointer in a later patch
std::pair<FT, FT> FT::split_ft(uns index) {
  uns index_uns = static_cast<uns>(index);
  FT tailing_FT = FT();
  ASSERT(proc_id, index_uns < ops.size() && index_uns >= 0);

  // Move out tailing FT as before
  if (index_uns < ops.size() - 1) {
    tailing_FT = move_over_ft(index_uns + 1, ops.size() - 1, 0);
    ASSERT(proc_id,
           tailing_FT.ft_info.static_info.start && tailing_FT.ft_info.static_info.length && tailing_FT.ops.size());
    ASSERT(proc_id, ops.size() == (ops.size() - tailing_FT.ops.size()) + tailing_FT.ops.size());
    if (tailing_FT.ft_info.dynamic_info.first_op_off_path) {
      tailing_FT.free_ops_and_clear();
    }
    // Remove moved ops from original FT
    ops.erase(ops.begin() + index_uns + 1, ops.end());
    // Assert ops length is updated to index_uns + 1
    ASSERT(proc_id, ops.size() == index_uns + 1);
  }

  // Reset the 'end' part of ft_info before rebuilding
  ft_info.static_info.length = 0;
  ft_info.static_info.n_uops = 0;
  ft_info.dynamic_info.ended_by = FT_NOT_ENDED;

  return {*this, tailing_FT};
}

FT FT::move_over_ft(uns start_idx, uns end_idx, Flag use_pred) {
  FT dest_ft = FT(0);
  ASSERT(0, start_idx <= end_idx && end_idx < ops.size() && start_idx >= 0);

  for (uns i = start_idx; i <= end_idx; ++i) {
    Op* op = ops[i];
    FT_Ended_By ft_ended_by = ft_get_ended_by(op, use_pred);
    dest_ft.add_op(op, ft_ended_by);
  }

  return dest_ft;
}

FT_Event FT::predict_one_cf_op(Op* op, uint64_t& dfe_op_count) {
  bool trace_mode = false;

#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  op->op_num = dfe_op_count++;
  if (op->table_info->cf_type) {
    ASSERT(proc_id, op->eom);
    if (!op->off_path)
      STAT_EVENT(proc_id, FT_CF_ON);
    bp_predict_op(g_bp_data, op, 1, op->inst_info->addr);
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
      ASSERT(0, (int)op->oracle_info.recover_at_decode + (int)op->oracle_info.recover_at_exec < 2);

      if (op->off_path) {
        op->oracle_info.recover_at_decode = FALSE;
        op->oracle_info.recover_at_exec = FALSE;
      }
      STAT_EVENT(proc_id, REDIRECT_FROM_ON);
      return FT_EVENT_MISPREDICT;
    } else if (trace_mode && op->off_path && op->oracle_info.pred == TAKEN) {
      // in this case, not a misprediction pred is taken so oracle info should be taken
      // and this should be last op in the FT
      // no misprediction, just manually redirect
      ASSERT(proc_id, op->oracle_info.dir == TAKEN);
      STAT_EVENT(proc_id, REDIRECT_FROM_OFF);
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

FT_PredictResult FT::bp_predict_ft(uint64_t& dfe_op_count, uns start_pos) {
  for (size_t idx = start_pos; idx < ops.size(); idx++) {
    Op* op = ops[idx];
    FT_Event event = predict_one_cf_op(op, dfe_op_count);
    if (event != FT_EVENT_NONE) {
      int return_idx = (event == FT_EVENT_MISPREDICT) ? static_cast<int>(idx) : -1;
      Addr pred_addr = op->oracle_info.pred_npc;  // or however you get it
      ASSERT(proc_id, (return_idx != -1) == (event == FT_EVENT_MISPREDICT));
      return {return_idx, event, op, pred_addr};
    }
  }
  return {-1, FT_EVENT_NONE, nullptr, 0};
}

FT_Info FT::get_ft_info() {
  return ft_info;
}

bool FT::is_consumed() {
  return consumed;
}

void FT::set_consumed() {
  consumed = true;
}

std::vector<Op*>& FT::get_ops() {
  return ops;
}

Op* FT::peek_last_op() {
  return ops.back();
}

// Add this method to FT class implementation
int FT::count_cfs_taken_this_ft() const {
  int count = 0;
  for (auto op : ops) {
    if (op->eom) {
      bool cf_taken = op->table_info->cf_type && op->oracle_info.pred == TAKEN;
      bool bar_fetch = IS_CALLSYS(op->table_info) || (op->table_info->bar_type & BAR_FETCH);
      count += (cf_taken || bar_fetch);
    }
  }
  return count;
}

bool FT::is_valid() const {
  return ft_info.static_info.start != 0;
}

bool FT::is_ended() const {
  return ft_info.dynamic_info.ended_by != FT_NOT_ENDED;
}
/* FT wrappers */
bool ft_can_fetch_op(FT* ft) {
  return ft->can_fetch_op();
}

Op* ft_fetch_op(FT* ft) {
  return ft->fetch_op();
}

bool ft_is_consumed(FT* ft) {
  return ft->is_consumed();
}

void ft_set_consumed(FT* ft) {
  ft->set_consumed();
}

FT_Info ft_get_ft_info(FT* ft) {
  return ft->get_ft_info();
}

FT_Ended_By ft_get_ended_by(Op* op, bool use_pred = true) {
  if (op->eom) {
    uns offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size) -
                 ROUND_DOWN(op->inst_info->addr, ICACHE_LINE_SIZE);
    bool end_of_icache_line = offset >= ICACHE_LINE_SIZE;
    bool cf_taken = use_pred ? (op->table_info->cf_type && op->oracle_info.pred == TAKEN)
                             : (op->table_info->cf_type && op->oracle_info.dir == TAKEN);
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