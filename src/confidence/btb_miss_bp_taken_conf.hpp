#ifndef __BTB_MISS_BP_TAKEN_H__
#define __BTB_MISS_BP_TAKEN_H__
#include "decoupled_frontend.h"

#include "confidence/conf.hpp"

class BTBMissBPTakenConf;

class BTBMissBPTakenConfStat : public ConfMechStatBase {
 public:
  BTBMissBPTakenConfStat(uns _proc_id, BTBMissBPTakenConf* _conf_mech);

  BTBMissBPTakenConf* conf_mech;
};

class BTBMissBPTakenConf : public ConfMechBase {
 public:
  BTBMissBPTakenConf(uns _proc_id)
      : ConfMechBase(_proc_id), cnt_btb_miss(0), btb_miss_rate(0.0), low_confidence_cnt(0), last_recover_cycle(0) {
    conf_mech_stat = new BTBMissBPTakenConfStat(_proc_id, this);
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
  Counter last_recover_cycle;

  friend BTBMissBPTakenConfStat;
};

#endif  // __BTB_MISS_BP_TAKEN_H__