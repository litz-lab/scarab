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
 * File         : mp.hpp
 * Author       : Surim Oh <soh31@ucsc.edu>
 * Date         : 11/08/2024
 * Description  :
 ***************************************************************************************/

#ifndef __MP_H__
#define __MP_H__

#include "globals/global_types.h"
#include "decoupled_frontend.h"
#include "ft_info.h"
#include "ft.hpp"

#include <deque>
#include <unordered_map>

using namespace std;

class MP_Info {
public:
  MP_Info();
private:
  uint64_t mp_dist_offpath;
  uint64_t runahead_dist_at_resteer;
  uint64_t runahead_dist_after_merge;
  uint64_t merge_point_pc;
  uint64_t overlapped_dist;
  uint64_t op_num_mp;
  uint64_t op_num_mp_old;
  uns8 merged_or_diverged; // 0: not merged, 1: merged, 2: merged and diverged again
  unordered_map<Addr, unordered_map<Addr, Counter>> per_br_mp; // per-branch PC merge point stats
};

class MP_Conf {
public:
  MP_Conf(uns _proc_id) :
    proc_id(_proc_id) {}
  void inc_low_conf_cnt() { low_confidence_cnt++; }
  void dec_low_conf_cnt() { low_confidence_cnt--; }
  Flag get_low_conf_cnt() { return low_confidence_cnt; }

private:
  uns proc_id;
  uns low_confidence_cnt;
};

class MP {
public:
  MP(uns proc_id);
  void init();
  void insert_mp_candidate(FT_Info* ft, uns64 ghist);
  void clear_old_fts();
  void update(FT* ft);
  Flag need_to_stop_prefetch();
  Addr get_hashed_line_addr(Addr line_addr, uns64 ghist);
  void search_mp_candidate(Addr line_addr);
  void insert_mp(Addr line_addr, uns64 ghist);
  Flag lookup(Addr hashed_line_addr, uns64 ghist);
  void hit_mp_candidate(Addr line_addr, uns64 ghist);
  void set_last_cl_unuseful(Addr line_addr) { last_cl_unuseful = line_addr; }
  Flag get_last_cl_unuseful() { return last_cl_unuseful; }
  MP_Conf* get_mp_conf() { return mp_conf; }

private:
  void insert_mp_to_inf_hash(Addr line_addr, uns64 ghist);
  void insert_mp_to_cache(Addr line_addr, uns64 ghist);
  Flag lookup_inf_hash(Addr hashed_line_addr, uns64 ghist);
  Flag lookup_cache(Addr hashed_line_addr, uns64 ghist);

  uns proc_id;
  // <CL address, ghist, cycle count, demand load hit>
  deque<pair<Addr, tuple<uns64, Counter, Flag>>> candidate_mps;
  Flag warmed_up;
  Addr last_cl_unuseful;
  MP_Info* mp_info;
  MP_Conf* mp_conf;

  unordered_map<Addr, Counter> found_mps;
  unordered_map<Addr, Counter> found_mps_aw;
  Cache* mp_cache;
};

#endif
