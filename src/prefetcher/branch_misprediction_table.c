/***************************************************************************************
 * File         : branch_misprediction_table.cc
 * Author       : Peter Braun
 * Date         : 10.28.2020
 * Description  : Store branch count and branch misprediction count for each branch.
 *                Used to identify candidates for dual path prefetching into the uop cache.
 ***************************************************************************************/

#include "prefetcher/branch_misprediction_table.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "prefetcher/pref.param.h"

#include "libs/cache_lib.h"
#include "libs/hash_lib.h"

#include "statistics.h"

#include <stdlib.h>

#define H2P_MIN_EXEC      5000
#define H2P_MIN_MISPRED    334
#define H2P_MAX_ACCURACY  0.99f

typedef struct Bm_Info_struct {
  Counter branch_count;
  Counter branch_mispred_count;
  Flag    is_data_dep;
  Flag    is_in_loop;
} Bm_Info;

/**************************************************************************************/
/* Global Variables */

uns8 proc_id;

Hash_Table inf_size_bm_table;

typedef struct { Addr start; Addr end; } Loop_Range;
static Loop_Range* loop_ranges = NULL;
static int loop_range_count = 0;
static int loop_range_capacity = 0;

/**************************************************************************************/
/* init_branch_misprediction_table */

void init_branch_misprediction_table(uns8 pid) {
  proc_id = pid;
  if (BRANCH_MISPREDICTION_TABLE_SIZE == 0) {
    init_hash_table(&inf_size_bm_table, "infinite sized", 15000000, sizeof(Bm_Info));
    // cpp version is not a lib yet. only one instance.
  }
}

float get_branch_misprediction_rate(Addr pc) {
  float rate = 0;
  if (BRANCH_MISPREDICTION_TABLE_SIZE == 0) {
    Bm_Info* info = (Bm_Info*)hash_table_access(&inf_size_bm_table, pc);
    if (info) {
      rate = info->branch_mispred_count / info->branch_count;
    }
  }
  return rate;
}

void increment_branch_count(Addr pc) {
  if (BRANCH_MISPREDICTION_TABLE_SIZE == 0) {
    Flag new_entry;
    Bm_Info* info = (Bm_Info*)hash_table_access_create(&inf_size_bm_table, pc, &new_entry);
    if (new_entry)
      memset(info, 0, sizeof(*info));
    info->branch_count++;
  }
}

void increment_branch_mispredictions(Addr pc) {
  if (BRANCH_MISPREDICTION_TABLE_SIZE == 0) {
    Flag new_entry;
    Bm_Info* info = (Bm_Info*)hash_table_access_create(&inf_size_bm_table, pc, &new_entry);
    if (new_entry)
      memset(info, 0, sizeof(*info));
    info->branch_mispred_count++;
  }
}

// debugging print -------------------------------------------------------------------------------------
typedef struct {
  Addr    pc;
  Counter branch_count;
  Counter branch_mispred_count;
  float   accuracy;
  Flag    is_data_dep;
  Flag    is_in_loop;
} H2P_Entry;

static int h2p_cmp(const void* a, const void* b) {
  float diff = ((H2P_Entry*)b)->accuracy - ((H2P_Entry*)a)->accuracy;
  return (diff > 0) - (diff < 0);
}
// debugging print -------------------------------------------------------------------------------------

