// CPP implementation of a cache.

#include <iostream>
#include <limits>
#include <list>
#include <unordered_map>
#include <vector>

#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "libs/cpp_cache.h"
#include "lookahead_buffer.h"

extern "C" {
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "memory/memory.param.h"
}

#define CPPC_DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CPP_CACHE, ##args)
#define UOP_CACHE_LINE_SIZE ICACHE_LINE_SIZE

template <typename User_Key_Type, typename User_Data_Type>
struct Entry {
  Flag valid;
  User_Key_Type key;
  User_Data_Type data;
  // for LRU replacement policy
  Counter accessed_cycle;
};

template <typename User_Key_Type, typename User_Data_Type>
class Set {
 public:
  std::vector<Entry<User_Key_Type, User_Data_Type>> entries;
  // for round-robin replacement policy
  uns next_evict;
};

template <typename User_Key_Type, typename User_Data_Type>
class Cpp_Cache {
 protected:
  std::vector<Set<User_Key_Type, User_Data_Type>> sets;
  Repl_Policy repl_policy;
  uns assoc;
  uns num_sets;
  uns line_bytes;

  // defines how to hash the key to the set index, need to be implemented by the user
  virtual uns set_idx_hash(User_Key_Type key) = 0;

  // replacement policy functions
  virtual void update_repl_states(Set<User_Key_Type, User_Data_Type>& set, uns hit_idx);
  virtual uns get_repl_idx(Set<User_Key_Type, User_Data_Type>& set, uns begin);

 public:
  Cpp_Cache() = default;
  Cpp_Cache(uns nl, uns asc, uns lb, Repl_Policy rp) {
    assoc = asc;
    num_sets = nl / assoc;
    line_bytes = lb;
    repl_policy = rp;

    sets = std::vector<Set<User_Key_Type, User_Data_Type>>(num_sets);
    for (uns i = 0; i < num_sets; i++) {
      sets[i].entries.resize(assoc);
    }
  }

  User_Data_Type* access(User_Key_Type key, bool update_repl);
  Entry<User_Key_Type, User_Data_Type> insert(User_Key_Type key, User_Data_Type data, uns begin);
  Entry<User_Key_Type, User_Data_Type> invalidate(User_Key_Type key);
  bool check_bypass(User_Key_Type key);
};

template <typename User_Key_Type, typename User_Data_Type>
void Cpp_Cache<User_Key_Type, User_Data_Type>::update_repl_states(Set<User_Key_Type, User_Data_Type>& set,
                                                                  uns hit_idx) {
  switch (repl_policy) {
    case REPL_TRUE_LRU: {
      set.entries[hit_idx].accessed_cycle = cycle_count;
    } break;
    case REPL_RANDOM: {
    } break;
    case REPL_ROUND_ROBIN: {
    } break;
    case BELADY_UOP: {
      set.entries[hit_idx].accessed_cycle = cycle_count;
    } break;
    default:
      ASSERT(0, FALSE);  // unsupported
  }
}

