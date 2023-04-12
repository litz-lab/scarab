#include "decoupled_frontend.h"
#include "frontend/frontend_intf.h"
#include "op.h"
#include "op_pool.h"
#include "isa/isa_macros.h"

#include <deque>
#include <vector>
#include <iostream>

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

std::vector<std::deque<Op*>> per_core_ftq;
std::vector<int> per_core_off_path;
std::vector<int> per_core_sched_off_path;
std::vector<uint64_t> per_core_op_count;
std::vector<std::vector<decoupled_fe_iter>> per_core_ftq_iterators;
std::vector<uint64_t> per_core_recovery_addr;
std::vector<uint64_t> per_core_redirect_cycle;
std::vector<bool> per_core_stalled;
//per_core pointers
std::deque<Op*> *df_ftq;
int *off_path;
int *sched_off_path;
int set_proc_id;
std::vector<decoupled_fe_iter> *ftq_iterator;
//need to overwrite op->op_num with decoupeld fe

bool trace_mode;

void alloc_mem_decoupled_fe(uns numCores) {
  per_core_ftq.resize(numCores);
  per_core_off_path.resize(numCores);
  per_core_sched_off_path.resize(numCores);
  per_core_op_count.resize(numCores);
  per_core_ftq_iterators.resize(numCores);
  per_core_recovery_addr.resize(numCores);
  per_core_redirect_cycle.resize(numCores);
  per_core_stalled.resize(numCores);
}

void init_decoupled_fe(uns proc_id, const char*) {
  trace_mode = false;

#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif

  per_core_off_path[proc_id] = false;
  per_core_sched_off_path[proc_id] = false;
  per_core_op_count[proc_id] = 1;
  per_core_recovery_addr[proc_id] = 0;
  per_core_redirect_cycle[proc_id] = 0;
}

void set_decoupled_fe(int proc_id) {

  df_ftq = &(per_core_ftq.data()[proc_id]);
  off_path = &(per_core_off_path.data()[proc_id]);
  sched_off_path = &(per_core_sched_off_path.data()[proc_id]);
  ftq_iterator = &(per_core_ftq_iterators.data()[proc_id]);
}


void reset_decoupled_fe() {}

void recover_decoupled_fe(int proc_id) {
  per_core_off_path[proc_id] = false;
  per_core_sched_off_path[proc_id] = false;
  per_core_recovery_addr[proc_id] = bp_recovery_info->recovery_fetch_addr;

  for (auto it = per_core_ftq[proc_id].begin(); it != per_core_ftq[proc_id].end(); it++) {
    free_op(*it);
  }
  per_core_ftq[proc_id].clear();
  per_core_op_count[proc_id] = bp_recovery_info->recovery_op_num + 1;
  DEBUG(set_proc_id,
        "Recovery signalled fetch_addr0x:%llx\n", bp_recovery_info->recovery_fetch_addr);
  for (auto it = per_core_ftq_iterators[proc_id].begin(); it != per_core_ftq_iterators[proc_id].end(); it++) {
    // When the FTQ flushes, reset all iterators
    it->pos = 0;
  }
  
  auto op = bp_recovery_info->recovery_op;

  if(per_core_stalled[set_proc_id])
    DEBUG(set_proc_id,
          "Decoupled fetch unstalled off-path barrier due to recovery fetch_addr0x:%llx off_path:%i op_num:%llu\n",
          op->inst_info->addr, op->off_path, op->op_num);
  per_core_stalled[set_proc_id] = false;

  if (op->oracle_info.recover_at_decode)
    STAT_EVENT(proc_id, FTQ_RECOVER_DECODE);
  else if (op->oracle_info.recover_at_exec)
    STAT_EVENT(proc_id, FTQ_RECOVER_EXEC);

  uint64_t offpath_cycles = cycle_count - per_core_redirect_cycle[proc_id];
  ASSERT(proc_id, cycle_count > per_core_redirect_cycle[proc_id]);
  INC_STAT_EVENT(proc_id, FTQ_OFFPATH_CYCLES, offpath_cycles);
  per_core_redirect_cycle[proc_id] = 0;

  //FIXME always fetch off path ops? should we get rid of this parameter?
  if(FETCH_OFF_PATH_OPS) {
    frontend_recover(proc_id, bp_recovery_info->recovery_inst_uid);
    ASSERTM(proc_id, bp_recovery_info->recovery_fetch_addr == frontend_next_fetch_addr(proc_id),
            "Scarab's recovery addr 0x%llx does not match frontend's recovery "
            "addr 0x%llx\n",
            bp_recovery_info->recovery_fetch_addr, frontend_next_fetch_addr(proc_id));
  }
}

void debug_decoupled_fe() {

}

