#ifndef __FDIP_NEW_H__
#define __FDIP_NEW_H__


#ifdef __cplusplus
extern "C" {
#endif

  #include "icache_stage.h"

  void init_fdip();
  void update_fdip();
  void set_fdip(int _proc_id, Icache_Stage *_ic);
  Flag fdip_off_path(void);
  void print_cl_info(void);
  void inc_cnt_useful(Addr line_addr);
  void inc_cnt_unuseful(Addr line_addr);
  void inc_cnt_useful_ret(Addr line_addr);
  void inc_icache_miss(Addr line_addr);
  void inc_prefetched_cls(Addr line_addr);
  void probe_prefetched_cls(Addr line_addr);
  void evict_prefetched_cls(Addr line_addr);
  uns get_miss_reason(Addr line_addr);
  uint64_t get_fdip_ftq_occupancy(void);
  
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
