#ifndef __LOOKAHEAD_BUFFER_H__
#define __LOOKAHEAD_BUFFER_H__

#include <cstdint>
#include <deque>
#include <map>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "ft.h"
#include "ft_info.h"
#include "op.h"

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
  uns proc_id;

  // the pointers for lookahead buffer read pos
  uint64_t rdptr_lb;
  // the pointers for lookahead buffer write pos
  uint64_t wrptr_lb;
  uint64_t ft_buffer_count;

  /* Reads from frontend until a complete FT is ready, inserts it into the buffer */
  void insert_ft();

  /* Helper method: updates all lookup map/vectors according to new FT inserted at buf pos */
  void update_search_indexes_on_insert(uint64_t buf_pos);

  /* Helper method: updates all lookup map/vectors when removing FT from a buf pos */
  void update_search_indexes_on_remove(uint64_t buf_pos);

 public:
  LookaheadBuffer() : have_seen_exit(0), rdptr_lb(0), wrptr_lb(0), ft_buffer_count(0) {}

  /* Initialization API, used when buffer size is non-zero */
  void init(uns proc_id);

  /* Returns a FT at the current read pointer and pops it from the buffer */
  FT* pop_ft();

  /* Refills lookahead buffer to the parameter size */
  void refill();

  /* Returns the start address for the FT at read pointer */
  FT* peek();

  /* Returns whether lookahead buffer can currently provide an op */
  Flag can_fetch_op();

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
#endif