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
FT_Info ft_get_ft_info(FT* ft);
void ft_free_op(Op* op);

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
  uint64_t index;
  FT_Event event;
  Op* op;          // Optionally, if DFE needs to know which op
  Addr pred_addr;  // Optionally, if DFE needs the predicted address
};

class FT {
 public:
  FT(uns _proc_id = 0);
  ~FT();
  void add_op(Op* op);
  bool can_fetch_op();
  Op* fetch_op();
  FT_Info get_ft_info() const;

  std::vector<Op*>& get_ops();

  // Change return type to FT_BuildResult
  Flag build(std::function<bool(uns8)> can_fetch_op_fn, std::function<bool(uns8, Op*)> fetch_op_fn, bool off_path,
             std::function<uint64_t()> get_next_op_id_fn);

  FT_PredictResult predict_ft();
  std::pair<FT*, FT*> extract_off_path_ft(uns split_index);

  Op* get_last_op() const;
  Op* get_first_op() const;
  Addr get_start_addr() const;
  bool is_consecutive(const FT& previous_ft) const;
  bool has_unread_ops() const { return ops.size() - op_pos != 0; }
  bool ended_by_exit() const { return ft_info.dynamic_info.ended_by == FT_APP_EXIT; }
  bool ended() const { return ft_info.dynamic_info.ended_by != FT_NOT_ENDED; }  // Check if FT is properly ended
  FT_Ended_By get_end_reason() const;
  void clear_recovery_info();

 private:
  uns proc_id;
  uint64_t op_pos;
  FT_Info ft_info;
  std::vector<Op*> ops;
  FT_Event predict_one_cf_op(Op* op);
  void generate_ft_info();
  friend class Decoupled_FE;
};

#endif  // __cplusplus

#endif  // __FT_H__
