#ifndef __FDIP_NEW_H__
#define __FDIP_NEW_H__


#ifdef __cplusplus
extern "C" {
#endif

  #include "icache_stage.h"

  void init_fdip();
  void update_fdip();
  void set_fdip(int _proc_id, Icache_Stage *_ic);
  
#ifdef __cplusplus
}
#endif


#endif
