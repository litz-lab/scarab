/***************************************************************************************
 * File         : uop_cache.h
 * Author       : Peter Braun
 * Date         : 10.28.2020
 * Description  : Interface for interacting with uop cache object.
 *                  Following Kotra et. al.'s MICRO 2020 description of uop cache baseline
 ***************************************************************************************/

#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "isa/isa_macros.h"

#include "bp/bp.h"
#include "op_pool.h"

#include "core.param.h"
#include "debug/debug.param.h"
#include "general.param.h"
#include "statistics.h"

#include "libs/cache_lib.h"
#include "memory/memory.h"
#include "memory/memory.param.h"
#include "libs/cpp_cache.h"
#include "uop_cache.h"
#include "icache_stage.h"
#include "uop_queue_stage.h"

#include <vector>

#include <unordered_map>
#include <fstream>
#include <cmath>
#include <memory>
#include <string>
#include <stdexcept>
/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_UOP_CACHE, ##args)

// Uop cache is byte-addressable, so tag/set index are generated from full address (no offset)
// Uop cache uses icache tag + icache offset as full TAG
#define UOP_CACHE_LINE_SIZE       ICACHE_LINE_SIZE

typedef std::pair<Addr, FT_Info_Static> Uop_Cache_Key;
uns tage_source = 0;
uns bp_confi = 0;
int branch_blk_dir = 0;
Addr start_branch_addr = 0;
/**************************************************************************************/
/* Local Prototypes */

// uop cache derived class
class Uop_Cache: public Cpp_Cache<Uop_Cache_Key, Uop_Cache_Data> {
 protected:
  uns   offset_bits;

  // implementing the virtual hash function
  uns set_idx_hash(Uop_Cache_Key key);

 public:
  // constructor
  Uop_Cache(uns nl, uns asc, uns lb, Repl_Policy rp) : Cpp_Cache<Uop_Cache_Key, Uop_Cache_Data>::Cpp_Cache(nl, asc, lb, rp) {
    // offset_bits is only used to denote the start of the set bits
    // the set bits follow the offset_bits on the significant side
    // in other words,
    // user manipulates the line_bytes of the Cpp_Cache to change the set id hashing pattern
    offset_bits  = LOG2(line_bytes);
  }
};

uns Uop_Cache::set_idx_hash(Uop_Cache_Key key) {
  // use % instead of masking to support num_sets that is not a power of 2
  return (key.first >> offset_bits) % num_sets;
}

// overload operator == of FT_Info_Static type
bool operator==(const FT_Info_Static& lhs, const FT_Info_Static& rhs) {
  return lhs.start == rhs.start && lhs.length == rhs.length && lhs.n_uops == rhs.n_uops;
}

// overload operator == of Uop_Cache_Key type
bool operator==(const Uop_Cache_Key& lhs, const Uop_Cache_Key& rhs) {
  bool pc_match = lhs.first == rhs.first;

  FT_Info_Static l_info = lhs.second;
  FT_Info_Static r_info = rhs.second;

  return pc_match && l_info == r_info;
}

// Forward declaration
class UOPCacheTracker;

// Custom deleter as a standalone class
class UOPTrackerDeleter {
public:
    void operator()(UOPCacheTracker* p);
};

class UOPCacheTracker {
    friend class UOPTrackerDeleter;
private:
    struct AccessInfo {
        unsigned long long timestamp;
        unsigned long long address;
        unsigned length;  // Added length field
    };

    // Define a custom key structure for the map
    struct AccessKey {
        unsigned long long address;
        unsigned length;

        // Define equality operator for the key
        bool operator==(const AccessKey& other) const {
            return address == other.address && length == other.length;
        }
    };

    // Custom hash function for AccessKey
    struct AccessKeyHash {
        std::size_t operator()(const AccessKey& k) const {
            return std::hash<unsigned long long>()(k.address) ^ 
                   (std::hash<unsigned>()(k.length) << 1);
        }
    };

    struct SetInfo {
        unsigned long long current_timestamp;
        std::unordered_map<AccessKey, AccessInfo, AccessKeyHash> address_history;
    };

    static std::unique_ptr<UOPCacheTracker, UOPTrackerDeleter> instance;
    
