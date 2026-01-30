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

#include "decoupled_frontend.h"
#include "ft.h"
#include "ft_info.h"
#include "op.h"
#include "op_pool.h"
#include "thread.h"

#include "confidence/conf.hpp"

const int CLINE = ~0x3F;

// operator== for FT_Info_Static
inline bool operator==(const FT_Info_Static& a, const FT_Info_Static& b) {
  return a.start == b.start && a.length == b.length && a.n_uops == b.n_uops;
}

// operator< for FT_Info_Static (used for map key comparison)
inline bool operator<(const FT_Info_Static& a, const FT_Info_Static& b) {
  return std::tie(a.start, a.length, a.n_uops) < std::tie(b.start, b.length, b.n_uops);
}

// Hash function for FT_Info_Static (used for unordered_map key hashing)
struct FTInfoHash {
  size_t operator()(const FT_Info_Static& x) const noexcept {
    size_t h1 = std::hash<uint64_t>{}(x.start);
    size_t h2 = std::hash<uint32_t>{}(x.length);
    size_t h3 = std::hash<uint32_t>{}(x.n_uops);
    return h1 ^ (h2 << 1) ^ (h3 << 2);
  }
};

// Helper struct to allow specialization
template <typename Key>
struct LookaheadIndexMapImpl {
  using type = std::unordered_map<Key, std::deque<uint64_t>>;
};

// Specialization for FT_Info_Static with custom hash
template <>
struct LookaheadIndexMapImpl<FT_Info_Static> {
  using type = std::unordered_map<FT_Info_Static, std::deque<uint64_t>, FTInfoHash>;
};

// Template alias that uses the helper
template <typename Key>
using LookaheadIndexMap = typename LookaheadIndexMapImpl<Key>::type;

/* Wrapper template for lookahead index with STL-like interface */
template <typename Key>
class LookaheadIndex {
 private:
  LookaheadIndexMap<Key> data;

 public:
  /* Insert a new buffer position referenced by the key */
  void insert(const Key& key, uint64_t buf_pos) { data[key].push_back(buf_pos); }

  /* Erase a specific buffer position referenced by the key */
  void erase(const Key& key, uint64_t buf_pos) {
    auto it = data.find(key);
    if (it != data.end()) {
      auto& pos_deque = it->second;
      auto it_pos = std::find(pos_deque.begin(), pos_deque.end(), buf_pos);
      if (it_pos != pos_deque.end()) {
        pos_deque.erase(it_pos);
      }
      if (pos_deque.empty()) {
        data.erase(it);
      }
    }
  }

  auto find(const Key& key) { return data.find(key); }
  auto end() { return data.end(); }
  auto begin() { return data.begin(); }
};

/* ============================================================================
 * LookaheadBuffer Class Definition
 * ============================================================================ */
class LookaheadBuffer {
 private:
  /* the lookahead buffer, contains FT pointers */
  std::vector<FT*> lookahead_buffer;
  /* the mapping from FT info to buffer position, support searching buffer position by FT info */
  LookaheadIndex<FT_Info_Static> ft_info_to_buf_pos;
  /* the mapping from PC to buffer positions, support searching buffer positions by PC  */
  LookaheadIndex<Addr> pc_to_buf_pos;
  /* the mapping from line address to buffer positions, support searching buffer positions by line address */
  LookaheadIndex<Addr> line_addr_to_buf_pos;

  Flag have_seen_exit;

  // the pointers for lookahead buffer read pos
  uint64_t rdptr_lb;
  // the pointers for lookahead buffer write pos
  uint64_t wrptr_lb;
  uint64_t ft_buffer_count;

  /* Reads from frontend until a complete FT is ready, inserts it into the buffer */
  void insert_ft(uns8 proc_id);

  /* Helper method: updates all lookup map/vectors according to new FT inserted at buf pos */
  void update_search_indexes_on_insert(uint64_t buf_pos);

  /* Helper method: updates all lookup map/vectors when removing FT from a buf pos */
  void update_search_indexes_on_remove(uint64_t buf_pos);

 public:
  LookaheadBuffer() : have_seen_exit(0), rdptr_lb(0), wrptr_lb(0), ft_buffer_count(0) {}

  /* Initialization API, used when buffer size is non-zero */
  void init();

  /* Returns a FT at the current read pointer and pops it from the buffer */
  FT* pop_ft(uns proc_id);

  /* Refills lookahead buffer to the parameter size */
  void refill(uns proc_id);

