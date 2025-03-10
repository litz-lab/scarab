// CPP implementation of a cache.

#include "libs/cpp_cache.h"
#include <iostream>
#include <list>
#include <unordered_map>
#include <vector>
#include <limits>
#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "frontend/pt_memtrace/memtrace_fe.h"

#include "statistics.h"

extern "C" {
#include "globals/assert.h"
#include "globals/utils.h"
#include "globals/global_vars.h"
#include "memory/memory.param.h"
}

#ifndef GHRP_H
#define GHRP_H

#include <vector>
#include <cstdint>



class GHRP {
public:
    // Structure to represent cache block metadata
    struct BlockInfo {
        uint32_t signature;  // Increased from uint16_t to handle larger address
        uint8_t lruPosition;
        bool isDead;
        bool valid;
        
        BlockInfo() : signature(0), lruPosition(0), isDead(true), valid(false) {}
    };

private:
    // Constants
    static const int NUM_PRED_TABLES = 3;
    static const int PRED_TABLE_SIZE = 65536;
    static const int HISTORY_BITS = 64;  // Increased from 16 to 32 bits
    
    // Member variables - ordered by initialization
    const size_t numSets;
    const size_t associativity;
    const uint8_t bypassThreshold;
    const uint8_t deadThreshold;
    
    std::vector<std::vector<uint8_t>> predTables;  // 2-bit counters
    std::vector<std::vector<BlockInfo>> blocks;    // Per-set block information
    uint64_t pathHistory;      // Non-speculative path history, increased to 32 bits
    uint64_t specPathHistory;  // Speculative path history, increased to 32 bits

public:
    GHRP(const size_t num_sets, const size_t ways, 
         const uint8_t bypass_thresh = 1, const uint8_t dead_thresh = 1) 
        : numSets(num_sets)
        , associativity(ways)
        , bypassThreshold(bypass_thresh)
        , deadThreshold(dead_thresh)
        , predTables(NUM_PRED_TABLES, std::vector<uint8_t>(PRED_TABLE_SIZE, 3))
        , blocks(num_sets, std::vector<BlockInfo>(ways))
        , pathHistory(0)
        , specPathHistory(0)
    {}

    void init() {
        pathHistory = 0;
        specPathHistory = 0;
        
        for (auto& table : predTables) {
            std::fill(table.begin(), table.end(), 3);
        }
        
        for (auto& set : blocks) {
            for (auto& block : set) {
                block = BlockInfo();
            }
        }
    }
    bool hasEmptyOrDeadBlock(size_t setIndex) const {
        for (size_t i = 0; i < associativity; i++) {
            // Check for invalid (empty) or predicted dead blocks
            if (!blocks[setIndex][i].valid || blocks[setIndex][i].isDead) {
                return true;
            }
        }
        return false;
    }
    // Update path history - call for every I-cache access
    void updatePathHistory(unsigned long long addr) {
      
        specPathHistory = (specPathHistory << 4) | ((addr & 0xF));
        specPathHistory &= 0xFFFFFFFFFFFFFFFFULL;
    }
    
    // Commit path history - call at instruction retirement
    void commitPathHistory(unsigned long long addr) {
        pathHistory = (pathHistory << 4) | ((addr & 0xF));
        pathHistory &= 0xFFFFFFFFFFFFFFFFULL;
    }
    
    // Recover path history on branch misprediction
    void recoverPathHistory() {
        specPathHistory = pathHistory;
        return;
    }
    
    // Check if block should be bypassed
    // Modified bypass check
    bool shouldBypass(unsigned long long addr, size_t setIndex) {
        // If we have empty space or dead blocks, don't bypass
        if (hasEmptyOrDeadBlock(setIndex)) {
            return false;
        }

        // Otherwise, check prediction tables
        uint32_t signature = generateSignature(addr);
        auto indices = computeIndices(signature);
        return majorityVote(indices, bypassThreshold);
    }
    
