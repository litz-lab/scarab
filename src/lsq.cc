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
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 2025
 * Description  :
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

/**************************************************************************************/
/* Definition */

struct LSQ_Entry {
  Op* op;
  Counter op_num;
  Counter unique_num;
  Flag off_path;
  Mem_Type mem_type;

  LSQ_Entry() {}
  LSQ_Entry(Op* op)
      : op(op),
        op_num(op->op_num),
        unique_num(op->unique_num),
        off_path(op->off_path),
        mem_type(op->table_info->mem_type) {}
};

class LSQ {
 private:
  int proc_id;
  size_t entry_num;

  std::deque<LSQ_Entry> entries;

 public:
  void init(int proc_id, size_t entry_num);
  void allocate(Op* op);
  void free(Op* op);
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

void LSQ::allocate(Op* op) {
  ASSERT(proc_id, entries.size() < entry_num);
  ASSERT(proc_id, op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_ST);

  entries.emplace_back(op);
}

void LSQ::free(Op* op) {
  ASSERT(proc_id, !entries.empty());
  ASSERT(op->proc_id, op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_ST);

  if (!op->off_path) {
    ASSERT(proc_id, entries.front().op_num == op->op_num);
    entries.pop_front();
  } else {
    ASSERT(proc_id, entries.back().op_num == op->op_num);
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
    LSQ_Entry& back_entry = entries.back();

    // Stop when reaching on-path ops earlier than the branch
    if (back_entry.op_num < op_num) {
      break;
    }

    // Free this off-path op from the back
    free(back_entry.op);
  }
}

/**************************************************************************************/
/* Values */

LSQ load_queue;
LSQ store_queue;

/**************************************************************************************/
/* External Methods */

/*
  Called by:
  --- cmp.c
  Desc:
  --- return TRUE if there is an available entry
*/

void lsq_init(void) {
  load_queue.init(0, LOAD_QUEUE_ENTRY_NUM);
  store_queue.init(0, STORE_QUEUE_ENTRY_NUM);
}

/*
  Called by:
  --- node_stage.c -> before a mem op is filled into ROB
  Desc:
  --- return TRUE if there is an available entry
*/
Flag lsq_available(Op* op) {
  if (!LSQ_ENABLE)
    return TRUE;

  ASSERT(op->proc_id, op->table_info->mem_type);
  switch (op->table_info->mem_type) {
    case MEM_LD:
      return static_cast<Flag>(load_queue.available());

    case MEM_ST:
      return static_cast<Flag>(store_queue.available());

    default:
      ASSERT(op->proc_id, FALSE);
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
void lsq_dispatch(Op* op) {
  if (!LSQ_ENABLE)
    return;

  ASSERT(op->proc_id, op->table_info->mem_type);
  switch (op->table_info->mem_type) {
    case MEM_LD:
      load_queue.allocate(op);
      break;

    case MEM_ST:
      store_queue.allocate(op);
      break;

    default:
      ASSERT(op->proc_id, FALSE);
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

  load_queue.recover(op_num);
  store_queue.recover(op_num);
}

/*
  Called by:
  --- node_stage.c -> when the op is retired
  Desc:
  --- free the entry
*/
void lsq_commit(Op* op) {
  if (!LSQ_ENABLE)
    return;

  ASSERT(op->proc_id, op->table_info->mem_type);
  switch (op->table_info->mem_type) {
    case MEM_LD:
      load_queue.free(op);
      break;

    case MEM_ST:
      store_queue.free(op);
      break;

    default:
      ASSERT(op->proc_id, FALSE);
      break;
  }
}
