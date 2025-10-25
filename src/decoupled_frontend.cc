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
  FT* get_ft();
  void pop_ft(FT* ft);
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
  std::deque<FT*> ftq;
  // keep track of the current FT to be pushed next
  FT* current_ft_to_push;
  FT* saved_recovery_ft;

  int sched_off_path;
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

  void check_consecutivity_and_push_to_ftq();
  void redirect_to_off_path(FT_PredictResult result);
  inline uint64_t ftq_max_size() { return ftq_ft_num; }
};

/* Global Variables */
Decoupled_FE* dfe = nullptr;
Flag have_seen_exit_on_prebuilt = 0;
uint64_t op_num_count = 1;
uint64_t current_off_path_op_num_count = 0;

uint64_t get_next_on_path_op_id() {
  return op_num_count++;
}
uint64_t get_next_off_path_op_id() {
  return current_off_path_op_num_count++;
}
void set_off_path_op_id(uint64_t op_num_count) {
  current_off_path_op_num_count = op_num_count;
};

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

void decoupled_fe_pop_ft(FT* ft) {
  dfe->pop_ft(ft);
}

FT* decoupled_fe_get_ft() {
  return dfe->get_ft();
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
Decoupled_FE::Decoupled_FE(uns _proc_id) : proc_id(_proc_id) {
  init(_proc_id);
}

void Decoupled_FE::init(uns _proc_id) {
  trace_mode = false;

#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  proc_id = _proc_id;
  sched_off_path = false;
  recovery_addr = 0;
  redirect_cycle = 0;
  stalled = false;
  ftq_ft_num = FE_FTQ_BLOCK_NUM;
  cur_op = nullptr;

  current_ft_to_push = nullptr;

  if (CONFIDENCE_ENABLE)
    conf = new Conf(_proc_id);

  state = SERVING_ON_PATH;
}

void Decoupled_FE::recover() {
  sched_off_path = false;
  cur_op = nullptr;
  recovery_addr = bp_recovery_info->recovery_fetch_addr;

  for (auto it = ftq.begin(); it != ftq.end(); it++) {
    delete (*it);
  }
  ftq.clear();

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
    ASSERT(proc_id, saved_recovery_ft->has_unread_ops());
    ASSERTM(proc_id, bp_recovery_info->recovery_fetch_addr == saved_recovery_ft->get_ft_info().static_info.start,
            "Scarab's recovery addr 0x%llx does not match save ft "
            "addr 0x%llx\n",
            bp_recovery_info->recovery_fetch_addr, saved_recovery_ft->get_ft_info().static_info.start);
    state = RECOVERING;
  }

  if (CONFIDENCE_ENABLE)
    conf->recover(op);
}

void Decoupled_FE::update() {
  uint64_t cfs_taken_this_cycle = 0;
  uint64_t ft_pushed_this_cycle = 0;
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

  // update per-cycle confidence mechanism state
  if (CONFIDENCE_ENABLE)
    conf->per_cycle_update();

  while (1) {
    ASSERT(proc_id, ftq.size() <= ftq_max_size());
    ASSERT(proc_id, cfs_taken_this_cycle <= FE_FTQ_TAKEN_CFS_PER_CYCLE);

    if (ftq.size() == ftq_max_size()) {
      DEBUG(proc_id, "Break due to full FTQ\n");
      STAT_EVENT(proc_id, FTQ_BREAK_FULL_FT_ONPATH + is_off_path_state());
      break;
    }
    if (cfs_taken_this_cycle >= FE_FTQ_TAKEN_CFS_PER_CYCLE) {
      DEBUG(proc_id, "Break due to max cfs taken per cycle\n");
      STAT_EVENT(proc_id, FTQ_BREAK_MAX_CFS_TAKEN_ONPATH + is_off_path_state());
      break;
    }
    // use `>=` because inst size does not necessarily align with FE_FTQ_BYTES_PER_CYCLE
    if (ft_pushed_this_cycle >= FE_FTQ_FT_PER_CYCLE) {
      DEBUG(proc_id, "Break due to max bytes per cycle\n");
      STAT_EVENT(proc_id, FTQ_BREAK_MAX_FT_ONPATH + is_off_path_state());
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
        current_ft_to_push = saved_recovery_ft;
        result = current_ft_to_push->predict_ft();
        if (current_ft_to_push->ended_by_exit()) {
          // Ensure that the very last simulated FT does not cause a recovery
          current_ft_to_push->clear_recovery_info();
          check_consecutivity_and_push_to_ftq();
          state = EXITING;
          return;
        }

        if (result.event == FT_EVENT_FETCH_BARRIER) {
          stall(result.op);
        } else if (result.event == FT_EVENT_MISPREDICT) {
          redirect_to_off_path(result);
        }

        break;
      }
      // recover will fall through to on-path exec
      case SERVING_ON_PATH: {
        current_ft_to_push = new FT();
        // Build new on-path FT if no recovery ft availble
        ASSERT(proc_id, !current_ft_to_push->has_unread_ops());
        auto build_success = current_ft_to_push->build([](uns8 pid) { return frontend_can_fetch_op(pid); },
                                                       [](uns8 pid, Op* op) -> bool {
                                                         frontend_fetch_op(pid, op);
                                                         return true;
                                                       },
                                                       false, []() { return get_next_on_path_op_id(); });
        ASSERT(proc_id, build_success);
        result = current_ft_to_push->predict_ft();
        // if current FT is the exit one, skip mispredict handling and directly push
        // set state and early return
        if (current_ft_to_push->ended_by_exit()) {
          // Ensure that the very last simulated FT does not cause a recovery
          current_ft_to_push->clear_recovery_info();
          check_consecutivity_and_push_to_ftq();
          state = EXITING;
          return;
        }

        if (result.event == FT_EVENT_FETCH_BARRIER) {
          stall(result.op);
        } else if (result.event == FT_EVENT_MISPREDICT) {
          redirect_to_off_path(result);
        }

        break;
      }

      case SERVING_OFF_PATH: {
        // for off-path just build and. redirect
        // cf processed while building
        current_ft_to_push = new FT();
        ASSERT(proc_id, !current_ft_to_push->has_unread_ops());
        auto build_success = current_ft_to_push->build([](uns8 pid) { return frontend_can_fetch_op(pid); },
                                                       [](uns8 pid, Op* op) -> bool {
                                                         frontend_fetch_op(pid, op);
                                                         return true;
                                                       },
                                                       true, []() { return get_next_off_path_op_id(); });
        ASSERT(proc_id, build_success);
        if (current_ft_to_push->get_end_reason() == FT_TAKEN_BRANCH) {
          frontend_redirect(proc_id, current_ft_to_push->get_last_op()->inst_uid,
                            current_ft_to_push->get_last_op()->oracle_info.pred_npc);
        } else if (current_ft_to_push->get_end_reason() == FT_BAR_FETCH) {
          stall(current_ft_to_push->get_last_op());
        }
        break;
      }
    }
    STAT_EVENT(proc_id, DFE_GEN_ON_PATH_FT + is_off_path_state());
    check_consecutivity_and_push_to_ftq();
    cfs_taken_this_cycle += (current_ft_to_push->get_end_reason() == FT_TAKEN_BRANCH) ||
                            (current_ft_to_push->get_end_reason() == FT_BAR_FETCH);
    ft_pushed_this_cycle++;
  }
}