  // Simplified victim selection using pre-computed isDead bit
    size_t getVictim(size_t setIndex) {
        // First try to find a predicted dead block
        for (size_t i = 0; i < associativity; i++) {
            if (blocks[setIndex][i].valid && blocks[setIndex][i].isDead) {
                return i;
            }
        }
        
        // If no dead block, find invalid or LRU block
        size_t lruWay = 0;
        uint8_t maxLRU = 0;
        for (size_t i = 0; i < associativity; i++) {
            if (!blocks[setIndex][i].valid) {
                return i;  // Invalid block takes precedence
            }
            if (blocks[setIndex][i].lruPosition > maxLRU) {
                maxLRU = blocks[setIndex][i].lruPosition;
                lruWay = i;
            }
        }
        return lruWay;
    }

  

  // Update block's dead status based on prediction tables
    void updateDeadStatus(size_t setIndex, size_t wayIndex, unsigned long long addr) {
        if (setIndex >= numSets || wayIndex >= associativity) return;
        
        auto& block = blocks[setIndex][wayIndex];
        uint32_t signature = generateSignature(addr);
        auto indices = computeIndices(signature);
        block.isDead = majorityVote(indices, deadThreshold);
    }

    // Update block state on replacement - now also updates dead status
    void updateReplacement(size_t setIndex, size_t wayIndex, unsigned long long newAddr) {
        if (setIndex >= numSets || wayIndex >= associativity) return;
        
        // Update prediction for the victim block if it's valid
        if (blocks[setIndex][wayIndex].valid) {
            auto indices = computeIndices(blocks[setIndex][wayIndex].signature);
            updatePredictors(indices, true);  // Mark old block as dead in tables
        }
        
        // Setup new block
        auto& block = blocks[setIndex][wayIndex];
        block.signature = generateSignature(newAddr);
        block.valid = true;
        updateDeadStatus(setIndex, wayIndex, newAddr);  // Update isDead flag
        updateLRU(setIndex, wayIndex);
    }
    // Update block state on hit - now also updates dead status
    void updateHit(size_t setIndex, size_t wayIndex, unsigned long long addr) {
        if (setIndex >= numSets || wayIndex >= associativity) return;
        
        auto& block = blocks[setIndex][wayIndex];
        auto indices = computeIndices(block.signature);
        updatePredictors(indices, false);  // Mark as not dead in tables
        
        block.signature = generateSignature(addr);
        updateDeadStatus(setIndex, wayIndex, addr);  // Update isDead flag
        updateLRU(setIndex, wayIndex);
    }

private:
    // Generate signature from instruction address and current history
    uint64_t generateSignature(unsigned long long addr) const {
        return specPathHistory ^ (static_cast<uint64_t>(addr & 0xFFFFFFFF));
    }
    
    // Get indices for all prediction tables
    std::vector<uint16_t> computeIndices(uint64_t signature) const {
        std::vector<uint16_t> indices(NUM_PRED_TABLES);
        indices[0] = hash1(signature);
        indices[1] = hash2(signature);
        indices[2] = hash3(signature);
        return indices;
    }
    
    uint16_t compressTo16(uint64_t signature) const {
      // XOR four 16-bit segments into a single 16-bit value
      return (signature & 0xFFFF) ^ ((signature >> 16) & 0xFFFF) ^ 
             ((signature >> 32) & 0xFFFF) ^ ((signature >> 48) & 0xFFFF);
    }

    // Hash functions for indexing prediction tables
    uint16_t hash1(uint64_t signature) const {
      uint16_t compressed = compressTo16(signature);
        return (compressed ^ (compressed >> 5)) & (PRED_TABLE_SIZE - 1);
    }
    
    uint16_t hash2(uint64_t signature) const {
      uint16_t compressed = compressTo16(signature);
        return (compressed ^ (compressed >> 7)) & (PRED_TABLE_SIZE - 1);
    }
    
    uint16_t hash3(uint64_t signature) const {
      uint16_t compressed = compressTo16(signature);
        return (compressed ^ (compressed << 3)) & (PRED_TABLE_SIZE - 1);
    }
    
