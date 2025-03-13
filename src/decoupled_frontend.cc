#include "decoupled_frontend.h"
#include "frontend/frontend_intf.h"
#include "op.h"
#include "op_pool.h"
#include "thread.h"
#include "isa/isa_macros.h"
#include "prefetcher/pref.param.h"
#include "memory/memory.param.h"

#include <deque>
#include <vector>
#include <iostream>
#include <tuple>
#include <cmath>

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

class FT{
 public:
  FT(uns _proc_id);
  void set_ft_started_by(FT_Started_By ft_started_by);
  void add_op(Op *op, FT_Ended_By ft_ended_by);
  void free_ops_and_clear();
  bool can_fetch_op();
  Op* fetch_op();
  void set_per_op_ft_info();
  FT_Info get_ft_info();
  bool is_consumed();
  void set_consumed();

 private:
  uns proc_id;
  // indicate the next op index to read by the consumer (icache or uop)
  uint64_t op_pos;
  FT_Info ft_info;
  std::vector<Op*> ops;
  bool consumed;

  friend class Decoupled_FE;
};

class Decoupled_FE {
public:
  Decoupled_FE(uns _proc_id);
  int is_off_path() { return off_path; }
  void recover();
  void update();
  FT* get_ft(uint64_t ft_pos);
  void pop_fts();
  decoupled_fe_iter* new_ftq_iter();
  Op* ftq_iter_get(decoupled_fe_iter* iter, bool* end_of_ft);
  Op* ftq_iter_get_next(decoupled_fe_iter* iter, bool* end_of_ft);
  uint64_t ftq_num_ops();
  uint64_t ftq_num_fts() { return ftq.size(); }
  void stall(Op* op);
  void retire(Op* op, int op_proc_id, uns64 inst_uid);
  void set_ftq_num(uint64_t set_ftq_ft_num) { ftq_ft_num = set_ftq_ft_num; }
  uint64_t get_ftq_num() { return ftq_ft_num; }

private:
  void init(uns proc_id);

  uns proc_id;

  // Per core fetch target queue:
  // Each core has a queue of FTs,
  // where each FT contains a queue of micro instructions.
  std::deque<FT> ftq;
  // keep track of the current FT to be pushed next
  FT current_ft_to_push;

  int off_path;
  int sched_off_path;
  uint64_t dfe_op_count;
  std::vector<decoupled_fe_iter> ftq_iterators;
  uint64_t recovery_addr;
  uint64_t redirect_cycle;
  bool stalled;
  uint64_t ftq_ft_num;
  bool trace_mode;
};

/* Global Variables */
Decoupled_FE* dfe = nullptr;

// Per core decoupled frontend
std::vector<Decoupled_FE> per_core_dfe;

/* Wrapper functions */
void alloc_mem_decoupled_fe(uns numCores) {
  for (uns i = 0; i < numCores; ++i)
    per_core_dfe.push_back(Decoupled_FE(i));
  ASSERT(0, per_core_dfe.size() == numCores);
}

void init_decoupled_fe(uns proc_id, const char*) {
}

bool decoupled_fe_is_off_path() {
  return dfe->is_off_path();
}

void set_decoupled_fe(uns proc_id) {
  dfe = &per_core_dfe[proc_id];
  ASSERT(proc_id, dfe);
}

void reset_decoupled_fe() {}

void recover_decoupled_fe() {
  dfe->recover();
}

void debug_decoupled_fe() {

}

void update_decoupled_fe() {
  dfe->update();
}

FT* decoupled_fe_get_ft(uint64_t ft_pos) {
  return dfe->get_ft(ft_pos);
}

decoupled_fe_iter* decoupled_fe_new_ftq_iter(uns proc_id) {
  return per_core_dfe[proc_id].new_ftq_iter();
}

/* Returns the Op at current FTQ iterator position. Returns NULL if the FTQ is empty */
Op* decoupled_fe_ftq_iter_get(decoupled_fe_iter* iter, bool* end_of_ft) {
  return dfe->ftq_iter_get(iter, end_of_ft);
}

