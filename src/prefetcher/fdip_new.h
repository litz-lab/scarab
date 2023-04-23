#ifndef __FDIP_NEW_H__
#define __FDIP_NEW_H__


#ifdef __cplusplus
extern "C" {
#endif

  #include "icache_stage.h"

  void alloc_mem_fdip(uns numProcs);
  void init_fdip(uns proc_id);
  void update_fdip();
  void set_fdip(int _proc_id, Icache_Stage *_ic);
  Flag fdip_off_path(uns proc_id);
  void print_cl_info(uns proc_id);
  void inc_cnt_useful(uns proc_id, Addr line_addr, Flag icache_off_path);
  void inc_cnt_unuseful(uns proc_id, Addr line_addr, Flag icache_off_path);
  void inc_cnt_useful_ret(uns proc_id, Addr line_addr);
  void inc_icache_miss(uns proc_id, Addr line_addr);
  void inc_prefetched_cls(Addr line_addr);
  void probe_prefetched_cls(Addr line_addr);
  void evict_prefetched_cls(uns proc_id, Addr line_addr);
  uns get_miss_reason(uns proc_id, Addr line_addr);
  uns get_last_miss_reason(uns proc_id);
  void set_last_miss_reason(uns proc_id, uns reason);
  uint64_t get_fdip_ftq_occupancy(uns proc_id);
  Flag determine_usefulness(Addr line_addr);
  void update_useful_lines(uns proc_id, Op* op);
  
#ifdef __cplusplus
}
#endif

typedef enum ICACHE_MISS_REASON_enum {
  IMISS_NOT_PREFETCHED,
  IMISS_TOO_EARLY,
  IMISS_MSHR_HIT,
} Imiss_Reason;

typedef enum IC_FETCH_TYPE_enum {
  DEMAND_LOAD,
  FDIP_ONPATH,
  FDIP_OFFPATH,
} IC_Fetch_Type;

#endif
