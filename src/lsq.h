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
 * File         : lsq.h
 * Author       : Litz Lab
 * Date         : 7/2025
 * Description  :
 ***************************************************************************************/

#ifndef __LSQ_H__
#define __LSQ_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "op.h"

/**************************************************************************************/
/* External Methods */

void alloc_mem_lsq(uns num_cores);
void set_lsq(uns8 proc_id);
void init_lsq(uns8 proc_id, const char* name);
void recover_lsq();

Flag lsq_available(Mem_Type mem_type);  // check if there is an available LSQ entry
void lsq_dispatch(Op* mem_op);          // insert mem op into LSQ when mem op is inserted into ROB
void lsq_commit(Op* mem_op);            // free the entry when the mem op is retired

int lsq_get_in_flight_load_num();

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __LSQ_H__ */