void count_h2p_branches(uns8 proc_id) {
  H2P_Entry* entries = NULL;
  int h2p_count = 0;
  int capacity = 0;

  for (uns i = 0; i < inf_size_bm_table.buckets; i++) {
    for (Hash_Table_Entry* e = inf_size_bm_table.entries[i]; e; e = e->next) {
      Bm_Info* info = (Bm_Info*)e->data;
      for (int r = 0; r < loop_range_count; r++) {
        if (e->key >= loop_ranges[r].start && e->key <= loop_ranges[r].end) {
          info->is_in_loop = TRUE;
          break;
        }
      }
    }
  }

  for (uns i = 0; i < inf_size_bm_table.buckets; i++) {
    for (Hash_Table_Entry* e = inf_size_bm_table.entries[i]; e; e = e->next) {
      Bm_Info* info = (Bm_Info*)e->data;

      if (info->branch_count < H2P_MIN_EXEC)
        continue;

      if (info->branch_mispred_count < H2P_MIN_MISPRED)
        continue;

      float accuracy = 1.0f - ((float)info->branch_mispred_count / (float)info->branch_count);

      if (accuracy < H2P_MAX_ACCURACY) {
        STAT_EVENT(proc_id, H2P_BRANCH_COUNT);

        if (info->is_data_dep) {
          STAT_EVENT(proc_id, H2P_DATA_DEP_BRANCH_COUNT);
          if (info->is_in_loop) {
            STAT_EVENT(proc_id, H2P_LOOP_DATA_DEP_BRANCH_COUNT);
          }
        }

        if (h2p_count == capacity) {
          capacity = capacity == 0 ? 16 : capacity * 2;
          entries = (H2P_Entry*)realloc(entries, sizeof(H2P_Entry) * capacity);
        }
        entries[h2p_count++] = (H2P_Entry){e->key, info->branch_count, info->branch_mispred_count, accuracy, info->is_data_dep, info->is_in_loop};
      }
    }
  }

  // debugging print -------------------------------------------------------------------------------------
  if (h2p_count > 0)
    qsort(entries, h2p_count, sizeof(H2P_Entry), h2p_cmp);

  fprintf(mystdout, "\n[H2P Branches] proc_id:%u  total:%d\n", proc_id, h2p_count);
  fprintf(mystdout, "  %-18s  %10s  %10s  %10s  %8s  %8s\n", "PC", "exec", "mispred", "accuracy", "data_dep", "in_loop");
  for (int i = 0; i < h2p_count; i++)
    fprintf(mystdout, "  0x%016llx  %10llu  %10llu  %9.4f%%  %8s  %8s\n",
            (unsigned long long)entries[i].pc,
            (unsigned long long)entries[i].branch_count,
            (unsigned long long)entries[i].branch_mispred_count,
            entries[i].accuracy * 100.0f,
            entries[i].is_data_dep ? "YES" : "NO",
            entries[i].is_in_loop  ? "YES" : "NO");
  fprintf(mystdout, "\n");
  // debugging print -------------------------------------------------------------------------------------

  free(entries);
}

void set_branch_data_dep(Addr pc) {
  if (BRANCH_MISPREDICTION_TABLE_SIZE == 0) {
    Flag new_entry;
    Bm_Info* info = (Bm_Info*)hash_table_access_create(&inf_size_bm_table, pc, &new_entry);
    if (new_entry)
      memset(info, 0, sizeof(*info));
    info->is_data_dep = TRUE;
  }
}

void register_loop_range(Addr start, Addr end) {
  for (int i = 0; i < loop_range_count; i++) {
    if (loop_ranges[i].start == start && loop_ranges[i].end == end)
      return;
  }

  if (loop_range_count == loop_range_capacity) {
    loop_range_capacity = loop_range_capacity == 0 ? 1024 : loop_range_capacity * 2;
    loop_ranges = (Loop_Range*)realloc(loop_ranges, sizeof(Loop_Range) * loop_range_capacity);
  }
  loop_ranges[loop_range_count++] = (Loop_Range){start, end};
}

void reset_branch_misprediction_counts(void) {
  if (BRANCH_MISPREDICTION_TABLE_SIZE != 0) return;

  for (uns i = 0; i < inf_size_bm_table.buckets; i++) {
    for (Hash_Table_Entry* e = inf_size_bm_table.entries[i]; e; e = e->next) {
      Bm_Info* info = (Bm_Info*)e->data;
      info->branch_count         = 0;
      info->branch_mispred_count = 0;
      // is_data_dep, is_in_loop는 유지
    }
  }
}