    bool majorityVote(const std::vector<uint16_t>& indices, uint8_t threshold) const {
        int votes = 0;
        for (int i = 0; i < NUM_PRED_TABLES; i++) {
            if (predTables[i][indices[i]] > threshold) {
                votes++;
            }
        }
        STAT_EVENT(0, GHRP_VOTED_ALIVE + (votes >= (NUM_PRED_TABLES / 2 + 1)));
        return votes >= (NUM_PRED_TABLES / 2 + 1);
    }
    
    void updatePredictors(const std::vector<uint16_t>& indices, bool isDead) {
        for (int i = 0; i < NUM_PRED_TABLES; i++) {
            uint8_t& counter = predTables[i][indices[i]];
            if (isDead && counter < 7) counter++;
            else if (!isDead && counter > 0) counter--;
        }
    }

    void updateLRU(size_t setIndex, size_t wayIndex) {
        uint8_t oldPos = blocks[setIndex][wayIndex].lruPosition;
        blocks[setIndex][wayIndex].lruPosition = 0;  // MRU position
        
        // Update other blocks' LRU positions
        for (size_t i = 0; i < associativity; i++) {
            if (i != wayIndex && blocks[setIndex][i].valid && 
                blocks[setIndex][i].lruPosition < oldPos) {
                blocks[setIndex][i].lruPosition++;
            }
        }
    }
};
#endif

#define CPPC_DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CPP_CACHE, ##args)



Addr bypass_repl_addr = 0;
uns bypass_repl_idx = 0;
bool bypass_repl_idx_valid = 0;
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
  // Order members by initialization
  Repl_Policy      repl_policy;
  uns              assoc;
  uns              num_sets;
  uns              line_bytes;
  GHRP             ghrp;  // Move after primitive members
  std::vector<Set<User_Key_Type, User_Data_Type>> sets;

  virtual uns set_idx_hash(User_Key_Type key) = 0;
  virtual void update_repl_states(Set<User_Key_Type, User_Data_Type>& set, uns hit_idx);
  virtual uns get_repl_idx(Set<User_Key_Type, User_Data_Type>& set, uns set_idx);


 public:
  Cpp_Cache() = default;
  
  Cpp_Cache(uns nl, uns asc, uns lb, Repl_Policy rp) 
    : repl_policy(rp)
    , assoc(asc)
    , num_sets(nl/asc)
    , line_bytes(lb)
    , ghrp(nl/asc, asc)  // Initialize GHRP with computed values
    , sets(nl/asc)
  {
    for (uns i = 0; i < num_sets; i++) {
      sets[i].entries.resize(assoc);
    }
    if(GHRP_ENABLE)
      ghrp.init();
  }

  User_Data_Type* access(User_Key_Type key, bool update_repl);
  Entry<User_Key_Type, User_Data_Type> insert(User_Key_Type key, User_Data_Type data);
  Entry<User_Key_Type, User_Data_Type> invalidate(User_Key_Type key);
  void check_used_counters();
  
  bool check_bypass(User_Key_Type key, Addr end_addr);
  // Add public accessors for GHRP functions
  void commitPathHistory(uint32_t addr) {
    if(GHRP_ENABLE) {
      ghrp.commitPathHistory(addr);
    }
  }

  void recoverPathHistory() {
    if(GHRP_ENABLE) {
      ghrp.recoverPathHistory();
    }
  }
};

template <typename User_Key_Type, typename User_Data_Type>
void Cpp_Cache<User_Key_Type, User_Data_Type>::update_repl_states(Set<User_Key_Type, User_Data_Type>& set, uns hit_idx) {
  switch(repl_policy) {
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
    case GHRP_UOP: {
     set.entries[hit_idx].accessed_cycle = cycle_count;
    } break;
    default:
      ASSERT(0, FALSE);  // unsupported
  }
}

