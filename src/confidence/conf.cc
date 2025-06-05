#include "prefetcher/pref.param.h"

// Implementations of the API
#include "confidence/btb_miss_bp_taken_conf.hpp"
#include "confidence/conf.hpp"
#include "confidence/weight_conf.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CONF, ##args)

void ConfMechStatBase::per_cycle_update(Conf_Off_Path_Reason reason) {
  if (conf_off_path_reason == REASON_CONF_NOT_IDENTIFIED && reason != REASON_CONF_NOT_IDENTIFIED)
    conf_off_path_reason = reason;
  if (off_path_reason) {
    if (conf_off_path_reason) {
      STAT_EVENT(proc_id, DFE_OFF_CONF_OFF_REALISTIC_CYCLES + perfect_off_path);
    } else {
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_CYCLES);
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NOT_IDENTIFIED_CYCLES + off_path_reason);
    }
  } else {
    if (conf_off_path_reason) {
      STAT_EVENT(proc_id, DFE_ON_CONF_OFF_CYCLES);
      STAT_EVENT(proc_id, DFE_ON_CONF_OFF_IBTB_MISS_BP_TAKEN_CYCLES + conf_off_path_reason);
    } else {
      STAT_EVENT(proc_id, DFE_ON_CONF_ON_CYCLES);
    }
  }
  DEBUG(proc_id, "stat cycle count: %llu\n", cycle_count);
}

void ConfMechStatBase::update(Op* op, Conf_Off_Path_Reason reason, bool last_in_ft) {
  // this function should be called at the BEGINNING of derived class's update function
  ASSERT(proc_id, CONFIDENCE_ENABLE);
  // set conf_off_path_reason
  if (conf_off_path_reason == REASON_CONF_NOT_IDENTIFIED && reason != REASON_CONF_NOT_IDENTIFIED)
    conf_off_path_reason = reason;
  DEBUG(proc_id, "prev_op: %p, op: %p\n", prev_op, op);
  DEBUG(proc_id, "off_path_reason: %d, conf_off_path_reason: %d\n", off_path_reason, reason);

  Flag dfe_off_path = op->off_path;

  // log on/off stats for ops, fetch targets, and cycles
  if (dfe_off_path) {
    // dfe off conf off
    if (op->conf_off_path) {
      STAT_EVENT(proc_id, DFE_OFF_CONF_OFF_REALISTIC_OPS + perfect_off_path);
      if (last_in_ft) {
        STAT_EVENT(proc_id, DFE_OFF_CONF_OFF_REALISTIC_FETCH_TARGETS + perfect_off_path);
      }
      // dfe off conf on
    } else {
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_OPS);
      if (last_in_ft) {
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_FETCH_TARGETS);
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NOT_IDENTIFIED_FETCH_TARGETS + off_path_reason);
      }
    }
  } else {
    // dfe on conf off
    if (op->conf_off_path) {
      STAT_EVENT(proc_id, DFE_ON_CONF_OFF_OPS);
      if (last_in_ft) {
        STAT_EVENT(proc_id, DFE_ON_CONF_OFF_FETCH_TARGETS);
        STAT_EVENT(proc_id, DFE_ON_CONF_OFF_IBTB_MISS_BP_TAKEN_FETCH_TARGETS + conf_off_path_reason);
      }
      // dfe on conf on
    } else {
      STAT_EVENT(proc_id, DFE_ON_CONF_ON_OPS);
      if (last_in_ft)
        STAT_EVENT(proc_id, DFE_ON_CONF_ON_FETCH_TARGETS);
    }
  }

  if (prev_op == nullptr)
    return;
  DEBUG(proc_id, "Updating confidence mech for op %llu\n", op->op_num);
  if (prev_op && prev_op->op_num == op->op_num) {
    DEBUG(proc_id, "Previous op is the same as current op %llu\n", op->op_num);
    return;
  }

  // log stats for on/off events
  if (dfe_off_path && !prev_op->off_path) {  // the actual path goes off
    DEBUG(proc_id, "off-path event: prev_op op_num: %llu, cf_type: %i, cur_op op_num: %llu, cf_type: %i\n",
          prev_op->op_num, prev_op->table_info->cf_type, decoupled_fe_get_cur_op()->op_num,
          decoupled_fe_get_cur_op()->table_info->cf_type);
    ASSERT(proc_id, off_path_reason == REASON_NOT_IDENTIFIED);
    ASSERT(proc_id, prev_op->table_info->cf_type);  // must be a cf as the last on-path op
    ASSERT(proc_id, prev_op->oracle_info.off_path_reason != REASON_NOT_IDENTIFIED);
    off_path_reason = (Off_Path_Reason)prev_op->oracle_info.off_path_reason;

    if (!op->conf_off_path) {
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NUM_EVENTS);
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NOT_IDENTIFIED_EVENTS + off_path_reason);
    }
  } else if (!dfe_off_path && !prev_op->conf_off_path &&
             op->conf_off_path) {  // the actual path is on, but conf off path
    ASSERT(proc_id, conf_off_path_reason != REASON_CONF_NOT_IDENTIFIED);
    STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_EVENTS);
    STAT_EVENT(proc_id, DFE_ON_CONF_OFF_IBTB_MISS_BP_TAKEN + conf_off_path_reason);
  }
}

