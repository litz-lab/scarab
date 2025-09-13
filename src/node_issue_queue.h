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
 * File         : node_issue_queue.h
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 4/15/2025
 * Description  :
 ***************************************************************************************/

#ifndef __NODE_ISSUE_QUEUE_H__
#define __NODE_ISSUE_QUEUE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "op.h"

/**************************************************************************************/
/* Constexpr */

typedef enum NODE_ISSUE_QUEUE_DISPATCH_SCHEME_enum {
  NODE_ISSUE_QUEUE_DISPATCH_SCHEME_FIND_EMPTIEST_RS,
  NODE_ISSUE_QUEUE_DISPATCH_SCHEME_NUM
} Node_Issue_Queue_Dispatch_Scheme;

typedef enum NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_enum {
  NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_OLDEST_FIRST,
  NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_NUM
} Node_Issue_Queue_Schedule_Scheme;

const static int64 NODE_ISSUE_QUEUE_RS_SLOT_INVALID = -1;
const static int32 NODE_ISSUE_QUEUE_FU_SLOT_INVALID = -1;

/**************************************************************************************/
/* External Methods */

void node_issue_queue_update();

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __NODE_ISSUE_QUEUE_H__ */
