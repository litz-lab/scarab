#include "prefetcher/fdip.h"

#include <unordered_map>
#include <queue>
#include <algorithm>
#include <iostream>

extern "C" {
#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "core.param.h"
#include "bp/bp.param.h"
#include "globals/assert.h"
#include "statistics.h"
#include "prefetcher/pref.param.h"
#include "memory/memory.param.h"
#include "memory/memory.h"
#include "uop_cache.h"
}

#define DUMMY_CFN ~0
struct ftq_req {
  Addr target;
  Counter prefetch_cycle;
  Op op;
  bool prefetched;
  bool taken;

  ftq_req(Addr _target, Counter _prefetch_cycle, Op _op, bool _prefetched,
    bool _taken)
  : target(_target),
    prefetch_cycle(_prefetch_cycle), op(_op), prefetched(_prefetched),
    taken(_taken) {};
};

std::unordered_map<Addr, Op> pc_to_op;
std::deque<std::pair<Addr, ftq_req>> ftq;
Addr runahead_pc;
Addr last_cl_prefetched = 0;
Icache_Stage *ic_stage;
Bp_Data *bp_data;
uns cf_num    = 0;
Flag runahead_disable = TRUE;
Op *recovery_checkpoint;
Flag recovery_checkpoint_valid = FALSE;
Flag fdip_on_path_bp = TRUE;
Flag fdip_on_path_pref = TRUE;
uns64 last_runahead_uid = 0; // uid of the branch that FDIP predicted most recently
uns64 max_runahead_uid = 0;
Counter last_runahead_op = 0;
Counter max_runahead_op = 0;
extern List op_buf;
Hash_Table top_mispred_br;
uns64 recovery_count = 0;
uns64 off_count = 0;
uns64 outstanding_prefs = 0;
// TODO: one of 1-counter mode and 2-counter mode will be deprecated after verifying with unlimited TAGE buffer. 2-counter mode is commented out in current version.
//uns64 outstanding_prefs_on_path = 0;
//uns64 outstanding_prefs_off_path = 0;
Flag mem_req_failed = FALSE;
Counter max_op_num = 0;
Counter last_recover_cycle = 0;

/**************************************************************************************/
/* init_topk_mispred: list of branches that provide a given coverage of resteers */
void init_topk_mispred() {
    init_hash_table(&top_mispred_br, "top mispredicting branches", 100000, sizeof(int));
    FILE* fp = fopen(TOP_MISPRED_BR_FILEPATH, "r");
    const int line_len = 256;
    char line[line_len];
    char* field;
    float coverage;
    Addr br;
    char* val = fgets(line, line_len, fp); // skip first line
    (void)val;
    while (fgets(line, line_len, fp)) {
      field = strtok(line, ",");
      field = strtok(NULL, ",");
      coverage = atof(field);
      field = strtok(NULL, ",");
      br = strtoull(field, NULL, 16);
      // add to map
      Flag new_entry;
      hash_table_access_create(&top_mispred_br, br, &new_entry);
      ASSERT(0, new_entry);
      STAT_EVENT(0, TOP_MISPRED_BR);
      if (coverage >= TOP_MISPRED_BR_RESTEER_COVERAGE)
        break;
    }
}

void fdip_init(Bp_Data* _bp_data,  Icache_Stage *_ic) {
  ASSERT(ic_stage->proc_id, !(FDIP_ENABLE && USE_LATE_BP));
  if (!FETCH_BREAK_ON_TAKEN) { // icache_stage does not break on taken branches
    ASSERT(ic_stage->proc_id, FDIP_MAX_TAKEN_BRANCHES >= ISSUE_WIDTH);
    ASSERT(ic_stage->proc_id, FDIP_MAX_OUTSTANDING_PREFETCHES >= ISSUE_WIDTH);
  }
  bp_data = _bp_data;
  ic_stage = _ic;
  ASSERT(ic_stage->proc_id, ic_stage);
  if ((FDIP_DUAL_PATH_PREF_IC_ENABLE || FDIP_DUAL_PATH_PREF_UOC_ENABLE)
        && TOP_MISPRED_BR_RESTEER_COVERAGE)
    init_topk_mispred();
  // Only one of these prefetching options should be set.
  ASSERT(ic_stage->proc_id, UOC_ORACLE_PREF + UOC_PREF + FDIP_DUAL_PATH_PREF_UOC_ENABLE <= 1);
}