    const unsigned UOP_CACHE_LINE_SIZE;
    const unsigned UOP_CACHE_LINES;
    const unsigned UOP_CACHE_ASSOC;
    const unsigned offset_bits;
    const unsigned num_sets;
    std::vector<SetInfo> sets;
    std::ofstream output_file;

protected:
    ~UOPCacheTracker() {
        if (output_file.is_open()) {
            dumpStatistics();
            output_file.close();
        }
    }

private:
    UOPCacheTracker(unsigned line_size, unsigned lines, unsigned assoc, const std::string& output_filename) 
        : UOP_CACHE_LINE_SIZE(line_size),
          UOP_CACHE_LINES(lines),
          UOP_CACHE_ASSOC(assoc),
          offset_bits(log2(line_size)),
          num_sets(lines / assoc),
          sets(lines / assoc) {
        
        output_file.open(output_filename);
        if (!output_file.is_open()) {
            throw std::runtime_error("Failed to open output file");
        }
        
        for (auto& set : sets) {
            set.current_timestamp = 0;
        }
        output_file << "FT_Start,FT_Length,NumUops,TAGEComp,start_branch_addr,bp_confi,bp_dir,"
                << "StartedBy,EndedBy,FirstOpOffPath,bypass_decision\n";
    }

public:
    static void initialize(unsigned line_size, unsigned lines, unsigned assoc, const std::string& output_filename) {
        if (!instance) {
            instance.reset(new UOPCacheTracker(line_size, lines, assoc, output_filename));
        }
    }

    static UOPCacheTracker& getInstance() {
        if (!instance) {
            throw std::runtime_error("UOPCacheTracker not initialized");
        }
        return *instance;
    }

    static void cleanup() {
        instance.reset();
    }

    void recordAccess(unsigned long long address, unsigned length) {
        unsigned target_set = (address >> offset_bits) % num_sets;
        SetInfo& set = sets[target_set];
        set.current_timestamp++;

        AccessKey key{address, length};
        unsigned long long reuse_distance = 0;
        auto it = set.address_history.find(key);
        
        if (it != set.address_history.end()) {
            reuse_distance = set.current_timestamp - it->second.timestamp;
        }

        if(DUMP_FT_INFO) {
            output_file << target_set << ","
                       << std::hex << address << std::dec << ","
                       << length << ","
                       << set.current_timestamp << ","
                       << reuse_distance << "\n";
        }

        set.address_history[key] = {
            set.current_timestamp,
            address,
            length
        };
    }

    void logFTInfo( const FT_Info_struct& ft_info, uns bypass) {
      output_file 
      << ft_info.static_info.start << ","
      << ft_info.static_info.length << ","
      << ft_info.static_info.n_uops << ","
      << ft_info.static_info.tage_comp << ","
      << ft_info.static_info.start_branch_addr << ","
      << ft_info.static_info.bp_confi << ","
      << ft_info.static_info.bp_dir << ","
      << static_cast<int>(ft_info.dynamic_info.started_by) << ","
      << static_cast<int>(ft_info.dynamic_info.ended_by) << ","
      << static_cast<int>(ft_info.dynamic_info.first_op_off_path) << ","
      << bypass << "\n";
    }


    void dumpStatistics() {
        // Statistics dumping remains the same
    }

    unsigned lookupLength(unsigned long long address) {
        unsigned target_set = (address >> offset_bits) % num_sets;
        SetInfo& set = sets[target_set];
        
        unsigned longest_length = 0;
        bool found = false;
        
        // Find the longest length for this address
        for (const auto& entry : set.address_history) {
            if (entry.first.address == address) {
                found = true;
                longest_length = std::max(longest_length, entry.first.length);
            }
        }
        STAT_EVENT(0, FT_LENGTH_NOT_FOUND+found);
        
        // if (found && DUMP_FT_INFO) {
        //     std::cerr << "Found length " << longest_length 
        //              << " for address 0x" << std::hex << address << std::dec << "\n";
        // }
        
        return longest_length;  // Returns 0 if address not found
    }

};

// Implement deleter
void UOPTrackerDeleter::operator()(UOPCacheTracker* p) { 
    delete p; 
}

