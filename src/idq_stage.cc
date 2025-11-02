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
 * File         : idq_stage.cc
 * Author       : Mingsheng Xu <mxu61@ucsc.edu>
 * Date         : 03/05/2025
 * Description  : Instruction Decode Queue (IDQ) bridges the front-end and the back-end.
 ***************************************************************************************/

#include "idq_stage.h"

#include <vector>

#include "ft.h"

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
#include "topdown.h"
}

class IDQ_Stage {
 public:
  void init(uns8 _proc_id, const char* name);
  void reset();
  void recover();
  void debug();
  void update(Stage_Data* dec_src_sd, Stage_Data* ic_uopc_sd, Stage_Data* uop_queue_sd);
  Stage_Data* get_output_stage_data();

  void set_recovery_cycle(int recovery_cycle);
  int get_recovery_cycle() const;

 private:
  uns8 proc_id;
  int capacity;
  std::vector<Op*> ops;
  int occupied_count;
  int head;
  int tail;
  Counter next_op_num;
  int recovery_cycle;

  /* the IDQ outpur stage data */
  Stage_Data idq_sd;

  Stage_Data* select_input_stage_data(Stage_Data* dec_src_sd, Stage_Data* ic_uopc_sd, Stage_Data* uop_queue_sd);
  void process_input_stage_data(Stage_Data* consume_from_sd, int& count_issued, int& count_issued_on_path);
  bool enqueue(Op* op);
  Op* dequeue();
  inline int wrap_around(int);
};

/* Global Variables */
IDQ_Stage* idq_stage = NULL;

/* Per-Core IDQ_Stage */
std::vector<IDQ_Stage> per_core_idq_stage;

void IDQ_Stage::init(uns8 _proc_id, const char* name) {
  proc_id = _proc_id;
  capacity = IDQ_SIZE;
  next_op_num = 1;

  /* Init the IDQ output stage data. */
  char tmp_name[MAX_STR_LENGTH + 1];
  snprintf(tmp_name, MAX_STR_LENGTH, "%s %d", name, 0);
  idq_sd.name = (char*)strdup(tmp_name);
  idq_sd.max_op_count = ISSUE_WIDTH;
  idq_sd.ops = (Op**)malloc(sizeof(Op*) * ISSUE_WIDTH);
  idq_sd.op_count = 0;

  reset();
}

void IDQ_Stage::reset() {
  ops.clear();
  ops.resize(capacity, NULL);
  occupied_count = 0;
  head = 0;
  tail = 0;
  recovery_cycle = 0;

  for (int i = 0; i < idq_sd.max_op_count; i++) {
    idq_sd.ops[i] = NULL;
  }
  idq_sd.op_count = 0;
}

