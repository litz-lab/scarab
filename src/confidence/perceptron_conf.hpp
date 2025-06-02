#ifndef __PERCEPTRON_CONF_H__
#define __PERCEPTRON_CONF_H__
#include "decoupled_frontend.h"

#include "confidence/conf.hpp"
#include "libs/perceptron.hpp"

const unsigned int PERCEPTRON_N_FEATURES = 22;

class PerceptronConf;

class PerceptronConfStat : public ConfMechStatBase {
 public:
  PerceptronConfStat(uns _proc_id, PerceptronConf* _conf_mech);
  void print_data() override;

  PerceptronConf* conf_mech;
};

class PerceptronConf : public ConfMechBase {
 public:
  PerceptronConf(uns _proc_id) : ConfMechBase(_proc_id) {
    conf_features.resize(PERCEPTRON_N_FEATURES);
    conf_mech_stat = new PerceptronConfStat(_proc_id, this);
    perceptron = new PerceptronTable(PERCEPTRON_N_FEATURES, PATH_CONF_N_PERCEPTRONS, PATH_CONF_PERCEPTRON_LR,
                                     CONF_PERCEPTRON_THETA, CONF_PERCEPTRON_WEIGHT_WIDTH, CONF_PERCEPTRON_THRESHOLD);
  }
  // update functions
  void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override {}
  void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override;
  void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) override {}
  void per_cycle_update(Conf_Off_Path_Reason& new_reason) override;

  void update_state_perfect_conf(Op* op) override {}

  // recovery functions
  void recover(Op* op, std::deque<FT>& ftq) override;

  // resolve cf
  void resolve_cf(Op* op) override;

 private:
  void update_features(Op* op);

  Counter cnt_btb_miss;
  double btb_miss_rate;
  Counter last_btb_recover_cycle;

  Counter cnt_ibtb_miss;
  double ibtb_miss_rate;
  Counter last_ibtb_recover_cycle;

  Counter cnt_misfetch;
  double misfetch_rate;
  Counter last_misfetch_recover_cycle;

  Counter cnt_mispred;
  double mispred_rate;
  Counter last_mispred_recover_cycle;

  PerceptronTable<>* perceptron;
  std::vector<double> conf_features;

  friend PerceptronConfStat;
};

#endif  // __PERCEPTRON_CONF_H__