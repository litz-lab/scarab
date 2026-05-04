#include "decoupled_frontend.h"

#include <cmath>
#include <deque>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>

#include "core.param.h"
#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"

extern "C" {
#include "bp/bp_targ_mech.h"
#include "bp/cbp_to_scarab.h"
}
#include "frontend/frontend_intf.h"
#include "isa/isa_macros.h"

#include "ft.h"
#include "lookahead_buffer.h"
#include "op.h"
#include "op_pool.h"
#include "thread.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

/* Global Variables */
Decoupled_FE* g_dfe = nullptr;
static int fwd_progress = 0;

// Per core decoupled frontend
std::vector<std::vector<std::unique_ptr<Decoupled_FE>>> per_core_dfe;

// Compute the alternate-direction redirect target for an alt DFE: the address
// the alt frontend should fetch from in order to explore the OPPOSITE of main
// BP's predicted direction at trigger_op (a CF op).
//   main pred TAKEN     -> fallthrough (pc + inst_size)
//   main pred NOT_TAKEN -> BTB target if known
// Returns 0 when no alt target is available (main pred NOT_TAKEN and BTB missed,
// so the TAKEN target is unknown without consulting oracle); the caller treats
// 0 as "no alternate path available" and skips alt activation for this trigger.
static Addr alt_direction_target(const Op* trigger_op) {
  const Addr pc_plus_offset =
      ADDR_PLUS_OFFSET(trigger_op->inst_info->addr, trigger_op->inst_info->trace_info.inst_size);
  if (trigger_op->bp_pred_info->pred == TAKEN)
    return pc_plus_offset;
  if (!btb_pred_miss(trigger_op->btb_pred_info))
    return trigger_op->btb_pred_info->pred_target;
  return 0;
}

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
    case MAIN_BP:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data,
                                         DFE0_TRIGGER_POLICY,  // should always be 0
                                         DFE0_STOP_POLICY);
      break;
    case ALT_BP_1:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE1_TRIGGER_POLICY, DFE1_STOP_POLICY);
      break;
    case ALT_BP_2:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE2_TRIGGER_POLICY, DFE2_STOP_POLICY);
      break;
    case ALT_BP_3:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE3_TRIGGER_POLICY, DFE3_STOP_POLICY);
      break;
    case ALT_BP_4:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE4_TRIGGER_POLICY, DFE4_STOP_POLICY);
      break;
  }
}

