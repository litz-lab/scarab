/*
 * Copyright 2026 University of California Santa Cruz
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
 * File         : reg_port.cc
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 10/2026
 * Description  : Register file read port arbitration, driven by the exec stage
 ***************************************************************************************/

#include "reg_port.h"

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "core.param.h"

#include "isa/isa.h"

#include "exec_stage.h"
#include "map_rename.h"
#include "op_info.h"
}

#include <algorithm>
#include <vector>

/**************************************************************************************/
/* Static methods */

// check if its source wakes up within the bypass window
static inline bool reg_read_port_src_forwarded(const Op* op, uns src_idx) {
  if (REG_READ_PORT_BYPASS_DEPTH == 0) {
    return false;
  }

  ASSERT(op->proc_id, src_idx < op->num_srcs);
  const Src_Info* src_info = &op->src_info[src_idx];
  ASSERT(op->proc_id, src_info->type == REG_DATA_DEP);

  // the producer has left the machine, so its value can only be read from the register file
  const Op* src_op = src_info->op;
  if (!src_op->op_pool_valid || src_op->unique_num != src_info->unique_num) {
    return false;
  }

  Counter wake_cycle = op_get_wake_cycle(src_op);
  ASSERT(op->proc_id, wake_cycle != MAX_CTR && cycle_count >= wake_cycle);

  return cycle_count - wake_cycle < REG_READ_PORT_BYPASS_DEPTH;
}

// the key identifies the register value being read by its register type and physical register id
static inline uns64 reg_read_port_src_key(const Op* op, uns src_idx, int reg_type) {
  constexpr uns REG_READ_PORT_KEY_TYPE_SHIFT = 48;
  uns16 physical_reg_id = op->src_reg_id[src_idx][REG_TABLE_TYPE_PHYSICAL];
  ASSERT(op->proc_id, physical_reg_id != REG_TABLE_REG_ID_INVALID);
  return (static_cast<uns64>(reg_type) << REG_READ_PORT_KEY_TYPE_SHIFT) | physical_reg_id;
}

/**************************************************************************************/
/* Structures */

/*
 * RegReadPortArbitrator arbitrates the register file read ports in each cycle.
 *
 * An issued op consumes one port for each distinct source register that is not forwarded.
 * If port sharing is enabled, ops reading the same register in the
 * same cycle share a single port.
 *
 * An op either acquires all ports it needs or none of them.
 * The ops are arbitrated in the descending FU slot order, and the FU slots of the ops that cannot
 * acquire their read ports are returned as a bitmask for the exec stage to reject them.
 */
class RegReadPortArbitrator {
 private:
  const uns proc_id;
  uns used_num = 0;
  std::vector<uns64> reg_keys;

  bool acquire(Op* op);

 public:
  explicit RegReadPortArbitrator(uns proc_id);

  uns64 arbitrate(Stage_Data* src_sd);
};

RegReadPortArbitrator::RegReadPortArbitrator(uns proc_id) : proc_id(proc_id) {
  ASSERT(proc_id, !REG_READ_PORT_ARBITRATE_ENABLE || REG_RENAMING_SCHEME == REG_RENAMING_SCHEME_REALISTIC);
  ASSERT(proc_id, !REG_READ_PORT_ARBITRATE_ENABLE || REG_READ_PORT_NUM > 0);
}

