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

// Confidence Mechanism interface
class ConfMechBase {
 public:
  ConfMechBase(uns _proc_id) : proc_id(_proc_id) {}
  // update functions
  virtual void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;
  virtual void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;
  virtual void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;
  virtual void per_cycle_update(Op* op, Conf_Off_Path_Reason& new_reason) = 0;

  virtual void update_state_perfect_conf(Op* op) = 0;

  // recovery functions
  virtual void recover(Op* op) = 0;

  // resolve cf
  virtual void resolve_cf(Op* op) = 0;

  uns proc_id;
};

// metadata for confidence
class Confidence_Info {
 public:
  Confidence_Info(uns _proc_id)
      : proc_id(_proc_id),
        prev_op(nullptr),
        off_path_reason(REASON_NOT_IDENTIFIED),
        conf_off_path_reason(REASON_CONF_NOT_IDENTIFIED),
        num_conf_0_branches(0),
        num_conf_1_branches(0),
        num_conf_2_branches(0),
        num_conf_3_branches(0),
        num_cf_br(0),
        num_cf_cbr(0),
        num_cf_call(0),
        num_cf_ibr(0),
        num_cf_icall(0),
        num_cf_ico(0),
        num_cf_ret(0),
        num_cf_sys(0),
        num_BTB_misses(0),
        num_op_dist_incs(0) {}
  void update(Op* op, Flag conf_off_path, Conf_Off_Path_Reason new_reson);
  void recover();

 private:
  void inc_br_conf_counters(int conf);
  void inc_cf_type_counters(Cf_Type cf_type);
  uns proc_id;
  Op* prev_op;

  Off_Path_Reason off_path_reason;
  Conf_Off_Path_Reason conf_off_path_reason;

  Counter num_conf_0_branches;
  Counter num_conf_1_branches;
  Counter num_conf_2_branches;
  Counter num_conf_3_branches;

  Counter num_cf_br;
  Counter num_cf_cbr;
  Counter num_cf_call;
  Counter num_cf_ibr;
  Counter num_cf_icall;
  Counter num_cf_ico;
  Counter num_cf_ret;
  Counter num_cf_sys;

  Counter num_BTB_misses;
  Counter num_op_dist_incs;
  friend class Conf;
};

class Conf {
 public:
  Conf(uns _proc_id);
  uns get_conf() { return conf_off_path; }
  void recover(Op* op);
  void set_prev_op(Op* op);
  void update(Op* op, Flag last_in_ft);
  void resolve_cf(Op* op) { conf_mech->resolve_cf(op); }
  Off_Path_Reason get_off_path_reason() { return conf_info->off_path_reason; }
  Conf_Off_Path_Reason get_conf_off_path_reason() { return conf_info->conf_off_path_reason; }

 private:
  void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason);
  void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason);
  void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason);
  void per_cycle_update(Op* op, Conf_Off_Path_Reason& new_reason);
  void update_state_perfect_conf(Op* op) { conf_mech->update_state_perfect_conf(op); }
  // confidence mech object
  ConfMechBase* conf_mech;

  uns proc_id;

  // confidence counter
  bool conf_off_path;
  Counter last_cycle_count;
  Confidence_Info* conf_info;
};

#endif
