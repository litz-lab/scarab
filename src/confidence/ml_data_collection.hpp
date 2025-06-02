#ifndef __ML_DATA_COLLECTION_HPP__
#define __ML_DATA_COLLECTION_HPP__
#include <tuple>
#include <vector>

#include "decoupled_frontend.h"

#include "confidence/conf.hpp"

class MLDataCollection;

// ft address
// ft length (bytes)
// ft ended by
// cycle seen by dfe
// cycle seen by icache stage (popped from ftq)
// cycles since last btb miss
// cycles since last ibtb miss
// cycles since last mispred
// cycles since last misfetch
// btb miss rate
// ibtb miss rate
// mispred rate
// misfetch rate
// cf_mask (mask of cf ops detected by BTB)
// tage_comp_base
// tage_comp_short
// tage_comp_long
// tage_comp_loop
// tage_comp_sc
// off_path_reason
// off_path
typedef std::tuple<Addr, Addr, FT_Ended_By, Counter, Counter, Counter, Counter, Counter, Counter, double, double,
                   double, double, int, bool, bool, bool, bool, bool, int, int>
    ml_data_entry;

class MLDataCollectionStat : public ConfMechStatBase {
 public:
  MLDataCollectionStat(uns _proc_id, MLDataCollection* _conf_mech);
  void update(Op* op, Conf_Off_Path_Reason reason, bool last_in_ft, FT& pushed_ft) override;
  void ft_consumed_update(FT_Info& ft_info, std::vector<Op*>& ops) override { log_ft_data(ft_info, ops); }
  void print_data() override;

  MLDataCollection* conf_mech;
  void set_ft_info(FT_Info& ft_info);
  void log_ft_data(FT_Info& ft_info, std::vector<Op*>& ops);
  std::vector<ml_data_entry> ml_data;
};

class MLDataCollection : public ConfMechBase {
 public:
  MLDataCollection(uns _proc_id)
      : ConfMechBase(_proc_id),
        cnt_btb_miss(0),
        btb_miss_rate(0.0),
        last_btb_recover_cycle(0),
        cnt_ibtb_miss(0),
        ibtb_miss_rate(0.0),
        last_ibtb_recover_cycle(0),
        cnt_misfetch(0),
        misfetch_rate(0.0),
        last_misfetch_recover_cycle(0),
        cnt_mispred(0),
        mispred_rate(0.0),
        last_mispred_recover_cycle(0) {
    conf_mech_stat = new MLDataCollectionStat(_proc_id, this);
  }
  // update functions
  void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override {};
  void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override {};
  void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) override {};
  void per_cycle_update(Conf_Off_Path_Reason& new_reason) override;

  void update_state_perfect_conf(Op* op) override {}

  // recovery functions
  void recover(Op* op, std::deque<FT>& ftq) override;

  // resolve cf
  void resolve_cf(Op* op) override {};

 private:
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

  friend MLDataCollectionStat;
};

#endif  // __ML_DATA_COLLECTION_HPP__