#include "lookahead_buffer.h"

#include <algorithm>
#include <cstdint>
#include <deque>
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

#include "decoupled_frontend.h"
#include "ft.h"
#include "ft_info.h"
#include "op.h"
#include "op_pool.h"
#include "thread.h"

#include "confidence/conf.hpp"

const int CLINE = ~0x3F;

std::vector<FT*> FT_buffer;

std::vector<std::pair<FT_Info_Static, std::vector<unsigned long long>>> ft_info_to_orders;
std::vector<std::pair<FT_Info_Static, std::vector<uint64_t>>> ft_info_to_buf_pos;
std::map<uint64_t, std::vector<FT*>> pc_to_fts;
std::map<uint64_t, std::vector<FT*>> line_addr_to_fts;

uint64_t insert_order = 0;


Flag have_seen_exit = 0;

uint64_t rdptr_lb;
uint64_t rdptr_lb_in_ftq;
uint64_t wrptr_lb;
uint64_t FT_id_count;
uint64_t ft_buffer_count = 0;  // Add this global

void ft_info_to_orders_remove();
void ft_info_to_orders_insert();
Flag match_ft_info(const FT_Info_Static& a, const FT_Info_Static& b);
uint64_t lookahead_buffer_count_valid_FTs();

void lookahead_buffer_refill(uns proc_id);

Flag match_ft_info(const FT_Info_Static& a, const FT_Info_Static& b) {
  return a.start == b.start && a.length == b.length && a.n_uops == b.n_uops;
}

void ft_info_to_orders_insert() {
  FT_Info_Static inserting_FT_info = FT_buffer[wrptr_lb]->get_ft_info().static_info;

  auto it = std::find_if(ft_info_to_orders.begin(), ft_info_to_orders.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, inserting_FT_info); });

  if (it != ft_info_to_orders.end()) {
    it->second.push_back(insert_order);
  } else {
    ft_info_to_orders.push_back({inserting_FT_info, {insert_order}});
  }

  auto it_buf_pos = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                                 [&](const auto& pair) { return match_ft_info(pair.first, inserting_FT_info); });

  if (it_buf_pos != ft_info_to_buf_pos.end()) {
    it_buf_pos->second.push_back(wrptr_lb);
  } else {
    ft_info_to_buf_pos.push_back({inserting_FT_info, {wrptr_lb}});
  }
  FT* ft_ptr = FT_buffer[wrptr_lb];
  for (Addr pc : ft_ptr->get_all_pc()) {
    pc_to_fts[pc].push_back(FT_buffer[wrptr_lb]);
  }

  // Insert into line_addr_to_fts using only ft_info.Static_info.start
  Addr line_addr = ft_ptr->get_ft_info().static_info.start & CLINE;
  line_addr_to_fts[line_addr].push_back(ft_ptr);
  insert_order++;
}

