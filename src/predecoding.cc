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
 * File         : predecoding.cc
 * Author       : Mingsheng Xu <mxu61@ucsc.edu>
 * Date         : 01/04/2025
 * Description  : Predecoding functions
 ***************************************************************************************/

#include "predecoding.h"
#include "globals/enum.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "memory/memory.param.h"
#include "decoupled_frontend.h"

#include <vector>

class PREDECODING {
public:
  void init(uns _proc_id);
  void probe_ftq();
  uint64_t get_next_ft_pos(Predecoding_Marker marker);
private:
  uns proc_id;
  decoupled_fe_iter* ftq_iter;
};

/* Global Variables */
PREDECODING* predecoding = NULL;

// Per core predecoding
std::vector<PREDECODING> per_core_predecoding;

void PREDECODING::init(uns _proc_id) {
  proc_id = _proc_id;
  ftq_iter = decoupled_fe_new_ftq_iter(proc_id);
}

void PREDECODING::probe_ftq() {
  // predecoding sets the markers
  int probe_per_cycle = 0;
  while (ftq_iter->ft_pos < decoupled_fe_ftq_num_fts() && probe_per_cycle < (int)UOP_CACHE_READ_PORTS + (int)UOP_CACHE_FTQ_ADDITIONAL_PROBE) {
    FT* next_probe_ft = decoupled_fe_get_ft(ftq_iter->ft_pos);
    ASSERT(proc_id, next_probe_ft);
    ASSERT(proc_id, ft_get_predecoding_marker(next_probe_ft) == FT_NOT_LOOKED_UP);
    FT_Info next_probe_info = ft_get_ft_info(next_probe_ft);
    if (uop_cache_lookup_ft_and_fill_lookup_buffer(next_probe_info, next_probe_info.dynamic_info.first_op_off_path)) {
      ft_set_predecoding_marker(next_probe_ft, FT_IN_UOP_CACHE);
    } else {
      ft_set_predecoding_marker(next_probe_ft, FT_NOT_IN_UOP_CACHE);
    }

    ftq_iter->ft_pos++;
    ftq_iter->flattened_op_pos += next_probe_info.static_info.n_uops;

    probe_per_cycle++;
  }
}

uint64_t PREDECODING::get_next_ft_pos(Predecoding_Marker marker) {
  if (ftq_iter->ft_pos < decoupled_fe_ftq_num_fts()) {
    ASSERT(proc_id, ft_get_predecoding_marker(decoupled_fe_get_ft(ftq_iter->ft_pos)) == FT_NOT_LOOKED_UP);
  }

  for (uint64_t i = 0; i < ftq_iter->ft_pos && i < decoupled_fe_ftq_num_fts(); i++) {
    FT* ft = decoupled_fe_get_ft(i);
    Predecoding_Marker marker_at_i = ft_get_predecoding_marker(ft);
    ASSERT(proc_id, marker_at_i != FT_NOT_LOOKED_UP);
    if (!ft_is_consumed(ft) && marker_at_i == marker) {
      if (ON_PATH_DECOUPLED_ICACHE_STAGE && ft_get_ft_info(ft).dynamic_info.first_op_off_path && i != 0) {
        // when ON_PATH_DECOUPLED_ICACHE_STAGE is on, stop the runahead if the ft is off-path;
        // i.e., only fetch at the ftq top
        return decoupled_fe_ftq_num_fts();
      }
      return i;
    }
  }

  // if the desired ft is not found, return the ftq size
  return decoupled_fe_ftq_num_fts();
}

/* Wrapper functions */
void alloc_mem_predecoding(uns numCores) {
  if (!DECOUPLED_ICACHE_STAGE) {
    return;
  }
  per_core_predecoding.resize(numCores);
}

void init_predecoding(uns proc_id) {
  if (!DECOUPLED_ICACHE_STAGE) {
    return;
  }
  ASSERT(proc_id, UOP_CACHE_ENABLE);
  per_core_predecoding[proc_id].init(proc_id);
}

void set_predecoding(int proc_id) {
  if (!DECOUPLED_ICACHE_STAGE) {
    return;
  }
  predecoding = &per_core_predecoding[proc_id];
}

void predecoding_probe_ftq() {
  predecoding->probe_ftq();
}

uint64_t predecoding_get_next_ft_pos(Predecoding_Marker marker) {
  return predecoding->get_next_ft_pos(marker);
}