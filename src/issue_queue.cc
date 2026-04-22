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
 * File         : issue_queue.cc
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 4/2026
 * Description  :
 ***************************************************************************************/

#include "issue_queue.h"

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
#include "memory/memory.h"

#include "exec_ports.h"
#include "map_rename.h"
#include "node_stage.h"
}

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_NODE_STAGE, ##args)

/**************************************************************************************/
/* Constexpr */

enum ISSUE_QUEUE_ENTRY_STATE {
  ISSUE_QUEUE_ENTRY_STATE_EMPTY,
  ISSUE_QUEUE_ENTRY_STATE_ALLOC,
  ISSUE_QUEUE_ENTRY_STATE_READY,
  ISSUE_QUEUE_ENTRY_STATE_NUM
};

/**************************************************************************************/
/* Prototypes */

static inline Flag issue_queue_check_op_ready(Op* op);
static inline Flag issue_queue_check_fu_available(Op* op, Func_Unit* fu);
static inline void issue_queue_pick_op(Op* op, uns fu_id);
static inline void issue_queue_update_mem_block();
static inline void issue_queue_track_stats();
static inline uns16 issue_queue_dispatch_find_emptiest(Op* op);

/**************************************************************************************/
/* Structures */

struct IssueQueueEntry {
  const uns16 queue_id;
  const uns16 entry_id;

  Op* op = nullptr;
  ISSUE_QUEUE_ENTRY_STATE state = ISSUE_QUEUE_ENTRY_STATE_EMPTY;

  explicit IssueQueueEntry(uns16 queue_id, uns16 entry_id) : queue_id(queue_id), entry_id(entry_id) {}
  void clear();
  void fill(Op* op);
};

void IssueQueueEntry::clear() {
  op = nullptr;
}

void IssueQueueEntry::fill(Op* op) {
  this->op = op;
}

/**************************************************************************************/
/*
 * The select logic is responsible for picking ready ops from the wakeup logic in the issue queue. Since the issue
 * queue is organized in a non-ordered (random) manner, the select logic typically relies on an age matrix to track
 * the relative age of entries.
 */

class SelectLogic {
 public:
  virtual void request(Op* op) = 0;
  virtual void release(Op* op) = 0;
  virtual void select() = 0;
};

/*
 * Serial picking is described in "Quantifying the complexity of superscalar processors", 1996.
 * The select logic consists of multiple blocks arranged in series and each block receives request signals that are
 * derived from the requests of the preceding block.
 *
 * A round-robin oldest-first selection is implemented in each block.
 */
class SerialOldestFirstSelectLogic : public SelectLogic {
 private:
  const uns queue_id;
  const std::vector<int> connected_fus;

  std::map<Counter, Op*> ready_list;  // age ordered ready list of ops
  int fu_idx = 0;                     // circular queue idx for round-robin selection

 public:
  SerialOldestFirstSelectLogic(uns queue_id, const std::vector<int>& connected_fus)
      : queue_id(queue_id), connected_fus(connected_fus) {}
  void request(Op* op) override;
  void release(Op* op) override;
  void select() override;
};

void SerialOldestFirstSelectLogic::request(Op* op) {
  op->in_rdy_list = TRUE;
  ready_list[op->op_num] = op;
}

void SerialOldestFirstSelectLogic::release(Op* op) {
  op->in_rdy_list = FALSE;
  ready_list.erase(op->op_num);
}

void SerialOldestFirstSelectLogic::select() {
  for (auto it = ready_list.begin(); it != ready_list.end(); ++it) {
    // check if the op is still ready and can be issued
    Op* op = it->second;
    if (!issue_queue_check_op_ready(op)) {
      continue;
    }

    // find a connected FU that can execute this op
    for (uns32 ii = 0; ii < connected_fus.size(); ++ii) {
      Func_Unit* fu = &exec->fus[connected_fus[(fu_idx + ii) % connected_fus.size()]];
      if (!issue_queue_check_fu_available(op, fu)) {
        continue;
      }

      // select this FU for the op
      issue_queue_pick_op(op, fu->fu_id);

      // move the circular queue idx to the next position for the next selection
      fu_idx = (fu_idx + ii + 1) % connected_fus.size();

      break;
    }
  }
}

/*
 * Parallel selection allows each functional unit to independently select a ready op in the same cycle. An arbitration
 * mechanism is then used to resolve conflicts when multiple functional units select the same op.
 *
 * A round-robin quick cancellation mechanism is used in this arbitration.
 */
class ParallelOldestFirstSelectLogic : public SelectLogic {
 private:
  const uns queue_id;
  const std::vector<int> connected_fus;