void ft_info_to_orders_remove() {
  // Build FT info using the FT being removed, notice that we need to build from index 0 in case the FT is
  // modified/mispredicted
  FT_Info_Static removing_FT_info = FT_buffer[rdptr_lb_in_ftq]->build_ft_info_from_index(0).static_info;

  // Erase buf_pos entry by matching rdptr_lb_in_ftq, and keep index to erase order consistently
  auto it_pos = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                             [&](const auto& pair) { return match_ft_info(pair.first, removing_FT_info); });
  size_t idx_in_group = SIZE_MAX;
  if (it_pos != ft_info_to_buf_pos.end() && !it_pos->second.empty()) {
    auto& pos_vec = it_pos->second;
    auto it_idx = std::find(pos_vec.begin(), pos_vec.end(), rdptr_lb_in_ftq);
    if (it_idx != pos_vec.end()) {
      idx_in_group = static_cast<size_t>(std::distance(pos_vec.begin(), it_idx));
      pos_vec.erase(it_idx);
    } else {
      // Fallback: erase front if not found
      idx_in_group = 0;
      pos_vec.erase(pos_vec.begin());
    }
    if (pos_vec.empty()) {
      ft_info_to_buf_pos.erase(it_pos);
    }
  }

  // Erase order at the same index to keep alignment
  auto it_order = std::find_if(ft_info_to_orders.begin(), ft_info_to_orders.end(),
                               [&](const auto& pair) { return match_ft_info(pair.first, removing_FT_info); });
  if (it_order != ft_info_to_orders.end() && !it_order->second.empty()) {
    auto& ord_vec = it_order->second;
    if (idx_in_group != SIZE_MAX && idx_in_group < ord_vec.size()) {
      ord_vec.erase(ord_vec.begin() + idx_in_group);
    } else {
      ord_vec.erase(ord_vec.begin());  // Fallback
    }
    if (ord_vec.empty()) {
      ft_info_to_orders.erase(it_order);
    }
  } else {
    // No corresponding entry in ft_info_to_orders; skip order removal to avoid crashing.
  }

  // Remove from PC and line_addr indexes
  FT* ft_ptr = FT_buffer[rdptr_lb_in_ftq];
  for (Addr pc : ft_ptr->get_all_pc()) {
    auto& vec = pc_to_fts[pc];
    vec.erase(std::remove(vec.begin(), vec.end(), ft_ptr), vec.end());
    if (vec.empty()) {
      pc_to_fts.erase(pc);
    }
  }
  Addr line_addr = ft_ptr->get_ft_info().static_info.start & CLINE;
  auto& vec2 = line_addr_to_fts[line_addr];
  vec2.erase(std::remove(vec2.begin(), vec2.end(), ft_ptr), vec2.end());
  if (vec2.empty()) {
    line_addr_to_fts.erase(line_addr);
  }
}

uint64_t lookahead_buffer_count_valid_FTs() {
  return ft_buffer_count;
}

void lookahead_buffer_refill(uns proc_id) {
  if (have_seen_exit)
    return;

  while (ft_buffer_count < LOOKAHEAD_BUF_SIZE) {
    if (have_seen_exit)
      break;
    FT_buffer_insert_FT(proc_id);
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

    for (uint i = 0; i < LOOKAHEAD_BUF_SIZE; i++) {
      FT_buffer_insert_FT(proc_id);
    }
  }
}

void FT_buffer_insert_FT(uns8 proc_id) {
  ASSERT(proc_id, have_seen_exit == 0);
  FT* new_ft = new FT(proc_id);
  Flag build_success = new_ft->build([&](uns8 pid) { return frontend_can_fetch_op(pid); },
                                     [&](uns8 pid, Op* op) {
                                       frontend_fetch_op(pid, op);
                                       return true;
                                     },
                                     false, []() { return decoupled_fe_get_next_on_path_op_num(); });
  ASSERT(proc_id, build_success);
  new_ft->set_from_lookahead(true);
  FT_buffer[wrptr_lb] = new_ft;
  ft_info_to_orders_insert();
  wrptr_lb = (wrptr_lb + 1) % LOOKAHEAD_BUF_SIZE;
  ft_buffer_count++;
  if (new_ft->get_end_reason() == FT_APP_EXIT)
    have_seen_exit = 1;
}

