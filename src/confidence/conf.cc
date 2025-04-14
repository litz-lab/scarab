#include "prefetcher/pref.param.h"

// Implementations of the API
#include "confidence/btb_miss_bp_taken_conf.hpp"
#include "confidence/conf.hpp"
#include "confidence/weight_conf.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

Conf::Conf(uns _proc_id) : proc_id(_proc_id), conf_off_path(false), last_cycle_count(0) {
  conf_info = new Confidence_Info(_proc_id);
  if (CONF_BTB_MISS_BP_TAKEN)
    conf_mech = new BTBMissBPTakenConf(_proc_id);
  else
    conf_mech = new WeightConf(_proc_id);
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
  if (dfe_off_path && !prev_op->off_path && off_path_reason == REASON_NOT_IDENTIFIED) {  // the actual path goes off
    DEBUG(proc_id, "prev_op op_num: %llu, cf_type: %i, cur_op op_num: %llu, cf_type: %i\n", prev_op->op_num,
          prev_op->table_info->cf_type, decoupled_fe_get_cur_op()->op_num,
          decoupled_fe_get_cur_op()->table_info->cf_type);
    ASSERT(proc_id, prev_op->table_info->cf_type);  // must be a cf as the last on-path op
    if (prev_op->oracle_info.mispred)               // check misprediction first
      off_path_reason = REASON_MISPRED;
    else if (prev_op->oracle_info.btb_miss)  // off path due to a btb miss
      off_path_reason = REASON_BTB_MISS;
    else if (prev_op->oracle_info.no_target)  // off path due to no target
      off_path_reason = REASON_NO_TARGET;
    else if (prev_op->oracle_info.misfetch)  // off path due to misfetch
      off_path_reason = REASON_MISFETCH;
    else {  // if some other reason (shouldn't happen)
      DEBUG(proc_id, "dfe off conf on event, unrecognized off path reason: op type: %u\n",
            prev_op->table_info->op_type);
      // ASSERT(proc_id, false); // Disable for now
    }

    if (!conf_off_path) {
      STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NUM_EVENTS);
      if (off_path_reason == REASON_MISPRED) {
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_BP_INCORRECT);
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_BP_INCORRECT_0_CONF + prev_op->bp_confidence);
      } else if (off_path_reason == REASON_BTB_MISS) {
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_BTB_MISS);
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_BTB_MISS_NOT_CF + prev_op->table_info->cf_type);
      } else if (off_path_reason == REASON_NO_TARGET)
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_NO_TARGET);
      else if (off_path_reason == REASON_MISFETCH)
        STAT_EVENT(proc_id, DFE_OFF_CONF_ON_MISFETCH);
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
void Conf::recover() {
  ASSERT(proc_id, conf_info->off_path_reason != REASON_NOT_IDENTIFIED);
  conf_off_path = false;
  conf_mech->recover();
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
  if (PERFECT_CONFIDENCE) {
    if (decoupled_fe_is_off_path())
      conf_off_path = true;
    if (conf_off_path)
      ASSERT(proc_id, decoupled_fe_is_off_path());
    update_state_perfect_conf(op);
  } else if (conf_info->off_path_reason == REASON_NOT_IDENTIFIED ||
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
  if (conf_info->off_path_reason == REASON_NOT_IDENTIFIED ||
      conf_info->conf_off_path_reason == REASON_CONF_NOT_IDENTIFIED)
    conf_info->update(op, conf_off_path, new_reason);
  set_prev_op(op);
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