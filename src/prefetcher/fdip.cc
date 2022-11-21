#include "prefetcher/fdip.h"

#include <unordered_map>
#include <queue>
#include <algorithm>
#include <iostream>
#include "libs/bloom_filter.hpp"

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
#include "sim.h"
}

#define DUMMY_CFN ~0
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_FDIP, ##args)
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

std::deque<std::pair<Addr, Flag>> cl_candidates;
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
std::unordered_map<Addr, Counter> cnt_useful;
std::unordered_map<Addr, Counter> cnt_unuseful;
std::unordered_map<Addr, Counter> useful_hash;
std::vector<Addr> fetch_cl_addr;
extern uns operating_mode;
Flag print_warmup_hash_table = FALSE;
Counter fdip_ftq_pos = 0;
Counter icache_ftq_pos = 0;
Cache fdip_cc;
bloom_filter *bloom;
bloom_filter *bloom2;
bloom_filter *bloom4;
bloom_parameters bloom1_parameters;
bloom_parameters bloom2_parameters;
bloom_parameters bloom4_parameters;
Addr last_prefetch_candidate;
uint32_t last_prefetch_candidate_counter = 0;
uint64_t bloom_inserts = 0;
Addr last_runahead_pc = 0;

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
  if (!FETCH_BREAK_ON_TAKEN) { // icache_stage does not break on taken branches
    ASSERT(ic_stage->proc_id, FDIP_MAX_TAKEN_BRANCHES >= ISSUE_WIDTH);
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
  if (!FDIP_BLOOM_FILTER) {
    init_cache(&fdip_cc, "FDIP_USEFULNESS_CACHE", FDIP_CC_SIZE, FDIP_CC_ASSOC, ICACHE_LINE_SIZE,
               0, REPL_TRUE_LRU); //Data size = 2 byte
  } else {
    bloom1_parameters.projected_element_count = FDIP_BLOOM_ENTRIES;
    //bloom_parameters.maximum_number_of_hashes = FDIP_BLOOM_HASHES;
    //bloom_parameters.minimum_number_of_hashes = FDIP_BLOOM_HASHES;
    bloom1_parameters.false_positive_probability = 0.005;
    //bloom1_parameters.maximum_size = FDIP_BLOOM_SIZE;
    //bloom_parameters.minimum_size = FDIP_BLOOM_SIZE;
    bloom1_parameters.compute_optimal_parameters();
    bloom = new bloom_filter(bloom1_parameters);

    bloom2_parameters.projected_element_count = FDIP_BLOOM2_ENTRIES;
    bloom2_parameters.false_positive_probability = 0.005;
    bloom2_parameters.compute_optimal_parameters();
    bloom2 = new bloom_filter(bloom2_parameters);

    bloom4_parameters.projected_element_count = FDIP_BLOOM4_ENTRIES;
    bloom4_parameters.false_positive_probability = 0.005;
    bloom4_parameters.compute_optimal_parameters();
    bloom4 = new bloom_filter(bloom4_parameters);
  }
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
  DEBUG(ic_stage->proc_id, "[%llu] [fdip_pred] right path - bp_pc: %llx, op->fetch_addr: %llx, op->op_num: %llu\n", cycle_count, bp_pc, op->fetch_addr, op->op_num);
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
  DEBUG(ic_stage->proc_id, "[%llu] [fdip_recover] info->npc : %llx\n", cycle_count, info->npc);
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
  fdip_ftq_pos = icache_ftq_pos;
  STAT_EVENT(ic_stage->proc_id, FDIP_RECOVER);
}

/* When a misfetch (btb miss) is resolved, the frontend informs FDIP to clear FDIP branch predictor states including the FTQ.
 */
void fdip_redirect(Addr recover_pc) {
  DEBUG(ic_stage->proc_id, "[fdip_redirect] recover_pc : %llx\n", recover_pc);
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
  BR_CACHELINE,                /* FDIP looks up ICACHE_LINE_SIZE amount of PCs per cycle */
  BR_MAX_TAKEN_BRANCHES,       /* 2 by default */
  BR_FTQ,                      /* the number of basic blocks in flight (not per cycle) */
  BR_RUNAHEAD_FIFO,            /* FDIP cannot enqueue more than one cache line into the timely fifo except when the fifo is not full. */
  BR_CFS_PER_CYCLE,            /* the number of branches that can be predicted per cycle */
  BR_LOOKAHEAD_BUFFER_LIMIT,   /* FDIP cannot run ahead due to no more available decoded ops in the lookahead buffer (last_runahead_op == max_runahead_op) */
  BR_TAGE_BUFFER_LIMIT,        /* TAGE buffer is full (!bp_is_predictable()) */
  BR_MEM_REQ_BUF_LIMIT,        /* Mem Req L1 queue is full */
} FDIP_Break_Reason;

void* bloom_lookup(Addr uc_line_addr) {
  Addr line_addr = uc_line_addr >> 6;
  return (void*)(bloom->contains(line_addr) || bloom2->contains(line_addr >> 1) || bloom4->contains(line_addr >> 2));
}

void insert1(Addr line_addr) {
  STAT_EVENT(ic_stage->proc_id, FDIP_BLOOM_1INSERT);
  if (!bloom2->contains(line_addr >> 1) && !bloom4->contains(line_addr >> 2)) {
    bloom->insert(line_addr);
  }

}