// Define static member
std::unique_ptr<UOPCacheTracker, UOPTrackerDeleter> UOPCacheTracker::instance = nullptr;

// API namespace
namespace uop_cache_tracker {
    void init(unsigned line_size, unsigned lines, unsigned assoc, const std::string& filename) {
        UOPCacheTracker::initialize(line_size, lines, assoc, filename);
    }

    void record_access(unsigned long long address, unsigned length) {
        UOPCacheTracker::getInstance().recordAccess(address, length);
    }

    void dump_stats() {
        UOPCacheTracker::getInstance().dumpStatistics();
    }

    void cleanup() {
        UOPCacheTracker::cleanup();
    }

    unsigned lookup_length(unsigned long long address) {
      return UOPCacheTracker::getInstance().lookupLength(address);
    }
    

    void record_FT(const FT_Info_struct& ft_info, uns bypass) {
     UOPCacheTracker::getInstance().logFTInfo(ft_info, bypass);
    }

}



  
/**************************************************************************************/
/* Global Variables */

uns8 uop_cache_proc_id;
// per core caches
std::vector<Uop_Cache*> per_core_uop_cache;

// std::vector<std::pair{n_uop, end_addr}> ft_len_info;

// uop cache per core accumulation structures
// the accumulation buffer stores the uop cache lines of an FT
// accumulated at the end of decoding pipeline.
// all lines are inserted into the uop cache when the entire FT has been accumulated
static std::vector<std::vector<Uop_Cache_Data>> per_core_accumulation_buffer;
static std::vector<Uop_Cache_Data> per_core_accumulating_line;
static std::vector<FT_Info> per_core_accumulating_ft;
static std::vector<Counter> per_core_accumulating_op_num;
// pointers to the structures of the current core in use
static std::vector<Uop_Cache_Data>* current_accumulation_buffer = NULL;
static Uop_Cache_Data* current_accumulating_line = NULL;
static FT_Info* current_accumulating_ft = NULL;
static Counter* current_accumulating_op_num = NULL;

// uop cache per core lookup structures
// the lookup buffer stores the uop cache lines of an FT
// to be consumed by the icache stage.
// all lines are cleared when the entire FT has been consumed by the icache stage
static std::vector<std::vector<Uop_Cache_Data>> per_core_lookup_buffer;
static std::vector<uns> per_core_num_looked_up_lines;
// pointers to the structures of the current core in use
static std::vector<Uop_Cache_Data>* current_lookup_buffer = NULL;
static uns* current_num_looked_up_lines = NULL;

void alloc_mem_uop_cache(uns num_cores) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }
  per_core_accumulation_buffer.resize(num_cores);
  per_core_accumulating_line.resize(num_cores);
  per_core_accumulating_ft.resize(num_cores);
  per_core_accumulating_op_num.resize(num_cores);

  per_core_lookup_buffer.resize(num_cores);
  for (uns i = 0; i < num_cores; i++) {
    per_core_lookup_buffer[i].resize(UOP_CACHE_ASSOC);
  }
  per_core_num_looked_up_lines.resize(num_cores);

  per_core_uop_cache.resize(num_cores);
}
/**************************************************************************************/
/* init_uop_cache */

void init_uop_cache(uns8 proc_id) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  // The cache library computes the number of entries from cache_size_bytes/cache_line_size_bytes,
  per_core_uop_cache[proc_id] = new Uop_Cache(UOP_CACHE_LINES, UOP_CACHE_ASSOC, UOP_CACHE_LINE_SIZE,
                                              (Repl_Policy)UOP_CACHE_REPL);

  uop_cache_tracker::init(UOP_CACHE_LINE_SIZE, UOP_CACHE_LINES, UOP_CACHE_ASSOC, "uop_cache_access_pattern.csv");

}



void set_uop_cache(uns8 proc_id) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  uop_cache_proc_id = proc_id;

  current_accumulation_buffer = &per_core_accumulation_buffer[proc_id];
  current_accumulating_line = &per_core_accumulating_line[proc_id];
  current_accumulating_ft = &per_core_accumulating_ft[proc_id];
  current_accumulating_op_num = &per_core_accumulating_op_num[proc_id];

  current_lookup_buffer = &per_core_lookup_buffer[proc_id];
  current_num_looked_up_lines = &per_core_num_looked_up_lines[proc_id];
}

