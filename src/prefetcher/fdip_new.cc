#include "decoupled_frontend.h"
#include "prefetcher/fdip_new.h"

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
// accumulated FTQ occupancy every cycle
uint64_t fdip_ftq_occupancy = 0;

extern Counter last_recover_cycle;

void init_fdip() {
  iter = decoupled_fe_new_ftq_iter();
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

    uint64_t pc_addr = op->inst_info->addr;
    Addr line_addr = op->inst_info->addr && ~0x3F;
    if (line_addr != last_line_addr) {
      STAT_EVENT(ic_ref->proc_id, FDIP_ATTEMPTED_PREF_ONPATH + !op->off_path);
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
      if (!line && !mem_req)
        emit_new_prefetch = TRUE;
      if (line)
        probe_prefetched_cls(line_addr);

      if (emit_new_prefetch && !mem_can_allocate_req_buffer(fdip_proc_id, MRT_FDIPPRF, FALSE)) {
        break_reason = BR_FULL_MEM_REQ_BUF;
        break;
      }
      if (!line) {
        cur_op = op;
        Flag success = new_mem_req(MRT_FDIPPRF, fdip_proc_id, line_addr,
                                   ICACHE_LINE_SIZE, 0, NULL, instr_fill_line, unique_count++, 0);
        // A buffer entry should be available since it is checked by mem_can_allocate_req_buffer for a new prefetch
        ASSERT(fdip_proc_id, success);
        if (success == Mem_Queue_Req_Result::SUCCESS_NEW)
          STAT_EVENT(ic_ref->proc_id, FDIP_NEW_PREFETCHES_OFFPATH + op->off_path);
        else if (success == Mem_Queue_Req_Result::SUCCESS_MERGED)
          STAT_EVENT(ic_ref->proc_id, FDIP_PREF_MSHR_PROBE_HIT_OFFPATH + op->off_path);
        else
          STAT_EVENT(ic_ref->proc_id, FDIP_PREF_ICACHE_PROBE_HIT_OFFPATH + op->off_path);
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
