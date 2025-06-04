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

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "bp/bp.param.h"

#include "bp/bp.h"
#include "frontend/frontend.h"
#include "frontend/pin_trace_fe.h"

#include "stage_data.h"

typedef enum OFF_PATH_REASON_enum {
  REASON_NOT_IDENTIFIED,
  REASON_IBTB_MISS,
  REASON_BTB_MISS,
  // op that misses in the BTB and the BP incorrectly predicts not taken
  REASON_BTB_MISS_MISPRED,
  REASON_MISPRED,
  REASON_MISFETCH,
} Off_Path_Reason;

typedef enum CONF_OFF_PATH_REASON_enum {
  REASON_IBTB_MISS_BP_TAKEN,
  REASON_BTB_MISS_BP_TAKEN_CONF_0,
  REASON_BTB_MISS_BP_TAKEN_CONF_1,
  REASON_BTB_MISS_BP_TAKEN_CONF_2,
  REASON_BTB_MISS_BP_TAKEN_CONF_3,
  REASON_BTB_MISS_RATE,
  REASON_IBTB_MISS_RATE,
  REASON_MISFETCH_RATE,
  REASON_MISPRED_RATE,
  REASON_CONF_THRESHOLD,
  REASON_PERFECT_CONF,
  REASON_CONF_NOT_IDENTIFIED,
} Conf_Off_Path_Reason;

typedef struct FT FT;

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
void alloc_mem_decoupled_fe(uns numProcs);
void init_decoupled_fe(uns proc_id, const char*);
void set_decoupled_fe(uns proc_id);
void reset_decoupled_fe();
void debug_decoupled_fe();
void update_decoupled_fe();
// Icache/Core API
void recover_decoupled_fe();
bool decoupled_fe_is_off_path();
void decoupled_fe_retire(Op* op, int proc_id, uns64 inst_uid);

FT* decoupled_fe_get_ft(uint64_t ft_pos);
// FTQ API
decoupled_fe_iter* decoupled_fe_new_ftq_iter(uns proc_id);
/* Returns the Op at current iterator position or NULL if FTQ is empty or the end of FTQ was reached
   if end_of_ft is true the Op is the last one in a fetch target (cache-line boundary of taken branch)*/
Op* decoupled_fe_ftq_iter_get(decoupled_fe_iter* iter, bool* end_of_ft);
/* Increments iterator and returns the Op at iterator position or NULL if FTQ is empty or the end of FTQ was reached
   if end_of_ft is true the Op is the last one in a fetch target (cache-line boundary of taken branch)*/
Op* decoupled_fe_ftq_iter_get_next(decoupled_fe_iter* iter, bool* end_of_ft);
/* Returns iter flattened offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_offset(decoupled_fe_iter* iter);
/* Returns iter ft offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_ft_offset(decoupled_fe_iter* iter);
uint64_t decoupled_fe_ftq_num_ops();
uint64_t decoupled_fe_ftq_num_fts();
void decoupled_fe_set_ftq_num(uint64_t ftq_ft_num);
uint64_t decoupled_fe_get_ftq_num();
Op* decoupled_fe_get_cur_op();
uns decoupled_fe_get_conf();
Off_Path_Reason decoupled_fe_get_off_path_reason();
Conf_Off_Path_Reason decoupled_fe_get_conf_off_path_reason();
void decoupled_fe_conf_resovle_cf(Op* op);
void decoupled_fe_print_conf_data();
#ifdef __cplusplus
}
#endif

#endif
