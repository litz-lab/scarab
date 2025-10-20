/*
 * Copyright 2025 University of California Santa Cruz
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
 * File         : uop_cache.cc
 * Author       : Peter Braun
 *                Litz Lab
 * Date         : 10.28.2020
 *                6. 8. 2025
 * Description  : Interface for interacting with uop cache object.
 *                  Following Kotra et. al.'s MICRO 2020 description of uop cache baseline
 ***************************************************************************************/

#include "uop_cache.h"

#include <map>
#include <vector>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "core.param.h"
#include "general.param.h"
#include "memory/memory.param.h"

#include "bp/bp.h"
#include "isa/isa_macros.h"
#include "libs/cache_lib.h"
#include "libs/cpp_cache.h"
#include "memory/memory.h"

#include "ft.h"
#include "icache_stage.h"
#include "op_pool.h"
#include "statistics.h"
#include "uop_queue_stage.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_UOP_CACHE, ##args)

// Uop cache is byte-addressable, so tag/set index are generated from full address (no offset)
// Uop cache uses icache tag + icache offset as full TAG
#define UOP_CACHE_LINE_SIZE ICACHE_LINE_SIZE

typedef std::pair<Addr, FT_Info_Static> Uop_Cache_Key;

/**************************************************************************************/
/* Local Prototypes */

class Uop_Cache : public Cpp_Cache<Uop_Cache_Key, Uop_Cache_Data> {
 protected:
  /*
   * 'offset_bits' specifies where the set index bits begin within the address.
   * These bits immediately follow the block offset, which is log2(line_bytes).
   * By adjusting 'line_bytes', the user indirectly controls how the address is hashed into sets.
   */
  uns offset_bits;

 public:
  uns set_idx_hash(Uop_Cache_Key key) override;
  Uop_Cache(uns nl, uns asc, uns lb, Repl_Policy rp)
      : Cpp_Cache<Uop_Cache_Key, Uop_Cache_Data>(nl, asc, lb, rp),
        offset_bits(static_cast<uns>(std::log2(line_bytes))) {}
};

uns Uop_Cache::set_idx_hash(Uop_Cache_Key key) {
  // use % instead of masking to support num_sets that is not a power of 2
  return (key.first >> offset_bits) % num_sets;
}

typedef struct Uop_Cache_Stage_Cpp_struct {
  Uop_Cache* uop_cache;

  /*
   * the lookup buffer stores the uop cache lines of an FT to be consumed by the icache stage.
   * all lines are cleared when the entire FT has been consumed by the icache stage
   */
  std::vector<Uop_Cache_Data> lookup_buffer;
  uns num_looked_up_lines;
} Uop_Cache_Stage_Cpp;

/**************************************************************************************/
/* Global Variables */

static std::vector<Uop_Cache_Stage_Cpp> per_core_uc_stage;
Uop_Cache_Stage* uc = NULL;

/**************************************************************************************/
/* Operator Overload */

inline bool operator==(const FT_Info_Static& lhs, const FT_Info_Static& rhs) {
  return lhs.start == rhs.start && lhs.length == rhs.length && lhs.n_uops == rhs.n_uops;
}

inline bool operator==(const Uop_Cache_Key& lhs, const Uop_Cache_Key& rhs) {
  return lhs.first == rhs.first && lhs.second == rhs.second;
}

inline bool operator==(const Uop_Cache_Data& lhs, const Uop_Cache_Data& rhs) {
  return lhs.n_uops == rhs.n_uops && lhs.offset == rhs.offset && lhs.end_of_ft == rhs.end_of_ft;
}

/**************************************************************************************/
/* External Vanilla Model Func */

void alloc_mem_uop_cache(uns num_cores) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  per_core_uc_stage.resize(num_cores);
}

void set_uop_cache_stage(Uop_Cache_Stage* new_uc) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  uc = new_uc;
}

void init_uop_cache_stage(uns8 proc_id, const char* name) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  uc->current_ft = NULL;

  DEBUG(proc_id, "Initializing %s stage\n", name);

  ASSERT(0, uc);
  memset(uc, 0, sizeof(Uop_Cache_Stage));

  uc->proc_id = proc_id;
  uc->sd.name = (char*)strdup(name);

  uc->sd.max_op_count = UOPC_ISSUE_WIDTH;
  uc->sd.op_count = 0;
  uc->sd.ops = (Op**)calloc(UOPC_ISSUE_WIDTH, sizeof(Op*));

  // The cache library computes the number of entries from cache_size_bytes/cache_line_size_bytes
  per_core_uc_stage[proc_id].uop_cache =
      new Uop_Cache(UOP_CACHE_LINES, UOP_CACHE_ASSOC, UOP_CACHE_LINE_SIZE, (Repl_Policy)UOP_CACHE_REPL);
}

