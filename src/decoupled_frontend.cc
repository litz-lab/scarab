#include "decoupled_frontend.h"

#include <cmath>
#include <deque>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>

#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"

#include "bp/bp.h"
#include "frontend/frontend_intf.h"
#include "isa/isa_macros.h"

#include "ft.h"
#include "op.h"
#include "op_pool.h"
#include "thread.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

/* Global Variables */
Decoupled_FE* g_dfe = nullptr;
static int fwd_progress = 0;

// Per core decoupled frontend
std::vector<std::vector<std::unique_ptr<Decoupled_FE>>> per_core_dfe;

extern "C" {

/* Wrapper functions */
void alloc_mem_decoupled_fe(uns numCores, uns numBPs) {
  per_core_dfe.reserve(numCores);
  for (uns i = 0; i < numCores; ++i) {
    std::vector<std::unique_ptr<Decoupled_FE>> dfe_vec;
    dfe_vec.reserve(numBPs);
    for (uns j = 0; j < numBPs; ++j)
      dfe_vec.emplace_back(std::make_unique<Decoupled_FE>());
    per_core_dfe.emplace_back(std::move(dfe_vec));
  }
}

void init_decoupled_fe(uns proc_id, uns bp_id, Bp_Data* bp_data) {
  ASSERT(0, NUM_BPS <= 5);  // Currently support five BPs at maximum
  switch (bp_id) {
    case 0:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE0_RECOVERY_POLICY);  // should always be 0
      break;
    case 1:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE1_RECOVERY_POLICY);
      break;
    case 2:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE2_RECOVERY_POLICY);
      break;
    case 3:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE3_RECOVERY_POLICY);
      break;
    case 4:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE4_RECOVERY_POLICY);
      break;
  }
}

bool decoupled_fe_is_off_path() {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  return g_dfe->is_off_path();
}

void set_decoupled_fe(uns proc_id, uns bp_id) {
  g_dfe = per_core_dfe[proc_id][bp_id].get();
  ASSERT(proc_id, g_dfe);
}

void reset_decoupled_fe() {
}

void recover_decoupled_fe(uns proc_id, uns bp_id, Cf_Type cf_type, Recovery_Info* info) {
  g_dfe = per_core_dfe[proc_id][bp_id].get();
  ASSERT(proc_id, g_dfe);
  ASSERT(proc_id, g_dfe->get_proc_id() == proc_id);
  ASSERT(proc_id, g_dfe->get_bp_id() == bp_id);
  per_core_dfe[proc_id][bp_id]->recover(cf_type, info);
}

void debug_decoupled_fe() {
}

void update_decoupled_fe(uns proc_id, uns bp_id) {
  ASSERT(proc_id, g_dfe->get_proc_id() == proc_id);
  ASSERT(proc_id, g_dfe->get_bp_id() == bp_id);
  per_core_dfe[proc_id][bp_id]->update();
}

void decoupled_fe_pop_ft(FT* ft) {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  g_dfe->pop_ft(ft);
}

FT* decoupled_fe_get_ft() {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  return g_dfe->get_ft();
}

Decoupled_FE* decoupled_fe_new_ftq_iter(uns proc_id, uns bp_id, uns* ftq_idx) {
  *ftq_idx = per_core_dfe[proc_id][bp_id]->new_ftq_iter();
  return per_core_dfe[proc_id][bp_id].get();
}

/* Returns the Op at current FTQ iterator position. Returns NULL if the FTQ is empty */
Op* decoupled_fe_ftq_iter_get(Decoupled_FE* dfe, uns iter_idx, bool* end_of_ft) {
  return dfe->ftq_iter_get(iter_idx, end_of_ft);
}

/* Increments the iterator and returns the Op at FTQ iterator position. Returns NULL if the FTQ is empty */
Op* decoupled_fe_ftq_iter_get_next(Decoupled_FE* dfe, uns iter_idx, bool* end_of_ft) {
  return dfe->ftq_iter_get_next(iter_idx, end_of_ft);
}

