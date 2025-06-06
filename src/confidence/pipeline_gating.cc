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
 * File         : pipeline_gating.cc
 * Author       : Naomi Rehman <narehman@ucsc.edu>
 * Date         : 10/31/2024
 * Description  : Original Paper: S. Manne, A. Klauser and D. Grunwald, "Pipeline gating: speculation control for energy
 reduction," Proceedings. 25th Annual International Symposium on Computer Architecture (Cat. No.98CB36235), Barcelona,
 Spain, 1998, pp. 132-141, doi: 10.1109/ISCA.1998.694769.


 ***************************************************************************************/
#include "confidence/pipeline_gating.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CONF, ##args)

PipelineGatingConfStat::PipelineGatingConfStat(uns _proc_id, PipelineGatingConf* _conf_mech)
    : ConfMechStatBase(_proc_id) {
  conf_mech = _conf_mech;
}

void PipelineGatingConf::recover(Op* op, std::deque<FT>& ftq) {
  cnt_low_conf_brs = 0;
}

void PipelineGatingConf::per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  if (((op)->table_info->cf_type == CF_CBR || (op)->table_info->cf_type == CF_IBR ||
       (op)->table_info->cf_type == CF_ICALL) &&
      (op->bp_confidence < 2 /*0 and 1 low confidence*/)) {
    cnt_low_conf_brs++;
    DEBUG(proc_id, "Low confidence branch detected: op_num %llu, bp_confidence %u\n", op->op_num, op->bp_confidence);
  }
  if (cnt_low_conf_brs >= CONF_PIPELINE_GATING_THRESHOLD) {
    new_reason = REASON_PIPELINE_GATING;
    DEBUG(proc_id, "Pipeline gating triggered due to low confidence branches: %u\n", cnt_low_conf_brs);
  }
}

void PipelineGatingConf::resolve_cf(Op* op) {
  // No specific resolution needed for pipeline gating
  DEBUG(proc_id, "Resolving cf for op %llu in Pipeline Gating Conf\n", op->op_num);
  if ((op)->table_info->cf_type == CF_CBR || (op)->table_info->cf_type == CF_IBR ||
      (op)->table_info->cf_type == CF_ICALL) {
    if (op->bp_confidence < 2 /*0 and 1 low confidence*/) {
      cnt_low_conf_brs--;
      DEBUG(proc_id, "Decrementing low confidence branch count: %u\n", cnt_low_conf_brs);
    }
  }
  ASSERT(proc_id, cnt_low_conf_brs >= 0);
}

bool PipelineGatingConf::go_back_on_path(Op* op) {
  return cnt_low_conf_brs < CONF_PIPELINE_GATING_THRESHOLD;
}