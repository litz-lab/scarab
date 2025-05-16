#include "lookahead_buffer.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <tuple>
#include <vector>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"

#include "frontend/frontend_intf.h"
#include "isa/isa_macros.h"

#include "ft.h"
#include "ft_info.h"
#include "op.h"
#include "op_pool.h"
#include "thread.h"

#include "confidence/conf.hpp"

const int CLINE = ~0x3F;

std::vector<FT> FT_buffer;
std::vector<Addr> deferred_FT_pop_addr;
std::vector<std::pair<FT_Info_Static, std::vector<unsigned long long>>> ft_info_to_orders;
std::vector<std::pair<FT_Info_Static, std::vector<uint64_t>>> ft_info_to_buf_pos;
std::map<uint64_t, std::vector<FT*>> pc_to_fts;
std::map<uint64_t, std::vector<FT*>> line_addr_to_fts;

uint64_t insert_order = 0;
FT current_ft_to_push(0);
uns off_path_lookahead = 0;
uns off_path_ft = 0;

uint64_t rdptr_lb;
uint64_t rdptr_lb_in_ftq;
uint64_t wrptr_lb;
uint64_t FT_id_count;

void ft_info_to_orders_remove();
void ft_info_to_orders_insert();
bool match_ft_info(const FT_Info_Static& a, const FT_Info_Static& b);

bool match_ft_info(const FT_Info_Static& a, const FT_Info_Static& b) {
  return a.start == b.start && a.length == b.length && a.n_uops == b.n_uops;
}

void ft_info_to_orders_insert() {
  FT_Info_Static inserting_FT_info = FT_buffer[wrptr_lb].get_ft_info().static_info;

  auto it = std::find_if(ft_info_to_orders.begin(), ft_info_to_orders.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, inserting_FT_info); });

  if (it != ft_info_to_orders.end()) {
    it->second.push_back(insert_order);
  } else {
    ft_info_to_orders.push_back({inserting_FT_info, {insert_order}});
  }

  auto it_ = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                          [&](const auto& pair) { return match_ft_info(pair.first, inserting_FT_info); });

  if (it_ != ft_info_to_buf_pos.end()) {
    it_->second.push_back(wrptr_lb);
  } else {
    ft_info_to_buf_pos.push_back({inserting_FT_info, {wrptr_lb}});
  }
  FT* ft_ptr = &FT_buffer[wrptr_lb];
  for (Addr pc : ft_ptr->get_all_pc()) {
    pc_to_fts[pc].push_back(&FT_buffer[wrptr_lb]);
  }

  // Insert into line_addr_to_fts using only ft_info.Static_info.start
  Addr line_addr = ft_ptr->get_ft_info().static_info.start & CLINE;
  line_addr_to_fts[line_addr].push_back(ft_ptr);

  insert_order++;
}

void ft_info_to_orders_remove() {
  FT_Info_Static removing_FT_info = FT_buffer[rdptr_lb_in_ftq].get_ft_info().static_info;

  auto it = std::find_if(ft_info_to_orders.begin(), ft_info_to_orders.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, removing_FT_info); });

  if (it != ft_info_to_orders.end() && !it->second.empty()) {
    it->second.erase(it->second.begin());
    if (it->second.empty()) {
      ft_info_to_orders.erase(it);
    }
  }

  auto it_ = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                          [&](const auto& pair) { return match_ft_info(pair.first, removing_FT_info); });

  if (it_ != ft_info_to_buf_pos.end() && !it_->second.empty()) {
    it_->second.erase(it_->second.begin());
    if (it_->second.empty()) {
      ft_info_to_buf_pos.erase(it_);
    }
  }

  FT* ft_ptr = &FT_buffer[rdptr_lb_in_ftq];
  for (Addr pc : ft_ptr->get_all_pc()) {
    auto& vec = pc_to_fts[pc];
    vec.erase(std::remove(vec.begin(), vec.end(), ft_ptr), vec.end());
    if (vec.empty()) {
      pc_to_fts.erase(pc);
    }
  }

  // Remove from line_addr_to_fts using only ft_info.Static_info.start
  Addr line_addr = ft_ptr->get_ft_info().static_info.start & CLINE;
  auto& vec2 = line_addr_to_fts[line_addr];
  vec2.erase(std::remove(vec2.begin(), vec2.end(), ft_ptr), vec2.end());
  if (vec2.empty()) {
    line_addr_to_fts.erase(line_addr);
  }
}