/* Returns iter flattened offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_offset(Decoupled_FE* dfe, uns iter_idx) {
  return dfe->ftq_iter_offset(iter_idx);
}

/* Returns iter ft offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_ft_offset(Decoupled_FE* dfe, uns iter_idx) {
  return dfe->ftq_iter_ft_offset(iter_idx);
}

uint64_t decoupled_fe_ftq_num_ops(Decoupled_FE* dfe) {
  return dfe->ftq_num_ops();
}

uint64_t decoupled_fe_ftq_num_fts(Decoupled_FE* dfe) {
  return dfe->ftq_num_fts();
}

void decoupled_fe_retire(Op* op, int op_proc_id, uns64 inst_uid) {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  g_dfe->retire(op, op_proc_id, inst_uid);
}

void decoupled_fe_set_ftq_num(uint64_t ftq_ft_num) {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  g_dfe->set_ftq_num(ftq_ft_num);
}

uint64_t decoupled_fe_get_ftq_num() {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  return g_dfe->get_ftq_num();
}

uint64_t decoupled_fe_get_next_on_path_op_num() {
  return g_dfe->get_next_on_path_op_num();
}

uint64_t decoupled_fe_get_next_off_path_op_num() {
  return g_dfe->get_next_off_path_op_num();
}

Op* decoupled_fe_get_cur_op() {
  return g_dfe->get_cur_op();
}

uns decoupled_fe_get_conf_off_path() {
  return g_dfe->get_conf_off_path();
}

Off_Path_Reason decoupled_fe_get_off_path_reason() {
  return g_dfe->get_off_path_reason();
}

Conf_Off_Path_Reason decoupled_fe_get_conf_off_path_reason() {
  return g_dfe->get_conf_off_path_reason();
}

void decoupled_fe_conf_resovle_cf(Op* op) {
  g_dfe->conf_resolve_cf(op);
}

void decoupled_fe_print_conf_data() {
  g_dfe->print_conf_data();
}

}  // extern "C"

/* Decoupled_FE member functions */
void Decoupled_FE::init(uns _proc_id, uns _bp_id, Bp_Data* _bp_data, uns _dfe_recovery_policy) {
#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  proc_id = _proc_id;
  bp_id = _bp_id;
  bp_data = _bp_data;
  dfe_recovery_policy = _dfe_recovery_policy;
  cur_op = nullptr;
  current_ft_to_push = nullptr;
  saved_recovery_ft = nullptr;
  saved_recovery_from_trailing = false;
  late_bp_ft_id = 0;
  late_bp_sched_op = nullptr;
  late_bp_iter_idx = USE_LATE_BP ? static_cast<int>(new_ftq_iter()) : -1;

  if (CONFIDENCE_ENABLE) {
    if (bp_id)
      conf = per_core_dfe[proc_id][0]->get_conf();
    else
      conf = new Conf(_proc_id);
  }

  state = bp_id ? INACTIVE : SERVING_ON_PATH;
  next_state = state;
}

Op* Decoupled_FE::get_last_fetch_op() {
  ASSERT(proc_id, current_ft_to_push);
  // Get the address to continue before flushing the primary FTQ
  if (current_ft_to_push->ops.size())
    return current_ft_to_push->ops.back();
  else if (!ftq.empty())
    return ftq.back()->ops.back();
  return nullptr;
}

