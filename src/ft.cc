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

#include "globals/assert.h"

#include "memory/memory.param.h"

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
      // assert consecutivity
      ASSERT(proc_id, ops.back()->inst_info->addr + ops.back()->inst_info->trace_info.inst_size == op->inst_info->addr);
    } else {
      // assert all uops of the same inst share the same addr
      ASSERT(proc_id, ops.back()->inst_info->addr == op->inst_info->addr);
    }
  }
  ops.emplace_back(op);
  if (ft_ended_by != FT_NOT_ENDED) {
    ASSERT(proc_id, op->eom && !ft_info.static_info.length);
    ASSERT(proc_id, ft_info.static_info.start);
    ft_info.static_info.n_uops = ops.size();
    ft_info.static_info.length = op->inst_info->addr + op->inst_info->trace_info.inst_size - ft_info.static_info.start;
    ASSERT(proc_id, ft_info.dynamic_info.ended_by == FT_NOT_ENDED);
    ft_info.dynamic_info.ended_by = ft_ended_by;

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

void FT::build_on_path_ft(uns8 proc_id, std::function<bool(uns8, Op*)> fetch_op_fn, FT last_ft, Flag off_path,
                          Flag from_lookahead_buffer) {
  FT_Ended_By ft_ended_by = FT_NOT_ENDED;
  ft_info.dynamic_info.FT_id = FT_id_count++;

  while (ft_ended_by == FT_NOT_ENDED) {
    Op* op = alloc_op(proc_id);
    bool fetched = fetch_op_fn(proc_id, op);
    if (!fetched)
      return;
    op->off_path = off_path;
    ft_ended_by = get_ft_ended_by(op, false);

    add_op(op, ft_ended_by);
  }
  if (ft_ended_by != FT_NOT_ENDED) {
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
        ASSERT(proc_id, last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size == start_addr);
      }
    }
  }
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

FT_Ended_By get_ft_ended_by(Op* op, bool use_pred = true) {
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