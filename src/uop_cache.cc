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
        STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERTED_ONPATH_USED_ONPATH + uoc_data->ft_first_op_off_path);
      }
    }

    ASSERT(uc->proc_id, uoc_data);

    if (!uoc_data->used && !offpath) {
      STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERTED_ONPATH_USED_ONPATH + uoc_data->ft_first_op_off_path);
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
  uns lines_needed = inserting_FT.size();

  ASSERT(uc->proc_id, lines_needed != 0);

  Uop_Cache_Key uop_cache_key = {inserting_FT[0].line_start, inserting_FT_info.static_info};

  uns free_space = uc_cpp->uop_cache->get_free_space(uop_cache_key);

  while (free_space < lines_needed) {
    Entry<Uop_Cache_Key, Uop_Cache_Data> evicted_entry = uc_cpp->uop_cache->evict_one_line(uop_cache_key);

    if (evicted_entry.valid) {
      uop_cache_evict_FT(evicted_entry);
    }
    ASSERT(uc->proc_id, uc_cpp->uop_cache->get_free_space(uop_cache_key) > free_space);
    free_space = uc_cpp->uop_cache->get_free_space(uop_cache_key);
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
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_SHORT_REUSE_CONFLICTED_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
  } else {
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERT_SUCCEEDED_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
  }

  // evict the uop cache entry with fake nop contained when the inserting FT has same start addr and length
  // so we can insert the new ft with valid ops
  if (lines_exist && first_lookup->contains_fake_nop) {
    Uop_Cache_Key key = {first_line->line_start, inserting_FT_info.static_info};
    Entry<Uop_Cache_Key, Uop_Cache_Data> invalidated_entry;
    invalidated_entry = uc_cpp->uop_cache->invalidate(key);
    if (invalidated_entry.valid) {
      uop_cache_evict_FT(invalidated_entry);
    }
    first_lookup = uop_cache_lookup_line(first_line->line_start, inserting_FT_info, TRUE);
    lines_exist = first_lookup != nullptr;
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
    if (lines_exist) {
      ASSERT(uc->proc_id, uop_cache_line && *uop_cache_line == it);
      STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_SHORT_REUSE_CONFLICTED_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
      continue;
    }
    ASSERT(uc->proc_id, !uop_cache_line);

    Uop_Cache_Key uop_cache_key = {it.line_start, inserting_FT_info.static_info};
    Entry<Uop_Cache_Key, Uop_Cache_Data> evicted_entry = uc_cpp->uop_cache->insert(uop_cache_key, it);
    DEBUG(uc->proc_id, "uop cache line inserted. off_path=%u, addr=0x%llx\n", off_path, it.line_start);

    if (evicted_entry.valid) {
      uop_cache_evict_FT(evicted_entry);
    }
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
 * Generate uop cache data for a given FT and fill `out`.
 * This is the moved implementation of the previous FT::generate_uop_cache_data().
 */
void generate_uop_cache_data_from_FT(FT* ft, std::vector<Uop_Cache_Data>& out) {
  out.clear();
  // Initialize current line tracking
  Uop_Cache_Data current_line = {};
  FT_Info ft_info = ft->get_ft_info();
  bool line_started = false;
  bool is_ft_end = false;

  auto& ops = ft->ops;
  for (size_t i = 0; i < ops.size(); ++i) {
    Op* op = ops[i];

    // Start a new line if needed
    if (!line_started) {
      current_line.line_start = op->inst_info->addr;
      current_line.ft_first_op_off_path = ft->get_first_op_off_path();
      current_line.contains_fake_nop = ft->get_contains_fake_nop();
      current_line.n_uops = 0;
      current_line.end_of_ft = FALSE;
      current_line.used = 0;
      current_line.priority = 0;
      line_started = true;
    }
    current_line.n_uops++;

    // Check for line termination conditions
    Addr ft_end_addr = ft->get_start_addr() + ft_info.static_info.length;
    Addr inst_end_addr = op->inst_info->addr + op->inst_info->trace_info.inst_size;

    is_ft_end = op->eom && (inst_end_addr == ft_end_addr);
    bool is_line_end = (current_line.n_uops == UOP_CACHE_WIDTH);
    ASSERT(uc->proc_id, current_line.n_uops <= UOP_CACHE_WIDTH);

    // Determine if this is the last op in the current line
    if (is_ft_end || is_line_end || i == ops.size() - 1) {
      if (is_ft_end) {
        current_line.end_of_ft = TRUE;
        current_line.offset = 0;  // No next line for FT end
      } else if (i + 1 < ops.size()) {
        // Calculate offset to next line start
        Op* next_op = ops[i + 1];
        Addr next_line_start = next_op->inst_info->addr;
        current_line.offset = next_line_start - current_line.line_start;
        current_line.end_of_ft = FALSE;
      } else {
        // Last op but not FT end
        current_line.offset = inst_end_addr - current_line.line_start;
        current_line.end_of_ft = TRUE;  // Assume end of FT if we're at the last op
      }
      out.push_back(current_line);
      current_line = {};
      line_started = false;
    }
  }
  ASSERT(uc->proc_id, ft->op_pos == ft->ops.size());
  ft->op_pos = 0;
  ft->generate_ft_info();
  ASSERT(uc->proc_id, !line_started && is_ft_end);
}

void uop_cache_insert_FT(FT* ft) {
  ASSERT(uc->proc_id, ft);
  std::vector<Uop_Cache_Data> buffer;
  generate_uop_cache_data_from_FT(ft, buffer);

  auto ft_info = ft->get_ft_info();
  // the entire buffer is inserted into the uop cache when the FT has ended
  Flag if_insertable = uop_cache_FT_if_insertable(buffer, ft_info);
  if (if_insertable) {
    // inserting an FT entirely avoids corner cases, but is not the most accurate
    uop_cache_insert_FT(buffer, ft_info);
  }

  uop_cache_insert_FT_update_stat(buffer, ft_info);
}

/*
 * Accumulation:
 *    tried to insert op
 *    will insert the whole FT if last op of FT detected
 */
void uop_cache_insert_op(Op* op) {
  if (!UOP_CACHE_ENABLE)
    return;
  ASSERT(uc->proc_id, op && op->parent_FT);
  if (op->parent_FT_off_path && op->parent_FT_off_path->get_last_op() == op)
    uop_cache_insert_FT(op->parent_FT_off_path);

  if (!op->parent_FT_off_path && op == op->parent_FT->get_last_op())
    uop_cache_insert_FT(op->parent_FT);
}