void Decoupled_FE::dfe_recover_op() {
  off_path = false;
  sched_off_path = false;
  cur_op = nullptr;
  recovery_addr = bp_recovery_info->recovery_fetch_addr;

  if (bp_recovery_info->late_bp_recovery) {
    auto full_flush_ftq = [&]() {
      for (auto it = ftq.begin(); it != ftq.end(); it++) {
        delete (*it);
      }
      ftq.clear();
    };
    ASSERT(proc_id, late_bp_iter_idx >= 0);
    decoupled_fe_iter* iter = ftq_iterators[late_bp_iter_idx].get();
    ASSERT(proc_id, iter->ft_pos < ftq.size());
    if (iter->pinned) {
      ASSERT(proc_id, ftq.at(iter->ft_pos)->get_ft_info().dynamic_info.FT_id == late_bp_ft_id);
      ASSERT(proc_id, saved_recovery_ft);
      if (saved_recovery_from_trailing) {
        DEBUG(proc_id,
              "[DFE%u] Late-BP recovery (case 3/4): replace FT_c with trailing_ft (op_pos=0) at ft_pos=%llu\n", bp_id,
              (unsigned long long)iter->ft_pos);
        // Replace the original FT entirely so the recovery FT starts from the same entry.
        saved_recovery_ft->op_pos = 0;
        saved_recovery_ft->generate_ft_info();
        // We are intentionally re-serving from FT_c start for consecutivity; skip recovery_addr check.
        recovery_addr = 0;
        for (size_t ii = iter->ft_pos; ii < ftq.size(); ++ii) {
          delete ftq.at(ii);
        }
        ftq.erase(ftq.begin() + iter->ft_pos, ftq.end());
        ASSERT(proc_id, ftq.size() == iter->ft_pos);
      } else {
        FT* late_ft = ftq.at(iter->ft_pos);
        if (late_ft->op_pos > 0) {
          ASSERT(proc_id, !iter->ft_pos);
          // icache_stage recovery will flush the off-path ops
          DEBUG(proc_id,
                "[DFE%u] Late-BP recovery (case 1/2): keep FT_c in-flight (op_pos=%llu), flush following FTs\n", bp_id,
                (unsigned long long)late_ft->op_pos);
          // Update ft_info to reflect current op_pos and late-pred end_reason without trimming.
          late_ft->force_use_late_pred_for_ft_on_last_op();
        } else {
          ASSERT(proc_id, iter->ft_pos);
          DEBUG(
              proc_id,
              "[DFE%u] Late-BP recovery (case 1/2): keep FT_c, trim off-path suffix, flush following FTs at ft_pos=%llu\n",
              bp_id, (unsigned long long)iter->ft_pos);
          // Keep the last on-path FT, drop any off-path padding.
          late_ft->trim_offpath_suffix();
          late_ft->force_use_late_pred_for_ft_on_last_op();
        }
        // Flush following FTs in both paths.
        for (size_t ii = iter->ft_pos + 1; ii < ftq.size(); ++ii) {
          delete ftq.at(ii);
        }
        ftq.erase(ftq.begin() + iter->ft_pos + 1, ftq.end());
        ASSERT(proc_id, ftq.size() == iter->ft_pos + 1);
      }
      unset_pinned_iter_ft_pos(late_bp_iter_idx);
    } else {
      DEBUG(proc_id, "[DFE%u] Late-BP recovery (full flush): late-bp FT already popped\n", bp_id);
      if (ftq.size())
        ASSERT(proc_id, ftq.at(0)->get_ft_info().dynamic_info.FT_id > late_bp_ft_id);
      // The late bp recovery op has already been popped from the FTQ
      full_flush_ftq();
    }
    late_bp_ft_id = 0;
    late_bp_sched_op = nullptr;
  } else {
    for (auto it = ftq.begin(); it != ftq.end(); it++) {
      delete (*it);
    }
    ftq.clear();
  }

  DEBUG(proc_id, "[DFE%u] after recovery: ftq.size=%zu state=%d next_state=%d\n", bp_id, ftq.size(), state,
        next_state);

  DEBUG(proc_id, "[DFE%u] Recovery signalled fetch_addr:0x%llx late_bp recovery:%i\n", bp_id,
        bp_recovery_info->recovery_fetch_addr, bp_recovery_info->late_bp_recovery);

  for (auto&& it : ftq_iterators) {
    // When the FTQ flushes, reset all iterators
    it->ft_pos = 0;
    it->op_pos = 0;
    it->flattened_op_pos = 0;
  }

  auto op = bp_recovery_info->recovery_op;

  if (!bp_id) {
    if (bp_recovery_info->late_bp_recovery)
      STAT_EVENT(proc_id, FTQ_RECOVER_LATE_BP);
    else if (op->oracle_info.recover_at_decode)
      STAT_EVENT(proc_id, FTQ_RECOVER_DECODE);
    else if (op->oracle_info.recover_at_exec)
      STAT_EVENT(proc_id, FTQ_RECOVER_EXEC);

    uint64_t offpath_cycles = cycle_count - redirect_cycle;
    ASSERT(proc_id, cycle_count > redirect_cycle);
    INC_STAT_EVENT(proc_id, FTQ_OFFPATH_CYCLES, offpath_cycles);

    // FIXME always fetch off path ops? should we get rid of this parameter?
    frontend_recover(proc_id, bp_id, bp_recovery_info->recovery_inst_uid);
    if (CONFIDENCE_ENABLE)
      conf->recover(op);
  }
  redirect_cycle = 0;
}