template <typename User_Key_Type, typename User_Data_Type>
uns Cpp_Cache<User_Key_Type, User_Data_Type>::get_repl_idx(Set<User_Key_Type, User_Data_Type>& set, uns set_idx) {
  // if there are invalid entries, replace them
  for (uns i = 0; i < assoc; i++) {
    Entry<User_Key_Type, User_Data_Type> entry = set.entries[i];
    if (!entry.valid) {
      return i;
    }
  }

  // otherwise, replace according to the policy
  uns repl_idx = 0;
  switch(repl_policy) {
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
      // printf("\n==== BELADY_UOP Replacement Start ====\n");
      // record the most recent used and not evict it
      // uns num_ft = 0;
      Counter mru_cycle = std::numeric_limits<Counter>::min();
      // printf("Finding MRU cycle and checking bypass...\n");
      // std::vector<Addr> begin_addresses;
      // std::vector<Addr> end_addresses;
      // std::vector<uns> lengths;
      // std::vector<uns> n_uop;
      // std::vector<uns> index;
      
      for (uns i = 0; i < assoc; i++) {
        Entry<User_Key_Type, User_Data_Type> entry = set.entries[i];
        // printf("Entry[%u]: addr=%llx, cycle=%llu, valid=%d\n", 
        //        i, entry.key.first, entry.accessed_cycle, entry.valid);
        
        // find biggest access cycle
        if (set.entries[i].valid && entry.accessed_cycle > mru_cycle) {
          mru_cycle = entry.accessed_cycle;
          // printf("New MRU cycle found: %llu at index %u\n", mru_cycle, i);
      }
      // if(set.entries[i].valid && set.entries[i].data.end_of_ft) {
      //   num_ft++;
      //   end_addresses.push_back(set.entries[i].key.first);
      //   begin_addresses.push_back(set.entries[i].key.second.start);
      //   lengths.push_back(set.entries[i].key.second.n_uops/ISSUE_WIDTH + 1);
      //   n_uop.push_back(set.entries[i].key.second.n_uops);
      //   index.push_back(i);
      //   // printf("Found end_of_ft at index %u, total ft count: %u\n", i, num_ft);
      // }
    }
      // printf("\nSearching for replacement candidate...\n");
      // don't have replacement cand when checking bypass, choose from set
      // Addr fartherest_timestamp = 0;
      // // Addr second_fartherest_timestamp = 0;
      // uns not_found_idx = 0;
      // uns not_found_ft_idx = 0;
      // uns replace_ft_idx = 0;
      // uns second_replace_ft_idx = 0;
      // bool have_not_found = 0;
      // // Counter not_found_cycle = std::numeric_limits<Counter>::max();
      // for (uns i = 0; i < num_ft; i++) {
      //   auto curr_timestamp = find_valid_ft_timestamp(begin_addresses[i], end_addresses[i], lengths[i], n_uop[i]);
      //   if(curr_timestamp && curr_timestamp > fartherest_timestamp){
      //     // second_fartherest_timestamp = fartherest_timestamp;
      //     fartherest_timestamp = curr_timestamp;
      //     second_replace_ft_idx = replace_ft_idx;
      //     replace_ft_idx = i;
      //   }
      //   if(!curr_timestamp){
      //     not_found_ft_idx = i;
      //     have_not_found = 1;
      //     // break;
      //   }
      // }
      // uns curr_ft = 0;
      // uns second_replace_idx = 0;
      Addr fartherest_timestamp = 0;
      // bool have_not_found = 0;
      // uns repl_idx_valid = 0;
      for (uns i = 0; i < assoc; i++) {
        if(set.entries[i].valid && set.entries[i].data.end_of_ft){

          auto curr_timestamp = BELADY_START_ONLY ? buffer_find_timestamp(set.entries[i].key.first) : find_valid_ft_timestamp(set.entries[i].key.second.start, set.entries[i].key.first, (set.entries[i].key.second.n_uops/ISSUE_WIDTH) + 1, set.entries[i].key.second.n_uops);

          
          if(curr_timestamp  && curr_timestamp > fartherest_timestamp){
            fartherest_timestamp = curr_timestamp;
            repl_idx = i;
          }
          if(!curr_timestamp && mru_cycle != set.entries[i].accessed_cycle){
            return i;
          }
          // if(curr_ft == not_found_ft_idx && have_not_found){
          //   not_found_idx = i;
          //   continue;
          // } else  if(curr_ft == replace_ft_idx && !have_not_found){
          //   repl_idx = i;
          //   continue;
          // } else  if(curr_ft == second_replace_ft_idx && !have_not_found){
          //   second_replace_idx = i;
          //   continue;
          // } 
          // if(bypass_repl_idx_valid == 1 && curr_ft == bypass_repl_addr && mru_cycle != set.entries[i].accessed_cycle){
          //   bypass_repl_idx_valid = 0;
          //   // printf("Choosing bypass index %u for eviction (addr=%llx)\n\n", 
          //   //        bypass_repl_idx, bypass_repl_addr);
          //   return i;
          // }
          // if(set.entries[i].data.end_of_ft)
          //   curr_ft += 1 ;
        }

          
        
      }
      

    if(mru_cycle == set.entries[repl_idx].accessed_cycle){
          Counter lru_cycle = std::numeric_limits<Counter>::max();
          for (uns j = 0; j < assoc; j++) {
            Entry<User_Key_Type, User_Data_Type> entry = set.entries[j];
            if (entry.valid && entry.accessed_cycle < lru_cycle) {
              repl_idx = j;
              lru_cycle = entry.accessed_cycle;
              // printf("New LRU found: index=%u, cycle=%llu\n", repl_idx, lru_cycle);
            }
          }
      }

      // else {
      //   printf("Final choice: farthest future timestamp entry at index %u\n", repl_idx);
      // }
      
      // printf("==== BELADY_UOP Replacement End ====\n\n");
      break;
    
  }
    case GHRP_UOP: {
      Counter mru_cycle = std::numeric_limits<Counter>::min();
      for (uns i = 0; i < assoc; i++) {
        Entry<User_Key_Type, User_Data_Type> entry = set.entries[i];
        // find biggest access cycle
        if (entry.accessed_cycle > mru_cycle) {
          mru_cycle = entry.accessed_cycle;
        }
      }
      repl_idx = ghrp.getVictim(set_idx);
      if(set.entries[repl_idx].accessed_cycle == mru_cycle){
         Counter lru_cycle = std::numeric_limits<Counter>::max();
        for (uns i = 0; i < assoc; i++) {
          Entry<User_Key_Type, User_Data_Type> entry = set.entries[i];
          // find smallest access cycle
          if (entry.accessed_cycle < lru_cycle) {
            repl_idx = i;
            lru_cycle = entry.accessed_cycle;
          }
        }
      }
      
    } break;
    default:
      ASSERT(0, FALSE);  // unsupported
  }
  return repl_idx;
}

