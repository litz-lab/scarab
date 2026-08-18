/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : op.h
 * Author       : HPS Research Group
 * Date         : 11/11/1997
 * Description  :
 ***************************************************************************************/

#ifndef __OP_H__
#define __OP_H__

#include "globals/assert.h"
#include "globals/enum.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"

#include "dyn_inst.h"
#include "ft_info.h"
#include "inst_info.h"
#include "op_info.h"
#include "pred_info.h"
#include "static_inst_info.h"
#include "table_info.h"

// forward declaration of FT
typedef struct FT FT;

/* All per-op pipeline cycle counters live in one struct on the Op. Each counter
 * is read through op_get_<name>_cycle() and written through op_set_<name>_cycle()
 * (defined below the Op struct). Every counter is reset to a sentinel when the op
 * is allocated (MAX_CTR, except rdy_cycle which starts at 1); except for the
 * rdy_cycle accumulator, each counter is write-once per op and its setter asserts
 * the counter had not been set since allocation. */
typedef struct Op_Cycles_struct {
  Counter fetch_cycle;   // cycle an individual instruction is fetched
  Counter bp_cycle;      // cycle a CF instruction accesses the branch predictor
  Counter map_cycle;     // cycle an individual instruction enters the map stage
  Counter renamed_cycle;  // cycle the op finished reg_file_rename (its SRT effect is applied); MAX_CTR until then
  Counter issue_cycle;   // cycle an individual instruction is issued -- same as chkpt
  Counter rdy_cycle;     // cycle the final source value is available (accumulator: MAX over producers)
  Counter sched_cycle;   // cycle when the op is scheduled (arrives at the functional unit)
  Counter exec_cycle;    // cycle when execution (or addr gen) of op will be completed (result usable)
  Counter dcache_cycle;  // cycle when the op accesses the dcache
  Counter done_cycle;    // cycle when the op is ready to retire
  Counter retire_cycle;  // cycle when the op actually retires
  Counter replay_cycle;  // cycle when the op catches a replay signal
  Counter pred_cycle;
  Counter precommit_cycle;  // cycle when the op is precommit (will eventually retire)
  Counter decode_cycle;     // cycle when decode completes
  Counter wake_cycle;       // cycle a wake up signal is sent to dependents
} Op_Cycles;

/* Register id slots on Op; must match REG_TABLE_REG_ID_INVALID in map_rename.h (0xFFFF). */
#define OP_REG_ID_INVALID ((uns16)0xFFFF)

/**************************************************************************************/
// Macro Defines

/* OP_SRCS_RDY uses op_sources_not_rdy_is_clear (op_info.c). */
#define OP_SRCS_RDY(x) (op_sources_not_rdy_is_clear((x)) && cycle_count >= op_get_rdy_cycle((x)))
#define OP_DONE(x) (cycle_count >= op_get_done_cycle((x)))
#define OP_BROADCAST(x) ((cycle_count + 1) >= op_get_done_cycle((x)))
#define MULTI_CYCLE_OP(x) ((x)->uop->latency > 1 + RFILE_STAGE || (x)->uop->mem_type == MEM_LD)
#define OP_BP_ID(x) ((x)->parent_FT->bp_id)
#define MAX_STRANDS 400
#define MAX_STRAND_BYTES (MAX_STRANDS / 8)
#define STRAND_BYTE(number) (((number) >> 3) % MAX_STRAND_BYTES)
#define STRAND_BIT_IS_SET(array, index) (((array)[STRAND_BYTE((index))] & (1 << ((index) & 7))) != 0)

/* Op_State is the state of the op in the datapath */
// clang-format off
#define OP_STATE_LIST(elem)                                                                 \
    elem(FETCHED)      /* op has been fetched, awaiting issue */                            \
    elem(IN_ROB)       /* op is in the node table (reorder buffer) */                       \
    elem(IN_RS)        /* op is in the scheduling window (RS), waiting for its sources */   \
    elem(SLEEP)        /* for pipelined schedule: wake up NEXT cycle */                     \
    elem(WAIT_FWD)     /* op is waiting for forwarding to happen */                         \
    elem(LOW_PRIORITY) /* op is waiting for forwarding to happen */                         \
    elem(READY)        /* op is ready to fire, awaiting scheduling */                       \
    elem(TENTATIVE)    /* op has been scheduled, but may fail and have to be rescheduled */ \
    elem(SCHEDULED)    /* op has been scheduled and will complete */                        \
    elem(MISS)         /* op has missed in the dcache */                                    \
    elem(WAIT_DCACHE)  /* op is waiting for a dcache port */                                \
    elem(WAIT_MEM)     /* op is waiting for a miss_buffer entry */                          \
    elem(DONE)         /* op is finished executing, awaiting retirement */