void Decoupled_FE::recover(Cf_Type cf_type, Recovery_Info* info) {
  // Get the last addr from the primary FTQ
  Op* alt_op = per_core_dfe[proc_id][0]->get_last_fetch_op();
  info->bp_id = bp_id;
  bp_recover_op(bp_data, cf_type, info);
  dfe_recover_op();
  switch (dfe_recovery_policy) {
    case PRIMARY_DFE:
      if (stalled) {
        ASSERT(proc_id, FRONTEND == FE_PIN_EXEC_DRIVEN);
        stalled = false;
      }
      // In Pin-driven frontend, we could see exit on off-path
      if (state != INACTIVE || (state == INACTIVE && exit_on_off_path)) {
        if (!bp_recovery_info->late_bp_recovery) {
          ASSERT(proc_id, saved_recovery_ft->has_unread_ops());
          ASSERTM(proc_id, bp_recovery_info->recovery_fetch_addr == saved_recovery_ft->get_ft_info().static_info.start,
                  "Scarab's recovery addr 0x%llx does not match save ft "
                  "addr 0x%llx\n",
                  bp_recovery_info->recovery_fetch_addr, saved_recovery_ft->get_ft_info().static_info.start);
        }
        exit_on_off_path = false;
        // In Pin-driven frontend, we need to adjust the on-path op num and remove ops from saved recovery FT
        if (FRONTEND == FE_PIN_EXEC_DRIVEN) {
          set_on_path_op_num(bp_recovery_info->recovery_op->op_num + 1);
          saved_recovery_ft->remove_op_after_exec_recover();
          auto build_event =
              saved_recovery_ft->build([](uns8 pid, uns8 bid) { return frontend_can_fetch_op(pid, bid); },
                                       [](uns8 pid, uns8 bid, Op* op) -> bool {
                                         frontend_fetch_op(pid, bid, op);
                                         return true;
                                       },
                                       false, conf_off_path, []() { return decoupled_fe_get_next_on_path_op_num(); });
          ASSERT(proc_id, build_event != FT_EVENT_BUILD_FAIL);
        }
        next_state = RECOVERING;
      }

      break;
    case CONTINUE_ON_RECOVERY:
      if (alt_op)
        frontend_redirect(proc_id, bp_id, alt_op->inst_uid, alt_op->inst_info->addr);
      else  // If it was stalled due to a fetch barrier, can be nullptr
        frontend_redirect(proc_id, bp_id, 0, 0);
      bp_sync(per_core_dfe[proc_id][0]->get_bp_data(), per_core_dfe[proc_id][bp_id]->get_bp_data());
      next_state = SERVING_OFF_PATH;
      set_conf_off_path();
      break;
    case CONTINUE_ON_PREDICTION:
      frontend_redirect(proc_id, bp_id, 0, 0);  // Passing fetch_addr = 0 will stop fetching the secondary
      next_state = INACTIVE;
      break;
  }
}

