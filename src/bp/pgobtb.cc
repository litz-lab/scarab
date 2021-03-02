/* Copyright 2021 EFESLab, University of Michigan
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

#include "pgobtb.h"

#include <iostream>
#include <bits/stdc++.h>

extern "C" {
#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "isa/isa_macros.h"

#include "bp/bp.h"
#include "bp/bp_targ_mech.h"
#include "libs/cache_lib.h"
#include "sim.h"

#include "bp/bp.param.h"
#include "core.param.h"
#include "debug/debug.param.h"
#include "statistics.h"
}

// My data structures
std::unordered_map<Addr,std::set<Addr>> branch_target_sets;
std::unordered_map<Addr,uint64_t> branch_pc_counts;
std::deque<std::pair<Addr,Addr>> last_32_branches;
std::unordered_map<Addr,std::unordered_map<Addr, uint64_t>> correlated_miss_counts;
uint64_t btb_lookup_count;
uint64_t btb_update_count;
uint64_t total_prefetch_count;
uint64_t total_predecessor_count;
Cache prefetch_buffer;

bool is_single_pair_btb_entry(Addr branch_pc) {
  // TODO: make branch target sets branch target dictionaries and prefetch the most popular target
  // Also, allow multiple target branches to be a good predecessor and prefetch target.
  return branch_target_sets.count(branch_pc) && branch_target_sets[branch_pc].size() == 1;
}

void  bp_btb_pgobtb_init(Bp_Data* bp_data) {
  // btb line size set to 1
  printf("Initializing pgo btb with fanout %u\n", FANOUT);
  init_cache(&bp_data->btb, "BTB", BTB_ENTRIES, BTB_ASSOC, 1, sizeof(Addr),
             REPL_TRUE_LRU);
  init_cache(&prefetch_buffer, "BTB-prefetch-buffer", 32, 32, 1, sizeof(Addr), REPL_TRUE_LRU);
  branch_target_sets.clear();
  branch_pc_counts.clear();
  last_32_branches.clear();
  correlated_miss_counts.clear();
  btb_lookup_count = 0;
  btb_update_count = 0;
  total_prefetch_count = 0;
  total_predecessor_count = 0;
}

Addr* bp_btb_pgobtb_pred(Bp_Data* bp_data, Op* op) {
  Addr line_addr;
  Addr branch_pc = op->oracle_info.pred_addr;
  Addr* result = (Addr*)cache_access(&bp_data->btb, branch_pc,
                               &line_addr, TRUE);
  if (result == nullptr) {
    result = (Addr*)cache_access(&prefetch_buffer, branch_pc, &line_addr, TRUE);
    if (result!=nullptr) {
      Addr *btb_line, btb_line_addr, repl_line_addr;
      btb_line  = (Addr*)cache_insert(&bp_data->btb, bp_data->proc_id, branch_pc,
                                   &btb_line_addr, &repl_line_addr);
      *btb_line = *result;
      cache_invalidate(&prefetch_buffer, branch_pc, &line_addr);
    }
  }
  btb_lookup_count += 1;
  /*int miss = 0;
  if (!result) {
    miss = 1;
  } else if (*result != op->oracle_info.target) {
    miss = 2;
  }*/
  // printf("BTB-Lookup: %llu %llu %s %llu %llu %d %d\n", cycle_count, op->op_num, cf_type_names[op->table_info->cf_type], branch_pc, op->oracle_info.target, miss, op->oracle_info.target==op->oracle_info.npc);
  if (operating_mode != SIMULATION_MODE) {
    branch_pc_counts[branch_pc]++;
    if(last_32_branches.size() == LBR_CAPACITY) {
      last_32_branches.pop_front();
    }
    last_32_branches.push_back(std::make_pair(branch_pc, op->oracle_info.target));
  } else {
    // Prefetching part
    // begin
    uint64_t prefetched_count=0;
    if(is_single_pair_btb_entry(branch_pc) && correlated_miss_counts.count(branch_pc)) {
      uint64_t branch_pc_execution_count = branch_pc_counts[branch_pc];
      for(auto candidate: correlated_miss_counts[branch_pc]) {
        Addr prefetched_branch_pc = candidate.first;
        uint64_t miss_count = candidate.second;
        Addr btb_line_addr;
        uns probability = ((100.0*miss_count)/branch_pc_execution_count);
        if (is_single_pair_btb_entry(prefetched_branch_pc) && probability >= FANOUT && cache_access(&prefetch_buffer, prefetched_branch_pc, &btb_line_addr, FALSE)==NULL) {
          // we should prefetch
          // printf("Tanvir: %u %u %llu %llu %s\n", miss_count, branch_pc_execution_count, prefetched_branch_pc, branch_pc, cf_type_names[op->table_info->cf_type]);
          Addr prefetched_target = *(branch_target_sets[prefetched_branch_pc].begin());
          Addr repl_line_addr;
          Addr *btb_line  = (Addr*)cache_insert_replpos(&prefetch_buffer, bp_data->proc_id,prefetched_branch_pc,&btb_line_addr, &repl_line_addr, INSERT_REPL_DEFAULT, TRUE);
          *btb_line = prefetched_target;
          prefetched_count+=1;
        }
      }
    }
    if (prefetched_count) {
      total_predecessor_count += 1;
      total_prefetch_count += prefetched_count;
      if (!(total_predecessor_count % 10000)) {
        printf("BTB-Prefetch: %lu %lu\n",total_predecessor_count, total_prefetch_count);
      }
    }
    // end
  }
  return PERFECT_BTB ?
           &op->oracle_info.target :
           result;
}

