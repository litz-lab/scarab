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
 * File         : idq_stage.h
 * Author       : Mingsheng Xu <mxu61@ucsc.edu>
 * Date         : 03/05/2025
 * Description  : Instruction Decode Queue (IDQ) bridges the front-end and the back-end.
 ***************************************************************************************/

#ifndef __IDQ_SATGE_H__
#define __IDQ_SATGE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "stage_data.h"

/**************************************************************************************/
/* Types */

typedef struct IDQ_Stage IDQ_Stage;

/**************************************************************************************/
/* External Variables */

extern IDQ_Stage* idq_stage;

/**************************************************************************************/
/* Prototypes */

void alloc_mem_idq_stage(uns8);
void set_idq_stage(uns8);
void init_idq_stage(uns8, const char*);
void reset_idq_stage(void);
void recover_idq_stage(void);
void debug_idq_stage(void);
void update_idq_stage(Stage_Data*, Stage_Data*, Stage_Data*);
Stage_Data* idq_stage_get_stage_data(void);
void idq_stage_set_recovery_cycle(int recovery_cycle);
int idq_stage_get_recovery_cycle();

#ifdef __cplusplus
}
#endif

#endif