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

  uns set_idx_hash(Uop_Cache_Key key) override;

 public:
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
   * the accumulation buffer stores the uop cache lines of an FT accumulated at the end of decoding pipeline.
   * all lines are inserted into the uop cache when the entire FT has been accumulated
   */
  std::vector<Uop_Cache_Data> accumulation_buffer;
  Uop_Cache_Data accumulating_line;
  FT_Info accumulating_ft;
  Counter accumulating_op_num;

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

void recover_uop_cache(void) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }
  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];

  // no accumulation on-going
  if (uc_cpp->accumulation_buffer.size() == 0 && uc_cpp->accumulating_line.n_uops == 0) {
    ASSERT(uc->proc_id, uc_cpp->accumulating_ft.static_info == FT_Info_Static{} && uc_cpp->accumulating_op_num == 0);
    return;
  }

  if (uc_cpp->accumulating_op_num >= bp_recovery_info->recovery_op_num) {
    uop_cache_accumulation_buffer_clear();
    return;
  }

  /*
   * Ops may be decoded out of order:
   *    an older op may be stalled in decode, and a younger op is speculatively fetched from the uop cache.
   * As a result, ops preceding the recovering op may not have called accumulate_op yet.
   * Thus, The recovery should not affect the current FT accumulation.
   */

  if (bp_recovery_info->recovery_op->ft_info.static_info == uc_cpp->accumulating_ft.static_info) {
    /*
     * A FT currently accumulating is caught up in a recovery
     * This is likely a corner case where the FT is already in the uop cache due to a short reuse distance.
     */
    ASSERT(uc->proc_id,
           uop_cache_lookup_line(uc_cpp->accumulating_ft.static_info.start, uc_cpp->accumulating_ft, FALSE) != NULL);
  }
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

void uop_cache_accumulation_buffer_clear() {
  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  uc_cpp->accumulating_line = {};
  uc_cpp->accumulation_buffer.clear();
  uc_cpp->accumulating_ft = {};
  uc_cpp->accumulating_op_num = 0;
}

/*
 * Do not insert the accumulation buffer into the uop cache if any of the following hold:
 *   1. An instruction generates more uops than the uop cache line width.
 *   2. The FT spans more lines than the uop cache associativity.
 */
Flag uop_cache_accumulation_buffer_if_insertable() {
  ASSERT(uc->proc_id, UOP_CACHE_ENABLE);
  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  FT_Info* buffer_ft_info = &uc_cpp->accumulating_ft;
  std::vector<Uop_Cache_Data>* accumulation_buffer = &uc_cpp->accumulation_buffer;
  Flag ft_off_path = buffer_ft_info->dynamic_info.first_op_off_path;

  /* if asked to only insert on-path FTs and the current FT is off-path */
  if (UOP_CACHE_INSERT_ONLY_ONPATH && ft_off_path) {
    return FALSE;
  }

  /*
   * if an inst generates more uops than the uop cache line width,
   * consecutive lines might share the same start address (indicated by a zero offset).
   * It causes ambiguity and we do not insert the FT.
   */
  for (const auto& it : uc_cpp->accumulation_buffer) {
    if (it.end_of_ft || it.offset != 0)
      continue;

    Uop_Cache_Data* uop_cache_line = uop_cache_lookup_line(buffer_ft_info->static_info.start, *buffer_ft_info, FALSE);
    ASSERT(uc->proc_id, !uop_cache_line);
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERT_FAILED_INST_TOO_BIG_ON_PATH + ft_off_path * UOP_CACHE_STAT_OFFSET);
    INC_STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERT_FAILED_INST_TOO_BIG_ON_PATH + ft_off_path * UOP_CACHE_STAT_OFFSET,
                   accumulation_buffer->size());

    return FALSE;
  }

  /* if the FT is too big, do not insert */
  if (accumulation_buffer->size() > UOP_CACHE_ASSOC) {
    Uop_Cache_Data* uop_cache_line = uop_cache_lookup_line(buffer_ft_info->static_info.start, *buffer_ft_info, FALSE);
    ASSERT(uc->proc_id, !uop_cache_line);
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERT_FAILED_FT_TOO_BIG_ON_PATH + ft_off_path * UOP_CACHE_STAT_OFFSET);
    INC_STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERT_FAILED_FT_TOO_BIG_ON_PATH + ft_off_path * UOP_CACHE_STAT_OFFSET,
                   accumulation_buffer->size());

    return FALSE;
  }

  return TRUE;
}