void init_lookahead_buffer() {
  if (!LOOKAHEAD_BUF_SIZE)
    return;

  FT_buffer.resize(LOOKAHEAD_BUF_SIZE);
  for (uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    rdptr_lb = 0;
    wrptr_lb = 0;
    rdptr_lb_in_ftq = 0;
    FT_id_count = 0;

    current_ft_to_push = FT();
    current_ft_to_push.set_ft_started_by(FT_STARTED_BY_APP);

    for (uint i = 0; i < LOOKAHEAD_BUF_SIZE; i++) {
      FT_buffer_insert_FT(proc_id);
    }
  }
}

void FT_buffer_insert_FT(uns8 proc_id) {
  if (off_path_lookahead) {
    off_path_ft += 1;
    return;
  }

  FT_Ended_By ft_ended_by = FT_NOT_ENDED;
  current_ft_to_push.write_FT_id(FT_id_count++);

  while (ft_ended_by == FT_NOT_ENDED) {
    if (!frontend_can_fetch_op(proc_id))
      return;
    Op* op = alloc_op(proc_id);
    frontend_fetch_op(proc_id, op);

    if (op->eom) {
      uns offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size) -
                   ROUND_DOWN(op->inst_info->addr, ICACHE_LINE_SIZE);
      bool end_of_icache_line = offset >= ICACHE_LINE_SIZE;
      bool cf_taken = op->table_info->cf_type && op->oracle_info.dir == TAKEN;
      bool bar_fetch = IS_CALLSYS(op->table_info) || op->table_info->bar_type & BAR_FETCH;

      if (op->exit) {
        ft_ended_by = FT_APP_EXIT;
      } else if (bar_fetch) {
        ft_ended_by = FT_BAR_FETCH;
      } else if (cf_taken) {
        ft_ended_by = FT_TAKEN_BRANCH;
      } else if (end_of_icache_line) {
        ft_ended_by = FT_ICACHE_LINE_BOUNDARY;
      }
    }

    current_ft_to_push.add_op(op, ft_ended_by);
  }

  if (ft_ended_by != FT_NOT_ENDED) {
    current_ft_to_push.set_per_op_ft_info();

    if (FT_buffer.size() > 0 && wrptr_lb > 0) {
      Op* last_op = FT_buffer[wrptr_lb - 1].peek_last_op();
      if (FT_buffer[wrptr_lb - 1].get_ft_info().dynamic_info.ended_by == FT_TAKEN_BRANCH) {
        ASSERT(proc_id, last_op->oracle_info.npc == current_ft_to_push.get_ft_info().static_info.start);
      } else if (FT_buffer[wrptr_lb - 1].get_ft_info().dynamic_info.ended_by == FT_BAR_FETCH) {
        ASSERT(proc_id, last_op->oracle_info.npc == current_ft_to_push.get_ft_info().static_info.start ||
                            last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size ==
                                current_ft_to_push.get_ft_info().static_info.start);
      } else {
        ASSERT(proc_id, last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size ==
                            current_ft_to_push.get_ft_info().static_info.start);
      }
    }
    FT_buffer[wrptr_lb] = current_ft_to_push;
    ft_info_to_orders_insert();

    current_ft_to_push = FT(proc_id);

    if (ft_ended_by == FT_ICACHE_LINE_BOUNDARY) {
      current_ft_to_push.set_ft_started_by(FT_STARTED_BY_ICACHE_LINE_BOUNDARY);
    } else if (ft_ended_by == FT_TAKEN_BRANCH) {
      current_ft_to_push.set_ft_started_by(FT_STARTED_BY_TAKEN_BRANCH);
    } else if (ft_ended_by == FT_BAR_FETCH) {
      current_ft_to_push.set_ft_started_by(FT_STARTED_BY_BAR_FETCH);
    }
  }
  wrptr_lb = (wrptr_lb + 1) % LOOKAHEAD_BUF_SIZE;
}