/* Increments the iterator and returns the Op at FTQ iterator position. Returns NULL if the FTQ is empty */
Op* decoupled_fe_ftq_iter_get_next(decoupled_fe_iter* iter, bool* end_of_ft) {
  return dfe->ftq_iter_get_next(iter, end_of_ft);
}

/* Returns iter flattened offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_offset(decoupled_fe_iter* iter) {
  return iter->flattened_op_pos;
}

/* Returns iter ft offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_ft_offset(decoupled_fe_iter* iter) {
  return iter->ft_pos;
}

uint64_t decoupled_fe_ftq_num_ops() {
  return dfe->ftq_num_ops();
}

uint64_t decoupled_fe_ftq_num_fts() {
  return dfe->ftq_num_fts();
}

void decoupled_fe_retire(Op *op, int op_proc_id, uns64 inst_uid) {
  dfe->retire(op, op_proc_id, inst_uid);
}

void decoupled_fe_set_ftq_num(uint64_t ftq_ft_num) {
  dfe->set_ftq_num(ftq_ft_num);
}

uint64_t decoupled_fe_get_ftq_num() {
  return dfe->get_ftq_num();
}

/* FT member functions */
FT::FT(uns _proc_id = 0) {
  proc_id = _proc_id;
  consumed = false;
  free_ops_and_clear();
}

void FT::free_ops_and_clear() {
  while (op_pos < ops.size()) {
    free_op(ops[op_pos]);
    op_pos++;
  }

  ops.clear();
  op_pos = 0;
  ft_info.static_info.start = 0;
  ft_info.static_info.length = 0;
  ft_info.static_info.n_uops = 0;
  ft_info.dynamic_info.started_by = FT_NOT_STARTED;
  ft_info.dynamic_info.ended_by = FT_NOT_ENDED;
  ft_info.dynamic_info.first_op_off_path = FALSE;
}

bool FT::can_fetch_op() {
  return op_pos < ops.size();
}

Op* FT::fetch_op() {
  ASSERT(proc_id, can_fetch_op());
  Op* op = ops[op_pos];
  op_pos++;

  DEBUG(proc_id,
        "Fetch op from FT fetch_addr0x:%llx off_path:%i op_num:%llu\n",
        op->inst_info->addr, op->off_path, op->op_num);

  return op;
}

void FT::set_per_op_ft_info() {
  for (auto op : ops) {
    op->ft_info = ft_info;
  }
}

void FT::set_ft_started_by(FT_Started_By ft_started_by) {
  ft_info.dynamic_info.started_by = ft_started_by;
}

