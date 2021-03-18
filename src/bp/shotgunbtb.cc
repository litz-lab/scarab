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
bool is_return = false;
Addr last_unconditional_branch_pc = 0;
stack<Addr> call_stack;

void bp_btb_shotgun_init(Bp_Data* bp_data) {
  printf("Initializing shotgun btb with %d u-btb entries, %d c-btb entries, and %d rib entries. The shotgun prefetch buffer size is %d and associativity is %d.\n", 4852, 404, 1620, BTB_PREFETCH_BUFFER_SIZE, BTB_PREFETCH_BUFFER_ASSOC);
  // btb line size set to 1
  printf("Init shotgun btb\n");
  init_cache(&ubtb, "U-BTB", SHOTGUN_UBTB_SIZE, BTB_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
  init_cache(&cbtb, "C-BTB", SHOTGUN_CBTB_SIZE, BTB_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
  init_cache(&rib, "RIB", SHOTGUN_RIB_SIZE, BTB_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
  init_cache(&shotgun_prefetch_buffer, "Shotgun-prefetch-buffer", BTB_PREFETCH_BUFFER_SIZE, BTB_PREFETCH_BUFFER_ASSOC, 1, sizeof(Addr), REPL_TRUE_LRU);
}


/**************************************************************************************/
/* bp_btb_shotgun_pred: */

Addr* bp_btb_shotgun_pred(Bp_Data* bp_data, Op* op) {
  if(PERFECT_BTB)return &op->oracle_info.target;
  Addr line_addr;
  Addr branch_pc = op->oracle_info.pred_addr;
  Addr *result = (Addr*)cache_access(&ubtb, branch_pc, &line_addr, TRUE);
  if (result!=nullptr)return result;
  result = (Addr*)cache_access(&cbtb, branch_pc, &line_addr, TRUE);
  if (result!=nullptr)return result;
  result = (Addr*)cache_access(&rib, branch_pc, &line_addr, TRUE);
  if (result!=nullptr)return result;
  return nullptr;
  // First look up at ubtb, then cbtb, then rib
  /*if (op->table_info->cf_type == CF_CBR) {
    tmp = &cbtb;
    if (cache_access(tmp, branch_pc, &line_addr, TRUE)==nullptr) {
      Addr* pb_result = (Addr*)cache_access(&shotgun_prefetch_buffer, branch_pc, &line_addr, FALSE);
      if (pb_result!=nullptr) {
        Addr *btb_line, btb_line_addr, repl_line_addr;
        btb_line  = (Addr*)cache_insert(&cbtb, bp_data->proc_id, branch_pc,&btb_line_addr,&repl_line_addr);
        *btb_line = *pb_result;
        cache_invalidate(&shotgun_prefetch_buffer, branch_pc, &line_addr);
        STAT_EVENT(op->proc_id, SHOTGUN_BTB_PREFETCH_HIT);
      }
    }
  } else if (op->table_info->cf_type == CF_RET) {
    tmp = &rib;
  } else {
    tmp = &ubtb;
  }*/
  //Addr* result = PERFECT_BTB ? &op->oracle_info.target : (Addr*)cache_access(tmp, branch_pc,&line_addr, TRUE);
  //return result;
}


/**************************************************************************************/
/* bp_btb_shotgun_update: */

void bp_btb_shotgun_update(Bp_Data* bp_data, Op* op) {
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
  /*bool was_present = true;

  if (op->table_info->cf_type == CF_CALL || op->table_info->cf_type == CF_ICALL) {
    call_stack.push(fetch_addr);
  }*/

  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  if(BTB_OFF_PATH_WRITES || !op->off_path) {
    STAT_EVENT(op->proc_id, BTB_ON_PATH_WRITE + op->off_path);
    /*if((Addr*)cache_access(&shotgun_prefetch_buffer, fetch_addr, &btb_line_addr, FALSE) != nullptr) {
      cache_invalidate(&shotgun_prefetch_buffer, fetch_addr, &btb_line_addr);
    }*/
    //was_present = (cache_access(tmp, fetch_addr, &btb_line_addr, FALSE) != nullptr);
    btb_line  = (Addr*)cache_insert(tmp, bp_data->proc_id, fetch_addr,
                                   &btb_line_addr, &repl_line_addr);
    *btb_line = op->oracle_info.target;
    ASSERT(bp_data->proc_id, (fetch_addr == btb_line_addr) || TRUE);
  }

  /*if ((last_unconditional_branch_pc != 0) && (op->table_info->cf_type == CF_CBR)) {
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
        for(const auto &kv: return_footprints[last_unconditional_branch_pc][return_footprints[last_unconditional_branch_pc].size()-1]) {
          Addr p_br_pc = kv.first;
          Addr p_br_target = kv.second;
          Addr p_br_cl = p_br_pc >> 6;
          if (p_br_cl>= left && p_br_cl <=right && was_present) {
            if(cache_access(&shotgun_prefetch_buffer, p_br_pc, &btb_line_addr, FALSE)==NULL && cache_access(&cbtb, p_br_pc, &btb_line_addr, FALSE)==NULL) {
              Addr repl_line_addr;
              Addr *btb_line  = (Addr*)cache_insert_replpos(&shotgun_prefetch_buffer, bp_data->proc_id,p_br_pc,&btb_line_addr, &repl_line_addr, INSERT_REPL_DEFAULT, TRUE);
              *btb_line = p_br_target;
            }
          }
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
        for(const auto &kv: call_footprints[last_unconditional_branch_pc][call_footprints[last_unconditional_branch_pc].size()-1]) {
          Addr p_br_pc = kv.first;
          Addr p_br_target = kv.second;
          Addr p_br_cl = p_br_pc >> 6;
          if (p_br_cl>= left && p_br_cl <=right && was_present) {
            if(cache_access(&shotgun_prefetch_buffer, p_br_pc, &btb_line_addr, FALSE)==NULL && cache_access(&cbtb, p_br_pc, &btb_line_addr, FALSE)==NULL) {
              Addr repl_line_addr;
              Addr *btb_line  = (Addr*)cache_insert_replpos(&shotgun_prefetch_buffer, bp_data->proc_id,p_br_pc,&btb_line_addr, &repl_line_addr, INSERT_REPL_DEFAULT, TRUE);
              *btb_line = p_br_target;
            }
          }
        }
        call_footprints[last_unconditional_branch_pc].clear();
      }
      call_footprints[last_unconditional_branch_pc].push_back(unordered_map<Addr,Addr>());
    }
  }*/
}