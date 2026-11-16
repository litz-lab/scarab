#ifndef __LOOKAHEAD_BUFFER_H__
#define __LOOKAHEAD_BUFFER_H__

#include <stdint.h>

#include "globals/global_types.h"

#include "ft.h"

#ifdef __cplusplus
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/utils.h"

#include "ft_info.h"
#include "op.h"

extern "C" {
#endif
// C-compatible APIs

/* Allocation API for per-core lookahead buffer instances */
void alloc_mem_lookahead_buffer(uns num_cores);

/* Pops an FT and synchronizes lookahead buffer state */
FT* lookahead_buffer_pop_ft(uns proc_id);

/* init lookahead buffer up to configured size */
void init_lookahead_buffer(uns proc_id);

/* Returns whether lookahead buffer can currently provide an op */
Flag lookahead_buffer_can_fetch_op(uns proc_id);

/* Searches for FT static info by buffer position; used in scanning */
FT* lookahead_buffer_get_FT(uns proc_id, uint64_t ptr_pos);

/* Returns current read pointer position included in FTQ */
uint64_t lookahead_buffer_rdptr(uns proc_id);

/* Returns number of FTs in buffer */
uint64_t lookahead_buffer_count(uns proc_id);

#ifdef __cplusplus
}

struct FTInfoHash {
  size_t operator()(const FT_Info_Static& x) const noexcept {
    size_t h1 = std::hash<uint64_t>{}(x.start);
    size_t h2 = std::hash<uint32_t>{}(x.length);
    size_t h3 = std::hash<uint32_t>{}(x.n_uops);
    return h1 ^ (h2 << 1) ^ (h3 << 2);
  }
};

template <typename Key>
struct LookaheadIndexMapImpl {
  using type = std::unordered_map<Key, std::deque<uint64_t>>;
};

template <>
struct LookaheadIndexMapImpl<FT_Info_Static> {
  using type = std::unordered_map<FT_Info_Static, std::deque<uint64_t>, FTInfoHash>;
};

template <typename Key>
using LookaheadIndexMap = typename LookaheadIndexMapImpl<Key>::type;

template <typename Key>
class LookaheadIndex {
 private:
  LookaheadIndexMap<Key> data;

 public:
  void insert(const Key& key, uint64_t buf_pos) { data[key].push_back(buf_pos); }

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

class LookaheadBuffer {
 private:
  std::vector<FT*> lookahead_buffer;
  LookaheadIndex<FT_Info_Static> ft_info_to_buf_pos;
  LookaheadIndex<Addr> pc_to_buf_pos;
  LookaheadIndex<Addr> line_addr_to_buf_pos;

  Flag have_seen_exit;
  uns proc_id;

  uint64_t rdptr_lb;
  uint64_t wrptr_lb;
  uint64_t ft_buffer_count;

  void insert_ft();
  void update_search_indexes_on_insert(uint64_t buf_pos);
  void update_search_indexes_on_remove(uint64_t buf_pos);

 public:
  /* Constructor initializes per-core lookahead buffer state */
  explicit LookaheadBuffer(uns proc_id);
  ~LookaheadBuffer();

  /* Pops an FT and synchronizes lookahead buffer when FT is removed from FTQ */
  FT* pop_ft();

  /* init lookahead buffer to the configured size */
  void init();

  /* Returns the FT at current read pointer without removing it */
  FT* peek();

  /* Returns whether lookahead buffer can currently provide an op */
  Flag can_fetch_op();

  /* Returns count of valid FTs currently in buffer */
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

  /* Returns oldest insertion-order FT of given FT info */
  FT* find_oldest_FT_by_ft_info(FT_Info_Static static_info);

  /* Searches for FT static info by buffer position; used in scanning */
  FT* get_FT(uint64_t ptr_pos);

  /* Returns current read pointer position included in FTQ */
  uint64_t get_rdptr();

  /* Returns number of FTs in buffer */
  uint64_t count() { return ft_buffer_count; };
};

/* Returns all FTs in the buffer matching given static FT info */
std::vector<FT*> lookahead_buffer_find_fts_by_ft_info(uns proc_id, const FT_Info_Static& target_info);

/* find youngest FT by static info built from find_by_static_info primitive */
FT* lookahead_buffer_find_youngest_ft_by_static_info(uns proc_id, const FT_Info_Static& target_info);

/* Returns all FTs with a given start address */
std::vector<FT*> lookahead_buffer_find_fts_by_start_addr(uns proc_id, uint64_t ft_start_addr);

/* Returns list of FTs containing the given PC */
std::vector<FT*> lookahead_buffer_find_fts_enclosing_pc(uns proc_id, Addr pc);

/* Returns list of FTs containing the given line address */
std::vector<FT*> lookahead_buffer_find_fts_enclosing_line_addr(uns proc_id, Addr line_addr);

/* returns oldest insertion order FT of given FT info */
FT* lookahead_buffer_find_oldest_ft_by_info(uns proc_id, FT_Info_Static static_info);
#endif

#endif