/*
 * The insertion evicted a cache line.
 * To maintain consistency, all lines belonging to the same FT must now be invalidated.
 */
void uop_cache_accumulation_buffer_evict_FT(const Entry<Uop_Cache_Key, Uop_Cache_Data>& evicted_entry) {
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

/*
 * Insert the uop cache line buffer of an FT into the uop cache.
 */
void uop_cache_accumulation_buffer_insert_cache() {
  ASSERT(uc->proc_id, UOP_CACHE_ENABLE);
  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  FT_Info* buffer_ft_info = &uc_cpp->accumulating_ft;
  Flag off_path = buffer_ft_info->dynamic_info.first_op_off_path;

  /*
   * If the first line from the FT is already in the uop cache, all lines of the FT should be in the uop cache;
   * otherwise, all lines should not be in the uop cache.
   */
  auto first_line = uc_cpp->accumulation_buffer.begin();
  Uop_Cache_Data* first_lookup = uop_cache_lookup_line(first_line->line_start, *buffer_ft_info, TRUE);

  bool lines_exist = first_lookup != nullptr;
  if (lines_exist) {
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERT_CONFLICTED_SHORT_REUSE_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
  } else {
    STAT_EVENT(uc->proc_id, UOP_CACHE_FT_INSERT_SUCCEEDED_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
  }

  for (const auto& it : uc_cpp->accumulation_buffer) {
    Uop_Cache_Data* uop_cache_line =
        (it == *first_line) ? first_lookup : uop_cache_lookup_line(it.line_start, *buffer_ft_info, TRUE);

    /*
     * This line may already exist in the uop cache if the reuse distance in cycles is too short.
     * In such cases, the first occurrence was not yet inserted while the second was looked up.
     * So by the time the second occurrence was inserted, the first occurrence was already in the uop cache.
     * As a result, the look-up above has updated the replacement policy, and we skip the insertion.
     */
    if (lines_exist) {
      ASSERT(uc->proc_id, uop_cache_line && *uop_cache_line == it);
      STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERT_CONFLICTED_SHORT_REUSE_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);

      continue;
    }
    ASSERT(uc->proc_id, !uop_cache_line);

    Uop_Cache_Key uop_cache_key = {it.line_start, buffer_ft_info->static_info};
    Entry<Uop_Cache_Key, Uop_Cache_Data> evicted_entry = uc_cpp->uop_cache->insert(uop_cache_key, it);
    DEBUG(uc->proc_id, "uop cache line inserted. off_path=%u, addr=0x%llx\n", off_path, it.line_start);

    if (evicted_entry.valid) {
      uop_cache_accumulation_buffer_evict_FT(evicted_entry);
    }
    STAT_EVENT(uc->proc_id, UOP_CACHE_LINE_INSERT_SUCCEEDED_ON_PATH + off_path * UOP_CACHE_STAT_OFFSET);
  }
}

void uop_cache_accumulation_buffer_update_stat() {
  ASSERT(uc->proc_id, UOP_CACHE_ENABLE);
  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];

  FT_Info* buffer_ft_info = &uc_cpp->accumulating_ft;
  std::vector<Uop_Cache_Data>* accumulation_buffer = &uc_cpp->accumulation_buffer;

  if (accumulation_buffer->size() > UOP_CACHE_FT_LINES_8_OFF_PATH - UOP_CACHE_FT_LINES_1_OFF_PATH + 1) {
    if (buffer_ft_info->dynamic_info.first_op_off_path) {
      STAT_EVENT(uc->proc_id, UOP_CACHE_FT_LINES_9_AND_MORE_OFF_PATH);
    } else {
      STAT_EVENT(uc->proc_id, UOP_CACHE_FT_LINES_9_AND_MORE_ON_PATH);
    }
  } else {
    if (buffer_ft_info->dynamic_info.first_op_off_path) {
      STAT_EVENT(uc->proc_id, UOP_CACHE_FT_LINES_1_OFF_PATH + accumulation_buffer->size() - 1);
    } else {
      STAT_EVENT(uc->proc_id, UOP_CACHE_FT_LINES_1_ON_PATH + accumulation_buffer->size() - 1);
    }
  }
}