FT* Decoupled_FE::get_ft() {
  if (!ftq.size())
    return nullptr;
  return ftq.front();
}

void Decoupled_FE::pop_ft(FT* ft) {
  ASSERT(proc_id, ft == ftq.front());
  uint64_t ft_num_ops = ftq.front()->ops.size();
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
  ASSERT(proc_id, iter->op_pos < ftq.at(iter->ft_pos)->ops.size());
  *end_of_ft = iter->op_pos == ftq.at(iter->ft_pos)->ops.size() - 1;
  return ftq.at(iter->ft_pos)->ops[iter->op_pos];
}

Op* Decoupled_FE::ftq_iter_get_next(decoupled_fe_iter* iter, bool* end_of_ft) {
  if (iter->ft_pos + 1 == ftq.size() && iter->op_pos + 1 == ftq.at(iter->ft_pos)->ops.size()) {
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
  } else if (iter->op_pos + 1 == ftq.at(iter->ft_pos)->ops.size()) {
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
    num_ops += (*it)->ops.size();
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

void Decoupled_FE::check_consecutivity_and_push_to_ftq() {
  if (ftq.size())
    ASSERT(proc_id, current_ft_to_push->is_consecutive(*ftq.back()));
  if (CONFIDENCE_ENABLE)
    conf->update(*current_ft_to_push);
  if (recovery_addr) {
    ASSERT(proc_id, recovery_addr == current_ft_to_push->get_start_addr());
    recovery_addr = 0;
  }
  ftq.emplace_back(std::move(current_ft_to_push));
}

void Decoupled_FE::redirect_to_off_path(FT_PredictResult result) {
  // misprediction and redirection handling
  ASSERT(proc_id, result.event == FT_EVENT_MISPREDICT);
  // Misprediction: Switch to off-path execution
  auto [off_path_FT, trailing_ft] = current_ft_to_push->extract_off_path_ft(result.index);
  current_ft_to_push = off_path_FT;
  // if we have a tailing ft, save it for recovery
  if (trailing_ft && trailing_ft->has_unread_ops()) {
    saved_recovery_ft = trailing_ft;
  }
  // no trailing ft, misprediction happened at the last op of the on-path FT, fetch the next on-path ft, then redirect
  else {
    saved_recovery_ft = new FT();
    auto build_success = saved_recovery_ft->build([](uns8 pid) { return frontend_can_fetch_op(pid); },
                                                  [](uns8 pid, Op* op) -> bool {
                                                    frontend_fetch_op(pid, op);
                                                    return true;
                                                  },
                                                  false, []() { return get_next_on_path_op_id(); });
    ASSERT(proc_id, build_success);
  }
  redirect_cycle = cycle_count;
  state = SERVING_OFF_PATH;
  frontend_redirect(proc_id, result.op->inst_uid, result.pred_addr);
  // set the current op number as the beginning op count of this off-path divergence
  set_off_path_op_id(current_ft_to_push->get_last_op()->op_num + 1);
  // patching/modify the current FT with off-path op if current FT not ended
  if (current_ft_to_push->get_end_reason() == FT_NOT_ENDED) {
    auto build_success = current_ft_to_push->build([](uns8 pid) { return frontend_can_fetch_op(pid); },
                                                   [](uns8 pid, Op* op) -> bool {
                                                     frontend_fetch_op(pid, op);
                                                     return true;
                                                   },
                                                   true, []() { return get_next_off_path_op_id(); });
    ASSERT(proc_id, build_success);
    if (current_ft_to_push->get_end_reason() == FT_TAKEN_BRANCH) {
      frontend_redirect(proc_id, current_ft_to_push->get_last_op()->inst_uid,
                        current_ft_to_push->get_last_op()->oracle_info.pred_npc);
    } else if (current_ft_to_push->get_end_reason() == FT_BAR_FETCH) {
      stall(current_ft_to_push->get_last_op());
    }
  }
  ASSERT(proc_id, current_ft_to_push->get_end_reason() != FT_NOT_ENDED);
}
