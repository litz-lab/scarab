/* Copyright 2020 HPS/SAFARI Research Groups
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
 * File         : pref_markov.c
 * Author       : HPS Research Group
 * Date         : 05/01/2008
 * Description  :
 ***************************************************************************************/

#include "prefetcher/pref_markov.h"

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
#include "prefetcher/pref.param.h"
#include "prefetcher/pref_markov.param.h"

#include "libs/cache_lib.h"
#include "libs/hash_lib.h"
#include "libs/list_lib.h"
#include "memory/memory.h"
#include "prefetcher/l2l1pref.h"
#include "prefetcher/pref_common.h"

#include "dcache_stage.h"
#include "op.h"
#include "statistics.h"

/**************************************************************************************/
/*
 * This prefetcher is implemented in a per-core way, meaning each core has its own instance of the prefetcher.
 * The proc_id parameter indicates which core is using the prefetcher, allowing the function to maintain
 * separate contexts for each core.
 */

/* Macros */
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_PREF_MARKOV, ##args)

markov_prefetchers markov_prefetchers_array;

void pref_markov_init(HWP* hwp) {
  if (!pref_hwp_enabled(hwp))
    return;
  hwp->hwp_info->enabled = TRUE;

  // One instance per configured training level; each instance's destination
  // comes from its list entry (pref_{dcache,mlc,l1}_prefetchers).
  if (pref_hwp_instance_enabled(hwp, PREF_TRAIN_LEVEL_DCACHE)) {
    HWP_Type dcache_dest = pref_hwp_instance_dest(hwp, PREF_TRAIN_LEVEL_DCACHE);
    markov_prefetchers_array.markov_hwp_core_dcache = (Pref_Markov*)calloc(NUM_CORES, sizeof(Pref_Markov));
    for (uns i = 0; i < NUM_CORES; i++)
      markov_prefetchers_array.markov_hwp_core_dcache[i].type = dcache_dest;
    markov_prefetchers_array.last_miss_addr_core_dcache = malloc(sizeof(Addr) * NUM_CORES);
    init_markov(hwp, markov_prefetchers_array.markov_hwp_core_dcache,
                markov_prefetchers_array.last_miss_addr_core_dcache);
  }
  if (pref_hwp_instance_enabled(hwp, PREF_TRAIN_LEVEL_UMLC)) {
    HWP_Type umlc_dest = pref_hwp_instance_dest(hwp, PREF_TRAIN_LEVEL_UMLC);
    markov_prefetchers_array.markov_hwp_core_umlc = (Pref_Markov*)calloc(NUM_CORES, sizeof(Pref_Markov));
    for (uns i = 0; i < NUM_CORES; i++)
      markov_prefetchers_array.markov_hwp_core_umlc[i].type = umlc_dest;
    markov_prefetchers_array.last_miss_addr_core_umlc = malloc(sizeof(Addr) * NUM_CORES);
    init_markov(hwp, markov_prefetchers_array.markov_hwp_core_umlc, markov_prefetchers_array.last_miss_addr_core_umlc);
  }
  if (pref_hwp_instance_enabled(hwp, PREF_TRAIN_LEVEL_UL1)) {
    HWP_Type ul1_dest = pref_hwp_instance_dest(hwp, PREF_TRAIN_LEVEL_UL1);
    markov_prefetchers_array.markov_hwp_core_ul1 = (Pref_Markov*)calloc(NUM_CORES, sizeof(Pref_Markov));
    for (uns i = 0; i < NUM_CORES; i++)
      markov_prefetchers_array.markov_hwp_core_ul1[i].type = ul1_dest;
    markov_prefetchers_array.last_miss_addr_core_ul1 = malloc(sizeof(Addr) * NUM_CORES);
    init_markov(hwp, markov_prefetchers_array.markov_hwp_core_ul1, markov_prefetchers_array.last_miss_addr_core_ul1);
  }
}