  std::vector<std::map<Counter, Op*>> ready_list;  // age ordered ready list of ops for each connected FU
  int fu_idx = 0;                                  // circular queue idx for round-robin selection

 public:
  ParallelOldestFirstSelectLogic(uns queue_id, const std::vector<int>& connected_fus)
      : queue_id(queue_id), connected_fus(connected_fus) {
    ready_list.resize(connected_fus.size());
  }
  void request(Op* op) override;
  void release(Op* op) override;
  void select() override;
};

// The op is added to the ready list of all connected FUs that can execute it
void ParallelOldestFirstSelectLogic::request(Op* op) {
  op->in_rdy_list = TRUE;
  for (size_t i = 0; i < connected_fus.size(); ++i) {
    Func_Unit* fu = &exec->fus[connected_fus[i]];
    if (get_fu_type(op->inst_info->table_info.op_type, op->inst_info->table_info.is_simd) & fu->type) {
      ready_list[i][op->op_num] = op;
    }
  }
}

// The op is removed from the ready list of all connected FUs that can execute it
void ParallelOldestFirstSelectLogic::release(Op* op) {
  op->in_rdy_list = FALSE;
  for (size_t i = 0; i < connected_fus.size(); ++i) {
    ready_list[i].erase(op->op_num);
  }
}

void ParallelOldestFirstSelectLogic::select() {
  // init iterators
  using ReadyIter = std::map<Counter, Op*>::iterator;
  std::vector<ReadyIter> cur_iter(connected_fus.size());
  for (size_t i = 0; i < connected_fus.size(); ++i) {
    cur_iter[i] = ready_list[i].begin();
  }

  // pick next candidate
  auto pick_next = [&](size_t i) -> Op* {
    auto& it = cur_iter[i];

    while (it != ready_list[i].end()) {
      Op* op = it->second;
      ++it;

      if (!issue_queue_check_op_ready(op)) {
        continue;
      }
      return op;
    }
    return nullptr;
  };

  // parallel pick an op for each connected FU
  std::vector<Op*> selected_ops(connected_fus.size(), nullptr);
  std::unordered_map<Counter, std::vector<size_t>> owner;
  std::unordered_set<Counter> conflict_ops;
  for (size_t i = 0; i < connected_fus.size(); ++i) {
    Op* op = pick_next(i);
    if (op == nullptr) {
      continue;
    }
    selected_ops[i] = op;

    auto it = owner.find(op->op_num);
    if (it != owner.end()) {
      conflict_ops.insert(op->op_num);
    }
    owner[op->op_num].push_back(i);
  }

  // arbitration among the selected ops if there are conflicts
  auto rr_rank = [&](size_t slot) { return (slot + connected_fus.size() - fu_idx) % connected_fus.size(); };
  while (!conflict_ops.empty()) {
    Counter conflict_op_num = *conflict_ops.begin();
    conflict_ops.erase(conflict_ops.begin());

    std::vector<size_t> slots = owner[conflict_op_num];
    if (slots.size() <= 1) {
      continue;
    }

    // pick the winner based on round-robin order
    size_t winner = slots[0];
    for (size_t s : slots) {
      if (rr_rank(s) < rr_rank(winner)) {
        winner = s;
      }
    }
    owner[conflict_op_num] = {winner};

    // pick new candidates for the losers
    for (size_t s : slots) {
      if (s == winner) {
        continue;
      }

      selected_ops[s] = nullptr;
      Op* op = pick_next(s);
      selected_ops[s] = op;
      if (op == nullptr) {
        continue;
      }

      auto& vec = owner[op->op_num];
      vec.push_back(s);

      // new selection may result in new conflicts
      if (vec.size() > 1) {
        conflict_ops.insert(op->op_num);
      }
    }
  }

  // issue the selected ops to their respective FUs
  for (size_t i = 0; i < connected_fus.size(); ++i) {
    Op* op = selected_ops[i];
    if (op == nullptr) {
      continue;
    }

    Func_Unit* fu = &exec->fus[connected_fus[i]];
    issue_queue_pick_op(op, fu->fu_id);
  }

  // move the circular queue idx to the next position for the next selection
  fu_idx = (fu_idx + 1) % connected_fus.size();
}

/**************************************************************************************/

class IssueQueue {
 private:
  const uns proc_id;
  const uns16 queue_id;

  std::vector<IssueQueueEntry> entries;
  std::deque<uns16> free_list;
  std::vector<int> connected_fus;
  std::shared_ptr<SelectLogic> select_logic;

  void init_connected_fus();
  void init_select_logic();

