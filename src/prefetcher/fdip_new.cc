#include "decoupled_frontend.h"
#include "prefetcher/fdip_new.h"
#include "libs/bloom_filter.hpp"
#include "sim.h"

extern "C" {
#include "op.h"
#include "prefetcher/pref.param.h"
#include "memory/memory.h"
#include "memory/memory.param.h"
}

#include <iostream>
#include <unordered_map>
#include <map>
#include <algorithm>
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_FDIP, ##args)

decoupled_fe_iter* iter;
const int MAX_FTQ_ENTRY_CYC = 2;
int fdip_proc_id;
Icache_Stage *ic_ref;
std::vector<Op*> per_core_cur_op;
std::vector<decoupled_fe_iter*> per_core_ftq_iter;
std::vector<Addr> per_core_last_line_addr;

typedef enum FDIP_BREAK_enum {
  BR_REACH_FTQ_END,
  BR_FTQ_EMPTY,
  BR_MAX_FTQ_ENTRY_CYC,
  BR_FULL_MEM_REQ_BUF,
} FDIP_Break;

typedef enum UTILITY_LEARN_POLICY_enum {
  LEARN_ONLY_ONPATH_USEFUL,
  LEARN_ON_OFF_USEFUL,
  LEARN_ONLY_UNUSEFUL,
  LEARN_ON_OFF_USEFUL_UNUSEFUL_2BIT,
  LEARN_ON_OFF_USEFUL_UNUSEFUL_3BIT,
} Utility_Learn_Policy;

/* global variables for dynamic FTQ adjustment based on utility/timeliness study */
std::vector<Utility_Timeliness_Info> per_core_utility_timeliness_info;

/* global variables for utility study and stats */
// for icache miss stats
std::vector<uns> per_core_last_imiss_reason;
std::vector<Counter> per_core_last_recover_cycle;
// <CL address, # of first demand load on-path hits of cache lines, # of first demand load off-path hits of cache lines> - useful count
std::vector<std::unordered_map<Addr, std::pair<Counter, Counter>>> per_core_cnt_useful;
// <CL address, # of evictions w/o hit of cache lines> - unuseful count
std::vector<std::unordered_map<Addr, std::pair<Counter, Counter>>> per_core_cnt_unuseful;
// <CL address, 2-bit counter for on/off-path useful/unuseful> init by 01 (1), not prefetch : 00, 01, prefetch : 10, 11
std::vector<std::unordered_map<Addr, Counter>> per_core_useful_unuseful_2bit;
// <CL address, 3-bit counter for on/off-path useful/unuseful> init by 011 (3) not prefetch : 000, 001, 010, 011, prefetch : 100, 101, 110, 111
std::vector<std::unordered_map<Addr, Counter>> per_core_useful_unuseful_3bit;
// <CL addresses, retirement count> - on-path retired cache line count
std::vector<std::unordered_map<Addr, Counter>> per_core_cnt_useful_ret;
// <CL addresses, icache miss count>
std::vector<std::map<Addr, Counter>> per_core_icache_miss;
// <CL addresses, prefetched count>
std::vector<std::map<Addr, Counter>> per_core_prefetched_cls;
// <CL address, cyc_access_by_fdip, cyc_evicted_from_l1> - prefetched and access time information for timeliness analysis
std::vector<std::unordered_map<Addr, std::pair<Counter, Counter>>> per_core_prefetched_cls_info;
// accumulated FTQ occupancy every cycle
std::vector<uint64_t> per_core_fdip_ftq_occupancy_ops;
std::vector<uint64_t> per_core_fdip_ftq_occupancy_blocks;
std::vector<Addr> per_core_last_cl_unuseful;
std::vector<Addr> per_core_last_bbl_start_addr;
// Utility cache
std::vector<Cache> per_core_fdip_uc;
// bloom filters
typedef struct Bloom_Filter_struct {
  bloom_filter *bloom;
  bloom_filter *bloom2;
  bloom_filter *bloom4;
  Addr last_prefetch_candidate;
  uint32_t last_prefetch_candidate_counter;
} Bloom_Filter;
std::vector<Bloom_Filter> per_core_bloom_filter;

void alloc_mem_fdip(uns numCores) {
  per_core_cur_op.resize(numCores);
  per_core_ftq_iter.resize(numCores);
  per_core_last_line_addr.resize(numCores);
  if (FDIP_ADJUSTABLE_FTQ)
    per_core_utility_timeliness_info.resize(numCores);
  per_core_last_imiss_reason.resize(numCores);
  per_core_last_recover_cycle.resize(numCores);
  per_core_cnt_useful.resize(numCores);
  per_core_cnt_unuseful.resize(numCores);
  per_core_useful_unuseful_2bit.resize(numCores);
  per_core_useful_unuseful_3bit.resize(numCores);
  per_core_cnt_useful_ret.resize(numCores);
  per_core_icache_miss.resize(numCores);
  per_core_prefetched_cls.resize(numCores);
  per_core_prefetched_cls_info.resize(numCores);
  per_core_fdip_ftq_occupancy_ops.resize(numCores);
  per_core_fdip_ftq_occupancy_blocks.resize(numCores);
  if (FDIP_UTILITY_HASH_ENABLE)
    ASSERT(fdip_proc_id, FDIP_UTILITY_LEARN_POLICY >= Utility_Learn_Policy::LEARN_ONLY_ONPATH_USEFUL &&
        FDIP_UTILITY_LEARN_POLICY <= Utility_Learn_Policy::LEARN_ON_OFF_USEFUL_UNUSEFUL_3BIT);

  if (FDIP_UTILITY_HASH_ENABLE || FDIP_UC_SIZE || FDIP_BLOOM_FILTER) {
    per_core_last_cl_unuseful.resize(numCores);
    per_core_last_bbl_start_addr.resize(numCores);
  }
  if (FDIP_UC_SIZE)
    per_core_fdip_uc.resize(numCores);
  if (FDIP_BLOOM_FILTER)
    per_core_bloom_filter.resize(numCores);
}

