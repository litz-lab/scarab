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

#include "shotgunbtb.h"

#include <iostream>
#include <bits/stdc++.h>
#include <boost/algorithm/string.hpp>

using namespace std;

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

/**************************************************************************************/
/* bp_btb_shotgun_init: */

// Shotgun data structures
Cache shotgun_prefetch_buffer;
Cache ubtb;
Cache cbtb;
Cache rib;
unordered_map<Addr,vector<unordered_map<Addr,Addr>>> call_footprints;
unordered_map<Addr,vector<unordered_map<Addr,Addr>>> return_footprints;
unordered_map<uint64_t,set<pair<Addr,Addr>>> cl_decoded_entries;
unordered_map<Addr,uint64_t> last_prefetched_cycle;
unordered_map<Addr,pair<uint64_t,bool>> last_evicted_cycle;
bool is_return = false;
Addr last_unconditional_branch_pc = 0;
stack<Addr> call_stack;

void update_evict_cycle(Addr evicted_branch_pc, bool is_cbtb = false) {
  if (evicted_branch_pc != 0) {
    last_evicted_cycle[evicted_branch_pc]=make_pair(cycle_count,is_cbtb);
  }
}

void update_prefetch_cycle(Addr prefetched_branch_pc) {
  last_prefetched_cycle[prefetched_branch_pc]=cycle_count;
}

void update_shotgun_inves_stat_after_btb_miss(Op* op) {
  if ( op == nullptr || op->table_info->cf_type != CF_CBR)return;
  Addr  branch_pc = op->oracle_info.pred_addr;
  if (!last_prefetched_cycle.count(branch_pc)) {
    STAT_EVENT(op->proc_id, SHOTGUN_CBTB_MISS_NOT_PREFETCHED);
    return;
  }
  uint64_t last_prefetch_cycle = last_prefetched_cycle[branch_pc];
  if(!last_evicted_cycle.count(branch_pc)) {
    // only for jit
    STAT_EVENT(op->proc_id, SHOTGUN_CBTB_MISS_JIT);
    return;
  }
  uint64_t last_evict_cycle = last_evicted_cycle[branch_pc].first;
  bool was_evicted_from_cbtb = last_evicted_cycle[branch_pc].second;
  if (last_evict_cycle < last_prefetch_cycle) {
    // only for jit
    STAT_EVENT(op->proc_id, SHOTGUN_CBTB_MISS_JIT);
    return;
  }
  STAT_EVENT(op->proc_id, SHOTGUN_CBTB_MISS_PREFETCHED_EVICTED);
  if (was_evicted_from_cbtb) {
    STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_FROM_CBTB);
  } else {
    STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_FROM_PREFETCH_BUFFER);
  }

  /*Update the distribution of cycle distance since last evict*/
  if (cycle_count < last_evict_cycle) {
    STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_INVALID);
  } else {
    uint64_t distance = cycle_count - last_evict_cycle;
    /*0 1 2 3 6 12 25 100 200 400 800 1600 3200 6400 12800 25600 51200 G*/
    if (distance == 0) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_E0);
    } else if ( distance == 1) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R1);
    } else if (distance == 2) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R2);
    } else if (distance == 3) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R3);
    } else if (distance < 7) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R6);
    } else if (distance < 13) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R12);
    } else if (distance < 26) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R25);
    } else if (distance < 51) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R50);
    } else if (distance < 101) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R100);
    } else if (distance < 201) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R200);
    } else if (distance < 401) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R400);
    } else if (distance < 801) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R800);
    } else if (distance < 1601) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R1600);
    } else if (distance < 3201) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R3200);
    } else if (distance < 6401) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R6400);
    } else if (distance < 12801) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R12800);
    } else if (distance < 25601) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R25600);
    } else if (distance < 51201) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_R51200);
    } else {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_SE_G);
    }
  }

  /*Update the distribution of cycle distance between last prefetch and last evict*/
  if (last_evict_cycle < last_prefetch_cycle) {
    STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_INVALID);
  } else {
    uint64_t distance = last_evict_cycle - last_prefetch_cycle;
    /*0 1 2 3 6 12 25 100 200 400 800 1600 3200 6400 12800 25600 51200 G*/
    if (distance == 0) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_E0);
    } else if ( distance == 1) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R1);
    } else if (distance == 2) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R2);
    } else if (distance == 3) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R3);
    } else if (distance < 7) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R6);
    } else if (distance < 13) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R12);
    } else if (distance < 26) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R25);
    } else if (distance < 51) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R50);
    } else if (distance < 101) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R100);
    } else if (distance < 201) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R200);
    } else if (distance < 401) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R400);
    } else if (distance < 801) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R800);
    } else if (distance < 1601) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R1600);
    } else if (distance < 3201) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R3200);
    } else if (distance < 6401) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R6400);
    } else if (distance < 12801) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R12800);
    } else if (distance < 25601) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R25600);
    } else if (distance < 51201) {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_R51200);
    } else {
      STAT_EVENT(op->proc_id, SHOTGUN_CM_PE_DPE_INVALID);
    }
  }
}

