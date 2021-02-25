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

#include "bp/bp.param.h"
#include "core.param.h"
#include "debug/debug.param.h"
#include "statistics.h"
}

#define LBR_CAPACITY 32
#define BTB_WARMUP 1000000
#define FANOUT 1

// My data structures
std::unordered_map<Addr,std::set<Addr>> branch_target_sets;
std::unordered_map<Addr,uint64_t> branch_pc_counts;
std::deque<std::pair<Addr,Addr>> last_32_branches;
std::unordered_map<Addr,std::unordered_map<Addr, uint64_t>> correlated_miss_counts;
uint64_t btb_lookup_count;
uint64_t btb_update_count;

bool is_single_pair_btb_entry(Addr branch_pc) {
  return branch_target_sets.count(branch_pc) && branch_target_sets[branch_pc].size() == 1;
}

void  bp_btb_pgobtb_init(Bp_Data* bp_data) {
  // btb line size set to 1
  printf("Initializing pgo btb\n");
  init_cache(&bp_data->btb, "BTB", BTB_ENTRIES, BTB_ASSOC, 1, sizeof(Addr),
             REPL_TRUE_LRU);
  branch_target_sets.clear();
  branch_pc_counts.clear();
  last_32_branches.clear();
  correlated_miss_counts.clear();
  btb_lookup_count = 0;
  btb_update_count = 0;
}

Addr* bp_btb_pgobtb_pred(Bp_Data* bp_data, Op* op) {
  Addr line_addr;
  Addr branch_pc = op->oracle_info.pred_addr;
  Addr* result = (Addr*)cache_access(&bp_data->btb, branch_pc,
                               &line_addr, TRUE);
  btb_lookup_count += 1;
  return PERFECT_BTB ?
           &op->oracle_info.target :
           result;
}

void  bp_btb_pgobtb_update(Bp_Data* bp_data, Op* op) {
  Addr  fetch_addr = op->oracle_info.pred_addr;
  Addr *btb_line, btb_line_addr, repl_line_addr;

  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  if(BTB_OFF_PATH_WRITES || !op->off_path) {
    STAT_EVENT(op->proc_id, BTB_ON_PATH_WRITE + op->off_path);
    btb_line  = (Addr*)cache_insert(&bp_data->btb, bp_data->proc_id, fetch_addr,
                                   &btb_line_addr, &repl_line_addr);
    *btb_line = op->oracle_info.target;
    // FIXME: the exceptions to this assert are really about x86 vs Alpha
    ASSERT(bp_data->proc_id, (fetch_addr == btb_line_addr) || TRUE);
  }
  Addr branch_pc = fetch_addr;
  if (btb_update_count <= BTB_WARMUP) {
    // warmup update meta data part
    // begin
    branch_target_sets[branch_pc].insert(op->oracle_info.target);
    branch_pc_counts[branch_pc]++;
    if(is_single_pair_btb_entry(branch_pc)) {
      // we will only prefetch single pair btb entries
      for(auto prev: last_32_branches) {
        if (prev.first != branch_pc && is_single_pair_btb_entry(prev.first)) {
          if(!correlated_miss_counts.count(prev.first)) {
            correlated_miss_counts[prev.first]=std::unordered_map<Addr,uint64_t>();
          }
          correlated_miss_counts[prev.first][branch_pc]+=1;
        }
      }
    }
    if(last_32_branches.size() == LBR_CAPACITY) {
      last_32_branches.pop_front();
    }
    last_32_branches.push_back(std::make_pair(branch_pc, op->oracle_info.target));
    // end
  } else {
    // Prefetching part
    // begin
    if(is_single_pair_btb_entry(branch_pc) && correlated_miss_counts.count(branch_pc)) {
      uint64_t branch_pc_execution_count = branch_pc_counts[branch_pc];
      for(auto candidate: correlated_miss_counts[branch_pc]) {
        Addr prefetched_branch_pc = candidate.first;
        uint64_t miss_count = candidate.second;
        if (is_single_pair_btb_entry(prefetched_branch_pc) && miss_count * FANOUT >= branch_pc_execution_count) {
          // we should prefetch
          Addr prefetched_target = *(branch_target_sets[prefetched_branch_pc].begin());
          Addr btb_line_addr, repl_line_addr;
          Addr *btb_line  = (Addr*)cache_insert(&bp_data->btb, bp_data->proc_id,prefetched_branch_pc,&btb_line_addr, &repl_line_addr);
          *btb_line = prefetched_target;
        }
      }
    }
    // end
  }
  btb_update_count += 1;
}