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
 * File         : icache_stage.h
 * Author       : Heiner Litz <hlitz@ucsc.edu>
 * Date         : 03/30/2023
 * Description  :
 ***************************************************************************************/

#ifndef __DECOUPLED_FE_H__
#define __DECOUPLED_FE_H__


#ifdef __cplusplus
extern "C" {
#endif

#include <math.h>
#include <stdbool.h>

#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "bp/bp.h"
#include "bp/bp.param.h"

#include "frontend/frontend.h"
#include "frontend/pin_trace_fe.h"

#include "stage_data.h"

// DFEx_RECOVERY_POLICY param
typedef enum DFE_Recovery_Policy_enum {
  PRIMARY_DFE,
  CONTINUE_ON_RECOVERY,
  CONTINUE_ON_PREDICTION,
  CONTINUE_ON_MP,
  UCP_POLICY,
} DFE_Recovery_Policy;


  typedef struct decoupled_fe_iter decoupled_fe_iter;
  
  struct decoupled_fe_iter {
    // the ft index
    uint64_t ft_pos;
    // the op index
    uint64_t op_pos;
    // the flattened op index, as if the ftq is an 1-d array
    uint64_t flattened_op_pos;
  };

  // Simulator API
  void alloc_mem_decoupled_fe(uns numProcs, uns numBPs);
  void init_decoupled_fe(uns proc_id, uns bp_id, Bp_Data* bp_data);
  void set_decoupled_fe(uns proc_id, uns bp_id);
  void reset_decoupled_fe();
  void debug_decoupled_fe();
  void update_decoupled_fe(uns proc_id);
  // Icache/Core API
  void recover_decoupled_fe(uns proc_id, Cf_Type cf_type, Recovery_Info* info);
  bool decoupled_fe_is_off_path();
  void decoupled_fe_retire(Op *op, int proc_id, uns64 inst_uid);
  bool decoupled_fe_current_ft_can_fetch_op();
  bool decoupled_fe_fill_icache_stage_data(int requested, Stage_Data *sd);
  bool decoupled_fe_can_fetch_ft();
  FT_Info decoupled_fe_fetch_ft();
  FT_Info decoupled_fe_peek_ft();
  void decoupled_fe_search_mp_candidate(Addr line_addr);
  void decoupled_fe_insert_mp_candidate(Addr line_addr, uns64 ghist);
#ifdef __cplusplus
}
#endif


#endif