void  bp_btb_pgobtb_update(Bp_Data* bp_data, Op* op) {
  Addr  fetch_addr = op->oracle_info.pred_addr;
  Addr *btb_line, btb_line_addr, repl_line_addr;
  /*int present = 1;
  Addr *line_if_present = (Addr *)cache_access(&bp_data->btb, fetch_addr, &btb_line_addr, FALSE);
  if(!line_if_present) {
    present = 0;
  } else if (*line_if_present != op->oracle_info.target) {
    present = 2;
  }*/

  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  if(BTB_OFF_PATH_WRITES || !op->off_path) {
    STAT_EVENT(op->proc_id, BTB_ON_PATH_WRITE + op->off_path);
    btb_line  = (Addr*)cache_insert(&bp_data->btb, bp_data->proc_id, fetch_addr,
                                   &btb_line_addr, &repl_line_addr);
    *btb_line = op->oracle_info.target;
    // printf("BTB-Update: %llu %llu %s %llu %llu %d %d\n", cycle_count, op->op_num, cf_type_names[op->table_info->cf_type], fetch_addr, op->oracle_info.target, present, op->oracle_info.target==op->oracle_info.npc);
    // FIXME: the exceptions to this assert are really about x86 vs Alpha
    ASSERT(bp_data->proc_id, (fetch_addr == btb_line_addr) || TRUE);
  }
  Addr branch_pc = fetch_addr;
  if (operating_mode != SIMULATION_MODE) {
    // warmup update meta data part
    // begin
    if (op->table_info->cf_type == CF_CBR || op->table_info->cf_type == CF_CALL || op->table_info->cf_type == CF_BR) {
      branch_target_sets[branch_pc].insert(op->oracle_info.target);
      if(is_single_pair_btb_entry(branch_pc)) {
        // we will only prefetch single pair btb entries
        std::set<Addr> has_seen;
        int tmp = last_32_branches.size() / 2;
        for(auto prev: last_32_branches) {
          if (tmp == 0)break;
          tmp--;
          if (prev.first != branch_pc && (!has_seen.count(prev.first)) && is_single_pair_btb_entry(prev.first)) {
            if(!correlated_miss_counts.count(prev.first)) {
              correlated_miss_counts[prev.first]=std::unordered_map<Addr,uint64_t>();
            }
            correlated_miss_counts[prev.first][branch_pc]+=1;
            has_seen.insert(prev.first);
          }
        }
        has_seen.clear();
      }
    }
    // end
  }
  btb_update_count += 1;
}