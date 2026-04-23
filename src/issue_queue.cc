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

struct FunctionalUnitPicker {
  const uns proc_id;
  const uns16 queue_id;
  const uns32 fu_id;
  const uns64 fu_type;
  using ReadyIter = std::map<Counter, Op*>::iterator;

  explicit FunctionalUnitPicker(uns proc_id, uns16 queue_id, uns32 fu_id, uns64 fu_type)
      : proc_id(proc_id), queue_id(queue_id), fu_id(fu_id), fu_type(fu_type) {}

  Flag check_op_ready(Op* op) const;
  Op* pick(ReadyIter& it, const std::map<Counter, Op*>& ready_list,
           const std::unordered_set<Counter>& picked_mask) const;
  void issue_op_into_sd(Op* op);
};

Flag FunctionalUnitPicker::check_op_ready(Op* op) const {
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

Op* FunctionalUnitPicker::pick(ReadyIter& it, const std::map<Counter, Op*>& ready_list,
                               const std::unordered_set<Counter>& picked_mask) const {
  while (it != ready_list.end()) {
    Op* op = it->second;
    ++it;

    // skip if the op has been picked by other FUs
    if (picked_mask.count(op->op_num)) {
      continue;
    }
    ASSERT(proc_id, node->sd.ops[fu_id] == nullptr);

    // check if the op is ready (it may become not ready due to memory blocking or waiting for forwarding)
    if (!check_op_ready(op)) {
      continue;
    }

    // check if the op can be issued to this FU based on its type
    if (!(get_fu_type(op->inst_info->table_info.op_type, op->inst_info->table_info.is_simd) & fu_type)) {
      continue;
    }

    return op;
  }

  return nullptr;
}

void FunctionalUnitPicker::issue_op_into_sd(Op* op) {
  ASSERT(proc_id, fu_id < (uns32)node->sd.max_op_count && node->sd.ops[fu_id] == nullptr);
  ASSERT(proc_id, node->sd.op_count < node->sd.max_op_count);

  op->fu_num = fu_id;
  node->sd.ops[op->fu_num] = op;
  node->last_scheduled_opnum = op->op_num;
  node->sd.op_count += 1;
}

/**************************************************************************************/
/*
 * The select logic is responsible for picking ready ops from the wakeup logic in the issue queue.
 * Since the issue queue is organized in a non-ordered (random) manner, the select logic typically relies
 * on an age matrix to track the relative age of entries.
 */

class SelectLogic {
 public:
  virtual void request(Op* op) = 0;
  virtual void release(Op* op) = 0;
  virtual void select() = 0;
};

class OldestFirstSelectLogic : public SelectLogic {
 private:
  const uns queue_id;
  std::vector<FunctionalUnitPicker> connected_fu_pickers;
  std::vector<std::map<Counter, Op*>> ready_lists;  // age ordered ready list of ops for each connected FU
  int fu_idx = 0;                                   // circular queue idx for round-robin selection

  void serial_select();
  void parallel_select();

 public:
  OldestFirstSelectLogic(uns queue_id, const std::vector<FunctionalUnitPicker>& connected_fu_pickers)
      : queue_id(queue_id), connected_fu_pickers(connected_fu_pickers) {
    ready_lists.resize(connected_fu_pickers.size());
  }
  void request(Op* op) override;
  void release(Op* op) override;
  void select() override;
};

void OldestFirstSelectLogic::request(Op* op) {
  op->in_rdy_list = TRUE;
  for (size_t i = 0; i < connected_fu_pickers.size(); ++i) {
    if (get_fu_type(op->inst_info->table_info.op_type, op->inst_info->table_info.is_simd) &
        connected_fu_pickers[i].fu_type) {
      ready_lists[i][op->op_num] = op;
    }
  }
}

void OldestFirstSelectLogic::release(Op* op) {
  op->in_rdy_list = FALSE;
  for (size_t i = 0; i < connected_fu_pickers.size(); ++i) {
    ready_lists[i].erase(op->op_num);
  }
}

void OldestFirstSelectLogic::select() {
  // serial_select();
  parallel_select();
}

/*
 * Serial selection is described in "Quantifying the complexity of superscalar processors", 1996.
 * The select logic consists of multiple picking blocks arranged in series and each block receives
 * request signals that are derived from the requests of the preceding block.
 */
void OldestFirstSelectLogic::serial_select() {
  std::unordered_set<Counter> picked_mask;

  for (uns32 i = 0; i < connected_fu_pickers.size(); ++i) {
    uns32 idx = (fu_idx + i) % connected_fu_pickers.size();
    FunctionalUnitPicker& fu_picker = connected_fu_pickers[idx];

    FunctionalUnitPicker::ReadyIter it = ready_lists[idx].begin();
    Op* op = fu_picker.pick(it, ready_lists[idx], picked_mask);
    if (op == nullptr) {
      continue;
    }

    fu_picker.issue_op_into_sd(op);
    picked_mask.insert(op->op_num);
  }

  // move the circular queue idx for the next selection
  fu_idx = (fu_idx + 1) % connected_fu_pickers.size();
}

/*
 * Parallel selection allows each functional unit to independently select a ready op in the same cycle.
 * An arbitrator is then used to resolve conflicts when multiple functional units select the same op.
 */
void OldestFirstSelectLogic::parallel_select() {
  std::vector<FunctionalUnitPicker::ReadyIter> cur_iter(connected_fu_pickers.size());
  for (size_t i = 0; i < connected_fu_pickers.size(); ++i) {
    cur_iter[i] = ready_lists[i].begin();
  }

  std::vector<Op*> selected_ops(connected_fu_pickers.size(), nullptr);
  std::unordered_map<Counter, std::vector<size_t>> owner;
  std::unordered_set<Counter> conflict_ops;
  std::unordered_set<Counter> picked_mask;
  auto try_pick = [&](size_t i) {
    FunctionalUnitPicker& fu_picker = connected_fu_pickers[i];
    Op* op = fu_picker.pick(cur_iter[i], ready_lists[i], picked_mask);
    if (op == nullptr) {
      return;
    }

    selected_ops[i] = op;
    picked_mask.insert(op->op_num);

    auto& v = owner[op->op_num];
    v.push_back(i);
    if (v.size() > 1) {
      conflict_ops.insert(op->op_num);
    }
  };

  // parallel pick an op for each connected FU
  for (size_t i = 0; i < connected_fu_pickers.size(); ++i) {
    try_pick(i);
  }

  // arbitration among the selected ops if there are conflicts
  auto rr_rank = [&](size_t slot) {
    return (slot + connected_fu_pickers.size() - fu_idx) % connected_fu_pickers.size();
  };
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
      try_pick(s);
    }
  }

  // issue the selected ops to their respective FUs
  for (size_t i = 0; i < connected_fu_pickers.size(); ++i) {
    Op* op = selected_ops[i];
    if (op == nullptr) {
      continue;
    }

    connected_fu_pickers[i].issue_op_into_sd(op);
    picked_mask.insert(op->op_num);
  }

  // move the circular queue idx to the next position for the next selection
  fu_idx = (fu_idx + 1) % connected_fu_pickers.size();
}

