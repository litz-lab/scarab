/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
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
 * File         : memory/mem_warmup.h
 * Description  : functional warmup (--warmup) of the cache hierarchy
 ***************************************************************************************/

#ifndef __MEM_WARMUP_H__
#define __MEM_WARMUP_H__

#include "globals/global_types.h"

/* An instruction fetch of ia; TRUE if it hit in the icache. Sets line_addr to the icache line. */
Flag mem_warmup_icache(uns proc_id, Addr ia, Addr* line_addr);

/* A load or store to va. */
void mem_warmup_dcache(uns proc_id, Addr va, Flag is_store);

/* Called once warmup ends: in an exclusive hierarchy, asserts no line was left at two levels. */
void mem_warmup_check(void);

#endif /* #ifndef __MEM_WARMUP_H__ */