/**************************************************************************************/
/* Uop Cache Lookup Buffer Func */

Flag uop_cache_lookup_ft_and_fill_lookup_buffer(FT_Info ft_info, Flag offpath) {
  if (!UOP_CACHE_ENABLE) {
    return FALSE;
  }

  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  ASSERT(uc->proc_id, uc_cpp->lookup_buffer.empty());
  ASSERT(uc->proc_id, uc_cpp->num_looked_up_lines == 0);
  Uop_Cache_Data* uoc_data = NULL;
  Addr lookup_addr = ft_info.static_info.start;
  do {
    uoc_data = uop_cache_lookup_line(lookup_addr, ft_info, TRUE);
    if (uc_cpp->lookup_buffer.empty()) {
      DEBUG(uc->proc_id, "UOC %s. ft_start=0x%llx, ft_length=%lld\n", uoc_data ? "hit" : "miss",
            ft_info.static_info.start, ft_info.static_info.length);
      if (!uoc_data) {
        return FALSE;
      }
      if (!uoc_data->used && !offpath) {
        STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERTED_ONPATH_USED_ONPATH + uoc_data->ft_info_dynamic.first_op_off_path);
      }
    }

    ASSERT(uc->proc_id, uoc_data);

    if (!uoc_data->used && !offpath) {
      STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERTED_ONPATH_USED_ONPATH + uoc_data->ft_info_dynamic.first_op_off_path);
      uoc_data->used += 1;
    }

    uc_cpp->lookup_buffer.emplace_back(*uoc_data);
    ASSERT(uc->proc_id, (uoc_data->offset == 0) == uoc_data->end_of_ft);
    lookup_addr += uoc_data->offset;
  } while (!uoc_data->end_of_ft);

  uc->lookups_per_cycle_count++;
  ASSERT(ic->proc_id, uc->lookups_per_cycle_count <= UOP_CACHE_READ_PORTS);

  return TRUE;
}

/* uop_cache_consume_uops_from_lookup_buffer: consume some uops from the uopc lookup buffer
 * if the uop num of the current line > requested, it will be partially consumed and the line index is unchanged
 * if the uop num of the current line <= requested, it will be fully consumed and the line index is incremented
 */
Uop_Cache_Data uop_cache_consume_uops_from_lookup_buffer(uns requested) {
  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  Uop_Cache_Data* uop_cache_line = &uc_cpp->lookup_buffer.at(uc_cpp->num_looked_up_lines);
  Uop_Cache_Data consumed_uop_cache_line = *uop_cache_line;
  if (uop_cache_line->n_uops > requested) {
    // the uopc line has more uops than requested; cannot fully consume it
    consumed_uop_cache_line.n_uops = requested;
    // update the remaining uops
    uop_cache_line->n_uops -= requested;
    if (consumed_uop_cache_line.end_of_ft) {
      consumed_uop_cache_line.end_of_ft = FALSE;
    }
  } else {
    // the current line is fully consumed; move to the next line
    uc_cpp->num_looked_up_lines += 1;
  }
  return consumed_uop_cache_line;
}

void uop_cache_clear_lookup_buffer() {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  uc_cpp->lookup_buffer.clear();
  uc_cpp->num_looked_up_lines = 0;
}

Uop_Cache_Data* uop_cache_lookup_line(Addr line_start, FT_Info ft_info, Flag update_repl) {
  if (!UOP_CACHE_ENABLE) {
    return NULL;
  }

  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  Uop_Cache_Data* uoc_data = uc_cpp->uop_cache->access({line_start, ft_info.static_info}, update_repl == TRUE);
  return uoc_data;
}

/**************************************************************************************/
/* Uop Cache Accumulation Buffer Func */

const static int UOP_CACHE_STAT_OFFSET = 4;

/*
 * Do not insert the FT into the uop cache if any of the following hold:
 *   1. An instruction generates more uops than the uop cache line width.
 *   2. The FT spans more lines than the uop cache associativity.
 */