void FT::add_op(Op *op, FT_Ended_By ft_ended_by) {
  if (ops.empty()) {
    ASSERT(proc_id, op->bom && !ft_info.static_info.start);
    ft_info.static_info.start = op->inst_info->addr;
    ft_info.dynamic_info.first_op_off_path = op->off_path;
  } else {
    if (op->bom) {
      // assert consecutivity
      ASSERT(proc_id, ops.back()->inst_info->addr + ops.back()->inst_info->trace_info.inst_size
                      == op->inst_info->addr);
    } else {
      // assert all uops of the same inst share the same addr
      ASSERT(proc_id, ops.back()->inst_info->addr == op->inst_info->addr);
    }
  }
  ops.emplace_back(op);
  if (ft_ended_by != FT_NOT_ENDED) {
    ASSERT(proc_id, op->eom && !ft_info.static_info.length);
    ASSERT(proc_id, ft_info.static_info.start);
    ft_info.static_info.n_uops = ops.size();
    ft_info.static_info.length = op->inst_info->addr + op->inst_info->trace_info.inst_size - ft_info.static_info.start;
    ASSERT(proc_id, ft_info.dynamic_info.ended_by == FT_NOT_ENDED);
    ft_info.dynamic_info.ended_by = ft_ended_by;

    // counting extremely short FT reason
    if (!ft_info.dynamic_info.first_op_off_path) {
      if (ft_info.static_info.n_uops <= (int)ISSUE_WIDTH) {
        if (ft_info.dynamic_info.started_by == FT_STARTED_BY_ICACHE_LINE_BOUNDARY) {
          STAT_EVENT(proc_id, FT_SHORT_ICACHE_LINE_BOUNDARY_ICACHE_LINE_BOUNDARY + ft_ended_by - 1);
          INC_STAT_EVENT(proc_id,
                        FT_SHORT_UOP_LOST_ICACHE_LINE_BOUNDARY_ICACHE_LINE_BOUNDARY + ft_ended_by - 1,
                        ISSUE_WIDTH - ft_info.static_info.n_uops);
        } else if (ft_info.dynamic_info.started_by == FT_STARTED_BY_TAKEN_BRANCH) {
          STAT_EVENT(proc_id, FT_SHORT_TAKEN_BRANCH_ICACHE_LINE_BOUNDARY + ft_ended_by - 1);
          INC_STAT_EVENT(proc_id,
                        FT_SHORT_UOP_LOST_TAKEN_BRANCH_ICACHE_LINE_BOUNDARY + ft_ended_by - 1,
                        ISSUE_WIDTH - ft_info.static_info.n_uops);
        } else if (ft_info.dynamic_info.started_by == FT_STARTED_BY_RECOVERY) {
          STAT_EVENT(proc_id, FT_SHORT_RECOVERY_ICACHE_LINE_BOUNDARY + ft_ended_by - 1);
          INC_STAT_EVENT(proc_id,
                        FT_SHORT_UOP_LOST_RECOVERY_ICACHE_LINE_BOUNDARY + ft_ended_by - 1,
                        ISSUE_WIDTH - ft_info.static_info.n_uops);
        } else {
          STAT_EVENT(proc_id, FT_SHORT_OTHER + ft_ended_by - 1);
          INC_STAT_EVENT(proc_id, FT_SHORT_UOP_LOST_OTHER, ISSUE_WIDTH - ft_info.static_info.n_uops);
        }
      } else {
        STAT_EVENT(proc_id, FT_NOT_SHORT);
      }
    }
  }
}

FT_Info FT::get_ft_info() {
  return ft_info;
}

bool FT::is_consumed() {
  return consumed;
}

void FT::set_consumed() {
  consumed = true;
}

/* FT wrappers */
bool ft_can_fetch_op(FT* ft) {
  return ft->can_fetch_op();
}

Op* ft_fetch_op(FT* ft) {
  return ft->fetch_op();
}

bool ft_is_consumed(FT* ft) {
  return ft->is_consumed();
}

void ft_set_consumed(FT* ft) {
  ft->set_consumed();
}

FT_Info ft_get_ft_info(FT* ft) {
  return ft->get_ft_info();
}

/* Decoupled_FE member functions */
Decoupled_FE::Decoupled_FE(uns _proc_id) {
  proc_id = _proc_id;
  init(_proc_id);
}

void Decoupled_FE::init(uns _proc_id) {
  trace_mode = false;

#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  proc_id = _proc_id;
  off_path = false;
  sched_off_path = false;
  dfe_op_count = 1;
  recovery_addr = 0;
  redirect_cycle = 0;
  stalled = false;
  ftq_ft_num = FE_FTQ_BLOCK_NUM;

  current_ft_to_push = FT(proc_id);
  current_ft_to_push.set_ft_started_by(FT_STARTED_BY_APP);
}