void ConfMechStatBase::recover(Op* op) {
  DEBUG(proc_id, "Recovering confidence mech stat base for op %llu\n", op->op_num);
  // this function should be called at the END of derived class's recover function
  prev_op = nullptr;
  off_path_reason = REASON_NOT_IDENTIFIED;
  conf_off_path_reason = REASON_CONF_NOT_IDENTIFIED;
  perfect_off_path = false;
}

void ConfMechStatBase::print_data() {
  DEBUG(proc_id, "Printing confidence mech data for proc %u\n", proc_id);
  // order of calling this within derived class is unimportant
}

void ConfMechStatBase::set_prev_op(Op* op) {
  prev_op = op;
  DEBUG(proc_id, "Set prev_op off_path:%i, op_num:%llu, cf_type:%i\n", prev_op->off_path, prev_op->op_num,
        prev_op->table_info->cf_type);
}

/* Conf member functions */
Conf::Conf(uns _proc_id) : proc_id(_proc_id), conf_off_path(false), last_cycle_count(0) {
  if (CONFIDENCE_MECH == CONF_MECH_BTB_MISS_BP_TAKEN)
    conf_mech = new BTBMissBPTakenConf(_proc_id);
  else if (CONFIDENCE_MECH == CONF_MECH_WEIGHT)
    conf_mech = new WeightConf(_proc_id);
  else
    ASSERT(proc_id, FALSE);
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

void Conf::process_op(Op* op, Conf_Off_Path_Reason& new_reason, bool last_in_ft) {
  op->conf_off_path = conf_off_path;
  if (!conf_off_path) {
    perfect_conf_update(op, new_reason);
    if (!PERFECT_CONFIDENCE && new_reason == REASON_CONF_NOT_IDENTIFIED) {
      per_op_update(op, new_reason);
      if (op->table_info->cf_type)
        per_cf_op_update(op, new_reason);
    }
  }

  conf_off_path |= (new_reason != REASON_CONF_NOT_IDENTIFIED);
  conf_mech->conf_mech_stat->update(op, new_reason, last_in_ft);
  STAT_EVENT(proc_id, CONF_OFF_IBTB_MISS_BP_TAKEN + new_reason);
  set_prev_op(op);
}

void Conf::update(FT pushed_ft) {
  ASSERT(proc_id, CONFIDENCE_ENABLE);

  std::vector<Op*> ops = pushed_ft.get_ops();
  ASSERT(proc_id, !ops.empty());

  Conf_Off_Path_Reason new_reason = REASON_CONF_NOT_IDENTIFIED;

  for (auto op = ops.begin(); op != ops.end() - 1; ++op) {
    process_op(*op, new_reason, false);
  }
  per_ft_update(ops.back(), new_reason);
  process_op(ops.back(), new_reason, true);
}

void Conf::perfect_conf_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  if (!PERFECT_CONFIDENCE && !CONF_PERFECT_BTB_MISS_CONF && !CONF_PERFECT_IBTB_MISS_CONF &&
      !CONF_PERFECT_MISFETCH_CONF && !CONF_PERFECT_MISPRED_CONF)
    return;
  if (PERFECT_CONFIDENCE) {
    if (op->oracle_info.off_path_reason) {
      ASSERT(proc_id, conf_mech->conf_mech_stat->perfect_off_path == false);
      DEBUG(proc_id, "Perfect conf update for op %llu, off_path_reason: %d, off_path %d\n", op->op_num,
            op->oracle_info.off_path_reason, op->off_path);
      new_reason = REASON_PERFECT_CONF;
      conf_mech->conf_mech_stat->perfect_off_path = true;
    }
    if (conf_off_path)
      ASSERT(proc_id, decoupled_fe_is_off_path());
    update_state_perfect_conf(op);
  } else {
    Off_Path_Reason off_path_reason = (Off_Path_Reason)op->oracle_info.off_path_reason;
    // add perfect to conf_op_reason
    if ((CONF_PERFECT_MISPRED_CONF &&
         (off_path_reason == REASON_MISPRED || off_path_reason == REASON_BTB_MISS_MISPRED)) ||
        (CONF_PERFECT_BTB_MISS_CONF &&
         (off_path_reason == REASON_BTB_MISS || off_path_reason == REASON_BTB_MISS_MISPRED)) ||
        (CONF_PERFECT_IBTB_MISS_CONF && (off_path_reason == REASON_IBTB_MISS)) ||
        (CONF_PERFECT_MISFETCH_CONF && (off_path_reason == REASON_MISFETCH))) {
      ASSERT(proc_id, conf_mech->conf_mech_stat->perfect_off_path == false);
      conf_mech->conf_mech_stat->perfect_off_path = true;
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

void Conf::per_cycle_update() {
  Conf_Off_Path_Reason new_reason = REASON_CONF_NOT_IDENTIFIED;
  if (!PERFECT_CONFIDENCE) {
    conf_mech->per_cycle_update(new_reason);
    conf_off_path |= new_reason != REASON_CONF_NOT_IDENTIFIED;
  }
  conf_mech->conf_mech_stat->per_cycle_update(new_reason);
  STAT_EVENT(proc_id, CONF_OFF_IBTB_MISS_BP_TAKEN + new_reason);
}