void insert2(Addr line_addr) {
  if((line_addr & 1) == 0) { //2CL aligned
    if (!bloom4->contains(line_addr >> 2) && !bloom2->contains(line_addr >> 1)) {
      bloom2->insert(line_addr >> 1);
    }
    STAT_EVENT(ic_stage->proc_id, FDIP_BLOOM_2INSERT);
  }
  else {
    insert1(line_addr);
    insert1(line_addr + 1);
  }
}

void insert3(Addr line_addr) {
  if((line_addr & 1) == 0) { //2CL aligned
    insert2(line_addr);
    insert1(line_addr + 2);
  }
  else {
    insert1(line_addr + 1);
    insert2(line_addr);
  }
}

void insert4(Addr line_addr) {
  ASSERT(0, (line_addr & 3) == 0);
  bloom4->insert(line_addr >> 2);
  STAT_EVENT(ic_stage->proc_id, FDIP_BLOOM_4INSERT);
}

void insert_remaining(uint32_t inserted) {
  while (inserted + 4 <= last_prefetch_candidate_counter) {
    insert4(last_prefetch_candidate + inserted);
    inserted += 4;
  }
  if (inserted + 3 == last_prefetch_candidate_counter)
    insert3(last_prefetch_candidate + inserted);
  if (inserted + 2 == last_prefetch_candidate_counter)
    insert2(last_prefetch_candidate + inserted);
  if (inserted + 1 == last_prefetch_candidate_counter)
    insert1(last_prefetch_candidate + inserted);

}

void bloom_insert() {
  uint32_t inserted = 0;
  if (last_prefetch_candidate_counter < 4) {
    insert_remaining(inserted);
    return;
  }
  if ((last_prefetch_candidate & 3) == 0) {
    //4cl aligned
    insert_remaining(inserted);
  }
  else if ((last_prefetch_candidate & 3) == 2) {
    //2cl algned
    insert2(last_prefetch_candidate);
    inserted += 2;
    insert_remaining(inserted);
  }
  else if ((last_prefetch_candidate & 3) == 1) {
    //cl aligned
    //cl aligned
    insert3(last_prefetch_candidate);
    inserted += 3;
    insert_remaining(inserted);
  }
  else if ((last_prefetch_candidate & 3) == 3) {
    //cl aligned
    insert1(last_prefetch_candidate);
    inserted +=1;
    insert_remaining(inserted);
  }
  return;
}

void detect_stream(Addr uc_line_addr) {
  Addr line_addr = uc_line_addr >> 6;

  if(last_prefetch_candidate_counter == 0) {
    last_prefetch_candidate_counter++;
    last_prefetch_candidate = line_addr;
    return;
  }
  STAT_EVENT(ic_stage->proc_id, FDIP_BLOOM_INSERTED);

  if (line_addr == last_prefetch_candidate + last_prefetch_candidate_counter) {
    last_prefetch_candidate_counter++;
  }
  else {
    bloom_insert();
    last_prefetch_candidate_counter = 1;
    last_prefetch_candidate = line_addr;
  }
}

