#include "decoupled_frontend.h"
#include "prefetcher/fdip_new.h"
#include "libs/bloom_filter.hpp"

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
const int MAX_PREF_CYC = 2;
uint64_t last_line_addr = 0;
int fdip_proc_id;
Icache_Stage *ic_ref;
Op *cur_op = NULL;

typedef enum FDIP_BREAK_enum {
  BR_REACH_FTQ_END,
  BR_FTQ_EMPTY,
  BR_MAX_PREF_CYC,
  BR_FULL_MEM_REQ_BUF,
} FDIP_Break;

// for stats or usefulness study
// <CL address, demand access count> - useful count
std::unordered_map<Addr, Counter> cnt_useful;
// <CL address, eviction count w/o hit> - unuseful count
std::unordered_map<Addr, Counter> cnt_unuseful;
// <CL addresses, retirement count> - on-path useful count
std::unordered_map<Addr, Counter> cnt_useful_ret;
// <CL addresses, icache miss count>
std::map<Addr, Counter> icache_miss;
// <CL addresses, prefetched count>
std::map<Addr, Counter> prefetched_cls;
// <CL address, cyc_access_by_fdip, cyc_evicted_from_l1> - prefetched and access time information for timeliness analysis
std::unordered_map<Addr, std::pair<Counter, Counter>> prefetched_cls_info;
Addr last_cl_unuseful = 0;
// accumulated FTQ occupancy every cycle
uint64_t fdip_ftq_occupancy = 0;
extern Counter last_recover_cycle;
Addr last_bbl_start_addr = 0;
// Utility cache
Cache fdip_uc;
// bloom filters
bloom_filter *bloom;
bloom_filter *bloom2;
bloom_filter *bloom4;
bloom_parameters bloom1_parameters;
bloom_parameters bloom2_parameters;
bloom_parameters bloom4_parameters;
Addr last_prefetch_candidate;
uint32_t last_prefetch_candidate_counter = 0;

void init_fdip() {
  iter = decoupled_fe_new_ftq_iter();
  if (FDIP_UC_SIZE) {
    ASSERT(fdip_proc_id, !FDIP_BLOOM_FILTER);
    init_cache(&fdip_uc, "FDIP_USEFULNESS_CACHE", FDIP_UC_SIZE, FDIP_UC_ASSOC, ICACHE_LINE_SIZE,
               0, REPL_TRUE_LRU); //Data size = 2 byte
  }
  if (FDIP_BLOOM_FILTER) {
    ASSERT(fdip_proc_id, !FDIP_UC_SIZE && !FDIP_UTILITY_HASH_ENABLE);
    bloom1_parameters.projected_element_count = FDIP_BLOOM_ENTRIES;
    //bloom_parameters.maximum_number_of_hashes = FDIP_BLOOM_HASHES;
    //bloom_parameters.minimum_number_of_hashes = FDIP_BLOOM_HASHES;
    bloom1_parameters.false_positive_probability = 0.005;
    //bloom1_parameters.maximum_size = FDIP_BLOOM_SIZE;
    //bloom_parameters.minimum_size = FDIP_BLOOM_SIZE;
    bloom1_parameters.compute_optimal_parameters();
    bloom = new bloom_filter(bloom1_parameters);

    bloom2_parameters.projected_element_count = FDIP_BLOOM2_ENTRIES;
    bloom2_parameters.false_positive_probability = 0.005;
    bloom2_parameters.compute_optimal_parameters();
    bloom2 = new bloom_filter(bloom2_parameters);

    bloom4_parameters.projected_element_count = FDIP_BLOOM4_ENTRIES;
    bloom4_parameters.false_positive_probability = 0.005;
    bloom4_parameters.compute_optimal_parameters();
    bloom4 = new bloom_filter(bloom4_parameters);
  }
}

void set_fdip(int _proc_id, Icache_Stage *_ic) {
  fdip_proc_id = _proc_id;
  ic_ref = _ic;
}