/* Called when a branch completes in the functional unit */
void fdip_resolve(Op *op) {
}

/* Called when a branch retires. If the branch retires that represents the
 * recovery checkpoint, the checkpoint is no longer valid. At this point we
 * do not have a valid recovery checkpoint. */
void fdip_retire(Op *op) {
  if (recovery_checkpoint_valid &&
    op->recovery_info.branch_id ==
    recovery_checkpoint->recovery_info.branch_id) {
    recovery_checkpoint_valid = FALSE;
  }
}

/* When the branch is initially predicted by FDIP, a placeholder op struct is
 * used to store the predicted branch outcome, target, etc. Need to copy this
 * data into the current op
 */
void patch_oracle_info(Op *op, Op *req, Addr bp_pc) {
  //Copy placeholder recovery info into current op
  op->recovery_info = req->recovery_info;
  //Update dynamic fields of the current op that were unavailable for FDIP
  op->recovery_info.new_dir = op->oracle_info.dir;
  op->recovery_info.op_num = op->op_num;
  op->recovery_info.PC = op->inst_info->addr;
  op->recovery_info.oracle_dir = op->oracle_info.dir;
  op->recovery_info.branchTarget = op->oracle_info.target;
}

/* Called when the frontend consumes the next prediction. There exist three
 * cases. a) FDIP has not predicted a branch yet after a recovery. 
 * b) The FDIP predicted branch matches the branch provided by the
 * frontend - consume the prediction. c) FDIP was on the wrong path and
 * predicted a branch that is never actually processed by the frontend
 */ 
Addr fdip_pred(Addr bp_pc, Op *op) {
  ASSERT(ic_stage->proc_id, !ftq.empty());
  ASSERT(ic_stage->proc_id, ftq.front().first == op->fetch_addr);
  /* FDIP predicted a branch on the right path. Consume the target and
   * patch the current instruction.
   */
  STAT_EVENT(ic_stage->proc_id, FDIP_PRED_CORRECT_PATH);
  auto req = &ftq.front().second;
  auto target = req->target;
  patch_oracle_info(op, &req->op, bp_pc);
  //Re-evaluate FDIP direction prediction based on the current oracle info
  op->cf_within_fetch = cf_num++;
  if (!fdip_on_path_bp) {
    //We may have mispredicted once but the branch PCs seen by the
    //frontend and FDIP still match
    recovery_checkpoint = op;
    recovery_checkpoint->recovery_info.npc = op->oracle_info.npc;
    recovery_checkpoint_valid = TRUE;
  }
  /* A branch processed by FDIP has been mispredicted. We do not recover
   * at this point as we want FDIP to continue prefetching on the wrong
   * path. Recover only if this branch is resolved by the backend.
   */
  ASSERT(ic_stage->proc_id, req->taken == req->op.oracle_info.pred);
  if (op->oracle_info.mispred || op->oracle_info.misfetch) {
    ASSERT(ic_stage->proc_id, op->oracle_info.mispred ||
        op->oracle_info.misfetch);
  }
  else {
    ASSERT(ic_stage->proc_id, !op->oracle_info.mispred &&
        !op->oracle_info.misfetch);
  }
  if (req->prefetched) {
    STAT_EVENT(ic_stage->proc_id, FDIP_PREF_CORRECT_PATH);
    INC_STAT_EVENT(ic_stage->proc_id, FDIP_SAVED_PREF_CYC,
        cycle_count - req->prefetch_cycle);
  }
  ftq.pop_front();
  return target;
}