bool RegReadPortArbitrator::acquire(Op* op) {
  ASSERT(proc_id, op != nullptr && op->proc_id == proc_id);
  ASSERT(proc_id, op->uop->num_src_regs <= MAX_SRCS);

  // the register file reads requested by this op
  uns64 req_keys[MAX_SRCS];
  uns req_num = 0;

  // only collect the source stats when the op is granted, since a denied op is arbitrated again later
  uns src_total = 0;
  uns src_forwarded = 0;
  uns src_shared = 0;
  uns src_shared_classes[REG_PORT_REG_CLASS_NUM] = {0};

  for (uns ii = 0; ii < op->uop->num_src_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->uop->srcs[ii].id);
    if (reg_type == REG_FILE_REG_TYPE_OTHER) {
      continue;
    }
    src_total++;

    // the value comes from the bypass network
    if (reg_read_port_src_forwarded(op, ii)) {
      src_forwarded++;
      continue;
    }

    // the same register is already read by another op in this cycle, except the flag registers
    uns64 key = reg_read_port_src_key(op, ii, reg_type);
    uns reg_class = reg_port_get_reg_class(op->uop->srcs[ii].id);
    bool read = REG_READ_PORT_SHARE && reg_class != REG_PORT_REG_CLASS_FLG &&
                std::find(reg_keys.begin(), reg_keys.end(), key) != reg_keys.end();

    if (read) {
      src_shared++;
      src_shared_classes[reg_class]++;
      continue;
    }

    req_keys[req_num++] = key;
  }

  // check the budget before reserving any port
  if (used_num + req_num > REG_READ_PORT_NUM) {
    STAT_EVENT(proc_id, REG_READ_PORT_OP_REJECTED);
    return false;
  }

  // reserve the ports
  used_num += req_num;
  reg_keys.insert(reg_keys.end(), req_keys, req_keys + req_num);

  STAT_EVENT(proc_id, REG_READ_PORT_OP_GRANTED);
  INC_STAT_EVENT(proc_id, REG_READ_PORT_SRC_TOTAL, src_total);
  INC_STAT_EVENT(proc_id, REG_READ_PORT_SRC_FORWARDED, src_forwarded);
  INC_STAT_EVENT(proc_id, REG_READ_PORT_SRC_SHARED, src_shared);

  // collect the register classes of the shared register reads
  if (src_shared > 0) {
    for (uns ii = 0; ii < REG_PORT_REG_CLASS_NUM; ++ii) {
      INC_STAT_EVENT(proc_id, REG_READ_PORT_SHARED_REG_GPR + ii, src_shared_classes[ii]);
    }
  }

  return true;
}

uns64 RegReadPortArbitrator::arbitrate(Stage_Data* src_sd) {
  ASSERT(proc_id, src_sd->max_op_count <= 64);
  if (!REG_READ_PORT_ARBITRATE_ENABLE) {
    return 0;
  }

  // clear the read port usage of the last cycle
  used_num = 0;
  reg_keys.clear();

  // the ops are arbitrated in the descending FU slot order
  uns64 reject_mask = 0;
  for (int ii = src_sd->max_op_count - 1; ii >= 0; --ii) {
    if (src_sd->ops[ii] == nullptr || acquire(src_sd->ops[ii])) {
      continue;
    }

    reject_mask |= 1ULL << ii;
  }

  return reject_mask;
}

/**************************************************************************************/
/* Global Values */

static std::vector<RegReadPortArbitrator> per_core_reg_read_port_arbitrator;
static RegReadPortArbitrator* reg_read_port_arbitrator = nullptr;

/**************************************************************************************/
/* External Function */

uns reg_port_get_reg_class(uns reg_id) {
  if (reg_id >= REG_RAX && reg_id < REG_CS)
    return REG_PORT_REG_CLASS_GPR;
  if (reg_id >= REG_ZPS && reg_id <= REG_DF)
    return REG_PORT_REG_CLASS_FLG;
  if (reg_id >= REG_ZMM0 && reg_id < REG_K0)
    return REG_PORT_REG_CLASS_VEC;

  ASSERT(0, reg_id >= REG_TMP0 && reg_id <= REG_TMP4);
  return REG_PORT_REG_CLASS_TMP;
}

uns64 reg_port_arbitrate(Stage_Data* src_sd) {
  ASSERT(exec->proc_id, reg_read_port_arbitrator != nullptr);
  return reg_read_port_arbitrator->arbitrate(src_sd);
}

/**************************************************************************************/
/* Vanilla API */

void alloc_mem_reg_port(uns num_cores) {
  per_core_reg_read_port_arbitrator.reserve(num_cores);
  for (uns ii = 0; ii < num_cores; ii++) {
    per_core_reg_read_port_arbitrator.emplace_back(ii);
  }
}

void set_reg_port(uns8 proc_id) {
  ASSERT(proc_id, proc_id < per_core_reg_read_port_arbitrator.size());
  reg_read_port_arbitrator = &per_core_reg_read_port_arbitrator[proc_id];
}