void update_decoupled_fe() {
  uint fetched_inst_bytes = 0;
  uint taken_cf = 0;
  uint predicted_branches = 0;
  uns cf_num = 0;

  while(1) {
    if (df_ftq->size() >= FE_FTQ_SIZE) {
      if (*off_path)
        STAT_EVENT(set_proc_id, FTQ_BREAK_FULL_OFFPATH);
      else
        STAT_EVENT(set_proc_id, FTQ_BREAK_FULL_ONPATH);
      break;
    }
    if (predicted_branches == FE_PREDICTED_BRANCHES_PER_CYCLE) {
      if (*off_path)
        STAT_EVENT(set_proc_id, FTQ_BREAK_PRED_BR_OFFPATH);
      else
        STAT_EVENT(set_proc_id, FTQ_BREAK_PRED_BR_ONPATH);
      break;
    }
    if (taken_cf == FE_TAKEN_CF_PER_CYCLE) {
      if (*off_path)
        STAT_EVENT(set_proc_id, FTQ_BREAK_TAKEN_CF_OFFPATH);
      else
        STAT_EVENT(set_proc_id, FTQ_BREAK_TAKEN_CF_ONPATH);

      break;
    }
    if (fetched_inst_bytes >= FE_FETCHED_INST_BYTES_PER_CYCLE) {
      if (*off_path)
        STAT_EVENT(set_proc_id, FTQ_BREAK_MAX_BYTES_OFFPATH);
      else
        STAT_EVENT(set_proc_id, FTQ_BREAK_MAX_BYTES_ONPATH);
      
      break;
    }
    if (!frontend_can_fetch_op(set_proc_id)) {
      std::cout << "Warning could not fetch inst from frontend" << std::endl;
      break;
    }

    uint64_t pred_addr = 3;
    Op* op = alloc_op(set_proc_id);
    frontend_fetch_op(set_proc_id, op);
    op->op_num = per_core_op_count[set_proc_id]++;
    op->off_path = *off_path;

    if(op->table_info->cf_type) {
      ASSERT(set_proc_id, op->eom);
      DEBUG(set_proc_id,
            "Predict Branch fetch_addr0x:%llx off_path:%i bar_fetch:%i\n",
            op->inst_info->addr, *off_path, op->table_info->bar_type & BAR_FETCH);
      pred_addr = bp_predict_op(g_bp_data, op, cf_num++, op->inst_info->addr);
        
      if (op->oracle_info.recover_at_decode || op->oracle_info.recover_at_exec) {
        DEBUG(set_proc_id,
              "Mispredict CF fetch_addr:%llx true_npc:%llx pred_npc:%lx mispred:%i misfetch:%i btb miss:%i taken:%i recover_at_decode:%i recover_at_exec:%i\n",
              op->inst_info->addr, op->oracle_info.npc, pred_addr,
              op->oracle_info.mispred, op->oracle_info.misfetch,
              op->oracle_info.btb_miss, op->oracle_info.pred == TAKEN,
              op->oracle_info.recover_at_decode, op->oracle_info.recover_at_exec);
        ASSERT(0, (int)op->oracle_info.recover_at_decode + (int)op->oracle_info.recover_at_exec < 2);
        /* If already on the off-path do not schedule recovery as scarab cannot recover OOO
           (An older op may recover at exec and a younger op may recover at decode)
           This is not accurate but it should not affect the time spend on the off-path */
        if (*off_path) {
          op->oracle_info.recover_at_decode = FALSE;
          op->oracle_info.recover_at_exec = FALSE;
        }
        *off_path = true;
        frontend_redirect(set_proc_id, op->inst_uid, pred_addr);
        per_core_redirect_cycle[set_proc_id] = cycle_count;
      }
      // If we are already on the off-path redirec on all taken branches in TRACE-MODE
      else if (trace_mode && *off_path && op->oracle_info.pred == TAKEN) {
        frontend_redirect(set_proc_id, op->inst_uid, pred_addr);
      }
      taken_cf = (op->oracle_info.pred == TAKEN) ? taken_cf + 1 : taken_cf;
    }
    else
      ASSERT(0,!(op->oracle_info.recover_at_decode | op->oracle_info.recover_at_exec));

    if (op->table_info->cf_type == CF_CBR) {
      predicted_branches++;
    }
    
    df_ftq->emplace_back(op);
    
    fetched_inst_bytes += op->inst_info->trace_info.inst_size;

    if (op->table_info->bar_type & BAR_FETCH && op->eom) {
      // If this is a mispredicted CF, wait for decode to recover, decode will then stall FE
      if (!op->oracle_info.recover_at_decode) {
        decoupled_fe_stall(op);
      }
      ASSERT(set_proc_id, !op->oracle_info.recover_at_exec);
    }

    if (*off_path) {
      STAT_EVENT(set_proc_id, FTQ_FETCHED_INS_OFFPATH);
    }
    else {
      STAT_EVENT(set_proc_id, FTQ_FETCHED_INS_ONPATH);
    }
      
    DEBUG(set_proc_id,
          "Push new op to FTQ fetch_addr0x:%llx off_path:%i op_num:%llu dis:%s recovery_addr:%lx\n",
          op->inst_info->addr, op->off_path, op->op_num, disasm_op(op, TRUE), per_core_recovery_addr[set_proc_id]);
    // Recovery sanity check
    if (per_core_recovery_addr[set_proc_id]) {
      ASSERT(set_proc_id, per_core_recovery_addr[set_proc_id] == op->inst_info->addr);
      per_core_recovery_addr[set_proc_id] = 0;
    }
  }
}