void init_fdip(uns proc_id) {
  per_core_ftq_iter[proc_id] = decoupled_fe_new_ftq_iter();
  per_core_last_line_addr[proc_id] = 0;
  if (FDIP_ADJUSTABLE_FTQ)
    per_core_utility_timeliness_info[proc_id] = {0, 0, 0.0, 0, 0, 0.0, FALSE};
  per_core_last_imiss_reason[proc_id] = Imiss_Reason::IMISS_NOT_PREFETCHED;
  per_core_last_recover_cycle[proc_id] = 0;
  per_core_fdip_ftq_occupancy_ops[proc_id] = 0;
  per_core_fdip_ftq_occupancy_blocks[proc_id] = 0;
  if (FDIP_UTILITY_HASH_ENABLE || FDIP_UC_SIZE || FDIP_BLOOM_FILTER) {
    per_core_last_cl_unuseful[proc_id] = 0;
    per_core_last_bbl_start_addr[proc_id] = 0;
  }
  if (FDIP_UC_SIZE) {
    ASSERT(fdip_proc_id, !FDIP_BLOOM_FILTER);
    init_cache(&per_core_fdip_uc[proc_id], "FDIP_USEFULNESS_CACHE", FDIP_UC_SIZE, FDIP_UC_ASSOC, ICACHE_LINE_SIZE,
               0, REPL_TRUE_LRU); //Data size = 2 byte
    per_core_fdip_uc[proc_id].tag_incl_offset = TRUE;
  }
  if (FDIP_BLOOM_FILTER) {
    ASSERT(fdip_proc_id, !FDIP_UC_SIZE && !FDIP_UTILITY_HASH_ENABLE);
    bloom_parameters bloom1_parameters;
    bloom1_parameters.projected_element_count = FDIP_BLOOM_ENTRIES;
    bloom1_parameters.false_positive_probability = 0.005;
    bloom1_parameters.compute_optimal_parameters();
    per_core_bloom_filter[proc_id].bloom = new bloom_filter(bloom1_parameters);

    bloom_parameters bloom2_parameters;
    bloom2_parameters.projected_element_count = FDIP_BLOOM2_ENTRIES;
    bloom2_parameters.false_positive_probability = 0.005;
    bloom2_parameters.compute_optimal_parameters();
    per_core_bloom_filter[proc_id].bloom2 = new bloom_filter(bloom2_parameters);

    bloom_parameters bloom4_parameters;
    bloom4_parameters.projected_element_count = FDIP_BLOOM4_ENTRIES;
    bloom4_parameters.false_positive_probability = 0.005;
    bloom4_parameters.compute_optimal_parameters();
    per_core_bloom_filter[proc_id].bloom4 = new bloom_filter(bloom4_parameters);

    per_core_bloom_filter[proc_id].last_prefetch_candidate_counter = 0;
  }
}

void set_fdip(int _proc_id, Icache_Stage *_ic) {
  fdip_proc_id = _proc_id;
  ic_ref = _ic;
  iter = per_core_ftq_iter[_proc_id];
}

