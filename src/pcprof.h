/***************************************************************************************
 * File         : pcprof.h
 * Description  : AsmDB-style per-static-instruction (per-PC) attribution, enabled with
 *                --pc_profile 1. Accumulates over the measured region only (after
 *                full_warmup) and writes <output_dir>/pcprof.csv and pcprof_summary.txt.
 *                With the flag off every hook returns immediately and nothing the
 *                timing model reads is touched.
 ***************************************************************************************/

#ifndef __PCPROF_H__
#define __PCPROF_H__

#include "globals/global_types.h"

#include "general.param.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Mem_Req_struct;

/* per-op ROB-head stall categories (Op::pcprof_stall) */
typedef enum Pcprof_Op_Cat_enum {
  PCPROF_OPC_BMISP,  // head is an on-path branch that will recover at exec (or has scheduled a recovery)
  PCPROF_OPC_MEM,    // head is a memory op that has accessed the dcache and is not done
  PCPROF_OPC_EXEC,   // head waits for operands or execution latency
  PCPROF_OPC_OTHER,  // anything else (redirect pending, ...)
} Pcprof_Op_Cat;

void pcprof_retire(Op* op);                                                   // node_retire, every retired op
void pcprof_node_cycle(void);                                                 // after node_retire, every node cycle
void pcprof_rob_insert(Op* op);                                               // node_fill_rob, every inserted op
void pcprof_recovery(Op* op);                                                 // cmp_recover, before any flush
void pcprof_fetch(Addr fetch_addr, Flag off_path, Flag uc_hit, Flag ic_hit);  // ft_arbitration
void pcprof_dcache_fill(Op* op, struct Mem_Req_struct* req);                  // dcache fill wakes a missing op
void pcprof_dump(uns proc_id);                                                // end of simulation

/* frontend phase events, keyed by the inst_uid of the first on-path instruction they
   precede (syscall marker) or of the marker instruction itself (roi begin/end) */
typedef enum Pcprof_Event_enum {
  PCPROF_EV_SYSCALL,     // value = syscall number (TRACE_MARKER_TYPE_SYSCALL)
  PCPROF_EV_MARK_BEGIN,  // XCHG rbx,rbx (scarab_marker_roi_begin)
  PCPROF_EV_MARK_END,    // XCHG rdx,rdx (scarab_marker_roi_end)
} Pcprof_Event;
void pcprof_fe_event(uns proc_id, uns64 inst_uid, int type, int64 value);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __PCPROF_H__ */
