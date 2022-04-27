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
#include "globals/enum.h"
#include "globals/count_min_sketch.h"

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
Counter last_runahead_op = 0; // the last runahead op predicted by FDIP looked by FDIP in find_op() to determine starting lookup point
Counter max_runahead_op = 0; // the last op in the lookahead buffer where FDIP can run ahead in maximum
extern List op_buf;
Hash_Table top_mispred_br;
uns64 recovery_count = 0;
uns64 off_count = 0;
Flag mem_req_failed = FALSE;
Counter max_op_num = 0;
Counter last_recover_cycle = 0;
CountMinSketch cms_useful;
CountMinSketch cms_unuseful;

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
    UNUSED(val);
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
  ASSERT(ic_stage->proc_id, FDIP_MAX_OUTSTANDING_PREFETCHES > 2);
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
  cms_init_optimal(&cms_useful, 0.001, 0.999);
  cms_init_optimal(&cms_unuseful, 0.001, 0.999);
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
  STAT_EVENT(ic_stage->proc_id, FDIP_RECOVER);
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
  void* line_info = NULL;
  Addr dummy_addr;

  if(!FDIP_ALWAYS_PREFETCH) {
    line = (Inst_Info**)cache_access(&ic_stage->icache, target,
                              &line_addr, TRUE);
    if(WP_COLLECT_STATS) {
      line_info = (Icache_Data*)cache_access(&ic_stage->icache_line_info, target, &dummy_addr, TRUE);
      UNUSED(line_info);
    }
  }
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

typedef enum FDIP_Break_Reason_enum {
  BR_NO_BREAK,
  BR_MAX_TAKEN_BRANCHES,       /* 2 by default */
  BR_MAX_OUTSTANDING_PREFS,    /* the number of outstanding prefetches in flight (not per cycle) */
  BR_CFS_PER_CYCLE,            /* the number of branches that can be predicted per cycle */
  BR_LOOKAHEAD_BUFFER_LIMIT,   /* FDIP cannot run ahead due to no more available decoded ops in the lookahead buffer (last_runahead_op == max_runahead_op) */
  BR_UNUSEFUL_LIMIT,           /* FDIP is trying to prefetch unuseful cache lines too much */
  BR_TAGE_BUFFER_LIMIT,        /* TAGE buffer is full (!bp_is_predictable()) */
  BR_MEM_REQ_BUF_LIMIT,         /* Mem Req L1 queue is full */
} FDIP_Break_Reason;