void update_fdip() {
  if (!FDIP_ENABLE)
    return;

  int ftq_entry_per_cycle = 0;
  FDIP_Break break_reason = BR_REACH_FTQ_END;

  while(true) {
    bool end_of_block;
    Op *op = decoupled_fe_ftq_iter_get(iter, &end_of_block);
    Addr last_line_addr = per_core_last_line_addr[fdip_proc_id];
    Flag emit_new_prefetch = FALSE;
    if (!op) {
      if (!decoupled_fe_ftq_iter_offset(iter)) {
        DEBUG(fdip_proc_id, "Break due to FTQ Empty\n");
        break_reason = BR_FTQ_EMPTY;
      } else {
        break_reason = BR_REACH_FTQ_END;
        DEBUG(fdip_proc_id, "Break due to reaching FTQ end\n");
      }
      break;
    }
    if (ftq_entry_per_cycle == MAX_FTQ_ENTRY_CYC) {
      DEBUG(fdip_proc_id, "Break due to max FTQ entries per cycle\n");
      break_reason = BR_MAX_FTQ_ENTRY_CYC;
      break;
    }
    if ((FDIP_UTILITY_HASH_ENABLE || FDIP_UC_SIZE || FDIP_BLOOM_FILTER) && !per_core_last_bbl_start_addr[fdip_proc_id]) {
      per_core_last_bbl_start_addr[fdip_proc_id] = op->inst_info->addr;
      DEBUG(fdip_proc_id, "init last_bbl_start_addr: %llx\n", per_core_last_bbl_start_addr[fdip_proc_id]);
    }

    uint64_t pc_addr = op->inst_info->addr;
    Addr line_addr = op->inst_info->addr & ~0x3F;
    DEBUG(fdip_proc_id, "op_num: %llu, op->inst_info->addr: %llx, line_addr: %llx, last_line_addr: %llx\n", op->op_num, op->inst_info->addr, line_addr, last_line_addr);
    if (line_addr != last_line_addr) {
      STAT_EVENT(ic_ref->proc_id, FDIP_ATTEMPTED_PREF_ONPATH + op->off_path);
      Flag demand_hit_prefetch = FALSE;
      Flag demand_hit_writeback = FALSE;
      Mem_Queue_Entry* queue_entry = NULL;
      Flag ramulator_match = FALSE;
      bool line = (Inst_Info**)cache_access(&ic_ref->icache, pc_addr, &line_addr, TRUE);
      // icache_line_info cache should be accessed same times with icache for a consistant line information
      if (WP_COLLECT_STATS) {
        Addr dummy_addr = 0;
        bool line_info = (Icache_Data*)cache_access(&ic_ref->icache_line_info, pc_addr, &dummy_addr, TRUE);
        UNUSED(dummy_addr);
        UNUSED(line_info);
      }
      Mem_Req* mem_req = mem_search_reqbuf_wrapper(ic_ref->proc_id, line_addr,
                                                   MRT_FDIPPRF, ICACHE_LINE_SIZE, &demand_hit_prefetch, &demand_hit_writeback,
                                                   QUEUE_MLC | QUEUE_L1 | QUEUE_BUS_OUT |
                                                   QUEUE_MEM | QUEUE_L1FILL | QUEUE_MLC_FILL,
                                                   &queue_entry, &ramulator_match);
      if (FDIP_UTILITY_HASH_ENABLE || FDIP_UC_SIZE || FDIP_BLOOM_FILTER)
        emit_new_prefetch = determine_usefulness(line_addr);
      else
        emit_new_prefetch = TRUE;

      if (line) {
        probe_prefetched_cls(line_addr);
        STAT_EVENT(ic_ref->proc_id, FDIP_PREF_ICACHE_PROBE_HIT_ONPATH + op->off_path);
      }

      if (emit_new_prefetch && !line && !mem_req && !mem_can_allocate_req_buffer(fdip_proc_id, MRT_FDIPPRF, FALSE)) {
        // This rarely happens if mem_req_buffer_entries and ramulator_readq_entries are big enough.
        // e.g. If FE_FTQ_BLOCK_NUM = 302, MEM_REQ_BUFFER_ENTRIES = 1024 and RAMULATOR_READQ_ENTRIES = 512 never cause this break.
        DEBUG(fdip_proc_id, "Break due to full mem_req buf\n");
        break_reason = BR_FULL_MEM_REQ_BUF;
        break;
      }
      if (!line && emit_new_prefetch) { // create a mem request only if line doesn't exist. If the corresponding mem_req exists, it will merge.
        per_core_cur_op[fdip_proc_id] = op;
        if (FDIP_PREF_NO_LATENCY) {
          Mem_Req req;
          req.off_path           = op ? op->off_path : FALSE;
          req.off_path_confirmed = FALSE;
          req.type               = MRT_FDIPPRF;
          mem_req_set_types(&req, MRT_FDIPPRF);
          req.proc_id            = fdip_proc_id;
          req.addr               = line_addr;
          req.oldest_op_unique_num = (Counter)0;
          req.oldest_op_op_num = (Counter)0;
          req.oldest_op_addr = (Addr)0;
          req.dirty_l0 = op && op->table_info->mem_type == MEM_ST && !op->off_path;
          req.fdip_pref_off_path = op->off_path;
          req.demand_icache_emitted_cycle = 0;
          req.fdip_emitted_cycle = cycle_count;
          if (icache_fill_line(&req))
            STAT_EVENT(fdip_proc_id, FDIP_NEW_PREFETCHES_ONPATH + op->off_path);
          else
            ASSERT(fdip_proc_id, false);
        } else {
          Flag success = new_mem_req(MRT_FDIPPRF, fdip_proc_id, line_addr,
              ICACHE_LINE_SIZE, 0, NULL, instr_fill_line, unique_count++, 0);
          // A buffer entry should be available since it is checked by mem_can_allocate_req_buffer for a new prefetch
          ASSERT(fdip_proc_id, success);
          if (success == Mem_Queue_Req_Result::SUCCESS_NEW) {
            STAT_EVENT(ic_ref->proc_id, FDIP_NEW_PREFETCHES_ONPATH + op->off_path);
            DEBUG(fdip_proc_id, "Success to emit a new prefetch for %llx\n", line_addr);
          } else if (success == Mem_Queue_Req_Result::SUCCESS_MERGED) {
            STAT_EVENT(ic_ref->proc_id, FDIP_PREF_MSHR_PROBE_HIT_ONPATH + op->off_path);
            DEBUG(fdip_proc_id, "Success to merge a prefetch for %llx\n", line_addr);
          }
        }
        inc_prefetched_cls(line_addr);
      }
      per_core_last_line_addr[fdip_proc_id] = line_addr;
    }
    if (end_of_block) {
      ftq_entry_per_cycle++;
      DEBUG(fdip_proc_id, "End of block - ftq_entry_per_cycle: %d\n", ftq_entry_per_cycle);
    }
  }
  STAT_EVENT(ic_ref->proc_id, FDIP_BREAK_REACH_FTQ_END + break_reason);
  DEBUG(fdip_proc_id, "FTQ size : %lu, FDIP prefetch offset : %lu\n", decoupled_fe_ftq_num_ops(), decoupled_fe_ftq_iter_offset(iter));
  if (FDIP_ADJUSTABLE_FTQ && cycle_count % FDIP_ADJUSTABLE_FTQ_CYC == 0) {
    if (per_core_utility_timeliness_info[ic_ref->proc_id].unuseful_prefetches) {
      per_core_utility_timeliness_info[ic_ref->proc_id].utility_ratio = (double)per_core_utility_timeliness_info[ic_ref->proc_id].useful_prefetches/((double)per_core_utility_timeliness_info[ic_ref->proc_id].useful_prefetches+(double)per_core_utility_timeliness_info[ic_ref->proc_id].unuseful_prefetches);
      per_core_utility_timeliness_info[ic_ref->proc_id].adjust = TRUE;
    }
    per_core_utility_timeliness_info[ic_ref->proc_id].useful_prefetches = 0;
    per_core_utility_timeliness_info[ic_ref->proc_id].unuseful_prefetches = 0;
    DEBUG(fdip_proc_id, "Update utility ratio : %lf\n", per_core_utility_timeliness_info[ic_ref->proc_id].utility_ratio);
    DEBUG(fdip_proc_id, "mshr_prefetch_hits : %llu, icache_prefetch_hits : %llu\n", per_core_utility_timeliness_info[ic_ref->proc_id].mshr_prefetch_hits, per_core_utility_timeliness_info[ic_ref->proc_id].icache_prefetch_hits);
    if (per_core_utility_timeliness_info[ic_ref->proc_id].icache_prefetch_hits &&
        (per_core_utility_timeliness_info[ic_ref->proc_id].icache_prefetch_hits + per_core_utility_timeliness_info[ic_ref->proc_id].mshr_prefetch_hits > 4)) { // do not adjust if there are too few samples?
      per_core_utility_timeliness_info[ic_ref->proc_id].timeliness_ratio = (double)per_core_utility_timeliness_info[ic_ref->proc_id].mshr_prefetch_hits/((double)per_core_utility_timeliness_info[ic_ref->proc_id].mshr_prefetch_hits+(double)per_core_utility_timeliness_info[ic_ref->proc_id].icache_prefetch_hits);
      per_core_utility_timeliness_info[ic_ref->proc_id].adjust = TRUE;
    }
    per_core_utility_timeliness_info[ic_ref->proc_id].mshr_prefetch_hits = 0;
    per_core_utility_timeliness_info[ic_ref->proc_id].icache_prefetch_hits = 0;
    DEBUG(fdip_proc_id, "Update timeliness ratio : %lf\n", per_core_utility_timeliness_info[ic_ref->proc_id].timeliness_ratio);
  }

  per_core_fdip_ftq_occupancy_ops[ic_ref->proc_id] += decoupled_fe_ftq_iter_offset(iter);
  if (break_reason == BR_REACH_FTQ_END)
    per_core_fdip_ftq_occupancy_blocks[ic_ref->proc_id] += decoupled_fe_ftq_num_blocks();
  //else
    //ASSERT(fdip_proc_id, false); // for now TODO
}