/* To enable off-path runahead prefetching, maintain a data structure of all branches
 * that have been seen so far. This allows us to predict branches without
 * receiving them from the simulator frontend.
 */
void fdip_new_branch(Addr bp_pc, Op *op) {
  if(pc_to_op.find(bp_pc) == pc_to_op.end()) {
    pc_to_op.insert(std::pair<Addr, Op>(bp_pc, *op));
  }
}

/* Clear the FTQ, branch predictor state needs to be recovered elsewhere
 */
void fdip_clear_ftq() {
  ASSERT(ic_stage->proc_id, ftq.empty());
  last_cl_prefetched = 0;
}

/* When a mispredicted branch is resolved, the frontend recovers the branch
 * predictor state (including FDIP predictions), hence here we only have to 
 * clear the FTQ.
 */
void fdip_recover(Recovery_Info *info) {
  recovery_count++;
  ASSERT(ic_stage->proc_id, off_count == recovery_count);
  ASSERT(ic_stage->proc_id, !PERFECT_NT_BTB || ftq.empty());
  fdip_clear_ftq();
  if (last_runahead_uid != max_runahead_uid)
    last_runahead_uid = 0;
  if (last_runahead_op != max_runahead_op)
    last_runahead_op = 0;
  runahead_pc = info->npc;
  runahead_disable = FALSE;
  fdip_on_path_bp = TRUE;
  fdip_on_path_pref = TRUE;
  if (UOC_ORACLE_PREF) {
    uop_cache_issue_prefetch(info->npc, fdip_on_path_pref);
  }
  last_recover_cycle = cycle_count;
  //outstanding_prefs_off_path = 0;
  fdip_update();
}

/* When a misfetch (btb miss) is resolved, the frontend informs FDIP to clear FDIP branch predictor states including the FTQ.
 */
void fdip_redirect(Addr recover_pc) {
  bp_recover_op(g_bp_data, recovery_checkpoint->table_info->cf_type, &recovery_checkpoint->recovery_info);
}

// Returns true if prefetch was emitted
Flag fdip_prefetch(Addr target, Op *op) {
  static Addr last_line_addr_prefetched = 0;
  Addr line_addr;
  Flag success = FAILED;
  void* line = NULL;

  if(!FDIP_ALWAYS_PREFETCH)
    line = (Inst_Info**)cache_access(&ic_stage->icache, target,
                              &line_addr, TRUE);
  if (fdip_on_path_pref) {
    STAT_EVENT(ic_stage->proc_id, FDIP_ATTEMPTED_PREF_ON_PATH);
  } else {
    STAT_EVENT(ic_stage->proc_id, FDIP_ATTEMPTED_PREF_OFF_PATH);
  }

  if(FDIP_ALWAYS_PREFETCH || (!line && (last_line_addr_prefetched != line_addr))) {
    if(FDIP_PREF_NO_LATENCY) {
      Mem_Req req;
      req.off_path             = op ? op->off_path : FALSE;
      req.off_path_confirmed   = FALSE;
      req.type                 = MRT_FDIPPRF;
      req.proc_id              = ic_stage->proc_id;
      req.addr                 = line_addr;
      req.oldest_op_unique_num = (Counter)0;
      req.oldest_op_op_num     = (Counter)0;
      req.oldest_op_addr       = (Addr)0;
      req.dirty_l0             = op && op->table_info->mem_type == MEM_ST && !op->off_path;
      if(icache_fill_line(&req)) {
        STAT_EVENT(ic_stage->proc_id, FDIP_PREFETCHES);
        success = SUCCESS_NEW;
      }
    } else {
      success = new_mem_req(MRT_FDIPPRF, ic_stage->proc_id, line_addr,
                    ICACHE_LINE_SIZE, 0, NULL, instr_fill_line, unique_count, 0);
      if (success) {
        STAT_EVENT(ic_stage->proc_id, FDIP_PREFETCHES);
      } else {
        //mem_req_failed = TRUE;
        ASSERT(ic_stage->proc_id, false);
      }
    }
  }
  else {
    STAT_EVENT(ic_stage->proc_id, FDIP_PREF_ICACHE_HIT);
  }
  last_line_addr_prefetched = line_addr;
  return success;
}