template <typename User_Key_Type, typename User_Data_Type>
uns Cpp_Cache<User_Key_Type, User_Data_Type>::get_repl_idx(Set<User_Key_Type, User_Data_Type>& set, uns begin) {
  // if there are invalid entries, replace them
  for (uns i = 0; i < assoc; i++) {
    Entry<User_Key_Type, User_Data_Type> entry = set.entries[i];
    if (!entry.valid) {
      return i;
    }
  }

  // otherwise, replace according to the policy
  uns repl_idx = 0;
  switch (repl_policy) {
    case REPL_TRUE_LRU: {
      Counter lru_cycle = std::numeric_limits<Counter>::max();
      for (uns i = 0; i < assoc; i++) {
        Entry<User_Key_Type, User_Data_Type> entry = set.entries[i];
        // find smallest access cycle
        if (entry.accessed_cycle < lru_cycle) {
          repl_idx = i;
          lru_cycle = entry.accessed_cycle;
        }
      }
    } break;
    case REPL_RANDOM: {
      repl_idx = rand() % assoc;
    } break;
    case REPL_ROUND_ROBIN: {
      repl_idx = (set.next_evict + 1) % assoc;
      set.next_evict = repl_idx;
    } break;
    case BELADY_UOP: {
      // for insertion of a line that is not ft beginning, cannot choose the most recently used one because it may 
      // end up evicting the FT self and cause an infinite loop
      Counter mru_cycle = std::numeric_limits<Counter>::min();
      for (uns i = 0; i < assoc; i++) {
        Entry<User_Key_Type, User_Data_Type> entry = set.entries[i];
        if (set.entries[i].valid && entry.accessed_cycle > mru_cycle) {
          mru_cycle = entry.accessed_cycle;
        }
      }

      uint64_t fartherest_timestamp = 0;
      for (uns i = 0; i < assoc; i++) {
        if(set.entries[i].valid && set.entries[i].data.begin_of_ft){
          if(mru_cycle == set.entries[i].accessed_cycle && !begin){
            //skip mru one if not begin of ft
            // STAT_EVENT(0, BELADY_REPL_MRU_SELECTED);
            continue;
          }

          auto curr_timestamp = lookahead_buffer_FT_search(set.entries[i].key.second);
          if(curr_timestamp  && curr_timestamp > fartherest_timestamp){
            fartherest_timestamp = curr_timestamp;
            repl_idx = i;

          }
          if(!curr_timestamp){
            fartherest_timestamp = std::numeric_limits<Addr>::max();
            repl_idx = i;
          }

        } 
      }

      if(mru_cycle == set.entries[repl_idx].accessed_cycle && !begin){
        Counter lru_cycle = std::numeric_limits<Counter>::max();
        for (uns j = 0; j < assoc; j++) {
          Entry<User_Key_Type, User_Data_Type> entry = set.entries[j];
          if (entry.valid && entry.accessed_cycle < lru_cycle) {
            repl_idx = j;
            lru_cycle = entry.accessed_cycle;
          }
        }
        }
        break;

    }
    default:
      ASSERT(0, FALSE);  // unsupported
  }
  return repl_idx;
}

// access: Looks up the cache based on key. Returns pointer to line data if found
template <typename User_Key_Type, typename User_Data_Type>
User_Data_Type* Cpp_Cache<User_Key_Type, User_Data_Type>::access(User_Key_Type key, bool update_repl) {
  User_Data_Type* data = NULL;
  uns set_idx = set_idx_hash(key);
  for (uns i = 0; i < assoc; i++) {
    Entry<User_Key_Type, User_Data_Type>& entry = sets[set_idx].entries[i];
    if (entry.valid && entry.key == key) {  // hit
      data = &entry.data;
      if (update_repl) {
        update_repl_states(sets[set_idx], i);
      }
      break;
    }
  }
  return data;
}

template <typename User_Key_Type, typename User_Data_Type>
Entry<User_Key_Type, User_Data_Type> Cpp_Cache<User_Key_Type, User_Data_Type>::insert(User_Key_Type key,
                                                                                      User_Data_Type data, uns begin) {
  // first check if line exists
  ASSERT(0, access(key, FALSE) == NULL);

  uns set_idx = set_idx_hash(key);
  // if the set is full, repl_idx will be overwritten;
  // if the set has vacancy, repl_idx will point to an invalid line
  uns repl_idx = get_repl_idx(sets[set_idx],begin);

  Entry<User_Key_Type, User_Data_Type> evicted_entry = sets[set_idx].entries[repl_idx];
  sets[set_idx].entries[repl_idx] = Entry<User_Key_Type, User_Data_Type>{TRUE, key, data, 0};
  update_repl_states(sets[set_idx], repl_idx);

  // the evicted entry is an eviciton victim if and only if it is valid
  return evicted_entry;
}

template <typename User_Key_Type, typename User_Data_Type>
Entry<User_Key_Type, User_Data_Type> Cpp_Cache<User_Key_Type, User_Data_Type>::invalidate(User_Key_Type key) {
  uns set_idx = set_idx_hash(key);
  Entry<User_Key_Type, User_Data_Type> invalidated_entry{};
  for (uns i = 0; i < assoc; i++) {
    Entry<User_Key_Type, User_Data_Type>& entry = sets[set_idx].entries[i];
    if (entry.valid && entry.key == key) {  // hit
      invalidated_entry = entry;
      entry.valid = FALSE;
      break;
    }
  }
  return invalidated_entry;
}