Flag fdip_off_path(uns proc_id) {
  ASSERT(proc_id, per_core_cur_op[proc_id]);
  return per_core_cur_op[proc_id]->off_path;
}

template<typename A, typename B>
std::pair<B,A> flip_pair(const std::pair<A,B> &p)
{
  return std::pair<B,A>(p.second, p.first);
}

template<typename A, typename B>
std::multimap<B,A> flip_map(const std::map<A,B> &src)
{
  std::multimap<B,A> dst;
  std::transform(src.begin(), src.end(), std::inserter(dst, dst.begin()), flip_pair<A,B>);
  return dst;
}

void print_cl_info(uns proc_id) {
  if (!FDIP_ENABLE)
    return;
  std::unordered_map<Addr, std::pair<Counter, Counter>>* cnt_useful = &per_core_cnt_useful[proc_id];
  std::unordered_map<Addr, std::pair<Counter, Counter>>* cnt_unuseful = &per_core_cnt_unuseful[proc_id];
  std::unordered_map<Addr, Counter>* cnt_useful_ret = &per_core_cnt_useful_ret[proc_id];
  std::map<Addr, Counter>* prefetched_cls = &per_core_prefetched_cls[proc_id];
  std::map<Addr, Counter>* icache_miss = &per_core_icache_miss[proc_id];

  DEBUG(proc_id, "cnt_useful (ICACHE_USEFUL_FETCHES) size: %lu\n", cnt_useful->size());
  INC_STAT_EVENT(proc_id, ICACHE_USEFUL_FETCHES, cnt_useful->size());
  DEBUG(proc_id, "cnt_useful_ret (USEFUL_CACHELINES_RETIRED) size: %lu\n", cnt_useful_ret->size());
  INC_STAT_EVENT(proc_id, USEFUL_CACHELINES_RETIRED, cnt_useful_ret->size());
  DEBUG(proc_id, "cnt_unuseful (ICACHE_UNUSEFUL_FETCHES) size: %lu\n", cnt_unuseful->size());
  INC_STAT_EVENT(proc_id, ICACHE_UNUSEFUL_FETCHES, cnt_unuseful->size());
  DEBUG(proc_id, "icache miss cache lines (UNIQUE_MISSED_LINES) size: %lu\n", icache_miss->size());
  INC_STAT_EVENT(proc_id, UNIQUE_MISSED_LINES, icache_miss->size());
  std::multimap<Counter, Addr> icache_miss_sorted = flip_map(*icache_miss);
  for(std::multimap<Counter, Addr>::const_iterator it = icache_miss_sorted.begin();
      it != icache_miss_sorted.end(); ++it) {
    DEBUG(proc_id, "[set %u] 0x%llx missed %llu times\n", (uns)(it->second >> ic_ref->icache.shift_bits & ic_ref->icache.set_mask), it->second, it->first);
  }
  DEBUG(proc_id, "unique prefetched lines (UNIQUE_PREFETCHED_LINES) size: %lu\n", prefetched_cls->size());
  INC_STAT_EVENT(proc_id, UNIQUE_PREFETCHED_LINES, prefetched_cls->size());
  std::multimap<Counter, Addr> prefetched_cls_sorted = flip_map(*prefetched_cls);
  for(std::multimap<Counter, Addr>::const_iterator it = prefetched_cls_sorted.begin();
      it != prefetched_cls_sorted.end(); ++it) {
    auto useful_ret_iter = cnt_useful_ret->find(it->second);
    if (useful_ret_iter == cnt_useful_ret->end()) {
      DEBUG(proc_id, "Unuseful 0x%llx prefetched %llu times\n", it->second, it->first);
    }
  }
}