 public:
  explicit IssueQueue(uns proc_id, uns16 queue_id, uns16 size);
  uns16 allocate_entry(Op* op);
  void free_entry(uns16 entry_id);

  size_t available_entries() const { return free_list.size(); }
  const std::vector<IssueQueueEntry>& get_entries() const { return entries; }
  const std::vector<int>& get_connected_fus() const { return connected_fus; }

  void wakeup(uns16 entry_id);
  void grant();
  void recover();
  void schedule();
};

IssueQueue::IssueQueue(uns proc_id, uns16 queue_id, uns16 size) : proc_id(proc_id), queue_id(queue_id) {
  // initialize entries and free list
  entries.reserve(size);
  for (uns16 i = 0; i < size; ++i) {
    entries.emplace_back(queue_id, i);
    free_list.push_back(i);
  }
  DEBUG(proc_id, "Initializing Issue Queue %d with size %d\n", queue_id, size);

  init_connected_fus();
  init_select_logic();
}

// initialize connected FUs based on configuration
void IssueQueue::init_connected_fus() {
  // parse the FU mask for this issue queue
  std::string s = RS_CONNECTIONS;
  size_t pos = 0;
  std::string token;
  for (uns16 i = 0; i <= queue_id; ++i) {
    size_t next = s.find(' ', pos);
    token = (next == std::string::npos) ? s.substr(pos) : s.substr(pos, next - pos);

    ASSERT(proc_id, !token.empty());
    ASSERT(proc_id, next != std::string::npos || i == queue_id);
    pos = (next == std::string::npos) ? s.size() : next + 1;
  }

  // transform the token into a FU mask
  auto parse_mask = [](const std::string& s) -> uns64 {
    switch (s.front()) {
      case 'x':
        return static_cast<uns64>(std::stoull(s.substr(1), nullptr, 16));

      case 'b':
        return static_cast<uns64>(std::stoull(s.substr(1), nullptr, 2));

      default:
        return static_cast<uns64>(std::stoull(s, nullptr, 10));
    }
  };
  ASSERT(proc_id, !token.empty());
  uns64 mask = parse_mask(token);
  DEBUG(proc_id, "Issue Queue %d connected FU mask: 0x%llx\n", queue_id, mask);

  // determine the connected FUs based on the mask
  while (mask) {
    int idx = __builtin_ctzll(mask);
    connected_fus.push_back(idx);
    mask &= (mask - 1);
  }
}

// initialize select logic based on scheduling scheme
void IssueQueue::init_select_logic() {
  select_logic = std::make_shared<ParallelOldestFirstSelectLogic>(queue_id, connected_fus);
}

uns16 IssueQueue::allocate_entry(Op* op) {
  // get an available entry from the free list
  ASSERT(proc_id, !free_list.empty());
  uns16 entry_id = free_list.front();
  free_list.pop_front();

  // fill the op info into the entry
  IssueQueueEntry& entry = entries[entry_id];
  ASSERT(proc_id, entry.state == ISSUE_QUEUE_ENTRY_STATE_EMPTY);
  entry.fill(op);
  entry.state = ISSUE_QUEUE_ENTRY_STATE_ALLOC;

  return entry_id;
}

void IssueQueue::free_entry(uns16 entry_id) {
  IssueQueueEntry& entry = entries[entry_id];
  ASSERT(proc_id, entry.state != ISSUE_QUEUE_ENTRY_STATE_EMPTY);

  entry.clear();
  entry.state = ISSUE_QUEUE_ENTRY_STATE_EMPTY;
  free_list.push_back(entry_id);
}

void IssueQueue::wakeup(uns16 entry_id) {
  ASSERT(proc_id, entry_id < entries.size());
  IssueQueueEntry& entry = entries[entry_id];
  ASSERT(proc_id, entry.state == ISSUE_QUEUE_ENTRY_STATE_ALLOC);

  Op* op = entry.op;
  ASSERT(proc_id, op != nullptr);

  entry.state = ISSUE_QUEUE_ENTRY_STATE_READY;
  select_logic->request(op);
}

void IssueQueue::grant() {
  // release the entries of ops that are scheduled
  for (IssueQueueEntry& entry : entries) {
    if (entry.state == ISSUE_QUEUE_ENTRY_STATE_EMPTY) {
      continue;
    }

    Op* op = entry.op;
    ASSERT(proc_id, op != nullptr);
    if (op->state != OS_SCHEDULED && op->state != OS_MISS) {
      continue;
    }

    STAT_EVENT(node->proc_id, OP_ISSUED);
    select_logic->release(op);
    free_entry(entry.entry_id);
  }
}