void Decoupled_FE::update() {
  uint64_t cfs_taken_this_cycle = 0;
  uint64_t ft_pushed_this_cycle = 0;
  if (!bp_id) {
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
  }

  while (1) {
    state = next_state;
    DEBUG(proc_id,
          "[DFE%u] state=%d next_state=%d ftq.size=%zu off_path=%d stalled=%d exit_on_off_path=%d late_bp_pending=%d\n",
          bp_id, state, next_state, ftq.size(), is_off_path_state(), stalled, exit_on_off_path,
          bp_recovery_info ? bp_recovery_info->late_bp_pending : -1);
    ASSERT(proc_id, ftq.size() <= ftq_max_size());
    ASSERT(proc_id, cfs_taken_this_cycle <= FE_FTQ_TAKEN_CFS_PER_CYCLE);

    if (ftq.size() == ftq_max_size()) {
      DEBUG(proc_id, "[DFE%u] Break due to full FTQ\n", bp_id);
      STAT_EVENT(proc_id, FTQ_BREAK_FULL_FT_ONPATH + is_off_path_state());
      break;
    }
    if (cfs_taken_this_cycle >= FE_FTQ_TAKEN_CFS_PER_CYCLE) {
      DEBUG(proc_id, "[DFE%u] Break due to max cfs taken per cycle\n", bp_id);
      STAT_EVENT(proc_id, FTQ_BREAK_MAX_CFS_TAKEN_ONPATH + is_off_path_state());
      break;
    }
    // use `>=` because inst size does not necessarily align with FE_FTQ_BYTES_PER_CYCLE
    if (ft_pushed_this_cycle >= FE_FTQ_FT_PER_CYCLE) {
      DEBUG(proc_id, "[DFE%u] Break due to max bytes per cycle\n", bp_id);
      STAT_EVENT(proc_id, FTQ_BREAK_MAX_FT_ONPATH + is_off_path_state());
      break;
    }
    if (BP_MECH != MTAGE_BP && !bp_is_predictable(g_bp_data)) {
      DEBUG(proc_id, "[DFE%u] Break due to limited branch predictor\n", bp_id);
      STAT_EVENT(proc_id, FTQ_BREAK_PRED_BR_ONPATH + is_off_path_state());
      break;
    }
    if (stalled) {
      ASSERT(proc_id, FRONTEND == FE_PIN_EXEC_DRIVEN);
      DEBUG(proc_id, "Break due to wait for fetch barrier resolved\n");
      STAT_EVENT(proc_id, FTQ_BREAK_BAR_FETCH_ONPATH + is_off_path_state());
      break;
    }

    if (!bp_id)
      fwd_progress = 0;
    // FSM-based FT build logic - states:
    // INACTIVE: idle until triggered or stop when end of trace seen
    // RECOVERING: handle recovery after misprediction
    // SERVING_ON_PATH: normal execution mode
    // SERVING_OFF_PATH: fetching off-path operations
    FT_PredictResult result;
    switch (state) {
      case INACTIVE:
        return;
      case RECOVERING: {
        // After recovery, we expect to serve the saved recovery FT
        next_state = SERVING_ON_PATH;
        current_ft_to_push = saved_recovery_ft;
        result = current_ft_to_push->predict_ft();
        DEBUG(proc_id, "[DFE%u] predict_ft result event=%d ops=%zu\n", bp_id, result.event,
              current_ft_to_push->get_ops().size());
        if (current_ft_to_push->ended_by_exit()) {
          // Ensure that the very last simulated FT does not cause a recovery
          current_ft_to_push->clear_recovery_info();
          check_consecutivity_and_push_to_ftq();
          next_state = INACTIVE;
          return;
        }

        DEBUG(proc_id, "[DFE%u] predict_ft event=%d idx=%llu op_num=%s pred_addr=%llx\n", bp_id, result.event,
              (unsigned long long)result.index, result.op ? unsstr64(result.op->op_num) : "none",
              (unsigned long long)result.pred_addr);
        if (result.event == FT_EVENT_FETCH_BARRIER && FRONTEND == FE_PIN_EXEC_DRIVEN) {
          stall(result.op);
        } else if (result.event == FT_EVENT_MISPREDICT) {
          redirect_to_off_path(result);
        } else if (result.event == FT_EVENT_LATE_BP_MISPREDICT) {
          redirect_to_late_bp_wrong(result);
        } else if (result.event == FT_EVENT_LATE_BP_CORRECT_OVERRIDE) {
          redirect_to_late_bp_correct(result);
        }

        break;
      }
      // recover will fall through to on-path exec
      case SERVING_ON_PATH: {
        current_ft_to_push = new FT(proc_id, bp_id);
        // Build new on-path FT if no recovery ft availble
        ASSERT(proc_id, !current_ft_to_push->has_unread_ops());
        auto build_event =
            current_ft_to_push->build([](uns8 pid, uns8 bid) { return frontend_can_fetch_op(pid, bid); },
                                      [](uns8 pid, uns8 bid, Op* op) -> bool {
                                        frontend_fetch_op(pid, bid, op);
                                        return true;
                                      },
                                      false, conf_off_path, []() { return decoupled_fe_get_next_on_path_op_num(); });
        ASSERT(proc_id, build_event != FT_EVENT_BUILD_FAIL);
        current_ft_to_push->set_prebuilt(true);
        result = current_ft_to_push->predict_ft();
        DEBUG(proc_id, "[DFE%u] predict_ft result event=%d ops=%zu\n", bp_id, result.event,
              current_ft_to_push->get_ops().size());
        // if current FT is the exit one, skip mispredict handling and directly push
        // set state and early return
        if (current_ft_to_push->ended_by_exit()) {
          // Ensure that the very last simulated FT does not cause a recovery
          current_ft_to_push->clear_recovery_info();
          check_consecutivity_and_push_to_ftq();
          next_state = INACTIVE;
          return;
        }

        DEBUG(proc_id, "[DFE%u] predict_ft event=%d idx=%llu op_num=%s pred_addr=%llx\n", bp_id, result.event,
              (unsigned long long)result.index, result.op ? unsstr64(result.op->op_num) : "none",
              (unsigned long long)result.pred_addr);
        if (result.event == FT_EVENT_FETCH_BARRIER && FRONTEND == FE_PIN_EXEC_DRIVEN) {
          stall(result.op);
        } else if (result.event == FT_EVENT_MISPREDICT) {
          redirect_to_off_path(result);
        } else if (result.event == FT_EVENT_LATE_BP_MISPREDICT) {
          redirect_to_late_bp_wrong(result);
        } else if (result.event == FT_EVENT_LATE_BP_CORRECT_OVERRIDE) {
          redirect_to_late_bp_correct(result);
        }

        break;
      }
      case SERVING_OFF_PATH: {
        // for off-path just build and. redirect
        // cf processed while building
        if (exit_on_off_path)
          return;
        current_ft_to_push = new FT(proc_id, bp_id);
        ASSERT(proc_id, !current_ft_to_push->has_unread_ops());
        while (current_ft_to_push->get_end_reason() == FT_NOT_ENDED) {
          auto build_event =
              current_ft_to_push->build([](uns8 pid, uns8 bid) { return frontend_can_fetch_op(pid, bid); },
                                        [](uns8 pid, uns8 bid, Op* op) -> bool {
                                          frontend_fetch_op(pid, bid, op);
                                          return true;
                                        },
                                        true, conf_off_path, []() { return decoupled_fe_get_next_off_path_op_num(); });
          ASSERT(proc_id, build_event != FT_EVENT_BUILD_FAIL);
          if (current_ft_to_push->ended_by_exit()) {
            // Ensure that the very last simulated FT does not cause a recovery
            current_ft_to_push->clear_recovery_info();
            check_consecutivity_and_push_to_ftq();
            next_state = INACTIVE;
            exit_on_off_path = true;
            return;
          }
          if (build_event == FT_EVENT_MISPREDICT || build_event == FT_EVENT_OFFPATH_TAKEN_REDIRECT) {
            frontend_redirect(proc_id, bp_id, current_ft_to_push->get_last_op()->inst_uid,
                              current_ft_to_push->get_last_op()->oracle_info.pred_npc);
          } else if (build_event == FT_EVENT_FETCH_BARRIER && FRONTEND == FE_PIN_EXEC_DRIVEN) {
            stall(current_ft_to_push->get_last_op());
          }
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
  for (auto&& it : ftq_iterators) {
    // When the icache consumes an FT decrement the iter's offset so it points to the same entry as before
    if (it->ft_pos > 0) {
      ASSERT(proc_id, it->flattened_op_pos >= ft_num_ops);
      it->flattened_op_pos -= ft_num_ops;
      it->ft_pos--;
    } else {
      ASSERT(proc_id, it->flattened_op_pos < ft_num_ops);
      it->flattened_op_pos = 0;
      it->op_pos = 0;
      if (it->pinned) {
        ASSERT(proc_id, ft->get_ft_info().dynamic_info.FT_id == late_bp_ft_id);
        DEBUG(proc_id, "[DFE%u] late-bp FT (ft_id=%ld) popped\n", bp_id, late_bp_ft_id);
        it->pinned = false;
      }
    }
  }
}

uns Decoupled_FE::new_ftq_iter() {
  ftq_iterators.push_back(std::make_unique<decoupled_fe_iter>());
  ftq_iterators.back().get()->ft_pos = 0;
  ftq_iterators.back().get()->op_pos = 0;
  ftq_iterators.back().get()->flattened_op_pos = 0;
  ftq_iterators.back().get()->pinned = false;
  return ftq_iterators.size() - 1;
}

void Decoupled_FE::unset_pinned_iter_ft_pos(uns iter_idx) {
  ASSERT(proc_id, iter_idx < ftq_iterators.size());
  decoupled_fe_iter* iter = ftq_iterators[iter_idx].get();
  ASSERT(proc_id, iter->pinned);
  ftq_iterators[iter_idx]->ft_pos = 0;
  ftq_iterators[iter_idx]->op_pos = 0;
  ftq_iterators[iter_idx]->flattened_op_pos = 0;
  ftq_iterators[iter_idx]->pinned = false;
}

void Decoupled_FE::set_pinned_iter_ft_pos(uns iter_idx, uint64_t ft_pos) {
  ASSERT(proc_id, iter_idx < ftq_iterators.size());
  decoupled_fe_iter* iter = ftq_iterators[iter_idx].get();
  ASSERT(proc_id, !iter->pinned);
  DEBUG(proc_id, "[DFE%u] set_pinned_iter_ft_pos: this=%p iter_idx=%u ft_pos=%llu ftq.size=%zu\n", bp_id, this,
        iter_idx, (unsigned long long)ft_pos, ftq.size());
  iter->ft_pos = ft_pos;
  iter->op_pos = 0;
  iter->flattened_op_pos = 0;
  iter->pinned = true;
  for (size_t ii = 0; ii < ft_pos && ii < ftq.size(); ++ii) {
    iter->flattened_op_pos += ftq.at(ii)->ops.size();
  }
}

Op* Decoupled_FE::ftq_iter_get(uns iter_idx, bool* end_of_ft) {
  decoupled_fe_iter* iter = ftq_iterators[iter_idx].get();
  ASSERT(proc_id, !iter->pinned);
  // if FTQ is empty or if iter has seen all FTs
  if (ftq.empty() || iter->ft_pos == ftq.size()) {
    if (ftq.empty())
      ASSERT(proc_id, iter[iter_idx].ft_pos == 0 && iter[iter_idx].op_pos == 0 && iter[iter_idx].flattened_op_pos == 0);
    return NULL;
  }

  ASSERT(proc_id, iter->ft_pos >= 0);
  ASSERT(proc_id, iter->ft_pos < ftq.size());
  ASSERT(proc_id, iter->op_pos >= 0);
  ASSERT(proc_id, iter->op_pos < ftq.at(iter->ft_pos)->ops.size());
  *end_of_ft = iter->op_pos == ftq.at(iter->ft_pos)->ops.size() - 1;
  return ftq.at(iter->ft_pos)->ops[iter->op_pos];
}

Op* Decoupled_FE::ftq_iter_get_next(uns iter_idx, bool* end_of_ft) {
  decoupled_fe_iter* iter = ftq_iterators[iter_idx].get();
  ASSERT(proc_id, !iter->pinned);
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
  return ftq_iter_get(iter_idx, end_of_ft);
}

uint64_t Decoupled_FE::ftq_iter_offset(uns iter_idx) {
  return ftq_iterators[iter_idx]->flattened_op_pos;
}

uint64_t Decoupled_FE::ftq_iter_ft_offset(uns iter_idx) {
  return ftq_iterators[iter_idx]->ft_pos;
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
    DEBUG(proc_id,
          "[DFE%u] Decoupled fetch saw barrier retire fetch_addr:0x%llx off_path:%i op_num:%llu list_count:%i\n", bp_id,
          op->inst_info->addr, op->off_path, op->op_num, td->seq_op_list.count);
    ASSERT(proc_id, td->seq_op_list.count == 1);
    stalled = false;
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
  if (CONFIDENCE_ENABLE && !bp_id)
    conf->update(*current_ft_to_push);
  if (!bp_id && recovery_addr) {
    ASSERT(proc_id, recovery_addr == current_ft_to_push->get_start_addr());
    recovery_addr = 0;
  }
  ftq.emplace_back(std::move(current_ft_to_push));
  ASSERT(proc_id, ftq.size() <= ftq_max_size());
  if (late_bp_ft_id && ftq.back()->get_ft_info().dynamic_info.FT_id == late_bp_ft_id) {
    DEBUG(proc_id, "[DFE%u] pin late-bp: this=%p ftq.size=%zu late_bp_iter_idx=%d ft_pos=%zu\n", bp_id, this,
          ftq.size(), late_bp_iter_idx, ftq.size() - 1);
    set_pinned_iter_ft_pos(late_bp_iter_idx, ftq.size() - 1);
  }
}


void Decoupled_FE::redirect_to_off_path(FT_PredictResult result) {
  // misprediction and redirection handling
  ASSERT(proc_id, result.event == FT_EVENT_MISPREDICT || result.event == FT_EVENT_LATE_BP_MISPREDICT ||
                      result.event == FT_EVENT_LATE_BP_CORRECT_OVERRIDE);
  DEBUG(proc_id, "[DFE%u] redirect_to_off_path event=%d idx=%llu op_num=%s pred_addr=%llx\n", bp_id, result.event,
        (unsigned long long)result.index, result.op ? unsstr64(result.op->op_num) : "none",
        (unsigned long long)result.pred_addr);
  // Misprediction: Switch to off-path execution
  auto [off_path_FT, trailing_ft] = current_ft_to_push->extract_off_path_ft(result.index);
  current_ft_to_push = off_path_FT;
  // if we have a tailing ft, save it for recovery
  if (trailing_ft && trailing_ft->has_unread_ops()) {
    saved_recovery_ft = trailing_ft;
    saved_recovery_from_trailing = true;
  }
  // no trailing ft, misprediction happened at the last op of the on-path FT, fetch the next on-path ft, then redirect
  else {
    saved_recovery_ft = new FT(proc_id, bp_id);
    saved_recovery_from_trailing = false;
    auto build_event =
        saved_recovery_ft->build([](uns8 pid, uns8 bid) { return frontend_can_fetch_op(pid, bid); },
                                 [](uns8 pid, uns8 bid, Op* op) -> bool {
                                   frontend_fetch_op(pid, bid, op);
                                   return true;
                                 },
                                 false, conf_off_path, []() { return decoupled_fe_get_next_on_path_op_num(); });
    ASSERT(proc_id, build_event != FT_EVENT_BUILD_FAIL);
  }
  saved_recovery_ft->set_prebuilt(true);
  redirect_cycle = cycle_count;
  next_state = SERVING_OFF_PATH;
  frontend_redirect(proc_id, bp_id, result.op->inst_uid, result.pred_addr);
  if (!bp_id) {
    for (uns _bp_id = 1; _bp_id < NUM_BPS; ++_bp_id) {
      if (per_core_dfe[proc_id][_bp_id]->get_dfe_recovery_policy() == CONTINUE_ON_PREDICTION &&
          !per_core_dfe[proc_id][_bp_id]->is_off_path()) {
        ASSERT(proc_id, !per_core_dfe[proc_id][_bp_id]->ftq_num_fts());
        Op alt_op = *result.op;
        Addr alt_pred_addr =
            bp_predict_op(per_core_dfe[proc_id][_bp_id]->bp_data, &alt_op, 0, result.op->inst_info->addr);
        frontend_redirect(proc_id, _bp_id, alt_op.inst_uid, alt_pred_addr);
        per_core_dfe[proc_id][_bp_id]->next_state = SERVING_OFF_PATH;
        per_core_dfe[proc_id][_bp_id]->set_conf_off_path();
        bp_sync(per_core_dfe[proc_id][bp_id]->get_bp_data(), per_core_dfe[proc_id][_bp_id]->get_bp_data());
      }
    }
  }

  // set the current op number as the beginning op count of this off-path divergence
  set_off_path_op_num(current_ft_to_push->get_last_op()->op_num + 1);
  // patching/modify the current FT with off-path op if current FT not ended
  while (current_ft_to_push->get_end_reason() == FT_NOT_ENDED) {
    auto build_event =
        current_ft_to_push->build([](uns8 pid, uns8 bid) { return frontend_can_fetch_op(pid, bid); },
                                  [](uns8 pid, uns8 bid, Op* op) -> bool {
                                    frontend_fetch_op(pid, bid, op);
                                    return true;
                                  },
                                  true, conf_off_path, []() { return decoupled_fe_get_next_off_path_op_num(); });
    ASSERT(proc_id, build_event != FT_EVENT_BUILD_FAIL);
    if (build_event == FT_EVENT_MISPREDICT || build_event == FT_EVENT_OFFPATH_TAKEN_REDIRECT) {
      frontend_redirect(proc_id, bp_id, current_ft_to_push->get_last_op()->inst_uid,
                        current_ft_to_push->get_last_op()->oracle_info.pred_npc);
    } else if (build_event == FT_EVENT_FETCH_BARRIER && FRONTEND == FE_PIN_EXEC_DRIVEN) {
      stall(current_ft_to_push->get_last_op());
    }
  }
  if (current_ft_to_push->ended_by_exit()) {
    next_state = INACTIVE;
    exit_on_off_path = true;
  }
  ASSERT(proc_id, current_ft_to_push->get_end_reason() != FT_NOT_ENDED);
}

void Decoupled_FE::redirect_to_late_bp_wrong(FT_PredictResult result) {
  ASSERT(proc_id, result.event == FT_EVENT_LATE_BP_MISPREDICT);
  STAT_EVENT(proc_id, LATE_BP_OVERRIDE_CALLED);
  DEBUG(proc_id, "[DFE%u] redirect_to_late_bp_wrong idx=%llu op_num=%s pred_addr=%llx\n", bp_id,
        (unsigned long long)result.index, result.op ? unsstr64(result.op->op_num) : "none",
        (unsigned long long)result.pred_addr);
  if (result.op) {
    late_bp_sched_op = result.op;
  } else {
    ASSERT(proc_id, current_ft_to_push);
    late_bp_sched_op = current_ft_to_push->get_last_op();
  }
  ASSERT(proc_id, late_bp_sched_op);

  STAT_EVENT(proc_id, LATE_BP_OVERRIDE_LATE_WRONG);
  late_bp_ft_id = 0;
  result.pred_addr = late_bp_sched_op->oracle_info.late_pred_npc;
  redirect_to_off_path(result);
  DEBUG(proc_id, "[DFE%u] Late-BP wrong: off-path to late target op_num=%llu\n", bp_id,
        (unsigned long long)late_bp_sched_op->op_num);
}

void Decoupled_FE::redirect_to_late_bp_correct(FT_PredictResult result) {
  ASSERT(proc_id, result.event == FT_EVENT_LATE_BP_CORRECT_OVERRIDE);
  STAT_EVENT(proc_id, LATE_BP_OVERRIDE_CALLED);
  STAT_EVENT(proc_id, LATE_BP_OVERRIDE_LATE_CORRECT);
  DEBUG(proc_id, "[DFE%u] redirect_to_late_bp_correct idx=%llu op_num=%s pred_addr=%llx\n", bp_id,
        (unsigned long long)result.index, result.op ? unsstr64(result.op->op_num) : "none",
        (unsigned long long)result.pred_addr);
  if (result.op) {
    late_bp_sched_op = result.op;
  } else {
    ASSERT(proc_id, current_ft_to_push);
    late_bp_sched_op = current_ft_to_push->get_last_op();
  }
  ASSERT(proc_id, late_bp_sched_op);
  // Early predictor may request exec recovery; allow it here since we override to late-correct path.
  redirect_to_off_path(result);
  // Pin the off-path FT for late recovery replacement.
  late_bp_ft_id = current_ft_to_push->get_ft_info().dynamic_info.FT_id;
  DEBUG(proc_id, "[DFE%u] Late-BP correct: off-path to early target op_num=%llu ft_id=%llu\n", bp_id,
        (unsigned long long)late_bp_sched_op->op_num, (unsigned long long)late_bp_ft_id);
  bp_sched_recovery(bp_recovery_info, late_bp_sched_op, cycle_count, /*late_bp_recovery=*/TRUE,
                    /*force_offpath=*/FALSE);
}