Flag uop_cache_lookup_ft_and_fill_lookup_buffer(FT_Info ft_info, Flag offpath) {
  if (!UOP_CACHE_ENABLE) {
    return FALSE;
  }
  // std::cout << "looking for uop cache:: Addr 0x" << std::hex<<ft_info.static_info.start
  //           << ", length: "<< ft_info.static_info.n_uops <<std::endl;

  Uop_Cache_Data* uoc_data = NULL;
  Addr lookup_addr = ft_info.static_info.start;
  int buffer_index = 0;
  do {
    uoc_data = uop_cache_lookup_line(lookup_addr, ft_info, TRUE);
    if (buffer_index == 0) {
      DEBUG(uop_cache_proc_id, "UOC %s. ft_start=0x%llx, ft_length=%lld\n",
            uoc_data ? "hit" : "miss", ft_info.static_info.start, ft_info.static_info.length);
      if (!uoc_data) {
        return FALSE;
      } else {
        // if (!uoc_data->used && !offpath) {
        //   uoc_data->used += 1;
        //   STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERTED_ONPATH_USED_ONPATH + uoc_data->ft_info_dynamic.first_op_off_path);
        // }
        if (!uoc_data->used ) {
          // std::cout << "Used uop cache:: Addr 0x" << std::hex<<uoc_data->line_start
          //   << ", length: "<< uoc_data->n_uops <<std::endl;
          uoc_data->used += 1;
          
          STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERTED_ONPATH_USED_ONPATH + uoc_data->ft_info_dynamic.first_op_off_path);
        }
      }
    }

    ASSERT(uop_cache_proc_id, uoc_data);

    // if (!uoc_data->used && !offpath) {
    //   STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERTED_ONPATH_USED_ONPATH + uoc_data->ft_info_dynamic.first_op_off_path);
    //   uoc_data->used += 1;
    // }
    if (!uoc_data->used) {
      STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERTED_ONPATH_USED_ONPATH + uoc_data->ft_info_dynamic.first_op_off_path);
      uoc_data->used += 1;
    }

    current_lookup_buffer->at(buffer_index) = *uoc_data;
    buffer_index++;
    ASSERT(uop_cache_proc_id, (uoc_data->offset == 0) == uoc_data->end_of_ft);
    lookup_addr += uoc_data->offset;
  } while (!uoc_data->end_of_ft);

  return TRUE;
}

Uop_Cache_Data* uop_cache_get_line_from_lookup_buffer() {
  Uop_Cache_Data* uop_cache_line = &current_lookup_buffer->at(*current_num_looked_up_lines);
  *current_num_looked_up_lines += 1;
  return uop_cache_line;
}

void uop_cache_clear_lookup_buffer() {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  std::fill(current_lookup_buffer->begin(), current_lookup_buffer->end(), Uop_Cache_Data{});
  *current_num_looked_up_lines = 0;
}

Uop_Cache_Data* uop_cache_lookup_line(Addr line_start, FT_Info ft_info, Flag update_repl) {
  if (!UOP_CACHE_ENABLE) {
    return NULL;
  }
  uop_cache_tracker::record_access(line_start, ft_info.static_info.n_uops);
  Uop_Cache_Data* uoc_data = per_core_uop_cache[uop_cache_proc_id]->access({line_start, ft_info.static_info}, update_repl == TRUE);
  return uoc_data;
}

void clear_accumulation(Flag clear_all) {
  *current_accumulating_line = {};

  if (clear_all) {
    current_accumulation_buffer->clear();
    *current_accumulating_ft = {};
    *current_accumulating_op_num = 0;
  }
}

