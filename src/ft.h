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
 * File         : ft.h
 * Author       : Mingsheng Xu, Yuanpeng Liao
 * Date         :
 * Description  : Fetch Target (FT) class header
 ***************************************************************************************/

#ifndef __FT_H__
#define __FT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ft_info.h"
#include "op.h"

// Forward declare FT as an opaque struct for C
typedef struct FT FT;

// C-compatible API
bool ft_can_fetch_op(FT* ft);
Op* ft_fetch_op(FT* ft);
bool ft_is_consumed(FT* ft);
void ft_set_consumed(FT* ft);
FT_Info ft_get_ft_info(FT* ft);
FT_Ended_By ft_get_ended_by(Op* op, bool use_pred);

#ifdef __cplusplus
}  // extern "C"
#endif

#ifdef __cplusplus

// C++-only includes
#include <functional>
#include <vector>

#include "globals/global_defs.h"
#include "globals/global_types.h"

#include "decoupled_frontend.h"

// C++ class definition
enum FT_Event {
  FT_EVENT_NONE,
  FT_EVENT_MISPREDICT,
  FT_EVENT_FETCH_BARRIER,
  FT_EVENT_OFFPATH_TAKEN_REDIRECT,
  // ... add more as needed
};

struct FT_PredictResult {
  int index;
  FT_Event event;
  Op* op;          // Optionally, if DFE needs to know which op
  Addr pred_addr;  // Optionally, if DFE needs the predicted address
};

// Add a struct to hold build result info
struct FT_BuildResult {
  bool build_complete = false;
  bool redirect_needed = false;
  Op* redirect_op = nullptr;
  uns64 redirect_uid = 0;
  Addr redirect_addr = 0;
};

class FT {
 public:
  FT(uns _proc_id = 0);
  void set_ft_started_by(FT_Started_By ft_started_by);
  void add_op(Op* op, FT_Ended_By ft_ended_by);
  void free_ops_and_clear();
  bool can_fetch_op();
  Op* fetch_op();
  void set_per_op_ft_info();
  FT_Info get_ft_info();
  bool is_consumed();
  void set_consumed();

  std::vector<Op*>& get_ops();
  // Change return type to FT_BuildResult
  FT_BuildResult build_full_ft(uns start_index, std::function<bool(uns8)> can_fetch_op_fn,
                               std::function<bool(uns8, Op*)> fetch_op_fn, FT last_ft, Flag off_path, Flag use_pred,
                               uint64_t& dfe_op_count);
  Op* peek_last_op();

  FT_PredictResult bp_predict_ft(uint64_t& dfe_op_count, uns start_pos);
  std::pair<FT, FT> split_ft(uns index);

  int count_cfs_taken_this_ft() const;
  bool is_valid() const;
  bool is_ended() const;

 private:
  uns proc_id;
  uint64_t op_pos;
  FT_Info ft_info;
  std::vector<Op*> ops;
  bool consumed;
  FT_Event predict_one_cf_op(Op* op, uint64_t& dfe_op_count);
  FT move_over_ft(uns start_idx, uns end_idx, Flag use_pred);

  friend class Decoupled_FE;
};

#endif  // __cplusplus

#endif  // __FT_H__