bool decoupled_fe_is_off_path() {
  ASSERT(0, g_dfe->get_bp_id() == MAIN_BP);
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

FT* decoupled_fe_pop_ft() {
  ASSERT(0, g_dfe->get_bp_id() == MAIN_BP);
  return g_dfe->pop_ft();
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
  ASSERT(0, g_dfe->get_bp_id() == MAIN_BP);
  g_dfe->retire(op, op_proc_id, inst_uid);
}

void decoupled_fe_set_ftq_num(uint64_t ftq_ft_num) {
  ASSERT(0, g_dfe->get_bp_id() == MAIN_BP);
  g_dfe->set_ftq_num(ftq_ft_num);
}

uint64_t decoupled_fe_get_ftq_num() {
  ASSERT(0, g_dfe->get_bp_id() == MAIN_BP);
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

void decoupled_fe_on_main_prediction(uns proc_id, Op* op) {
  per_core_dfe[proc_id][MAIN_BP]->drive_alt_on_prediction(op);
}

void decoupled_fe_capture_main_pre_state(uns proc_id, Op* op) {
  per_core_dfe[proc_id][MAIN_BP]->capture_main_pre_state_for_alts(op);
}

}  // extern "C"

/* Decoupled_FE member functions */
Decoupled_FE::~Decoupled_FE() {
  if (CONFIDENCE_ENABLE && bp_id == MAIN_BP) {
    delete conf;
  }
}

void Decoupled_FE::init(uns _proc_id, uns _bp_id, Bp_Data* _bp_data, uns _dfe_trigger_policy, uns _dfe_stop_policy) {
#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  proc_id = _proc_id;
  bp_id = _bp_id;
  bp_data = _bp_data;
  dfe_trigger_policy = _dfe_trigger_policy;
  dfe_stop_policy = _dfe_stop_policy;
  // PRIMARY_DFE_STOP is reserved for the primary DFE (MAIN_BP / PRIMARY_DFE
  // trigger). Enforce both directions so neither alt configs accidentally pick
  // it up nor MAIN_BP gets configured with an alt stop policy.
  ASSERTM(_proc_id, (dfe_trigger_policy == PRIMARY_DFE) == (dfe_stop_policy == PRIMARY_DFE_STOP),
          "trigger_policy=%u stop_policy=%u: PRIMARY_DFE_STOP is required iff trigger is PRIMARY_DFE\n",
          dfe_trigger_policy, dfe_stop_policy);
  // Alt-BP per-bp_id frontend dispatch is only implemented on memtrace / PT.
  // pin_exec_driven_redirect ignores bp_id (would corrupt main's PIN process
  // state) and trace_redirect FATALs on any redirect call. Fail-fast at init
  // so misconfigured runs surface clearly instead of silently misbehaving.
  if (bp_id != MAIN_BP) {
    ASSERTM(_proc_id, FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE,
            "alt BP (bp_id=%u, trigger_policy=%u) requires FRONTEND in {FE_PT, FE_MEMTRACE}; got FRONTEND=%u\n", _bp_id,
            dfe_trigger_policy, (uns)FRONTEND);
  }
  cur_op = nullptr;
  current_ft_to_push = nullptr;
  saved_recovery_ft = nullptr;

  if (CONFIDENCE_ENABLE) {
    if (bp_id != MAIN_BP)
      conf = per_core_dfe[proc_id][MAIN_BP]->get_conf();
    else
      conf = new Conf(_proc_id);
  }

  state = (bp_id != MAIN_BP) ? INACTIVE : SERVING_ON_PATH;
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
  bool found_recovery_ft = false;
  FT* recovery_ft = nullptr;
  Op* recovery_ft_op = nullptr;
  bool recovery_op_is_last = false;
  auto erase_from = ftq.begin();

  for (auto it = ftq.begin(); it != ftq.end() && bp_id == MAIN_BP; ++it) {
    FT* ft = *it;

    for (uint64_t op_idx = 0; op_idx < ft->ops.size(); ++op_idx) {
      Op* op = ft->ops[op_idx];
      if (IS_FLUSHING_OP(op)) {
        found_recovery_ft = true;
        recovery_ft = ft;
        recovery_ft_op = op;
        op_select_bp_pred_info(op, BP_PRED_MAIN);

        // FT layout is guaranteed to be an on-path prefix followed by an optional
        // off-path suffix, so a recovery op is the last on-path op iff it is the
        // literal last op or the FT ends with an off-path suffix.
        recovery_op_is_last = (op_idx == ft->ops.size() - 1) || ft->ops.back()->off_path;
        if (recovery_op_is_last) {
          recovery_ft->trim_unread_tail([&](Op* op) {
            if (!FLUSH_OP(op))
              return false;
            DEBUG(proc_id, "FT recovery flushing unread op_num:%llu off_path:%u\n", (unsigned long long)op->op_num,
                  op->off_path);
            ASSERT(proc_id, op->off_path);
            return true;
          });
        }

        DEBUG(proc_id,
              "[DFE%u] FTQ scan: recovery op_num:%llu found in ft_id:%llu at idx:%llu/%zu"
              " literal_last:%u recovery_op_is_last:%u\n",
              bp_id, (unsigned long long)op->op_num, (unsigned long long)ft->get_ft_info().dynamic_info.FT_id,
              (unsigned long long)op_idx, ft->ops.size(), (unsigned)(op_idx == ft->ops.size() - 1),
              (unsigned)recovery_op_is_last);
        break;
      }
    }

    if (found_recovery_ft) {
      erase_from = recovery_op_is_last ? std::next(it) : it;
      break;
    }
  }

  if (erase_from != ftq.end()) {
    for (auto it = erase_from; it != ftq.end(); ++it)
      delete (*it);
    ftq.erase(erase_from, ftq.end());
  }

  if (!found_recovery_ft)
    ASSERT(proc_id, !ftq.size());

  if (found_recovery_ft && recovery_op_is_last) {
    ASSERT(proc_id, recovery_ft);
    ASSERT(proc_id, recovery_ft_op);
    recovery_ft->generate_ft_info();
  } else if (found_recovery_ft && !recovery_op_is_last) {
    ASSERT(proc_id, saved_recovery_ft);
    ASSERT(proc_id, !saved_recovery_ft->ops.empty());
    DEBUG(proc_id,
          "[DFE%u] FTQ recover (not last): recovery_op_num:%llu ft_id:%llu ft_ops:%zu"
          " saved_recovery_ft_id:%llu saved_op_pos:%llu\n",
          bp_id, (unsigned long long)bp_recovery_info->recovery_op_num,
          (unsigned long long)recovery_ft->get_ft_info().dynamic_info.FT_id, recovery_ft->ops.size(),
          (unsigned long long)saved_recovery_ft->get_ft_info().dynamic_info.FT_id,
          (unsigned long long)saved_recovery_ft->op_pos);
    // Update start address
    saved_recovery_ft->op_pos = 0;
    saved_recovery_ft->generate_ft_info();
  }

  // Early recovery (e.g., L0 wrong / main correct) may recover into an FT already in FTQ.

  DEBUG(proc_id, "[DFE%u] Recovery signalled fetch_addr:0x%llx recovery_op_num:%llu\n", bp_id,
        bp_recovery_info->recovery_fetch_addr, (unsigned long long)bp_recovery_info->recovery_op_num);
  for (size_t iter_idx = 0; iter_idx < ftq_iterators.size(); iter_idx++) {
    auto&& it = ftq_iterators[iter_idx];
    if (ftq.empty()) {
      DEBUG(proc_id, "[DFE%u] FTQ iter reset: iter:%zu ft_pos:%llu->0 (ftq empty)\n", bp_id, iter_idx,
            (unsigned long long)it->ft_pos);
      it->ft_pos = 0;
      it->op_pos = 0;
      it->flattened_op_pos = 0;
      continue;
    }

    if (it->ft_pos >= ftq.size()) {
      DEBUG(proc_id, "[DFE%u] FTQ iter clamp: iter:%zu ft_pos:%llu->%zu (after recover resize)\n", bp_id, iter_idx,
            (unsigned long long)it->ft_pos, ftq.size());
      it->ft_pos = ftq.size();
      it->op_pos = 0;

      uint64_t flat = it->op_pos;
      for (uint64_t ft_idx = 0; ft_idx < it->ft_pos; ft_idx++) {
        flat += ftq.at(ft_idx)->ops.size();
      }
      it->flattened_op_pos = flat;
    } else if (it->op_pos >= ftq.at(it->ft_pos)->ops.size()) {
      // op_pos is out of range: the FT at ft_pos had its off-path tail trimmed
      // during recovery. Advance iterator to start of next FT.
      DEBUG(proc_id, "[DFE%u] FTQ iter op_pos clamp: iter:%zu ft_pos:%llu op_pos:%llu->0 ft+1 (tail trimmed)\n", bp_id,
            iter_idx, (unsigned long long)it->ft_pos, (unsigned long long)it->op_pos);
      it->ft_pos += 1;
      it->op_pos = 0;

      uint64_t flat = 0;
      for (uint64_t ft_idx = 0; ft_idx < it->ft_pos; ft_idx++) {
        flat += ftq.at(ft_idx)->ops.size();
      }
      it->flattened_op_pos = flat;
    }
  }

  auto op = bp_recovery_info->recovery_op;

  if (bp_id == MAIN_BP) {
    if (op->bp_pred_info->recover_at_decode)
      STAT_EVENT(proc_id, FTQ_RECOVER_DECODE);
    else if (op->bp_pred_info->recover_at_exec)
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
  // Snapshot main's last-fetched op before main's FTQ is recovered/cleaned up.
  // For CONTINUE_ON_RECOVERY, alt continues the off-path stream main was on by
  // resuming from this address (rather than restarting at the misprediction).
  Op* alt_op = per_core_dfe[proc_id][MAIN_BP]->get_last_fetch_op();
  bp_recover_op(bp_data, cf_type, info);
  dfe_recover_op();
  switch (dfe_trigger_policy) {
    case PRIMARY_DFE:
      if (stalled) {
        ASSERT(proc_id, FRONTEND == FE_PIN_EXEC_DRIVEN);
        stalled = false;
      }
      // In Pin-driven frontend, we could see exit on off-path
      if (state != INACTIVE || (state == INACTIVE && exit_on_off_path)) {
        ASSERT(proc_id, saved_recovery_ft->has_unread_ops());
        ASSERTM(proc_id, bp_recovery_info->recovery_fetch_addr == saved_recovery_ft->get_ft_info().static_info.start,
                "Scarab's recovery addr 0x%llx does not match save ft "
                "addr 0x%llx\n",
                bp_recovery_info->recovery_fetch_addr, saved_recovery_ft->get_ft_info().static_info.start);
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
    default:
      // Alt DFE recovery-event handling.
      // Stop axis: STOP_ON_RECOVERY deactivates a running alt at the recovery.
      // Other stop policies (STOP_ON_PREDICTION, STOP_ON_MISPREDICTION) leave
      // alt's state to those event handlers; recovery is a no-op for stop here.
      // (No frontend_redirect needed; INACTIVE short-circuits alt's update()
      // before any frontend_fetch_op call. See stop_alt_episode for details.)
      if (is_active() && dfe_stop_policy == STOP_ON_RECOVERY) {
        next_state = INACTIVE;
      }
      // Trigger axis: CONTINUE_ON_RECOVERY (re-)activates alt on every recovery,
      // continuing main's just-abandoned off-path from main's last-fetched op
      // address. dfe_recover_op above already cleared alt's FTQ, so this can
      // safely run regardless of whether the stop branch above also fired.
      // alt_op may be nullptr when main was stalled at a fetch barrier;
      // activate_off_path(0, 0) then stops alt fetching.
      if (dfe_trigger_policy == CONTINUE_ON_RECOVERY) {
        activate_off_path(alt_op ? alt_op->inst_uid : 0, alt_op ? alt_op->inst_info->addr : 0);
      }
      break;
  }
}

void Decoupled_FE::update() {
  uint64_t cfs_taken_this_cycle = 0;
  uint64_t ft_pushed_this_cycle = 0;
  if (bp_id == MAIN_BP) {
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

    if (bp_id == MAIN_BP)
      fwd_progress = 0;
    // FSM-based FT build logic - states:
    // INACTIVE: idle until triggered or stop when end of trace seen
    // RECOVERING: handle recovery after misprediction
    // SERVING_ON_PATH: normal execution mode
    // SERVING_OFF_PATH: fetching off-path operations
    FT_PredictResult result = {};
    switch (state) {
      case INACTIVE:
        return;
      case RECOVERING: {
        ASSERT(proc_id, bp_id == MAIN_BP);
        // After recovery, we expect to serve the saved recovery FT
        next_state = SERVING_ON_PATH;
        current_ft_to_push = saved_recovery_ft;
        saved_recovery_ft = nullptr;
        result = current_ft_to_push->predict_ft();
        if (current_ft_to_push->ended_by_exit()) {
          // Ensure that the very last simulated FT does not cause a recovery
          current_ft_to_push->clear_recovery_info();
          check_consecutivity_and_push_to_ftq();
          next_state = INACTIVE;
          saved_recovery_ft = nullptr;
          return;
        }

        if (result.event == FT_EVENT_FETCH_BARRIER && FRONTEND == FE_PIN_EXEC_DRIVEN) {
          stall(result.op);
        } else if (result.event == FT_EVENT_MISPREDICT) {
          redirect_to_off_path(result);
        }

        break;
      }
      // recover will fall through to on-path exec
      case SERVING_ON_PATH: {
        // Only main BP should read from lookahead buffer in multi-BP setups.
        // All other BPs use the same update() function, so this check is necessary here.
        // The lookahead buffer is a simulation feature, not a typical CPU component.
        // Its use is controlled by LOOKAHEAD_BUF_SIZE.
        ASSERT(proc_id, bp_id == MAIN_BP);
        // Lookahead buffer always enabled
        if (LOOKAHEAD_BUF_SIZE) {
          current_ft_to_push = lookahead_buffer_pop_ft(proc_id);
          ASSERT(proc_id, current_ft_to_push->get_is_prebuilt());
        } else {
          current_ft_to_push = new FT(proc_id, bp_id);
          auto build_event =
              current_ft_to_push->build([](uns8 pid, uns8 bid) { return frontend_can_fetch_op(pid, bid); },
                                        [](uns8 pid, uns8 bid, Op* op) -> bool {
                                          frontend_fetch_op(pid, bid, op);
                                          return true;
                                        },
                                        false, conf_off_path, []() { return decoupled_fe_get_next_on_path_op_num(); });
          ASSERT(proc_id, build_event != FT_EVENT_BUILD_FAIL);
          current_ft_to_push->set_prebuilt(true);
        }

        result = current_ft_to_push->predict_ft();
        // if current FT is the exit one, skip mispredict handling and directly push
        // set state and early return
        if (current_ft_to_push->ended_by_exit()) {
          // Ensure that the very last simulated FT does not cause a recovery
          current_ft_to_push->clear_recovery_info();
          check_consecutivity_and_push_to_ftq();
          next_state = INACTIVE;
          return;
        }
        if (result.event == FT_EVENT_FETCH_BARRIER && FRONTEND == FE_PIN_EXEC_DRIVEN) {
          stall(result.op);
        } else if (result.event == FT_EVENT_MISPREDICT) {
          redirect_to_off_path(result);
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
                              current_ft_to_push->get_last_op()->bp_pred_info->pred_npc);
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

FT* Decoupled_FE::pop_ft() {
  if (!ftq.size())
    return nullptr;

  FT* ft = ftq.front();
  uint64_t ft_num_ops = ft->ops.size();
  ftq.pop_front();
  for (auto&& it : ftq_iterators) {
    // When the icache consumes an FT decrement the iter's offset so it points to the same entry as before
    if (it->ft_pos > 0) {
      ASSERT(proc_id, it->flattened_op_pos >= ft_num_ops);
      it->flattened_op_pos -= ft_num_ops;
      it->ft_pos--;
    } else {
      ASSERT(proc_id, it->flattened_op_pos < ft_num_ops);
      DEBUG(proc_id, "[DFE%u] FTQ iter reset: iter ft_pos:%llu op_pos:%llu->0 (popped front FT)\n", bp_id,
            (unsigned long long)it->ft_pos, (unsigned long long)it->op_pos);
      it->flattened_op_pos = 0;
      it->op_pos = 0;
    }
  }
  DEBUG(proc_id,
        "[DFE%u] Pop FT from FTQ: ft_id:%llu ft_ops:%llu off_path:%u end_reason:%d ftq_size_before:%zu after:%zu\n",
        bp_id, (unsigned long long)ft->get_ft_info().dynamic_info.FT_id, (unsigned long long)ft_num_ops,
        ft->get_ft_info().dynamic_info.first_op_off_path, (int)ft->get_end_reason(), ftq.size() + 1, ftq.size());
  return ft;
}

uns Decoupled_FE::new_ftq_iter() {
  ftq_iterators.push_back(std::make_unique<decoupled_fe_iter>());
  ftq_iterators.back().get()->ft_pos = 0;
  ftq_iterators.back().get()->op_pos = 0;
  ftq_iterators.back().get()->flattened_op_pos = 0;
  return ftq_iterators.size() - 1;
}

Op* Decoupled_FE::ftq_iter_get(uns iter_idx, bool* end_of_ft) {
  decoupled_fe_iter* iter = ftq_iterators[iter_idx].get();
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

void Decoupled_FE::bp_sync_from_main() {
  ASSERT(proc_id, bp_id != MAIN_BP);
  bp_sync(per_core_dfe[proc_id][MAIN_BP]->get_bp_data(), bp_data);
}

void Decoupled_FE::activate_off_path_only(uns64 inst_uid, Addr fetch_addr) {
  ASSERT(proc_id, bp_id != MAIN_BP);
  frontend_redirect(proc_id, bp_id, inst_uid, fetch_addr);
  // Update both state and next_state so is_active() reflects the activation
  // immediately. The alt-event dispatchers may stop and re-trigger alt within
  // a single event, and they re-check is_active() between the two steps.
  state = SERVING_OFF_PATH;
  next_state = SERVING_OFF_PATH;
  set_conf_off_path();
}

void Decoupled_FE::activate_off_path(uns64 inst_uid, Addr fetch_addr) {
  bp_sync_from_main();
  activate_off_path_only(inst_uid, fetch_addr);
}

void Decoupled_FE::apply_alt_spec_update(Op* trigger_op) {
  ASSERT(proc_id, bp_id != MAIN_BP);
  // Re-apply the trigger op's spec_update on alt's TAGE with alt's direction.
  // alt's bp_data was just synced to main's PRE-spec-update state (via the
  // pre-hook in bp_predict_op_impl); this call extends that state forward
  // with alt's chosen direction at the trigger op.
  const Flag alt_dir = trigger_op->bp_pred_info->pred ^ 1;
  bp_alt_spec_update_TAGE64K(proc_id, bp_id, trigger_op, alt_dir);
}

void Decoupled_FE::trigger_alt(Op* trigger_op) {
  ASSERT(proc_id, bp_id != MAIN_BP);
  ASSERT(proc_id, !is_active());
  ASSERT(proc_id, !ftq_num_fts());
  const Addr alt_pred_addr = alt_direction_target(trigger_op);
  ASSERT(proc_id, alt_pred_addr);
  // alt's bp_data is already at main's pre-spec-update state from the
  // pre-hook (capture_main_pre_state_for_alts). Re-apply the trigger op's
  // spec_update with alt's direction so alt sees "main's pre-trigger state +
  // alt's direction at the last branch", then redirect alt's frontend.
  apply_alt_spec_update(trigger_op);
  activate_off_path_only(trigger_op->inst_uid, alt_pred_addr);
}

void Decoupled_FE::capture_main_pre_state_for_alts(Op* trigger_op) {
  ASSERT(proc_id, bp_id == MAIN_BP);
  // Pre-hook fires for every CF main predicts (in FT::predict_ft, gated on
  // SIMULATION_MODE && !off_path by the caller in bp.c). Determine which alt
  // DFEs will be (re-)triggered by this prediction event and bp_sync them to
  // capture main's pre-spec-update state.
  const Flag is_misprediction = trigger_op->bp_pred_main.recover_at_fe || trigger_op->bp_pred_main.recover_at_decode ||
                                trigger_op->bp_pred_main.recover_at_exec;
  for (uns _bp_id = ALT_BP_1; _bp_id < NUM_BPS; ++_bp_id) {
    Decoupled_FE* alt = per_core_dfe[proc_id][_bp_id].get();
    const uns trigger_policy = alt->get_dfe_trigger_policy();
    const uns stop_policy = alt->get_dfe_stop_policy();
    const bool fires_per_cf = (trigger_policy == ALTERNATE_ON_PREDICTION);
    const bool fires_on_mispred = (trigger_policy == ALTERNATE_ON_MISPREDICTION) && is_misprediction;
    if (!fires_per_cf && !fires_on_mispred)
      continue;
    if (alt->is_active()) {
      // Will alt actually be stopped + re-triggered by this event?
      const bool stops_then_triggers = (fires_per_cf && stop_policy == STOP_ON_PREDICTION) ||
                                       (fires_on_mispred && stop_policy == STOP_ON_MISPREDICTION);
      if (!stops_then_triggers)
        continue;  // running alt won't be disturbed; preserve its state
    }
    alt->bp_sync_from_main();
  }
}

void Decoupled_FE::stop_alt_episode() {
  ASSERT(proc_id, bp_id != MAIN_BP);
  ASSERT(proc_id, is_active());
  // dfe_recover_op clears alt's FTQ + iterators + off_path/cur_op flags. Safe
  // on alt: the FTQ-scan branch is gated on MAIN_BP and alt never reads
  // recovery_addr.
  dfe_recover_op();
  // No frontend_redirect here. state = INACTIVE short-circuits alt's update()
  // before any frontend_fetch_op call, so the frontend backends (memtrace's
  // ext_trace_*, pin_exec_driven_*, etc.) are not queried for the dormant alt
  // stream. The next activation issues a fresh frontend_redirect with the new
  // fetch_addr, which fully reinitializes the alt's per-bp_id frontend state.
  // Update both state and next_state so is_active() reflects the stop
  // immediately for the dispatcher's downstream re-trigger check.
  state = INACTIVE;
  next_state = INACTIVE;
}

// Shared dispatcher for the per-event alt drive. Each alt has at most one
// trigger policy and at most one stop policy, and the two _ON_PREDICTION /
// _ON_MISPREDICTION variants are mutually exclusive within an alt, so each
// caller passes the policy values that match the event it represents.
//   Stop phase fires if active && stop_policy == match_stop.
//   Trigger phase fires (independently) if inactive && trigger_policy == match_trigger
//     and an alt_direction_target is computable. An event can stop a running
//     alt and re-trigger it on the same CF; the second is_active() check
//     re-reads state after stop_alt_episode.
void Decoupled_FE::drive_alt_on_event(Op* trigger_op, DFE_Trigger_Policy match_trigger, DFE_Stop_Policy match_stop) {
  ASSERT(proc_id, bp_id == MAIN_BP);
  for (uns _bp_id = ALT_BP_1; _bp_id < NUM_BPS; ++_bp_id) {
    Decoupled_FE* alt = per_core_dfe[proc_id][_bp_id].get();
    if (alt->is_active() && alt->get_dfe_stop_policy() == match_stop)
      alt->stop_alt_episode();
    if (!alt->is_active() && alt->get_dfe_trigger_policy() == match_trigger) {
      if (alt_direction_target(trigger_op))
        alt->trigger_alt(trigger_op);
    }
  }
}

void Decoupled_FE::drive_alt_on_misprediction(Op* trigger_op) {
  drive_alt_on_event(trigger_op, ALTERNATE_ON_MISPREDICTION, STOP_ON_MISPREDICTION);
}

void Decoupled_FE::drive_alt_on_prediction(Op* trigger_op) {
  drive_alt_on_event(trigger_op, ALTERNATE_ON_PREDICTION, STOP_ON_PREDICTION);
}

void Decoupled_FE::stall(Op* op) {
  stalled = true;
  DEBUG(proc_id, "Decoupled fetch stalled due to barrier fetch_addr0x:%llx off_path:%i op_num:%llu\n",
        op->inst_info->addr, op->off_path, op->op_num);
}

void Decoupled_FE::retire(Op* op, int op_proc_id, uns64 inst_uid) {
  if (op->inst_info->table_info.bar_type & BAR_FETCH) {
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
  if (!(op->bp_pred_info->recover_at_fe || op->bp_pred_info->recover_at_decode || op->bp_pred_info->recover_at_exec)) {
    return REASON_NOT_IDENTIFIED;
  }
  // mispred
  if (op->bp_pred_info->pred_orig != op->oracle_info.dir && !btb_pred_miss(op->btb_pred_info)) {
    return REASON_MISPRED;
  }
  // misfetch
  else if (!btb_pred_miss(op->btb_pred_info) && op->bp_pred_info->pred_orig == op->oracle_info.dir &&
           op->bp_pred_info->pred_npc != op->oracle_info.npc) {
    return REASON_MISFETCH;
  }
  // ibtb miss
  else if (ENABLE_IBP &&
           (op->inst_info->table_info.cf_type == CF_IBR || op->inst_info->table_info.cf_type == CF_ICALL) &&
           op->btb_pred_info->ibp_miss && op->bp_pred_info->pred_orig == TAKEN) {
    return REASON_IBTB_MISS;
  }
  // btb miss and mispred (would have been incorrect with or without btb miss)
  else if (op->bp_pred_info->pred_orig != op->oracle_info.dir && btb_pred_miss(op->btb_pred_info)) {
    return REASON_BTB_MISS_MISPRED;
  }
  // true btb miss
  else if (btb_pred_miss(op->btb_pred_info)) {
    return REASON_BTB_MISS;
  }
  // A BTB level hit, but it was not available early enough for the active BP level.
  else if (op->btb_pred_info->btb_pred_latency != MAX_UNS &&
           op->btb_pred_info->btb_pred_latency > op->bp_pred_info->bp_ready_cycle - op->recovery_info.predict_cycle) {
    return REASON_LATE_BTB_HIT;
  } else {
    // all cases should be covered
    ASSERTM(proc_id, FALSE,
            "Unclassified off-path reason: op_num:%llu inst_uid:%llu pc:0x%llx cf_type:%d "
            "active_is_l0:%d active_rec_fe:%u active_rec_decode:%u active_rec_exec:%u active_pred_orig:%u "
            "active_pred:%u active_pred_npc:0x%llx oracle_dir:%u oracle_npc:0x%llx oracle_target:0x%llx "
            "btb_miss:%u ibp_miss:%u no_target:%u "
            "l0_rec_fe:%u l0_rec_decode:%u l0_rec_exec:%u l0_pred_orig:%u l0_pred:%u l0_pred_npc:0x%llx "
            "main_rec_fe:%u main_rec_decode:%u main_rec_exec:%u main_pred_orig:%u main_pred:%u "
            "main_pred_npc:0x%llx\n",
            (unsigned long long)op->op_num, (unsigned long long)op->inst_uid, (unsigned long long)op->inst_info->addr,
            (int)op->inst_info->table_info.cf_type, op->bp_pred_info == &op->bp_pred_l0,
            op->bp_pred_info->recover_at_fe, op->bp_pred_info->recover_at_decode, op->bp_pred_info->recover_at_exec,
            op->bp_pred_info->pred_orig, op->bp_pred_info->pred, (unsigned long long)op->bp_pred_info->pred_npc,
            op->oracle_info.dir, (unsigned long long)op->oracle_info.npc, (unsigned long long)op->oracle_info.target,
            btb_pred_miss(op->btb_pred_info), op->btb_pred_info->ibp_miss, op->btb_pred_info->no_target,
            op->bp_pred_l0.recover_at_fe, op->bp_pred_l0.recover_at_decode, op->bp_pred_l0.recover_at_exec,
            op->bp_pred_l0.pred_orig, op->bp_pred_l0.pred, (unsigned long long)op->bp_pred_l0.pred_npc,
            op->bp_pred_main.recover_at_fe, op->bp_pred_main.recover_at_decode, op->bp_pred_main.recover_at_exec,
            op->bp_pred_main.pred_orig, op->bp_pred_main.pred, (unsigned long long)op->bp_pred_main.pred_npc);
  }
}

void Decoupled_FE::check_consecutivity_and_push_to_ftq() {
  if (ftq.size())
    ASSERT(proc_id, current_ft_to_push->is_consecutive(*ftq.back()));
  if (CONFIDENCE_ENABLE && bp_id == MAIN_BP)
    conf->update(*current_ft_to_push);
  if (bp_id == MAIN_BP && recovery_addr) {
    ASSERT(proc_id, recovery_addr == current_ft_to_push->get_start_addr());
    recovery_addr = 0;
  }
  DEBUG(proc_id,
        "[DFE%u] Push FT to FTQ: ft_id:%llu ft_ops:%zu off_path:%u end_reason:%d ftq_size_before:%zu after:%zu\n",
        bp_id, (unsigned long long)current_ft_to_push->get_ft_info().dynamic_info.FT_id, current_ft_to_push->ops.size(),
        current_ft_to_push->get_ft_info().dynamic_info.first_op_off_path, (int)current_ft_to_push->get_end_reason(),
        ftq.size(), ftq.size() + 1);
  ftq.emplace_back(std::move(current_ft_to_push));
}

void Decoupled_FE::redirect_to_off_path(FT_PredictResult result) {
  // misprediction and redirection handling
  ASSERT(proc_id, bp_id == MAIN_BP);
  ASSERT(proc_id, result.event == FT_EVENT_MISPREDICT);
  // Misprediction: Switch to off-path execution
  const Off_Path_Reason reason = eval_off_path_reason(result.op);
  ASSERT(proc_id, reason != REASON_NOT_IDENTIFIED);
  if (CONFIDENCE_ENABLE)
    conf->set_off_path_reason(reason);
  auto [off_path_FT, trailing_ft] = current_ft_to_push->extract_off_path_ft(result.index);
  current_ft_to_push = off_path_FT;
  // if we have a tailing ft, save it for recovery
  if (trailing_ft && trailing_ft->has_unread_ops()) {
    saved_recovery_ft = trailing_ft;
    DEBUG(proc_id, "[DFE%u] saved_recovery_ft<-trailing_ft id:%llu start:0x%llx ops:%zu\n", bp_id,
          (unsigned long long)saved_recovery_ft->get_ft_info().dynamic_info.FT_id,
          (unsigned long long)saved_recovery_ft->get_ft_info().static_info.start, saved_recovery_ft->ops.size());
  }
  // no trailing ft, misprediction happened at the last op of the on-path FT, fetch the next on-path ft, then redirect
  else {
    if (LOOKAHEAD_BUF_SIZE) {
      saved_recovery_ft = lookahead_buffer_pop_ft(proc_id);
      ASSERT(proc_id, saved_recovery_ft->get_is_prebuilt());
    } else {
      saved_recovery_ft = new FT(proc_id, bp_id);
      auto build_event =
          saved_recovery_ft->build([](uns8 pid, uns8 bid) { return frontend_can_fetch_op(pid, bid); },
                                   [](uns8 pid, uns8 bid, Op* op) -> bool {
                                     frontend_fetch_op(pid, bid, op);
                                     return true;
                                   },
                                   false, conf_off_path, []() { return decoupled_fe_get_next_on_path_op_num(); });
      ASSERT(proc_id, build_event != FT_EVENT_BUILD_FAIL);
      saved_recovery_ft->set_prebuilt(true);
    }

    DEBUG(proc_id, "[DFE%u] saved_recovery_ft<-newly_built id:%llu start:0x%llx ops:%zu\n", bp_id,
          (unsigned long long)saved_recovery_ft->get_ft_info().dynamic_info.FT_id,
          (unsigned long long)saved_recovery_ft->get_ft_info().static_info.start, saved_recovery_ft->ops.size());
  }
  redirect_cycle = cycle_count;
  next_state = SERVING_OFF_PATH;
  frontend_redirect(proc_id, bp_id, result.op->inst_uid, result.pred_addr);
  if (bp_id == MAIN_BP) {
    // Misprediction event: drive alt DFEs that subscribe to it. The
    // _ON_MISPREDICTION variants are oracle-aware (gating on simulator-known
    // misprediction at predict-stage) and not realistic in real hardware;
    // they're useful for upper-bound studies. Realistic _ON_PREDICTION
    // semantics are driven from main's update() per CF after predict_ft.
    drive_alt_on_misprediction(result.op);
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
                        current_ft_to_push->get_last_op()->bp_pred_info->pred_npc);
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
