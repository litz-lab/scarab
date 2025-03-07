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
 * File         : idq.cc
 * Author       : Mingsheng Xu <mxu61@ucsc.edu>
 * Date         : 03/05/2025
 * Description  : Instruction Decode Queue (IDQ) bridges the front-end and the back-end.
 ***************************************************************************************/

#include "idq.h"

#include <vector>

extern "C" {
#include "globals/assert.h"
#include "globals/enum.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "bp/bp.h"

#include "decode_stage.h"
#include "op_pool.h"
}

class IDQ {
 public:
  void init(uns8 _proc_id);
  void reset();
  void recover();
  void debug();
  void update(Stage_Data* dec_src_sd, Stage_Data* uopc_src_sd);
  bool enqueue(Op* op);
  Op* dequeue();
  Op* peek();
  inline int wrap_around(int);

 private:
  uns8 proc_id;
  int capacity;
  std::vector<Op*> ops;
  int occupied_count;
  int head;
  int tail;
  Counter next_op_num;
};

/* Global Variables */
IDQ* idq = NULL;

/* Per-Core IDQ */
std::vector<IDQ> per_core_idq;

void IDQ::init(uns8 _proc_id) {
  proc_id = _proc_id;
  capacity = IDQ_SIZE;
  next_op_num = 1;
  reset();
}

void IDQ::reset() {
  ops.clear();
  ops.resize(capacity, NULL);
  occupied_count = 0;
  head = 0;
  tail = 0;
}

void IDQ::recover() {
  if (occupied_count != 0) {
    int i = wrap_around(tail - 1);
    do {
      if (FLUSH_OP(ops[i])) {
        ASSERT(proc_id, i == wrap_around(tail - 1));
        free_op(ops[i]);
        ops[i] = NULL;
        occupied_count--;
        tail = wrap_around(tail - 1);
      }
      i = wrap_around(i - 1);
    } while (i != wrap_around(head - 1));
  }

  if (occupied_count == 0) {
    ASSERT(proc_id, head == tail);
  }

  if (next_op_num > bp_recovery_info->recovery_op_num) {
    next_op_num = bp_recovery_info->recovery_op_num + 1;
  }
}

void IDQ::debug() {
}

void IDQ::update(Stage_Data* dec_src_sd, Stage_Data* uopc_src_sd) {
  // When the uop cache is enabled, the next op to enqueue the idq is from either:
  // 1. the decode stage
  // 2. the uop cache source
  // Furthermore, the uop cache source is either:
  // 1. the uop queue
  // 2. the icache stage uopc stage data bypassing the uop queue

  // The ops are enqueued in order, i.e.,
  // only consume if older ops have already been consumed by this stage.
  Stage_Data* consume_from_sd = NULL;
  if (UOP_CACHE_ENABLE) {
    ASSERT(proc_id, uopc_src_sd != NULL);
    if (dec_src_sd->op_count && dec_src_sd->ops[0]->op_num == next_op_num) {
      consume_from_sd = dec_src_sd;
    } else if (uopc_src_sd->op_count && uopc_src_sd->ops[0]->op_num == next_op_num) {
      consume_from_sd = uopc_src_sd;
    }
  } else {
    // When the uop cache is disabled, the next op to enqueue is from the decode stage.
    ASSERT(proc_id, uopc_src_sd == NULL);
    if (dec_src_sd->op_count) {
      ASSERT(proc_id, dec_src_sd->ops[0]->op_num == next_op_num);
      consume_from_sd = dec_src_sd;
    }
  }

  /* return if the next expected op is not ready or there is no enough space */
  if (!consume_from_sd || capacity - occupied_count < consume_from_sd->op_count) {
    return;
  }

  int size_before_consuming = consume_from_sd->op_count;
  for (int ii = 0; ii < size_before_consuming; ii++) {
    Op* op = consume_from_sd->ops[ii];
    ASSERT(proc_id, op && op->op_num == next_op_num);
    if (!op->decode_cycle) {
      decode_stage_process_op(op);
    }
    bool success = enqueue(op);
    ASSERT(proc_id, success);
    consume_from_sd->ops[ii] = NULL;
    consume_from_sd->op_count--;
    next_op_num++;
  }

  ASSERT(proc_id, !consume_from_sd->op_count);
}

bool IDQ::enqueue(Op* op) {
  if (occupied_count < capacity) {
    ops[tail] = op;
    occupied_count++;
    tail = wrap_around(tail + 1);
    return true;
  } else {
    return false;
  }
}

Op* IDQ::dequeue() {
  if (occupied_count > 0) {
    Op* op = ops[head];
    ASSERT(proc_id, op);
    ops[head] = NULL;
    occupied_count--;
    head = wrap_around(head + 1);
    return op;
  } else {
    return NULL;
  }
}

Op* IDQ::peek() {
  if (occupied_count > 0) {
    ASSERT(proc_id, ops[head]);
    return ops[head];
  } else {
    return NULL;
  }
}

int IDQ::wrap_around(int index) {
  return (index + capacity) % capacity;
}

/* Wrapper functions */
void alloc_mem_idq(uns8 numCores) {
  per_core_idq.resize(numCores);
}

void set_idq(uns8 proc_id) {
  idq = &per_core_idq[proc_id];
}

void init_idq(uns8 proc_id) {
  idq->init(proc_id);
}

void reset_idq(void) {
  idq->reset();
}

void recover_idq(void) {
  idq->recover();
}

void debug_idq(void) {
  idq->debug();
}

void update_idq(Stage_Data* dec_src_sd, Stage_Data* uopc_src_sd) {
  idq->update(dec_src_sd, uopc_src_sd);
}

Op* dequeue_op_from_idq(void) {
  return idq->dequeue();
}

Op* peek_op_from_idq(void) {
  return idq->peek();
}