Op* FT_buffer_read_Op(uns proc_id) {
  if (!FT_buffer[rdptr_lb].can_fetch_op()) {
    FT_buffer[rdptr_lb].set_consumed();
    if (deferred_FT_pop_addr.size() > 0 &&
        (FT_buffer[rdptr_lb].get_ft_info().static_info.start == *deferred_FT_pop_addr.begin() ||
         FT_buffer[rdptr_lb + off_path_ft].get_ft_info().static_info.start == deferred_FT_pop_addr[0])) {
      ft_info_to_orders_remove();
      FT_buffer[rdptr_lb_in_ftq] = FT();
      rdptr_lb_in_ftq = (rdptr_lb_in_ftq + 1) % LOOKAHEAD_BUF_SIZE;
      FT_buffer_insert_FT(proc_id);
      deferred_FT_pop_addr.erase(deferred_FT_pop_addr.begin());
    }
    rdptr_lb = (rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE;
  }

  if (FT_buffer[rdptr_lb].can_fetch_op()) {
    auto return_op = FT_buffer[rdptr_lb].fetch_op();
    return_op->from_lookahead_buffer = 1;
    return return_op;
  } else {
    ASSERT(proc_id, 0);
    return NULL;
  }
}

void lookahead_buffer_pop_ft(uns proc_id, Addr addr, Flag from_lookahead_buffer) {
  // only pop FT if FT op is from lookahead buffer and consumed
  if ((FT_buffer[rdptr_lb_in_ftq + deferred_FT_pop_addr.size()].get_ft_info().static_info.start == addr) &&
      from_lookahead_buffer) {
    if (FT_buffer[rdptr_lb_in_ftq].is_consumed()) {
      ft_info_to_orders_remove();
      FT_buffer[rdptr_lb_in_ftq] = FT();
      rdptr_lb_in_ftq = (rdptr_lb_in_ftq + 1) % LOOKAHEAD_BUF_SIZE;
      FT_buffer_insert_FT(proc_id);

    } else {
      // if FT is not consumed, add to deferred pop list
      // to be popped when FT is consumed in FT_buffer_read_Op
      deferred_FT_pop_addr.push_back(addr);
    }
  }
  return;
}

Addr lookahead_buffer_next_addr() {
  if (FT_buffer[rdptr_lb].can_fetch_op()) {
    return FT_buffer[rdptr_lb].peek_next_op()->inst_info->addr;
  }
  return FT_buffer[(rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE].get_ft_info().static_info.start;
}

Flag lookahead_buffer_can_fetch_op(uns proc_id) {
  // can fetch from FT buffer if the current ft have op left or
  // next ft is not consumed and valid
  return FT_buffer[rdptr_lb].can_fetch_op() ||
         (!FT_buffer[(rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE].is_consumed() &&
          FT_buffer[(rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE].get_ft_info().static_info.start);
}

Op* lookahead_buffer_fetch_op(uns proc_id, Op* new_op) {
  if (off_path_lookahead) {
    new_op = alloc_op(proc_id);
    frontend_fetch_op(proc_id, new_op);
    new_op->exit = false;
    new_op->from_lookahead_buffer = false;
    return new_op;
  }
  // if still can read from FT buffer
  // current ft have op left or
  // next ft is not consumed and valid
  if (!off_path_lookahead &&
      (FT_buffer[rdptr_lb].can_fetch_op() || !FT_buffer[(rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE].is_consumed())) {
    auto ret_op = FT_buffer_read_Op(proc_id);
    ;
    return ret_op;
  }

  ASSERT(proc_id, 0);
  return nullptr;
}

std::vector<FT> find_FTs(const FT_Info_Static& target_info) {
  std::vector<FT> result;

  auto it = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, target_info); });

  if (it != ft_info_to_buf_pos.end()) {
    for (uint64_t pos : it->second) {
      if (pos < FT_buffer.size()) {
        result.push_back(FT_buffer[pos]);
      }
    }

    std::sort(result.begin(), result.end(), [](FT& a, FT& b) { return a.get_FT_id() < b.get_FT_id(); });
  }

  return result;
}

FT find_youngest_FT(const FT_Info_Static& target_info) {
  auto it = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, target_info); });

  FT* youngest = nullptr;

  if (it != ft_info_to_buf_pos.end()) {
    for (uint64_t pos : it->second) {
      if (pos < FT_buffer.size()) {
        FT& candidate = FT_buffer[pos];
        if (!youngest || candidate.get_FT_id() > youngest->get_FT_id()) {
          youngest = &candidate;
        }
      }
    }
  }

  if (!youngest) {
    throw std::runtime_error("No matching FT found");
  }

  return *youngest;
}