  /* Returns the start address for the FT at read pointer */
  FT* peek();

  /* Returns whether lookahead buffer can currently provide an op */
  Flag can_fetch_op(uns proc_id);

  /* Returns count of valid FTs in buffer */
  uint64_t count_valid_fts();

  /* Returns all FTs in the buffer matching given static FT info */
  std::vector<FT*> find_fts_by_ft_info(const FT_Info_Static& target_info);

  /* Returns all FTs with a given start address */
  std::vector<FT*> find_fts_by_start_addr(uint64_t FT_start_addr);

  /* Returns FTs grouped by static info for a given start address */
  std::map<FT_Info_Static, std::vector<FT*>> get_fts_bygrouped_by_(uint64_t FT_start_addr);

  /* Returns list of FTs containing the given PC */
  std::vector<FT*> find_fts_enclosing_pc(Addr PC);

  /* Returns list of FTs containing the given line address */
  std::vector<FT*> find_fts_enclosing_line_addr(Addr line_addr);

  /* returns oldest oldest FT (by insertion time) of given FT info */
  FT* find_oldest_FT_by_ft_info(FT_Info_Static static_info);

  /* Returns FT at a given buffer position */
  FT* get_FT(uint64_t ptr_pos);

  /* Returns current read pointer position*/
  uint64_t get_rdptr();

  uint64_t count() { return ft_buffer_count; };
};

/* ============================================================================
 * LookaheadBuffer Implementation
 * ============================================================================ */

/* updates all lookup map/vectors according to new FT inserted at buf pos */
void LookaheadBuffer::update_search_indexes_on_insert(uint64_t buf_pos) {
  FT_Info_Static inserting_FT_info = lookahead_buffer[buf_pos]->get_ft_info().static_info;

  ft_info_to_buf_pos.insert(inserting_FT_info, buf_pos);

  FT* ft_ptr = lookahead_buffer[buf_pos];
  for (Addr pc : ft_ptr->get_pcs()) {
    pc_to_buf_pos.insert(pc, buf_pos);
  }

  Addr line_addr = ft_ptr->get_ft_info().static_info.start & CLINE;
  line_addr_to_buf_pos.insert(line_addr, buf_pos);
}

/* updates all lookup map/vectors when removing FT from a buf pos */
void LookaheadBuffer::update_search_indexes_on_remove(uint64_t buf_pos) {
  FT* ft = lookahead_buffer[buf_pos];
  if (!ft)
    return;

  FT_Info_Static removing_FT_info = ft->get_ft_info().static_info;

  ft_info_to_buf_pos.erase(removing_FT_info, buf_pos);

  for (Addr pc : ft->get_pcs()) {
    pc_to_buf_pos.erase(pc, buf_pos);
  }

  Addr line_addr = removing_FT_info.start & CLINE;
  line_addr_to_buf_pos.erase(line_addr, buf_pos);
}

uint64_t LookaheadBuffer::count_valid_fts() {
  return ft_buffer_count;
}

/* Refills lookahead buffer to the parameter size*/
void LookaheadBuffer::refill(uns proc_id) {
  if (have_seen_exit)
    return;

  while (ft_buffer_count < LOOKAHEAD_BUF_SIZE) {
    if (have_seen_exit)
      break;
    insert_ft(proc_id);
  }
}

void LookaheadBuffer::init() {
  if (!LOOKAHEAD_BUF_SIZE)
    return;

  lookahead_buffer.resize(LOOKAHEAD_BUF_SIZE);
  for (uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    rdptr_lb = 0;
    wrptr_lb = 0;

    for (uint i = 0; i < LOOKAHEAD_BUF_SIZE; i++) {
      insert_ft(proc_id);
    }
  }
}

/* Reads from frontend until a complete FT is ready, inserts it into the buffer;
   used to prefill and refill the buffer */
void LookaheadBuffer::insert_ft(uns8 proc_id) {
  ASSERT(proc_id, have_seen_exit == 0);
  FT* new_ft = new FT(proc_id, 0);
  Flag build_success = new_ft->build([&](uns8 pid, uns8 bid) { return frontend_can_fetch_op(pid, bid); },
                                     [&](uns8 pid, uns8 bid, Op* op) {
                                       frontend_fetch_op(pid, bid, op);
                                       return true;
                                     },
                                     false, false, []() { return decoupled_fe_get_next_on_path_op_num(); });
  ASSERT(proc_id, build_success);
  new_ft->set_prebuilt(true);
  lookahead_buffer[wrptr_lb] = new_ft;
  update_search_indexes_on_insert(wrptr_lb);
  wrptr_lb = (wrptr_lb + 1) % LOOKAHEAD_BUF_SIZE;
  ft_buffer_count++;
  if (new_ft->get_end_reason() == FT_APP_EXIT)
    have_seen_exit = 1;
}