DECLARE_ENUM(Op_State, OP_STATE_LIST, OS_);
// clang-format on

/**************************************************************************************/

typedef struct Wake_Up_Entry_struct {
  Op* op;
  Counter unique_num;
  Dep_Type dep_type;
  uns rdy_bit; /* index into dep_op->src_info; may exceed 255 */
  struct Wake_Up_Entry_struct* next;
} Wake_Up_Entry;

// this information is used when the op mispredicts
typedef struct Recovery_Info_struct {  // QUESTION no proc_id?
  uns proc_id;
  uns bp_id;
  uns32 pred_global_hist;                  // the global history used for the prediction
  uns64 conf_perceptron_global_hist;       // Only for confidnece perceptron, a copy of the correct global history
  uns64 conf_perceptron_global_misp_hist;  // Only for confidnece perceptron, a copy of the correct global history
  uns32 targ_hist;                         // a copy of the correct indirect branch pattern history
  Addr npc;

  // next three are used to recover the realistic CRS
  uns crs_tos;
  uns crs_next;
  uns crs_depth;

  Counter op_num;
  Addr tos_addr;  // address on the top of CRS when this op was fetched

  Flag oracle_dir;  // filled by oracle
  Flag new_dir;     // used to repair predictor state (equals oracle_dir by default).

  Cf_Type cf_type;
  Addr PC;
  Op* op;
  Addr branchTarget;
  int64 branch_id;  // set by the branch predictor timestamp_func().
  uns64 predict_cycle;
} Recovery_Info;

typedef struct Dp_Info_struct {
  Flag follows_off_path;                            // op is target of mispredict / redirect
  Flag bogus_result;                                // necessary because state can change from OS_MISS to OS_SCHEDULED
  unsigned char dep_strand_mask[MAX_STRAND_BYTES];  // dependence strand mask.
  Counter preceding_unique_num;                     // unique_num of preceding op in program order.
  Counter strand_number;
} Dp_Info;

/**************************************************************************************/
/* typedef in globals/global_types.h */

struct Op_struct {
  // {{{ op_pool stuff --- don't use outside of op pool management
  Flag op_pool_valid;  // is op allocated from the op_pool?
  Op* op_pool_next;    // either next free or next active op
  uns op_pool_id;      // unique identifier for op (doesn't change)
  // }}}
  // NOTE: op_pool_setup_op zeroes everything after this prefix using
  // offsetof(Op, proc_id). Keep proc_id as the first non-pool field.

  // {{{ op numbers and info pointers
  uns proc_id;                  // processor id for cmp model
  Flag bom;                     // begining of macro instruction when we use op as a uop
  Flag eom;                     // end of macro instruction when we use op as a uop
  Flag fetched_instruction;     // is this op fetched or a rep op?
  Counter op_num;               // op number
  Counter unique_num;           // unique number for each instance of an op (not reset on recovery)
  Counter unique_num_per_proc;  // unique number per core
  uns64 inst_uid;               // unique number for the macro instruction provided by the frontend (PIN)
  Static_Inst_Info* inst;       // shared per-macro-instruction static info
  Static_Op_Info* uop;          // per-uop static info
  Dynamic_Inst* dyn_inst;       // this op's dynamic macro instance (its sibling uop ops)
  Op_Info oracle_info;          // information about the execution of the op in the oracle
  Op_Info engine_info;          // information about the execution of the op in the engine
  uns num_srcs;                 // number of map dependencies (order matches srcs_not_rdy_words / wake-up)
  Src_Info* src_info;           /* grown by map (2 -> 8 -> 128, then x2); freed in free_op */
  uns src_info_cap;
  Bp_Pred_Info bp_pred_l0;       // l0 branch prediction info
  Bp_Pred_Info bp_pred_main;     // main branch prediction info
  Btb_Pred_Info btb_pred;        // btb prediction info
  Bp_Pred_Info* bp_pred_info;    // selected/active branch prediction info
  Btb_Pred_Info* btb_pred_info;  // selected/active btb prediction info
  // }}}

  int32 conf_perceptron_output;  // confidece perceptron
  // {{{ state and event cycle counters
  Op_State state;    // the state of the op in the datapath
  Op_Cycles cycles;  // all per-op pipeline cycle counters (access via op_get/op_set_<name>_cycle)
  // }}}