template <typename User_Key_Type, typename User_Data_Type>
bool Cpp_Cache<User_Key_Type, User_Data_Type>::check_bypass(User_Key_Type key){
  std::unordered_map<Addr, std::vector<std::pair<int, bool>>> uop_count_map;
  std::vector<Addr> line_addrs;
  std::vector<int> n_uop;

  uint64_t inserting_addr_orders = lookahead_buffer_FT_search(key.second);
  line_addrs.push_back(key.first);
  n_uop.push_back(key.second.n_uops);
  int required_length = (key.second.n_uops + UOP_CACHE_WIDTH - 1) / UOP_CACHE_WIDTH;

  if(!inserting_addr_orders){
    STAT_EVENT(0, BELADY_BYPASS_FIRST_NOT_FOUND);
    return 1;
  }else{
    STAT_EVENT(0, BELADY_BYPASS_FIRST_FOUND);
    int available_space = UOP_CACHE_ASSOC;
    uns set_idx = set_idx_hash(key);
    auto set = sets[set_idx];

    for (uns i = 0; i < assoc; i++) {
      if(set.entries[i].valid && set.entries[i].data.end_of_ft){
        auto existing_orders = lookahead_buffer_FT_search(set.entries[i].key.second);
        line_addrs.push_back(set.entries[i].key.first);
        n_uop.push_back(set.entries[i].key.second.n_uops);
        if(existing_orders && existing_orders < inserting_addr_orders){
          available_space -= (set.entries[i].key.second.n_uops + UOP_CACHE_WIDTH - 1) / UOP_CACHE_WIDTH;
          if(available_space < required_length){
            return 1;
          }  
        }
      } 
    }
    if(available_space < 0 || available_space < required_length){
      return 1;
    }
    uns offset_bits = LOG2(UOP_CACHE_LINE_SIZE);
    uns num_sets = UOP_CACHE_LINES / UOP_CACHE_ASSOC;
    uns target_set = (key.first >> offset_bits) % num_sets;
    uint64_t rd_ptr_pos = lookahead_buffer_rdptr();
    // checked inserting and existing addrs, now scan the buffer for not in fts with more than 1 use
    uint64_t scan_pos = lookahead_buffer_FT_search_buf_pos(key.second);
    if (scan_pos == 0) {
      printf("not found in lookahead buffer\n");
      // ASSERT(0, FALSE);
      return 1;
    }
    if (scan_pos < rd_ptr_pos) 
      scan_pos += LOOKAHEAD_BUF_SIZE;
    for (uint64_t i = rd_ptr_pos; i < scan_pos; i++) {
      FT_Info_Static ft_info_in = lookahead_buffer_ptr_search(i % LOOKAHEAD_BUF_SIZE);
      if (ft_info_in.start == 0)
        continue;
  
      uns curr_set = (ft_info_in.start >> offset_bits) % num_sets;
      if (curr_set != target_set)
        continue;
  
      bool in_candidates = false;
      for (size_t j = 0; j < line_addrs.size(); j++) {
        if (line_addrs[j] == ft_info_in.start && n_uop[j] == ft_info_in.n_uops) {
          in_candidates = true;
          break;
        }
      }
  
      if (in_candidates)
        continue;
  
      auto &vec = uop_count_map[ft_info_in.start];
      bool found = false;
      for (auto &entry : vec) {
        if (entry.first == ft_info_in.n_uops) {
          found = true;
          if (!entry.second) {
            available_space -= (ft_info_in.n_uops + UOP_CACHE_WIDTH - 1) / UOP_CACHE_WIDTH;
            STAT_EVENT(0, NOT_IN_SET_MULTI_FOUND);
            if (available_space < required_length)
              return true;
            entry.second = true;
          }
          break;
        }
      }
  
      if (!found)
        vec.emplace_back(ft_info_in.n_uops, false);
    }
  
    return (available_space >= required_length) ? false : true;
  }
}