Flag uop_cache_FT_if_insertable(const std::vector<Uop_Cache_Data> inserting_FT, FT_Info inserting_FT_info) {
  ASSERT(uc->proc_id, UOP_CACHE_ENABLE);

  Flag ft_off_path = inserting_FT_info.dynamic_info.first_op_off_path;

  /* if asked to only insert on-path FTs and the current FT is off-path */
  if (UOP_CACHE_INSERT_ONLY_ONPATH && ft_off_path) {
    return FALSE;
  }

  /*
   * if an inst generates more uops than the uop cache line width,
   * consecutive lines might share the same start address (indicated by a zero offset).
   * It causes ambiguity and we do not insert the FT.
   */
  for (const auto& it : inserting_FT) {
    if (it.end_of_ft || it.offset != 0)
      continue;

    Uop_Cache_Data* uop_cache_line =
        uop_cache_lookup_line(inserting_FT_info.static_info.start, inserting_FT_info, FALSE);
    ASSERT(uc->proc_id, !uop_cache_line);
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERT_FAILED_INST_TOO_BIG_ON_PATH + ft_off_path * UOP_CACHE_STAT_OFFSET);
    INC_STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERT_FAILED_INST_TOO_BIG_ON_PATH + ft_off_path * UOP_CACHE_STAT_OFFSET,
                   inserting_FT.size());

    return FALSE;
  }

  /* if the FT is too big, do not insert */
  if (inserting_FT.size() > UOP_CACHE_ASSOC) {
    Uop_Cache_Data* uop_cache_line =
        uop_cache_lookup_line(inserting_FT_info.static_info.start, inserting_FT_info, FALSE);
    ASSERT(uc->proc_id, !uop_cache_line);
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERT_FAILED_FT_TOO_BIG_ON_PATH + ft_off_path * UOP_CACHE_STAT_OFFSET);
    INC_STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERT_FAILED_FT_TOO_BIG_ON_PATH + ft_off_path * UOP_CACHE_STAT_OFFSET,
                   inserting_FT.size());

    return FALSE;
  }

  return TRUE;
}

/*
 * The insertion evicted a cache line.
 * To maintain consistency, all lines belonging to the same FT must now be invalidated.
 */
void uop_cache_evict_FT(const Entry<Uop_Cache_Key, Uop_Cache_Data>& evicted_entry) {
  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  FT_Info_Static evicted_ft_info_static = evicted_entry.key.second;
  Addr invalidate_addr = evicted_ft_info_static.start;
  Entry<Uop_Cache_Key, Uop_Cache_Data> invalidated_entry{};

  do {
    invalidated_entry = uc_cpp->uop_cache->invalidate({invalidate_addr, evicted_ft_info_static});
    if (invalidate_addr == evicted_entry.key.first) {
      // this was the one evicted at first
      ASSERT(uc->proc_id, !invalidated_entry.valid);
      invalidated_entry = evicted_entry;
    }
    invalidate_addr += invalidated_entry.data.offset;

    if (invalidated_entry.data.used)
      STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_EVICTED_USEFUL);
    else
      STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_EVICTED_USELESS);
  } while (!invalidated_entry.data.end_of_ft);
}

void uop_cache_preallocate_space(const std::vector<Uop_Cache_Data>& inserting_FT, FT_Info inserting_FT_info) {
  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];

  // Track which slots we've already pre-allocated per set
  std::map<uns, uns> preallocated_count_per_set;

  for (const auto& it : inserting_FT) {
    Uop_Cache_Key uop_cache_key = {it.line_start, inserting_FT_info.static_info};
    uns set_idx = uc_cpp->uop_cache->set_idx_hash(uop_cache_key);

    // Check if this key already exists in cache
    if (uc_cpp->uop_cache->access(uop_cache_key, FALSE) != NULL) {
      continue;  // Key already exists, no space needed
    }

    // Count how many slots we've already pre-allocated for this set
    uns already_preallocated = preallocated_count_per_set[set_idx];

    if (uc_cpp->uop_cache->has_no_space_for_key_with_preallocated(uop_cache_key, already_preallocated)) {
      Entry<Uop_Cache_Key, Uop_Cache_Data> evicted_entry = uc_cpp->uop_cache->make_space_for_key(uop_cache_key);

      if (evicted_entry.valid) {
        DEBUG(uc->proc_id, "Pre-allocation evicted FT start=0x%llx, line=0x%llx for set %u\n",
              evicted_entry.key.second.start, evicted_entry.key.first, set_idx);
        uop_cache_evict_FT(evicted_entry);
      }
    }

    // Increment our pre-allocated count for this set
    preallocated_count_per_set[set_idx]++;
  }
}

