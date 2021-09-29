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

std::queue<std::pair<Addr, ftq_req>> ftq;
Addr runahead_pc;
Addr last_cl_prefetched = 0;
Icache_Stage *ic_stage;
Bp_Data *bp_data;
uns cf_num    = 0;
Flag runahead_disable = TRUE;
Op *recovery_checkpoint;
Flag recovery_checkpoint_valid = FALSE;
bool on_wrong_path = false;
Flag fdip_pred_on_path = TRUE;
uns64 last_runahead_uid = 0;
uns64 max_runahead_uid = 0;
extern List op_buf;
Hash_Table top_mispred_br;

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
    fgets(line, line_len, fp); // skip first line
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
  bp_data = _bp_data;
  ic_stage = _ic;
  ASSERT(ic_stage->proc_id, ic_stage);
  if ((FDIP_DUAL_PATH_PREF_IC_ENABLE || FDIP_DUAL_PATH_PREF_UOC_ENABLE)
        && TOP_MISPRED_BR_RESTEER_COVERAGE)
    init_topk_mispred();
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

  Op_Info *op_info = &op->oracle_info;
  Op_Info *req_info = &req->oracle_info;

  op_info->pred_addr = req_info->pred_addr;
  op_info->btb_miss_resolved = req_info->btb_miss_resolved;
  op_info->pred_npc = req_info->pred_npc;
  op_info->pred = req_info->pred;
  op_info->btb_miss = req_info->btb_miss;
  op_info->no_target = req_info->no_target;

// FIXME late prefetcher
  //uns8 late_pred;  // predicted direction of branch, set by the multi-cycle
  // branch predictor
  //Addr late_pred_npc;  // predicted next pc field by the multi-cycle branch
  // predictor
  //Flag late_misfetch;  // true if target address is the ONLY thing that was
  // wrong after the multi-cycle branch prediction kicks in
  //Flag  late_mispred;  // true if the multi-cycle branch predictor mispredicted
  //Flag  recovery_sch;  // true if this op_info has scheduled a recovery
  op_info->pred_global_hist = req_info->pred_global_hist;

//TODO: Verify perceptron correctness with FDIP
  op_info->pred_perceptron_global_hist =
    req_info->pred_perceptron_global_hist;
  op_info->pred_conf_perceptron_global_hist =
    req_info->pred_conf_perceptron_global_hist;
  op_info->pred_conf_perceptron_global_misp_hist =
    req_info->pred_conf_perceptron_global_misp_hist;
  op_info->pred_gpht_entry = req_info->pred_gpht_entry;
  op_info->pred_ppht_entry = req_info->pred_ppht_entry;
  op_info->pred_spht_entry = req_info->pred_spht_entry;
  
  op_info->pred_local_hist = req_info->pred_local_hist;
  op_info->pred_targ_hist = req_info->pred_targ_hist;
  op_info->hybridgp_gpred = req_info->hybridgp_gpred;
  op_info->hybridgp_ppred = req_info->hybridgp_ppred;
  op_info->pred_tc_selector_entry = req_info->pred_tc_selector_entry;
  op_info->ibp_miss = req_info->ibp_miss;
}

/* Called when the frontend consumes the next prediction. There exist three
 * cases. a) FDIP has not predicted a branch yet after a recovery. 
 * b) The FDIP predicted branch matches the branch provided by the
 * frontend - consume the prediction. c) FDIP was on the wrong path and
 * predicted a branch that is never actually processed by the frontend
 */ 
