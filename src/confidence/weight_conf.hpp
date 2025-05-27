#ifndef __DEFAULT_CONF_H__
#define __DEFAULT_CONF_H__
#include "decoupled_frontend.h"

#include "confidence/conf.hpp"

class WeightConf;

class WeightConfStat : public ConfMechStatBase {
 public:
  WeightConfStat(uns _proc_id, WeightConf* _conf_mech);

  WeightConf* conf_mech;
};

class WeightConf : public ConfMechBase {
 public:
  WeightConf(uns _proc_id)
      : ConfMechBase(_proc_id), cnt_btb_miss(0), btb_miss_rate(0.0), low_confidence_cnt(0), cf_op_distance(0.0) {
    conf_mech_stat = new WeightConfStat(_proc_id, this);
  }
  // update functions
  void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override;
  void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override;
  void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) override;
  void per_cycle_update(Conf_Off_Path_Reason& new_reason) override;

  void update_state_perfect_conf(Op* op) override;

  // recovery functions
  void recover(Op* op) override;

  // resolve cf
  void resolve_cf(Op* op) override;

 private:
  /* global variables for BTB miss-based BP confidence */
  Counter cnt_btb_miss;
  double btb_miss_rate;

  // confidence counter
  uns low_confidence_cnt;
  double cf_op_distance;
};

#endif  // __CONF_MECH_H__