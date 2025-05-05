#include "prefetcher/pref.param.h"

// Implementations of the API
#include "confidence/btb_miss_bp_taken_conf.hpp"
#include "confidence/conf.hpp"
#include "confidence/weight_conf.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CONF, ##args)

void ConfMechStatBase::update(Op* op, Conf_Off_Path_Reason reason, bool last_in_ft, bool new_cycle) {
  if (!CONFIDENCE_ENABLE)
    return;
  if (!conf_off_path_reason && reason)
    conf_off_path_reason = reason;
  DEBUG(proc_id, "prev_op: %p, op: %p\n", prev_op, op);

  if (prev_op == nullptr || op == nullptr)
    return;
  DEBUG(proc_id, "Updating confidence mech for op %llu\n", op->op_num);
  if (prev_op && prev_op->op_num == op->op_num) {
    DEBUG(proc_id, "Previous op is the same as current op %llu\n", op->op_num);
    return;
  }

  DEBUG(proc_id, "off_path_reason: %d, conf_off_path_reason: %d\n", off_path_reason, reason);

  Flag dfe_off_path = op->off_path;

  // check for on/off events
  if (dfe_off_path && !prev_op->off_path) {  // the actual path goes off
    DEBUG(proc_id, "off-path event: prev_op op_num: %llu, cf_type: %i, cur_op op_num: %llu, cf_type: %i\n",
          prev_op->op_num, prev_op->table_info->cf_type, decoupled_fe_get_cur_op()->op_num,
          decoupled_fe_get_cur_op()->table_info->cf_type);
    ASSERT(proc_id, off_path_reason == REASON_NOT_IDENTIFIED);
    ASSERT(proc_id, prev_op->table_info->cf_type);  // must be a cf as the last on-path op
    ASSERT(proc_id, prev_op->off_path_reason != REASON_NOT_IDENTIFIED);
    off_path_reason = (Off_Path_Reason)prev_op->off_path_reason;

    if (!op->conf_off_path) {
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NUM_EVENTS);
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NOT_IDENTIFIED_EVENTS + off_path_reason);
    }
  } else if (!dfe_off_path && !prev_op->conf_off_path &&
             op->conf_off_path) {  // the actual path is on, but conf off path
    STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_EVENTS);
    STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NOT_IDENTIFIED + conf_off_path_reason);
  }

  // log on/off stats
  if (dfe_off_path) {
    // dfe off conf off
    if (op->conf_off_path) {
      STAT_EVENT(proc_id, DFE_OFF_CONF_OFF_REALISTIC_OPS + perfect_off_path);
      if (last_in_ft) {
        STAT_EVENT(proc_id, DFE_OFF_CONF_OFF_REALISTIC_FETCH_TARGETS + perfect_off_path);
      }
      if (new_cycle) {
        STAT_EVENT(proc_id, DFE_OFF_CONF_OFF_REALISTIC_CYCLES + perfect_off_path);
      }
      // dfe off conf on
    } else {
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_OPS);
      if (last_in_ft) {
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_FETCH_TARGETS);
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NOT_IDENTIFIED_FETCH_TARGETS + off_path_reason);
      }
      if (new_cycle) {
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_CYCLES);
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NOT_IDENTIFIED_CYCLES + off_path_reason);
      }
    }
  } else {
    // dfe on conf off
    if (op->conf_off_path) {
      STAT_EVENT(proc_id, DFE_ON_CONF_OFF_OPS);
      if (last_in_ft) {
        STAT_EVENT(proc_id, DFE_ON_CONF_OFF_FETCH_TARGETS);
        STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NOT_IDENTIFIED_FETCH_TARGETS + conf_off_path_reason);
      }
      if (new_cycle) {
        STAT_EVENT(proc_id, DFE_ON_CONF_OFF_CYCLES);
        STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NOT_IDENTIFIED_CYCLES + conf_off_path_reason);
      }
      // dfe on conf on
    } else {
      STAT_EVENT(proc_id, DFE_ON_CONF_ON_OPS);
      if (last_in_ft)
        STAT_EVENT(proc_id, DFE_ON_CONF_ON_FETCH_TARGETS);
      if (new_cycle)
        STAT_EVENT(proc_id, DFE_ON_CONF_ON_CYCLES);
    }
  }
  ext_update(op, reason, last_in_ft, new_cycle);
}