void IssueQueue::recover() {
  for (IssueQueueEntry& entry : entries) {
    Op* op = entry.op;
    if (op == nullptr || !op->off_path) {
      continue;
    }

    select_logic->release(op);
    free_entry(entry.entry_id);
  }
}

void IssueQueue::schedule() {
  select_logic->select();
}

/**************************************************************************************/

class IssueQueues {
 private:
  const uns proc_id;
  std::vector<IssueQueue> issue_queues;

 public:
  explicit IssueQueues(uns proc_id);
  const std::vector<IssueQueue>& get_issue_queues() const { return issue_queues; }

  void grant();
  void dispatch();
  void schedule();
  void recover();
  void wakeup(Op* op);
};

IssueQueues::IssueQueues(uns proc_id) : proc_id(proc_id) {
  // create issue queues based on configuration
  issue_queues.reserve(NUM_RS);
  const char* p = RS_SIZES;
  for (uns16 i = 0; i < NUM_RS; ++i) {
    char* end = nullptr;
    uns64 size = strtoull(p, &end, 10);
    issue_queues.emplace_back(proc_id, i, size);
    p = end;
  }
}

/*
 * Remove scheduled ops (i.e., going from RS to FUs) from the RS and ready queue
 */
void IssueQueues::grant() {
  for (IssueQueue& queue : issue_queues) {
    queue.grant();
  }
}

/*
 * Fill the scheduling window (RS) with oldest available ops.
 * For each available op:
 *  - Allocate it to its designated reservation station.
 *  - If all source operands are ready, insert it into the ready list.
 */
void IssueQueues::dispatch() {
  Op* op = NULL;
  uns32 num_fill_rs = 0;

  for (op = node->next_op_into_rs; op; op = op->next_node) {
    ASSERT(proc_id, op->queue_id == MAX_UNS16 && op->queue_entry_id == MAX_UNS16);
    uns16 queue_id = issue_queue_dispatch_find_emptiest(op);
    if (queue_id == MAX_UNS16) {
      break;
    }

    IssueQueue& queue = issue_queues[queue_id];
    op->queue_id = queue_id;
    ASSERT(proc_id, queue.available_entries() > 0);

    uns16 entry_id = queue.allocate_entry(op);
    ASSERT(proc_id, op->queue_entry_id == MAX_UNS16);
    op->queue_entry_id = entry_id;

    ASSERT(node->proc_id, op->state == OS_IN_ROB);
    op->state = OS_IN_RS;

    if (op_sources_not_rdy_is_clear(op)) {
      queue.wakeup(entry_id);
      op->state = (cycle_count + 1 >= op->rdy_cycle ? OS_READY : OS_WAIT_FWD);
    }

    // maximum number of operations to fill into the RS per cycle (0 = unlimited)
    if (RS_FILL_WIDTH && (num_fill_rs == RS_FILL_WIDTH)) {
      op = op->next_node;
      break;
    }
  }

  // mark the next node to continue filling in the next cycle
  node->next_op_into_rs = op;
}

/*
 * Schedule ready ops (ops that are currently in the ready list).
 *
 * Input:  ready_list, containing all ops that are ready to issue from each of the RSs.
 * Output: node->sd, containing ops thats being passed to the FUs.
 *
 * If a functional unit is available, it will accept the scheduled op,
 * which is then removed from the ready list. If no FU is available, the
 * op remains in the ready list to be considered in the next
 * scheduling cycle.
 */
void IssueQueues::schedule() {
  // the next stage is supposed to clear them out
  ASSERT(node->proc_id, node->sd.op_count == 0);

  // checks if any of the L1 MSHRs have become available
  issue_queue_update_mem_block();

  // for each issue queue, select ready ops to issue based on the scheduling scheme and FU availability
  for (IssueQueue& queue : issue_queues) {
    queue.schedule();
  }
}

void IssueQueues::recover() {
  for (IssueQueue& queue : issue_queues) {
    queue.recover();
  }
}

void IssueQueues::wakeup(Op* op) {
  uns16 queue_id = op->queue_id;
  ASSERT(proc_id, queue_id < issue_queues.size());
  IssueQueue& queue = issue_queues[queue_id];
  ASSERT(proc_id, op->queue_entry_id != MAX_UNS16);
  queue.wakeup(op->queue_entry_id);
}

/**************************************************************************************/
/* Global Values */

static std::vector<IssueQueues> per_core_issue_queues;
IssueQueues* issue_queues = nullptr;

/**************************************************************************************/
/* Inline Functions */

