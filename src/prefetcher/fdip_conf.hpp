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
 * File         : fdip_conf.hpp
 * Author       : Surim Oh <soh31@ucsc.edu> and Naomi Rehman <narehman@ucsc.edu>
 * Date         : 10/31/2024
 * Description  :
 ***************************************************************************************/

#ifndef __FDIP_CONF_H__
#define __FDIP_CONF_H__

#include "prefetcher/fdip.h"

typedef enum OFF_PATH_REASON_enum {
  REASON_BTB_MISS,
  REASON_MISPRED,
  REASON_MISFETCH,
  REASON_NO_TARGET,
} Off_Path_Reason;

typedef enum CONF_OFF_PATH_REASON_enum {
  REASON_BTB_MISS_BP_TAKEN_CONF_0,
  REASON_BTB_MISS_BP_TAKEN_CONF_1,
  REASON_BTB_MISS_BP_TAKEN_CONF_2,
  REASON_BTB_MISS_BP_TAKEN_CONF_3,
  REASON_BTB_MISS_RATE,
  REASON_INV_CONF_INC,
} Conf_Off_Path_Reason;

// metadata for fdip confidence
class FDIP_Confidence_Info {
 public:
  FDIP_Confidence_Info(uns _proc_id)
      : proc_id(_proc_id),
        prev_op(nullptr),
        fdip_off_path_event(FALSE),
        fdip_on_conf_off_event(FALSE),
        fdip_off_conf_on_event(FALSE),
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
  void recover();
  void log_stats_bp_conf_on();
  void log_stats_bp_conf_off();

 private:
  uns proc_id;
  Op* prev_op;

  // probably only need one of these lads
  Flag fdip_off_path_event;
  Flag fdip_on_conf_off_event;
  Flag fdip_off_conf_on_event;

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
  friend class FDIP_Conf;
};

class FDIP_Conf {
 public:
  FDIP_Conf(uns _proc_id)
      : proc_id(_proc_id), cnt_btb_miss(0), btb_miss_rate(0.0), low_confidence_cnt(0), cf_op_distance(0.0) {
    conf_info = new FDIP_Confidence_Info(_proc_id);
  }
  uns get_low_confidence_cnt() { return low_confidence_cnt; }
  void recover();
  void cyc_reset();
  void set_prev_op(Op* op, Flag off_path);
  Flag get_off_path_event() { return conf_info->fdip_off_path_event; }
  void update(Op* op);
  void log_stats_bp_conf();
  void log_stats_bp_conf_emitted();
  void inc_cnt_btb_miss() { cnt_btb_miss++; }

 private:
  void default_conf_update(Op* op);
  void btb_miss_bp_taken_conf_update(Op* op);
  void inc_br_conf_counters(int conf);
  void inc_cf_type_counters(Cf_Type cf_type);

  uns proc_id;
  /* global variables for BTB miss-based BP confidence */
  Counter cnt_btb_miss;
  double btb_miss_rate;

  // confidence counter
  uns low_confidence_cnt;
  double cf_op_distance;

  FDIP_Confidence_Info* conf_info;
};

#endif
