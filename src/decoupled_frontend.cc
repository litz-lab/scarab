#include "decoupled_frontend.hpp"
#include "frontend/frontend_intf.h"
#include "op.h"
#include "op_pool.h"
#include "thread.h"
#include "isa/isa_macros.h"
#include "prefetcher/pref.param.h"
#include "memory/memory.param.h"
#include "frontend/pt_memtrace/memtrace_fe.h"
#include "mp.hpp"

#include <deque>
#include <vector>
#include <iostream>
#include <tuple>
#include <cmath>
#include <memory>

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)
#define DFE_BREAK_REASON_COUNT 5

/* Global Variables */
Decoupled_FE* g_dfe = nullptr;
static int fwd_progress = 0;

// Per core decoupled frontend
std::vector<std::vector<std::unique_ptr<Decoupled_FE>>> per_core_dfe;

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
  ASSERT(0, NUM_BPS <= 5); // Currently support five BPs at maximum
  switch(bp_id) {
    case 0:
      per_core_dfe[proc_id][bp_id]->init(proc_id, bp_id, bp_data, DFE0_RECOVERY_POLICY); // should always be 0
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

void reset_decoupled_fe() {}

void recover_decoupled_fe(uns proc_id, Cf_Type cf_type, Recovery_Info* info) {
  ASSERT(proc_id, g_dfe->get_proc_id() == proc_id);
  ASSERT(proc_id, g_dfe->get_bp_id() == 0);
  // recover the primary DFE at last because some information from the primary is used during the others' recovery
  for (int bp_id = NUM_BPS-1; bp_id >= 0; bp_id--)
    per_core_dfe[proc_id][bp_id]->recover(cf_type, info);
}

void debug_decoupled_fe() {

}

void update_decoupled_fe(uns proc_id) {
  ASSERT(proc_id, g_dfe->get_proc_id() == proc_id);
  ASSERT(proc_id, g_dfe->get_bp_id() == 0);
  for (uns bp_id = 0; bp_id < NUM_BPS; ++bp_id)
    per_core_dfe[proc_id][bp_id]->update();
}

bool decoupled_fe_current_ft_can_fetch_op() {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  return g_dfe->current_ft_can_fetch_op();
}

bool decoupled_fe_can_fetch_ft() {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  return g_dfe->can_fetch_ft();
}

FT_Info decoupled_fe_fetch_ft() {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  return g_dfe->fetch_ft();
}

FT_Info decoupled_fe_peek_ft() {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  return g_dfe->peek_ft();
}

Decoupled_FE* decoupled_fe_new_ftq_iter(uns proc_id, uns bp_id, uns* ftq_idx) {
  *ftq_idx = per_core_dfe[proc_id][bp_id]->new_ftq_iter();
  return per_core_dfe[proc_id][bp_id].get();
}

/* Returns the Op at current FTQ iterator position. Returns NULL if the FTQ is empty */
Op* decoupled_fe_ftq_iter_get(Decoupled_FE* dfe, uns iter_idx, bool* end_of_ft) {
  return dfe->ftq_iter_get(iter_idx, end_of_ft);
}

// fill in the icache stage data with current FT in use
// return if FT has ended
// if true, the requested number of ops might not be fulfilled
bool decoupled_fe_fill_icache_stage_data(int requested, Stage_Data *sd) {
  ASSERT(0, g_dfe->get_bp_id() == 0);
  return g_dfe->fill_icache_stage_data(requested, sd);
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
  return dfe->ftq_iter_offset(iter_idx);
}

uint64_t decoupled_fe_ftq_num_ops(Decoupled_FE* dfe) {
  return dfe->ftq_num_ops();
}

uint64_t decoupled_fe_ftq_num_fts(Decoupled_FE* dfe) {
  return dfe->ftq_num_fts();
}

void decoupled_fe_retire(Op *op, int op_proc_id, uns64 inst_uid) {
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

void decoupled_fe_set_off_path(uns proc_id, uns bp_id) {
  per_core_dfe[proc_id][bp_id]->set_off_path();
}

void decoupled_fe_search_mp_candidate(Addr line_addr) {
  if (NUM_BPS == 1)
    return;
  g_dfe->search_mp_candidate(line_addr);
}

/* FT member functions */
FT::FT() {
  proc_id = 0;
  free_ops_and_clear();
}

FT::FT(uns _proc_id) {
  proc_id = _proc_id;
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
      DEBUG(proc_id, "back addr + size %llx fetch addr %llx\n",
          ops.back()->inst_info->addr + ops.back()->inst_info->trace_info.inst_size,
          op->inst_info->addr);
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

/* Decoupled_FE member functions */
void Decoupled_FE::init(uns _proc_id, uns _bp_id, Bp_Data* _bp_data, uns _dfe_recovery_policy) {
#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  proc_id = _proc_id;
  bp_id = _bp_id;
  bp_data = _bp_data;
  dfe_recovery_policy = _dfe_recovery_policy;
  current_ft_to_push.set_ft_started_by(FT_STARTED_BY_APP);
  if (bp_id)
    mp = per_core_dfe[proc_id][0]->get_mp();
  else if (NUM_BPS > 1 &&
      (DFE1_RECOVERY_POLICY == CONTINUE_ON_MP ||
        DFE2_RECOVERY_POLICY == CONTINUE_ON_MP ||
        DFE3_RECOVERY_POLICY == CONTINUE_ON_MP ||
        DFE4_RECOVERY_POLICY == CONTINUE_ON_MP))
      mp = new MP(proc_id);
}

FT* Decoupled_FE::get_last_fetch_target() {
  // Get the address to continue before flushing the primary FTQ
  if (current_ft_to_push.ops.size())
    return &current_ft_to_push;
  else if (!ftq.empty())
    return &ftq.back();
  return nullptr;
}

void Decoupled_FE::dfe_recover_op() {
  off_path = false;
  sched_off_path = false;
  recovery_addr = bp_recovery_info->recovery_fetch_addr;

  for (auto it = ftq.begin(); it != ftq.end(); it++) {
    it->free_ops_and_clear();
  }
  ftq.clear();

  current_ft_to_push.free_ops_and_clear();
  current_ft_to_push.set_ft_started_by(FT_STARTED_BY_RECOVERY);
  current_ft_in_use.free_ops_and_clear();

  dfe_op_count = bp_recovery_info->recovery_op_num + 1;
  DEBUG(proc_id,
        "[DFE%u] Recovery signalled fetch_addr0x:%llx\n", bp_id, bp_recovery_info->recovery_fetch_addr);

  for (auto&& it : ftq_iterators) {
    // When the FTQ flushes, reset all iterators
    it->ft_pos = 0;
    it->op_pos = 0;
    it->flattened_op_pos = 0;
  }

  auto op = bp_recovery_info->recovery_op;

  if(stalled) {
    DEBUG(proc_id,
        "[DFE%u] Unstalled off-path fetch barrier due to recovery fetch_addr0x:%llx off_path:%i op_num:%llu\n",
        bp_id, op->inst_info->addr, op->off_path, op->op_num);
    stalled = false;
  }

  if (!bp_id) {
    if (op->oracle_info.recover_at_decode)
      STAT_EVENT(proc_id, FTQ_RECOVER_DECODE);
    else if (op->oracle_info.recover_at_exec)
      STAT_EVENT(proc_id, FTQ_RECOVER_EXEC);

    uint64_t offpath_cycles = cycle_count - redirect_cycle;
    ASSERT(proc_id, cycle_count > redirect_cycle);
    INC_STAT_EVENT(proc_id, FTQ_OFFPATH_CYCLES, offpath_cycles);

    //FIXME always fetch off path ops? should we get rid of this parameter?
    frontend_recover(proc_id, bp_id, bp_recovery_info->recovery_inst_uid);
    ASSERTM(proc_id, bp_recovery_info->recovery_fetch_addr == frontend_next_fetch_addr(proc_id),
        "Scarab's recovery addr 0x%llx does not match frontend's recovery "
        "addr 0x%llx\n",
        bp_recovery_info->recovery_fetch_addr, frontend_next_fetch_addr(proc_id));
  }
  redirect_cycle = 0;
}

void Decoupled_FE::recover(Cf_Type cf_type, Recovery_Info* info) {
  // Get the last addr from the primary FTQ
  FT* last_ft_primary = per_core_dfe[proc_id][0]->get_last_fetch_target();
  Op* alt_op = last_ft_primary ? last_ft_primary->ops.back() : nullptr;
  info->bp_id = bp_id;
  switch(dfe_recovery_policy) {
    case PRIMARY_DFE:
      bp_recover_op(bp_data, cf_type, info);
      dfe_recover_op();
      break;
    case CONTINUE_ON_RECOVERY:
      bp_recover_op(bp_data, cf_type, info);
      dfe_recover_op();
      if (alt_op)
        frontend_redirect(proc_id, bp_id, alt_op->inst_uid, alt_op->inst_info->addr);
      else // If it was stalled due to a fetch barrier, can be nullptr
        frontend_redirect(proc_id, bp_id, 0, 0);
      set_off_path();
      bp_sync(per_core_dfe[proc_id][0]->get_bp_data(), per_core_dfe[proc_id][bp_id]->get_bp_data());
      break;
    case CONTINUE_ON_MP:
      if (last_ft_primary)
        insert_mp_candidate(&(last_ft_primary->ft_info), bp_data->global_hist);
      bp_recover_op(bp_data, cf_type, info);
      dfe_recover_op();
      if (alt_op && !determine_to_run_alt_by_mp(alt_op->inst_info->addr))
        alt_op = nullptr;
      if (alt_op)
        frontend_redirect(proc_id, bp_id, alt_op->inst_uid, alt_op->inst_info->addr);
      else // If it was stalled due to a fetch barrier, can be nullptr
        frontend_redirect(proc_id, bp_id, 0, 0);
      set_off_path();
      bp_sync(per_core_dfe[proc_id][0]->get_bp_data(), per_core_dfe[proc_id][bp_id]->get_bp_data());
      break;
    case CONTINUE_ON_PREDICTION:
      bp_recover_op(bp_data, cf_type, info);
      dfe_recover_op();
      frontend_redirect(proc_id, bp_id, 0, 0); // Passing fetch_addr = 0 will stop fetching the secondary
      break;
  }
}

void Decoupled_FE::update() {
  uns cf_num = 0;
  uint64_t bytes_this_cycle = 0;
  uint64_t cfs_taken_this_cycle = 0;
  if (!bp_id) {
    fwd_progress++;
    if (fwd_progress >= 100000) {
      std::cout << "No forward progress for 1000000 cycles" << std::endl;
      ASSERT(0,0);
    }
    if (off_path)
      STAT_EVENT(proc_id, FTQ_CYCLES_OFFPATH);
    else
      STAT_EVENT(proc_id, FTQ_CYCLES_ONPATH);
  }

  while(1) {
    ASSERT(proc_id, ftq_num_fts() <= ftq_ft_num);
    ASSERT(proc_id, cfs_taken_this_cycle <= FE_FTQ_TAKEN_CFS_PER_CYCLE);

    if (ftq_num_fts() == ftq_ft_num) {
      DEBUG(proc_id, "[DFE%u] Break due to full FTQ\n", bp_id);
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_FULL_FT_OFFPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_FULL_FT_ONPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      break;
    }
    if (cfs_taken_this_cycle == FE_FTQ_TAKEN_CFS_PER_CYCLE) {
      DEBUG(proc_id, "[DFE%u] Break due to max cfs taken per cycle\n", bp_id);
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_MAX_CFS_TAKEN_OFFPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_MAX_CFS_TAKEN_ONPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      break;
    }
    // use `>=` because inst size does not necessarily align with FE_FTQ_BYTES_PER_CYCLE
    if (bytes_this_cycle >= FE_FTQ_BYTES_PER_CYCLE) {
      DEBUG(proc_id, "[DFE%u] Break due to max bytes per cycle\n", bp_id);
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_MAX_BYTES_OFFPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_MAX_BYTES_ONPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      break;
    }
    if (BP_MECH != MTAGE_BP && !bp_is_predictable(g_bp_data)) {
      DEBUG(proc_id, "[DFE%u] Break due to limited branch predictor\n", bp_id);
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_PRED_BR_OFFPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_PRED_BR_ONPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      break;
    }
    if (stalled) {
      DEBUG(proc_id, "[DFE%u] Break due to wait for fetch barrier resolved\n", bp_id);
      if (off_path)
        STAT_EVENT(proc_id, FTQ_BREAK_BAR_FETCH_OFFPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      else
        STAT_EVENT(proc_id, FTQ_BREAK_BAR_FETCH_ONPATH0 + DFE_BREAK_REASON_COUNT*bp_id);
      break;
    }
    if (!frontend_can_fetch_op(proc_id, bp_id)) {
      if (!bp_id)
        std::cout << "Warning could not fetch inst from frontend" << std::endl;
      break;
    }

    if (!bp_id)
      fwd_progress = 0;
    if (!bp_id  && mp)
      mp->clear_old_fts();

    uint64_t pred_addr = 3;
    Op* op = alloc_op(proc_id, bp_id);
    frontend_fetch_op(proc_id, bp_id, op);
    op->op_num = dfe_op_count++;
    op->off_path = off_path;

    if(op->table_info->cf_type) {
      ASSERT(proc_id, op->eom);
      Op alt_op;
      if (!bp_id)
        alt_op = *op;
      pred_addr = bp_predict_op(bp_data, op, cf_num++, op->inst_info->addr);
      DEBUG(proc_id,
            "[DFE%u] Predict CF fetch_addr:%llx true_npc:%llx pred_npc:%lx mispred:%i misfetch:%i btb miss:%i taken:%i recover_at_decode:%i recover_at_exec:%i off_path:%i bar_fetch:%i\n",
            bp_id, op->inst_info->addr, op->oracle_info.npc, pred_addr,
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
        frontend_redirect(proc_id, bp_id, op->inst_uid, pred_addr);
        redirect_cycle = cycle_count;
        if (!bp_id) {
          for (uns _bp_id = 1; _bp_id < NUM_BPS; ++_bp_id) {
            if (per_core_dfe[proc_id][_bp_id]->get_dfe_recovery_policy() == CONTINUE_ON_PREDICTION
                && !per_core_dfe[proc_id][_bp_id]->is_off_path()) {
              ASSERT(proc_id, !per_core_dfe[proc_id][_bp_id]->ftq_num_fts());
              Addr alt_pred_addr = bp_predict_op(per_core_dfe[proc_id][_bp_id]->bp_data, &alt_op, 0, op->inst_info->addr);
              frontend_redirect(proc_id, _bp_id, alt_op.inst_uid, alt_pred_addr);
              per_core_dfe[proc_id][_bp_id]->set_off_path();
              bp_sync(per_core_dfe[proc_id][bp_id]->get_bp_data(), per_core_dfe[proc_id][_bp_id]->get_bp_data());
            }
          }
        }
      }
      // If we are already on the off-path redirect on all taken branches in TRACE-MODE
      else if (trace_mode && off_path && op->oracle_info.pred == TAKEN) {
        frontend_redirect(proc_id, bp_id, op->inst_uid, pred_addr);
      }
    }
    else {
      ASSERT(0,!(op->oracle_info.recover_at_decode | op->oracle_info.recover_at_exec));
      /* On fetch barrier stall the frontend. */
      if (op->table_info->bar_type & BAR_FETCH) {
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
      DEBUG(proc_id, "[DFE%u] FTQ size: %lu\n", bp_id, ftq_num_fts());
      current_ft_to_push = FT(proc_id);
      if (ft_ended_by == FT_ICACHE_LINE_BOUNDARY) {
        current_ft_to_push.set_ft_started_by(FT_STARTED_BY_ICACHE_LINE_BOUNDARY);
      } else if (ft_ended_by == FT_TAKEN_BRANCH) {
        current_ft_to_push.set_ft_started_by(FT_STARTED_BY_TAKEN_BRANCH);
      } else if (ft_ended_by == FT_BAR_FETCH) {
        current_ft_to_push.set_ft_started_by(FT_STARTED_BY_BAR_FETCH);
      }
    }

    if (off_path) {
      STAT_EVENT(proc_id, FTQ_FETCHED_INS_OFFPATH0 + 2*bp_id);
    }
    else {
      STAT_EVENT(proc_id, FTQ_FETCHED_INS_ONPATH0 + 2*bp_id);
    }

    DEBUG(proc_id,
          "[DFE%u] Push new op to FTQ fetch_addr0x:%llx off_path:%i op_num:%llu dis:%s recovery_addr:%lx fetch_bar:%i\n",
          bp_id, op->inst_info->addr, op->off_path, op->op_num, disasm_op(op, TRUE), recovery_addr, op->table_info->bar_type & BAR_FETCH);
    // Recovery sanity check
    if (!bp_id && recovery_addr) {
      ASSERT(proc_id, recovery_addr == op->inst_info->addr);
      recovery_addr = 0;
    }
  }
}

bool Decoupled_FE::fill_icache_stage_data(int requested, Stage_Data *sd) {
  ASSERT(proc_id, requested && requested <= sd->max_op_count - sd->op_count);
  ASSERT(proc_id, current_ft_in_use.can_fetch_op());

  while (requested && current_ft_in_use.can_fetch_op()) {
    sd->ops[sd->op_count] = current_ft_in_use.fetch_op();
    sd->op_count++;
    requested--;
  }

  return !current_ft_in_use.can_fetch_op();
}

FT_Info Decoupled_FE::fetch_ft() {
  if (ftq.size()) {
    current_ft_in_use = ftq.front();
    ftq.pop_front();
    FT* ft = &current_ft_in_use;

    for (auto&& it : ftq_iterators) {
      // When the icache consumes an FT decrement the iter's offset so it points to the same entry as before
      if (it->ft_pos > 0) {
        ASSERT(proc_id, it->flattened_op_pos >= ft->ops.size());
        it->flattened_op_pos -= ft->ops.size();
        it->ft_pos--;
      } else {
        ASSERT(proc_id, it->flattened_op_pos < ft->ops.size());
        it->flattened_op_pos = 0;
        it->op_pos = 0;
      }
    }

    return ft->ft_info;
  }
  return FT_Info();
}

FT_Info Decoupled_FE::peek_ft() {
  if (ftq.size()) {
    return ftq.front().ft_info;
  } else {
    return FT_Info();
  }
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
  ASSERT(proc_id, iter->op_pos < ftq.at(iter->ft_pos).ops.size());
  *end_of_ft = iter->op_pos == ftq.at(iter->ft_pos).ops.size() - 1;
  return ftq.at(iter->ft_pos).ops[iter->op_pos];
}

Op* Decoupled_FE::ftq_iter_get_next(uns iter_idx, bool *end_of_ft) {
  decoupled_fe_iter* iter = ftq_iterators[iter_idx].get();
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
  for (auto ft = ftq.begin(); ft != ftq.end(); ft++) {
    num_ops += ft->ops.size();
  }
  return num_ops;
}

void Decoupled_FE::stall(Op *op) {
  stalled = true;
  DEBUG(proc_id,
        "[DFE%u] Decoupled fetch stalled due to barrier fetch_addr0x:%llx off_path:%i op_num:%llu\n",
        bp_id, op->inst_info->addr, op->off_path, op->op_num);
}

void Decoupled_FE::retire(Op *op, int op_proc_id, uns64 inst_uid) {
  if((op->table_info->bar_type & BAR_FETCH) || IS_CALLSYS(op->table_info)) {
    stalled = false;
    DEBUG(proc_id,
          "[DFE%u] Decoupled fetch unstalled due to retired barrier fetch_addr0x:%llx off_path:%i op_num:%llu list_count:%i\n",
          bp_id, op->inst_info->addr, op->off_path, op->op_num, td->seq_op_list.count);
    ASSERT(proc_id, td->seq_op_list.count == 1);
  }

  //unblock pin exec driven, trace frontends do not need to block/unblock
  frontend_retire(op_proc_id, inst_uid);
}

Flag Decoupled_FE::determine_to_run_alt_by_mp(Addr fetch_addr) {
  ASSERT(proc_id, dfe_recovery_policy == CONTINUE_ON_MP);

  Flag run_alt = FALSE;
  Addr line_addr = fetch_addr & ~0x3F;
  if (PERFECT_MP)
    run_alt = buf_map_find(line_addr);
  else {
    run_alt = mp->lookup(line_addr, bp_data->global_hist);
  }
  if (run_alt)
    STAT_EVENT(proc_id, MP_DECIDE_TO_RUN);
  else
    STAT_EVENT(proc_id, MP_DECIDE_NOT_TO_RUN);
  return run_alt;
}

Flag Decoupled_FE::determine_to_prefetch_by_mp(Addr fetch_addr) {
  ASSERT(proc_id, dfe_recovery_policy == CONTINUE_ON_MP);

  Flag emit_new_prefetch = FALSE;
  Addr line_addr = fetch_addr & ~0x3F;
  if (PERFECT_MP)
    emit_new_prefetch = buf_map_find(line_addr);
  else {
    emit_new_prefetch = mp->lookup(line_addr, bp_data->global_hist);
  }
  return emit_new_prefetch;
}