void read_file(std::string file_path, std::vector<std::string> &data_destination)
{
  std::ifstream infile(file_path);
  std::string line;
  data_destination.clear();
  while(std::getline(infile, line)) {
    data_destination.push_back(line);
  }
  infile.close();
}

void bp_btb_shotgun_init(Bp_Data* bp_data) {
  printf("Initializing shotgun btb with %d u-btb entries, %d c-btb entries, and %d rib entries. The shotgun prefetch buffer size is %d and associativity is %d.\n", SHOTGUN_UBTB_SIZE, SHOTGUN_CBTB_SIZE, SHOTGUN_RIB_SIZE, BTB_PREFETCH_BUFFER_SIZE, BTB_PREFETCH_BUFFER_ASSOC);
  // btb line size set to 1
  printf("Init shotgun btb\n");
  init_cache(&ubtb, "U-BTB", SHOTGUN_UBTB_SIZE, BTB_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
  init_cache(&cbtb, "C-BTB", SHOTGUN_CBTB_SIZE, BTB_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
  init_cache(&rib, "RIB", SHOTGUN_RIB_SIZE, BTB_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
  init_cache(&shotgun_prefetch_buffer, "Shotgun-prefetch-buffer", BTB_PREFETCH_BUFFER_SIZE, BTB_PREFETCH_BUFFER_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
  if (FOOTPRINT) {
    std::cout<<FOOTPRINT<<std::endl;
    std::vector<std::string> all_strings;
    read_file(FOOTPRINT,all_strings);
    std::cout<<all_strings.size()<<std::endl;
    for(uint64_t j = 0; j < all_strings.size(); j++) {
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
      cl_decoded_entries[function_start] =set<pair<Addr,Addr>>();
      for(int i = 0; i<entry_count; i++) {
        uint64_t pc = strtoul(parsed[2+2*i].c_str(), NULL, 10);
        uint64_t target = strtoul(parsed[2+1+2*i].c_str(), NULL, 10);
        cl_decoded_entries[function_start].insert(make_pair(pc,target));
      }
    }
    printf("Initializing prefetch footprint with size %u\n", cl_decoded_entries.size());
    all_strings.clear();
  }
}

void perform_prefetch_update_metadata(Bp_Data* bp_data, Op* op) {
  Addr  fetch_addr = op->oracle_info.pred_addr;
  Addr *btb_line, btb_line_addr, repl_line_addr;
  Cache *tmp;
  if (op->table_info->cf_type == CF_CBR) {
    tmp = &cbtb;
  } else if (op->table_info->cf_type == CF_RET) {
    tmp = &rib;
  } else {
    tmp = &ubtb;
  }

  bool was_present = true;

  if (op->table_info->cf_type == CF_CALL || op->table_info->cf_type == CF_ICALL) {
    call_stack.push(fetch_addr);
  }

  was_present = (cache_access(tmp, fetch_addr, &btb_line_addr, FALSE) != nullptr);


  if ((last_unconditional_branch_pc != 0) && (op->table_info->cf_type == CF_CBR)) {
    // continue adding to the current recording and current call/return
    if (is_return) {
      return_footprints[last_unconditional_branch_pc][return_footprints[last_unconditional_branch_pc].size()-1][fetch_addr]=op->oracle_info.target;
    } else {
      call_footprints[last_unconditional_branch_pc][call_footprints[last_unconditional_branch_pc].size()-1][fetch_addr]=op->oracle_info.target;
    }
  }
  if (op->table_info->cf_type != CF_CBR) {
    // either start a new recording or switch from call to return
    if (op->table_info->cf_type == CF_RET && call_stack.size()) {
      is_return = true;
      last_unconditional_branch_pc = call_stack.top();
      call_stack.pop();
      was_present = (cache_access(&ubtb, last_unconditional_branch_pc, &btb_line_addr, FALSE) != nullptr);
      if (!return_footprints.count(last_unconditional_branch_pc)) {
        return_footprints[last_unconditional_branch_pc]=vector<unordered_map<Addr,Addr>>();
      } else if (return_footprints[last_unconditional_branch_pc][return_footprints[last_unconditional_branch_pc].size()-1].size() > 0) {
        // prefetch all previous entries that are within [-2,5] cache line distance from this branch pc's cache line
        Addr cl = last_unconditional_branch_pc;
        cl = cl >> 6;
        Addr left = cl-2;
        Addr right = cl+6;
        set<uint64_t> all_cache_lines;
        for(const auto &kv: return_footprints[last_unconditional_branch_pc][return_footprints[last_unconditional_branch_pc].size()-1]) {
          Addr p_br_pc = kv.first;
          Addr p_br_target = kv.second;
          Addr p_br_cl = p_br_pc >> 6;
          if (p_br_cl>= left && p_br_cl <=right && was_present) {
            if (SHOTGUN_CONFLUENCE_ENABLE) {  
              if(cache_access(&shotgun_prefetch_buffer, p_br_pc, &btb_line_addr, FALSE)==NULL && cache_access(&cbtb, p_br_pc, &btb_line_addr, FALSE)==NULL) {
                Addr repl_line_addr;
                Addr *btb_line  = (Addr*)cache_insert_replpos(&shotgun_prefetch_buffer, bp_data->proc_id,p_br_pc,&btb_line_addr, &repl_line_addr, INSERT_REPL_DEFAULT, TRUE);
                STAT_EVENT(op->proc_id, SHOTGUN_BTB_PREFETCH_CNT);
                update_prefetch_cycle(p_br_pc);
                update_evict_cycle(repl_line_addr);
                *btb_line = p_br_target;
              }
            } else {
              all_cache_lines.insert(p_br_cl);
            }
          }
        }
        if (!SHOTGUN_CONFLUENCE_ENABLE) {
          for(const auto &cl: all_cache_lines) {
            if (cl_decoded_entries.count(cl)) {
              for(const auto &kv: cl_decoded_entries[cl]) {
                if(cache_access(&shotgun_prefetch_buffer, kv.first, &btb_line_addr, FALSE)==NULL && cache_access(&cbtb, kv.first, &btb_line_addr, FALSE)==NULL) {
                  Addr repl_line_addr;
                  Addr *btb_line  = (Addr*)cache_insert_replpos(&shotgun_prefetch_buffer, bp_data->proc_id,kv.first,&btb_line_addr, &repl_line_addr, INSERT_REPL_DEFAULT, TRUE);
                  STAT_EVENT(op->proc_id, SHOTGUN_BTB_PREFETCH_CNT);
                  update_prefetch_cycle(kv.first);
                  update_evict_cycle(repl_line_addr);
                  *btb_line = kv.second;
                }
              }
            }
          }
          all_cache_lines.clear();
        }
        return_footprints[last_unconditional_branch_pc].clear();
      }
      return_footprints[last_unconditional_branch_pc].push_back(unordered_map<Addr,Addr>());
    } else if ((op->table_info->cf_type == CF_CALL) || (op->table_info->cf_type == CF_BR)) {
      is_return = false;
      last_unconditional_branch_pc = fetch_addr;
      if (!call_footprints.count(last_unconditional_branch_pc)) {
        call_footprints[last_unconditional_branch_pc]=vector<unordered_map<Addr,Addr>>();
      } else if (call_footprints[last_unconditional_branch_pc][call_footprints[last_unconditional_branch_pc].size()-1].size() > 0) {
        // prefetch all previous entries that are within [-2,5] cache line distance from this branch target's cache line
        Addr cl = op->oracle_info.target;
        cl = cl >> 6;
        Addr left = cl-2;
        Addr right = cl+6;
        set<uint64_t> all_cache_lines;
        for(const auto &kv: call_footprints[last_unconditional_branch_pc][call_footprints[last_unconditional_branch_pc].size()-1]) {
          Addr p_br_pc = kv.first;
          Addr p_br_target = kv.second;
          Addr p_br_cl = p_br_pc >> 6;
          if (p_br_cl>= left && p_br_cl <=right && was_present) {
            if (SHOTGUN_CONFLUENCE_ENABLE) {
              if(cache_access(&shotgun_prefetch_buffer, p_br_pc, &btb_line_addr, FALSE)==NULL && cache_access(&cbtb, p_br_pc, &btb_line_addr, FALSE)==NULL) {
                Addr repl_line_addr;
                Addr *btb_line  = (Addr*)cache_insert_replpos(&shotgun_prefetch_buffer, bp_data->proc_id,p_br_pc,&btb_line_addr, &repl_line_addr, INSERT_REPL_DEFAULT, TRUE);
                STAT_EVENT(op->proc_id, SHOTGUN_BTB_PREFETCH_CNT);
                update_prefetch_cycle(p_br_pc);
                update_evict_cycle(repl_line_addr);
                *btb_line = p_br_target;
              }
            } else {
              all_cache_lines.insert(p_br_cl);
            }
          }
        }
        if (!SHOTGUN_CONFLUENCE_ENABLE) {
          for(const auto &cl: all_cache_lines) {
            if (cl_decoded_entries.count(cl)) {
              for(const auto &kv: cl_decoded_entries[cl]) {
                if(cache_access(&shotgun_prefetch_buffer, kv.first, &btb_line_addr, FALSE)==NULL && cache_access(&cbtb, kv.first, &btb_line_addr, FALSE)==NULL) {
                  Addr repl_line_addr;
                  Addr *btb_line  = (Addr*)cache_insert_replpos(&shotgun_prefetch_buffer, bp_data->proc_id,kv.first,&btb_line_addr, &repl_line_addr, INSERT_REPL_DEFAULT, TRUE);
                  STAT_EVENT(op->proc_id, SHOTGUN_BTB_PREFETCH_CNT);
                  update_prefetch_cycle(kv.first);
                  update_evict_cycle(repl_line_addr);
                  *btb_line = kv.second;
                }
              }
            }
          }
          all_cache_lines.clear();
        }
        call_footprints[last_unconditional_branch_pc].clear();
      }
      call_footprints[last_unconditional_branch_pc].push_back(unordered_map<Addr,Addr>());
    }
  }
}


/**************************************************************************************/
/* bp_btb_shotgun_pred: */

Addr* bp_btb_shotgun_pred(Bp_Data* bp_data, Op* op) {
  if(PERFECT_BTB)return &op->oracle_info.target;
  // First look up at ubtb, then cbtb, then rib, at the end prefetch_buffer
  Addr line_addr;
  Addr branch_pc = op->oracle_info.pred_addr;
  Addr *result = (Addr*)cache_access(&ubtb, branch_pc, &line_addr, TRUE);
  if (result==nullptr)result = (Addr*)cache_access(&cbtb, branch_pc, &line_addr, TRUE);
  if (result==nullptr)result = (Addr*)cache_access(&rib, branch_pc, &line_addr, TRUE);
  if (result==nullptr) {
    Addr* pb_result = (Addr*)cache_access(&shotgun_prefetch_buffer, branch_pc, &line_addr, FALSE);
    if (pb_result!=nullptr) {
      Addr *btb_line, btb_line_addr;
      Addr repl_line_addr;
      btb_line  = (Addr*)cache_insert(&cbtb, bp_data->proc_id, branch_pc,&btb_line_addr,&repl_line_addr);
      *btb_line = *pb_result;
      update_evict_cycle(repl_line_addr,true);
      cache_invalidate(&shotgun_prefetch_buffer, branch_pc, &line_addr);
      STAT_EVENT(op->proc_id, SHOTGUN_BTB_PREFETCH_HIT);
      result=btb_line;
    }
  }
  if (result == nullptr) {
    update_shotgun_inves_stat_after_btb_miss(op);
  }
  perform_prefetch_update_metadata(bp_data, op);
  return result;
}


/**************************************************************************************/
/* bp_btb_shotgun_update: */

void bp_btb_shotgun_update(Bp_Data* bp_data, Op* op) {
  Addr  fetch_addr = op->oracle_info.pred_addr;
  Addr *btb_line, btb_line_addr;
  Addr repl_line_addr;
  Cache *tmp;
  if (op->table_info->cf_type == CF_CBR) {
    tmp = &cbtb;
    if (!FOOTPRINT) {
      uint64_t cl_address = fetch_addr >> 6;
      if(!cl_decoded_entries.count(cl_address)) {
        cl_decoded_entries[cl_address] = set<pair<Addr,Addr>>();
      }
      cl_decoded_entries[cl_address].insert(make_pair(fetch_addr, op->oracle_info.target));
    }
  } else if (op->table_info->cf_type == CF_RET) {
    tmp = &rib;
  } else {
    tmp = &ubtb;
  }

  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  if(BTB_OFF_PATH_WRITES || !op->off_path) {
    STAT_EVENT(op->proc_id, BTB_ON_PATH_WRITE + op->off_path);
    if((Addr*)cache_access(&shotgun_prefetch_buffer, fetch_addr, &btb_line_addr, FALSE) != nullptr) {
      cache_invalidate(&shotgun_prefetch_buffer, fetch_addr, &btb_line_addr);
    }
    btb_line  = (Addr*)cache_insert(tmp, bp_data->proc_id, fetch_addr,
                                   &btb_line_addr, &repl_line_addr);
    if (tmp == &cbtb) {
      update_evict_cycle(repl_line_addr, true);
    }
    *btb_line = op->oracle_info.target;
    ASSERT(bp_data->proc_id, (fetch_addr == btb_line_addr) || TRUE);
  }
}