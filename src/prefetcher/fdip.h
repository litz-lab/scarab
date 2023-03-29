#ifndef __FDIP_H_
#define __FDIP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "op.h"
#include "bp/bp.h"
#include "icache_stage.h"
#include "globals/global_types.h"

  /*************Interface to Scarab***************/
  void fdip_init(Bp_Data* _bp_data,  Icache_Stage *_ic);
  Addr fdip_pred(Addr bp_pc, Op *op);
  void fdip_retire(Op *op);
  void fdip_resolve(Op *op);
  void fdip_recover(Recovery_Info *info);
  void fdip_redirect(Addr recover_pc);
  void fdip_reset_on_path(Addr next_fetch_addr);
  void fdip_update();
  Flag fdip_pred_off_path(void);
  Flag fdip_pref_off_path(void);
  void fdip_inc_cnt_useful(Addr line_addr);
  void fdip_inc_cnt_unuseful(Addr line_addr);
  void fdip_insert_cl_fetch_addr(Addr line_addr);
  void fdip_remove_cl_fetch_addr(Addr line_addr);
  void fdip_print_hash_tables();
  void fdip_touch_cl_candidates(Addr line_addr);
  void fdip_inc_useful_hash(Addr line_addr);
  void fdip_dec_useful_hash(Addr line_addr);
  void fdip_inc_icache_miss(Addr line_addr);
  void fdip_inc_prefetched_cls(Addr line_addr);
  void fdip_touch_prefetched_cls(Addr line_addr);
  void fdip_evict_prefetched_cls(Addr line_addr);
  uns fdip_get_miss_reason(Addr line_addr);
  void fdip_inc_all_useful_lines(Addr line_addr);
  Flag determine_by_usefulness(Addr line_addr);
  Flag can_fetch_op_from_ftq(Op* op);
  int get_avg_ftq_occupancy();
  int get_avg_ftq_occupancy_on_path();
  int get_avg_ftq_occupancy_off_path();
  int get_avg_resteer_interval();
  int get_avg_ftq_entries_reset();
  int get_avg_pref_bw_resteer();

  /* Private*/
  void fdip_new_branch(Addr bp_pc, Op *op);
  void fdip_clear_ftq();

#ifdef __cplusplus
}
#endif

#endif