void Decoupled_FE::recover() {
  off_path = false;
  sched_off_path = false;
  recovery_addr = bp_recovery_info->recovery_fetch_addr;

  for (auto it = ftq.begin(); it != ftq.end(); it++) {
    it->free_ops_and_clear();
  }
  ftq.clear();

  current_ft_to_push.free_ops_and_clear();
  current_ft_to_push.set_ft_started_by(FT_STARTED_BY_RECOVERY);

  dfe_op_count = bp_recovery_info->recovery_op_num + 1;
  DEBUG(proc_id,
        "Recovery signalled fetch_addr0x:%llx\n", bp_recovery_info->recovery_fetch_addr);

  for (auto it = ftq_iterators.begin(); it != ftq_iterators.end(); it++) {
    // When the FTQ flushes, reset all iterators
    it->ft_pos = 0;
    it->op_pos = 0;
    it->flattened_op_pos = 0;
  }

  auto op = bp_recovery_info->recovery_op;

  if(stalled) {
    DEBUG(proc_id,
          "Unstalled off-path fetch barrier due to recovery fetch_addr0x:%llx off_path:%i op_num:%llu\n",
          op->inst_info->addr, op->off_path, op->op_num);
    stalled = false;
  }

  if (op->oracle_info.recover_at_decode)
    STAT_EVENT(proc_id, FTQ_RECOVER_DECODE);
  else if (op->oracle_info.recover_at_exec)
    STAT_EVENT(proc_id, FTQ_RECOVER_EXEC);

  uint64_t offpath_cycles = cycle_count - redirect_cycle;
  ASSERT(proc_id, cycle_count > redirect_cycle);
  INC_STAT_EVENT(proc_id, FTQ_OFFPATH_CYCLES, offpath_cycles);
  redirect_cycle = 0;

  //FIXME always fetch off path ops? should we get rid of this parameter?
  frontend_recover(proc_id, bp_recovery_info->recovery_inst_uid);
  ASSERTM(proc_id, bp_recovery_info->recovery_fetch_addr == frontend_next_fetch_addr(proc_id),
          "Scarab's recovery addr 0x%llx does not match frontend's recovery "
          "addr 0x%llx\n",
          bp_recovery_info->recovery_fetch_addr, frontend_next_fetch_addr(proc_id));
}

