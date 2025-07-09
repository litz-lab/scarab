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
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 2025
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

void lsq_init();                   // clear the lsq queue and set the max size
Flag lsq_available(Op* mem_op);    // check if there is an available LSQ entry
void lsq_dispatch(Op* mem_op);     // insert mem op into LSQ when mem op is inserted into ROB
void lsq_recover(Counter op_num);  // clear the off-path entry when there is a flushing event
void lsq_commit(Op* mem_op);       // free the entry when the mem op is retired

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __LSQ_H__ */