static inline Flag issue_queue_check_op_ready(Op* op) {
  ASSERT(node->proc_id, op != nullptr);

  // if the op is waiting for memory, check if it is still blocked by memory. If not, it becomes ready
  if (op->state == OS_WAIT_MEM) {
    if (node->mem_blocked) {
      return FALSE;
    }
    op->state = OS_READY;
  }

  if (op->state == OS_TENTATIVE || op->state == OS_WAIT_DCACHE) {
    return FALSE;
  }
  ASSERT(node->proc_id, op->state == OS_IN_RS || op->state == OS_READY || op->state == OS_WAIT_FWD);

  // if the op is waiting for forwarding, check if it can be forwarded in time to be ready in the next cycle
  if (cycle_count < op->rdy_cycle - 1) {
    return FALSE;
  }
  ASSERT(node->proc_id, op_sources_not_rdy_is_clear(op));

  return TRUE;
}

static inline Flag issue_queue_check_fu_available(Op* op, Func_Unit* fu) {
  if (!(get_fu_type(op->inst_info->table_info.op_type, op->inst_info->table_info.is_simd) & fu->type)) {
    return FALSE;
  }

  if (node->sd.ops[fu->fu_id]) {
    return FALSE;
  }

  return TRUE;
}

static inline void issue_queue_pick_op(Op* op, uns32 fu_id) {
  ASSERT(node->proc_id, fu_id < (uns32)node->sd.max_op_count);
  ASSERT(node->proc_id, node->sd.ops[fu_id] == nullptr);
  op->fu_num = fu_id;
  node->sd.ops[op->fu_num] = op;
  node->last_scheduled_opnum = op->op_num;
  node->sd.op_count += 1;
  ASSERT(node->proc_id, node->sd.op_count <= node->sd.max_op_count);
}

// Memory is blocked when there are no more MSHRs in the L1 Q (i.e., there is no way to handle a D-Cache miss)
static inline void issue_queue_update_mem_block() {
  // if we are stalled due to lack of MSHRs to the L1, check to see if there is space now
  if (node->mem_blocked && mem_can_allocate_req_buffer(node->proc_id, MRT_DFETCH, FALSE)) {
    node->mem_blocked = FALSE;
    STAT_EVENT(node->proc_id, MEM_BLOCK_LENGTH_0 + MIN2(node->mem_block_length, 5000) / 100);
    node->mem_block_length = 0;
  }

  INC_STAT_EVENT(node->proc_id, CORE_MEM_BLOCKED, node->mem_blocked);
  node->mem_block_length += node->mem_blocked;
}

static inline void issue_queue_track_stats() {
  // TODO: track issue queue fu idle stats here
}

/**************************************************************************************/

uns16 issue_queue_dispatch_find_emptiest(Op* op) {
  uns16 emptiest_queue_id = MAX_UNS16;
  size_t emptiest_queue_entries = 0;

  for (size_t queue_id = 0; queue_id < issue_queues->get_issue_queues().size(); ++queue_id) {
    const IssueQueue& queue = issue_queues->get_issue_queues()[queue_id];
    for (size_t i = 0; i < queue.get_connected_fus().size(); ++i) {
      // find the FU that can execute this op
      Func_Unit* fu = &exec->fus[queue.get_connected_fus()[i]];
      if (!(get_fu_type(op->inst_info->table_info.op_type, op->inst_info->table_info.is_simd) & fu->type)) {
        continue;
      }

      size_t empty_entries = queue.available_entries();
      if (empty_entries == 0) {
        continue;
      }

      if (emptiest_queue_entries < empty_entries) {
        emptiest_queue_id = queue_id;
        emptiest_queue_entries = empty_entries;
      }
    }
  }

  return emptiest_queue_id;
}

/**************************************************************************************/
/* External Function */

void issue_queue_update() {
  // remove scheduled ops from the ready list
  issue_queues->grant();

  // dispatch ops from the reorder buffer to the corresponding issue queues
  issue_queues->dispatch();

  // schedule ready ops in the issue queue to the functional units
  issue_queues->schedule();
}

void issue_queue_wakeup(Op* op) {
  issue_queues->wakeup(op);
}

/**************************************************************************************/
/* Vanilla API */

void alloc_mem_issue_queue(uns num_cores) {
  per_core_issue_queues.reserve(num_cores);
  for (uns ii = 0; ii < num_cores; ii++) {
    per_core_issue_queues.emplace_back(ii);
  }
}

void set_issue_queue(uns8 proc_id) {
  issue_queues = &per_core_issue_queues[proc_id];
}

void recover_issue_queue() {
  ASSERT(node->proc_id, issue_queues != nullptr);
  issue_queues->recover();
}
