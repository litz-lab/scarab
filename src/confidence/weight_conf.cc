#include "confidence/weight_conf.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CONF, ##args)
WeightConfStat::WeightConfStat(uns _proc_id, WeightConf* _conf_mech) : ConfMechStatBase(_proc_id) {
  conf_mech = _conf_mech;
}

void WeightConf::per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  if (!CONF_BTB_MISS_RATE_CONF && !(op->table_info->cf_type)) {
    if (cf_op_distance >= CONF_OFF_PATH_THRESHOLD) {
      low_confidence_cnt += CONF_OFF_PATH_INC + (double)CONF_BTB_MISS_RATE_WEIGHT * btb_miss_rate;
      cf_op_distance = 0.0;
    } else {
      cf_op_distance += (1.0 + (double)CONF_BTB_MISS_RATE_WEIGHT * btb_miss_rate);
    }
  }

  if (low_confidence_cnt >= CONF_OFF_PATH_THRESHOLD)
    new_reason = REASON_CONF_THRESHOLD;
}

void WeightConf::per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  if (!CONF_PERFECT_BTB_MISS_CONF && !CONF_PERFECT_MISPRED_CONF) {
    low_confidence_cnt +=
        3 - op->bp_confidence + (double)CONF_BTB_MISS_RATE_WEIGHT * btb_miss_rate;  // 3 is highest bp_confidence
    cf_op_distance = 0.0;
  }

  if (low_confidence_cnt >= CONF_OFF_PATH_THRESHOLD)
    new_reason = REASON_CONF_THRESHOLD;
}

void WeightConf::per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  return;
}

void WeightConf::per_cycle_update(Conf_Off_Path_Reason& new_reason) {
  if (cycle_count % CONF_BTB_MISS_SAMPLE_RATE == 0) {
    btb_miss_rate = (double)cnt_btb_miss / (double)CONF_BTB_MISS_SAMPLE_RATE;
    cnt_btb_miss = 0;
  }
}

void WeightConf::update_state_perfect_conf(Op* op) {
  cf_op_distance = 0.0;
}

void WeightConf::recover(Op* op) {
  if (op->oracle_info.off_path_reason == REASON_BTB_MISS)
    cnt_btb_miss++;
  low_confidence_cnt = 0;
  cf_op_distance = 0.0;
}

void WeightConf::resolve_cf(Op* op) {
  return;
}