void update_fdip() {
  if (!FDIP_ENABLE)
    return;

  int prefetch_per_cycle = 0;
  Flag emit_new_prefetch = FALSE;
  FDIP_Break break_reason = BR_REACH_FTQ_END;

  do {
    Op *op = decoupled_fe_ftq_iter_get(iter);
    if (!op) {
      std::cout << "FTQ Empty" << std::endl;
      break_reason = BR_FTQ_EMPTY;
      break;
    }
    if (prefetch_per_cycle == MAX_PREF_CYC) {
      break_reason = BR_MAX_PREF_CYC;
      break;
    }
    if (op->op_num == 0)
      last_bbl_start_addr = op->inst_info->addr;

    uint64_t pc_addr = op->inst_info->addr;
    Addr line_addr = op->inst_info->addr && ~0x3F;
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
      if (!line && !mem_req) {
        if (FDIP_UTILITY_HASH_ENABLE || FDIP_UC_SIZE || FDIP_BLOOM_FILTER)
          emit_new_prefetch = determine_usefulness(line_addr);
        else
          emit_new_prefetch = TRUE;
      }

      if (line) {
        probe_prefetched_cls(line_addr);
        STAT_EVENT(ic_ref->proc_id, FDIP_PREF_ICACHE_PROBE_HIT_ONPATH + op->off_path);
      }

      if (emit_new_prefetch && !mem_can_allocate_req_buffer(fdip_proc_id, MRT_FDIPPRF, FALSE)) {
        break_reason = BR_FULL_MEM_REQ_BUF;
        break;
      }
      if (!line) { // create a mem request only if line doesn't exist. If the corresponding mem_req exists, it will merge.
        cur_op = op;
        Flag success = new_mem_req(MRT_FDIPPRF, fdip_proc_id, line_addr,
                                   ICACHE_LINE_SIZE, 0, NULL, instr_fill_line, unique_count++, 0);
        // A buffer entry should be available since it is checked by mem_can_allocate_req_buffer for a new prefetch
        ASSERT(fdip_proc_id, success);
        if (success == Mem_Queue_Req_Result::SUCCESS_NEW)
          STAT_EVENT(ic_ref->proc_id, FDIP_NEW_PREFETCHES_ONPATH + op->off_path);
        else if (success == Mem_Queue_Req_Result::SUCCESS_MERGED)
          STAT_EVENT(ic_ref->proc_id, FDIP_PREF_MSHR_PROBE_HIT_ONPATH + op->off_path);
        prefetch_per_cycle++;
      }
    }
    last_line_addr = line_addr;

  } while (decoupled_fe_ftq_iter_advance(iter));
  STAT_EVENT(ic_ref->proc_id, FDIP_BREAK_REACH_FTQ_END + break_reason);
  fdip_ftq_occupancy += decoupled_fe_ftq_iter_offset(iter);
}

