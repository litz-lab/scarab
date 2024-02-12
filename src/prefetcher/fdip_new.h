#ifndef __FDIP_NEW_H__
#define __FDIP_NEW_H__


#ifdef __cplusplus
extern "C" {
#endif

  #include "icache_stage.h"

  void alloc_mem_fdip(uns numProcs);
  void init_fdip(uns proc_id);
  void update_fdip();
  void recover_fdip();
  void set_fdip(int _proc_id, Icache_Stage *_ic);
  Flag fdip_off_path(uns proc_id);
  uns64 fdip_get_ghist();
  uns64 fdip_hash_addr_ghist(uint64_t addr, uint64_t ghist);
  void print_cl_info(uns proc_id);
  void inc_cnt_useful(uns proc_id, Addr line_addr, Flag pref_miss);
  void inc_cnt_unuseful(uns proc_id, Addr line_addr);
  void inc_cnt_useful_signed(uns proc_id, Addr line_addr);
  void dec_cnt_useful_signed(uns proc_id, Addr line_addr);
  void inc_cnt_useful_ret(uns proc_id, Addr line_addr);
  void inc_icache_miss(uns proc_id, Addr line_addr);
  void inc_icache_hit(uns proc_id, Addr line_addr);
  void inc_off_fetched_cls(Addr line_addr);
  void inc_prefetched_cls(Addr line_addr, uns success);
  void not_prefetch(Addr line_addr);
  void probe_prefetched_cls(Addr line_addr);
  void evict_prefetched_cls(uns proc_id, Addr line_addr, Flag by_fdip);
  uns get_miss_reason(uns proc_id, Addr line_addr);
  uns get_last_miss_reason(uns proc_id);
  void set_last_miss_reason(uns proc_id, uns reason);
  uint64_t get_fdip_ftq_occupancy_ops(uns proc_id);
  uint64_t get_fdip_ftq_occupancy(uns proc_id);
  Flag determine_usefulness(Addr line_addr);
  void update_useful_lines(uns proc_id, Op* op);
  void update_useful_lines_uc(uns proc_id, Addr line_addr);
  void update_unuseful_lines_uc(uns proc_id, Addr line_addr);
  void inc_useful_lines_uc(uns proc_id, Addr line_addr);
  void dec_useful_lines_uc(uns proc_id, Addr line_addr);
  void update_useful_lines_bloom_filter(uns proc_id, Addr line_addr);
  void inc_utility_info(uns proc_id, Flag useful);
  void inc_timeliness_info(uns proc_id, Flag mshr_hit);
  void fdip_inc_cnt_btb_miss(uns proc_id);
  Flag fdip_search_pref_candidate(Addr addr);
  void insert_pref_candidate_to_seniority_ftq(Addr line_addr);
  void clear_old_seniority_ftq();
  void assert_fdip_break_reason(uns proc_id, Addr line_addr);
  
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

typedef struct Utility_Timeliness_Info_struct {
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
} Utility_Timeliness_Info;

#endif