int fdip_dual_path_prefetch(Addr target, Op* op) {
  int icache_pref = 0;
  int uoc_pref = 0;
  ASSERT(ic->proc_id, FDIP_DUAL_PATH_PREF_IC_ENABLE 
                  || FDIP_DUAL_PATH_PREF_UOC_ENABLE);

  if (FDIP_DUAL_PATH_PREF_IC_ENABLE) {
    // is prefetching one CL for the alt path enough?
    // pred_target is the target stored by the BTB used if predicted taken
    Flag success = fdip_prefetch(op->pred_target, op);
    if (success) {
      icache_pref += 1;
      ftq.back().second.prefetched = true;
    }
    success = fdip_prefetch(op->inst_info->addr + ICACHE_LINE_SIZE, op);
    if (success)
      icache_pref += 1;
    STAT_EVENT(ic_stage->proc_id, FDIP_ALT_PATH_PREFETCHES_IC_TRIGGERED);
    INC_STAT_EVENT(ic_stage->proc_id, FDIP_ALT_PATH_PREFETCHES_IC_EMITTED, 
                    icache_pref);
  }
  if (FDIP_DUAL_PATH_PREF_UOC_ENABLE) {
    uoc_pref += uop_cache_issue_prefetch(op->pred_target, fdip_on_path_pref && op->oracle_info.pred);
    uoc_pref += uop_cache_issue_prefetch(op->pc_plus_offset, fdip_on_path_pref && !op->oracle_info.pred);
    STAT_EVENT(ic_stage->proc_id, FDIP_ALT_PATH_PREFETCHES_UOC_TRIGGERED);
    INC_STAT_EVENT(ic_stage->proc_id, FDIP_ALT_PATH_PREFETCHES_UOC_EMITTED,
                    uoc_pref);
  }
  return icache_pref;
}

