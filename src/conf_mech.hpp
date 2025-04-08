#ifndef __CONF_MECH_H__
#define __CONF_MECH_H__
#include "decoupled_frontend.h"

class ConfMech {
 public:
  ConfMech(uns _proc_id) : proc_id(_proc_id) {}
  // update functions
  virtual void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;
  virtual void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;
  virtual void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;
  virtual void per_cycle_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;

  virtual void update_state_perfect_conf(Op* op) = 0;

  // recovery functions
  virtual void recover() = 0;

  // resolve cf
  virtual void resolve_cf(Op* op) = 0;

  uns proc_id;

 private:
};
#endif  // __CONF_MECH_H__