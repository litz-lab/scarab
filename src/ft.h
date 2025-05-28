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

#ifdef __cplusplus
}  // extern "C"
#endif

#ifdef __cplusplus

// C++-only includes
#include <vector>

#include "globals/global_defs.h"
#include "globals/global_types.h"

#include "decoupled_frontend.h"

// C++ class definition
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

 private:
  uns proc_id;
  uint64_t op_pos;
  FT_Info ft_info;
  std::vector<Op*> ops;
  bool consumed;

  friend class Decoupled_FE;
};

#endif  // __cplusplus

#endif  // __FT_H__