// Called each cycle to trigger runahead prefetches
void fdip_update() {
  Addr MASK_32B = 0x2F;
  uint32_t taken_branches        = 0;
  uint32_t num_cfs               = 0;
  bool do_prefetch_2_hash        = false;
  bool do_prefetch               = false;
  FDIP_Break_Reason break_reason = BR_NO_BREAK;
  Addr last_cl_unuseful = 0;
  bool cl_candidates_popped = false;
  Addr fdip_break_addr_top = runahead_pc | MASK_32B;
  Addr fdip_break_addr_bottom = runahead_pc & ~MASK_32B;

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
    DEBUG(ic_stage->proc_id, "[fdip_update] runahead_pc: %llx, max_runahead_op: %llu, last_runahead_op: %llu, max_op_num : %llu, ftq.size(): %ld\n", runahead_pc, max_runahead_op, last_runahead_op, max_op_num, ftq.size());
    if (operating_mode == SIMULATION_MODE && print_warmup_hash_table == FALSE) {
      fdip_print_hash_tables();
      print_warmup_hash_table = TRUE;
    }

    if (FDIP_TIMELY_FIFO_SIZE && cl_candidates_popped == true) {
      break_reason = BR_RUNAHEAD_FIFO;
      break;
    }

    if (runahead_pc > fdip_break_addr_top || runahead_pc < fdip_break_addr_bottom) {
      break_reason = BR_CACHELINE;
      break;
    }

    if (taken_branches == FDIP_MAX_TAKEN_BRANCHES) {
      break_reason = BR_MAX_TAKEN_BRANCHES;
      break;
    }

    if (FDIP_FTQ_DEPTH && fdip_ftq_pos >= icache_ftq_pos + FDIP_FTQ_DEPTH) {
      break_reason = BR_FTQ;
      break;
    }

    // Determine to emit a new prefetch
    do_prefetch_2_hash = false;
    do_prefetch = false;
    DEBUG(ic_stage->proc_id, "last_cl_prefetched: %llx\n", last_cl_prefetched);
    if (!last_cl_prefetched || (get_cache_line_addr(&ic->icache, runahead_pc) != last_cl_prefetched)) {
      line = (Inst_Info**)cache_access(&ic_stage->icache, runahead_pc, &line_addr, TRUE);
      ASSERT(ic_stage->proc_id, line_addr ==  get_cache_line_addr(&ic->icache, runahead_pc));
      // TODO: need to check if there is outstanding prefethces in MSHR queue for the cache line.
      if (WP_COLLECT_STATS) {
        line_info = (Icache_Data*)cache_access(&ic_stage->icache_line_info, runahead_pc, &dummy_addr, TRUE);
        UNUSED(line_info);
      }
      // MSHR hit
      Flag mem_buf_hit = mem_buf_access(get_cache_line_addr(&ic->icache, runahead_pc));
      if (line || mem_buf_hit) {
        DEBUG(ic_stage->proc_id, "[fdip_update] line already exists in L1 or MSHR\n");
        STAT_EVENT(ic_stage->proc_id, FDIP_PREF_ICACHE_HIT);
        last_cl_prefetched = line_addr;
        // TODO: insert a CL candidate into the FIFO cl_candidates
      } else {
        if (!FDIP_HASH_ENABLE && !FDIP_CC_SIZE) {
          do_prefetch = true;
        } else if (FDIP_HASH_ENABLE == 3 && !FDIP_CC_SIZE) { // compare 2-hash vs 1-hash
          if (last_cl_unuseful != line_addr) {
            auto useful_iter = cnt_useful.find(line_addr);
            auto unuseful_iter = cnt_unuseful.find(line_addr);
            auto useful_cl_iter = useful_hash.find(line_addr);
            DEBUG(ic_stage->proc_id, "[fdip_update] runahead_pc: %llx, max_runahead_op: %llu, last_runahead_op: %llu, : %llu, ftq.size(): %ld, last_cl_prefetched: %llx\n", runahead_pc, max_runahead_op, last_runahead_op, max_op_num, ftq.size(), last_cl_prefetched);
            if (FDIP_TIMELY_FIFO_SIZE) {
              // With the timeliness fifo, prefetch as pessimistic as possible. If the cacheline has not yet seen by the backend within the timely window, do not prefetch, but stil enqueue the cacheline into the fifo.
              if (unuseful_iter != cnt_unuseful.end()) {
                if (useful_iter != cnt_useful.end() && useful_iter->second >= unuseful_iter->second) {
                  STAT_EVENT(ic_stage->proc_id, FDIP_HASH_HIT);
                  do_prefetch = true;
                  DEBUG(ic_stage->proc_id, "do_prefetch for cl 0x%llx", line_addr);
                  if (!fdip_on_path_bp) {
                    DEBUG(ic_stage->proc_id, " OFF path\n");
                  }
                  else {
                    DEBUG(ic_stage->proc_id, " ON path\n");
                  }
                } else {
                  STAT_EVENT(ic_stage->proc_id, FDIP_HASH_MISS);
                  DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx\n", line_addr);
                  do_prefetch = false;
                  last_cl_unuseful = line_addr;
                }
              } else {
                if (useful_iter != cnt_useful.end()) {
                  STAT_EVENT(ic_stage->proc_id, FDIP_HASH_HIT);
                  do_prefetch = true;
                  DEBUG(ic_stage->proc_id, "do_prefetch for cl 0x%llx\n", line_addr);
                  if (!fdip_on_path_bp)
                    DEBUG(ic_stage->proc_id, " OFF path\n");
                  else
                    DEBUG(ic_stage->proc_id, " ON path\n");
                } else {
                  STAT_EVENT(ic_stage->proc_id, FDIP_HASH_MISS);
                  DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx\n", line_addr);
                  do_prefetch = false;
                  last_cl_unuseful = line_addr;
                }
              }

              if (useful_cl_iter != useful_hash.end()) {
                STAT_EVENT(ic_stage->proc_id, FDIP_HASH_HIT);
                do_prefetch_2_hash = true;
                DEBUG(ic_stage->proc_id, "do_prefetch for cl0x%llx", line_addr);
                if (!fdip_on_path_bp) {
                  DEBUG(ic_stage->proc_id, " OFF path\n");
                } else {
                  DEBUG(ic_stage->proc_id, " ON path\n");
                }
              } else if (useful_cl_iter == useful_hash.end()) {
                STAT_EVENT(ic_stage->proc_id, FDIP_HASH_MISS);
                DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx\n", line_addr);
                do_prefetch_2_hash = false;
              }

              if (do_prefetch != do_prefetch_2_hash) {
                DEBUG(ic_stage->proc_id, "hash and cache returns different decisions!\n");
              }

              // enqueue the cacheline into the timely fifo (cl_candidates)
              bool  cl_candidate_found = false;
              for (auto it = cl_candidates.begin(); it != cl_candidates.end(); ++it) {
                if (it->first == line_addr) {
                  cl_candidate_found = true;
                  DEBUG(ic_stage->proc_id, "cl_candidate found for cl 0x%llx\n", line_addr);
                  break;
                }
              }
              if (!cl_candidate_found) {
                if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                  auto popped_cl = cl_candidates.front();
                  DEBUG(ic_stage->proc_id, "cl_candidate popped for cl 0x%llx", popped_cl.first);
                  if (popped_cl.second == TRUE) {
                    fdip_inc_cnt_useful(popped_cl.first);
                    fdip_inc_useful_hash(popped_cl.first);
                    DEBUG(ic_stage->proc_id, " useful\n");
                  } else {
                    fdip_inc_cnt_unuseful(popped_cl.first);
                    DEBUG(ic_stage->proc_id, " unuseful\n");
                  }
                  cl_candidates.pop_front();
                  cl_candidates_popped = true;
                  cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                  DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
                } else {
                  cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                  DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
                  if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                    cl_candidates_popped = true;
                    DEBUG(ic_stage->proc_id, "cl_candidates are firstly filled full\n");
                  }
                }
              }
            }
          }
        } else if (FDIP_HASH_ENABLE == 2 && !FDIP_CC_SIZE) { // 2-hash confidence table
          if (last_cl_unuseful != line_addr) {
            auto useful_iter = cnt_useful.find(line_addr);
            auto unuseful_iter = cnt_unuseful.find(line_addr);
            DEBUG(ic_stage->proc_id, "[fdip_update] runahead_pc: %llx, max_runahead_op: %llu, last_runahead_op: %llu, : %llu, ftq.size(): %ld, last_cl_prefetched: %llx\n", runahead_pc, max_runahead_op, last_runahead_op, max_op_num, ftq.size(), last_cl_prefetched);
            if (FDIP_TIMELY_FIFO_SIZE) {
              // With the timeliness fifo, prefetch as pessimistic as possible. If the cacheline has not yet seen by the backend within the timely window, do not prefetch, but stil enqueue the cacheline into the fifo.
              if (unuseful_iter != cnt_unuseful.end()) {
                if (useful_iter != cnt_useful.end() && useful_iter->second >= unuseful_iter->second) {
                  STAT_EVENT(ic_stage->proc_id, FDIP_HASH_HIT);
                  do_prefetch = true;
                  DEBUG(ic_stage->proc_id, "do_prefetch for cl 0x%llx", line_addr);
                  if (!fdip_on_path_bp) {
                    DEBUG(ic_stage->proc_id, " OFF path\n");
                  }
                  else {
                    DEBUG(ic_stage->proc_id, " ON path\n");
                  }
                } else {
                  STAT_EVENT(ic_stage->proc_id, FDIP_HASH_MISS);
                  DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx\n", line_addr);
                  do_prefetch = false;
                  last_cl_unuseful = line_addr;
                }
              } else {
                if (useful_iter != cnt_useful.end()) {
                  STAT_EVENT(ic_stage->proc_id, FDIP_HASH_HIT);
                  do_prefetch = true;
                  DEBUG(ic_stage->proc_id, "do_prefetch for cl 0x%llx\n", line_addr);
                  if (!fdip_on_path_bp)
                    DEBUG(ic_stage->proc_id, " OFF path\n");
                  else
                    DEBUG(ic_stage->proc_id, " ON path\n");
                } else {
                  STAT_EVENT(ic_stage->proc_id, FDIP_HASH_MISS);
                  DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx\n", line_addr);
                  do_prefetch = false;
                  last_cl_unuseful = line_addr;
                }
              }
              // enqueue the cacheline into the timely fifo (cl_candidates)
              bool  cl_candidate_found = false;
              for (auto it = cl_candidates.begin(); it != cl_candidates.end(); ++it) {
                if (it->first == line_addr) {
                  cl_candidate_found = true;
                  DEBUG(ic_stage->proc_id, "cl_candidate found for cl 0x%llx\n", line_addr);
                  break;
                }
              }
              if (!cl_candidate_found) {
                if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                  auto popped_cl = cl_candidates.front();
                  DEBUG(ic_stage->proc_id, "cl_candidate popped for cl 0x%llx", popped_cl.first);
                  if (popped_cl.second == TRUE) {
                    fdip_inc_cnt_useful(popped_cl.first);
                    DEBUG(ic_stage->proc_id, " useful\n");
                  } else {
                    fdip_inc_cnt_unuseful(popped_cl.first);
                    DEBUG(ic_stage->proc_id, " unuseful\n");
                  }
                  cl_candidates.pop_front();
                  cl_candidates_popped = true;
                  cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                  DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
                } else {
                  cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                  DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
                  if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                    cl_candidates_popped = true;
                    DEBUG(ic_stage->proc_id, "cl_candidates are firstly filled full\n");
                  }
                }
              }
            } else {
              if (unuseful_iter == cnt_unuseful.end() || (useful_iter != cnt_useful.end() && FDIP_HASH_USEFUL_WEIGHT*useful_iter->second >= unuseful_iter->second+FDIP_HASH_BIAS)) {
                do_prefetch = true;
                DEBUG(ic_stage->proc_id, "do_prefetch for cl 0x%llx", line_addr);
                if (!fdip_on_path_bp) {
                  DEBUG(ic_stage->proc_id, " OFF path\n");
                }
                else {
                  DEBUG(ic_stage->proc_id, " ON path\n");
                }
              } else {
                DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx\n", line_addr);
                do_prefetch = false;
                last_cl_unuseful = line_addr;
              }
            }
          }
        } else if (!FDIP_HASH_ENABLE && FDIP_CC_SIZE) { // confidence cache
          if (last_cl_unuseful != line_addr) {
            DEBUG(ic_stage->proc_id, "[fdip_update] runahead_pc: %llx, max_runahead_op: %llu, last_runahead_op: %llu, : %llu, ftq.size(): %ld, last_cl_prefetched: %llx\n", runahead_pc, max_runahead_op, last_runahead_op, max_op_num, ftq.size(), last_cl_prefetched);
            Addr uc_line_addr = line_addr;
            Addr dummy_uc_line_addr = 0;
            void* useful;
            if (!FDIP_BLOOM_FILTER) {
              useful = (void*)cache_access(&fdip_cc, uc_line_addr, &dummy_uc_line_addr, TRUE);
            }
            else {
              useful = bloom_lookup(uc_line_addr);
            }
            ASSERT(ic_stage->proc_id, FDIP_TIMELY_FIFO_SIZE);

            // With the timeliness fifo, prefetch as pessimistic as possible. If the cacheline has not yet seen by the backend within the timely window, do not prefetch, but stil enqueue the cacheline into the fifo.
            if (useful) {
              STAT_EVENT(ic_stage->proc_id, FDIP_CC_HIT);
              do_prefetch = true;
              DEBUG(ic_stage->proc_id, "do_prefetch for cl 0x%llx, uc_line_addr %llx", line_addr, uc_line_addr);
              if (!fdip_on_path_bp)
                DEBUG(ic_stage->proc_id, " OFF path\n");
              else
                DEBUG(ic_stage->proc_id, " ON path\n");
            } else {
              STAT_EVENT(ic_stage->proc_id, FDIP_CC_MISS);
              DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx, uc_line_addr %llx\n", line_addr, uc_line_addr);
              do_prefetch = false;
              last_cl_unuseful = line_addr;
            }
            // enqueue the cacheline into the timely fifo (cl_candidates)
            bool  cl_candidate_found = false;
            for (auto it = cl_candidates.begin(); it != cl_candidates.end(); ++it) {
              if (it->first == line_addr) {
                cl_candidate_found = true;
                DEBUG(ic_stage->proc_id, "cl_candidate found for cl 0x%llx\n", line_addr);
                break;
              }
            }
            if (!cl_candidate_found) {
              if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                auto popped_cl = cl_candidates.front();
                DEBUG(ic_stage->proc_id, "cl_candidate popped for cl 0x%llx", popped_cl.first);
                Addr repl_uc_line_addr;
                uc_line_addr = popped_cl.first;
                void*cnt;
                if (!FDIP_BLOOM_FILTER) {
                  cnt = (void*)cache_access(&fdip_cc, uc_line_addr, &dummy_uc_line_addr, TRUE);
                } else {
                  cnt = bloom_lookup(uc_line_addr);
                }
                if (popped_cl.second == TRUE) {
                  if(!cnt) {
                    if (!FDIP_BLOOM_FILTER) {
                      cache_insert_replpos(&fdip_cc, ic_stage->proc_id, uc_line_addr, &dummy_uc_line_addr,
                                           &repl_uc_line_addr, (Cache_Insert_Repl)FDIP_CC_INSERT_REPLPOL, FALSE);
                    } else {
                      detect_stream(uc_line_addr);
                    }
                    if (repl_uc_line_addr)
                      STAT_EVENT(ic_stage->proc_id, FDIP_CC_REPLACEMENT);
                  }
                  DEBUG(ic_stage->proc_id, " useful\n");
                } else {
                  DEBUG(ic_stage->proc_id, " unuseful\n");
                }
                cl_candidates.pop_front();
                cl_candidates_popped = true;
                cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
              } else {
                cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
                if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                  cl_candidates_popped = true;
                  DEBUG(ic_stage->proc_id, "cl_candidates are firstly filled full\n");
                }
              }
            }
          }
        } else if (FDIP_HASH_ENABLE == 1 && !FDIP_CC_SIZE) { // 1-hash confidence table
          if (last_cl_unuseful != line_addr) {
            auto useful_cl_iter = useful_hash.find(line_addr);
            DEBUG(ic_stage->proc_id, "[fdip_update] runahead_pc: %llx, max_runahead_op: %llu, last_runahead_op: %llu, : %llu, ftq.size(): %ld, last_cl_prefetched: %llx\n", runahead_pc, max_runahead_op, last_runahead_op, max_op_num, ftq.size(), last_cl_prefetched);
            if (FDIP_TIMELY_FIFO_SIZE) {
              // With the timeliness fifo, prefetch as pessimistic as possible. If the cacheline has not yet seen by the backend within the timely window, do not prefetch, but stil enqueue the cacheline into the fifo.
              if (useful_cl_iter != useful_hash.end()) {
                STAT_EVENT(ic_stage->proc_id, FDIP_HASH_HIT);
                do_prefetch = true;
                DEBUG(ic_stage->proc_id, "do_prefetch for cl0x%llx", line_addr);
                if (!fdip_on_path_bp) {
                  DEBUG(ic_stage->proc_id, " OFF path\n");
                } else {
                  DEBUG(ic_stage->proc_id, " ON path\n");
                }
              } else if (useful_cl_iter == useful_hash.end()) {
                STAT_EVENT(ic_stage->proc_id, FDIP_HASH_MISS);
                DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx\n", line_addr);
                do_prefetch = false;
                last_cl_unuseful = line_addr;
              }
              // enqueue the cacheline into the timely fifo (cl_candidates)
              bool  cl_candidate_found = false;
              for (auto it = cl_candidates.begin(); it != cl_candidates.end(); ++it) {
                if (it->first == line_addr) {
                  cl_candidate_found = true;
                  DEBUG(ic_stage->proc_id, "cl_candidate found for cl 0x%llx\n", line_addr);
                  break;
                }
              }
              if (!cl_candidate_found) {
                if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                  auto popped_cl = cl_candidates.front();
                  DEBUG(ic_stage->proc_id, "cl_candidate popped for cl 0x%llx", popped_cl.first);
                  if (popped_cl.second == TRUE) {
                    fdip_inc_useful_hash(popped_cl.first);
                    DEBUG(ic_stage->proc_id, " useful\n");
                  } else {
                    DEBUG(ic_stage->proc_id, " unuseful\n");
                  }
                  cl_candidates.pop_front();
                  cl_candidates_popped = true;
                  cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                  DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
                } else {
                  cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                  DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
                  if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                    cl_candidates_popped = true;
                    DEBUG(ic_stage->proc_id, "cl_candidates are firstly filled full\n");
                  }
                }
              }
            }
          }
        } else if (FDIP_HASH_ENABLE == 1 && FDIP_CC_SIZE) { // compare 1-hash vs cache
          if (last_cl_unuseful != line_addr) {
            Addr uc_line_addr = line_addr;
            void *useful;
            if (!FDIP_BLOOM_FILTER) {
              Addr dummy_uc_line_addr = 0;
              useful    = (void*)cache_access(&fdip_cc, uc_line_addr, &dummy_uc_line_addr, TRUE);
            } else {
              useful = bloom_lookup(uc_line_addr);
            }
            auto useful_cl_iter = useful_hash.find(line_addr);
            DEBUG(ic_stage->proc_id, "[fdip_update] runahead_pc: %llx, max_runahead_op: %llu, last_runahead_op: %llu, : %llu, ftq.size(): %ld, last_cl_prefetched: %llx\n", runahead_pc, max_runahead_op, last_runahead_op, max_op_num, ftq.size(), last_cl_prefetched);
            if (FDIP_TIMELY_FIFO_SIZE) {
              // With the timeliness fifo, prefetch as pessimistic as possible. If the cacheline has not yet seen by the backend within the timely window, do not prefetch, but stil enqueue the cacheline into the fifo.
              if (useful_cl_iter != useful_hash.end()) {
                STAT_EVENT(ic_stage->proc_id, FDIP_HASH_HIT);
                do_prefetch = true;
                DEBUG(ic_stage->proc_id, "do_prefetch for cl0x%llx, uc_line_addr %llx", line_addr, uc_line_addr);
                if (!fdip_on_path_bp) {
                  DEBUG(ic_stage->proc_id, " OFF path\n");
                } else {
                  DEBUG(ic_stage->proc_id, " ON path\n");
                }
              } else if (useful_cl_iter == useful_hash.end()) {
                STAT_EVENT(ic_stage->proc_id, FDIP_HASH_MISS);
                DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx, uc_line_addr %llx\n", line_addr, uc_line_addr);
                do_prefetch = false;
                last_cl_unuseful = line_addr;
              }
              if (useful) {
                STAT_EVENT(ic_stage->proc_id, FDIP_CC_HIT);
                do_prefetch_2_hash = true;
                DEBUG(ic_stage->proc_id, "do_prefetch for cl 0x%llx, uc_line_addr %llx", line_addr, uc_line_addr);
                if (!fdip_on_path_bp)
                  DEBUG(ic_stage->proc_id, " OFF path\n");
                else
                  DEBUG(ic_stage->proc_id, " ON path\n");
              } else if (!useful) {
                STAT_EVENT(ic_stage->proc_id, FDIP_CC_MISS);
                DEBUG(ic_stage->proc_id, "do not prefetch for cl 0x%llx, uc_line_addr %llx\n", line_addr, uc_line_addr);
                do_prefetch_2_hash = false;
              }

              // enqueue the cacheline into the timely fifo (cl_candidates)
              bool  cl_candidate_found = false;
              for (auto it = cl_candidates.begin(); it != cl_candidates.end(); ++it) {
                if (it->first == line_addr) {
                  cl_candidate_found = true;
                  DEBUG(ic_stage->proc_id, "cl_candidate found for cl 0x%llx\n", line_addr);
                  break;
                }
              }
              if (!cl_candidate_found) {
                if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                  auto popped_cl = cl_candidates.front();
                  Addr repl_uc_line_addr;
                  uc_line_addr = popped_cl.first;
                  Addr dummy_uc_line_addr = 0;
                  void*cnt;
                  if(!FDIP_BLOOM_FILTER) {
                    cnt = (void*)cache_access(&fdip_cc, uc_line_addr, &dummy_uc_line_addr, TRUE);
                  } else {
                    cnt = bloom_lookup(uc_line_addr);
                  }
                  DEBUG(ic_stage->proc_id, "cl_candidate popped for cl 0x%llx", popped_cl.first);
                  if (popped_cl.second == TRUE) {
                    fdip_inc_useful_hash(popped_cl.first);
                    if(!cnt) {
                      if(!FDIP_BLOOM_FILTER) {
                        cache_insert_replpos(&fdip_cc, ic_stage->proc_id, uc_line_addr, &dummy_uc_line_addr,
                                             &repl_uc_line_addr, (Cache_Insert_Repl)FDIP_CC_INSERT_REPLPOL, FALSE);
                      } else {
                        detect_stream(uc_line_addr);
                      }
                    }
                    DEBUG(ic_stage->proc_id, " useful\n");
                  } else {
                    DEBUG(ic_stage->proc_id, " unuseful\n");
                  }
                  cl_candidates.pop_front();
                  cl_candidates_popped = true;
                  cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                  DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
                } else {
                  cl_candidates.push_back(std::pair<Addr, Flag>(line_addr, FALSE));
                  DEBUG(ic_stage->proc_id, "cl_candidate pushed for cl 0x%llx\n", line_addr);
                  if (cl_candidates.size() == FDIP_TIMELY_FIFO_SIZE) {
                    cl_candidates_popped = true;
                    DEBUG(ic_stage->proc_id, "cl_candidates are firstly filled full\n");
                  }
                }
              }
              if (do_prefetch != do_prefetch_2_hash) {
                DEBUG(ic_stage->proc_id, "hash and cache returns different decisions!\n");
              }
            }
          }
        }
      }
    }

    // Break on lookahead buffer limit
    if (last_runahead_op == max_runahead_op) {
      break_reason = BR_LOOKAHEAD_BUFFER_LIMIT;
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
        DEBUG(ic_stage->proc_id, "[%lld] [fdip_update] is_branch (on path) - fetch_addr: %llx, op->inst_uid: %llu, op->op_num: %llu, ftq.size(): %lu, op->table_info->cf_type: %d\n", unique_count, op->fetch_addr, op->inst_uid, op->op_num, ftq.size(), op->table_info->cf_type);
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

        if (op->oracle_info.pred) {
          taken_branches++;
          DEBUG(ic_stage->proc_id, "predict T\n");
        } else {
          DEBUG(ic_stage->proc_id, "predict NT\n");
        }

        if (op->oracle_info.mispred || op->oracle_info.misfetch) {
          off_count++;
          fdip_on_path_bp = FALSE;
          if (op->oracle_info.mispred) {
            DEBUG(ic_stage->proc_id, "mispred\n");
          }
          if (op->oracle_info.misfetch) {
            DEBUG(ic_stage->proc_id, "misfetch\n");
          }
          if ((FDIP_STOP_ON_MISPRED && op->oracle_info.mispred) || (FDIP_STOP_ON_MISFETCH && op->oracle_info.misfetch))
            runahead_disable = TRUE;
        } else if (btb_ras_miss) {
          ASSERT(ic_stage->proc_id, !op->oracle_info.mispred && !op->oracle_info.misfetch);
          off_count++;
          fdip_on_path_bp = FALSE;
          DEBUG(ic_stage->proc_id, "btb_ras_miss, target: %llx\n", target);
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
          DEBUG(ic_stage->proc_id, "[fdip_update] off-path prediction btb_target: %llx, predicted_target: %llx, predicted taken: %d\n", *btb_target, target, op->oracle_info.pred? 1 : 0);
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
        if (success) {
          STAT_EVENT(ic_stage->proc_id, FDIP_PREFETCHES);
          if (success == SUCCESS_NEW) {
            STAT_EVENT(ic_stage->proc_id, FDIP_NEW_PREFETCHES);
            if (FDIP_HASH_ENABLE && !WARMUP && !FDIP_TIMELY_FIFO_SIZE)
              fdip_insert_cl_fetch_addr(line_addr);
          }
          DEBUG(ic_stage->proc_id, "[%llu] new_mem_req for cl%llx success\n", cycle_count, line_addr);
        }
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
      DEBUG(ic_stage->proc_id, "break due to off path\n");
      break;
    }
  }

  switch(break_reason) {
    case BR_NO_BREAK:
      STAT_EVENT(ic_stage->proc_id, FDIP_NO_BREAK);
      break;
    case BR_CACHELINE:
      DEBUG(ic_stage->proc_id, "break on a cache line\n");
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_CACHELINE);
      break;
    case BR_MAX_TAKEN_BRANCHES:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_MAX_TAKEN_BRANCHES);
      DEBUG(ic_stage->proc_id, "break due to taken branches\n");
      break;
    case BR_FTQ:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_FULL_FTQ);
      DEBUG(ic_stage->proc_id, "break due to FTQ depth\n");
      break;
    case BR_RUNAHEAD_FIFO:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_RUNAHEAD_FIFO);
      DEBUG(ic_stage->proc_id, "break due to runahead fifo\n");
      break;
    case BR_LOOKAHEAD_BUFFER_LIMIT:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_LOOKAHEAD_BUFFER);
      DEBUG(ic_stage->proc_id, "break due to max runahead op (lookahead buf)\n");
      break;
    case BR_TAGE_BUFFER_LIMIT:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_TAGE_BUFFER_LIMIT);
      DEBUG(ic_stage->proc_id, "break due to tage buffer limit\n");
      break;
    case BR_MEM_REQ_BUF_LIMIT:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_FULL_MEM_REQ_BUF);
      DEBUG(ic_stage->proc_id, "break due to l1 mem req queue\n");
      break;
    case BR_CFS_PER_CYCLE:
      STAT_EVENT(ic_stage->proc_id, FDIP_BREAK_ON_CFS_PER_CYCLE);
      DEBUG(ic_stage->proc_id, "break due to cfs per cycle\n");
      break;
    default:
      break;
  }

  if (last_runahead_pc != runahead_pc)
    fdip_ftq_pos++;
  last_runahead_pc = runahead_pc;
  STAT_EVENT(ic_stage->proc_id, FDIP_CYCLE_COUNT);
}

