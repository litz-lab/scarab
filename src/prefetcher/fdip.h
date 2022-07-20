#ifndef __FDIP_H_
#define __FDIP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "op.h"
#include "bp/bp.h"
#include "icache_stage.h"
#include "globals/global_types.h"
#include "globals/count_min_sketch.h"

  /*************Interface to Scarab***************/
  void fdip_init(Bp_Data* _bp_data,  Icache_Stage *_ic);
  Addr fdip_pred(Addr bp_pc, Op *op);
  void fdip_retire(Op *op);
  void fdip_resolve(Op *op);
  void fdip_recover(Recovery_Info *info);
  void fdip_redirect(Addr recover_pc);
  void fdip_update();
  Flag fdip_pref_off_path(void);
  Flag fdip_is_max_op(Op *op);

  /* Private*/
  void fdip_new_branch(Addr bp_pc, Op *op);
  void fdip_clear_ftq();

#ifdef __cplusplus
}
#endif

#endif