  // {{{ path and fetch info
  Flag off_path;                // is the op on the correct path of the program? - oracle information
  Flag conf_off_path;           // is the op on the correct path of the program? - confidence information
  Flag exit;                    // is this the last instruction to execute?
  Recovery_Info recovery_info;  // information that will be used to recover a mispredict by the op
  // }}}

  // {{{ scheduler information
  uns fu_num;         // functional unit number the op will or did execute on
  Counter node_id;    // id for position in the node table
  Counter chkpt_num;  // id for chkpt (WARNING: this can change due to recoveries)

  uns16 queue_id;        // id for which issue queue this op is assigned to
  uns16 queue_entry_id;  // id for which entry in the issue queue this op is

  struct Op_struct* next_rdy;     // pointer to next ready op (node table)
  Flag in_rdy_list;               // is the op in the node stage's ready list?
  struct Op_struct* next_node;    // pointer to the next op in the node table
  Flag in_node_list;              // is the op in the node list?
  Flag precommitted;              // if the op is pre-commit in the ROB
  Flag macro_fused;               // if the op should be fused with the previous op (CMP/TEST)
  Flag move_eliminated;           // if the op can be move-eliminated
  Flag load_value_predicted;      // if consumers of the op can be ready before this load
  Flag load_addr_predicted;       // early-AGEN: load may issue w/o addr operands, access load_pred_addr
  Addr load_pred_addr;            // predicted effective addr (early-AGEN/RFP), verified vs va at exec
  Counter load_pred_ready_cycle;  // cycle a predicted load's result is available to consumers (RFP: +DCACHE_CYCLES)
  Counter load_pred_ready_delay;  // produce->availability latency (0 value pred; DCACHE_CYCLES RFP), applied at rename
  Flag replay;                    // is the op waiting to replay?
  uns exec_count;                 // how many times has this op been executed?
  // }}}

  // {{{ dependency information
  uns64* srcs_not_rdy_words; /* ceil(src_info_cap/64) words; bit i == src i not ready */
  uns srcs_not_rdy_nwords;
  Flag wake_up_signaled[NUM_DEP_TYPES];  // set to true once a wake up has been signaled by the op for the given type
  Wake_Up_Entry* wake_up_head;           // list of ops that are dependent on this op, by dependency type
  Wake_Up_Entry* wake_up_tail;           // last entry in each wake up list (for speed)
  uns wake_up_count;                     // count of ops to be awakened by this op (wake up list length)
  // wake_cycle now lives in Op_Cycles (op->cycles.wake_cycle); use op_get/op_set_wake_cycle
  // }}}

  struct Mem_Req_struct* req;  // pointer to memory request responsible for waking up the op

  Flag marked;  // for algorithms that mark already seen ops

  /*------------------------------------------------------------------------------------*/
  // FIELDS BELOW THIS POINT SHOULD BE MOVED INTO OTHER HEADERS
  // (along with any related structs above)

  // Use bp_pred_info->pred_npc instead
  // Addr pred_target; // last predicted target for this op.

  // {{{ temporary fields -> will be deleted later (move these)
  Flag recovery_scheduled;
  Flag redirect_scheduled;
  // }}}

  // {{{ uop cache
  Flag fetched_from_uop_cache;
  // }}}
  int bp_confidence;

  // {{{ source and destination values
  uint64_t src_val[MAX_SRCS];
  uint64_t dst_val[MAX_DESTS];
  // }}}

  // {{{ register renaming
  uns16 src_reg_id[MAX_SRCS][REG_TABLE_TYPE_NUM];        // the reg id of the source reg file entries
  uns16 dst_reg_id[MAX_DESTS][REG_TABLE_TYPE_NUM];       // the reg id of allocated reg file entries
  uns16 prev_dst_reg_id[MAX_DESTS][REG_TABLE_TYPE_NUM];  // the previous dst reg id with the same parent register id
  // }}}
  FT* parent_FT;
  FT* parent_FT_off_path;
};

/* Schedules the exec-time squash for a mispredicted predicted load (defined in
 * load_value_pred.cc; it does bp_sched_recovery, which op.h cannot call directly
 * because bp/bp.h includes op.h). Called from op_set_exec_cycle below. */
#ifdef __cplusplus
extern "C" {
#endif
void predicted_load_schedule_recovery(Op* op, Counter recovery_cycle);
#ifdef __cplusplus
}
#endif

