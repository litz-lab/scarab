/* Copyright 2020 University of Michigan (implemented by Nathan Brown)
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
 * File         : frontend/pt_fe.h
 * Author       : Nathan Brown
 * Date         : 02/24/2021
 * Description  : Interface to simulate Champsim simulator traces
 ***************************************************************************************/

#ifndef CHAMPSIM_FE_H
#define CHAMPSIM_FE_H

#include "globals/global_types.h"

/**************************************************************************************/
/* Forward Declarations */

struct Trace_Uop_struct;
typedef struct Trace_Uop_struct Trace_Uop;
struct Op_struct;

/**************************************************************************************/
/* Prototypes */

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

void champsim_init(void);

/* Implementing the frontend interface */
Addr champsim_next_fetch_addr(uns proc_id);
Flag champsim_can_fetch_op(uns proc_id);
void champsim_fetch_op(uns proc_id, Op *op);
void champsim_redirect(uns proc_id, uns64 inst_uid, Addr fetch_addr);
void champsim_recover(uns proc_id, uns64 inst_uid);
void champsim_retire(uns proc_id, uns64 inst_uid);

/* For restarting of champsim */
void champsim_done(void);
void champsim_close_trace_file(uns proc_id);
void champsim_setup(uns proc_id);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CHAMPSIM_FE_H