/**************************************************************************************/
/*
 * The issue queue is organized as a random queue, where instructions are dispatched into free entries
 * without any particular ordering.
 */

class IssueQueue {
 private:
  const uns proc_id;
  const uns16 queue_id;

  std::vector<IssueQueueEntry> entries;
  std::deque<uns16> free_list;
  std::shared_ptr<SelectLogic> select_logic;

 public:
  explicit IssueQueue(uns proc_id, uns16 queue_id, uns16 size,
                      const std::vector<FunctionalUnitPicker>& connected_fu_pickers);
  uns16 allocate_entry(Op* op);
  void free_entry(uns16 entry_id);

  size_t available_entries() const { return free_list.size(); }
  const std::vector<IssueQueueEntry>& get_entries() const { return entries; }

  void wakeup(uns16 entry_id);
  void grant();
  void recover();
  void schedule();
};

IssueQueue::IssueQueue(uns proc_id, uns16 queue_id, uns16 size,
                       const std::vector<FunctionalUnitPicker>& connected_fu_pickers)
    : proc_id(proc_id), queue_id(queue_id) {
  // initialize entries and free list
  entries.reserve(size);
  for (uns16 i = 0; i < size; ++i) {
    entries.emplace_back(queue_id, i);
    free_list.push_back(i);
  }
  DEBUG(proc_id, "Initializing Issue Queue %d with size %d\n", queue_id, size);

  // initialize select logic based on scheduling scheme
  select_logic = std::make_shared<OldestFirstSelectLogic>(queue_id, connected_fu_pickers);
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
  std::vector<uns16> fu_map;
  std::vector<uns64> fu_types;

  void update_mem_block();
  void track_stats();
  uns16 find_emptiest_queue(Op* op);

 public:
  explicit IssueQueues(uns proc_id);

  void grant();
  void dispatch();
  void schedule();
  void recover();
  void wakeup(Op* op);
};