void Decoupled_FE::update() {
  uns cf_num = 0;
  uint64_t bytes_this_cycle = 0;
  uint64_t cfs_taken_this_cycle = 0;
  static int fwd_progress = 0;
  fwd_progress++;
  if (fwd_progress >= 100000) {
    std::cout << "No forward progress for 1000000 cycles" << std::endl;
    ASSERT(0,0);
  }
  if (off_path)
    STAT_EVENT(proc_id, FTQ_CYCLES_OFFPATH);
  else
    STAT_EVENT(proc_id, FTQ_CYCLES_ONPATH);

  // pop used fts in the front of the ftq
  pop_fts();

  while(1) {
    ASSERT(proc_id, ftq_num_fts() <= ftq_ft_num);
    ASSERT(proc_id, cfs_taken_this_cycle <= FE_FTQ_TAKEN_CFS_PER_CYCLE);

    if (ftq_num_fts() == ftq_ft_num) {
      DEBUG(proc_id, "Break due to full FTQ\n");
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_FULL_FT_OFFPATH);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_FULL_FT_ONPATH);
      break;
    }
    if (cfs_taken_this_cycle == FE_FTQ_TAKEN_CFS_PER_CYCLE) {
      DEBUG(proc_id, "Break due to max cfs taken per cycle\n");
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_MAX_CFS_TAKEN_OFFPATH);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_MAX_CFS_TAKEN_ONPATH);
      break;
    }
    // use `>=` because inst size does not necessarily align with FE_FTQ_BYTES_PER_CYCLE
    if (bytes_this_cycle >= FE_FTQ_BYTES_PER_CYCLE) {
      DEBUG(proc_id, "Break due to max bytes per cycle\n");
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_MAX_BYTES_OFFPATH);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_MAX_BYTES_ONPATH);
      break;
    }
    if (BP_MECH != MTAGE_BP && !bp_is_predictable(g_bp_data, proc_id)) {
      DEBUG(proc_id, "Break due to limited branch predictor\n");
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_PRED_BR_OFFPATH);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_PRED_BR_ONPATH);
      break;
    }
    if (stalled) {
      DEBUG(proc_id, "Break due to wait for fetch barrier resolved\n");
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_BAR_FETCH_OFFPATH);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_BAR_FETCH_ONPATH);
      break;
    }
    if (!frontend_can_fetch_op(proc_id)) {
      std::cout << "Warning could not fetch inst from frontend" << std::endl;
      break;
    }

    fwd_progress = 0;
    uint64_t pred_addr = 3;
    Op* op = alloc_op(proc_id);
    frontend_fetch_op(proc_id, op);
    op->op_num = dfe_op_count++;
    op->off_path = off_path;

    if(op->table_info->cf_type) {
      ASSERT(proc_id, op->eom);
      pred_addr = bp_predict_op(g_bp_data, op, cf_num++, op->inst_info->addr);
      DEBUG(proc_id,
            "Predict CF fetch_addr:%llx true_npc:%llx pred_npc:%lx mispred:%i misfetch:%i btb miss:%i taken:%i recover_at_decode:%i recover_at_exec:%i off_path:%i bar_fetch:%i\n",
            op->inst_info->addr, op->oracle_info.npc, pred_addr,
            op->oracle_info.mispred, op->oracle_info.misfetch,
            op->oracle_info.btb_miss, op->oracle_info.pred == TAKEN,
            op->oracle_info.recover_at_decode, op->oracle_info.recover_at_exec,
            off_path, op->table_info->bar_type & BAR_FETCH);

      /* On fetch barrier stall the frontend. Ignore BTB misses here as the exec frontend cannot
         handle recovery/execution until syscalls retire. This is ok as stalling causes the same
         cycle penalty than recovering from BTB miss. */
      if ((op->table_info->bar_type & BAR_FETCH) || IS_CALLSYS(op->table_info)) {
        op->oracle_info.recover_at_decode = FALSE;
        op->oracle_info.recover_at_exec = FALSE;
        if (off_path)
          STAT_EVENT(proc_id, FTQ_SAW_BAR_FETCH_OFFPATH);
        else
          STAT_EVENT(proc_id, FTQ_SAW_BAR_FETCH_ONPATH);
        stall(op);
      }

      if(op->oracle_info.recover_at_decode || op->oracle_info.recover_at_exec) {
        ASSERT(0, (int)op->oracle_info.recover_at_decode + (int)op->oracle_info.recover_at_exec < 2);
        /* If already on the off-path do not schedule recovery as scarab cannot recover OOO
           (An older op may recover at exec and a younger op may recover at decode)
           This is not accurate but it should not affect the time spend on the off-path */
        if (off_path) {
          op->oracle_info.recover_at_decode = FALSE;
          op->oracle_info.recover_at_exec = FALSE;
        }
        off_path = true;
        frontend_redirect(proc_id, op->inst_uid, pred_addr);
        redirect_cycle = cycle_count;
      }
      // If we are already on the off-path redirect on all taken branches in TRACE-MODE
      else if (trace_mode && off_path && op->oracle_info.pred == TAKEN) {
        frontend_redirect(proc_id, op->inst_uid, pred_addr);
      }
    }
    else {
      ASSERT(0,!(op->oracle_info.recover_at_decode | op->oracle_info.recover_at_exec));
      /* On fetch barrier stall the frontend. */
      if (op->table_info->bar_type & BAR_FETCH) {
        if (off_path)
          STAT_EVENT(proc_id, FTQ_SAW_BAR_FETCH_OFFPATH);
        else
          STAT_EVENT(proc_id, FTQ_SAW_BAR_FETCH_ONPATH);
        stall(op);
      }
    }
    // We start a new fetch target if:
    // 1. crossing a icache line
    // 2. taking a control flow op
    // 3. seeing a sysop or serializing (fence) instruction
    // 4. reaching the application exit
    FT_Ended_By ft_ended_by = FT_NOT_ENDED;
    if (op->eom) {
      uns offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size) -
                    ROUND_DOWN(op->inst_info->addr, ICACHE_LINE_SIZE);
      bool end_of_icache_line = offset >= ICACHE_LINE_SIZE;
      bool cf_taken = op->table_info->cf_type && op->oracle_info.pred == TAKEN;
      bool bar_fetch = IS_CALLSYS(op->table_info) || op->table_info->bar_type & BAR_FETCH;

      if (op->exit) {
        ft_ended_by = FT_APP_EXIT;
      } else if (bar_fetch) {
        ft_ended_by = FT_BAR_FETCH;
      } else if (cf_taken) {
        ft_ended_by = FT_TAKEN_BRANCH;
      } else if (end_of_icache_line) {
        ft_ended_by = FT_ICACHE_LINE_BOUNDARY;
      }

      bytes_this_cycle += op->inst_info->trace_info.inst_size;
      cfs_taken_this_cycle += cf_taken || bar_fetch;
    }

    current_ft_to_push.add_op(op, ft_ended_by);
    // ft_ended_by != FT_NOT_ENDED indicates the end of the current fetch target
    // it is now ready to be pushed to the queue
    if (ft_ended_by != FT_NOT_ENDED) {
      ASSERT(proc_id, current_ft_to_push.ft_info.static_info.start && current_ft_to_push.ft_info.static_info.length && current_ft_to_push.ops.size());
      ASSERT(proc_id, current_ft_to_push.ops.front()->bom && current_ft_to_push.ops.back()->eom);
      current_ft_to_push.set_per_op_ft_info();
      if (!ftq.empty()) {
        // sanity check of consecutivity
        Op* last_op = ftq.back().ops.back();
        if (ftq.back().ft_info.dynamic_info.ended_by == FT_TAKEN_BRANCH) {
          ASSERT(proc_id, last_op->oracle_info.pred_npc == current_ft_to_push.ft_info.static_info.start);
        } else if (ftq.back().ft_info.dynamic_info.ended_by == FT_BAR_FETCH) {
          ASSERT(proc_id, last_op->oracle_info.pred_npc == current_ft_to_push.ft_info.static_info.start ||
                              last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size == current_ft_to_push.ft_info.static_info.start);
        } else {
          ASSERT(proc_id, last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size == current_ft_to_push.ft_info.static_info.start);
        }
      }
      ftq.emplace_back(current_ft_to_push);
      current_ft_to_push = FT(proc_id);
      if (ft_ended_by == FT_ICACHE_LINE_BOUNDARY) {
        current_ft_to_push.set_ft_started_by(FT_STARTED_BY_ICACHE_LINE_BOUNDARY);
      } else if (ft_ended_by == FT_TAKEN_BRANCH) {
        current_ft_to_push.set_ft_started_by(FT_STARTED_BY_TAKEN_BRANCH);
      } else if (ft_ended_by == FT_BAR_FETCH) {
        current_ft_to_push.set_ft_started_by(FT_STARTED_BY_BAR_FETCH);
      }
      STAT_EVENT(proc_id, POWER_BTB_READ);
    }

    if (off_path) {
      STAT_EVENT(proc_id, FTQ_FETCHED_INS_OFFPATH);
    }
    else {
      STAT_EVENT(proc_id, FTQ_FETCHED_INS_ONPATH);
    }
      
    DEBUG(proc_id,
          "Push new op to FTQ fetch_addr0x:%llx off_path:%i op_num:%llu dis:%s recovery_addr:%lx fetch_bar:%i\n",
          op->inst_info->addr, op->off_path, op->op_num, disasm_op(op, TRUE), recovery_addr, op->table_info->bar_type & BAR_FETCH);
    // Recovery sanity check
    if (recovery_addr) {
      ASSERT(proc_id, recovery_addr == op->inst_info->addr);
      recovery_addr = 0;
    }
  }
}