void inc_cnt_useful(uns proc_id, Addr line_addr, Flag icache_off_path) {
  auto useful_iter = per_core_cnt_useful[proc_id].find(line_addr);
  if (useful_iter == per_core_cnt_useful[proc_id].end()) {
    if (icache_off_path)
      per_core_cnt_useful[proc_id].insert(std::make_pair(std::move(line_addr), std::make_pair(0, 1)));
    else
      per_core_cnt_useful[proc_id].insert(std::make_pair(std::move(line_addr), std::make_pair(1, 0)));
  } else {
    if (icache_off_path)
      useful_iter->second.second++;
    else
      useful_iter->second.first++;
  }
}

void inc_cnt_unuseful(uns proc_id, Addr line_addr, Flag icache_off_path) {
  auto unuseful_iter = per_core_cnt_unuseful[proc_id].find(line_addr);
  if (unuseful_iter == per_core_cnt_unuseful[proc_id].end()) {
    if (icache_off_path)
      per_core_cnt_unuseful[proc_id].insert(std::make_pair(std::move(line_addr), std::make_pair(0, 1)));
    else
      per_core_cnt_unuseful[proc_id].insert(std::make_pair(std::move(line_addr), std::make_pair(1, 0)));
  } else {
    if (icache_off_path)
      unuseful_iter->second.second++;
    else
      unuseful_iter->second.first++;
  }
}

void inc_useful_unuseful_2bit(uns proc_id, Addr line_addr) {
  auto useful_iter = per_core_useful_unuseful_2bit[proc_id].find(line_addr);
  if (useful_iter == per_core_useful_unuseful_2bit[proc_id].end())
    per_core_useful_unuseful_2bit[proc_id].insert(std::pair<Addr, Counter>(line_addr, 2));
  else if (useful_iter->second < 3)
    useful_iter->second++;
}

void dec_useful_unuseful_2bit(uns proc_id, Addr line_addr) {
  auto useful_iter = per_core_useful_unuseful_2bit[proc_id].find(line_addr);
  if (useful_iter == per_core_useful_unuseful_2bit[proc_id].end())
    per_core_useful_unuseful_2bit[proc_id].insert(std::pair<Addr, Counter>(line_addr, 0));
  else if (useful_iter->second > 0)
    useful_iter->second--;
}

void inc_useful_unuseful_3bit(uns proc_id, Addr line_addr) {
  auto useful_iter = per_core_useful_unuseful_3bit[proc_id].find(line_addr);
  if (useful_iter == per_core_useful_unuseful_3bit[proc_id].end())
    per_core_useful_unuseful_3bit[proc_id].insert(std::pair<Addr, Counter>(line_addr, 4));
  else if (useful_iter->second < 7)
    useful_iter->second++;
}

void dec_useful_unuseful_3bit(uns proc_id, Addr line_addr) {
  auto useful_iter = per_core_useful_unuseful_3bit[proc_id].find(line_addr);
  if (useful_iter == per_core_useful_unuseful_3bit[proc_id].end())
    per_core_useful_unuseful_3bit[proc_id].insert(std::pair<Addr, Counter>(line_addr, 2));
  else if (useful_iter->second > 0)
    useful_iter->second--;
}

void inc_cnt_useful_ret(uns proc_id, Addr line_addr) {
  auto useful_iter = per_core_cnt_useful_ret[proc_id].find(line_addr);
  if (useful_iter == per_core_cnt_useful_ret[proc_id].end())
    per_core_cnt_useful_ret[proc_id].insert(std::pair<Addr, Counter>(line_addr, 1));
  else
    useful_iter->second++;
}

void inc_icache_miss(uns proc_id, Addr line_addr) {
  auto cl_iter = per_core_icache_miss[proc_id].find(line_addr);
  if (cl_iter == per_core_icache_miss[proc_id].end())
    per_core_icache_miss[proc_id].insert(std::pair<Addr, Counter>(line_addr, 1));
  else
    cl_iter->second++;
}

void inc_prefetched_cls(Addr line_addr) {
  auto cl_iter = per_core_prefetched_cls[fdip_proc_id].find(line_addr);
  if (cl_iter == per_core_prefetched_cls[fdip_proc_id].end()) {
    per_core_prefetched_cls[fdip_proc_id].insert(std::pair<Addr, Counter>(line_addr, 1));
    per_core_prefetched_cls_info[fdip_proc_id].insert(std::make_pair(std::move(line_addr), std::make_pair(std::move(cycle_count), 0)));
  } else {
    cl_iter->second++;
    auto cl_info_iter = per_core_prefetched_cls_info[fdip_proc_id].find(line_addr);
    ASSERT(fdip_proc_id, cl_info_iter != per_core_prefetched_cls_info[fdip_proc_id].end());
    cl_info_iter->second.first = cycle_count;
  }
}