/* Returns a FT at the current read pointer */
FT* LookaheadBuffer::pop_ft(uns proc_id) {
  FT* current_read_FT = lookahead_buffer[rdptr_lb];

  update_search_indexes_on_remove(rdptr_lb);
  lookahead_buffer[rdptr_lb] = nullptr;
  rdptr_lb = (rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE;
  ft_buffer_count--;
  refill(proc_id);

  return current_read_FT;
}

/* Returns the start address for the FT at read pointer */
FT* LookaheadBuffer::peek() {
  ASSERT(0, lookahead_buffer[rdptr_lb] != nullptr);
  return lookahead_buffer[(rdptr_lb) % LOOKAHEAD_BUF_SIZE];
}

/* Returns whether lookahead buffer can currently provide an op */
Flag LookaheadBuffer::can_fetch_op(uns proc_id) {
  FT* current_ft = lookahead_buffer[rdptr_lb];
  if (!current_ft) {
    return 0;
  }
  return current_ft->can_fetch_op();
}

/* Returns all FTs in the buffer matching given static FT info
   sorted by FT_id */
std::vector<FT*> LookaheadBuffer::find_fts_by_ft_info(const FT_Info_Static& target_info) {
  std::vector<FT*> result;

  auto it = ft_info_to_buf_pos.find(target_info);

  if (it != ft_info_to_buf_pos.end()) {
    for (uint64_t pos : it->second) {
      if (pos < lookahead_buffer.size() && lookahead_buffer[pos] != nullptr) {  // Check for freed entries
        result.push_back(lookahead_buffer[pos]);
      }
    }

    std::sort(result.begin(), result.end(),
              [](FT* a, FT* b) { return a->get_ft_info().dynamic_info.FT_id < b->get_ft_info().dynamic_info.FT_id; });
  }

  return result;
}

/* Returns all FTs with a given start address */
std::vector<FT*> LookaheadBuffer::find_fts_by_start_addr(uint64_t FT_start_addr) {
  std::vector<FT*> result;

  for (const auto& [static_info, pos_deque] : ft_info_to_buf_pos) {
    if (static_info.start != FT_start_addr)
      continue;

    for (uint64_t pos : pos_deque) {
      if (pos < lookahead_buffer.size() && lookahead_buffer[pos]) {
        result.push_back(lookahead_buffer[pos]);
      }
    }
  }

  return result;
}

/* Returns list of FTs containing the given PC */
std::vector<FT*> LookaheadBuffer::find_fts_enclosing_pc(Addr PC) {
  std::vector<FT*> result;
  // Look up the PC in the map
  auto it = pc_to_buf_pos.find(PC);
  if (it != pc_to_buf_pos.end()) {
    for (uint64_t pos : it->second) {
      if (pos < lookahead_buffer.size() && lookahead_buffer[pos] != nullptr) {
        result.push_back(lookahead_buffer[pos]);
      }
    }
  }

  return result;
}

/* Returns list of FTs containing the given line address */
std::vector<FT*> LookaheadBuffer::find_fts_enclosing_line_addr(Addr line_addr) {
  std::vector<FT*> result;
  // Look up the line address in the map
  auto it = line_addr_to_buf_pos.find(line_addr);
  if (it != line_addr_to_buf_pos.end()) {
    for (uint64_t pos : it->second) {
      if (pos < lookahead_buffer.size() && lookahead_buffer[pos] != nullptr) {
        result.push_back(lookahead_buffer[pos]);
      }
    }
  }

  return result;
}

/* returns oldest FT_id of given FT info
   Now uses O(1) unordered_map lookup */
FT* LookaheadBuffer::find_oldest_FT_by_ft_info(FT_Info_Static static_info) {
  auto it = ft_info_to_buf_pos.find(static_info);
  if (it != ft_info_to_buf_pos.end() && !it->second.empty()) {
    // Verify the FT is actually in the buffer
    auto it_pos = ft_info_to_buf_pos.find(static_info);

    if (it_pos != ft_info_to_buf_pos.end() && !it_pos->second.empty()) {
      uint64_t buf_pos = it_pos->second.front(); /* deque provides efficient front() access */
      if (buf_pos < lookahead_buffer.size() && lookahead_buffer[buf_pos] != nullptr)
        return lookahead_buffer[buf_pos]; /* deque provides efficient front() access */
    }
  }
  return nullptr;
}

/* return FT static info of given buffer position; used in scanning */
FT* LookaheadBuffer::get_FT(uint64_t ptr_pos) {
  // Ensure ptr_pos is within bounds before accessing lookahead_buffer
  if (ptr_pos >= lookahead_buffer.size())
    return nullptr;
  FT* ft_entry = lookahead_buffer[ptr_pos];
  if (ft_entry == nullptr)
    return nullptr;  // Skip null entries
  ASSERT(0, ft_entry->get_ft_info().static_info.start);
  return ft_entry;
}

uint64_t LookaheadBuffer::get_rdptr() {
  return rdptr_lb;
}

/* ============================================================================
 * Global Instance and C-Style Wrapper Functions for Backward Compatibility
 * ============================================================================ */

/* Global instance of LookaheadBuffer */
static LookaheadBuffer g_lookahead_buffer;

/* Initialization API, used when buffer size is non-zero */
void init_lookahead_buffer() {
  g_lookahead_buffer.init();
}

/* Refills lookahead buffer to the parameter size */
void lookahead_buffer_refill(uns proc_id) {
  g_lookahead_buffer.refill(proc_id);
}

/* Pops an ft, Synchronizes lookahead buffer when FT is removed from FTQ */
FT* lookahead_buffer_pop_ft(uns proc_id) {
  ASSERT(proc_id, LOOKAHEAD_BUF_SIZE);
  return g_lookahead_buffer.pop_ft(proc_id);
}

/* Returns the FT to be read next */
FT_Info lookahead_buffer_peek() {
  return g_lookahead_buffer.peek()->get_ft_info();
}

/* Returns whether lookahead buffer can currently provide FT */
Flag lookahead_buffer_can_fetch_op(uns proc_id) {
  return g_lookahead_buffer.can_fetch_op(proc_id);
}

/* Returns all FTs in the buffer matching given static FT info */
std::vector<FT*> lookahead_buffer_find_FTs_by_ft_info(const FT_Info_Static& target_info) {
  return g_lookahead_buffer.find_fts_by_ft_info(target_info);
}

/* find youngest FT by static info
   built from find_by_ft_info primitive */
FT* lookahead_buffer_find_youngest_FT_by_static_info(const FT_Info_Static& target_info) {
  auto fts = g_lookahead_buffer.find_fts_by_ft_info(target_info);
  FT* youngest = nullptr;
  for (auto ft : fts) {
    if (!youngest || ft->get_ft_info().dynamic_info.FT_id > youngest->get_ft_info().dynamic_info.FT_id) {
      youngest = ft;
    }
  }
  return youngest;
}

/* Returns all FTs with a given start address */
std::vector<FT*> lookahead_buffer_find_FTs_by_start_addr(uint64_t FT_start_addr) {
  return g_lookahead_buffer.find_fts_by_start_addr(FT_start_addr);
}

/* Returns list of FTs containing the given PC */
std::vector<FT*> lookahead_buffer_find_FTs_enclosing_PC(Addr PC) {
  return g_lookahead_buffer.find_fts_enclosing_pc(PC);
}

/* Returns list of FTs containing the given line address */
std::vector<FT*> lookahead_buffer_find_FTs_enclosing_line_addr(Addr line_addr) {
  return g_lookahead_buffer.find_fts_enclosing_line_addr(line_addr);
}

/* returns oldest FT of given FT info */
FT* lookahead_buffer_find_oldest_FT_by_FT_info(FT_Info_Static static_info) {
  return g_lookahead_buffer.find_oldest_FT_by_ft_info(static_info);
}

/* Searches for FT static info by buffer position; used in scanning */
FT* lookahead_buffer_get_FT(uint64_t ptr_pos) {
  return g_lookahead_buffer.get_FT(ptr_pos);
}

/* Returns current read pointer position included in FTQ */
uint64_t lookahead_buffer_rdptr() {
  return g_lookahead_buffer.get_rdptr();
}

/* Returns number of FTs in buffer */
uint64_t lookahead_buffer_count() {
  return g_lookahead_buffer.count();
}