void uop_cache_insert_FT(const std::vector<Uop_Cache_Data> inserting_FT, FT_Info inserting_FT_info) {
  ASSERT(uc->proc_id, UOP_CACHE_ENABLE);
  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  Flag off_path = inserting_FT_info.dynamic_info.first_op_off_path;

  // Check if first line already exists
  auto first_line = inserting_FT.begin();
  Uop_Cache_Data* first_lookup = uop_cache_lookup_line(first_line->line_start, inserting_FT_info, TRUE);

  bool lines_exist = first_lookup != nullptr;
  if (lines_exist) {
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERT_CONFLICTED_SHORT_REUSE_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
  } else {
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERT_SUCCEEDED_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
  }

  // Pre-allocate space for the entire FT before insertion
  if (!lines_exist) {
    uop_cache_preallocate_space(inserting_FT, inserting_FT_info);
  }

  for (const auto& it : inserting_FT) {
    Uop_Cache_Data* uop_cache_line =
        (it == *first_line) ? first_lookup : uop_cache_lookup_line(it.line_start, inserting_FT_info, TRUE);

    /*
     * This line may already exist in the uop cache if the reuse distance in cycles is too short.
     * In such cases, the first occurrence was not yet inserted while the second was looked up.
     * So by the time the second occurrence was inserted, the first occurrence was already in the uop cache.
     * As a result, the look-up above has updated the replacement policy, and we skip the insertion.
     */

    if (lines_exist && uop_cache_line) {
      if (!(uop_cache_line && *uop_cache_line == it)) {
        DEBUG(uc->proc_id, "Inconsistency detected - invalidating FT from addr=0x%llx\n", it.line_start);
        // Replace this line and invalidate all following lines in the FT
        Addr invalidate_addr = it.line_start;
        Entry<Uop_Cache_Key, Uop_Cache_Data> invalidated_entry;

        // First, replace the inconsistent line with new data
        Uop_Cache_Key uop_cache_key = {invalidate_addr, inserting_FT_info.static_info};
        invalidated_entry = uc_cpp->uop_cache->replace_if_exists(uop_cache_key, it);
        ASSERT(uc->proc_id, invalidated_entry.valid);  // Should always succeed since line exists

        // Now invalidate all subsequent lines in the FT
        while (!invalidated_entry.data.end_of_ft) {
          invalidate_addr += invalidated_entry.data.offset;
          Uop_Cache_Key next_key = {invalidate_addr, inserting_FT_info.static_info};
          invalidated_entry = uc_cpp->uop_cache->invalidate(next_key);

          if (invalidated_entry.valid) {
            DEBUG(uc->proc_id, "Invalidated following line at addr=0x%llx\n", invalidate_addr);
          } else {
            // No more lines found - this can happen if the FT was partially cached
            break;
          }
        }

        DEBUG(uc->proc_id, "Completed invalidation of inconsistent FT from addr=0x%llx\n", it.line_start);
      }

      STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERT_CONFLICTED_SHORT_REUSE_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
      continue;
    }
    ASSERT(uc->proc_id, !uop_cache_line);

    Uop_Cache_Key uop_cache_key = {it.line_start, inserting_FT_info.static_info};

    Entry<Uop_Cache_Key, Uop_Cache_Data> evicted_entry = uc_cpp->uop_cache->insert(uop_cache_key, it);
    DEBUG(uc->proc_id, "uop cache line inserted. off_path=%u, addr=0x%llx\n", off_path, it.line_start);

    // Since we pre-allocated space, no eviction should occur during insertion
    ASSERT(uc->proc_id, !evicted_entry.valid);

    STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERT_SUCCEEDED_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
  }
}

void uop_cache_insert_FT_update_stat(const std::vector<Uop_Cache_Data> inserting_FT, FT_Info inserting_FT_info) {
  ASSERT(uc->proc_id, UOP_CACHE_ENABLE);

  if (inserting_FT.size() > UOP_CACHE_FT_LINES_8_OFF_PATH - UOP_CACHE_FT_LINES_1_OFF_PATH + 1) {
    if (inserting_FT_info.dynamic_info.first_op_off_path) {
      STAT_EVENT(uc->proc_id, UOP_CACHE_FT_LINES_9_AND_MORE_OFF_PATH);
    } else {
      STAT_EVENT(uc->proc_id, UOP_CACHE_FT_LINES_9_AND_MORE_ON_PATH);
    }
  } else {
    if (inserting_FT_info.dynamic_info.first_op_off_path) {
      STAT_EVENT(uc->proc_id, UOP_CACHE_FT_LINES_1_OFF_PATH + inserting_FT.size() - 1);
    } else {
      STAT_EVENT(uc->proc_id, UOP_CACHE_FT_LINES_1_ON_PATH + inserting_FT.size() - 1);
    }
  }
}

/*
 * Accumulation:
 *    tried to insert op
 *    will insert the whole FT if last op of FT detected
 */
void uop_cache_insert_op(Op* op) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  if (op && op->parent_FT && op == op->parent_FT->get_last_op()) {
    auto buffer = op->parent_FT->generate_uop_cache_data();

    auto ft = op->parent_FT->get_ft_info();
    // the entire buffer is inserted into the uop cache when the FT has ended
    Flag if_insertable = uop_cache_FT_if_insertable(buffer, ft);
    if (if_insertable) {
      // inserting an FT entirely avoids corner cases, but is not the most accurate
      uop_cache_insert_FT(buffer, ft);
    }

    uop_cache_insert_FT_update_stat(buffer, ft);
  }
}
