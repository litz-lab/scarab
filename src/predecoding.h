/* Copyright 2024 Litz Lab
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
 * File         : predecoding.h
 * Author       : Mingsheng Xu <mxu61@ucsc.edu>
 * Date         : 01/04/2025
 * Description  :
 ***************************************************************************************/

#ifndef __PREDECODING_H__
#define __PREDECODING_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "uop_cache.h"

typedef enum Predecoding_Marker_enum {
  FT_NOT_LOOKED_UP,
  FT_IN_UOP_CACHE,
  FT_NOT_IN_UOP_CACHE
} Predecoding_Marker;

void alloc_mem_predecoding(uns numProcs);
void init_predecoding(uns proc_id);
void set_predecoding(int proc_id);
void predecoding_probe_ftq();
uint64_t predecoding_get_next_ft_pos(Predecoding_Marker marker);
#ifdef __cplusplus
}
#endif

#endif