// first seen by dfe
void ConfMechStatBase::log_off_path_event(Op* op) {
  if (!CONFIDENCE_ENABLE || !CONF_LOG_DFE_TO_REC)
    return;
  Off_Path_Reason op_reason = (Off_Path_Reason)op->off_path_reason;
  if (!off_path_reason)
    return;
  std::get<0>(resteer_ops_cycles[op->op_num]) = cycle_count;
  std::get<1>(resteer_ops_cycles[op->op_num]) = 0;
  std::get<2>(resteer_ops_cycles[op->op_num]) = op_reason;

  std::get<0>(resteer_ops_ops[op->op_num]) = cnt_total_ops;
  std::get<1>(resteer_ops_ops[op->op_num]) = 0;
  std::get<2>(resteer_ops_ops[op->op_num]) = op_reason;
}

// resolved
void ConfMechStatBase::log_resolution(Op* op) {
  if (!CONFIDENCE_ENABLE || !CONF_LOG_DFE_TO_REC)
    return;
  std::get<1>(resteer_ops_cycles[op->op_num]) = cycle_count;
  std::get<2>(resteer_ops_cycles[op->op_num]) = (Off_Path_Reason)op->off_path_reason;

  std::get<1>(resteer_ops_cycles[op->op_num]) = cnt_total_ops;
  std::get<2>(resteer_ops_cycles[op->op_num]) = (Off_Path_Reason)op->off_path_reason;
  DEBUG(proc_id, "Op off-path reason");
}

void ConfMechStatBase::recover(Op* op) {
  DEBUG(proc_id, "Recovering confidence mech stat base for op %llu\n", op->op_num);
  ext_recover(op);
  prev_op = nullptr;
  off_path_reason = REASON_NOT_IDENTIFIED;
  conf_off_path_reason = REASON_CONF_NOT_IDENTIFIED;
}

void ConfMechStatBase::print_data() {
  DEBUG(proc_id, "Printing confidence mech data for proc %u\n", proc_id);
  FILE* fp;
  if (CONF_LOG_DFE_TO_REC) {
    fp = fopen("off_path_events_cycles.csv", "w");
    fprintf(fp, "op_num,dfe_cycle,resolved_cycle,off_path_reason\n");
    for (const std::pair<const Counter, std::tuple<Counter, Counter, Off_Path_Reason>>& line : resteer_ops_cycles) {
      fprintf(fp, "%llu,%llu,%llu,%d", line.first, std::get<0>(line.second), std::get<1>(line.second),
              std::get<2>(line.second));
      fprintf(fp, "\n");
    }
    fclose(fp);
    fp = fopen("off_path_events_ops.csv", "w");
    fprintf(fp, "op_num,dfe_num_ops,resolved_num_ops,off_path_reason\n");
    for (const std::pair<const Counter, std::tuple<Counter, Counter, Off_Path_Reason>>& line : resteer_ops_ops) {
      fprintf(fp, "%llu,%llu,%llu,%d", line.first, std::get<0>(line.second), std::get<1>(line.second),
              std::get<2>(line.second));
      fprintf(fp, "\n");
    }
    fclose(fp);
  }
  ext_print_data();
}

void ConfMechStatBase::set_prev_op(Op* op) {
  prev_op = op;
  DEBUG(proc_id, "Set prev_op off_path:%i, op_num:%llu, cf_type:%i\n", prev_op->off_path, prev_op->op_num,
        prev_op->table_info->cf_type);
}

/* Conf member functions */
Conf::Conf(uns _proc_id) : proc_id(_proc_id), conf_off_path(false), last_cycle_count(0) {
  if (CONF_BTB_MISS_BP_TAKEN)
    conf_mech = new BTBMissBPTakenConf(_proc_id);
  else
    conf_mech = new WeightConf(_proc_id);
}