// Called each cycle to trigger runahead prefetches
void fdip_update() {
  uns64 orig_last_runahead_uid = last_runahead_uid;
  uint32_t taken_branches = 0;
  Flag break_on_tage_limit = FALSE;

  //if (ftq.size() > FDIP_MAX_RUNAHEAD || runahead_disable) {
  if (runahead_disable) {
    return;
  }

  // Predict branches across cache lines. In many implementations, the BTB
  // is looked up once per cycle, returning all branches in the cache line
  mem_req_failed = FALSE;

  // Find the next branch after the runahead PC. As the BTB/BP is addressed
  // with a PC and not cache line address we need to byte-wise increment the
  // runahead PC
  /*
   * (1) FDIP_PERFECT_RUNAHEAD : FDIP runs ahead enough (ISSUE_WIDTH) regardless of the number of taken branches.
   * (2) FDIP_MAX_TAKEN_BRANCHES : 2 by default
   * (3) last_runahead_op != max_runahead_op : necessary to prevent FDIP from trying to run ahead when there are not available decoded ops.
   *     set by find_op() when the next branch is not found
   * (4) outstanding_prefs < FDIP_MAX_OUTSTANDING_PREFETCHES : outstanding_prefs is the number of outstanding prefetches not per cycle.
   *     It is incremented by 1 whenever FDIP emits a cache line prefetch and decremented by 1 whenever the backend consumes a branch from FTQ.
   *     It is reset by 0 whenever FTQ is cleared and the backend hits an FTQ-empty case.
   * (5) bp_is_predictable() : necessary to ensure that the TAGE buffer doesn't become full.
   * (6) !mem_req_failed : necessary for FDIP to break out from the while loop when L1 queue is full.
   *     Even with outstanding_pref = 1, the queue becomes full because the backend consumes a branch from FTQ and decreases the count (outstanding_prefs), but it is possible that the requested cache line has not yet loaded due to the latency.
   *     In this case, FDIP runs ahead again and continuously increases the number of memory requests although all the previous requests have not yet been handled.
   */
  while ((FDIP_PERFECT_RUNAHEAD && !mem_req_failed &&
                ((!fdip_on_path_bp && (outstanding_prefs < FDIP_MAX_OUTSTANDING_PREFETCHES)) ||
                //((!fdip_on_path_bp && (outstanding_prefs_on_path + outstanding_prefs_off_path < FDIP_MAX_OUTSTANDING_PREFETCHES)) ||
                 (fdip_on_path_bp && (last_runahead_uid < orig_last_runahead_uid + ISSUE_WIDTH) && (last_runahead_uid != max_runahead_uid)))) ||
      ((!FDIP_PERFECT_RUNAHEAD &&
                (taken_branches < FDIP_MAX_TAKEN_BRANCHES) &&
                //(last_runahead_uid != max_runahead_uid) &&
                (last_runahead_op != max_runahead_op) &&
                (outstanding_prefs < FDIP_MAX_OUTSTANDING_PREFETCHES) &&
                //(outstanding_prefs_on_path + outstanding_prefs_off_path < FDIP_MAX_OUTSTANDING_PREFETCHES) &&
                //(count_fdip_mem_l1_reqs() < FDIP_MAX_OUTSTANDING_PREFETCHES) && // the actual FTQ size
                !mem_req_failed))) {

    bool btb_ras_miss = false;
    bool ftq_pushed = false;

    Op* op = NULL;
    bool is_branch = false;
    Addr target = 0;

    if (!mem_can_allocate_req_buffer(ic_stage->proc_id, MRT_FDIPPRF, FALSE)) {
      mem_req_failed = TRUE;
      break;
    }
    // prediction
    if (fdip_on_path_bp) {
      op = find_op(runahead_pc);
      is_branch = (op && op->table_info->cf_type)? true : false;
      // need to break without updating runahead_pc
      if (!is_branch && last_runahead_op == max_runahead_op)
      {
        break;
      }
      if (is_branch) {
        if (!bp_is_predictable(g_bp_data, op)) {
          break_on_tage_limit = TRUE;
          move_to_prev_op();
          break;
        }
        fdip_new_branch(runahead_pc, op);
        ASSERT(ic_stage->proc_id, op->fetch_addr == runahead_pc);
        target = bp_predict_op(g_bp_data, op, DUMMY_CFN, runahead_pc);
        ftq.push_back(std::pair<Addr, ftq_req>(runahead_pc, ftq_req(
                                    target, cycle_count, *op,
                                    0, /*prefetched*/
                                    op->oracle_info.pred == TAKEN
                                    )));
        ftq_pushed = true;
        // No matter how branch is predicted, prefetch the correct next PW.
        // Only predict branches that are found when on the on-path.
        if (UOC_ORACLE_PREF) {
          uop_cache_issue_prefetch(op->oracle_info.npc, true); // Does this hit every time after recovery? If so then the prefetch at fdip_recover is redundant
        }
        STAT_EVENT(ic_stage->proc_id, FDIP_PRED_ON_PATH);
        Flag bf = op->table_info->bar_type & BAR_FETCH ? TRUE : FALSE;
        btb_ras_miss = (op->oracle_info.btb_miss && op->oracle_info.pred) || (op->oracle_info.btb_miss && !op->oracle_info.pred && !bf) || target == 0;
        if (op->oracle_info.pred)
          taken_branches++;
        if (op->oracle_info.mispred || op->oracle_info.misfetch) {
          off_count++;
          fdip_on_path_bp = FALSE;
          if ((FDIP_STOP_ON_MISPRED && op->oracle_info.mispred) || (FDIP_STOP_ON_MISFETCH && op->oracle_info.misfetch)) {
            runahead_disable = TRUE;
          }
        } else if (btb_ras_miss) {
          ASSERT(ic_stage->proc_id, !op->oracle_info.mispred && !op->oracle_info.misfetch);
          off_count++;
          fdip_on_path_bp = FALSE;
          if (FDIP_STOP_ON_BTB_MISS) {
            runahead_disable = TRUE;
          }
        }
      }
    } else {
      auto op_iter = pc_to_op.find(runahead_pc);
      is_branch = op_iter != pc_to_op.end();
      if (is_branch) {
        op = &op_iter->second;
        if (!bp_is_predictable(g_bp_data, op)) {
          break_on_tage_limit = TRUE;
          break;
        }

        ASSERT(ic_stage->proc_id, op->fetch_addr == runahead_pc);
        Addr* btb_target = g_bp_data->bp_btb->pred_func(g_bp_data, op);
        if (btb_target) {
          target = bp_predict_op(g_bp_data, op, DUMMY_CFN, runahead_pc);
          STAT_EVENT(ic_stage->proc_id, FDIP_PRED_OFF_PATH);
          if (op->oracle_info.pred)
            taken_branches++;
        }
      }
    }

    bool do_prefetch = !last_cl_prefetched || (get_cache_line_addr(&ic->icache, runahead_pc) != last_cl_prefetched)? TRUE : FALSE;
    if (do_prefetch) {
      if ((FDIP_DUAL_PATH_PREF_IC_ENABLE || FDIP_DUAL_PATH_PREF_UOC_ENABLE)
          && hash_table_access(&top_mispred_br, runahead_pc)) {
        // TODO: change dual path prefetch to prefetch current op instead of the target
        //fdip_dual_path_prefetch(runahead_pc, op);
        //last_cl_prefetched = get_cache_line_addr(&ic->icache, runahead_pc);
      } else {
        if (FDIP_PREF_USEFUL_LINE && !will_be_accessed(runahead_pc)) {
          last_cl_prefetched = get_cache_line_addr(&ic->icache, runahead_pc);
        } else {
          Flag success = fdip_prefetch(runahead_pc, NULL);
          if (UOC_PREF)
            uop_cache_issue_prefetch(runahead_pc, fdip_on_path_pref);
          last_cl_prefetched = get_cache_line_addr(&ic->icache, runahead_pc);
          if (success) {
            if (ftq_pushed)
              ftq.back().second.prefetched = true;
            fdip_inc_outstanding_prefs(success);
            if (target)
              STAT_EVENT(ic_stage->proc_id, op->oracle_info.pred ? 
                  FDIP_BRANCH_TAKEN_PREF : FDIP_NL_PREF);
            if (fdip_on_path_pref) {
              STAT_EVENT(ic_stage->proc_id, FDIP_PREF_ON_PATH);
            }
            else {
              STAT_EVENT(ic_stage->proc_id, FDIP_PREF_OFF_PATH);
            }
          }
        }
      }
    }

    if (fdip_on_path_pref)
      set_max_op_num(is_branch, last_cl_prefetched);
    if (!fdip_on_path_bp && fdip_on_path_pref)
      fdip_on_path_pref = FALSE;

    // In an actual implemenation, FDIP cannot differentiate between a btb
    // miss and the op not being a branch (since the BTB is used to runahead
    // and find the next branch). Thus FDIP would continue as if it was not
    // branch, incrementing runahead_pc. This may cause cache pollution.
    // Boomerang CAN distinguish these cases by storing the end of the bbl
    if (btb_ras_miss || !target)
      runahead_pc++;
    else {
      ASSERT(ic_stage->proc_id, target);
      runahead_pc = target;
    }
    if (PERFECT_FDIP && !fdip_on_path_pref) {
      runahead_disable = TRUE;
    }
    if (runahead_disable)
      break;
  }

  if (taken_branches >= FDIP_MAX_TAKEN_BRANCHES) {
    STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_MAX_TAKEN_BRANCHES);
  }
  else if (last_runahead_op == max_runahead_op) {
    STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_LOOKAHEAD_BUFFER);
  }
  //else if (outstanding_prefs_on_path + outstanding_prefs_off_path >= FDIP_MAX_OUTSTANDING_PREFETCHES)
  else if (outstanding_prefs >= FDIP_MAX_OUTSTANDING_PREFETCHES)
  {
    STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_MAX_OUTSTANDING_PREFETCHES);
  }
  else if (break_on_tage_limit) {
    STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_TAGE_BUFFER_LIMIT);
  }
  else if (mem_req_failed)
  {
    STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_FULL_L1_MEM_REQ_QUEUE);
  }

  if (FDIP_PREF_NO_LATENCY) {
    outstanding_prefs = 0;
    //outstanding_prefs_on_path = 0;
    //outstanding_prefs_off_path = 0;
  }
}