// Called each cycle to trigger runahead prefetches
void fdip_update() {
  uint32_t taken_branches        = 0;
  uint32_t num_cfs               = 0;
  bool do_prefetch               = false;
  FDIP_Break_Reason break_reason = BR_NO_BREAK;
  uint32_t num_unuseful_lines    = 0;
  Addr last_cl_unuseful = 0;

  if (runahead_disable)
    return;

  mem_req_failed = FALSE;

  // Predict branches across cache lines. In many implementations, the BTB
  // is looked up once per cycle, returning all branches in the cache line

  // Find the next branch after the runahead PC. As the BTB/BP is addressed
  // with a PC and not cache line address we need to byte-wise increment the runahead PC

  while (true) {
    bool btb_ras_miss = false;
    bool ftq_pushed   = false;
    bool is_branch    = false;
    Op* op            = NULL;
    Addr target       = 0;
    Addr line_addr    = 0;
    Addr dummy_addr    = 0;
    void* line        = NULL;
    void* line_info   = NULL;

    // Determine to emit a new prefetch
    do_prefetch = false;
    if (!last_cl_prefetched || (get_cache_line_addr(&ic->icache, runahead_pc) != last_cl_prefetched)) {
      line = (Inst_Info**)cache_access(&ic_stage->icache, runahead_pc, &line_addr, TRUE);
      // TODO: need to check if there is outstanding prefethces in MSHR queue for the cache line.
      if (WP_COLLECT_STATS) {
        line_info = (Icache_Data*)cache_access(&ic_stage->icache_line_info, runahead_pc, &dummy_addr, TRUE);
        UNUSED(line_info);
      }
      // MSHR hit
      Flag l1_queue_hit = l1_queue_access(get_cache_line_addr(&ic->icache, runahead_pc));
      if (line || l1_queue_hit) { // TODO: need to add MSHR hit???
        STAT_EVENT(ic_stage->proc_id, FDIP_PREF_ICACHE_HIT);
        last_cl_prefetched = line_addr;
      }
      if (!FDIP_CMS_ENABLE && (!last_cl_prefetched || (!line && last_cl_prefetched != line_addr))) {
        do_prefetch = true;
      } else if (FDIP_CMS_ENABLE) {
        if (!last_cl_prefetched || (!line && last_cl_prefetched != line_addr && last_cl_unuseful != line_addr)) {
          Counter res_useful = cms_check(&cms_useful, hexstr64s(line_addr));
          Counter res_unuseful = cms_check(&cms_unuseful, hexstr64s(line_addr));
          //cms_print(&cms_useful);
          //cms_print(&cms_unuseful);
          if ((res_useful == 0 && res_unuseful == 0) || ((res_useful+1)*FDIP_CMS_USEFUL_WEIGHT > res_unuseful)) {
            do_prefetch = true;
            if (!fdip_on_path_bp) {
            }
          } else {
            do_prefetch = false;
            num_unuseful_lines++;
            last_cl_unuseful = line_addr;
          }
        }
      }
    }

    // Break on lookahead buffer limit
    if (last_runahead_op == max_runahead_op) {
      break_reason = BR_LOOKAHEAD_BUFFER_LIMIT;
      break;
    }

    // Break on max unuseful lines
    if (num_unuseful_lines == FDIP_MAX_RUNAHEAD_UNUSEFUL_LINES) {
      break_reason = BR_UNUSEFUL_LIMIT;
      //runahead_disable = TRUE;
      break;
    }

    if (do_prefetch) {
      // Break on mem req buffer limit
      if(!mem_can_allocate_req_buffer(ic_stage->proc_id, MRT_FDIPPRF, FALSE)) {
        break_reason = BR_MEM_REQ_BUF_LIMIT;
        mem_req_failed = TRUE;
        break;
      }
    }

    // on-path prediction
    if (fdip_on_path_bp) {
      // find the corresponding op of runahead_pc from the lookahead buffer
      op = find_op(runahead_pc);
      is_branch = (op && op->table_info->cf_type)? true : false;
      if (is_branch) {
        // Break on TAGE buffer limit
        if (do_prefetch && BP_MECH != MTAGE_BP && !bp_is_predictable(g_bp_data, op)) {
          break_reason = BR_TAGE_BUFFER_LIMIT;
          move_to_prev_op();
          break;
        }

        // Break on the number of predictable branches per cycle
        if (num_cfs == CFS_PER_CYCLE) {
          break_reason = BR_CFS_PER_CYCLE;
          move_to_prev_op();
          break;
        }

        // Break on the maximum taken branches
        // We don't know the branch is taken or not before calling bp_predict_op(), but we break here to prevent from updating the branch predictor multiple times although breaking on a taken branch is not accurately simulated.
        if (taken_branches == FDIP_MAX_TAKEN_BRANCHES) {
          break_reason = BR_MAX_TAKEN_BRANCHES;
          move_to_prev_op();
          break;
        }
        num_cfs++;

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
          if ((FDIP_STOP_ON_MISPRED && op->oracle_info.mispred) || (FDIP_STOP_ON_MISFETCH && op->oracle_info.misfetch))
            runahead_disable = TRUE;
        } else if (btb_ras_miss) {
          ASSERT(ic_stage->proc_id, !op->oracle_info.mispred && !op->oracle_info.misfetch);
          off_count++;
          fdip_on_path_bp = FALSE;
          if (FDIP_STOP_ON_BTB_MISS)
            runahead_disable = TRUE;
        }
      }
    } else { // off-path prediction
      auto op_iter = pc_to_op.find(runahead_pc);
      is_branch = op_iter != pc_to_op.end();
      if (is_branch) {
        op = &op_iter->second;

        // Break on TAGE buffer limit
        if (do_prefetch && BP_MECH != MTAGE_BP && !bp_is_predictable(g_bp_data, op)) {
          break_reason = BR_TAGE_BUFFER_LIMIT;
          break;
        }

        // Break on the number of predictable branches per cycle
        if (num_cfs == CFS_PER_CYCLE) {
          break_reason = BR_CFS_PER_CYCLE;
          break;
        }

        // Break on the maximum taken branches
        // We don't know the branch is taken or not before calling bp_predict_op(), but we break here to prevent from updating the branch predictor multiple times although breaking on a taken branch is not accurately simulated.
        if (taken_branches == FDIP_MAX_TAKEN_BRANCHES) {
          break_reason = BR_MAX_TAKEN_BRANCHES;
          break;
        }
        num_cfs++;

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

    if (do_prefetch) {
      // TODO: need to add FDIP_DUAL_PATH_PREF_IC_ENABLE || FDIP_DUAL_PATH_PREF_UOC_ENABLE prefetching
      // change dual path prefetch to prefetch current op instead of the target
      Flag success = FALSE;
      if (fdip_on_path_pref)
        STAT_EVENT(ic_stage->proc_id, FDIP_ATTEMPTED_PREF_ON_PATH);
      else
        STAT_EVENT(ic_stage->proc_id, FDIP_ATTEMPTED_PREF_OFF_PATH);
      if (FDIP_PREF_NO_LATENCY) {
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
        if (icache_fill_line(&req)) {
          STAT_EVENT(ic_stage->proc_id, FDIP_PREFETCHES);
          success = SUCCESS_NEW;
        }
      } else {
        success = new_mem_req(MRT_FDIPPRF, ic_stage->proc_id, line_addr,
                      ICACHE_LINE_SIZE, 0, NULL, instr_fill_line, unique_count, 0);
        if (success)
          STAT_EVENT(ic_stage->proc_id, FDIP_PREFETCHES);
        else
          ASSERT(ic_stage->proc_id, false);
      }

      last_cl_prefetched = get_cache_line_addr(&ic->icache, runahead_pc);
      if (success) {
        if (ftq_pushed)
          ftq.back().second.prefetched = true;
        if (target)
          STAT_EVENT(ic_stage->proc_id, op->oracle_info.pred ? FDIP_BRANCH_TAKEN_PREF : FDIP_NL_PREF);
        if (fdip_on_path_pref)
          STAT_EVENT(ic_stage->proc_id, FDIP_PREF_ON_PATH);
        else
          STAT_EVENT(ic_stage->proc_id, FDIP_PREF_OFF_PATH);
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
    Op** op_p = (Op**)list_get_tail(&op_buf);
    op = *op_p;
    if (!is_branch && op->op_num == last_runahead_op) // this is a corner case where FDIP reaches the end of the lookahead buffer which is not a branch. Since other ops with the same fetch address can be decoded and followed, the runahead_pc should not be incremented.
      runahead_pc = runahead_pc;
    else if (btb_ras_miss || !target)
      runahead_pc++;
    else {
      ASSERT(ic_stage->proc_id, target);
      runahead_pc = target;
    }

    if (PERFECT_FDIP && !fdip_on_path_pref)
      runahead_disable = TRUE;

    if (runahead_disable) {
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_OFF_PATH);
      break;
    }
  }

  switch(break_reason) {
    case BR_NO_BREAK:
      STAT_EVENT(ic_stage->proc_id, FDIP_NO_BREAK);
      break;
    case BR_MAX_TAKEN_BRANCHES:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_MAX_TAKEN_BRANCHES);
      break;
    case BR_LOOKAHEAD_BUFFER_LIMIT:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_LOOKAHEAD_BUFFER);
      break;
    case BR_UNUSEFUL_LIMIT:
      if (fdip_on_path_bp)
        STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_UNUSEFUL_LINES_ON_PATH);
      else
        STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_UNUSEFUL_LINES_OFF_PATH);
      break;
    case BR_TAGE_BUFFER_LIMIT:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_TAGE_BUFFER_LIMIT);
      break;
    case BR_MEM_REQ_BUF_LIMIT:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_FULL_MEM_REQ_BUF);
      break;
    case BR_CFS_PER_CYCLE:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_CFS_PER_CYCLE);
    default:
      break;
  }

  STAT_EVENT(ic_stage->proc_id, FDIP_CYCLE_COUNT);
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