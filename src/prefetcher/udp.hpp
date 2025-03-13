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
 * File         : udp.hpp
 * Author       : Surim Oh <soh31@ucsc.edu>
 * Date         : 10/31/2024
 * Description  :
 ***************************************************************************************/

#ifndef __UDP_H__
#define __UDP_H__

#include <deque>
#include <tuple>

#include "prefetcher/fdip.h"

#include "libs/bloom_filter.hpp"

using namespace std;

// bloom filters
class Bloom_Filter {
 public:
  Bloom_Filter(uns _proc_id)
      : proc_id(_proc_id),
        bloom(nullptr),
        bloom2(nullptr),
        bloom4(nullptr),
        last_prefetch_candidate(0),
        last_prefetch_candidate_counter(0),
        last_clear_cycle_count(0),
        new_prefs(0),
        cnt_unuseful(0),
        cnt_insert_bloom(0),
        cnt_insert_bloom2(0),
        cnt_insert_bloom4(0) {}
  void* lookup(Addr line_addr);
  void detect_stream(Addr line_addr);

 private:
  void bloom_insert();
  void insert1(Addr line_addr);
  void insert2(Addr line_addr);
  void insert3(Addr line_addr);
  void insert4(Addr line_addr);
  void insert_remaining(uint32_t inserted);

  uns proc_id;
  bloom_filter* bloom;
  bloom_filter* bloom2;
  bloom_filter* bloom4;
  Addr last_prefetch_candidate;
  uint32_t last_prefetch_candidate_counter;
  Counter last_clear_cycle_count;
  Counter new_prefs;
  Counter cnt_unuseful;
  Counter cnt_insert_bloom;
  Counter cnt_insert_bloom2;
  Counter cnt_insert_bloom4;
  friend class UDP;
};

class UDP {
 public:
  UDP(uns _proc_id);
  void cyc_reset();
  void clear_old_seniority_ftq();
  void set_last_bbl_start_addr(Addr addr);
  Addr get_last_cl_unuseful() { return last_cl_unuseful; }
  void set_last_cl_unuseful(Addr cl_unuseful) { last_cl_unuseful = cl_unuseful; }
  Cache* get_fdip_uc() { return fdip_uc; }
  Cache* get_fdip_uc_unuseful() { return fdip_uc_unuseful; }
  Cache* get_fdip_uc_signed() { return fdip_uc_signed; }
  void* bloom_lookup(Addr uc_line_addr);
  void bloom_inc_new_prefs() { bf->new_prefs++; }
  void bloom_inc_cnt_unuseful() { bf->cnt_unuseful++; }
  void detect_stream(Addr uc_line_addr);

  /* Seniority-FTQ */
  // <Cl address, cycle count, on/off-path>
  deque<tuple<uns64, Counter, Flag>> seniority_ftq;

 private:
  uns proc_id;

  Addr last_cl_unuseful;
  Addr last_bbl_start_addr;
  // Utility cache
  Cache* fdip_uc;
  Cache* fdip_uc_unuseful;
  Cache* fdip_uc_signed;

  Bloom_Filter* bf;
};

#endif