void Conf::recover(Op* op) {
  DEBUG(proc_id, "Recovering confidence mech stat base for op %llu\n", op->op_num);
  conf_off_path = false;
  conf_mech->conf_mech_stat->recover(op);
  conf_mech->recover(op);
}

void Conf::set_prev_op(Op* op) {
  conf_mech->conf_mech_stat->set_prev_op(op);
}

void Conf::update(Op* op, Flag last_in_ft) {
  if (!CONFIDENCE_ENABLE)
    return;
  Conf_Off_Path_Reason new_reason = REASON_CONF_NOT_IDENTIFIED;
  perfect_conf_update(op, new_reason);
  bool new_cycle = cycle_count > last_cycle_count;
  if (!new_reason || get_off_path_reason() == REASON_NOT_IDENTIFIED ||
      get_conf_off_path_reason() == REASON_CONF_NOT_IDENTIFIED) {  // update until both real/confidence path go off
    per_op_update(op, new_reason);
    if (op->table_info->cf_type)
      per_cf_op_update(op, new_reason);
    if (last_in_ft)
      per_ft_update(op, new_reason);
    if (new_cycle) {
      last_cycle_count = cycle_count;
      per_cycle_update(op, new_reason);
    }
    conf_off_path = new_reason != REASON_CONF_NOT_IDENTIFIED;
  }
  op->conf_off_path = conf_off_path;
  STAT_EVENT(proc_id, REASON_CONF_NOT_IDENTIFIED + new_reason);
  conf_mech->conf_mech_stat->update(op, new_reason, last_in_ft, new_cycle);
  STAT_EVENT(proc_id, CONF_OFF_PATH_REASON_NOT_IDENTIFIED + new_reason);
  set_prev_op(op);
}

void Conf::perfect_conf_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  if (!PERFECT_CONFIDENCE && !CONF_PERFECT_BTB_MISS_CONF && !CONF_PERFECT_IBTB_MISS_CONF &&
      !CONF_PERFECT_MISFETCH_CONF && !CONF_PERFECT_MISPRED_CONF)
    return;
  if (PERFECT_CONFIDENCE) {
    if (decoupled_fe_is_off_path())
      conf_off_path = true;
    if (conf_off_path)
      ASSERT(proc_id, decoupled_fe_is_off_path());
    update_state_perfect_conf(op);
    conf_mech->conf_mech_stat->set_perfect_off_path();
    new_reason = REASON_PERFECT_CONF;
  } else {
    Off_Path_Reason off_path_reason = (Off_Path_Reason)op->off_path_reason;
    // add perfect to conf_op_reason
    if ((CONF_PERFECT_MISPRED_CONF &&
         (off_path_reason == REASON_MISPRED || off_path_reason == REASON_BTB_MISS_MISPRED)) ||
        (CONF_PERFECT_BTB_MISS_CONF &&
         (off_path_reason == REASON_BTB_MISS || off_path_reason == REASON_BTB_MISS_MISPRED)) ||
        (CONF_PERFECT_IBTB_MISS_CONF && (off_path_reason == REASON_IBTB_MISS)) ||
        (CONF_PERFECT_MISFETCH_CONF && (off_path_reason == REASON_MISFETCH))) {
      conf_mech->conf_mech_stat->set_perfect_off_path();
      new_reason = REASON_PERFECT_CONF;
    }
  }
}

void Conf::per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  conf_mech->per_op_update(op, new_reason);
}

void Conf::per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  conf_mech->per_cf_op_update(op, new_reason);

  // log conf stats
  // if it is a cf with bp conf
  if ((op)->table_info->cf_type == CF_CBR || (op)->table_info->cf_type == CF_IBR ||
      (op)->table_info->cf_type == CF_ICALL) {
    if (op->oracle_info.mispred) {
      // reorder stats
      STAT_EVENT(proc_id, DFE_CONF_0_MISPRED + op->bp_confidence);
    } else {
      STAT_EVENT(proc_id, DFE_CONF_0_CORRECT + op->bp_confidence);
    }
  }
}

void Conf::per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  conf_mech->per_ft_update(op, new_reason);
}

void Conf::per_cycle_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  conf_mech->per_cycle_update(op, new_reason);
}