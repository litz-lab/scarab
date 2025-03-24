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
 * File         : fdip.h
 * Author       : Surim Oh <soh31@ucsc.edu>
 * Date         : 10/31/2024
 * Description  :
 ***************************************************************************************/

#ifndef __FDIP_H__
#define __FDIP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "icache_stage.h"

void alloc_mem_fdip(uns numProcs);
void init_fdip(uns proc_id);
void update_fdip();
void recover_fdip();
void set_fdip(int _proc_id, Icache_Stage* _ic);
Flag fdip_off_path();
uns64 fdip_get_ghist();
uns64 fdip_hash_addr_ghist(uint64_t addr, uint64_t ghist);
void print_cl_info(uns proc_id);
void inc_cnt_useful(uns proc_id, Addr line_addr, Flag pref_miss);
void inc_cnt_unuseful(uns proc_id, Addr line_addr);
void inc_cnt_useful_signed(Addr line_addr);
void dec_cnt_useful_signed(Addr line_addr);
void inc_icache_miss(Addr line_addr);
void inc_icache_hit(Addr line_addr);
void inc_off_fetched_cls(Addr line_addr);
void add_evict_seq(Addr line_addr);
void evict_prefetched_cls(Addr line_addr, Flag by_fdip);
uns get_miss_reason(Addr line_addr);
uns get_last_miss_reason();
void set_last_miss_reason(uns reason);
uint64_t get_fdip_ftq_occupancy_ops(uns proc_id);
uint64_t get_fdip_ftq_occupancy(uns proc_id);
void update_useful_lines_uc(Addr line_addr);
void update_unuseful_lines_uc(Addr line_addr);
void inc_useful_lines_uc(Addr line_addr);
void dec_useful_lines_uc(Addr line_addr);
void update_useful_lines_bloom_filter(Addr line_addr);
void inc_utility_info(Flag useful);
void inc_timeliness_info(Flag mshr_hit);
Flag fdip_search_pref_candidate(Addr addr);
void assert_fdip_break_reason(Addr line_addr);
Op* fdip_get_cur_op();
Counter fdip_get_last_recover_cycle();

#ifdef __cplusplus
}
#endif

typedef enum ICACHE_MISS_REASON_enum {
  IMISS_NOT_PREFETCHED,
  IMISS_TOO_EARLY_EVICTED_BY_IFETCH,
  IMISS_TOO_EARLY_EVICTED_BY_FDIP,
  IMISS_MSHR_HIT_PREFETCHED_OFFPATH,
  IMISS_MSHR_HIT_PREFETCHED_ONPATH,
} Imiss_Reason;

typedef enum IC_FETCH_TYPE_enum {
  DEMAND_LOAD,
  FDIP_ONPATH,
  FDIP_OFFPATH,
  FDIP_BOTHPATH,
} IC_Fetch_Type;

#endif
