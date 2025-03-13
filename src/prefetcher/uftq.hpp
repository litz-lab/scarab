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
 * File         : uftq.hpp
 * Author       : Surim Oh <soh31@ucsc.edu>
 * Date         : 10/31/2024
 * Description  :
 ***************************************************************************************/

#ifndef __UFTQ_H__
#define __UFTQ_H__

#include "prefetcher/fdip.h"

/* dynamic FTQ adjustment based on utility/timeliness study */
class UFTQ {
 public:
  UFTQ(uns _proc_id)
      : proc_id(_proc_id),
        useful_prefetches(0),
        unuseful_prefetches(0),
        utility_ratio(0.0),
        mshr_prefetch_hits(0),
        icache_prefetch_hits(0.0),
        timeliness_ratio(0.0),
        adjust(FALSE),
        qdaur(0),
        qdatr(0) {}
  void cyc_reset();
  void inc_useful_prefetches() { useful_prefetches++; }
  void inc_unuseful_prefetches() { unuseful_prefetches++; }
  void inc_mshr_prefetch_hits() { mshr_prefetch_hits++; }
  void inc_icache_prefetch_hits() { icache_prefetch_hits++; }
  void set_ftq_ft_num();

 private:
  uns proc_id;
  // useful prefetch counter per 100,000 cycles
  Counter useful_prefetches;
  // unuseful prefetch counter per 100,000 cycles
  Counter unuseful_prefetches;
  double utility_ratio;
  // MSHR prefetch hit counter per 100,000 cycles
  Counter mshr_prefetch_hits;
  // Icache prefetch hit counter per 100,000 cycles
  Counter icache_prefetch_hits;
  double timeliness_ratio;
  Flag adjust;
  uint64_t qdaur;
  uint64_t qdatr;
};

#endif