template <typename User_Key_Type, typename User_Data_Type>
bool Cpp_Cache<User_Key_Type, User_Data_Type>::check_bypass(User_Key_Type key, Addr end_addr){
  uns set_idx = set_idx_hash(key);
  uns num_ft = 0;
  bypass_repl_idx_valid = 0;
  auto set = sets[set_idx];
  std::vector<Addr> begin_addresses;
  std::vector<Addr> end_addresses;
  std::vector<uns> lengths;
  std::vector<Addr> byte_lengths;
  std::vector<uns> n_uop;
  // add_to_ft_map(key.first, key.second.n_uops, end_addr);
  // uns total_length = 0;
  begin_addresses.push_back(key.first);
  end_addresses.push_back(end_addr);
  byte_lengths.push_back(key.second.length);
  lengths.push_back(key.second.n_uops/ISSUE_WIDTH + 1);
  n_uop.push_back(key.second.n_uops);
  // total_length =total_length +(key.second.n_uops/ISSUE_WIDTH + 1);

  uns valid_cnt = 0;
  if(GHRP_ENABLE)
    return ghrp.shouldBypass(key.first,set_idx);
  for (uns i = 0; i < assoc; i++) {
    // if have empty entry don't bypass
      
    if(set.entries[i].valid && set.entries[i].data.end_of_ft){
      num_ft++;
      valid_cnt++;
      // add_to_ft_map(set.entries[i].key.first, set.entries[i].key.second.n_uops, set.entries[i].key.first);
      // printf("inserting addr %llx of length %i\n", set.entries[i].key.second.start, set.entries[i].key.second.n_uops);
      end_addresses.push_back(set.entries[i].key.first);
      begin_addresses.push_back(set.entries[i].key.second.start);
      byte_lengths.push_back(set.entries[i].key.second.length);
      lengths.push_back(set.entries[i].key.second.n_uops/ISSUE_WIDTH + 1);
      n_uop.push_back(set.entries[i].key.second.n_uops);
      // total_length =total_length +(set.entries[i].key.second.n_uops/ISSUE_WIDTH + 1);
      
    }
      
  }
  // if(begin_addresses.size()<2){
  //   // printf("not full should not bypass\n");
  //   return 0;
  // }
  // if(total_length < assoc){
  //   return 0;
  // }
    
  // printf("set has valid line %i ft %i :\n", valid_cnt, num_ft);
  uns replace_addr = BELADY_START_ONLY ? buffer_find_replace_start(begin_addresses, end_addresses, lengths, byte_lengths,n_uop,  num_ft) : buffer_find_replace(begin_addresses, end_addresses, lengths, byte_lengths,n_uop,  num_ft);
  if(!replace_addr){
    //bypass if 0 return
    return 1;
  }else{
    bypass_repl_addr = replace_addr - 1;
    bypass_repl_idx_valid = 1;
    return 0;
  }


    
  
}

