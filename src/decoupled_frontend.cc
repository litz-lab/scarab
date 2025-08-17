#include "decoupled_frontend.h"

#include <cmath>
#include <deque>
#include <iostream>
#include <tuple>
#include <vector>

#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"

#include "frontend/frontend_intf.h"
#include "isa/isa_macros.h"

#include "ft.h"
#include "op.h"
#include "op_pool.h"
#include "thread.h"

#include "confidence/conf.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

class Decoupled_FE {
 public:
  Decoupled_FE(uns _proc_id);
  int is_off_path() { return is_off_path_state(); }
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
  Op* get_cur_op() { return cur_op; }
  uns get_conf() { return conf->get_conf(); }
  Off_Path_Reason get_off_path_reason() { return conf->get_off_path_reason(); }
  Conf_Off_Path_Reason get_conf_off_path_reason() { return conf->get_conf_off_path_reason(); }
  void conf_resolve_cf(Op* op) { conf->resolve_cf(op); }
  Off_Path_Reason eval_off_path_reason(Op* op);
  void print_conf_data() { conf->print_data(); }

  // FSM states for DFE
  enum DFE_STATE {
    SERVING_ON_PATH,
    SERVING_OFF_PATH,
    RECOVERING,
    EXITING
    // Add more states as needed
  };

 private:
  void init(uns proc_id);

  uns proc_id;

  // Per core fetch target queue:
  // Each core has a queue of FTs,
  // where each FT contains a queue of micro instructions.
  std::deque<FT> ftq;
  // keep track of the current FT to be pushed next
  FT current_ft_to_push;
  FT saved_recovery_ft;

  int sched_off_path;
  uint64_t dfe_op_count;
  uint64_t current_off_path_dfe_op_count;
  std::vector<decoupled_fe_iter> ftq_iterators;
  uint64_t recovery_addr;
  uint64_t redirect_cycle;
  bool stalled;
  uint64_t ftq_ft_num;
  bool trace_mode;
  Op* cur_op;
  Conf* conf;

  DFE_STATE state;  // FSM state
  bool is_off_path_state() const { return state == SERVING_OFF_PATH; }

  void validate_ft_and_push_to_ftq(FT& current_ft_to_push);
  void process_on_path_ft(FT& current_ft_to_push, FT_PredictResult result);
  inline FT_BuildResult build_off_path_ft(FT& ft, uint64_t op_count);
  inline uint64_t FTQ_MAX_SIZE() { return ftq_ft_num; }
};

/* Global Variables */
Decoupled_FE* dfe = nullptr;
Flag have_seen_exit_on_prebuilt = 0;

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

void reset_decoupled_fe() {
}

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

void decoupled_fe_retire(Op* op, int op_proc_id, uns64 inst_uid) {
  dfe->retire(op, op_proc_id, inst_uid);
}

void decoupled_fe_set_ftq_num(uint64_t ftq_ft_num) {
  dfe->set_ftq_num(ftq_ft_num);
}

uint64_t decoupled_fe_get_ftq_num() {
  return dfe->get_ftq_num();
}

Op* decoupled_fe_get_cur_op() {
  return dfe->get_cur_op();
}

uns decoupled_fe_get_conf() {
  return dfe->get_conf();
}

Off_Path_Reason decoupled_fe_get_off_path_reason() {
  return dfe->get_off_path_reason();
}

Conf_Off_Path_Reason decoupled_fe_get_conf_off_path_reason() {
  return dfe->get_conf_off_path_reason();
}

void decoupled_fe_conf_resovle_cf(Op* op) {
  dfe->conf_resolve_cf(op);
}

void decoupled_fe_print_conf_data() {
  dfe->print_conf_data();
}

/* Decoupled_FE member functions */
Decoupled_FE::Decoupled_FE(uns _proc_id) : proc_id(_proc_id), current_ft_to_push(_proc_id) {
  init(_proc_id);
}