void probe_prefetched_cls(Addr line_addr) {
  auto cl_iter = per_core_prefetched_cls_info[fdip_proc_id].find(line_addr);
  if (cl_iter != per_core_prefetched_cls_info[fdip_proc_id].end())
    cl_iter->second.first = cycle_count;
}

void evict_prefetched_cls(uns proc_id, Addr line_addr) {
  auto cl_iter = per_core_prefetched_cls_info[proc_id].find(line_addr);
  if (cl_iter != per_core_prefetched_cls_info[proc_id].end())
    cl_iter->second.second = cycle_count;
}

uns get_miss_reason(uns proc_id, Addr line_addr) {
  auto cl_iter = per_core_prefetched_cls_info[proc_id].find(line_addr);
  if (cl_iter == per_core_prefetched_cls_info[proc_id].end()) {
    auto tmp_iter = per_core_prefetched_cls[proc_id].find(line_addr);
    DEBUG(proc_id, "%llx misses due to 'not prefetched'\n", line_addr);
    ASSERT(proc_id, tmp_iter == per_core_prefetched_cls[proc_id].end());
    return Imiss_Reason::IMISS_NOT_PREFETCHED;
  }
  if (cl_iter->second.first >= per_core_last_recover_cycle[proc_id] && cl_iter->second.second > cl_iter->second.first) {
    DEBUG(proc_id, "%llx misses due to 'prefetched too early'\n", line_addr);
    return Imiss_Reason::IMISS_TOO_EARLY;
  }
  DEBUG(proc_id, "%llx misses due to 'MSHR hit'\n", line_addr);
  return Imiss_Reason::IMISS_MSHR_HIT;
}

uns get_last_miss_reason(uns proc_id) {
  return per_core_last_imiss_reason[proc_id];
}

void set_last_miss_reason(uns proc_id, uns reason) {
  per_core_last_imiss_reason[proc_id] = reason;
}

uint64_t get_fdip_ftq_occupancy_ops(uns proc_id) {
  return (uint64_t)per_core_fdip_ftq_occupancy_ops[proc_id]/cycle_count;
}

uint64_t get_fdip_ftq_occupancy(uns proc_id) {
  return (uint64_t)per_core_fdip_ftq_occupancy_blocks[proc_id]/cycle_count;
}

static inline void determine_usefulness_by_inf_hash(Addr line_addr, Flag* emit_new_prefetch) {
  switch(FDIP_UTILITY_LEARN_POLICY) {
    case Utility_Learn_Policy::LEARN_ONLY_ONPATH_USEFUL: {
      std::unordered_map<Addr, std::pair<Counter, Counter>>* cnt_useful = &per_core_cnt_useful[fdip_proc_id];
      auto iter = cnt_useful->find(line_addr);
      if (iter != cnt_useful->end() && iter->second.first > 0)
        *emit_new_prefetch = TRUE;
      else
        *emit_new_prefetch = FALSE;
      break;
    }
    case Utility_Learn_Policy::LEARN_ON_OFF_USEFUL: {
      std::unordered_map<Addr, std::pair<Counter, Counter>>* cnt_useful = &per_core_cnt_useful[fdip_proc_id];
      auto iter = cnt_useful->find(line_addr);
      if (iter != cnt_useful->end())
        *emit_new_prefetch = TRUE;
      else
        *emit_new_prefetch = FALSE;
      break;
    }
    case Utility_Learn_Policy::LEARN_ONLY_UNUSEFUL: {
      std::unordered_map<Addr, std::pair<Counter, Counter>>* cnt_unuseful = &per_core_cnt_unuseful[fdip_proc_id];
      auto iter = cnt_unuseful->find(line_addr);
      if (iter == cnt_unuseful->end())
        *emit_new_prefetch = TRUE;
      else
        *emit_new_prefetch = FALSE;
      break;
    }
    case Utility_Learn_Policy::LEARN_ON_OFF_USEFUL_UNUSEFUL_2BIT: {
      std::unordered_map<Addr, Counter>* useful_unuseful_2bit = &per_core_useful_unuseful_2bit[fdip_proc_id];
      auto iter = useful_unuseful_2bit->find(line_addr);
      if (iter != useful_unuseful_2bit->end() && iter->second >= 2)
        *emit_new_prefetch = TRUE;
      else
        *emit_new_prefetch = FALSE;
      break;
    }
    case Utility_Learn_Policy::LEARN_ON_OFF_USEFUL_UNUSEFUL_3BIT: {
      std::unordered_map<Addr, Counter>* useful_unuseful_3bit = &per_core_useful_unuseful_3bit[fdip_proc_id];
      auto iter = useful_unuseful_3bit->find(line_addr);
      if (iter != useful_unuseful_3bit->end() && iter->second >= 4)
        *emit_new_prefetch = TRUE;
      else
        *emit_new_prefetch = FALSE;
      break;
    }
  }
  if (*emit_new_prefetch) {
    DEBUG(fdip_proc_id, "emit a new prefetch for cl 0x%llx", line_addr);
  } else {
    DEBUG(fdip_proc_id, "do not emit a new prefetch for cl 0x%llx", line_addr);
    per_core_last_cl_unuseful[fdip_proc_id] = line_addr;
  }
}