/*
 * Accumulation:
 *    accumulate uops into a buffer
 *    insert into the uop cache at the end of a cache line
 *
 * FT may span multiple cache entries. Additional terminating conditions per line:
 *    1. max uops per line
 *    2. max imm/disp per line (not simulated)
 *    3. max micro-coded instr per line (not simulated)
 */
void uop_cache_accumulation_buffer_update(Op* op) {
  if (!UOP_CACHE_ENABLE) {
    return;
  }

  Uop_Cache_Stage_Cpp* uc_cpp = &per_core_uc_stage[uc->proc_id];
  auto& buffer = uc_cpp->accumulation_buffer;
  auto& line = uc_cpp->accumulating_line;
  auto& ft = uc_cpp->accumulating_ft;

  bool is_line_start = (buffer.size() == 0 && line.n_uops == 0);
  bool is_ft_start = (op->bom && op->inst_info->addr == op->ft_info.static_info.start);
  ASSERT(uc->proc_id, is_line_start == is_ft_start);

  if (line.n_uops == 0) {
    if (buffer.empty()) {
      ft = op->ft_info;
      ASSERT(uc->proc_id, ft.dynamic_info.first_op_off_path == op->off_path);
    }

    line.ft_info_dynamic = ft.dynamic_info;
    line.line_start = op->inst_info->addr;
  }

  // validate FT consistency
  ASSERT(uc->proc_id, ft.static_info.start == op->ft_info.static_info.start &&
                          ft.static_info.length == op->ft_info.static_info.length);

  line.n_uops++;
  uc_cpp->accumulating_op_num = op->op_num;

  // detect line termination conditions
  Addr ft_end_addr = ft.static_info.start + ft.static_info.length;
  Addr inst_end_addr = op->inst_info->addr + op->inst_info->trace_info.inst_size;

  bool is_ft_end = op->eom && (inst_end_addr == ft_end_addr);
  bool is_line_end = (line.n_uops == UOP_CACHE_WIDTH);
  if (!is_ft_end && !is_line_end)
    return;

  if (!is_ft_end) {
    ASSERT(uc->proc_id, line.line_start);

    // handle continued FT in the next cache line
    Addr next_line_start = op->eom ? inst_end_addr : op->inst_info->addr;
    line.offset = next_line_start - line.line_start;

    bool valid_npc = (next_line_start == op->oracle_info.npc);
    bool recover = (op->oracle_info.recover_at_decode || op->oracle_info.recover_at_exec || op->off_path);
    ASSERT(uc->proc_id, valid_npc || recover);

    // end line accumulation
    uc_cpp->accumulation_buffer.emplace_back(uc_cpp->accumulating_line);
    uc_cpp->accumulating_line = {};
    return;
  }

  line.end_of_ft = TRUE;
  uc_cpp->accumulation_buffer.emplace_back(uc_cpp->accumulating_line);

  // the entire buffer is inserted into the uop cache when the FT has ended
  Flag if_insertable = uop_cache_accumulation_buffer_if_insertable();
  if (if_insertable) {
    // inserting an FT entirely avoids corner cases, but is not the most accurate
    uop_cache_accumulation_buffer_insert_cache();
  }

  uop_cache_accumulation_buffer_update_stat();
  uop_cache_accumulation_buffer_clear();
}
