/*
 * Copyright 2025 University of California Santa Cruz
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
 * File         : load_value_pred.h
 * Author       : Yinyuan Zhao
 * Date         : 10/2025
 * Description  :
 ***************************************************************************************/

#ifndef __LOAD_VALUE_PRED_H__
#define __LOAD_VALUE_PRED_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "op.h"

/**************************************************************************************/
/* Constexpr */

typedef enum LOAD_VALUE_PRED_SCHEME_enum {
  LOAD_VALUE_PRED_SCHEME_NONE,
  LOAD_VALUE_PRED_SCHEME_CONST_ADDR_PRED,
  LOAD_VALUE_PRED_SCHEME_NUM
} Load_Value_Pred_Scheme;

/**************************************************************************************/
/* External Methods */

void alloc_mem_load_value_predictor(uns num_cores);
void set_load_value_predictor(uns8 proc_id);
void init_load_value_predictor(uns8 proc_id, const char* name);
void recover_load_value_predictor();

void load_value_predictor_predict_op(Op* op);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __LOAD_VALUE_PRED_H__ */