static inline void determine_usefulness_by_utility_cache(Addr line_addr, Flag* emit_new_prefetch) {
  Addr uc_line_addr = 0;
  void* useful = (void*)cache_access(&per_core_fdip_uc[fdip_proc_id], line_addr, &uc_line_addr, TRUE);
  if (useful) {
    STAT_EVENT(fdip_proc_id, FDIP_UC_HIT);
    *emit_new_prefetch = TRUE;
    DEBUG(fdip_proc_id, "uc : emit a new prefetch for cl 0x%llx, uc_line_addr %llx, fdip_off_path: %d", line_addr, uc_line_addr, fdip_off_path(fdip_proc_id) ? 1 : 0);
  } else {
    STAT_EVENT(fdip_proc_id, FDIP_UC_MISS);
    DEBUG(fdip_proc_id, "uc : do not emit a new prefetch for cl 0x%llx, uc_line_addr %llx", line_addr, uc_line_addr);
    *emit_new_prefetch = FALSE;
    per_core_last_cl_unuseful[fdip_proc_id] = line_addr;
  }
}

static inline void* bloom_lookup(uns proc_id, Addr uc_line_addr) {
  Bloom_Filter* bf = &per_core_bloom_filter[proc_id];
  Addr line_addr = uc_line_addr >> 6;
  return (void*)(bf->bloom->contains(line_addr) || bf->bloom2->contains(line_addr >> 1) || bf->bloom4->contains(line_addr >> 2));
}

static inline void insert1(Addr line_addr) {
  Bloom_Filter* bf = &per_core_bloom_filter[fdip_proc_id];
  STAT_EVENT(fdip_proc_id, FDIP_BLOOM_1INSERT);
  if (!bf->bloom2->contains(line_addr >> 1) && !bf->bloom4->contains(line_addr >> 2)) {
    bf->bloom->insert(line_addr);
  }

}

static inline void insert2(Addr line_addr) {
  Bloom_Filter* bf = &per_core_bloom_filter[fdip_proc_id];
  if((line_addr & 1) == 0) { //2CL aligned
    if (!bf->bloom4->contains(line_addr >> 2) && !bf->bloom2->contains(line_addr >> 1)) {
      bf->bloom2->insert(line_addr >> 1);
    }
    STAT_EVENT(fdip_proc_id, FDIP_BLOOM_2INSERT);
  }
  else {
    insert1(line_addr);
    insert1(line_addr + 1);
  }
}

static inline void insert3(Addr line_addr) {
  if((line_addr & 1) == 0) { //2CL aligned
    insert2(line_addr);
    insert1(line_addr + 2);
  }
  else {
    insert1(line_addr + 1);
    insert2(line_addr);
  }
}

static inline void insert4(Addr line_addr) {
  Bloom_Filter* bf = &per_core_bloom_filter[fdip_proc_id];
  ASSERT(0, (line_addr & 3) == 0);
  bf->bloom4->insert(line_addr >> 2);
  STAT_EVENT(fdip_proc_id, FDIP_BLOOM_4INSERT);
}

static inline void insert_remaining(uint32_t inserted) {
  Bloom_Filter* bf = &per_core_bloom_filter[fdip_proc_id];
  while (inserted + 4 <= bf->last_prefetch_candidate_counter) {
    insert4(bf->last_prefetch_candidate + inserted);
    inserted += 4;
  }
  if (inserted + 3 == bf->last_prefetch_candidate_counter)
    insert3(bf->last_prefetch_candidate + inserted);
  if (inserted + 2 == bf->last_prefetch_candidate_counter)
    insert2(bf->last_prefetch_candidate + inserted);
  if (inserted + 1 == bf->last_prefetch_candidate_counter)
    insert1(bf->last_prefetch_candidate + inserted);

}

static inline void bloom_insert() {
  Bloom_Filter* bf = &per_core_bloom_filter[fdip_proc_id];
  uint32_t inserted = 0;
  if (bf->last_prefetch_candidate_counter < 4) {
    insert_remaining(inserted);
    return;
  }
  if ((bf->last_prefetch_candidate & 3) == 0) {
    //4cl aligned
    insert_remaining(inserted);
  }
  else if ((bf->last_prefetch_candidate & 3) == 2) {
    //2cl algned
    insert2(bf->last_prefetch_candidate);
    inserted += 2;
    insert_remaining(inserted);
  }
  else if ((bf->last_prefetch_candidate & 3) == 1) {
    //cl aligned
    //cl aligned
    insert3(bf->last_prefetch_candidate);
    inserted += 3;
    insert_remaining(inserted);
  }
  else if ((bf->last_prefetch_candidate & 3) == 3) {
    //cl aligned
    insert1(bf->last_prefetch_candidate);
    inserted +=1;
    insert_remaining(inserted);
  }
  return;
}

static inline void detect_stream(uns proc_id, Addr uc_line_addr) {
  Bloom_Filter* bf = &per_core_bloom_filter[proc_id];
  Addr line_addr = uc_line_addr >> 6;

  if(bf->last_prefetch_candidate_counter == 0) {
    bf->last_prefetch_candidate_counter++;
    bf->last_prefetch_candidate = line_addr;
    return;
  }
  STAT_EVENT(fdip_proc_id, FDIP_BLOOM_INSERTED);

  if (line_addr == bf->last_prefetch_candidate + bf->last_prefetch_candidate_counter) {
    bf->last_prefetch_candidate_counter++;
  }
  else {
    bloom_insert();
    bf->last_prefetch_candidate_counter = 1;
    bf->last_prefetch_candidate = line_addr;
  }
}