IssueQueues::IssueQueues(uns proc_id) : proc_id(proc_id) {
  auto split_tokens = [](const std::string& s) {
    std::vector<std::string> tokens;
    for (size_t pos = 0; pos < s.size();) {
      size_t next = s.find(' ', pos);
      tokens.emplace_back(s.substr(pos, next == std::string::npos ? s.size() - pos : next - pos));
      if (next == std::string::npos)
        break;
      pos = next + 1;
    }
    return tokens;
  };
  auto parse_mask = [](const std::string& s) -> uns64 {
    ASSERT(0, !s.empty());
    switch (s.front()) {
      case 'x':
        return static_cast<uns64>(std::stoull(s.substr(1), nullptr, 16));
      case 'b':
        return static_cast<uns64>(std::stoull(s.substr(1), nullptr, 2));
      default:
        return static_cast<uns64>(std::stoull(s, nullptr, 10));
    }
  };

  // initialize connection map from FUs to issue queues
  std::vector<std::string> connection_tokens = split_tokens(RS_CONNECTIONS);
  ASSERT(proc_id, connection_tokens.size() == NUM_RS);
  fu_map.assign(NUM_FUS, MAX_UNS16);
  for (size_t i = 0; i < NUM_RS; ++i) {
    for (uns64 mask = parse_mask(connection_tokens[i]); mask; mask &= (mask - 1)) {
      int idx = __builtin_ctzll(mask);
      ASSERT(proc_id, idx < (int)NUM_FUS);
      fu_map[idx] = i;
    }
  }

  // initialize the connected FUs for each issue queue
  std::vector<std::string> fu_tokens = split_tokens(FU_TYPES);
  ASSERT(proc_id, fu_tokens.size() == NUM_FUS);
  std::vector<std::vector<FunctionalUnitPicker>> fu_connection(NUM_RS);
  fu_types.assign(NUM_RS, 0);
  for (size_t i = 0; i < NUM_FUS; ++i) {
    uns64 fu_type = parse_mask(fu_tokens[i]);
    fu_connection[fu_map[i]].emplace_back(proc_id, fu_map[i], i, fu_type);
    fu_types[fu_map[i]] |= fu_type;
    DEBUG(proc_id, "FU %ld, queue: %d, type: 0x%llx\n", i, fu_map[i], fu_type);
  }

  // create issue queues based on configuration
  issue_queues.reserve(NUM_RS);
  const char* p = RS_SIZES;
  for (uns16 i = 0; i < NUM_RS; ++i) {
    char* end = nullptr;
    uns64 size = strtoull(p, &end, 10);
    issue_queues.emplace_back(proc_id, i, size, fu_connection[i]);
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
    uns16 queue_id = find_emptiest_queue(op);
    if (queue_id == MAX_UNS16) {
      break;
    }

    IssueQueue& queue = issue_queues[queue_id];
    ASSERT(proc_id, queue.available_entries() > 0);
    op->queue_id = queue_id;
    op->queue_entry_id = queue.allocate_entry(op);
    ;

    ASSERT(node->proc_id, op->state == OS_IN_ROB);
    op->state = OS_IN_RS;

    if (op_sources_not_rdy_is_clear(op)) {
      queue.wakeup(op->queue_entry_id);
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
  update_mem_block();

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

// Memory is blocked when there are no more MSHRs in the L1 Q (i.e., there is no way to handle a D-Cache miss)
void IssueQueues::update_mem_block() {
  // if we are stalled due to lack of MSHRs to the L1, check to see if there is space now
  if (node->mem_blocked && mem_can_allocate_req_buffer(node->proc_id, MRT_DFETCH, FALSE)) {
    node->mem_blocked = FALSE;
    STAT_EVENT(node->proc_id, MEM_BLOCK_LENGTH_0 + MIN2(node->mem_block_length, 5000) / 100);
    node->mem_block_length = 0;
  }

  INC_STAT_EVENT(node->proc_id, CORE_MEM_BLOCKED, node->mem_blocked);
  node->mem_block_length += node->mem_blocked;
}

void IssueQueues::track_stats() {
  // TODO: track issue queue fu idle stats here
}

uns16 IssueQueues::find_emptiest_queue(Op* op) {
  uns16 emptiest_queue_id = MAX_UNS16;
  size_t emptiest_queue_entries = 0;

  uns64 op_fu_type = get_fu_type(op->inst_info->table_info.op_type, op->inst_info->table_info.is_simd);
  for (size_t queue_id = 0; queue_id < issue_queues.size(); ++queue_id) {
    const IssueQueue& queue = issue_queues[queue_id];
    uns64 queue_fu_types = fu_types[queue_id];
    if (!(op_fu_type & queue_fu_types)) {
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

  return emptiest_queue_id;
}

/**************************************************************************************/
/* Global Values */

static std::vector<IssueQueues> per_core_issue_queues;
IssueQueues* issue_queues = nullptr;

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