void init_markov(HWP* hwp, Pref_Markov* markov_hwp_core, Addr* last_miss_addr_core) {
  uns ii, jj, proc_id;
  Pref_Markov* markov_hwp;
  for (proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    last_miss_addr_core[proc_id] = 0;
    markov_hwp = &markov_hwp_core[proc_id];
    markov_hwp->hwp_info = hwp->hwp_info;

    markov_hwp->markov_table = (Markov_Table_Entry**)calloc(PREF_MARKOV_NUM_ENTRIES, sizeof(Markov_Table_Entry*));
    for (ii = 0; ii < PREF_MARKOV_NUM_ENTRIES; ii++) {
      markov_hwp->markov_table[ii] =
          (Markov_Table_Entry*)calloc(PREF_MARKOV_NUM_NEXT_STATES, sizeof(Markov_Table_Entry));
      for (jj = 0; jj < PREF_MARKOV_NUM_NEXT_STATES; jj++) {
        markov_hwp->markov_table[ii][jj].valid = 0;
        markov_hwp->markov_table[ii][jj].next_addr = 0;
        markov_hwp->markov_table[ii][jj].tag = 0;
        markov_hwp->markov_table[ii][jj].count = 0;
      }
    }
  }
}

void pref_markov_ul1_prefhit(uns8 proc_id, Addr lineAddr, Addr load_PC, uns32 global_hist) {
  if (PREF_MARKOV_UPDATE_ON_PREF_HIT) {
    pref_markov_update_table(&markov_prefetchers_array.markov_hwp_core_ul1[proc_id],
                             markov_prefetchers_array.last_miss_addr_core_ul1, proc_id, lineAddr, 0);
  }
  if (PREF_MARKOV_SEND_ON_PREF_HIT) {
    pref_markov_send_prefetches(&markov_prefetchers_array.markov_hwp_core_ul1[proc_id], proc_id, lineAddr);
  }
}

void pref_markov_ul1_miss(uns8 proc_id, Addr lineAddr, Addr load_PC, uns32 global_hist) {
  pref_markov_update_table(&markov_prefetchers_array.markov_hwp_core_ul1[proc_id],
                           markov_prefetchers_array.last_miss_addr_core_ul1, proc_id, lineAddr, 1);
  pref_markov_send_prefetches(&markov_prefetchers_array.markov_hwp_core_ul1[proc_id], proc_id, lineAddr);
}

void pref_markov_umlc_prefhit(uns8 proc_id, Addr lineAddr, Addr load_PC, uns32 global_hist) {
  if (PREF_MARKOV_UPDATE_ON_PREF_HIT) {
    pref_markov_update_table(&markov_prefetchers_array.markov_hwp_core_umlc[proc_id],
                             markov_prefetchers_array.last_miss_addr_core_umlc, proc_id, lineAddr, 0);
  }
  if (PREF_MARKOV_SEND_ON_PREF_HIT) {
    pref_markov_send_prefetches(&markov_prefetchers_array.markov_hwp_core_umlc[proc_id], proc_id, lineAddr);
  }
}

void pref_markov_umlc_miss(uns8 proc_id, Addr lineAddr, Addr load_PC, uns32 global_hist) {
  pref_markov_update_table(&markov_prefetchers_array.markov_hwp_core_umlc[proc_id],
                           markov_prefetchers_array.last_miss_addr_core_umlc, proc_id, lineAddr, 1);
  pref_markov_send_prefetches(&markov_prefetchers_array.markov_hwp_core_umlc[proc_id], proc_id, lineAddr);
}
/* Dcache (L1D) training: the dl0 dispatcher carries no proc_id. */
void pref_markov_dl0_hit(Addr lineAddr, Addr loadPC) {
  if (PREF_MARKOV_UPDATE_ON_PREF_HIT) {
    pref_markov_update_table(&markov_prefetchers_array.markov_hwp_core_dcache[0],
                             markov_prefetchers_array.last_miss_addr_core_dcache, 0, lineAddr, 0);
  }
  if (PREF_MARKOV_SEND_ON_PREF_HIT) {
    pref_markov_send_prefetches(&markov_prefetchers_array.markov_hwp_core_dcache[0], 0, lineAddr);
  }
}

void pref_markov_dl0_miss(Addr lineAddr, Addr loadPC) {
  pref_markov_update_table(&markov_prefetchers_array.markov_hwp_core_dcache[0],
                           markov_prefetchers_array.last_miss_addr_core_dcache, 0, lineAddr, 1);
  pref_markov_send_prefetches(&markov_prefetchers_array.markov_hwp_core_dcache[0], 0, lineAddr);
}