static inline void determine_usefulness_by_bloom_filter(Addr line_addr, Flag* emit_new_prefetch) {
  void* useful = bloom_lookup(fdip_proc_id, line_addr);
  if (useful) {
    STAT_EVENT(fdip_proc_id, FDIP_BLOOM_HIT);
    *emit_new_prefetch = TRUE;
    DEBUG(fdip_proc_id, "bloom : emit a new prefetch for cl 0x%llx off_path: %u", line_addr, fdip_off_path(fdip_proc_id) ? 1 : 0);
  } else {
    STAT_EVENT(fdip_proc_id, FDIP_BLOOM_MISS);
    DEBUG(fdip_proc_id, "bloom : do not emit a new prefetch for cl 0x%llx", line_addr);
    *emit_new_prefetch = FALSE;
    per_core_last_cl_unuseful[fdip_proc_id] = line_addr;
  }
}

Flag determine_usefulness(Addr line_addr) {
  if (operating_mode == SIMULATION_MODE && inst_count[fdip_proc_id] < FDIP_UTILITY_MIN_LEARNING_INST)
    return TRUE;

  Flag emit_new_prefetch = FALSE;
  if (per_core_last_cl_unuseful[fdip_proc_id] != line_addr) {
    if (FDIP_UTILITY_HASH_ENABLE)
      determine_usefulness_by_inf_hash(line_addr, &emit_new_prefetch);
    else if (FDIP_UC_SIZE)
      determine_usefulness_by_utility_cache(line_addr, &emit_new_prefetch);
    else if (FDIP_BLOOM_FILTER)
      determine_usefulness_by_bloom_filter(line_addr, &emit_new_prefetch);
  }
  return emit_new_prefetch;
}

void update_useful_lines(uns proc_id, Op* op) {
  if (!FDIP_UTILITY_HASH_ENABLE && !FDIP_UC_SIZE && !FDIP_BLOOM_FILTER)
    return;

  Addr fetch_addr = per_core_last_bbl_start_addr[proc_id];
  Addr useful_cl_addr = per_core_last_bbl_start_addr[proc_id] & ~0x3F;
  Addr last_useful_cl_addr = 0;
  while (fetch_addr <= op->inst_info->addr) {
    if (last_useful_cl_addr != useful_cl_addr) {
      if (FDIP_UTILITY_HASH_ENABLE)
        inc_cnt_useful_ret(proc_id, useful_cl_addr);
      if (FDIP_UC_SIZE) {
        Addr uc_line_addr = 0;
        Addr repl_uc_line_addr = 0;
        void* cnt = (void*)cache_access(&per_core_fdip_uc[proc_id], useful_cl_addr, &uc_line_addr, TRUE);
        if (!cnt)
          cache_insert_replpos(&per_core_fdip_uc[proc_id], fdip_proc_id, useful_cl_addr, &uc_line_addr,
              &repl_uc_line_addr, (Cache_Insert_Repl)FDIP_UC_INSERT_REPLPOL, FALSE);
        UNUSED(uc_line_addr);
        if (repl_uc_line_addr)
          STAT_EVENT(proc_id, FDIP_UC_REPLACEMENT);
      }
      if (FDIP_BLOOM_FILTER) {
        void* cnt = bloom_lookup(proc_id, useful_cl_addr);
        if (!cnt)
          detect_stream(proc_id, useful_cl_addr);
      }
    }

    last_useful_cl_addr = useful_cl_addr;
    useful_cl_addr = ++fetch_addr & ~0x3F;
  }
  per_core_last_bbl_start_addr[proc_id] = op->oracle_info.npc;
  DEBUG(proc_id, "last_bbl_start_addr: %llx\n", per_core_last_bbl_start_addr[proc_id]);
}

void update_useful_lines_uc(uns proc_id, Addr line_addr) {
  if (!FDIP_UC_SIZE)
    return;
  Addr uc_line_addr = 0;
  Addr repl_uc_line_addr = 0;
  void* cnt = (void*)cache_access(&per_core_fdip_uc[proc_id], line_addr, &uc_line_addr, TRUE);
  if (!cnt)
    cache_insert_replpos(&per_core_fdip_uc[proc_id], fdip_proc_id, line_addr, &uc_line_addr,
        &repl_uc_line_addr, (Cache_Insert_Repl)FDIP_UC_INSERT_REPLPOL, FALSE);
  UNUSED(uc_line_addr);
  if (repl_uc_line_addr)
    STAT_EVENT(proc_id, FDIP_UC_REPLACEMENT);
}

void update_useful_lines_bloom_filter(uns proc_id, Addr line_addr) {
  if (!FDIP_BLOOM_FILTER)
    return;
  void* cnt = bloom_lookup(proc_id, line_addr);
  if (!cnt)
    detect_stream(proc_id, line_addr);
}

void inc_utility_info(uns proc_id, Flag useful) {
  if (FDIP_ADJUSTABLE_FTQ != 1 && FDIP_ADJUSTABLE_FTQ != 3)
    return;
  if (useful)
    per_core_utility_timeliness_info[proc_id].useful_prefetches++;
  else
    per_core_utility_timeliness_info[proc_id].unuseful_prefetches++;
}

void inc_timeliness_info(uns proc_id, Flag mshr_hit) {
  if (FDIP_ADJUSTABLE_FTQ != 2 && FDIP_ADJUSTABLE_FTQ != 3)
    return;
  if (mshr_hit)
    per_core_utility_timeliness_info[proc_id].mshr_prefetch_hits++;
  else
    per_core_utility_timeliness_info[proc_id].icache_prefetch_hits++;
}