FT* FT_buffer_read_FT(uns proc_id) {
  FT* current_read_FT = FT_buffer[rdptr_lb];
  rdptr_lb = (rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE;

  return current_read_FT;
}

uint64_t pop_ft_in_lookahead = 0;
void lookahead_buffer_pop_ft(uns proc_id, Addr addr, Flag from_lookahead_buffer, int n_uops, Flag off_path,
                             uint64_t pop_count) {
  (void)pop_count;
  // only pop FT if FT op is from lookahead buffer and consumed
  if ((FT_buffer[rdptr_lb_in_ftq]->get_ft_info().static_info.start == addr) && from_lookahead_buffer &&
      n_uops >= FT_buffer[rdptr_lb_in_ftq]->get_ft_info().static_info.n_uops) {
    ft_info_to_orders_remove();
    FT_buffer[rdptr_lb_in_ftq] = nullptr;
    rdptr_lb_in_ftq = (rdptr_lb_in_ftq + 1) % LOOKAHEAD_BUF_SIZE;
    ft_buffer_count--;
    pop_ft_in_lookahead++;
    // only refill lookahead buffer when on-path
    if (!off_path)
      lookahead_buffer_refill(proc_id);
  }

}

Addr lookahead_buffer_next_addr() {
  return FT_buffer[(rdptr_lb) % LOOKAHEAD_BUF_SIZE]->get_ft_info().static_info.start;
}

Flag lookahead_buffer_can_fetch_op(uns proc_id) {
  // can fetch from FT buffer if the current ft have op left or
  // next ft is not consumed and valid
  FT* current_ft = FT_buffer[rdptr_lb];
  if (!current_ft) {
    return 0;
  }
  return current_ft->can_fetch_op();
}

std::vector<FT*> find_FTs(const FT_Info_Static& target_info) {
  std::vector<FT*> result;
  auto it = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, target_info); });

  if (it != ft_info_to_buf_pos.end()) {
    for (uint64_t pos : it->second) {
      if (pos < FT_buffer.size()) {
        result.push_back(FT_buffer[pos]);
      }
    }

    std::sort(result.begin(), result.end(),
              [](FT* a, FT* b) { return a->get_ft_info().dynamic_info.FT_id < b->get_ft_info().dynamic_info.FT_id; });
  }

  return result;
}

FT* find_youngest_FT(const FT_Info_Static& target_info) {
  auto it = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, target_info); });

  FT* youngest = nullptr;

  if (it != ft_info_to_buf_pos.end()) {
    for (uint64_t pos : it->second) {
      if (pos < FT_buffer.size()) {
        FT* candidate = FT_buffer[pos];
        if (!youngest || candidate->get_ft_info().dynamic_info.FT_id > youngest->get_ft_info().dynamic_info.FT_id) {
          youngest = candidate;
        }
      }
    }
  }

  return youngest;
}

std::map<FT_Info_Static, std::vector<FT*>> find_FT_start(uint64_t FT_start) {
  std::map<FT_Info_Static, std::vector<FT*>> result;
  for (FT* ft : FT_buffer) {
    if (!ft)
      continue;  // Skip freed entries
    FT_Info info = ft->get_ft_info();
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
    // Verify the FT is actually in the buffer
    auto it_pos = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                               [&](const auto& pair) { return match_ft_info(pair.first, static_info); });

    if (it_pos != ft_info_to_buf_pos.end() && !it_pos->second.empty()) {
      uint64_t buf_pos = it_pos->second[0];
      if (buf_pos < FT_buffer.size() && FT_buffer[buf_pos] != nullptr) {
        return it->second[0];
      } else {
        return UINT64_MAX;
      }
    } else {
      return UINT64_MAX;
    }
  } else {
    return UINT64_MAX;
  }
}

uint64_t lookahead_buffer_FT_search_buf_pos(FT_Info_Static static_info) {
  auto it = std::find_if(ft_info_to_buf_pos.begin(), ft_info_to_buf_pos.end(),
                         [&](const auto& pair) { return match_ft_info(pair.first, static_info); });

  if (it != ft_info_to_buf_pos.end() && !it->second.empty()) {
    return it->second[0];
  } else {
    return UINT64_MAX;
  }
}

FT_Info_Static lookahead_buffer_ptr_search(uint64_t ptr_pos) {
  if (FT_buffer[ptr_pos] == nullptr)
    return FT_Info_Static();
  // Skip freed entries
  if (FT_buffer[ptr_pos]->get_ft_info().static_info.start)
    return FT_buffer[ptr_pos]->get_ft_info().static_info;
  else
    return FT_Info_Static();
}

// to support map key comparison
inline bool operator<(const FT_Info_Static& a, const FT_Info_Static& b) {
  return std::tie(a.start, a.length, a.n_uops) < std::tie(b.start, b.length, b.n_uops);
}