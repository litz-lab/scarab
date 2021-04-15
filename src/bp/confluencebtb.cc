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

#include "confluencebtb.h"

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

std::unordered_map<Addr,std::unordered_map<Addr,Addr>> stream;
Cache stream_buffer;

void read_stream_file(std::string file_path, std::vector<std::string> &data_destination)
{
  std::ifstream infile(file_path);
  std::string line;
  data_destination.clear();
  while(std::getline(infile, line)) {
    data_destination.push_back(line);
  }
  infile.close();
}

void stream_prefetch(Bp_Data* bp_data, Op* op) {
  uint64_t cl_address = (op->oracle_info.pred_addr) >> 6;
  if(stream.count(cl_address)) {
    Addr btb_line_addr;
    uint64_t prefetched_count=0;
    for(const auto &kv: stream[cl_address]) {
      auto prefetched_branch_pc = kv.first;
      auto prefetched_target = kv.second;
      if(cache_access(&stream_buffer, prefetched_branch_pc, &btb_line_addr, FALSE)==NULL && cache_access(&bp_data->btb, prefetched_branch_pc, &btb_line_addr, FALSE)==NULL) {
        Addr repl_line_addr;
        Addr *btb_line  = (Addr*)cache_insert_replpos(&stream_buffer, bp_data->proc_id,prefetched_branch_pc,&btb_line_addr, &repl_line_addr, INSERT_REPL_DEFAULT, TRUE);
        *btb_line = prefetched_target;
        prefetched_count += 1;
      }
    }
    if (prefetched_count) {
      INC_STAT_EVENT(op->proc_id, CONFLUENCE_BTB_PREFETCH_CNT, prefetched_count);
      STAT_EVENT(op->proc_id, CONFLUENCE_BTB_CANDIDATE_CNT);
    }
  }
}

/**************************************************************************************/
/* bp_btb_confluence_init: */

void bp_btb_confluence_init(Bp_Data* bp_data) {
  printf("Initializing confluence btb\n");
  init_cache(&bp_data->btb, "BTB", BTB_ENTRIES, BTB_ASSOC, 1, sizeof(Addr),REPL_TRUE_LRU);
  init_cache(&stream_buffer, "BTB-prefetch-buffer", BTB_PREFETCH_BUFFER_SIZE, BTB_PREFETCH_BUFFER_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
  if (FOOTPRINT) {
    std::cout<<FOOTPRINT<<std::endl;
    std::vector<std::string> all_strings;
    read_stream_file(FOOTPRINT,all_strings);
    std::cout<<all_strings.size()<<std::endl;
    for(uint64_t j = 0; j < all_strings.size(); j++) {
      std::string line = all_strings[j];
      std::vector<std::string> parsed;
      boost::split(parsed, line, boost::is_any_of(" "),boost::token_compress_on);
      if(parsed.size()<3) {
        panic("ConfluenceBTB: boost parse error");
        throw "Could not parse prefetch file";
      }
      uint64_t function_start = strtoul(parsed[0].c_str(), NULL, 10);
      uint64_t entry_count = strtoul(parsed[1].c_str(), NULL, 10);
      stream[function_start] =std::unordered_map<Addr,Addr>();
      for(int i = 0; i<entry_count; i++) {
        uint64_t pc = strtoul(parsed[2+2*i].c_str(), NULL, 10);
        uint64_t target = strtoul(parsed[2+1+2*i].c_str(), NULL, 10);
        stream[function_start].insert(make_pair(pc,target));
      }
    }
    printf("Initializing stream footprint with size %u\n", stream.size());
    all_strings.clear();
  }
}


/**************************************************************************************/
/* bp_btb_confluence_pred: */

Addr* bp_btb_confluence_pred(Bp_Data* bp_data, Op* op) {
  Addr line_addr;
  Addr branch_pc = op->oracle_info.pred_addr;
  Addr* result = (Addr*)cache_access(&bp_data->btb, branch_pc,
                               &line_addr, TRUE);
  if (result == nullptr) {
    result = (Addr*)cache_access(&stream_buffer, branch_pc, &line_addr, FALSE);
    if (result!=nullptr) {
      Addr *btb_line, btb_line_addr, repl_line_addr;
      btb_line  = (Addr*)cache_insert(&bp_data->btb, bp_data->proc_id, branch_pc,&btb_line_addr,&repl_line_addr);
      *btb_line = *result;
      result = btb_line;
      STAT_EVENT(op->proc_id, CONFLUENCE_BTB_PREFETCH_HIT);
      cache_invalidate(&stream_buffer, branch_pc, &line_addr);
    }
  }
  stream_prefetch(bp_data, op);
  
  return PERFECT_BTB ?
           &op->oracle_info.target :
           result;
}


/**************************************************************************************/
/* bp_btb_confluence_update: */

void bp_btb_confluence_update(Bp_Data* bp_data, Op* op) {
  Addr  fetch_addr = op->oracle_info.pred_addr;
  Addr *btb_line, btb_line_addr, repl_line_addr;

  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  if(BTB_OFF_PATH_WRITES || !op->off_path) {
    STAT_EVENT(op->proc_id, BTB_ON_PATH_WRITE + op->off_path);
    if((Addr*)cache_access(&stream_buffer, fetch_addr, &btb_line_addr, FALSE) != nullptr) {
      cache_invalidate(&stream_buffer, fetch_addr, &btb_line_addr);
    }
    btb_line  = (Addr*)cache_insert(&bp_data->btb, bp_data->proc_id, fetch_addr,
                                   &btb_line_addr, &repl_line_addr);
    *btb_line = op->oracle_info.target;
    // FIXME: the exceptions to this assert are really about x86 vs Alpha
    ASSERT(bp_data->proc_id, (fetch_addr == btb_line_addr) || TRUE);
  }
}