/**************************************************************************************/
/* insert_buffer_into_uopc: insert the uopc line buffer of an FT into the uopc. */
bool insert_buffer_into_uopc(FT_Info* buffer_ft_info, std::vector<Uop_Cache_Data>* accumulation_buffer) {
  bool success = false;
  // corner case handling
  // if an inst generates more uops than the uop cache line width,
  // consecutive lines might share the same start address (indicated by a zero offset).
  // it causes ambiguity and we do not insert the FT.
  Flag ft_contains_zero_offset_line = FALSE;
  for (uns i = 0; i < accumulation_buffer->size(); i++) {
    Uop_Cache_Data* insert_line = &accumulation_buffer->at(i);
    if (!insert_line->end_of_ft && insert_line->offset == 0) {
      ft_contains_zero_offset_line = TRUE;
      break;
    }
  }

  // insert accumulation buffer
  if (UOP_CACHE_INSERT_ONLY_ONPATH && buffer_ft_info->dynamic_info.first_op_off_path) {
    // if asked to only insert on-path FTs and the current FT is off-path,
    // no insertion for the entire FT
  } else if (ft_contains_zero_offset_line) {
    // the corner case described above; do not insert
    Uop_Cache_Data* uop_cache_line = uop_cache_lookup_line(buffer_ft_info->static_info.start, *buffer_ft_info, FALSE);
    ASSERT(uop_cache_proc_id, !uop_cache_line);
    if (buffer_ft_info->dynamic_info.first_op_off_path) {
      STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERT_FAILED_INST_TOO_BIG_OFF_PATH);
      INC_STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERT_FAILED_INST_TOO_BIG_OFF_PATH, accumulation_buffer->size());
    } else {
      STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERT_FAILED_INST_TOO_BIG_ON_PATH);
      INC_STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERT_FAILED_INST_TOO_BIG_ON_PATH, accumulation_buffer->size());
    }
  } else if (accumulation_buffer->size() > UOP_CACHE_ASSOC) {
    // if the FT is too big, do not insert
    Uop_Cache_Data* uop_cache_line = uop_cache_lookup_line(buffer_ft_info->static_info.start, *buffer_ft_info, FALSE);
    ASSERT(uop_cache_proc_id, !uop_cache_line);
    if (buffer_ft_info->dynamic_info.first_op_off_path) {
      STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERT_FAILED_FT_TOO_BIG_OFF_PATH);
      INC_STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERT_FAILED_FT_TOO_BIG_OFF_PATH, accumulation_buffer->size());
    } else {
      STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERT_FAILED_FT_TOO_BIG_ON_PATH);
      INC_STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERT_FAILED_FT_TOO_BIG_ON_PATH, accumulation_buffer->size());
    }
  } else {
    success = true;
    bool lines_exist = false;
    for (uns i = 0; i < accumulation_buffer->size(); i++) {
      Uop_Cache_Data* insert_line = &accumulation_buffer->at(i);
      Uop_Cache_Data* uop_cache_line = uop_cache_lookup_line(insert_line->line_start, *buffer_ft_info, TRUE);

      if (i == 0)
        insert_line->begin_of_ft = 1;
      else
        insert_line->begin_of_ft = 0;

      if (i == 0 && uop_cache_line) {
        // when i == 0, we are looking up the first line from the FT;
        // if this is already in the uop cache, all lines of the FT should be in the uop cache;
        // otherwise, all lines should not be in the uop cache.
        lines_exist = true;
      }

      if (lines_exist) {
        // this line could already exist in the uop cache if
        // the reuse distance in cycle is too short:
        // the first occurrence was not yet inserted while the second was looked up,
        // so by the time the second occurrence was inserted, the first occurrence was already in the uop cache.
        // in that case the look-up above has updated the replacement policy,
        // and we skip the insertion.
        ASSERT(uop_cache_proc_id, uop_cache_line);

        // the line should be identical
        ASSERT(uop_cache_proc_id, uop_cache_line->n_uops == insert_line->n_uops &&
                                  uop_cache_line->offset == insert_line->offset &&
                                  uop_cache_line->end_of_ft == insert_line->end_of_ft);

        if (buffer_ft_info->dynamic_info.first_op_off_path) {
          if (i == 0) {
            STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERT_CONFLICTED_SHORT_REUSE_OFF_PATH);
          }
          STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERT_CONFLICTED_SHORT_REUSE_OFF_PATH);
        } else {
          if (i == 0) {
            STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERT_CONFLICTED_SHORT_REUSE_ON_PATH);
          }
          STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERT_CONFLICTED_SHORT_REUSE_ON_PATH);
        }
      } else {
        ASSERT(uop_cache_proc_id, !uop_cache_line);
        // if(i==0){
        //             std::cout << "trying to Inserting uop cache head:: Addr 0x" << std::hex<<insert_line->line_start << " off path: " << std::boolalpha<< buffer_ft_info->dynamic_info.first_op_off_path 
        //     << "length: "<< insert_line->n_uops <<std::endl;
        // }
        auto end_address = &accumulation_buffer->back().line_start;

        


        if (UOP_CACHE_BYPASS) {
          
          if (i == 0)
            STAT_EVENT(uop_cache_proc_id, UOP_INSERTION_BYPASS_CHECK);
          if(UOP_BYPASS_ALL_OFFPATH){
            if(i == 0 && buffer_ft_info->dynamic_info.first_op_off_path){
              STAT_EVENT(uop_cache_proc_id, UOP_INSERTION_BYPASSED_OFFPATH);
              return true;
            }
          }
         
          
            // if(*end_address == insert_line->line_start)
            // STAT_EVENT(uop_cache_proc_id, UOP_INSERTION_BYPASSED_OFFPATH);
          if (i == 0 ) {
            auto should_bypass = per_core_uop_cache[uop_cache_proc_id]->check_bypass(
              {insert_line->line_start, buffer_ft_info->static_info}, *end_address);
            if(should_bypass)
            {
                STAT_EVENT(uop_cache_proc_id, UOP_INSERTION_CHECKED_BYPASSING);
                if(buffer_ft_info->static_info.start)
                  uop_cache_tracker::record_FT(*buffer_ft_info, 1);
                return true;
            }else
            {
              if(buffer_ft_info->static_info.start)
                uop_cache_tracker::record_FT(*buffer_ft_info, 0);
            }
            
          }
        }
        
        // if(i!=0){
        //             std::cout << "trying to Inserting uop cache not head:: Addr 0x" << std::hex<<insert_line->line_start << " off path: " << std::hex<< buffer_ft_info->dynamic_info.first_op_off_path 
        //     << "length: "<< insert_line->n_uops <<std::endl;
        // }
        Entry<Uop_Cache_Key, Uop_Cache_Data> evicted_entry = per_core_uop_cache[uop_cache_proc_id]->insert({insert_line->line_start, buffer_ft_info->static_info}, *insert_line);
        // std::cout << "Inserted uop cache:: Addr 0x" << std::hex<<insert_line->line_start << std::endl;
        // std::cout << "Evicted uop cache:: Addr 0x" << std::hex << evicted_entry.key.first << std::endl;
        DEBUG(uop_cache_proc_id,
              "uop cache line inserted. off_path=%u, addr=0x%llx\n",
              buffer_ft_info->dynamic_info.first_op_off_path, insert_line->line_start);

        if (evicted_entry.valid) {
          // the insertion above evicted a line
          // need to invalidate all lines from the same FT
          FT_Info_Static evicted_ft_info_static = evicted_entry.key.second;
          Addr invalidate_addr = evicted_ft_info_static.start;
          Entry<Uop_Cache_Key, Uop_Cache_Data> invalidated_entry{};
          STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_EVICTED_LENGTH_1 + (evicted_ft_info_static.n_uops / ISSUE_WIDTH));


          if(evicted_entry.data.used)
              STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_EVICTED_USEFUL);
            else{
              // std::cout << "USELESS:: Addr 0x" << std::hex<<invalidate_addr << " eviceted unused" << std::endl;
              STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_EVICTED_USELESS);
            }
              
          
          do {
            invalidated_entry = per_core_uop_cache[uop_cache_proc_id]->invalidate({invalidate_addr, evicted_ft_info_static});
            if (invalidate_addr == evicted_entry.key.first) {
              // this was the one evicted at first
              ASSERT(uop_cache_proc_id, !invalidated_entry.valid);
              invalidated_entry = evicted_entry;
            }
            invalidate_addr += invalidated_entry.data.offset;
            if(invalidated_entry.data.used)
              STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_EVICTED_USEFUL);
            else
              STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_EVICTED_USELESS);
          } while (!invalidated_entry.data.end_of_ft);
        }

        if (buffer_ft_info->dynamic_info.first_op_off_path) {
          if (i == 0) {
            STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERT_SUCCEEDED_OFF_PATH);
          }
          STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERT_SUCCEEDED_OFF_PATH);
        } else {
          if (i == 0) {
            STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_INSERT_SUCCEEDED_ON_PATH);
          }
          STAT_EVENT(uop_cache_proc_id, UOP_CACHE_LINE_INSERT_SUCCEEDED_ON_PATH);
        }
      }
    }
  }

  // stats
  if (accumulation_buffer->size() > UOP_CACHE_FT_LINES_8_OFF_PATH - UOP_CACHE_FT_LINES_1_OFF_PATH + 1) {
    if (buffer_ft_info->dynamic_info.first_op_off_path) {
      STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_LINES_9_AND_MORE_OFF_PATH);
    } else {
      STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_LINES_9_AND_MORE_ON_PATH);
    }
  } else {
    if (buffer_ft_info->dynamic_info.first_op_off_path) {
      STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_LINES_1_OFF_PATH + accumulation_buffer->size() - 1);
    } else {
      STAT_EVENT(uop_cache_proc_id, UOP_CACHE_FT_LINES_1_ON_PATH + accumulation_buffer->size() - 1);
    }
  }
  return success;
}


