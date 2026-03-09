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
 * File         : globals/debug_stage.h
 * Description  : Shared debug-print helper for pipeline stage op arrays.
 ***************************************************************************************/

#ifndef __DEBUG_STAGE_H__
#define __DEBUG_STAGE_H__

#include <stdio.h>

#include "globals/utils.h"

#include "op.h"

/* Print op_num and on/off-path flag for each op in an array.
 * NULL slots are shown as '-'. */
static inline void print_stage_op_nums(FILE* stream, Op** ops, int count) {
  fprintf(stream, " [");
  for (int i = 0; i < count; i++) {
    if (i)
      fprintf(stream, " ");
    Op* op = ops[i];
    if (!op) {
      fprintf(stream, "-");
      continue;
    }
    fprintf(stream, "%llu%s", (unsigned long long)op->op_num, op->off_path ? "o" : "n");
  }
  fprintf(stream, "]");
}

static inline const char* sd_head_opnum_str(Stage_Data* sd) {
  return (sd && sd->op_count > 0 && sd->ops[0]) ? unsstr64(sd->ops[0]->op_num) : "none";
}

static inline const char* sd_tail_opnum_str(Stage_Data* sd) {
  if (!sd || sd->op_count <= 0) {
    return "none";
  }
  for (int i = sd->max_op_count - 1; i >= 0; i--) {
    if (sd->ops[i]) {
      return unsstr64(sd->ops[i]->op_num);
    }
  }
  return "none";
}

#endif /* #ifndef __DEBUG_STAGE_H__ */
