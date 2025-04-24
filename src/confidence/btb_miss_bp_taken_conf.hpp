#ifndef __BTB_MISS_BP_TAKEN_H__
#define __BTB_MISS_BP_TAKEN_H__
#include <map>
#include <tuple>
#include <vector>

#include "decoupled_frontend.h"

#include "confidence/conf.hpp"

class BTBMissBPTakenConf : public ConfMechBase {
 public:
  BTBMissBPTakenConf(uns _proc_id)
      : ConfMechBase(_proc_id), cnt_btb_miss(0), btb_miss_rate(0.0), last_recover_cycle(0), low_confidence_cnt(0) {}
  // update functions
  void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override;
  void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override;
  void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) override;
  void per_cycle_update(Op* op, Conf_Off_Path_Reason& new_reason) override;

  void update_state_perfect_conf(Op* op) override;

  // recovery functions
  void recover(Op*op) override;

  // resolve cf
  void resolve_cf(Op* op) override;

  void print_data() override;

 private:
  void reset_counters();

  Conf_Off_Path_Reason update_resteer_rate_ctrs(Conf_Off_Path_Reason conf_op_reason);


  void log_phase_cycles(Op* op);
  void log_off_path_event(Op* op);
  void log_resolution(Op* op);

  /* miss-rate based confidence */
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

  Counter cnt_on_path_instructions;
  double effective_ipc;

  Counter cnt_total_ops;
  Counter last_recover_cycle;

  // confidence counter
  uns low_confidence_cnt;

  // for csvs
  typedef std::tuple<Counter, Counter, Counter, double> phase_cycles_line;
  std::vector<phase_cycles_line> btb_miss_event_cycles;
  std::vector<phase_cycles_line> ibtb_miss_event_cycles;
  std::vector<phase_cycles_line> mispred_event_cycles;
  std::vector<phase_cycles_line> misfetch_event_cycles;

  std::map<Counter, std::tuple<Counter, Counter, Off_Path_Reason>> resteer_ops_cycles;
  std::map<Counter, std::tuple<Counter, Counter, Off_Path_Reason>> resteer_ops_ops;

};

#endif  // __BTB_MISS_BP_TAKEN_H__