Flag fdip_off_path(void) {
  ASSERT(fdip_proc_id, cur_op);
  return cur_op->off_path;
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

void print_cl_info(void) {
  if (!FDIP_ENABLE)
    return;

  DEBUG(fdip_proc_id, "============= cnt_useful ============== size: %lu\n", cnt_useful.size());
  INC_STAT_EVENT(fdip_proc_id, FDIP_USEFUL_CACHELINES, cnt_useful.size());
  DEBUG(fdip_proc_id, "%lu useful cache lines have not been learned\n", cnt_useful.size() - cnt_useful_ret.size());
  DEBUG(fdip_proc_id, "%lu useful cache lines have not been prefetched\n", cnt_useful.size() - prefetched_cls.size());
  for(std::unordered_map<Addr, Counter>::const_iterator it = cnt_useful.begin();
        it != cnt_useful.end(); ++it) {
    auto useful_ret_iter = cnt_useful_ret.find(it->first);
    if (useful_ret_iter == cnt_useful_ret.end()) {
      DEBUG(fdip_proc_id, "Useful 0x%llx has not been learned in hash - hit count : %llu\n", it->first, it->second);
    }
    auto prefetched_cl_iter = prefetched_cls.find(it->first);
    if (prefetched_cl_iter == prefetched_cls.end() && it->second > 1) {
      DEBUG(fdip_proc_id, "Useful 0x%llx has not been prefetched - hit count is greater than 1: %llu", it->first, it->second);
      auto icache_miss_iter = icache_miss.find(it->first);
      if (icache_miss_iter != icache_miss.end()) {
        DEBUG(fdip_proc_id, ", miss count: %llu\n", icache_miss_iter->second);
      } else
        DEBUG(fdip_proc_id, ", miss count: 0\n");
    }
  }
  DEBUG(fdip_proc_id, "============= cnt_useful_ret ============= size: %lu\n", cnt_useful_ret.size());
  DEBUG(fdip_proc_id, "%lu useful (retired) cache lines have not been prefetched\n", cnt_useful_ret.size() - prefetched_cls.size());
  INC_STAT_EVENT(fdip_proc_id, FDIP_USEFUL_CACHELINES_RETIRED, cnt_useful_ret.size());
  for(std::unordered_map<Addr, Counter>::const_iterator it = cnt_useful_ret.begin();
    it != cnt_useful_ret.end(); ++it) {
    auto prefetched_cl_iter = prefetched_cls.find(it->first);
    if (prefetched_cl_iter == prefetched_cls.end() && it->second > 1) {
      DEBUG(fdip_proc_id, "Useful 0x%llx has not been prefetched ever - useful count is greater than 1: %llu\n", it->first, it->second);
    }
  }
  DEBUG(fdip_proc_id, "============= cnt_unuseful ============== size: %lu\n", cnt_unuseful.size());
  INC_STAT_EVENT(fdip_proc_id, FDIP_UNUSEFUL_CACHELINES, cnt_unuseful.size());
  for(std::unordered_map<Addr, Counter>::const_iterator it = cnt_unuseful.begin();
     it != cnt_unuseful.end(); ++it) {
    DEBUG(fdip_proc_id, "0x%llx - %llu\n", it->first, it->second);
    auto useful_iter = cnt_useful.find(it->first);
    if (useful_iter != cnt_useful.end() && it->second >= useful_iter->second) {
      DEBUG(fdip_proc_id, "useful count (%llu) is smaller\n", useful_iter->second);
    }
  }
//  DEBUG(fdip_proc_id, "============= fetch cl addresses not touched ever ===== size: %lu\n", fetch_cl_addr.size());
  //INC_STAT_EVENT(fdip_proc_id, NOT_TOUCHED_CACHELINES, fetch_cl_addr.size());
  //for(std::vector<Counter>::const_iterator it = fetch_cl_addr.begin();
      //it != fetch_cl_addr.end(); ++it) {
    //DEBUG(fdip_proc_id, "0x%llx has never touched\n", *it);
  //}

  DEBUG(fdip_proc_id, "============= icache miss cache lines ===== size: %lu\n", icache_miss.size());
  INC_STAT_EVENT(fdip_proc_id, UNIQUE_MISSED_LINES, icache_miss.size());
  std::multimap<Counter, Addr> icache_miss_sorted = flip_map(icache_miss);
  for(std::multimap<Counter, Addr>::const_iterator it = icache_miss_sorted.begin();
      it != icache_miss_sorted.end(); ++it) {
    DEBUG(fdip_proc_id, "[set %u] 0x%llx missed %llu times", (uns)(it->second >> ic_ref->icache.shift_bits & ic_ref->icache.set_mask), it->second, it->first);
    auto useful_iter = cnt_useful.find(it->second);
    if (useful_iter != cnt_useful.end())
      DEBUG(fdip_proc_id, ", hit %llu times\n", useful_iter->second);
    else
      DEBUG(fdip_proc_id, ", hit 0 times\n");
  }

  DEBUG(fdip_proc_id, "============= unique prefetched lines ===== size: %lu\n", prefetched_cls.size());
  INC_STAT_EVENT(fdip_proc_id, UNIQUE_PREFETCHED_LINES, prefetched_cls.size());
  std::multimap<Counter, Addr> prefetched_cls_sorted = flip_map(prefetched_cls);
  for(std::multimap<Counter, Addr>::const_iterator it = prefetched_cls_sorted.begin();
      it != prefetched_cls_sorted.end(); ++it) {
    auto useful_ret_iter = cnt_useful_ret.find(it->second);
    if (useful_ret_iter == cnt_useful_ret.end()) {
      DEBUG(fdip_proc_id, "Unuseful 0x%llx prefetched %llu times\n", it->second, it->first);
    }
  }
}

void inc_cnt_useful(Addr line_addr) {
  auto useful_iter = cnt_useful.find(line_addr);
  if (useful_iter == cnt_useful.end())
    cnt_useful.insert(std::pair<Addr, Counter>(line_addr, 1));
  else
    useful_iter->second++;
}

void inc_cnt_unuseful(Addr line_addr) {
  auto unuseful_iter = cnt_unuseful.find(line_addr);
  if (unuseful_iter == cnt_unuseful.end())
    cnt_unuseful.insert(std::pair<Addr, Counter>(line_addr, 1));
  else
    unuseful_iter->second++;
}

void inc_cnt_useful_ret(Addr line_addr) {
  auto useful_iter = cnt_useful_ret.find(line_addr);
  if (useful_iter == cnt_useful_ret.end())
    cnt_useful_ret.insert(std::pair<Addr, Counter>(line_addr, 1));
  else
    useful_iter->second++;
}

void inc_icache_miss(Addr line_addr) {
  auto cl_iter = icache_miss.find(line_addr);
  if (cl_iter == icache_miss.end())
    icache_miss.insert(std::pair<Addr, Counter>(line_addr, 1));
  else
    cl_iter->second++;
}

void inc_prefetched_cls(Addr line_addr) {
  auto cl_iter = prefetched_cls.find(line_addr);
  if (cl_iter == prefetched_cls.end()) {
    prefetched_cls.insert(std::pair<Addr, Counter>(line_addr, 1));
    prefetched_cls_info.insert(std::make_pair(std::move(line_addr), std::make_pair(std::move(cycle_count), 0)));
  } else {
    cl_iter->second++;
    auto cl_info_iter = prefetched_cls_info.find(line_addr);
    ASSERT(fdip_proc_id, cl_info_iter != prefetched_cls_info.end());
    cl_info_iter->second.first = cycle_count;
  }
}

void probe_prefetched_cls(Addr line_addr) {
  auto cl_iter = prefetched_cls_info.find(line_addr);
  if (cl_iter != prefetched_cls_info.end())
    cl_iter->second.first = cycle_count;
}

void evict_prefetched_cls(Addr line_addr) {
  auto cl_iter = prefetched_cls_info.find(line_addr);
  if (cl_iter != prefetched_cls_info.end())
    cl_iter->second.second = cycle_count;
}

uns get_miss_reason(Addr line_addr) {
  auto cl_iter = prefetched_cls_info.find(line_addr);
  if (cl_iter == prefetched_cls_info.end()) {
    auto tmp_iter = prefetched_cls.find(line_addr);
    DEBUG(fdip_proc_id, "%llx misses due to 'not prefetched'\n", line_addr);
    ASSERT(fdip_proc_id, tmp_iter == prefetched_cls.end());
    return Imiss_Reason::IMISS_NOT_PREFETCHED;
  }
  if (cl_iter->second.first > last_recover_cycle && cl_iter->second.second > cl_iter->second.first) {
    DEBUG(fdip_proc_id, "%llx misses due to 'prefetched too early'\n", line_addr);
    return Imiss_Reason::IMISS_TOO_EARLY;
  }
  DEBUG(fdip_proc_id, "%llx misses due to 'MSHR hit'\n", line_addr);
  return Imiss_Reason::IMISS_MSHR_HIT;
}

uint64_t get_fdip_ftq_occupancy() {
  return (uint64_t)fdip_ftq_occupancy/cycle_count;
}

static inline void determine_usefulness_by_inf_hash(Addr line_addr, Flag* emit_new_prefetch) {
  if (FDIP_PREF_CONSERVATIVE) {
    auto useful_cl_iter = cnt_useful_ret.find(line_addr);
    if (useful_cl_iter != cnt_useful_ret.end()) {
      STAT_EVENT(fdip_proc_id, FDIP_UTILITY_HASH_HIT);
      *emit_new_prefetch = TRUE;
      DEBUG(fdip_proc_id, "cons : emit a new prefetch for cl 0x%llx", line_addr);
      if (fdip_off_path())
        DEBUG(fdip_proc_id, " OFF path\n");
      else
        DEBUG(fdip_proc_id, " ON path\n");
    } else {
      STAT_EVENT(fdip_proc_id, FDIP_UTILITY_HASH_MISS);
      DEBUG(fdip_proc_id, "cons : do not emit a prefetch for cl 0x%llx\n", line_addr);
      *emit_new_prefetch = FALSE;
      last_cl_unuseful = line_addr;
    }
  } else if (FDIP_PREF_OPTIMISTIC == 1) {
    auto unuseful_cl_iter = cnt_unuseful.find(line_addr);
    if (unuseful_cl_iter != cnt_unuseful.end()) {
      DEBUG(fdip_proc_id, "opt1 : do not emit a prefetch for cl 0x%llx\n", line_addr);
      *emit_new_prefetch = FALSE;
      last_cl_unuseful = line_addr;
    } else {
      *emit_new_prefetch = TRUE;
      DEBUG(fdip_proc_id, "opt1 : emit a new prefetch for cl 0x%llx", line_addr);
    }
  } else if (FDIP_PREF_OPTIMISTIC == 2) {
    auto unuseful_cl_iter = cnt_unuseful.find(line_addr);
    if (unuseful_cl_iter != cnt_unuseful.end()) {
      auto useful_cl_iter = cnt_useful_ret.find(line_addr);
      if (useful_cl_iter != cnt_useful_ret.end() && useful_cl_iter->second > unuseful_cl_iter->second)
        *emit_new_prefetch = TRUE;
      else {
        DEBUG(fdip_proc_id, "opt2 : do not emit a prefetch for cl 0x%llx\n", line_addr);
        *emit_new_prefetch = FALSE;
        last_cl_unuseful = line_addr;
      }
    } else {
      *emit_new_prefetch = TRUE;
      DEBUG(fdip_proc_id, "opt2 : emit a new prefetch for cl 0x%llx", line_addr);
    }
  }
}

static inline void determine_usefulness_by_utility_cache(Addr line_addr, Flag* emit_new_prefetch) {
  Addr uc_line_addr = 0;
  void* useful = (void*)cache_access(&fdip_uc, line_addr, &uc_line_addr, TRUE);
  if (useful) {
    STAT_EVENT(fdip_proc_id, FDIP_UC_HIT);
    *emit_new_prefetch = TRUE;
    DEBUG(fdip_proc_id, "uc : emit a new prefetch for cl 0x%llx, uc_line_addr %llx", line_addr, uc_line_addr);
    if (fdip_off_path())
      DEBUG(fdip_proc_id, " OFF path\n");
    else
      DEBUG(fdip_proc_id, " ON path\n");
  } else {
    STAT_EVENT(fdip_proc_id, FDIP_UC_MISS);
    DEBUG(fdip_proc_id, "uc : do not emit a new prefetch for cl 0x%llx, uc_line_addr %llx", line_addr, uc_line_addr);
    *emit_new_prefetch = FALSE;
    last_cl_unuseful = line_addr;
  }
}

static inline void* bloom_lookup(Addr uc_line_addr) {
  Addr line_addr = uc_line_addr >> 6;
  return (void*)(bloom->contains(line_addr) || bloom2->contains(line_addr >> 1) || bloom4->contains(line_addr >> 2));
}

static inline void insert1(Addr line_addr) {
  STAT_EVENT(fdip_proc_id, FDIP_BLOOM_1INSERT);
  if (!bloom2->contains(line_addr >> 1) && !bloom4->contains(line_addr >> 2)) {
    bloom->insert(line_addr);
  }

}

static inline void insert2(Addr line_addr) {
  if((line_addr & 1) == 0) { //2CL aligned
    if (!bloom4->contains(line_addr >> 2) && !bloom2->contains(line_addr >> 1)) {
      bloom2->insert(line_addr >> 1);
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
  ASSERT(0, (line_addr & 3) == 0);
  bloom4->insert(line_addr >> 2);
  STAT_EVENT(fdip_proc_id, FDIP_BLOOM_4INSERT);
}

static inline void insert_remaining(uint32_t inserted) {
  while (inserted + 4 <= last_prefetch_candidate_counter) {
    insert4(last_prefetch_candidate + inserted);
    inserted += 4;
  }
  if (inserted + 3 == last_prefetch_candidate_counter)
    insert3(last_prefetch_candidate + inserted);
  if (inserted + 2 == last_prefetch_candidate_counter)
    insert2(last_prefetch_candidate + inserted);
  if (inserted + 1 == last_prefetch_candidate_counter)
    insert1(last_prefetch_candidate + inserted);

}

static inline void bloom_insert() {
  uint32_t inserted = 0;
  if (last_prefetch_candidate_counter < 4) {
    insert_remaining(inserted);
    return;
  }
  if ((last_prefetch_candidate & 3) == 0) {
    //4cl aligned
    insert_remaining(inserted);
  }
  else if ((last_prefetch_candidate & 3) == 2) {
    //2cl algned
    insert2(last_prefetch_candidate);
    inserted += 2;
    insert_remaining(inserted);
  }
  else if ((last_prefetch_candidate & 3) == 1) {
    //cl aligned
    //cl aligned
    insert3(last_prefetch_candidate);
    inserted += 3;
    insert_remaining(inserted);
  }
  else if ((last_prefetch_candidate & 3) == 3) {
    //cl aligned
    insert1(last_prefetch_candidate);
    inserted +=1;
    insert_remaining(inserted);
  }
  return;
}

static inline void detect_stream(Addr uc_line_addr) {
  Addr line_addr = uc_line_addr >> 6;

  if(last_prefetch_candidate_counter == 0) {
    last_prefetch_candidate_counter++;
    last_prefetch_candidate = line_addr;
    return;
  }
  STAT_EVENT(fdip_proc_id, FDIP_BLOOM_INSERTED);

  if (line_addr == last_prefetch_candidate + last_prefetch_candidate_counter) {
    last_prefetch_candidate_counter++;
  }
  else {
    bloom_insert();
    last_prefetch_candidate_counter = 1;
    last_prefetch_candidate = line_addr;
  }
}

static inline void determine_usefulness_by_bloom_filter(Addr line_addr, Flag* emit_new_prefetch) {
  void* useful = bloom_lookup(line_addr);
  if (useful) {
    STAT_EVENT(fdip_proc_id, FDIP_BLOOM_HIT);
    *emit_new_prefetch = TRUE;
    DEBUG(fdip_proc_id, "bloom : emit a new prefetch for cl 0x%llx", line_addr);
    if (fdip_off_path())
      DEBUG(fdip_proc_id, " OFF path\n");
    else
      DEBUG(fdip_proc_id, " ON path\n");
  } else {
    STAT_EVENT(fdip_proc_id, FDIP_BLOOM_MISS);
    DEBUG(fdip_proc_id, "bloom : do not emit a new prefetch for cl 0x%llx", line_addr);
    *emit_new_prefetch = FALSE;
    last_cl_unuseful = line_addr;
  }
}

Flag determine_usefulness(Addr line_addr) {
  Flag emit_new_prefetch = FALSE;
  if (last_cl_unuseful != line_addr) {
    if (FDIP_UTILITY_HASH_ENABLE)
      determine_usefulness_by_inf_hash(line_addr, &emit_new_prefetch);
    else if (FDIP_UC_SIZE)
      determine_usefulness_by_utility_cache(line_addr, &emit_new_prefetch);
    else if (FDIP_BLOOM_FILTER)
      determine_usefulness_by_bloom_filter(line_addr, &emit_new_prefetch);
  }
  return emit_new_prefetch;
}

void update_useful_lines(Op* op) {
  if (!FDIP_UTILITY_HASH_ENABLE && !FDIP_UC_SIZE && !FDIP_BLOOM_FILTER)
    return;

  Addr fetch_addr = last_bbl_start_addr;
  Addr useful_cl_addr = last_bbl_start_addr && ~0x3F;
  Addr last_useful_cl_addr = 0;
  while (fetch_addr <= op->inst_info->addr) {
    if (last_useful_cl_addr == useful_cl_addr)
      continue;

    if (FDIP_UTILITY_HASH_ENABLE)
      inc_cnt_useful_ret(useful_cl_addr);
    void* cnt;
    if (FDIP_UC_SIZE) {
      Addr uc_line_addr = 0;
      Addr repl_uc_line_addr = 0;
      cnt = (void*)cache_access(&fdip_uc, useful_cl_addr, &uc_line_addr, TRUE);
      if (!cnt)
        cache_insert_replpos(&fdip_uc, fdip_proc_id, useful_cl_addr, &uc_line_addr,
                    &repl_uc_line_addr, (Cache_Insert_Repl)FDIP_UC_INSERT_REPLPOL, FALSE);
      UNUSED(uc_line_addr);
      if (repl_uc_line_addr)
        STAT_EVENT(fdip_proc_id, FDIP_UC_REPLACEMENT);
    }
    if (FDIP_BLOOM_FILTER) {
      cnt = bloom_lookup(useful_cl_addr);
      if (!cnt)
        detect_stream(useful_cl_addr);
    }

    last_useful_cl_addr = useful_cl_addr;
    useful_cl_addr = ++fetch_addr && ~0x3F;
  }
  last_bbl_start_addr = op->oracle_info.target;
}