FT* Decoupled_FE::get_ft(uint64_t ft_pos) {
  if (ft_pos < ftq.size()) {
    return &ftq[ft_pos];
  } else {
    return NULL;
  }
}

void Decoupled_FE::pop_fts() {
  while (!ftq.empty() && ftq.front().consumed) {
    uint64_t ft_num_ops = ftq.front().ops.size();
    ftq.front().free_ops_and_clear();
    ftq.pop_front();
    for (auto it = ftq_iterators.begin(); it != ftq_iterators.end(); it++) {
      // When the icache consumes an FT decrement the iter's offset so it points to the same entry as before
      if (it->ft_pos > 0) {
        ASSERT(proc_id, it->flattened_op_pos >= ft_num_ops);
        it->flattened_op_pos -= ft_num_ops;
        it->ft_pos--;
      } else {
        ASSERT(proc_id, it->flattened_op_pos < ft_num_ops);
        it->flattened_op_pos = 0;
        it->op_pos = 0;
      }
    }
  }
}

decoupled_fe_iter* Decoupled_FE::new_ftq_iter() {
  ftq_iterators.push_back(decoupled_fe_iter());
  return &(ftq_iterators.back());
}

Op* Decoupled_FE::ftq_iter_get(decoupled_fe_iter* iter, bool* end_of_ft) {
  // if FTQ is empty or if iter has seen all FTs
  if (ftq.empty() || iter->ft_pos == ftq.size()) {
    if (ftq.empty())
      ASSERT(proc_id, iter->ft_pos == 0 && iter->op_pos == 0 && iter->flattened_op_pos == 0);
    return NULL;
  }

  ASSERT(proc_id, iter->ft_pos >= 0);
  ASSERT(proc_id, iter->ft_pos < ftq.size());
  ASSERT(proc_id, iter->op_pos >= 0);
  ASSERT(proc_id, iter->op_pos < ftq.at(iter->ft_pos).ops.size());
  *end_of_ft = iter->op_pos == ftq.at(iter->ft_pos).ops.size() - 1;
  return ftq.at(iter->ft_pos).ops[iter->op_pos];
}