/* Per-op cycle-counter accessors. Each counter has its own get/set function so
 * that per-counter behavior (stats, debug, invariants) can be added in one place.
 * op_set_<name>_cycle() is write-once: it asserts the counter has not been set
 * since the op was allocated (still MAX_CTR). Multiple assignment SITES are fine
 * as long as they are mutually exclusive for a given op. rdy_cycle is the sole
 * exception (an accumulator: MAX over the op's producers) and has no assert. */

// Reach this op's sibling dynamic uop ops via its Dynamic_Inst (index 0 == bom, num_uops-1 == eom).
static inline uns op_inst_num_uops(const Op* op) {
  return op->inst->num_uop;
}
static inline Op* op_inst_uop(const Op* op, uns i) {
  ASSERT(op->proc_id, op->dyn_inst && i < op->inst->num_uop);
  return op->dyn_inst->uops[i];
}
static inline Op* op_inst_eom(const Op* op) {
  ASSERT(op->proc_id, op->dyn_inst);
  return op->dyn_inst->uops[op->inst->num_uop - 1];
}
// Recovery stage for this macro (RECOVER_AT_NONE if none). Invariant: at most one uop is marked -- asserts otherwise.
static inline Recovery_Point op_inst_recovery_point(const Op* op) {
  Recovery_Point rp = RECOVER_AT_NONE;
  for (uns i = 0; i < op_inst_num_uops(op); i++) {
    const Op* u = op_inst_uop(op, i);
    if (u->bp_pred_info && u->bp_pred_info->recovery_point != RECOVER_AT_NONE) {
      ASSERT(op->proc_id, rp == RECOVER_AT_NONE);
      rp = u->bp_pred_info->recovery_point;
    }
  }
  return rp;
}

static inline Counter op_get_fetch_cycle(const Op* op) {
  return op->cycles.fetch_cycle;
}
static inline void op_set_fetch_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.fetch_cycle == MAX_CTR);
  op->cycles.fetch_cycle = cycle;
}

static inline Counter op_get_bp_cycle(const Op* op) {
  return op->cycles.bp_cycle;
}
static inline void op_set_bp_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.bp_cycle == MAX_CTR);
  op->cycles.bp_cycle = cycle;
}

static inline Counter op_get_map_cycle(const Op* op) {
  return op->cycles.map_cycle;
}
static inline void op_set_map_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.map_cycle == MAX_CTR);
  op->cycles.map_cycle = cycle;
}
static inline Counter op_get_renamed_cycle(const Op* op) {
  return op->cycles.renamed_cycle;
}
static inline void op_set_renamed_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.renamed_cycle == MAX_CTR);
  op->cycles.renamed_cycle = cycle;
}

static inline Counter op_get_issue_cycle(const Op* op) {
  return op->cycles.issue_cycle;
}
static inline void op_set_issue_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.issue_cycle == MAX_CTR);
  op->cycles.issue_cycle = cycle;
}

static inline Counter op_get_sched_cycle(const Op* op) {
  return op->cycles.sched_cycle;
}
static inline void op_set_sched_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.sched_cycle == MAX_CTR);
  op->cycles.sched_cycle = cycle;
}

static inline Counter op_get_exec_cycle(const Op* op) {
  return op->cycles.exec_cycle;
}
static inline void op_set_exec_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.exec_cycle == MAX_CTR);
  op->cycles.exec_cycle = cycle;
  // value/RFP-pred load only: recover at exec if the eom has renamed, else defer to RECOVER_AT_RENAME.
  if (!op->bp_pred_info || op->bp_pred_info->recovery_point != RECOVER_AT_EXEC || op->uop->cf_type != NOT_CF ||
      op->load_addr_predicted)
    return;
  if (op_get_renamed_cycle(op_inst_eom(op)) != MAX_CTR)
    predicted_load_schedule_recovery(op, op_get_exec_cycle(op));
  else
    op->bp_pred_info->recovery_point = RECOVER_AT_RENAME;
}

static inline Counter op_get_dcache_cycle(const Op* op) {
  return op->cycles.dcache_cycle;
}
static inline void op_set_dcache_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.dcache_cycle == MAX_CTR);
  op->cycles.dcache_cycle = cycle;
}

static inline Counter op_get_done_cycle(const Op* op) {
  return op->cycles.done_cycle;
}
static inline void op_set_done_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.done_cycle == MAX_CTR);
  op->cycles.done_cycle = cycle;
}