/**************************************************************************************/
/* end_line_accumulate: called when a uop cache line is built. */
/* the line is added to the buffer, and when the FT has ended, */
/* the entire buffer is inserted into the uop cache. */
/* inserting an FT entirely avoids corner cases, but is not the most accurate. */
void end_line_accumulate(Flag last_line_of_ft) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  // add line to the buffer
  current_accumulation_buffer->emplace_back(*current_accumulating_line);

  if (last_line_of_ft) {
    insert_buffer_into_uopc(current_accumulating_ft, current_accumulation_buffer);
  }
  clear_accumulation(last_line_of_ft);
}

/**************************************************************************************/
/* accumulate_op: accumulate into buffer. Insert into cache at end of an uop cache line. */
void accumulate_op(Op* op) {
  // FT may span multiple cache entries. Additional terminating conditions per line:
  // 1. max uops per line
  // 2. max imm/disp per line (not simulated)
  // 3. max micro-coded instr per line (not simulated)

  if (!UOP_CACHE_ENABLE) {
    return;
  }
  if (op->table_info->cf_type){
    if ((op->table_info->cf_type != CF_CBR) || (op->table_info->cf_type == CF_CBR && op->oracle_info.pred)){
      start_branch_addr = op->inst_info->addr;
      if (op->oracle_info.target <= op->inst_info->addr)
        branch_blk_dir = 1;
      else if(op->oracle_info.target > op->inst_info->addr)
        branch_blk_dir = 2;
    }
      else
        branch_blk_dir = 0;
    

  }
  if (op->table_info->cf_type == CF_CBR){
    tage_source = op->oracle_info.tage_source +1;
    bp_confi = op->bp_confidence+1;
  } else if (op->table_info->cf_type){
    tage_source = 0;
    bp_confi = 0;
  }
    

  // uop cache line begin detection
  // these two conditions should be identical
  ASSERT(uop_cache_proc_id, (current_accumulation_buffer->size() == 0 && current_accumulating_line->n_uops == 0) ==
                            (op->bom && op->inst_info->addr == op->ft_info.static_info.start));
  if (current_accumulating_line->n_uops == 0) {
    if (current_accumulation_buffer->size() == 0) {
      // set current FT info
      *current_accumulating_ft = op->ft_info;
      current_accumulating_ft->static_info.tage_comp = tage_source;
      current_accumulating_ft->static_info.start_branch_addr = start_branch_addr;
      current_accumulating_ft->static_info.bp_confi = bp_confi;
      current_accumulating_ft->static_info.bp_dir = branch_blk_dir;
      
      ASSERT(uop_cache_proc_id, current_accumulating_ft->dynamic_info.first_op_off_path == op->off_path);
    }
    // set per line meta info
    current_accumulating_line->ft_info_dynamic = current_accumulating_ft->dynamic_info;
    current_accumulating_line->line_start = op->inst_info->addr;
  }

  // once current FT is set, they should be identical within the FT
  ASSERT(uop_cache_proc_id, current_accumulating_ft->static_info.start == op->ft_info.static_info.start &&
                            current_accumulating_ft->static_info.length == op->ft_info.static_info.length);

  current_accumulating_line->n_uops++;
  *current_accumulating_op_num = op->op_num;

  // uop cache line end detection
  Addr addr_following_ft = current_accumulating_ft->static_info.start + current_accumulating_ft->static_info.length;
  Addr addr_following_inst = op->inst_info->addr + op->inst_info->trace_info.inst_size;
  // condition 1: if the FT ends
  bool end_condition_1 = op->eom && addr_following_inst == addr_following_ft;
  // condition 2: if the uop cache line reaches the uop num limit
  bool end_condition_2 = current_accumulating_line->n_uops == ISSUE_WIDTH;
  if (end_condition_1 || end_condition_2) {
    if (end_condition_1) {
      current_accumulating_line->end_of_ft = TRUE;
    } else {
      // otherwise, calculate offset pointing to the next uop cache line
      ASSERT(uop_cache_proc_id, current_accumulating_line->line_start);
      Addr next_line_start;
      if (op->eom) {
        next_line_start = op->inst_info->addr + op->inst_info->trace_info.inst_size;
      } else {
        next_line_start = op->inst_info->addr;
      }

      // if next_line_start != npc, the decoupled fe did not end the FT correctly (mispredict or btb miss), or op is off-path
      ASSERT(uop_cache_proc_id, next_line_start == op->oracle_info.npc || op->oracle_info.recover_at_decode || op->oracle_info.recover_at_exec || op->off_path);
      current_accumulating_line->offset = next_line_start - current_accumulating_line->line_start;
    }

    end_line_accumulate(end_condition_1);
  }
}