void Decoupled_FE::init(uns _proc_id) {
  trace_mode = false;

#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  proc_id = _proc_id;
  sched_off_path = false;
  dfe_op_count = 1;
  recovery_addr = 0;
  redirect_cycle = 0;
  stalled = false;
  ftq_ft_num = FE_FTQ_BLOCK_NUM;
  cur_op = nullptr;

  current_ft_to_push = FT(proc_id);

  if (CONFIDENCE_ENABLE)
    conf = new Conf(_proc_id);

  state = SERVING_ON_PATH;
}

void Decoupled_FE::recover() {
  sched_off_path = false;
  cur_op = nullptr;
  recovery_addr = bp_recovery_info->recovery_fetch_addr;

  for (auto it = ftq.begin(); it != ftq.end(); it++) {
    it->free_ops_and_clear();
  }
  ftq.clear();

  current_ft_to_push.free_ops_and_clear();
  DEBUG(proc_id, "Recovery signalled fetch_addr0x:%llx\n", bp_recovery_info->recovery_fetch_addr);

  for (auto it = ftq_iterators.begin(); it != ftq_iterators.end(); it++) {
    // When the FTQ flushes, reset all iterators
    it->ft_pos = 0;
    it->op_pos = 0;
    it->flattened_op_pos = 0;
  }

  auto op = bp_recovery_info->recovery_op;

  if (stalled) {
    DEBUG(proc_id, "Unstalled off-path fetch barrier due to recovery fetch_addr0x:%llx off_path:%i op_num:%llu\n",
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

  // FIXME always fetch off path ops? should we get rid of this parameter?
  frontend_recover(proc_id, bp_recovery_info->recovery_inst_uid);
  if (state != EXITING) {
    ASSERT(proc_id, saved_recovery_ft.get_size() != 0);
    ASSERTM(proc_id, bp_recovery_info->recovery_fetch_addr == saved_recovery_ft.ft_info.static_info.start,
            "Scarab's recovery addr 0x%llx does not match save ft"
            "addr 0x%llx\n",
            bp_recovery_info->recovery_fetch_addr, saved_recovery_ft.ft_info.static_info.start);
    state = RECOVERING;
  }

  if (CONFIDENCE_ENABLE)
    conf->recover(op);
}

void Decoupled_FE::update() {
  uint64_t cfs_taken_this_cycle = 0;
  static int fwd_progress = 0;
  fwd_progress++;
  if (fwd_progress >= 1000000) {
    std::cout << "No forward progress for 1000000 cycles" << std::endl;
    ASSERT(0, 0);
  }
  if (is_off_path_state())
    STAT_EVENT(proc_id, FTQ_CYCLES_OFFPATH);
  else
    STAT_EVENT(proc_id, FTQ_CYCLES_ONPATH);

  // pop used fts in the front of the ftq
  pop_fts();
  // update per-cycle confidence mechanism state
  if (CONFIDENCE_ENABLE)
    conf->per_cycle_update();

  while (1) {
    ASSERT(proc_id, ftq.size() <= FTQ_MAX_SIZE());
    ASSERT(proc_id, cfs_taken_this_cycle <= FE_FTQ_TAKEN_CFS_PER_CYCLE);

    if (ftq.size() == FTQ_MAX_SIZE()) {
      DEBUG(proc_id, "Break due to full FTQ\n");
      STAT_EVENT(proc_id, FTQ_BREAK_FULL_FT_ONPATH + is_off_path_state());
      break;
    }
    if (BP_MECH != MTAGE_BP && !bp_is_predictable(g_bp_data, proc_id)) {
      DEBUG(proc_id, "Break due to limited branch predictor\n");
      STAT_EVENT(proc_id, FTQ_BREAK_PRED_BR_ONPATH + is_off_path_state());
      break;
    }
    if (stalled) {
      DEBUG(proc_id, "Break due to wait for fetch barrier resolved\n");
      STAT_EVENT(proc_id, FTQ_BREAK_BAR_FETCH_ONPATH + is_off_path_state());
      break;
    }

    fwd_progress = 0;
    // FSM-based FT build logic - four states:
    // EXITING: stop whend end of track seen
    // RECOVERING: handle recovery after misprediction
    // SERVING_ON_PATH: normal execution mode
    // SERVING_OFF_PATH: fetching off-path operations
    FT_PredictResult result;
    switch (state) {
      case EXITING:
        return;

      case RECOVERING: {
        // After recovery, we expect to serve the saved recovery FT
        state = SERVING_ON_PATH;
        ASSERT(proc_id, saved_recovery_ft.get_size() != 0);
        current_ft_to_push = saved_recovery_ft;
        saved_recovery_ft.clear();
        result = current_ft_to_push.predict_ft();

        process_on_path_ft(current_ft_to_push, result);
        break;
      }
      // recover will fall through to on-path exec
      case SERVING_ON_PATH: {
        // Build new on-path FT if no recovery ft availble
        auto build_result = current_ft_to_push.build_full_ft([](uns8 pid) { return frontend_can_fetch_op(pid); },
                                                             [](uns8 pid, Op* op) -> bool {
                                                               frontend_fetch_op(pid, op);
                                                               return true;
                                                             },
                                                             false, false, dfe_op_count);
        ASSERT(proc_id, build_result.build_complete);
        dfe_op_count += current_ft_to_push.get_op_count();
        result = current_ft_to_push.predict_ft();
        // if current FT is the exit one, skip mispredict handling and directly push
        // set state and early return
        if (current_ft_to_push.ended_by_exit()) {
          validate_ft_and_push_to_ftq(current_ft_to_push);
          state = EXITING;
          return;
        }
        process_on_path_ft(current_ft_to_push, result);
        break;
      }

      case SERVING_OFF_PATH: {
        // for off-path just build and. redirect
        // cf processed while building
        ASSERT(proc_id, current_ft_to_push.get_size() == 0);
        build_off_path_ft(current_ft_to_push, current_off_path_dfe_op_count);
        current_off_path_dfe_op_count += current_ft_to_push.get_op_count();
        break;
      }
    }
    STAT_EVENT(proc_id, DFE_GEN_ON_PATH_FT + is_off_path_state());
    validate_ft_and_push_to_ftq(current_ft_to_push);
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

Op* Decoupled_FE::ftq_iter_get_next(decoupled_fe_iter* iter, bool* end_of_ft) {
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

void Decoupled_FE::stall(Op* op) {
  stalled = true;
  DEBUG(proc_id, "Decoupled fetch stalled due to barrier fetch_addr0x:%llx off_path:%i op_num:%llu\n",
        op->inst_info->addr, op->off_path, op->op_num);
}

void Decoupled_FE::retire(Op* op, int op_proc_id, uns64 inst_uid) {
  if ((op->table_info->bar_type & BAR_FETCH) || IS_CALLSYS(op->table_info)) {
    stalled = false;
    DEBUG(proc_id,
          "Decoupled fetch unstalled due to retired barrier fetch_addr0x:%llx off_path:%i op_num:%llu list_count:%i\n",
          op->inst_info->addr, op->off_path, op->op_num, td->seq_op_list.count);
    ASSERT(proc_id, td->seq_op_list.count == 1);
  }

  // unblock pin exec driven, trace frontends do not need to block/unblock
  frontend_retire(op_proc_id, inst_uid);
}

Off_Path_Reason Decoupled_FE::eval_off_path_reason(Op* op) {
  if (!(op->oracle_info.recover_at_decode || op->oracle_info.recover_at_exec)) {
    return REASON_NOT_IDENTIFIED;
  }
  // mispred
  if (op->oracle_info.pred_orig != op->oracle_info.dir && !op->oracle_info.btb_miss) {
    return REASON_MISPRED;
  }
  // misfetch
  else if (!op->oracle_info.btb_miss && op->oracle_info.pred_orig == op->oracle_info.dir &&
           op->oracle_info.pred_npc != op->oracle_info.npc) {
    return REASON_MISFETCH;
  }
  // ibtb miss
  else if (ENABLE_IBP && (op->table_info->cf_type == CF_IBR || op->table_info->cf_type == CF_ICALL) &&
           op->oracle_info.btb_miss && op->oracle_info.ibp_miss && op->oracle_info.pred_orig == TAKEN) {
    return REASON_IBTB_MISS;
  }
  // btb miss and mispred (would have been incorrect with or without btb miss)
  else if (op->oracle_info.pred_orig != op->oracle_info.dir && op->oracle_info.btb_miss) {
    return REASON_BTB_MISS_MISPRED;
  }
  // true btb miss
  else if (op->oracle_info.btb_miss) {
    return REASON_BTB_MISS;
  } else {
    // all cases should be covered
    ASSERT(proc_id, FALSE);
  }
}

void Decoupled_FE::validate_ft_and_push_to_ftq(FT& current_ft_to_push) {
  current_ft_to_push.set_per_op_ft_info();
  if (ftq.size())
    ASSERT(proc_id, current_ft_to_push.is_consecutive(ftq.back()));
  ftq.emplace_back(current_ft_to_push);
  if (CONFIDENCE_ENABLE)
    conf->update(current_ft_to_push);
  if (recovery_addr) {
    ASSERT(proc_id, recovery_addr == current_ft_to_push.get_start_addr());
    recovery_addr = 0;
  }
  current_ft_to_push.clear();
}

void Decoupled_FE::process_on_path_ft(FT& current_ft_to_push, FT_PredictResult result) {
  // misprediction and redirection handling
  ASSERT(proc_id, result.event != FT_EVENT_OFFPATH_TAKEN_REDIRECT);
  if (result.event == FT_EVENT_MISPREDICT) {
    // Misprediction: Switch to off-path execution
    FT tailing_ft;
    bool need_rebuild = current_ft_to_push.split_ft(result.index, tailing_ft);
    // if we have a tailing ft, save it for recovery
    if (tailing_ft.get_size() != 0) {
      saved_recovery_ft = tailing_ft;
    }
    // if mispred happens at the last op of the on-path FT, we fetch the next on-path ft then redirect
    else {
      ASSERT(proc_id, result.index == current_ft_to_push.get_op_count() - 1);
      ASSERT(proc_id, saved_recovery_ft.get_size() == 0);
      auto build_result = saved_recovery_ft.build_full_ft([](uns8 pid) { return frontend_can_fetch_op(pid); },
                                                          [](uns8 pid, Op* op) -> bool {
                                                            frontend_fetch_op(pid, op);
                                                            return true;
                                                          },
                                                          false, false, dfe_op_count);
      ASSERT(proc_id, build_result.build_complete);
      dfe_op_count += saved_recovery_ft.get_op_count();
    }
    redirect_cycle = cycle_count;
    state = SERVING_OFF_PATH;
    frontend_redirect(proc_id, result.op->inst_uid, result.pred_addr);
    // set off-path op count when going off-path
    current_off_path_dfe_op_count = current_ft_to_push.get_last_op()->op_num + 1;
    // patching/modify the current FT if current FT not ended
    if (need_rebuild) {
      auto n_uop_before_padding = current_ft_to_push.get_op_count();
      auto build_result = build_off_path_ft(current_ft_to_push, current_off_path_dfe_op_count);
      current_off_path_dfe_op_count += current_ft_to_push.get_op_count() - n_uop_before_padding;
      ASSERT(proc_id, build_result.build_complete);
    }
    ASSERT(proc_id, current_ft_to_push.ended());
  } else if (result.event == FT_EVENT_FETCH_BARRIER) {
    stall(result.op);
  }
}

inline FT_BuildResult Decoupled_FE::build_off_path_ft(FT& ft, uint64_t op_count) {
  auto build_result = ft.build_full_ft([](uns8 pid) { return frontend_can_fetch_op(pid); },
                                       [](uns8 pid, Op* op) -> bool {
                                         frontend_fetch_op(pid, op);
                                         return true;
                                       },
                                       true, true, op_count);
  if (build_result.redirect_needed) {
    frontend_redirect(proc_id, build_result.trigger_op->inst_uid, build_result.redirect_addr);
  } else if (build_result.fetch_bar_needed) {
    stall(build_result.trigger_op);
  }
  return build_result;
}