void pref_markov_update_table(Pref_Markov* markov_hwp, Addr* last_miss_addr_core, uns8 proc_id, Addr current_addr,
                              Flag true_miss) {
  unsigned ii = 0;
  Addr last_miss_addr = last_miss_addr_core[proc_id];
  unsigned table_index = (last_miss_addr >> LOG2(L1_LINE_SIZE)) % PREF_MARKOV_NUM_ENTRIES;
  Flag new_entry = 0;
  Markov_Table_Entry temp = {0};

  if (!last_miss_addr) {
    last_miss_addr_core[proc_id] = current_addr;
    return;
  }

  for (ii = 0; ii < PREF_MARKOV_NUM_NEXT_STATES; ii++) {
    if (markov_hwp->markov_table[table_index][ii].valid) {
      if ((markov_hwp->markov_table[table_index][ii].next_addr == current_addr) &&
          (markov_hwp->markov_table[table_index][ii].tag == last_miss_addr)) {
        if (markov_hwp->markov_table[table_index][ii].count < MAX_CTR)
          markov_hwp->markov_table[table_index][ii].count++;  // only used for
                                                              // LFU, not used
                                                              // for LRU
        temp = markov_hwp->markov_table[table_index][ii];
        break;
      }
    } else {
      new_entry = 1;
      break;
    }
  }

  if (ii == PREF_MARKOV_NUM_NEXT_STATES) {
    new_entry = 1;
    ii--;
  }

  if (PREF_MARKOV_TABLE_UPDATE_POLICY == 0) {  // LRU
    for (; ii > 0; ii--)
      markov_hwp->markov_table[table_index][ii] = markov_hwp->markov_table[table_index][ii - 1];

    if (new_entry) {
      markov_hwp->markov_table[table_index][0].next_addr = current_addr;
      markov_hwp->markov_table[table_index][0].valid = 1;
      markov_hwp->markov_table[table_index][0].tag = last_miss_addr;
      markov_hwp->markov_table[table_index][0].count = 1;
    } else
      markov_hwp->markov_table[table_index][0] = temp;
  } else if (PREF_MARKOV_TABLE_UPDATE_POLICY == 1) {  // LFU
    if (new_entry) {
      markov_hwp->markov_table[table_index][ii].next_addr = current_addr;
      markov_hwp->markov_table[table_index][ii].valid = 1;
      markov_hwp->markov_table[table_index][ii].tag = last_miss_addr;
      markov_hwp->markov_table[table_index][ii].count = 1;
    } else {
      for (; ii > 0; ii--) {
        if (markov_hwp->markov_table[table_index][ii].count > markov_hwp->markov_table[table_index][ii - 1].count) {
          temp = markov_hwp->markov_table[table_index][ii];
          markov_hwp->markov_table[table_index][ii] = markov_hwp->markov_table[table_index][ii - 1];
          markov_hwp->markov_table[table_index][ii - 1] = temp;
        } else
          break;
      }
    }
  }

  if (true_miss)
    last_miss_addr_core[proc_id] = current_addr;
}

void pref_markov_send_prefetches(Pref_Markov* markov_hwp, uns8 proc_id, Addr miss_lineAddr) {
  unsigned ii = 0;
  unsigned table_index = (miss_lineAddr >> LOG2(L1_LINE_SIZE)) % PREF_MARKOV_NUM_ENTRIES;

  for (ii = 0; ii < PREF_MARKOV_NUM_NEXT_STATES; ii++) {
    if (markov_hwp->markov_table[table_index][ii].valid) {
      if ((markov_hwp->markov_table[table_index][ii].tag == miss_lineAddr) &&
          (markov_hwp->markov_table[table_index][ii].count > PREF_MARKOV_SEND_THRESHOLD)) {
        pref_addto_dest_req_queue(proc_id, markov_hwp->type,
                                  markov_hwp->markov_table[table_index][ii].next_addr >> LOG2(L1_LINE_SIZE),
                                  markov_hwp->hwp_info->id);
      }
    } else
      break;
  }
}