Op* Decoupled_FE::ftq_iter_get_next(decoupled_fe_iter* iter, bool *end_of_ft) {
  if (iter->ft_pos + 1 == ftq.size() && iter->op_pos + 1 == ftq.at(iter->ft_pos).ops.size()) {
    // if iter is at the last op and the last FT
    iter->ft_pos += 1;
    // at this moment iter is at the last FT
    // but later FTQ will receive new FT
    // so we prepare for that case by setting op_pos to zero
    iter->op_pos = 0;
    iter->flattened_op_pos++;
    return NULL;
  } else if (iter->ft_pos == ftq.size()) {
    // if iter has seen all FTs
    ASSERT(proc_id, iter->op_pos == 0);
    return NULL;
  } else if (iter->op_pos + 1 == ftq.at(iter->ft_pos).ops.size()) {
    // if iter is at the last op, but not the last FT
    iter->ft_pos += 1;
    iter->op_pos = 0;
    iter->flattened_op_pos++;
  } else {
    // if iter is not at the last op, nor the last FT
    iter->op_pos++;
    iter->flattened_op_pos++;
  }
  return decoupled_fe_ftq_iter_get(iter, end_of_ft);
}

uint64_t Decoupled_FE::ftq_num_ops() {
  uint64_t num_ops = 0;
  for (auto it = ftq.begin(); it != ftq.end(); it++) {
    num_ops += it->ops.size();
  }
  return num_ops;
}

void Decoupled_FE::stall(Op *op) {
  stalled = true;
  DEBUG(proc_id,
        "Decoupled fetch stalled due to barrier fetch_addr0x:%llx off_path:%i op_num:%llu\n",
        op->inst_info->addr, op->off_path, op->op_num);
}

void Decoupled_FE::retire(Op *op, int op_proc_id, uns64 inst_uid) {
  if((op->table_info->bar_type & BAR_FETCH) || IS_CALLSYS(op->table_info)) {
    stalled = false;
    DEBUG(proc_id,
          "Decoupled fetch unstalled due to retired barrier fetch_addr0x:%llx off_path:%i op_num:%llu list_count:%i\n",
          op->inst_info->addr, op->off_path, op->op_num, td->seq_op_list.count);
    ASSERT(proc_id, td->seq_op_list.count == 1);
  }

  //unblock pin exec driven, trace frontends do not need to block/unblock
  frontend_retire(op_proc_id, inst_uid);
}