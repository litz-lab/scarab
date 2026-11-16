/*
 * Copyright 2026 University of California Santa Cruz
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
 * File         : bp/ittage.h
 * Author       : Litz Lab
 * Description  : ITTAGE indirect branch target predictor (Seznec, "A 64-Kbytes
 *                ITTAGE indirect branch predictor", JWAC-2 / CBP-3, 2011).
 *
 * This header holds only the small plain-C types that have to live inside Op
 * (per-op prediction metadata and the speculative-history checkpoint used for
 * misprediction recovery), so it can be included from pred_info.h / op.h
 * without pulling in the rest of the branch predictor.
 ***************************************************************************************/

#ifndef __ITTAGE_H__
#define __ITTAGE_H__

#include "globals/global_types.h"

/* Upper bound on the number of tagged tables (ITTAGE_NUM_TABLES must be <= this). */
#define ITTAGE_MAX_TABLES 16

/* Speculative global history state, checkpointed per control-flow op.
 * head:   position of the next bit to be written in the circular history buffer
 * path:   16-bit path history (one address bit per history update)
 * fold:   per tagged table, [0] index fold, [1] tag fold, [2] second tag fold */
typedef struct Ittage_Ckpt_struct {
  uns32 head;
  uns16 path;
  uns16 fold[ITTAGE_MAX_TABLES][3];
} Ittage_Ckpt;

/* Per-op record of an ITTAGE lookup, consumed by the update at resolve time.
 * Table 0 is the tagless base table, 1..N the tagged tables. */
typedef struct Ittage_Pred_Info_struct {
  int16 provider;      // longest matching table (0 = base)
  int16 alt;           // next matching table (0 = base, -1 = none)
  Flag provider_weak;  // provider confidence was 0 at prediction time
  Flag used_alt;       // final prediction came from alt instead of provider
  Addr provider_target;
  Addr alt_target;
  Addr pred_target;  // final ITTAGE prediction (0 = no prediction)
  uns32 index[ITTAGE_MAX_TABLES + 1];
  uns16 tag[ITTAGE_MAX_TABLES + 1];
} Ittage_Pred_Info;

#endif /* #ifndef __ITTAGE_H__ */
