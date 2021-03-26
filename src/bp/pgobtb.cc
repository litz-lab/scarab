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
#include <boost/algorithm/string.hpp>

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

#define panic(...) printf(__VA_ARGS__)

// My data structures
std::unordered_map<Addr,std::unordered_map<Addr,uint64_t>> branch_target_sets;
std::unordered_map<Addr,uint64_t> branch_pc_counts;
std::deque<Addr> last_32_branches;
std::unordered_map<Addr,std::set<uint64_t>> predecessor_miss_index;
std::unordered_map<Addr,std::set<Addr>> prefetch_list;
std::unordered_map<Addr,std::unordered_map<Addr,Addr>> prefetch_footprint;
std::vector<Addr> btb_miss_list;
uint64_t btb_lookup_count;
uint64_t btb_update_count;
uint64_t total_prefetch_count;
uint64_t total_predecessor_count;
uint64_t total_prefetch_hit_count;
Cache prefetch_buffer;
bool has_simulation_started;
Addr current_function_start;
std::stack<Addr> function_call_stack;

bool is_single_pair_btb_entry(Addr branch_pc) {
  return branch_target_sets.count(branch_pc);
}

Addr get_popular_target(Addr branch_pc) {
  assert(is_single_pair_btb_entry(branch_pc) && branch_target_sets[branch_pc].size());
  bool is_first = true;
  Addr best_target = branch_pc;
  uint64_t max_count;
  for(auto kv: branch_target_sets[branch_pc]) {
    if(is_first) {
      best_target = kv.first;
      max_count = kv.second;
      is_first = false;
    } else if (max_count < kv.second) {
      best_target = kv.first;
      max_count = kv.second;
    }
  }
  return best_target;
}

void find_prefetch_candidates() {
  prefetch_list.clear();
  std::unordered_map<uint64_t,std::set<Addr>> inverse_predecessor_miss_index;
  std::unordered_map<Addr, std::unordered_map<Addr, uint64_t>> correlated_miss_counts;

  for(const auto &kv: predecessor_miss_index) {
    Addr predecessor = kv.first;
    correlated_miss_counts[predecessor] = std::unordered_map<Addr, uint64_t>();
    for(const auto &miss_index: kv.second) {
      if(!inverse_predecessor_miss_index.count(miss_index)) {
        inverse_predecessor_miss_index[miss_index]=std::set<Addr>();
      }
      inverse_predecessor_miss_index[miss_index].insert(predecessor);
      Addr missed_branch_pc = btb_miss_list[miss_index];
      if(!correlated_miss_counts[predecessor].count(missed_branch_pc)) {
        correlated_miss_counts[predecessor][missed_branch_pc] = 1;
      } else {
        correlated_miss_counts[predecessor][missed_branch_pc] += 1;
      }
    }
  }

  for(const auto &kv:inverse_predecessor_miss_index) {
    uint64_t miss_index = kv.first;
    Addr missed_branch_pc = btb_miss_list[miss_index];
    double best_probability = 0.0;
    Addr best = 0;
    for(const auto &candidate: kv.second) {
      double current_probability = ((100.0 * correlated_miss_counts[candidate][missed_branch_pc]) / branch_pc_counts[candidate]);
      if (current_probability > best_probability && (!prefetch_list.count(candidate) || prefetch_list[candidate].count(missed_branch_pc) || (prefetch_list[candidate].size() < MAX_BTB_PREFETCH_DEPTH))) {
        best_probability = current_probability;
        best = candidate;
      }
    }
    if (best_probability >= FANOUT) {
      if(!prefetch_list.count(best)) {
        prefetch_list[best] = std::set<Addr>();
      }
      prefetch_list[best].insert(missed_branch_pc);
    }
  }

  correlated_miss_counts.clear();
  inverse_predecessor_miss_index.clear();
}

bool is_good_cf(Cf_Type type) {
  if(type==CF_BR||type==CF_CBR||type==CF_CALL)return true;
  return false;
}

