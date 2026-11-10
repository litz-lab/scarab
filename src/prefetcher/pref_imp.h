/* Copyright 2020 HPS/SAFARI Research Groups
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
 * File         : prefetcher/pref_imp.h
 * Description  : Indirect Memory Prefetcher (Yu et al., MICRO 2015), A[B[i]] only.
 ***************************************************************************************/
#ifndef __PREF_IMP_H__
#define __PREF_IMP_H__

#include "pref_common.h"

void pref_imp_init(HWP* hwp);

/* Called from the dcache stage on every demand load (hit or miss): unlike the
   line-address hooks, IMP needs the word address, the size and the loaded values. */
   void pref_imp_dl0_access(uns8 proc_id, Addr va, uns size, Addr load_PC, Flag hit, uns64 ld_val, uns64 ld_ahead_val);

#endif /* __PREF_IMP_H__ */