Flag fdip_pred_off_path(void) {
  return !fdip_on_path_bp;
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

void fdip_inc_cnt_useful(Addr line_addr) {
  auto useful_iter = cnt_useful.find(line_addr);
  if (useful_iter == cnt_useful.end())
    cnt_useful.insert(std::pair<Addr, Counter>(line_addr, 1));
  else
    useful_iter->second++;
}

void fdip_inc_cnt_unuseful(Addr line_addr) {
  auto unuseful_iter = cnt_unuseful.find(line_addr);
  if (unuseful_iter == cnt_unuseful.end())
    cnt_unuseful.insert(std::pair<Addr, Counter>(line_addr, 1));
  else
    unuseful_iter->second++;
}

void fdip_insert_cl_fetch_addr(Addr line_addr) {
  auto cl_addr_iter = std::find(fetch_cl_addr.begin(), fetch_cl_addr.end(), line_addr);

  if (cl_addr_iter == fetch_cl_addr.end())
    fetch_cl_addr.push_back(line_addr);
}

void fdip_remove_cl_fetch_addr(Addr line_addr) {
  auto cl_addr_iter = std::find(fetch_cl_addr.begin(), fetch_cl_addr.end(), line_addr);
  if (cl_addr_iter != fetch_cl_addr.end())
    fetch_cl_addr.erase(cl_addr_iter);
}

void fdip_print_hash_tables(void) {
  if (!FDIP_ENABLE)
    return;
  DEBUG(ic_stage->proc_id, "=============cnt_useful============== size: %lu\n", cnt_useful.size());
  INC_STAT_EVENT(ic_stage->proc_id, FDIP_USEFUL_CACHELINES, cnt_useful.size());
  for(std::unordered_map<Addr, Counter>::const_iterator it = cnt_useful.begin();
        it != cnt_useful.end(); ++it)
    DEBUG(ic_stage->proc_id, "0x%llx - %llu\n", it->first, it->second);
  DEBUG(ic_stage->proc_id, "=============useful hash============= size: %lu\n", useful_hash.size());
  INC_STAT_EVENT(ic_stage->proc_id, FDIP_USEFUL_CACHELINES_HASH, useful_hash.size());
  for(std::unordered_map<Addr, Counter>::const_iterator it = useful_hash.begin();
    it != useful_hash.end(); ++it)
  DEBUG(ic_stage->proc_id, "0x%llx - %llu\n", it->first, it->second);
  DEBUG(ic_stage->proc_id, "=============cnt_unuseful============== size: %lu\n", cnt_unuseful.size());
  INC_STAT_EVENT(ic_stage->proc_id, FDIP_UNUSEFUL_CACHELINES, cnt_unuseful.size());
  for(std::unordered_map<Addr, Counter>::const_iterator it = cnt_unuseful.begin();
     it != cnt_unuseful.end(); ++it) {
    DEBUG(ic_stage->proc_id, "0x%llx - %llu\n", it->first, it->second);
    auto useful_iter = cnt_useful.find(it->first);
    if (useful_iter != cnt_useful.end() && it->second >= useful_iter->second) {
      DEBUG(ic_stage->proc_id, "useful count (%llu) is smaller\n", useful_iter->second);
    }
  }
  DEBUG(ic_stage->proc_id, "=============fetch cl addresses not touched ever ===== size: %lu\n", fetch_cl_addr.size());
  INC_STAT_EVENT(ic_stage->proc_id, FDIP_NOT_TOUCHED_CACHELINES, fetch_cl_addr.size());
  for(std::vector<Counter>::const_iterator it = fetch_cl_addr.begin();
      it != fetch_cl_addr.end(); ++it) {
    DEBUG(ic_stage->proc_id, "0x%llx has never touched\n", *it);
  }
}

void fdip_print_usefulness_cache(void) {
  if (!FDIP_ENABLE)
    return;
  DEBUG(ic_stage->proc_id, "=============useful cachelines===========\n");
  uint32_t ii, jj;
  uint32_t cnt = 0;
  Cache* cache = &fdip_cc;

  for(ii = 0; ii < cache->num_sets; ii++) {
    for(jj = 0; jj < cache->assoc; jj++) {
      if(cache->entries[ii][jj].valid) {
        cnt++;
        DEBUG(ic_stage->proc_id, "tag 0x%llx\n", cache->entries[ii][jj].tag);
      }
    }
  }
  INC_STAT_EVENT(ic_stage->proc_id, FDIP_CC_USEFUL_LINES, cnt);
}

void fdip_touch_cl_candidates(Addr line_addr) {
  for (auto it = cl_candidates.begin(); it != cl_candidates.end(); ++it) {
    if (it->first == line_addr) {
      it->second = TRUE;
      break;
    }
  }
}

void fdip_inc_useful_hash(Addr line_addr) {
  auto useful_cl_iter = useful_hash.find(line_addr);
  if (useful_cl_iter == useful_hash.end())
    useful_hash.insert(std::pair<Addr, Counter>(line_addr, 1));
  else {
    useful_cl_iter->second++;
  }
}

void fdip_dec_useful_hash(Addr line_addr) {
  auto useful_cl_iter = useful_hash.find(line_addr);
  if (useful_cl_iter != useful_hash.end()) {
    if (useful_cl_iter->second)
      useful_cl_iter->second--;
    else
      useful_hash.erase(useful_cl_iter);
  }
}
