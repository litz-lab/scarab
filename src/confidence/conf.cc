#include "prefetcher/pref.param.h"

// Implementations of the API
#include "confidence/btb_miss_bp_taken_conf.hpp"
#include "confidence/conf.hpp"
#include "confidence/weight_conf.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

Off_Path_Reason ConfMechBase::eval_off_path_reason(Op* op) {
  // if the instruction is not a resteer op no reason to log
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

/* Confidence_Info member functions */
void Confidence_Info::inc_br_conf_counters(int conf) {
  switch (conf) {
    case 0:
      num_conf_0_branches += 1;
      break;
    case 1:
      num_conf_1_branches += 1;
      break;
    case 2:
      num_conf_2_branches += 1;
      break;
    case 3:
      num_conf_3_branches += 1;
      break;
    default:
      DEBUG(proc_id, "inc_br_conf_counters: invalid conf value\n");
      break;
  }
}

void Confidence_Info::inc_cf_type_counters(Cf_Type cf_type) {
  switch (cf_type) {
    case NOT_CF:
      DEBUG(proc_id, "inc_cf_type_counters: instruction is not a cf inst.\n");
      break;
    case CF_BR:
      num_cf_br += 1;
      break;
    case CF_CBR:
      num_cf_cbr += 1;
      break;
    case CF_CALL:
      num_cf_call += 1;
      break;
    case CF_IBR:
      num_cf_ibr += 1;
      break;
    case CF_ICALL:
      num_cf_icall += 1;
      break;
    case CF_ICO:
      num_cf_ico += 1;
      break;
    case CF_RET:
      num_cf_ret += 1;
      break;
    case CF_SYS:
      num_cf_sys += 1;
      break;
    default:
      DEBUG(proc_id, "inc_cf_type_counters: instruction is not a valid cf inst.\n");
      break;
  }
}

Off_Path_Reason Confidence_Info::eval_off_path_reason(Op* op) {
    // if the instruction is not a resteer op no reason to log
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

void Confidence_Info::update(Op* op, Flag conf_off_path, Conf_Off_Path_Reason new_reason) {
  DEBUG(proc_id, "off_path_reason: %d, conf_off_path_reason: %d\n", off_path_reason, conf_off_path_reason);

  if (!prev_op || (conf_off_path_reason != REASON_CONF_NOT_IDENTIFIED && off_path_reason != REASON_NOT_IDENTIFIED))
    return;

  if (op->table_info->cf_type) {
    if (op->oracle_info.btb_miss)
      num_BTB_misses += 1;
    inc_br_conf_counters(op->bp_confidence);
    inc_cf_type_counters(op->table_info->cf_type);
    DEBUG(proc_id, "op->bp_confidence: %d, conf: %d, off_path: %d\n", op->bp_confidence, decoupled_fe_get_conf(),
          op->off_path ? 1 : 0);
  }
  Flag dfe_off_path = op->off_path;
  // change off_path_reason check to an assertion
  if (dfe_off_path && !prev_op->off_path && off_path_reason == REASON_NOT_IDENTIFIED) {  // the actual path goes off
    DEBUG(proc_id, "prev_op op_num: %llu, cf_type: %i, cur_op op_num: %llu, cf_type: %i\n", prev_op->op_num,
          prev_op->table_info->cf_type, decoupled_fe_get_cur_op()->op_num,
          decoupled_fe_get_cur_op()->table_info->cf_type);
    ASSERT(proc_id, prev_op->table_info->cf_type);  // must be a cf as the last on-path op
    off_path_reason = eval_off_path_reason(op);

    if (!conf_off_path) {
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NUM_EVENTS);
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NOT_IDENTIFIED + off_path_reason);
    }
  } else if (!dfe_off_path && prev_op->off_path) {  // the actual path is on, but conf off path
    STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_EVENTS);

    STAT_EVENT(proc_id, DFE_ON_CONF_OFF_BTB_MISS_BP_TAKEN_CONF_0 + conf_off_path_reason);

    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CF_BR, num_cf_br);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CF_CBR, num_cf_cbr);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CF_CALL, num_cf_call);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CF_IBR, num_cf_ibr);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CF_ICALL, num_cf_icall);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CF_ICO, num_cf_ico);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CF_RET, num_cf_ret);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CF_SYS, num_cf_sys);

    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CONF_0_BR, num_conf_0_branches);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CONF_1_BR, num_conf_1_branches);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CONF_2_BR, num_conf_2_branches);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_CONF_3_BR, num_conf_3_branches);

    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_BTB_MISS, num_BTB_misses);
    INC_STAT_EVENT(proc_id, DFE_ON_CONF_OFF_NUM_OP_DIST_INC, num_op_dist_incs);
  }

  if (new_reason != REASON_CONF_NOT_IDENTIFIED) {
    DEBUG(proc_id, "conf_off_path_reason updated to %d\n", new_reason);
    conf_off_path_reason = new_reason;
  }
}

