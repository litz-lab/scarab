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

#include "bp/bp.h"

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
  uns8 proc_id;
  Mem_Type mem_type;
  size_t entry_num;

  std::deque<LSQ_Entry> entries;

 public:
  void init(uns8 proc_id, Mem_Type mem_type, size_t entry_num);
  void allocate(Op* mem_op);
  void free(Op* mem_op);
  bool available();
  void recover(Counter flush_op_num);

  LSQ(){};
  const std::deque<LSQ_Entry>& get_entries() const { return entries; }
};

void LSQ::init(const uns8 proc_id, const Mem_Type mem_type, const size_t entry_num) {
  this->proc_id = proc_id;
  this->mem_type = mem_type;
  this->entry_num = entry_num;
  entries.clear();
}

void LSQ::allocate(Op* mem_op) {
  ASSERT(proc_id, entries.size() < entry_num);
  ASSERT(proc_id, mem_op->table_info->mem_type == this->mem_type);

  entries.emplace_back(mem_op);
}

void LSQ::free(Op* mem_op) {
  ASSERT(proc_id, !entries.empty());
  ASSERT(proc_id, mem_op->table_info->mem_type == this->mem_type);
  ASSERT(proc_id, !mem_op->off_path);

  ASSERT(proc_id, entries.front().op_num == mem_op->op_num);
  entries.pop_front();
}

bool LSQ::available() {
  ASSERT(proc_id, entries.size() <= entry_num);
  if (entries.size() == entry_num) {
    return false;
  }

  return true;
}

void LSQ::recover(Counter flush_op_num) {
  while (!entries.empty()) {
    auto& back_entry = entries.back();

    // Stop when reaching on-path ops earlier than the branch
    if (back_entry.op_num < flush_op_num) {
      break;
    }

    // Free this off-path mem op from the back
    ASSERT(proc_id, !entries.empty());
    ASSERT(proc_id, back_entry.op->off_path);
    ASSERT(proc_id, entries.back().op_num == back_entry.op->op_num);
    ASSERT(proc_id, back_entry.op->table_info->mem_type == this->mem_type);
    entries.pop_back();
  }
}

/**************************************************************************************/

class LSQ_Unit {
 private:
  uns8 proc_id;
  LSQ load_queue;
  LSQ store_queue;

 public:
  LSQ_Unit(uns8 proc_id);
  const LSQ* get_queue(Mem_Type mem_type) const;

  void init(uns8 proc_id);
  Flag available(Mem_Type mem_type);
  void dispatch(Op* mem_op);
  void recover(Counter flush_op_num);
  void commit(Op* mem_op);
};

LSQ_Unit::LSQ_Unit(uns8 proc_id) {
  this->init(proc_id);
}

const LSQ* LSQ_Unit::get_queue(Mem_Type mem_type) const {
  switch (mem_type) {
    case MEM_LD:
      return &load_queue;

    case MEM_ST:
      return &store_queue;

    default:
      ASSERT(this->proc_id, FALSE);
      break;
  }

  return nullptr;
}

void LSQ_Unit::init(uns8 proc_id) {
  load_queue.init(proc_id, MEM_LD, LOAD_QUEUE_ENTRY_NUM);
  store_queue.init(proc_id, MEM_ST, STORE_QUEUE_ENTRY_NUM);
}

Flag LSQ_Unit::available(Mem_Type mem_type) {
  switch (mem_type) {
    case MEM_LD:
      return static_cast<Flag>(load_queue.available());

    case MEM_ST:
      return static_cast<Flag>(store_queue.available());

    default:
      ASSERT(this->proc_id, FALSE);
      break;
  }

  return FALSE;
}

void LSQ_Unit::dispatch(Op* mem_op) {
  switch (mem_op->table_info->mem_type) {
    case MEM_LD:
      load_queue.allocate(mem_op);
      break;

    case MEM_ST:
      store_queue.allocate(mem_op);
      break;

    default:
      ASSERT(this->proc_id, FALSE);
      break;
  }
}

void LSQ_Unit::recover(Counter flush_op_num) {
  load_queue.recover(flush_op_num);
  store_queue.recover(flush_op_num);
}

void LSQ_Unit::commit(Op* mem_op) {
  switch (mem_op->table_info->mem_type) {
    case MEM_LD:
      load_queue.free(mem_op);
      break;

    case MEM_ST:
      store_queue.free(mem_op);
      break;

    default:
      ASSERT(mem_op->proc_id, FALSE);
      break;
  }
}

/**************************************************************************************/
/* Global Values */

static std::vector<LSQ_Unit> per_core_lsq_unit;
LSQ_Unit* lsq_unit = nullptr;

/**************************************************************************************/
/* External Methods */

void alloc_mem_lsq(uns num_cores) {
  if (!LSQ_ENABLE)
    return;

  for (uns ii = 0; ii < num_cores; ii++) {
    per_core_lsq_unit.push_back(LSQ_Unit(ii));
  }
}

void set_lsq(uns8 proc_id) {
  if (!LSQ_ENABLE)
    return;

  lsq_unit = &per_core_lsq_unit[proc_id];
}

void init_lsq(uns8 proc_id, const char* name) {
  if (!LSQ_ENABLE)
    return;

  lsq_unit->init(proc_id);
}

void recover_lsq() {
  if (!LSQ_ENABLE)
    return;

  lsq_unit->recover(bp_recovery_info->recovery_op_num);
}

/**************************************************************************************/

/*
  Called by:
  --- node_stage.c -> before a mem op is filled into ROB
  Desc:
  --- return TRUE if there is an available entry
*/
Flag lsq_available(Mem_Type mem_type) {
  if (!LSQ_ENABLE)
    return TRUE;

  return lsq_unit->available(mem_type);
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
  lsq_unit->dispatch(mem_op);
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
  lsq_unit->commit(mem_op);
}

/**************************************************************************************/

int lsq_get_in_flight_load_num() {
  if (!LSQ_ENABLE)
    return 0;

  int in_flight_num = 0;
  const auto& store_entries = lsq_unit->get_queue(MEM_LD)->get_entries();
  for (const auto& entry : store_entries) {
    if (entry.op->state >= OS_IN_RS)
      in_flight_num++;
  }

  return in_flight_num;
}
