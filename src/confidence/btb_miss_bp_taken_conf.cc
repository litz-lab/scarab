#include "confidence/btb_miss_bp_taken_conf.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CONF, ##args)

BTBMissBPTakenConfStat::BTBMissBPTakenConfStat(uns _proc_id, BTBMissBPTakenConf* _conf_mech)
    : ConfMechStatBase(_proc_id) {
  conf_mech = _conf_mech;
}

void BTBMissBPTakenConf::per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  if (!CONFIDENCE_ENABLE)
    return;

  DEBUG(proc_id, "btb miss rate: %f, cycles since recovery: %llu\n", btb_miss_rate, cycle_count - last_recover_cycle);
  if ((double)((cycle_count - last_recover_cycle) * btb_miss_rate) >= CONF_BTB_MISS_RATE_CYCLES_THRESHOLD) {
    low_confidence_cnt = ~0U;
    STAT_EVENT(proc_id, CONF_BTB_NUM_CYCLES_OFF_PATH_EVENT);
    new_reason = REASON_BTB_MISS_RATE;
  }
}

void BTBMissBPTakenConf::per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  if (op->oracle_info.btb_miss && (op->oracle_info.pred_orig == TAKEN) &&
      (op->bp_confidence >= CONF_BTB_MISS_BP_TAKEN_THRESHOLD)) {
    low_confidence_cnt = ~0U;
    ASSERT(proc_id, op->bp_confidence >= 0 && op->bp_confidence <= 3);
    new_reason = static_cast<Conf_Off_Path_Reason>(REASON_BTB_MISS_BP_TAKEN_CONF_0 + op->bp_confidence);
  } else {  // update confidence
    low_confidence_cnt += 3 - op->bp_confidence;
    if (low_confidence_cnt >= CONF_OFF_PATH_THRESHOLD)
      new_reason = REASON_CONF_THRESHOLD;
  }
}

void BTBMissBPTakenConf::per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  return;
}

void BTBMissBPTakenConf::per_cycle_update(Conf_Off_Path_Reason& new_reason) {
  if (cycle_count % CONF_BTB_MISS_SAMPLE_RATE == 0) {
    btb_miss_rate = (double)cnt_btb_miss / (double)CONF_BTB_MISS_SAMPLE_RATE;
    cnt_btb_miss = 0;
  }
  if ((double)((cycle_count - last_recover_cycle) * btb_miss_rate) >= CONF_BTB_MISS_RATE_CYCLES_THRESHOLD) {
    low_confidence_cnt = ~0U;
    STAT_EVENT(proc_id, CONF_BTB_NUM_CYCLES_OFF_PATH_EVENT);
    new_reason = REASON_BTB_MISS_RATE;
  }
}

void BTBMissBPTakenConf::update_state_perfect_conf(Op* op) {
  return;
}

void BTBMissBPTakenConf::recover(Op* op) {
  if (op->oracle_info.off_path_reason == REASON_BTB_MISS)
    cnt_btb_miss++;
  low_confidence_cnt = 0;
  last_recover_cycle = cycle_count;
}

void BTBMissBPTakenConf::resolve_cf(Op* op) {
  return;
}