Addr fdip_pred(Addr bp_pc, Op *op) {
  if (ftq.empty()) {
    // FTQ should never be empty if we have a perfect branch predictor and no btb miss.
    //ASSERT(ic_stage->proc_id, !(PERFECT_BP && PERFECT_BTB));
    /* If the FTQ is empty we have not been able to predict ahead. Should
      * be a corner case e.g. directly after a recovery.
      */
    runahead_disable = FALSE;
    Addr target = bp_predict_op(g_bp_data, op, cf_num++, bp_pc);
    if (op->oracle_info.mispred || op->oracle_info.misfetch) {
      fdip_pred_on_path = FALSE;
      STAT_EVENT(ic_stage->proc_id, FDIP_PRED_OFF_PATH);
    } else {
      fdip_pred_on_path = TRUE;
      STAT_EVENT(ic_stage->proc_id, FDIP_PRED_ON_PATH);
    }
    recovery_checkpoint = op;
    recovery_checkpoint_valid = TRUE;
    runahead_pc = target;
    last_cl_prefetched = 0;
    on_wrong_path = false;
    STAT_EVENT(ic_stage->proc_id, FDIP_PRED_FTQ_EMPTY);
    return target;
  }
  else if (ftq.front().first == op->fetch_addr){
    /* FDIP predicted a branch on the right path. Consume the target and
      * patch the current instruction.
      */
    STAT_EVENT(ic_stage->proc_id, FDIP_PRED_CORRECT_PATH);
      auto req = &ftq.front().second;
    auto target = req->target;
    patch_oracle_info(op, &req->op, bp_pc);
    //Re-evaluate FDIP direction prediction based on the current oracle info
    bp_predict_op_evaluate(g_bp_data, op, req->target);
    //ASSERT(ic_stage->proc_id, PERFECT_FDIP && !op->oracle_info.mispred && !op->oracle_info.misfetch);
    op->cf_within_fetch = cf_num++;
    if (!on_wrong_path) {
      //We may have mispredicted once but the branch PCs seen by the
      //frontend and FDIP still match
      recovery_checkpoint = op;
      recovery_checkpoint_valid = TRUE;
    }
    /* A branch processed by FDIP has been mispredicted. We do not recover
      * at this point as we want FDIP to continue prefetching on the wrong
      * path. Recover only if this branch is resolved by the backend.
      */
    ASSERT(ic_stage->proc_id, req->taken == req->op.oracle_info.pred);
    if (op->oracle_info.mispred || op->oracle_info.misfetch) {
      on_wrong_path = true;
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
    ftq.pop();
    STAT_EVENT(ic_stage->proc_id, FDIP_ON_PATH_FTQ_POPS);
    return target;
  }
  else {
    /* If the executed branch does not match the oldest entry in the FTQ,
      * we are on the wrong path. Recover, clear FTQ and retry branch pred.
      * At this point we may not have realized that we are on the wrong path
      * as the branch has not been resolved yet. This should only happen with
      * trace frontend which do not actually see branches on the wrong path.
      */
    INC_STAT_EVENT(ic_stage->proc_id, FDIP_PRED_WRONG_PATH, ftq.size());
    // The executed branch should always match the oldest entry in the FTQ if we have a perfect branch predictor and no btb miss.
    ASSERT(ic_stage->proc_id, !(PERFECT_BP && !FDIP_BREAK_ICACHE && (PERFECT_BTB || FDIP_STOP_ON_BTB_MISS)));
    if (recovery_checkpoint_valid) {
      /* The frontend has fetched a branch and that branch is still
        * executing. Hence we have a valid recovery point to which we will
        * recover (squash all later branches predicted by FDIP).
        */
      bp_recover_op(g_bp_data, recovery_checkpoint->table_info->cf_type,
                &recovery_checkpoint->recovery_info);
    } else {
      /* MAYBE A HACK: The backend currently is not executing a branch. As
        * a result we have no valid recovery point. FDIP predicted a branch
        * that is never actually executed.
        * First recover it so that all later branches are squashed then
        * retire it like a regular instruction to keep the BP state in sync
        * The BP history now contains an executed, mispredicted, retired
        * branch that has never actually gone through the backend, this
        * impact TAGE's accuracy (hopefully it doesn't happen often).
        */
      //Copy req onto the stack, as bp_recover_op will clear the ftq 
      ftq_req req = ftq.front().second;
      bp_recover_op(g_bp_data, req.op.table_info->cf_type,
                &req.op.recovery_info);
      bp_retire_op(g_bp_data, &req.op);
      STAT_EVENT(ic_stage->proc_id, FDIP_SQUASH_FAKE_BRANCH);
    }
    fdip_clear_ftq(bp_pc);
    auto target =  bp_predict_op(g_bp_data, op, cf_num++, bp_pc);
    if (op->oracle_info.mispred || op->oracle_info.misfetch) {
      fdip_pred_on_path = FALSE;
      STAT_EVENT(ic_stage->proc_id, FDIP_PRED_OFF_PATH);
    } else {
      fdip_pred_on_path = TRUE;
      STAT_EVENT(ic_stage->proc_id, FDIP_PRED_ON_PATH);
    }
    recovery_checkpoint = op;
    recovery_checkpoint_valid = TRUE;
    runahead_disable = FALSE;
    runahead_pc = target;
    last_cl_prefetched = 0;
    on_wrong_path = false;

    return target;
  }
}

/* Clear the FTQ, branch predictor state needs to be recovered elsewhere
 */
void fdip_clear_ftq(Addr recover_pc) {
  runahead_pc = recover_pc;
  last_cl_prefetched = 0;
  while (!ftq.empty()) {
    if (ftq.front().second.prefetched) {
      STAT_EVENT(ic_stage->proc_id, FDIP_PREF_WRONG_PATH);
    }
    ftq.pop();
    STAT_EVENT(ic_stage->proc_id, FDIP_OFF_PATH_FTQ_POPS);
  }
}

/* When a mispredicted branch is resolved, the frontend recovers the branch
 * predictor state (including FDIP predictions), hence here we only have to 
 * clear the FTQ.
 */
void fdip_recover(Recovery_Info *info) {
  recovery_checkpoint = NULL;
  recovery_checkpoint_valid = false;
  fdip_clear_ftq(info->npc);
  (&op_buf)->current = NULL;
  last_runahead_uid = 0;
  runahead_disable = FALSE;
  on_wrong_path = false;
  fdip_update();
}

// Returns true if prefetch was emitted
bool fdip_prefetch(Addr target, Op *op) {
  static Addr last_line_addr_prefetched = 0;
  Addr line_addr;
  bool success = false;
  void* line = NULL;

  if (fdip_pred_on_path)
    STAT_EVENT(ic_stage->proc_id, FDIP_ATTEMPTED_PREF_ON_PATH);
  else
    STAT_EVENT(ic_stage->proc_id, FDIP_ATTEMPTED_PREF_OFF_PATH);

  if(!FDIP_ALWAYS_PREFETCH)
    line = (Inst_Info**)cache_access(&ic_stage->icache, target,
                              &line_addr, TRUE);
  if(FDIP_ALWAYS_PREFETCH || (!line && (last_line_addr_prefetched != line_addr))) {
    if(FDIP_PREF_NO_LATENCY) {
      Mem_Req req;
      req.off_path             = op ? op->off_path : FALSE;
      req.off_path_confirmed   = FALSE;
      req.type                 = MRT_IFETCH;
      req.proc_id              = ic_stage->proc_id;
      req.addr                 = line_addr;
      req.oldest_op_unique_num = (Counter)0;
      req.oldest_op_op_num     = (Counter)0;
      req.oldest_op_addr       = (Addr)0;
      req.dirty_l0             = op && op->table_info->mem_type == MEM_ST && !op->off_path;
      if(icache_fill_line(&req)) {
        STAT_EVENT(ic_stage->proc_id, FDIP_PREFETCHES);
        success = true;
      }
    } else {
      if(new_mem_req(MRT_IFETCH, ic_stage->proc_id, line_addr,
            ICACHE_LINE_SIZE, 0, NULL, icache_fill_line,
            unique_count,
            0)) {
        STAT_EVENT(ic_stage->proc_id, FDIP_PREFETCHES);
        success = true;
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
    bool success = fdip_prefetch(op->pred_target, op);
    icache_pref += success;
    ftq.back().second.prefetched = success;
    success = fdip_prefetch(op->inst_info->addr + ICACHE_LINE_SIZE, op);
    icache_pref += success;
    STAT_EVENT(ic_stage->proc_id, FDIP_ALT_PATH_PREFETCHES_IC_TRIGGERED);
    INC_STAT_EVENT(ic_stage->proc_id, FDIP_ALT_PATH_PREFETCHES_IC_EMITTED, 
                    icache_pref);
  }
  if (FDIP_DUAL_PATH_PREF_UOC_ENABLE) {
    uoc_pref += uop_cache_prefetch(op->pred_target);
    uoc_pref += uop_cache_prefetch(op->pc_plus_offset);
    STAT_EVENT(ic_stage->proc_id, FDIP_ALT_PATH_PREFETCHES_UOC_TRIGGERED);
    INC_STAT_EVENT(ic_stage->proc_id, FDIP_ALT_PATH_PREFETCHES_UOC_EMITTED,
                    uoc_pref);
  }
  return icache_pref;
}

// Called each cycle to trigger runahead prefetches
void fdip_update() {
  uint32_t predicts = 0;
  uint32_t prefetches = 0;

  if (ftq.size() > FDIP_MAX_RUNAHEAD || runahead_disable) {
    return;
  }

  // Predict branches across cache lines. In many implementations, the BTB
  // is looked up once per cycle, returning all branches in the cache line
  const Addr orig_cl = get_cache_line_addr(&ic->icache, runahead_pc);
  Addr cur_cl = orig_cl;

  // Find the next branch after the runahead PC. As the BTB/BP is addressed
  // with a PC and not cache line address we need to byte-wise increment the
  // runahead PC
  while (predicts < FDIP_BP_PER_CYC && prefetches < FDIP_PREF_PER_CYC &&
          ftq.size() <= FDIP_MAX_RUNAHEAD &&
          !(FDIP_BREAK_ICACHE && cur_cl != orig_cl)) {
    bool btb_ras_miss = false;
    if (LOOKAHEAD_BUF_SIZE && (max_runahead_uid == last_runahead_uid)) {
      runahead_disable = TRUE;
      break;
    }
    Op* op = find_op(runahead_pc);
    bool is_branch = (op && op->table_info->cf_type)? true : false;
    Addr target;
    if (is_branch) {
      ASSERT(ic_stage->proc_id, op->fetch_addr == runahead_pc);
      target = bp_predict_op(g_bp_data, op, DUMMY_CFN, runahead_pc);
      ftq.push(std::pair<Addr, ftq_req>(runahead_pc, ftq_req(
                                  target, cycle_count, *op,
                                  0, /*prefetched*/
                                  op->oracle_info.pred == TAKEN
                                  )));
      predicts++;
      if (PERFECT_FDIP && (op->oracle_info.mispred || op->oracle_info.misfetch)) {
        runahead_disable = TRUE;
        fdip_pred_on_path = FALSE;
        STAT_EVENT(ic_stage->proc_id, FDIP_PRED_OFF_PATH);
        break;
      } else {
        fdip_pred_on_path = TRUE;
        STAT_EVENT(ic_stage->proc_id, FDIP_PRED_ON_PATH);
      }
      STAT_EVENT(ic_stage->proc_id, FDIP_ON_PATH_FTQ_INSERTS);
      btb_ras_miss = op->oracle_info.btb_miss || target == 0;
      if (btb_ras_miss) {
        STAT_EVENT(ic_stage->proc_id, FDIP_BTB_RAS_MISS);
        // In an actual implemenation, FDIP cannot differentiate between a btb
        // miss and the op not being a branch (since the BTB is used to runahead
        // and find the next branch). Thus FDIP would continue as if it was not
        // branch, incrementing runahead_pc. This may cause cache pollution.
        // Boomerang CAN distinguish these cases by storing the end of the bbl
        if (FDIP_STOP_ON_BTB_MISS || PERFECT_FDIP) {
          runahead_disable = TRUE;
          break;
        }
      } else {
        // target is set to whichever instr is predicted to follow branch
        bool continuing_to_next_cl = !last_cl_prefetched? TRUE : (get_cache_line_addr(&ic->icache, target) ==
            last_cl_prefetched + ic->icache.offset_mask + 1)? TRUE : FALSE;
        if ((FDIP_DUAL_PATH_PREF_IC_ENABLE || FDIP_DUAL_PATH_PREF_UOC_ENABLE) 
            && hash_table_access(&top_mispred_br, runahead_pc)) {
          fdip_dual_path_prefetch(target, op);
          last_cl_prefetched = get_cache_line_addr(&ic->icache, target);
        } else if (op->oracle_info.pred == TAKEN || continuing_to_next_cl) {
          if (FDIP_PREF_USEFUL_LINE && !will_be_accessed(target)) {
            last_cl_prefetched = get_cache_line_addr(&ic->icache, target);
          } else {
            bool success = fdip_prefetch(target, op);
            prefetches += success;
            if (ftq.size())
              ftq.back().second.prefetched = success;
            last_cl_prefetched = get_cache_line_addr(&ic->icache, target);
            if (success){
              STAT_EVENT(ic_stage->proc_id, op->oracle_info.pred ? 
                  FDIP_BRANCH_TAKEN_PREF : FDIP_NL_PREF);
              if (fdip_pred_on_path) {
                STAT_EVENT(ic_stage->proc_id, FDIP_PREF_ON_PATH);
              }
              else
                STAT_EVENT(ic_stage->proc_id, FDIP_PREF_OFF_PATH);
            }
          }
        }
        runahead_pc = target;
      }
    }
    if (!is_branch || btb_ras_miss) { // FDIP continues as for non-branch
      // If continuing to next cache line (no control flow change), prefetch it
      bool continuing_to_next_cl = !last_cl_prefetched? TRUE : (get_cache_line_addr(&ic->icache, runahead_pc+1) ==
          last_cl_prefetched + ic->icache.offset_mask + 1)? TRUE : FALSE;
      if (continuing_to_next_cl) {
        if (FDIP_PREF_USEFUL_LINE && !will_be_accessed(runahead_pc+1)) {
          last_cl_prefetched = get_cache_line_addr(&ic->icache, runahead_pc+1);
        } else {
          bool success = fdip_prefetch(runahead_pc+1, NULL);
          prefetches += success;
          if (ftq.size())
            ftq.back().second.prefetched = success;
          last_cl_prefetched = get_cache_line_addr(&ic->icache, runahead_pc+1);
          if (success) {
            STAT_EVENT(ic_stage->proc_id, FDIP_NL_PREF);
            if (fdip_pred_on_path) {
              STAT_EVENT(ic_stage->proc_id, FDIP_PREF_ON_PATH);
            }
            else
              STAT_EVENT(ic_stage->proc_id, FDIP_PREF_OFF_PATH);
          }
        }
      }
      runahead_pc++;
    }
    cur_cl = get_cache_line_addr(&ic->icache, runahead_pc);
  }
}