void recover_uop_cache(void) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  if (current_accumulation_buffer->size() != 0 || current_accumulating_line->n_uops != 0) {
    if (*current_accumulating_op_num >= bp_recovery_info->recovery_op_num) {
      clear_accumulation(TRUE);
    } else {
      // Ops may be decoded out of order: e.g. when a previous op is slowly getting
      // decoded, and the current op is (speculatively) fetched already-decoded from the uop cache.
      // So, ops preceding the recovering op may not have called accumulate_op yet.
      // should not affect the current FT accumulation.

      if (bp_recovery_info->recovery_op->ft_info.static_info == current_accumulating_ft->static_info) {
        // a FT currently accumulating is caught up in a recovery... how come?
        // recall the corner case where the accumulated FT is already in the uop cache because the reuse distance is small.
        // that should be the case here.
        ASSERT(uop_cache_proc_id, uop_cache_lookup_line(current_accumulating_ft->static_info.start, *current_accumulating_ft, FALSE) != NULL);
      }
    }
  } else {
    // no accumulation on-going
    ASSERT(uop_cache_proc_id, current_accumulating_ft->static_info == FT_Info_Static{} &&
                              *current_accumulating_op_num == 0);
  }
}

void dump_uop_reuse(uns prod_id){
  uop_cache_tracker::dump_stats();
  // uop_cache_tracker::cleanup();
}


void ghrp_commitPathHistory(Addr addr){
  if(GHRP_ENABLE)
    per_core_uop_cache[0]->commitPathHistory(addr);
}

void ghrp_recoverPathHistory(){
  if(GHRP_ENABLE)
    per_core_uop_cache[0]->recoverPathHistory();
}

// Add this function to your code:
unsigned get_uop_length(Addr line_start) {
    if (!UOP_CACHE_ENABLE) {
        return 0;
    }
    return uop_cache_tracker::lookup_length(line_start);
}

void stat_ucache_inside(uns proc_id) {
  per_core_uop_cache[proc_id]-> check_used_counters();
}

