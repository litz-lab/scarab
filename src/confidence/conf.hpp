/* Copyright 2024 Litz Lab
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : conf.hpp
 * Author       : Surim Oh <soh31@ucsc.edu> and Naomi Rehman <narehman@ucsc.edu>
 * Date         : 10/31/2024
 * Description  :
 ***************************************************************************************/

#ifndef __CONF_H__
#define __CONF_H__

#include "decoupled_frontend.h"
#include "ft.h"

typedef enum Confidence_Mechanism_enum {
  CONF_MECH_WEIGHT,
  CONF_MECH_BTB_MISS_BP_TAKEN,
} Confidence_Mechanism;

class ConfMechBase;  // forward declaration

class ConfMechStatBase {
 public:
  ConfMechStatBase(uns _proc_id)
      : proc_id(_proc_id),
        prev_op(nullptr),
        off_path_reason(REASON_NOT_IDENTIFIED),
        conf_off_path_reason(REASON_CONF_NOT_IDENTIFIED),
        perfect_off_path(false) {}
  virtual void update(Op* op, Conf_Off_Path_Reason reason, bool last_in_ft);
  virtual void per_cycle_update(Conf_Off_Path_Reason reason);
  virtual void recover(Op* op);
  virtual void print_data();
  void set_prev_op(Op* op);

  Off_Path_Reason get_off_path_reason() { return off_path_reason; }
  Conf_Off_Path_Reason get_conf_off_path_reason() { return conf_off_path_reason; }

  // pointer to outer class, use this to access mech-specific data
  ConfMechBase* conf_mech;

  uns proc_id;
  Op* prev_op;

  Off_Path_Reason off_path_reason;
  Conf_Off_Path_Reason conf_off_path_reason;

  bool perfect_off_path;
};

// Confidence Mechanism interface
class ConfMechBase {
 public:
  ConfMechBase(uns _proc_id) : proc_id(_proc_id) {}
  // update functions
  virtual void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;
  virtual void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;
  virtual void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;
  virtual void per_cycle_update(Conf_Off_Path_Reason& new_reason) = 0;

  virtual void update_state_perfect_conf(Op* op) = 0;

  // recovery functions
  virtual void recover(Op* op) = 0;

  // resolve cf
  virtual void resolve_cf(Op* op) = 0;

  uns proc_id;

  ConfMechStatBase* conf_mech_stat;

  friend ConfMechStatBase;
};

class Conf {
 public:
  Conf(uns _proc_id);
  uns get_conf() { return conf_off_path; }
  void recover(Op* op);
  void set_prev_op(Op* op);
  void update(FT ft_pushed);
  void resolve_cf(Op* op) { conf_mech->resolve_cf(op); }
  Off_Path_Reason get_off_path_reason() { return conf_mech->conf_mech_stat->get_off_path_reason(); }
  Conf_Off_Path_Reason get_conf_off_path_reason() { return conf_mech->conf_mech_stat->get_conf_off_path_reason(); }
  void print_data() { conf_mech->conf_mech_stat->print_data(); }
  // called every cycle, even if DFE is stalled
  void per_cycle_update();

 private:
  void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason);
  void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason);
  void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason);
  void update_state_perfect_conf(Op* op) { conf_mech->update_state_perfect_conf(op); }
  void perfect_conf_update(Op* op, Conf_Off_Path_Reason& new_reason);
  void process_op(Op* op, Conf_Off_Path_Reason& new_reason, bool last_in_ft);

  // confidence mech object
  ConfMechBase* conf_mech;

  uns proc_id;

  // confidence counter
  bool conf_off_path;
  Counter last_cycle_count;
};

#endif