void IDQ_Stage::recover() {
  if (occupied_count != 0) {
    int i = wrap_around(tail - 1);
    do {
      if (FLUSH_OP(ops[i])) {
        ASSERT(proc_id, i == wrap_around(tail - 1));
        ft_free_op(ops[i]);
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

  for (int i = idq_sd.op_count - 1; i >= 0; i--) {
    Op* op = idq_sd.ops[i];
    if (op && FLUSH_OP(op)) {
      ASSERT(proc_id, i == idq_sd.op_count - 1);
      ft_free_op(op);
      idq_sd.ops[i] = NULL;
      idq_sd.op_count--;
    }
  }
}

void IDQ_Stage::debug() {
}

Stage_Data* IDQ_Stage::select_input_stage_data(Stage_Data* dec_src_sd, Stage_Data* ic_uopc_sd,
                                               Stage_Data* uop_queue_sd) {
  /* The uops are enqueued in order, i.e.,
   * only consume if older uops have already been consumed by this stage.
   * This is enforced by the `next_op_num` counter of IDQ stage. */

  /* When the uop cache is disabled, the next uop to enqueue the idq is from the decode stage. */
  if (!UOP_CACHE_ENABLE) {
    Stage_Data* consume_from_sd = NULL;
    if (dec_src_sd->op_count) {
      ASSERT(proc_id, dec_src_sd->ops[0]->op_num == next_op_num);
      consume_from_sd = dec_src_sd;
    }
    return consume_from_sd;
  }

  /* When the uop cache is enabled, the next uop to enqueue the idq is from either:
   * 1. the decode stage
   * 2. the uop cache source
   * Furthermore, the uop cache source is either:
   * 2.1. the uop queue
   * 2.2. the icache stage uopc stage data bypassing the uop queue */
  Stage_Data* consume_from_sd = NULL;
  Stage_Data* uopc_src_sd = uop_queue_sd->op_count ? uop_queue_sd : ic_uopc_sd;
  if (uop_queue_sd->op_count && ic_uopc_sd->op_count) {
    /* The stage data from the uop queue should be older. */
    ASSERT(proc_id, uop_queue_sd->ops[uop_queue_sd->op_count - 1]->op_num < ic_uopc_sd->ops[0]->op_num);
  }
  if (dec_src_sd->op_count && dec_src_sd->ops[0]->op_num == next_op_num) {
    consume_from_sd = dec_src_sd;
  } else if (uopc_src_sd->op_count && uopc_src_sd->ops[0]->op_num == next_op_num) {
    consume_from_sd = uopc_src_sd;
  }
  return consume_from_sd;
}

void IDQ_Stage::process_input_stage_data(Stage_Data* consume_from_sd, int& count_issued, int& count_issued_on_path) {
  /* Return if the next expected uop has not yet arrived. */
  if (!consume_from_sd) {
    return;
  }

  /* Return if there is no enough space. */
  if (capacity - occupied_count < consume_from_sd->op_count) {
    ASSERT(proc_id, idq_sd.op_count == idq_sd.max_op_count);
    return;
  }

  /* Process the input stage data. */
  int op_count_before_consuming = consume_from_sd->op_count;
  for (int i = 0; i < op_count_before_consuming; i++) {
    Op* op = consume_from_sd->ops[i];
    ASSERT(proc_id, op && op->op_num == next_op_num);
    /* If the uops are fetched from the uop cache,
     * it is possible that they have not yet called decode_stage_process_op */
    if (!op->decode_cycle) {
      ASSERT(proc_id, op->fetched_from_uop_cache);
      decode_stage_process_op(op);
    }
    /* If there are still slots in the output stage data,
     * bypass the queue and go straight to the output data.
     * Otherwise, enqueue the IDQ. */
    if (idq_sd.op_count < idq_sd.max_op_count) {
      ASSERT(proc_id, !occupied_count);
      idq_sd.ops[idq_sd.op_count++] = op;
      if (!op->off_path) {
        count_issued_on_path++;
      }
      count_issued++;
    } else {
      bool success = enqueue(op);
      ASSERT(proc_id, success);
    }
    consume_from_sd->ops[i] = NULL;
    consume_from_sd->op_count--;
    next_op_num++;
  }
  ASSERT(proc_id, !consume_from_sd->op_count);

  /* The output stage data should be full unless the IDQ is empty. */
  ASSERT(proc_id, idq_sd.op_count == idq_sd.max_op_count || !occupied_count);
}

void IDQ_Stage::update(Stage_Data* dec_src_sd, Stage_Data* ic_uopc_sd, Stage_Data* uop_queue_sd) {
  /* Fill the IDQ output stage data with uops from IDQ. */
  int count_issued = 0;
  int count_issued_on_path = 0;
  for (int i = idq_sd.op_count; i < idq_sd.max_op_count; i++) {
    Op* op = dequeue();
    if (!op) {
      ASSERT(proc_id, !occupied_count);
      break;
    }
    idq_sd.ops[i] = op;
    idq_sd.op_count++;

    if (!op->off_path) {
      count_issued_on_path++;
    }
    count_issued++;
  }

  /* Select the input stage data. */
  Stage_Data* consume_from_sd = select_input_stage_data(dec_src_sd, ic_uopc_sd, uop_queue_sd);
  process_input_stage_data(consume_from_sd, count_issued, count_issued_on_path);

  topdown_idq_update(proc_id, idq_sd.op_count, count_issued, count_issued_on_path);
}

bool IDQ_Stage::enqueue(Op* op) {
  if (occupied_count == capacity) {
    return false;
  }
  ASSERT(proc_id, op);
  ops[tail] = op;
  occupied_count++;
  ASSERT(proc_id, occupied_count <= capacity);
  tail = wrap_around(tail + 1);
  return true;
}

Op* IDQ_Stage::dequeue() {
  if (occupied_count == 0) {
    return NULL;
  }
  Op* op = ops[head];
  ASSERT(proc_id, op);
  ops[head] = NULL;
  occupied_count--;
  ASSERT(proc_id, occupied_count >= 0);
  head = wrap_around(head + 1);
  return op;
}

int IDQ_Stage::wrap_around(int index) {
  return (index + capacity) % capacity;
}

void IDQ_Stage::set_recovery_cycle(int recovery_cycle) {
  this->recovery_cycle = recovery_cycle;
}

int IDQ_Stage::get_recovery_cycle() const {
  return recovery_cycle;
}

Stage_Data* IDQ_Stage::get_output_stage_data() {
  return &idq_sd;
}

/* Wrapper functions */
void alloc_mem_idq_stage(uns8 num_cores) {
  per_core_idq_stage.resize(num_cores);
}

void set_idq_stage(uns8 proc_id) {
  idq_stage = &per_core_idq_stage[proc_id];
}

void init_idq_stage(uns8 proc_id, const char* name) {
  idq_stage->init(proc_id, name);
}

void reset_idq_stage(void) {
  idq_stage->reset();
}

void recover_idq_stage(void) {
  idq_stage->recover();
}

void debug_idq_stage(void) {
  idq_stage->debug();
}

void update_idq_stage(Stage_Data* dec_src_sd, Stage_Data* ic_uopc_sd, Stage_Data* uop_queue_sd) {
  idq_stage->update(dec_src_sd, ic_uopc_sd, uop_queue_sd);
}

Stage_Data* idq_stage_get_stage_data() {
  return idq_stage->get_output_stage_data();
}

void idq_stage_set_recovery_cycle(int recovery_cycle) {
  idq_stage->set_recovery_cycle(recovery_cycle);
}

int idq_stage_get_recovery_cycle() {
  return idq_stage->get_recovery_cycle();
}