void update_metadata(Op* op) {
  if (false && operating_mode != SIMULATION_MODE && is_good_cf(op->table_info->cf_type)) {
    // warmup update meta data part
    // begin
    Addr branch_pc = op->oracle_info.pred_addr;
    uint64_t miss_index = btb_miss_list.size();
    btb_miss_list.push_back(branch_pc);
    if (!branch_target_sets.count(branch_pc)) {
      branch_target_sets[branch_pc]=std::unordered_map<Addr,uint64_t>();
    }
    if(!branch_target_sets[branch_pc].count(op->oracle_info.target)) {
      branch_target_sets[branch_pc][op->oracle_info.target]=1;
    } else {
      branch_target_sets[branch_pc][op->oracle_info.target]+=1;
    }
    for(auto prev: last_32_branches) {
      if (prev != branch_pc) {
        if (!predecessor_miss_index.count(prev)) {
          predecessor_miss_index[prev] = std::set<uint64_t>();
        }
        predecessor_miss_index[prev].insert(miss_index);
      }
    }
    branch_pc_counts[branch_pc]++;
    if(last_32_branches.size() == LBR_CAPACITY) {
      last_32_branches.pop_front();
    }
    last_32_branches.push_back(branch_pc);
    // end
  }
}

void perform_prefetch(Bp_Data* bp_data, Op* op) {
  Addr next_target = 0;
  if (op->table_info->cf_type == CF_RET) {
    if (!function_call_stack.empty()) {
      current_function_start = function_call_stack.top();
      function_call_stack.pop();
      next_target = current_function_start;
    }
  } else {
    next_target = op->oracle_info.target;
  }
  if(prefetch_footprint.count(op->oracle_info.pred_addr)) {
    Addr btb_line_addr;
    uint64_t prefetched_count=0;
    if (current_function_start != 0) {
      function_call_stack.push(current_function_start);
    }
    current_function_start = next_target;
    for(const auto &kv: prefetch_footprint[op->oracle_info.pred_addr]) {
      auto prefetched_branch_pc = kv.first;
      auto prefetched_target = kv.second;
      if(cache_access(&prefetch_buffer, prefetched_branch_pc, &btb_line_addr, FALSE)==NULL && cache_access(&bp_data->btb, prefetched_branch_pc, &btb_line_addr, FALSE)==NULL) {
        Addr repl_line_addr;
        Addr *btb_line  = (Addr*)cache_insert_replpos(&prefetch_buffer, bp_data->proc_id,prefetched_branch_pc,&btb_line_addr, &repl_line_addr, INSERT_REPL_DEFAULT, TRUE);
        *btb_line = prefetched_target;
        prefetched_count += 1;
      }
    }
    if (prefetched_count) {
      INC_STAT_EVENT(op->proc_id, PGO_BTB_PREFETCH_CNT, prefetched_count);
      STAT_EVENT(op->proc_id, PGO_BTB_CANDIDATE_CNT);
    }
  }
  if (false && operating_mode == SIMULATION_MODE && is_good_cf(op->table_info->cf_type)) {
    // Prefetching part
    // begin
    Addr branch_pc = op->oracle_info.pred_addr;
    if (!has_simulation_started) {
      has_simulation_started = true;
      find_prefetch_candidates();
    }
    uint64_t prefetched_count=0;
    if(prefetch_list.count(branch_pc)) {
      for(auto candidate: prefetch_list[branch_pc]) {
        Addr prefetched_branch_pc = candidate;
        Addr btb_line_addr;
        if (is_single_pair_btb_entry(prefetched_branch_pc) && cache_access(&prefetch_buffer, prefetched_branch_pc, &btb_line_addr, FALSE)==NULL && cache_access(&bp_data->btb, prefetched_branch_pc, &btb_line_addr, FALSE)==NULL) {
          // we should prefetch
          // printf("Tanvir: %u %u %llu %llu %s\n", miss_count, branch_pc_execution_count, prefetched_branch_pc, branch_pc, cf_type_names[op->table_info->cf_type]);
          Addr prefetched_target = get_popular_target(prefetched_branch_pc);// *(branch_target_sets[prefetched_branch_pc].begin());
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
        printf("BTB-Prefetch: %lu %lu %lu\n",total_predecessor_count, total_prefetch_count, total_prefetch_hit_count);
      }
    }
    // end
  }
}

void read_full_file(std::string file_path, std::vector<std::string> &data_destination)
{
  std::ifstream infile(file_path);
  std::string line;
  data_destination.clear();
  while(std::getline(infile, line)) {
    data_destination.push_back(line);
  }
  infile.close();
}

void  bp_btb_pgobtb_init(Bp_Data* bp_data) {
  printf("Initializing pgo btb with fanout %u\n", FANOUT);
  init_cache(&bp_data->btb, "BTB", BTB_ENTRIES, BTB_ASSOC, 1, sizeof(Addr),
             REPL_TRUE_LRU);
  init_cache(&prefetch_buffer, "BTB-prefetch-buffer", BTB_PREFETCH_BUFFER_SIZE, BTB_PREFETCH_BUFFER_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
  branch_target_sets.clear();
  branch_pc_counts.clear();
  last_32_branches.clear();
  predecessor_miss_index.clear();
  btb_miss_list.clear();
  prefetch_list.clear();
  btb_lookup_count = 0;
  btb_update_count = 0;
  total_prefetch_count = 0;
  total_predecessor_count = 0;
  total_prefetch_hit_count = 0;
  has_simulation_started = false;
  current_function_start = 0;
  if (FOOTPRINT) {
    std::cout<<FOOTPRINT<<std::endl;
    std::vector<std::string> all_strings;
    read_full_file(FOOTPRINT,all_strings);
    std::cout<<all_strings.size()<<std::endl;
    for(uint64_t j = 1; j < all_strings.size(); j++) {
      std::string line = all_strings[j];
      //boost::trim_if(line, boost::is_any_of("\n"));
      std::vector<std::string> parsed;
      boost::split(parsed, line, boost::is_any_of(" "),boost::token_compress_on);
      //std::cout<<parsed.size()<<' '<<j<<std::endl;
      //std::cout<<j<<' '<<line<<std::endl;
      if(parsed.size()<3) {
        panic("PGOBTB: boost parse error");
        throw "Could not parse prefetch file";
      }
      uint64_t function_start = strtoul(parsed[0].c_str(), NULL, 10);
      // uint64_t function_end = strtoul(parsed[1].c_str(), NULL, 10);
      uint64_t entry_count = strtoul(parsed[1].c_str(), NULL, 10);
      prefetch_footprint[function_start] =std::unordered_map<Addr,Addr>();
      for(int i = 0; i<entry_count; i++) {
        uint64_t pc = strtoul(parsed[2+2*i].c_str(), NULL, 10);
        uint64_t target = strtoul(parsed[2+1+2*i].c_str(), NULL, 10);
        prefetch_footprint[function_start][pc]=target;
      }
    }
    printf("Initializing prefetch footprint with size %u\n", prefetch_footprint.size());
    all_strings.clear();
  }
}

Addr* bp_btb_pgobtb_pred(Bp_Data* bp_data, Op* op) {
  Addr line_addr;
  Addr branch_pc = op->oracle_info.pred_addr;
  Addr* result = (Addr*)cache_access(&bp_data->btb, branch_pc,
                               &line_addr, TRUE);
  if (result == nullptr) {
    result = (Addr*)cache_access(&prefetch_buffer, branch_pc, &line_addr, FALSE);
    if (result!=nullptr) {
      Addr *btb_line, btb_line_addr, repl_line_addr;
      btb_line  = (Addr*)cache_insert(&bp_data->btb, bp_data->proc_id, branch_pc,&btb_line_addr,&repl_line_addr);
      *btb_line = *result;
      result = btb_line;
      STAT_EVENT(op->proc_id, PGO_BTB_PREFETCH_HIT);
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
  update_metadata(op);
  perform_prefetch(bp_data, op);
  
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
    if((Addr*)cache_access(&prefetch_buffer, fetch_addr, &btb_line_addr, FALSE) != nullptr) {
      cache_invalidate(&prefetch_buffer, fetch_addr, &btb_line_addr);
    }
    btb_line  = (Addr*)cache_insert(&bp_data->btb, bp_data->proc_id, fetch_addr,
                                   &btb_line_addr, &repl_line_addr);
    *btb_line = op->oracle_info.target;
    // printf("BTB-Update: %llu %llu %s %llu %llu %d %d\n", cycle_count, op->op_num, cf_type_names[op->table_info->cf_type], fetch_addr, op->oracle_info.target, present, op->oracle_info.target==op->oracle_info.npc);
    // FIXME: the exceptions to this assert are really about x86 vs Alpha
    ASSERT(bp_data->proc_id, (fetch_addr == btb_line_addr) || TRUE);
  }
  btb_update_count += 1;
}