void Confidence_Info::recover() {
  // set previous reset previous instruction
  prev_op = nullptr;

  num_conf_0_branches = 0;
  num_conf_1_branches = 0;
  num_conf_2_branches = 0;
  num_conf_3_branches = 0;

  num_cf_br = 0;
  num_cf_cbr = 0;
  num_cf_call = 0;
  num_cf_ibr = 0;
  num_cf_icall = 0;
  num_cf_ico = 0;
  num_cf_ret = 0;
  num_cf_sys = 0;

  num_BTB_misses = 0;
  num_op_dist_incs = 0;

  off_path_reason = REASON_NOT_IDENTIFIED;
  conf_off_path_reason = REASON_CONF_NOT_IDENTIFIED;
}

/* Conf member functions */
Conf::Conf(uns _proc_id) : proc_id(_proc_id), conf_off_path(false), last_cycle_count(0) {
  conf_info = new Confidence_Info(_proc_id);
  if (CONF_BTB_MISS_BP_TAKEN)
    conf_mech = new BTBMissBPTakenConf(_proc_id);
  else
    conf_mech = new WeightConf(_proc_id);
}

void Conf::recover(Op* op) {
  ASSERT(proc_id, conf_info->off_path_reason != REASON_NOT_IDENTIFIED);
  conf_off_path = false;  
  conf_mech->recover(op);
  conf_info->recover();
}

void Conf::set_prev_op(Op* op) {
  conf_info->prev_op = op;
  DEBUG(proc_id, "Set prev_op off_path:%i, op_num:%llu, cf_type:%i\n", conf_info->prev_op->off_path,
        conf_info->prev_op->op_num, conf_info->prev_op->table_info->cf_type);
}

void Conf::update(Op* op, Flag last_in_ft) {
  if (!CONFIDENCE_ENABLE)
    return;
  Conf_Off_Path_Reason new_reason = REASON_CONF_NOT_IDENTIFIED;
  perfect_conf_update(op, new_reason);
  if (!new_reason || conf_info->off_path_reason == REASON_NOT_IDENTIFIED ||
             conf_info->conf_off_path_reason ==
                 REASON_CONF_NOT_IDENTIFIED) {  // update until both real/confidence path go off
    per_op_update(op, new_reason);
    if (op->table_info->cf_type)
      per_cf_op_update(op, new_reason);
    if (last_in_ft)
      per_ft_update(op, new_reason);
    if (cycle_count > last_cycle_count) {
      last_cycle_count = cycle_count;
      per_cycle_update(op, new_reason);
    }
    conf_off_path = new_reason != REASON_CONF_NOT_IDENTIFIED;
  }
  op->conf_off_path = conf_off_path;
  STAT_EVENT(proc_id, REASON_CONF_NOT_IDENTIFIED + new_reason);
  if (conf_info->off_path_reason == REASON_NOT_IDENTIFIED ||
      conf_info->conf_off_path_reason == REASON_CONF_NOT_IDENTIFIED)
    conf_info->update(op, conf_off_path, new_reason);
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
    conf_info->perfect_off_path = true;
    new_reason = REASON_PERFECT_CONF;
  } else {
    Off_Path_Reason off_path_reason = conf_mech->eval_off_path_reason(op);
    // add perfect to conf_op_reason
    if ((CONF_PERFECT_MISPRED_CONF &&
        (off_path_reason == REASON_MISPRED || off_path_reason == REASON_BTB_MISS_MISPRED)) ||
        (CONF_PERFECT_BTB_MISS_CONF &&
        (off_path_reason == REASON_BTB_MISS || off_path_reason == REASON_BTB_MISS_MISPRED)) ||
        (CONF_PERFECT_IBTB_MISS_CONF && (off_path_reason == REASON_IBTB_MISS)) || 
        (CONF_PERFECT_MISFETCH_CONF && (off_path_reason == REASON_MISFETCH))) {
      conf_info->perfect_off_path = true;
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

