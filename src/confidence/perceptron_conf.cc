#include "confidence/perceptron_conf.hpp"

#include <string>

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

PerceptronConfStat::PerceptronConfStat(uns _proc_id, PerceptronConf* _conf_mech) : ConfMechStatBase(_proc_id) {
  conf_mech = _conf_mech;
}

void PerceptronConfStat::print_data() {
  ConfMechStatBase::print_data();
  FILE* fp = fopen("perceptron_conf_weights.csv", "w");
  conf_mech->perceptron->print_weights(fp,
                                       "ft_start_addr, \
  ft_length,ft_ended_by_not,ft_ended_by_ic,ft_ended_by_tb, \
  ft_ended_by_bf,ft_ended_by_ae,dfe_cycle,cycles_since_btb_rec, \
  cycles_since_ibtb_rec,cycles_since_misfetch_rec, \
  cycles_since_mispred_rec,btb_miss_rate,ibtb_miss_rate, \
  misfetch_rate,mispred_rate,tage_comp_base,tage_comp_short, \
  tage_comp_long,tage_comp_loop,tage_comp_sc,bp_confidence");
}

void PerceptronConf::per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  update_features(op);
  bool prediction;
  double prediction_val;
  perceptron->predict(conf_features, prediction_val, prediction, op->inst_info->addr, op->oracle_info.pred_global_hist);
  op->perceptron_conf_prediction = prediction_val;
  if (prediction) {
    new_reason = REASON_PERCEPTRON;
  }
}

void PerceptronConf::per_cycle_update(Conf_Off_Path_Reason& new_reason) {
  if (cycle_count % CONF_BTB_MISS_SAMPLE_RATE == 0) {
    btb_miss_rate = (double)cnt_btb_miss / (double)CONF_BTB_MISS_SAMPLE_RATE;
    cnt_btb_miss = 0;
  }
  if (cycle_count % CONF_IBTB_MISS_SAMPLE_RATE == 0) {
    ibtb_miss_rate = (double)cnt_ibtb_miss / (double)CONF_IBTB_MISS_SAMPLE_RATE;
    cnt_ibtb_miss = 0;
  }
  if (cycle_count % CONF_MISFETCH_SAMPLE_RATE == 0) {
    misfetch_rate = (double)cnt_misfetch / (double)CONF_MISFETCH_SAMPLE_RATE;
    cnt_misfetch = 0;
  }
  if (cycle_count % CONF_MISPRED_SAMPLE_RATE == 0) {
    mispred_rate = (double)cnt_mispred / (double)CONF_MISPRED_SAMPLE_RATE;
    cnt_mispred = 0;
  }
}

void PerceptronConf::update_features(Op* op) {
  // change CONF to OP_CONF
  op->conf_features = new double[N_FEATURES];
  if (CONF_PERCEPTRON_START)
    conf_features[0] = (double)op->ft_info.static_info.start;
  if (CONF_PERCEPTRON_LENGTH)
    conf_features[1] = (double)op->ft_info.static_info.length / 32;
  if (CONF_PERCEPTRON_FTEB_NOT)
    conf_features[2] = (double)op->ft_info.dynamic_info.ended_by == 0;
  if (CONF_PERCEPTRON_FTEB_IC)
    conf_features[3] = (double)op->ft_info.dynamic_info.ended_by == 1;
  if (CONF_PERCEPTRON_FTEB_TB)
    conf_features[4] = (double)op->ft_info.dynamic_info.ended_by == 2;
  if (CONF_PERCEPTRON_FTEB_BF)
    conf_features[5] = (double)op->ft_info.dynamic_info.ended_by == 3;
  if (CONF_PERCEPTRON_FTEB_AE)
    conf_features[6] = (double)op->ft_info.dynamic_info.ended_by == 4;
  if (CONF_PERCEPTRON_CYCLE)
    conf_features[7] = cycle_count;
  if (CONF_PERCEPTRON_BTB_REC_CYCLES)
    conf_features[8] = cycle_count - last_btb_recover_cycle;
  if (CONF_PERCEPTRON_IBTB_REC_CYCLES)
    conf_features[9] = cycle_count - last_ibtb_recover_cycle;
  if (CONF_PERCEPTRON_MISFETCH_REC_CYCLES)
    conf_features[10] = cycle_count - last_misfetch_recover_cycle;
  if (CONF_PERCEPTRON_MISPRED_REC_CYCLES)
    conf_features[11] = cycle_count - last_mispred_recover_cycle;
  if (CONF_PERCEPTRON_BTB_RATE)
    conf_features[12] = btb_miss_rate;
  if (CONF_PERCEPTRON_IBTB_RATE)
    conf_features[13] = ibtb_miss_rate;
  if (CONF_PERCEPTRON_MISFETCH_RATE)
    conf_features[14] = misfetch_rate;
  if (CONF_PERCEPTRON_MISPRED_RATE)
    conf_features[15] = mispred_rate;
  if (CONF_PERCEPTRON_TAGE_BASE)
    conf_features[16] = (double)(op->tage_comp == 0);
  // DEBUG(proc_id, "tage_comp_base: %f\n", conf_features[16]);
  if (CONF_PERCEPTRON_TAGE_SHORT)
    conf_features[17] = (double)(op->tage_comp == 1);
  // DEBUG(proc_id, "tage_comp_short: %f\n", conf_features[17]);
  if (CONF_PERCEPTRON_TAGE_LONG)
    conf_features[18] = (double)(op->tage_comp == 2);
  // DEBUG(proc_id, "tage_comp_long: %f\n", conf_features[18]);
  if (CONF_PERCEPTRON_TAGE_LOOP)
    conf_features[19] = (double)(op->tage_comp == 3);
  // DEBUG(proc_id, "tage_comp_loop: %f\n", conf_features[19]);
  if (CONF_PERCEPTRON_TAGE_SC)
    conf_features[20] = (double)(op->tage_comp == 4);
  // DEBUG(proc_id, "tage_comp_sc: %f\n", conf_features[20]);
  if (CONF_PERCEPTRON_BP_CONF)
    conf_features[21] = (double)op->bp_confidence;

  for (int i = 0; i < (int)conf_features.size(); i++) {
    op->conf_features[i] = conf_features[i];
  }
}

void PerceptronConf::recover(Op* op, std::deque<FT>& ftq) {
  // walk ftq to train on off-path ops
  for (auto it = ftq.begin(); it != ftq.end(); it++) {
    for (auto _op : it->get_ops()) {
      if (_op->table_info->cf_type) {
        resolve_cf(_op);
      }
    }
  }

  switch (op->oracle_info.off_path_reason) {
    case REASON_BTB_MISS:
      cnt_btb_miss++;
      last_btb_recover_cycle = cycle_count;
      break;
    case REASON_BTB_MISS_MISPRED:
      cnt_btb_miss++;
      cnt_mispred++;
      last_btb_recover_cycle = cycle_count;
      last_mispred_recover_cycle = cycle_count;
      break;
    case REASON_MISPRED:
      cnt_mispred++;
      last_mispred_recover_cycle = cycle_count;
      break;
    case REASON_MISFETCH:
      cnt_misfetch++;
      last_misfetch_recover_cycle = cycle_count;
      break;
  }
}

void PerceptronConf::resolve_cf(Op* op) {
  perceptron->train(conf_features, op->conf_off_path, op->off_path, op->perceptron_conf_prediction, op->inst_info->addr,
                    op->oracle_info.pred_global_hist);
}