// access: Looks up the cache based on key. Returns pointer to line data if found
template <typename User_Key_Type, typename User_Data_Type>
User_Data_Type* Cpp_Cache<User_Key_Type, User_Data_Type>::access(User_Key_Type key, bool update_repl) {
  User_Data_Type* data = NULL;
  uns set_idx = set_idx_hash(key);
  if(GHRP_ENABLE)
    ghrp.updatePathHistory(key.first);
  for (uns i = 0; i < assoc; i++) {
    Entry<User_Key_Type, User_Data_Type>& entry = sets[set_idx].entries[i];
    if (entry.valid && entry.key == key) {  // hit
      data = &entry.data;
      ghrp.updateHit(set_idx, i, key.first);
      if (update_repl) {
        update_repl_states(sets[set_idx], i);
      }
      break;
    }
  }
  return data;
}

template <typename User_Key_Type, typename User_Data_Type>
Entry<User_Key_Type, User_Data_Type> Cpp_Cache<User_Key_Type, User_Data_Type>::insert(User_Key_Type key, User_Data_Type data) {
  // first check if line exists
  ASSERT(0, access(key, FALSE) == NULL);
  
  uns set_idx = set_idx_hash(key);
  // printf("SET index: inserting addr %llx to set %i \n", key.first, set_idx);
  // if the set is full, repl_idx will be overwritten;
  // if the set has vacancy, repl_idx will point to an invalid line
  uns repl_idx = get_repl_idx(sets[set_idx], set_idx);
  // printf("SET index:replacing idx %i \n", repl_idx);
  Entry<User_Key_Type, User_Data_Type> evicted_entry = sets[set_idx].entries[repl_idx];
  // printf("SET index:replacing addr %llx in set idx %i \n", evicted_entry.key.first, repl_idx);
  sets[set_idx].entries[repl_idx] = Entry<User_Key_Type, User_Data_Type>{TRUE, key, data, 0};
  update_repl_states(sets[set_idx], repl_idx);
  if(GHRP_ENABLE)
    ghrp.updateReplacement(repl_idx, set_idx, key.first);

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
void Cpp_Cache<User_Key_Type, User_Data_Type>::check_used_counters() {
  for (uns set_idx = 0; set_idx < num_sets; set_idx++) {
    for (uns i = 0; i < assoc; i++) {
      Entry<User_Key_Type, User_Data_Type>& entry = sets[set_idx].entries[i];
      if (entry.valid && entry.data.used > 0) {  // Check if entry is valid and used counter > 0
        STAT_EVENT(0, IN_UCACHE_LINE_USED);
      }else if(entry.valid && entry.data.used == 0)
      STAT_EVENT(0, IN_UCACHE_LINE_NOT_USED);
    }
  }
}