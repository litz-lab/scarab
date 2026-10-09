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
 * File         : memory/mshr.h
 * Author       : Litz Lab
 * Date         : 10/2026
 * Description  : MSHR file: one entry per line in flight, each with the requests
 *                waiting on that line. Shared by every level, and by the writeback
 *                files.
 ***************************************************************************************/

#ifndef __MSHR_H__
#define __MSHR_H__

#include "globals/global_types.h"

#include "memory/mem_req.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Mshr_Line_struct Mshr_Line;

typedef struct Mshr_struct {
  struct Mshr_Impl_struct* impl;
  /* Lines whose data has arrived and is waiting to fill this level (mshr_line_ready).
     Only when it is non-zero does anything walk the file for them. */
  uns pending_fills;
  uns size; /* entries: lines in flight */
} Mshr;

void mshr_init(Mshr* mshr, const char* name, uns size);
void mshr_reset(Mshr* mshr);
void mshr_destroy(Mshr* mshr);
const char* mshr_name(Mshr* mshr);

uns mshr_count(Mshr* mshr);            /* entries in use */
Flag mshr_full(Mshr* mshr, Addr addr); /* addr has no entry and none is free */
Flag mshr_holds(Mshr* mshr, Mem_Req* req);
Mshr_Line* mshr_line(Mshr* mshr, Addr addr); /* the line's entry, or NULL */
void mshr_add(Mshr* mshr, Mem_Req* req);     /* join the line's entry, or allocate one */
void mshr_remove(Mshr* mshr, Mem_Req* req);  /* the entry goes with its last request */

/* Entries in allocation order. Read next before anything that can free the current. */
Mshr_Line* mshr_first(Mshr* mshr);
Mshr_Line* mshr_next(Mshr* mshr, Mshr_Line* line);
Counter mshr_line_rdy(Mshr_Line* line); /* when its first request is looked up */
void mshr_line_set_rdy(Mshr_Line* line, Counter rdy);
/* The line's data has arrived: fill this level at fill_rdy. The line stays ready until
   its last request leaves. */
void mshr_line_ready(Mshr* mshr, Mshr_Line* line, Counter fill_rdy);
Counter mshr_line_fill_rdy(Mshr_Line* line); /* 0 until the data arrives */
Flag mshr_line_filled(Mshr_Line* line);      /* this level's cache already took it */
void mshr_line_set_filled(Mshr_Line* line);
/* The request that looks the line up and goes down for it: the first to arrive. */
Mem_Req* mshr_line_first(Mshr_Line* line);
/* The line's requests in arrival order, copied into reqs (mshr_line_count of them). */
uns mshr_line_count(Mshr_Line* line);
uns mshr_line_reqs(Mshr_Line* line, Mem_Req** reqs);

/* A request on addr's line that a new request of this type can fold into, or NULL. */
Mem_Req* mshr_search(Mshr* mshr, uns8 proc_id, Addr addr, Mem_Req_Type type, uns size, Flag* demand_hit_prefetch);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __MSHR_H__ */