Flag fdip_pref_off_path(void) {
  return !fdip_on_path_pref;
}

Flag fdip_is_max_op(Op* op) {
  if (!FDIP_ENABLE)
    return FALSE;
  if (op->op_num == max_op_num)
    return TRUE;
  return FALSE;
}

void fdip_dec_outstanding_prefs(Addr cl_addr) {
  ASSERT(ic_stage->proc_id, outstanding_prefs);
  outstanding_prefs--;
  return;
}

/*
void fdip_dec_outstanding_prefs(Addr cl_addr, Flag off_path, Counter emitted_cycle) {
  if (off_path) {
    if (emitted_cycle < last_recover_cycle)
      return;
    ASSERT(ic_stage->proc_id, outstanding_prefs_off_path);
    outstanding_prefs_off_path--;
  } else {
    ASSERT(ic_stage->proc_id, outstanding_prefs_on_path);
    outstanding_prefs_on_path--;
  }
  return;
}
*/

void fdip_inc_outstanding_prefs(Flag success) {
  switch(success) {
    case SUCCESS_NEW:
    case SUCCESS_DIFF_TYPE_ADDED:
      outstanding_prefs++;
      break;
    //case SUCCESS_SAME_TYPE_PATH_CHANGED:
    case SUCCESS_SAME_TYPE_INVALID_OFF_PATH_CHANGED:
    case SUCCESS_SAME_TYPE_VALID_OFF_PATH_CHANGED:
      ASSERT(ic_stage->proc_id, fdip_on_path_pref);
    case SUCCESS_SAME_TYPE:
    case SUCCESS_DIFF_TYPE:
    case FAILED:
    default:
      break;
  }
}

/*
void fdip_inc_outstanding_prefs(Flag success) {
  switch(success) {
    case SUCCESS_NEW:
    case SUCCESS_DIFF_TYPE_ADDED:
      if (fdip_on_path_pref) {
        outstanding_prefs_on_path++;
      }
      else {
        outstanding_prefs_off_path++;
      }
      break;
    case SUCCESS_SAME_TYPE_INVALID_OFF_PATH_CHANGED:
      ASSERT(ic_stage->proc_id, fdip_on_path_pref);
      outstanding_prefs_on_path++;
      break;
    case SUCCESS_SAME_TYPE_VALID_OFF_PATH_CHANGED:
      ASSERT(ic_stage->proc_id, fdip_on_path_pref);
      outstanding_prefs_on_path++;
      outstanding_prefs_off_path--; // TODO: if resetting this counter on a recovery, this should be fixed. (new_mem_req should return if the replaced off-path request was emitted before or after the recovery.
      break;
    case SUCCESS_SAME_TYPE:
    case SUCCESS_DIFF_TYPE:
    case FAILED:
    default:
      break;
  }
}
*/