std::map<FT_Info_Static, std::vector<FT>> find_FT_start(uint64_t FT_start) {
  std::map<FT_Info_Static, std::vector<FT>> result;

  for (FT& ft : FT_buffer) {
    FT_Info info = ft.get_ft_info();
    FT_Info_Static static_info = info.static_info;

    if (static_info.start == FT_start) {
      result[static_info].push_back(ft);
    }
  }

  return result;
}

std::vector<FT*> get_enclosing_FTs(Addr PC) {
  std::vector<FT*> result;

  // Look up the PC in the map
  auto it = pc_to_fts.find(PC);
  if (it != pc_to_fts.end()) {
    // Return a copy of the vector of FT*'s
    result = it->second;
  }

  return result;
}

std::vector<FT*> get_enclosing_FTs_line_addr(Addr line_addr) {
  std::vector<FT*> result;

  // Look up the line address in the map
  auto it = line_addr_to_fts.find(line_addr);
  if (it != line_addr_to_fts.end()) {
    // Return a copy of the vector of FT*'s
    result = it->second;
  }

  return result;
}

uint64_t lookahead_buffer_FT_search(FT_Info_Static static_info) {
  auto it = std::find_if(ft_info_to_orders.begin(), ft_info_to_orders.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, static_info); });

  if (it != ft_info_to_orders.end() && !it->second.empty()) {
    return it->second[0];
  } else {
    return 0;
  }
}

uint64_t lookahead_buffer_FT_search_buf_pos(FT_Info_Static static_info) {
  auto it = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, static_info); });

  if (it != ft_info_to_buf_pos.end() && !it->second.empty()) {
    return it->second[0] + 1;
  } else {
    return 0;
  }
}

FT_Info_Static lookahead_buffer_ptr_search(uint64_t ptr_pos) {
  if (FT_buffer[ptr_pos].get_ft_info().static_info.start)
    return FT_buffer[ptr_pos].get_ft_info().static_info;
  else
    return FT_Info_Static();
}

void lookahead_buffer_redirect() {
  // add additional frontend call for execution-driven support here
  off_path_lookahead = 1;
}

void lookahead_buffer_recover() {
  // add additional frontend call for execution-driven support here
  off_path_lookahead = 0;
  for (uns i = 0; i < off_path_ft; i++) {
    FT_buffer_insert_FT(0);
  }
  off_path_ft = 0;
}

uint64_t lookahead_buffer_rdptr() {
  return rdptr_lb_in_ftq;
}

inline bool operator<(const FT_Info_Static& a, const FT_Info_Static& b) {
  return std::tie(a.start, a.length, a.n_uops) < std::tie(b.start, b.length, b.n_uops);
}