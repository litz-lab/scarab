#ifndef __SYNTH_FE_H__
#define __SYNTH_FE_H__
#include "globals/global_types.h"
#ifdef __cplusplus
extern "C" {
#endif
struct Op_struct;

void synth_init();
void synth_done();

/*Front End Interface*/
Addr synth_next_fetch_addr(uns proc_id);
Flag synth_can_fetch_op(uns proc_id, uns bp_id);
void synth_fetch_op(uns proc_id, uns bp_id, struct Op_struct* op);
void synth_redirect(uns proc_id, uns bp_id, uns64 inst_uid, Addr fetch_addr);
void synth_recover(uns proc_id, uns bp_id, uns64 inst_uid);
void synth_retire(uns proc_id, uns64 inst_uid);

#ifdef __cplusplus
}
#endif

#endif