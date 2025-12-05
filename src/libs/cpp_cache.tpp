// CPP implementation of a cache.

#include <iostream>
#include <limits>
#include <list>
#include <unordered_map>
#include <vector>

#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "libs/cpp_cache.h"

extern "C" {
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "memory/memory.param.h"
}

#define CPPC_DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CPP_CACHE, ##args)

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
  virtual uns get_repl_idx(Set<User_Key_Type, User_Data_Type>& set);
  virtual uns get_victim_idx_by_policy(Set<User_Key_Type, User_Data_Type>& set);

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
  Entry<User_Key_Type, User_Data_Type> insert(User_Key_Type key, User_Data_Type data);
  Entry<User_Key_Type, User_Data_Type> invalidate(User_Key_Type key);
  // New APIs for refactored preallocation
  uns get_free_space(User_Key_Type key);
  Entry<User_Key_Type, User_Data_Type> evict_one_line(User_Key_Type key);
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
    default:
      ASSERT(0, FALSE);  // unsupported
  }
}

template <typename User_Key_Type, typename User_Data_Type>
uns Cpp_Cache<User_Key_Type, User_Data_Type>::get_repl_idx(Set<User_Key_Type, User_Data_Type>& set) {
  // if there are invalid entries, replace them
  for (uns i = 0; i < assoc; i++) {
    Entry<User_Key_Type, User_Data_Type> entry = set.entries[i];
    if (!entry.valid) {
      return i;
    }
  }

  // otherwise, replace according to the policy using the new function
  return get_victim_idx_by_policy(set);
}

template <typename User_Key_Type, typename User_Data_Type>
uns Cpp_Cache<User_Key_Type, User_Data_Type>::get_victim_idx_by_policy(Set<User_Key_Type, User_Data_Type>& set) {
  // Apply replacement policy but ignore invalid entries
  uns repl_idx = 0;
  switch (repl_policy) {
    case REPL_TRUE_LRU: {
      Counter lru_cycle = std::numeric_limits<Counter>::max();
      bool found_valid = false;
      
      for (uns i = 0; i < assoc; i++) {
        Entry<User_Key_Type, User_Data_Type> entry = set.entries[i];
        
        // Skip invalid entries
        if (!entry.valid)
          continue;
        
        // Find entry with smallest access cycle
        if (entry.accessed_cycle < lru_cycle) {
          repl_idx = i;
          lru_cycle = entry.accessed_cycle;
          found_valid = true;
        }
      }
      
      // If no valid entries found, fallback to first entry
      if (!found_valid)
        repl_idx = 0;
    } break;
    case REPL_RANDOM: {
      // Collect valid indices and pick randomly from them
      std::vector<uns> valid_indices;
      for (uns i = 0; i < assoc; i++) {
        if (set.entries[i].valid)
          valid_indices.push_back(i);
      }
      
      if (!valid_indices.empty()) {
        repl_idx = valid_indices[rand() % valid_indices.size()];
      } else {
        repl_idx = rand() % assoc; // Fallback if no valid entries
      }
    } break;
    case REPL_ROUND_ROBIN: {
      // Find next valid entry in round-robin fashion
      uns start_idx = (set.next_evict + 1) % assoc;
      bool found_valid = false;
      
      for (uns offset = 0; offset < assoc; offset++) {
        uns candidate_idx = (start_idx + offset) % assoc;
        if (set.entries[candidate_idx].valid) {
          repl_idx = candidate_idx;
          set.next_evict = repl_idx;
          found_valid = true;
          break;
        }
      }
      
      if (!found_valid) {
        // Fallback: use original logic if no valid entries found
        repl_idx = (set.next_evict + 1) % assoc;
        set.next_evict = repl_idx;
      }
    } break;
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
                                                                                      User_Data_Type data) {
  // first check if line exists
  ASSERT(0, access(key, FALSE) == NULL);

  uns set_idx = set_idx_hash(key);
  // if the set is full, repl_idx will be overwritten;
  // if the set has vacancy, repl_idx will point to an invalid line
  uns repl_idx = get_repl_idx(sets[set_idx]);

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
uns Cpp_Cache<User_Key_Type, User_Data_Type>::get_free_space(User_Key_Type key) {
  uns set_idx = set_idx_hash(key);
  Set<User_Key_Type, User_Data_Type>& set = sets[set_idx];
  uns free_count = 0;

  for (uns i = 0; i < assoc; i++) {
    if (!set.entries[i].valid)
      free_count++;
  }
  return free_count;
}

template <typename User_Key_Type, typename User_Data_Type>
Entry<User_Key_Type, User_Data_Type> Cpp_Cache<User_Key_Type, User_Data_Type>::evict_one_line(User_Key_Type key) {
  uns set_idx = set_idx_hash(key);
  Set<User_Key_Type, User_Data_Type>& set = sets[set_idx];
  
  // Use the replacement policy to select a victim
  uns victim_idx = get_victim_idx_by_policy(set);
  Entry<User_Key_Type, User_Data_Type> evicted_entry = set.entries[victim_idx];
  
  // Mark the victim as invalid
  set.entries[victim_idx].valid = FALSE;
  
  return evicted_entry;
}