static inline Counter op_get_retire_cycle(const Op* op) {
  return op->cycles.retire_cycle;
}
static inline void op_set_retire_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.retire_cycle == MAX_CTR);
  op->cycles.retire_cycle = cycle;
}

static inline Counter op_get_replay_cycle(const Op* op) {
  return op->cycles.replay_cycle;
}
static inline void op_set_replay_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.replay_cycle == MAX_CTR);
  op->cycles.replay_cycle = cycle;
}

static inline Counter op_get_pred_cycle(const Op* op) {
  return op->cycles.pred_cycle;
}
static inline void op_set_pred_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.pred_cycle == MAX_CTR);
  op->cycles.pred_cycle = cycle;
}

static inline Counter op_get_precommit_cycle(const Op* op) {
  return op->cycles.precommit_cycle;
}
static inline void op_set_precommit_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.precommit_cycle == MAX_CTR);
  op->cycles.precommit_cycle = cycle;
}

static inline Counter op_get_decode_cycle(const Op* op) {
  return op->cycles.decode_cycle;
}
static inline void op_set_decode_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.decode_cycle == MAX_CTR);
  op->cycles.decode_cycle = cycle;
}

static inline Counter op_get_wake_cycle(const Op* op) {
  return op->cycles.wake_cycle;
}
static inline void op_set_wake_cycle(Op* op, Counter cycle) {
  ASSERT(op->proc_id, op->cycles.wake_cycle == MAX_CTR);
  op->cycles.wake_cycle = cycle;
}

/* rdy_cycle is an accumulator (MAX over the op's producers), so it is intentionally
 * NOT write-once and has no assert. */
static inline Counter op_get_rdy_cycle(const Op* op) {
  return op->cycles.rdy_cycle;
}
static inline void op_set_rdy_cycle(Op* op, Counter cycle) {
  op->cycles.rdy_cycle = cycle;
}

/* At retirement every on-path op has passed through each pipeline stage, so its
 * cycle counters must all be set (!= MAX_CTR). Genuinely conditional counters are
 * handled specially:
 *   - bp_cycle    : only control-flow ops access the branch predictor;
 *   - dcache_cycle: only memory ops access the dcache;
 * and these are intentionally NOT checked (set only in specific situations):
 *   - replay_cycle: set only when the op actually replayed;
 *   - pred_cycle  : stamped only by predictors that use it (e.g. hybridgp);
 *   - retire_cycle: set at retirement itself, after this check runs.
 * rdy_cycle defaults to 1 for born-ready ops, so it is always set. */
static inline void op_assert_cycles_set_at_retire(const Op* op) {
  ASSERT(op->proc_id, op->cycles.fetch_cycle != MAX_CTR);
  ASSERT(op->proc_id, op->cycles.map_cycle != MAX_CTR);
  ASSERT(op->proc_id, op->cycles.issue_cycle != MAX_CTR);
  ASSERT(op->proc_id, op->cycles.rdy_cycle != MAX_CTR);
  ASSERT(op->proc_id, op->cycles.sched_cycle != MAX_CTR);
  ASSERT(op->proc_id, op->cycles.exec_cycle != MAX_CTR);
  ASSERT(op->proc_id, op->cycles.done_cycle != MAX_CTR);
  ASSERT(op->proc_id, op->cycles.precommit_cycle != MAX_CTR);
  ASSERT(op->proc_id, op->cycles.decode_cycle != MAX_CTR);
  ASSERT(op->proc_id, op->cycles.wake_cycle != MAX_CTR);
  // Syscalls and fetch-barrier CF ops are serializing: the frontend treats them as
  // fetch barriers (predict_op_ft_event returns FETCH_BARRIER) rather than predicted
  // branches, so they never stamp bp_cycle. Require it only for predicted CF ops.
  if (op->uop->cf_type && op->uop->cf_type != CF_SYS && !(op->uop->bar_type & BAR_FETCH))
    ASSERT(op->proc_id, op->cycles.bp_cycle != MAX_CTR);
  if (op->uop->mem_type != NOT_MEM)
    ASSERT(op->proc_id, op->cycles.dcache_cycle != MAX_CTR);
}

static inline void op_select_bp_pred_info(Op* op, Bp_Pred_Level level) {
  op->bp_pred_info = (level == BP_PRED_L0) ? &op->bp_pred_l0 : &op->bp_pred_main;
  // btb_pred_info is set exclusively by bp_predict_btb(); do not touch it here.
}

/**************************************************************************************/

#endif  // #ifndef __OP_H__
