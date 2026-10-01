/*
 * Copyright (c) 2025 University of California, Santa Cruz
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
 * File         : topdown.h
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 05/2025
 * Description  :
 *    Implements the Top-Down performance analysis methodology based on:
 *      Yasin, A. "A Top-Down Method for Performance Analysis and Counters Architecture,"
 *      2014 IEEE International Symposium on Performance Analysis of Systems and Software.
 ***************************************************************************************/

#ifndef __TOPDOWN_H__
#define __TOPDOWN_H__

#include "globals/global_types.h"

#include "op.h"

// Reason a frontend-bound cycle left slots unfilled, used to subdivide the frontend bound.
typedef enum Topdown_Fe_Bubble_enum {
  TD_FE_OTHER = 0,           // e.g. empty FTQ / branch resteer
  TD_FE_UOPC_FRAGMENTATION,  // served from the uop cache but the line was not full
  TD_FE_UOPC_MISS,           // uop cache missed, served from the narrower icache/decode path
  TD_FE_ICACHE_MISS,         // both uop cache and icache missed (fetching from memory)
} Topdown_Fe_Bubble;

void topdown_bp_recovery(uns proc_id, Op* op);
void topdown_fetch_update(uns proc_id, Flag off_path, Flag backend_stall, int count_fetched, int on_path_fetched,
                          Topdown_Fe_Bubble fe_reason);
void topdown_exec_update(uns proc_id, uns8 fus_busy);
void topdown_done(uns proc_id);

#endif /* #ifndef __TOPDOWN_H__ */
