/*
 * Copyright 2025 University of California Santa Cruz
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
 * File         : lsq.cc
 * Author       : Litz Lab
 * Date         : 7/2025
 * Description  : Load/Store Queue
 ***************************************************************************************/

#include "lsq.h"

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "exec_ports.h"
#include "node_stage.h"
}

#include <deque>
#include <vector>

/**************************************************************************************/
/* Definition */

struct LSQ_Entry {
  Op* op;
  Counter op_num;
  Counter unique_num;
  Flag off_path;
  Mem_Type mem_type;

  LSQ_Entry() {}
  LSQ_Entry(Op* mem_op)
      : op(mem_op),
        op_num(mem_op->op_num),
        unique_num(mem_op->unique_num),
        off_path(mem_op->off_path),
        mem_type(mem_op->table_info->mem_type) {}
};

class LSQ {
 private:
  int proc_id;
  size_t entry_num;

  std::deque<LSQ_Entry> entries;

 public:
  void init(int proc_id, size_t entry_num);
  void allocate(Op* mem_op);
  void free(Op* mem_op);
  bool available();
  void recover(Counter op_num);

  LSQ(){};
  const std::deque<LSQ_Entry>& get_entries() const { return entries; }
};

/**************************************************************************************/

void LSQ::init(const int proc_id, const size_t entry_num) {
  this->proc_id = proc_id;
  this->entry_num = entry_num;
  entries.clear();
}

void LSQ::allocate(Op* mem_op) {
  ASSERT(proc_id, entries.size() < entry_num);
  ASSERT(proc_id, mem_op->table_info->mem_type == MEM_LD || mem_op->table_info->mem_type == MEM_ST);

  entries.emplace_back(mem_op);
}

void LSQ::free(Op* mem_op) {
  ASSERT(proc_id, !entries.empty());
  ASSERT(mem_op->proc_id, mem_op->table_info->mem_type == MEM_LD || mem_op->table_info->mem_type == MEM_ST);

  if (!mem_op->off_path) {
    ASSERT(proc_id, entries.front().op_num == mem_op->op_num);
    entries.pop_front();
  } else {
    ASSERT(proc_id, entries.back().op_num == mem_op->op_num);
    entries.pop_back();
  }
}

bool LSQ::available() {
  ASSERT(proc_id, entries.size() <= entry_num);
  if (entries.size() == entry_num) {
    return false;
  }

  return true;
}

void LSQ::recover(Counter op_num) {
  while (!entries.empty()) {
    auto& back_entry = entries.back();

    // Stop when reaching on-path ops earlier than the branch
    if (back_entry.op_num < op_num) {
      break;
    }

    // Free this off-path mem op from the back
    free(back_entry.op);
  }
}

/**************************************************************************************/
/* Global Values */

struct LSQ_Unit {
  LSQ load_queue;
  LSQ store_queue;
};

static std::vector<LSQ_Unit> lsq_unit_per_core;

/**************************************************************************************/
/* External Methods */

/*
  Called by:
  --- cmp.c
*/

void alloc_mem_lsq(uns num_cores) {
  if (!LSQ_ENABLE) {
    return;
  }

  lsq_unit_per_core.resize(num_cores);
  for (uns ii = 0; ii < num_cores; ii++) {
    lsq_unit_per_core[ii].load_queue.init(ii, LOAD_QUEUE_ENTRY_NUM);
    lsq_unit_per_core[ii].store_queue.init(ii, STORE_QUEUE_ENTRY_NUM);
  }
}

/*
  Called by:
  --- node_stage.c -> before a mem op is filled into ROB
  Desc:
  --- return TRUE if there is an available entry
*/
Flag lsq_available(Op* mem_op) {
  if (!LSQ_ENABLE)
    return TRUE;

  ASSERT(mem_op->proc_id, mem_op->table_info->mem_type);
  auto& lsq_unit = lsq_unit_per_core[mem_op->proc_id];

  switch (mem_op->table_info->mem_type) {
    case MEM_LD:
      return static_cast<Flag>(lsq_unit.load_queue.available());

    case MEM_ST:
      return static_cast<Flag>(lsq_unit.store_queue.available());

    default:
      ASSERT(mem_op->proc_id, FALSE);
      break;
  }

  return FALSE;
}

/*
  Called by:
  --- node_stage.c -> after a mem op is filled into ROB
  Desc:
  --- allocate an load/store entry
*/
void lsq_dispatch(Op* mem_op) {
  if (!LSQ_ENABLE)
    return;

  ASSERT(mem_op->proc_id, mem_op->table_info->mem_type);
  auto& lsq_unit = lsq_unit_per_core[mem_op->proc_id];

  switch (mem_op->table_info->mem_type) {
    case MEM_LD:
      lsq_unit.load_queue.allocate(mem_op);
      break;

    case MEM_ST:
      lsq_unit.store_queue.allocate(mem_op);
      break;

    default:
      ASSERT(mem_op->proc_id, FALSE);
      break;
  }
}

/*
  Called by:
  --- node_stage.c -> when there is a flushing event
  Desc:
  --- clear the off-path entry
*/
void lsq_recover(Counter op_num) {
  if (!LSQ_ENABLE)
    return;

  auto& lsq_unit = lsq_unit_per_core[node->proc_id];
  lsq_unit.load_queue.recover(op_num);
  lsq_unit.store_queue.recover(op_num);
}

/*
  Called by:
  --- node_stage.c -> when a mem op is retired
  Desc:
  --- free the entry
*/
void lsq_commit(Op* mem_op) {
  if (!LSQ_ENABLE)
    return;

  ASSERT(mem_op->proc_id, mem_op->table_info->mem_type);
  auto& lsq_unit = lsq_unit_per_core[mem_op->proc_id];

  switch (mem_op->table_info->mem_type) {
    case MEM_LD:
      lsq_unit.load_queue.free(mem_op);
      break;

    case MEM_ST:
      lsq_unit.store_queue.free(mem_op);
      break;

    default:
      ASSERT(mem_op->proc_id, FALSE);
      break;
  }
}
