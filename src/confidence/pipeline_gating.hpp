/* Copyright 2025 Litz Lab
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
 * File         : pipeline_gating.hpp
 * Author       : Naomi Rehman <narehman@ucsc.edu>
 * Date         : 10/31/2024
 * Description  : Original Paper: S. Manne, A. Klauser and D. Grunwald, "Pipeline gating: speculation control for energy
 *reduction," Proceedings. 25th Annual International Symposium on Computer Architecture (Cat. No.98CB36235), Barcelona,
 *Spain, 1998, pp. 132-141, doi: 10.1109/ISCA.1998.694769.
 ***************************************************************************************/
#ifndef __PIPELINE_GATING_H__
#define __PIPELINE_GATING_H__

#include <map>
#include <tuple>
#include <vector>

#include "decoupled_frontend.h"

#include "confidence/conf.hpp"

class PipelineGatingConf;

class PipelineGatingConfStat : public ConfMechStatBase {
 public:
  PipelineGatingConfStat(uns _proc_id, PipelineGatingConf* _conf_mech);

  PipelineGatingConf* conf_mech;
};

class PipelineGatingConf : public ConfMechBase {
 public:
  PipelineGatingConf(uns _proc_id) : ConfMechBase(_proc_id), cnt_low_conf_brs(0) {
    conf_mech_stat = new PipelineGatingConfStat(_proc_id, this);
  }
  // update functions
  void per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override {};
  void per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) override;
  void per_cycle_update(Conf_Off_Path_Reason& new_reason) override {};
  void per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) override {};
  bool go_back_on_path(Op* op) override;

  void update_state_perfect_conf(Op* op) override {};

  // recovery functions
  void recover(Op* op, std::deque<FT>& ftq) override;

  // resolve cf
  void resolve_cf(Op* op) override;

 private:
  uns cnt_low_conf_brs;  // count of low confidence branch instructions in the pipeline

  friend PipelineGatingConfStat;
};

#endif  // __PIPELINE_GATING_H__