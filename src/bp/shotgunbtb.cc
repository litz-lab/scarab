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

void bp_btb_shotgun_init(Bp_Data* bp_data) {
  // btb line size set to 1
  printf("Init shotgun btb\n");
  init_cache(&ubtb, "U-BTB", 4852, BTB_ASSOC, 1, sizeof(Addr),
             REPL_TRUE_LRU);
  init_cache(&cbtb, "C-BTB", 404, BTB_ASSOC, 1, sizeof(Addr),
             REPL_TRUE_LRU);
  init_cache(&rib, "RIB", 1620, BTB_ASSOC, 1, sizeof(Addr),
             REPL_TRUE_LRU);
}


/**************************************************************************************/
/* bp_btb_shotgun_pred: */

Addr* bp_btb_shotgun_pred(Bp_Data* bp_data, Op* op) {
  Cache *tmp;
  if (op->table_info->cf_type == CF_CBR) {
    tmp = &cbtb;
  } else if (op->table_info->cf_type == CF_RET) {
    tmp = &rib;
  } else {
    tmp = &ubtb;
  }
  Addr line_addr;
  Addr* result = PERFECT_BTB ? &op->oracle_info.target : (Addr*)cache_access(tmp, op->oracle_info.pred_addr,&line_addr, TRUE);
  return result;
}


/**************************************************************************************/
/* bp_btb_shotgun_update: */

void bp_btb_shotgun_update(Bp_Data* bp_data, Op* op) {
  Cache *tmp;
  if (op->table_info->cf_type == CF_CBR) {
    tmp = &cbtb;
  } else if (op->table_info->cf_type == CF_RET) {
    tmp = &rib;
  } else {
    tmp = &ubtb;
  }
  Addr  fetch_addr = op->oracle_info.pred_addr;
  Addr *btb_line, btb_line_addr, repl_line_addr;

  ASSERT(bp_data->proc_id, bp_data->proc_id == op->proc_id);
  if(BTB_OFF_PATH_WRITES || !op->off_path) {
    STAT_EVENT(op->proc_id, BTB_ON_PATH_WRITE + op->off_path);
    btb_line  = (Addr*)cache_insert(tmp, bp_data->proc_id, fetch_addr,
                                   &btb_line_addr, &repl_line_addr);
    *btb_line = op->oracle_info.target;
    ASSERT(bp_data->proc_id, (fetch_addr == btb_line_addr) || TRUE);
  }
}