bool decoupled_fe_fetch_op(Op** op, int proc_id) {
  ASSERT(proc_id, (uns)proc_id<per_core_ftq.size());
  if (per_core_ftq[proc_id].size()) {
    *op = per_core_ftq[proc_id].front();
    per_core_ftq[proc_id].pop_front();
    DEBUG(set_proc_id,
          "Fetch op from FTQ fetch_addr0x:%llx off_path:%i op_num:%llu\n",
          (*op)->inst_info->addr, (*op)->off_path, (*op)->op_num);
    for (auto it = per_core_ftq_iterators[proc_id].begin(); it != per_core_ftq_iterators[proc_id].end(); it++) {
      // When the icache consumes an op decrement the iter's offset so it points to the same entry as before
      if (it->pos) {
        it->pos--;
      }
    }
    return true;
  }
  return false;
}

bool decoupled_fe_can_fetch_op(int proc_id) {
  return per_core_ftq[proc_id].size() > 0;
}

uint64_t decoupled_fe_next_fetch_addr(int proc_id) {
  //ASSERT(proc_id, per_core_ftq[proc_id].size() > 0);
  if(!per_core_ftq[proc_id].size())
    return frontend_next_fetch_addr(proc_id);
  
  return per_core_ftq[proc_id].front()->inst_info->addr;
}

void decoupled_fe_return_op(Op *op) {
  df_ftq->emplace_front(op);
  DEBUG(set_proc_id,
        "Return fetched op backt to FTQ fetch_addr0x:%llx off_path:%i\n",
        op->inst_info->addr, *off_path);
  for (auto it = per_core_ftq_iterators[set_proc_id].begin(); it != per_core_ftq_iterators[set_proc_id].end(); it++) {
    // When the icache returns an op increment the iter's offset so it points to the same entry as before
    if (it->pos) {
      it->pos++;
    }
  }
  
}
  
decoupled_fe_iter* decoupled_fe_new_ftq_iter() {
  per_core_ftq_iterators[set_proc_id].push_back(decoupled_fe_iter());
  return &per_core_ftq_iterators[set_proc_id].back();
}

/* Returns the Op at current FTQ iterator position. Returns NULL if the FTQ is empty */ 
Op* decoupled_fe_ftq_iter_get(decoupled_fe_iter* iter) {
  if (df_ftq->size() == 0) {
    ASSERT(set_proc_id, iter->pos == 0);
    return NULL;
  }
  ASSERT(set_proc_id, iter->pos >= 0);
  ASSERT(set_proc_id, iter->pos < df_ftq->size());
  return df_ftq->at(iter->pos);
}

/* Returns true if advanced, false if reached end of FTQ */
bool decoupled_fe_ftq_iter_advance(decoupled_fe_iter* iter) {
  if (iter->pos + 1 == df_ftq->size()) {
    return false;
  }
  iter->pos++;
  return true;
}

/* Returns iter offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_offset(decoupled_fe_iter* iter) {
  return iter->pos;
}

void decoupled_fe_stall(Op *op) {
  per_core_stalled[set_proc_id] = true;
  DEBUG(set_proc_id,
        "Decoupled fetch stalled due to barrier fetch_addr0x:%llx off_path:%i op_num:%llu\n",
        op->inst_info->addr, op->off_path, op->op_num);
}

void decoupled_fe_retire(Op *op, int proc_id, uns64 inst_uid) {
  if(op->table_info->bar_type & BAR_FETCH) {
    per_core_stalled[set_proc_id] = false;
    DEBUG(set_proc_id,
          "Decoupled fetch unstalled due to retired barrier fetch_addr0x:%llx off_path:%i op_num:%llu\n",
          op->inst_info->addr, op->off_path, op->op_num);
  }
  else {
    ASSERT(proc_id, !IS_CALLSYS(op->table_info)); //bar fetch should be set for syscal?
  }
  //unblock pin exec driven, trace frontends do not need to block/unblock
  frontend_retire(proc_id, inst_uid);
}
