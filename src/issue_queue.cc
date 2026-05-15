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

#include "core.param.h"

#include "bp/bp.h"
#include "memory/memory.h"

#include "exec_ports.h"
#include "exec_stage.h"
#include "map_rename.h"
#include "node_stage.h"
}

#include <deque>
#include <list>
#include <memory>
#include <string>
#include <vector>

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_NODE_STAGE, ##args)

/**************************************************************************************/
/* Constexpr */

enum ISSUE_QUEUE_ENTRY_STATE {
  ISSUE_QUEUE_ENTRY_STATE_EMPTY,
  ISSUE_QUEUE_ENTRY_STATE_ALLOCATED,
  ISSUE_QUEUE_ENTRY_STATE_READY,
  ISSUE_QUEUE_ENTRY_STATE_PICKED,
  ISSUE_QUEUE_ENTRY_STATE_NUM
};

/**************************************************************************************/
/* Static methods */

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

/**************************************************************************************/
/* Structures */

struct IssueQueueEntry {
  const uns16 queue_id;
  const uns16 entry_id;

  Op* op = nullptr;
  uns64 op_fu_type = 0;
  ISSUE_QUEUE_ENTRY_STATE state = ISSUE_QUEUE_ENTRY_STATE_EMPTY;

  explicit IssueQueueEntry(uns16 queue_id, uns16 entry_id) : queue_id(queue_id), entry_id(entry_id) {}
  void clear();
  void fill(Op* op);
};

void IssueQueueEntry::clear() {
  op = nullptr;
  op_fu_type = 0;
}

void IssueQueueEntry::fill(Op* op) {
  this->op = op;
  op_fu_type = get_fu_type(op->inst_info->table_info.op_type, op->inst_info->table_info.is_simd);
}

/**************************************************************************************/
/* Virtual Interface */

/*
 * SchedulePolicy defines the relative priority between ready ops during selection. When
 * multiple ops compete for the same picker, the policy determines which op is preferred.
 *
 * Typical scheduling policies include oldest-first, criticality-based, and dependency-aware
 * prioritization.
 */
class SchedulePolicy {
 public:
  virtual ~SchedulePolicy() = default;

  // Return if the lhs entry is the preference
  virtual bool compare(const IssueQueueEntry* lhs, const IssueQueueEntry* rhs) const = 0;
};

/*
 * ArbitrationPolicy determines the picker traversal order, where pickers earlier in the
 * order have higher priority.
 *
 * In serial selection, requests for later pickers are derived from the selections made
 * by earlier pickers.
 *
 * In parallel selection, earlier pickers win arbitration and cancel conflicting selections
 * from later pickers.
 */
class ArbitrationPolicy {
 public:
  virtual ~ArbitrationPolicy() = default;

  // Prepare the picker order for the current selection cycle
  virtual void build_picker_order(std::vector<size_t>& picker_order) = 0;
};

/**************************************************************************************/

/*
 * FunctionalUnitPicker selects a single op for a functional unit during the current
 * scheduling cycle.
 *
 * Each picker tracks the currently selected entry and updates the selection according to
 * the scheduling policy when competing ops are observed.
 *
 * In serial selection, a newly selected op may displace the current selection and forward
 * the displaced request to later pickers.
 *
 * In parallel selection, selections are resolved during grant.
 */
class FunctionalUnitPicker {
 private:
  const uns proc_id;
  const uns16 queue_id;
  const uns32 fu_id;
  const uns64 fu_type;

  std::unique_ptr<SchedulePolicy> sched_policy;

  // current selection for this cycle
  IssueQueueEntry* picked_entry = nullptr;

 public:
  explicit FunctionalUnitPicker(uns proc_id, uns16 queue_id, uns32 fu_id, uns64 fu_type,
                                std::unique_ptr<SchedulePolicy> sched_policy)
      : proc_id(proc_id), queue_id(queue_id), fu_id(fu_id), fu_type(fu_type), sched_policy(std::move(sched_policy)) {
    // queue_id is reserved for upcoming use; reference it to silence -Wunused-private-field
    (void)this->queue_id;
  }

  void pick(IssueQueueEntry*& candidate);
  void grant();
  bool is_compatible(uns64 op_fu_type) const;

  uns32 get_fu_id() const { return fu_id; }
};

/*
 * If the incoming request has higher priority than the current
 * selection, it replaces the existing selection according to the
 * scheduling policy.
 */
void FunctionalUnitPicker::pick(IssueQueueEntry*& request_entry) {
  if (!(picked_entry == nullptr || sched_policy->compare(request_entry, picked_entry))) {
    return;
  }

  // replace the current selection
  IssueQueueEntry* displaced_entry = picked_entry;
  picked_entry = request_entry;

  // forward the displaced request to later pickers
  if (!ISSUE_QUEUE_PARALLEL_PICK) {
    request_entry = displaced_entry;
  }
}

void FunctionalUnitPicker::grant() {
  // no valid selection this cycle
  if (picked_entry == nullptr) {
    return;
  }

  ASSERT(proc_id, ISSUE_QUEUE_PARALLEL_PICK || picked_entry->state == ISSUE_QUEUE_ENTRY_STATE_READY);
  // another picker has already granted this entry
  if (picked_entry->state == ISSUE_QUEUE_ENTRY_STATE_PICKED) {
    picked_entry = nullptr;
    return;
  }

  Op* op = picked_entry->op;
  ASSERT(proc_id, op != nullptr);
  ASSERT(proc_id, fu_id < (uns32)node->sd.max_op_count);
  op->fu_num = fu_id;

  // issue the op into the stage data
  ASSERT(proc_id, node->sd.ops[fu_id] == nullptr && node->sd.op_count < node->sd.max_op_count);
  node->sd.ops[op->fu_num] = op;
  node->last_scheduled_opnum = op->op_num;
  node->sd.op_count += 1;

  picked_entry->state = ISSUE_QUEUE_ENTRY_STATE_PICKED;
  picked_entry = nullptr;
}

bool FunctionalUnitPicker::is_compatible(uns64 op_fu_type) const {
  return op_fu_type & fu_type;
}

/**************************************************************************************/
/*
 * The select logic is responsible for choosing ready ops for issue.
 *
 * It combines a SchedulePolicy, which defines the relative priority between competing ops,
 * and an ArbitrationPolicy, which determines the priority of pickers during selection.
 */

class SelectLogic {
 protected:
  const uns proc_id;
  const uns16 queue_id;

  std::vector<FunctionalUnitPicker> connected_fu_pickers;
  std::unique_ptr<ArbitrationPolicy> arbitration_policy;

  std::vector<size_t> picker_order;  // picker traversal order generated each cycle
  std::list<IssueQueueEntry*> ready_list;
  uns64 ready_op_types = 0;  // bitmask of ready op types in this cycle

 public:
  explicit SelectLogic(uns proc_id, uns16 queue_id, std::vector<FunctionalUnitPicker> connected_fu_pickers,
                       std::unique_ptr<ArbitrationPolicy> arbitration_policy)
      : proc_id(proc_id),
        queue_id(queue_id),
        connected_fu_pickers(std::move(connected_fu_pickers)),
        arbitration_policy(std::move(arbitration_policy)),
        picker_order(this->connected_fu_pickers.size()) {}

  void select();
  void request(IssueQueueEntry* entry);
  void release(IssueQueueEntry* entry);
  bool has_ready_ops() const;
};

/*
 * Serial selection is described in "Quantifying the complexity of superscalar processors", 1996.
 * The select logic consists of multiple picking blocks arranged in series and each block receives
 * request signals that are derived from the requests of the preceding block.
 *
 * Parallel selection allows each functional unit to independently select a ready op in the same cycle.
 * An arbitrator is then used to resolve conflicts when multiple functional units select the same op.
 */
void SelectLogic::select() {
  const size_t fu_num = connected_fu_pickers.size();
  arbitration_policy->build_picker_order(picker_order);
  ready_op_types = 0;

  for (IssueQueueEntry* entry : ready_list) {
    // check if the op is ready (it may become not ready due to memory blocking or waiting for forwarding)
    if (entry->state != ISSUE_QUEUE_ENTRY_STATE_READY || !issue_queue_check_op_ready(entry->op)) {
      continue;
    }

    // track the ready op types for stat collection
    ready_op_types |= entry->op_fu_type;

    // the current request propagated through the serial picker chain
    IssueQueueEntry* request_entry = entry;

    for (size_t i = 0; i < fu_num; ++i) {
      // traverse pickers in arbitration order
      size_t picker_idx = picker_order[i];
      FunctionalUnitPicker& fu_picker = connected_fu_pickers[picker_idx];

      if (!fu_picker.is_compatible(request_entry->op_fu_type)) {
        continue;
      }
      fu_picker.pick(request_entry);

      // the request has been consumed
      if (request_entry == nullptr) {
        break;
      }
    }

    // the entry is not picked or displaced
    if (request_entry != nullptr) {
      STAT_EVENT(node->proc_id, RS_OP_READY_NOT_ISSUED_TOTAL);
      STAT_EVENT(node->proc_id, RS_0_OP_READY_NOT_ISSUED + (queue_id < 8 ? queue_id : 8));
    }
  }

  for (size_t i = 0; i < fu_num; ++i) {
    // grant the pick after scanning the ready list
    FunctionalUnitPicker& fu_picker = connected_fu_pickers[picker_order[i]];
    fu_picker.grant();

    // track FU idle stats after scheduling
    uns32 fu_id = fu_picker.get_fu_id();
    if (node->sd.ops[fu_id] != NULL)
      continue;

    Func_Unit* fu = &exec->fus[fu_id];
    if (fu->avail_cycle > cycle_count || fu->held_by_mem)
      continue;

    if ((ready_op_types & fu->type) == 0) {
      STAT_EVENT(node->proc_id, FU_IDLE_NO_READY_OPS_TOTAL);
      STAT_EVENT(node->proc_id, FU_0_IDLE_NO_READY_OPS + (fu_id < 32 ? fu_id : 32));
    }
  }
}

void SelectLogic::request(IssueQueueEntry* entry) {
  ASSERT(node->proc_id, entry != nullptr);
  ready_list.push_front(entry);
}

void SelectLogic::release(IssueQueueEntry* entry) {
  ASSERT(node->proc_id, entry != nullptr);
  ready_list.remove(entry);
}

bool SelectLogic::has_ready_ops() const {
  return !ready_list.empty();
}

/**************************************************************************************/
/* Implementation */

// OldestFirstSchedulePolicy prioritize the older ops
class OldestFirstSchedulePolicy : public SchedulePolicy {
 public:
  bool compare(const IssueQueueEntry* lhs, const IssueQueueEntry* rhs) const override;
};

bool OldestFirstSchedulePolicy::compare(const IssueQueueEntry* lhs, const IssueQueueEntry* rhs) const {
  ASSERT(node->proc_id, lhs != nullptr && rhs != nullptr);
  return lhs->op->op_num < rhs->op->op_num;
}

// RoundRobinArbitrationPolicy ensures fairness by rotating through pickers in a circular manner.
class RoundRobinArbitrationPolicy : public ArbitrationPolicy {
 private:
  const size_t fu_num;
  int fu_idx = 0;  // circular queue idx for round-robin selection

 public:
  explicit RoundRobinArbitrationPolicy(size_t fu_num) : fu_num(fu_num), fu_idx(fu_num - 1) {}
  void build_picker_order(std::vector<size_t>& picker_order) override;
};

void RoundRobinArbitrationPolicy::build_picker_order(std::vector<size_t>& picker_order) {
  ASSERT(node->proc_id, picker_order.size() == fu_num);
  fu_idx = (fu_idx + 1) % fu_num;
  for (size_t i = 0; i < fu_num; ++i) {
    picker_order[i] = (fu_idx + i) % fu_num;
  }
}

/**************************************************************************************/
/* Factory */
// TODO: abstract factory

class IssueQueuePolicyFactory {
 public:
  std::unique_ptr<SchedulePolicy> make_schedule_policy(uns16 queue_id, uns32 fu_id, uns64 fu_type) {
    return std::make_unique<OldestFirstSchedulePolicy>();
  }

  std::unique_ptr<ArbitrationPolicy> make_arbitration_policy(size_t fu_num) {
    return std::make_unique<RoundRobinArbitrationPolicy>(fu_num);
  }
};

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
  std::unique_ptr<SelectLogic> select_logic;

 public:
  explicit IssueQueue(uns proc_id, uns16 queue_id, uns16 size, std::vector<FunctionalUnitPicker> connected_fu_pickers);
  uns16 allocate_entry(Op* op);
  void free_entry(uns16 entry_id);
  size_t available_entries() const { return free_list.size(); }
  const std::vector<IssueQueueEntry>& get_entries() const { return entries; }

  void wakeup(uns16 entry_id);
  void issued(uns16 entry_id);
  void reject(uns16 entry_id);

  void recover();
  void schedule();
  bool has_ready_ops() const;
};

IssueQueue::IssueQueue(uns proc_id, uns16 queue_id, uns16 size, std::vector<FunctionalUnitPicker> connected_fu_pickers)
    : proc_id(proc_id), queue_id(queue_id) {
  // this->queue_id is reserved for upcoming use; reference it to silence -Wunused-private-field
  (void)this->queue_id;
  // initialize entries and free list
  entries.reserve(size);
  for (uns16 i = 0; i < size; ++i) {
    entries.emplace_back(queue_id, i);
    free_list.push_back(i);
  }
  DEBUG(proc_id, "Initializing Issue Queue %d with size %d\n", queue_id, size);

  // initialize select logic from the scheduling and arbitration policies.
  IssueQueuePolicyFactory factory;
  size_t fu_num = connected_fu_pickers.size();
  select_logic = std::make_unique<SelectLogic>(proc_id, queue_id, std::move(connected_fu_pickers),
                                               factory.make_arbitration_policy(fu_num));
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
  entry.state = ISSUE_QUEUE_ENTRY_STATE_ALLOCATED;

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
  ASSERT(proc_id, entry.state == ISSUE_QUEUE_ENTRY_STATE_ALLOCATED);

  Op* op = entry.op;
  ASSERT(proc_id, op != nullptr);

  op->in_rdy_list = TRUE;
  entry.state = ISSUE_QUEUE_ENTRY_STATE_READY;
  select_logic->request(&entry);
}

void IssueQueue::issued(uns16 entry_id) {
  ASSERT(proc_id, entry_id < entries.size());
  IssueQueueEntry& entry = entries[entry_id];
  ASSERT(proc_id, entry.state == ISSUE_QUEUE_ENTRY_STATE_PICKED);

  Op* op = entry.op;
  ASSERT(proc_id, op != nullptr);

  STAT_EVENT(node->proc_id, OP_ISSUED);
  op->in_rdy_list = FALSE;
  select_logic->release(&entry);
  free_entry(entry.entry_id);
}

void IssueQueue::reject(uns16 entry_id) {
  ASSERT(proc_id, entry_id < entries.size());
  IssueQueueEntry& entry = entries[entry_id];
  ASSERT(proc_id, entry.state == ISSUE_QUEUE_ENTRY_STATE_PICKED);

  entry.state = ISSUE_QUEUE_ENTRY_STATE_READY;
}

void IssueQueue::recover() {
  for (IssueQueueEntry& entry : entries) {
    Op* op = entry.op;
    if (op == nullptr || !op->off_path) {
      continue;
    }

    if (!FLUSH_OP(op)) {
      continue;
    }

    op->in_rdy_list = FALSE;
    select_logic->release(&entry);
    free_entry(entry.entry_id);
  }
}

void IssueQueue::schedule() {
  select_logic->select();
}

bool IssueQueue::has_ready_ops() const {
  return select_logic->has_ready_ops();
}

/**************************************************************************************/

class IssueQueues {
 private:
  const uns proc_id;
  std::vector<IssueQueue> issue_queues;
  std::vector<uns16> fu_map;
  std::vector<uns64> fu_types;

  void update_mem_block();
  uns16 find_emptiest_queue(Op* op);

 public:
  explicit IssueQueues(uns proc_id);

  void dispatch();
  void schedule();
  void recover();

  void wakeup(Op* op);
  void issued(Op* op);
  void reject(Op* op);
  bool has_ready_ops() const;
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
      int fu_id = __builtin_ctzll(mask);
      ASSERT(proc_id, fu_id < (int)NUM_FUS && fu_map[fu_id] == MAX_UNS16);
      fu_map[fu_id] = i;
    }
  }

  // initialize the connected FUs for each issue queue
  std::vector<std::string> fu_tokens = split_tokens(FU_TYPES);
  ASSERT(proc_id, fu_tokens.size() == NUM_FUS);
  std::vector<std::vector<FunctionalUnitPicker>> fu_connection(NUM_RS);
  fu_types.assign(NUM_RS, 0);
  std::vector<Flag> connected_to_int(NUM_RS, FALSE);
  std::vector<Flag> connected_to_fp(NUM_RS, FALSE);
  IssueQueuePolicyFactory factory;
  for (size_t i = 0; i < NUM_FUS; ++i) {
    uns16 queue_id = fu_map[i];
    ASSERT(proc_id, queue_id != MAX_UNS16);
    uns64 fu_type = parse_mask(fu_tokens[i]);
    fu_connection[queue_id].emplace_back(proc_id, queue_id, i, fu_type,
                                         factory.make_schedule_policy(queue_id, i, fu_type));
    fu_types[queue_id] |= fu_type;
    DEBUG(proc_id, "FU %ld, queue: %d, type: 0x%llx\n", i, queue_id, fu_type);

    connected_to_int[queue_id] |= is_alu_type(fu_type) || is_mul_or_div_type(fu_type);
    connected_to_fp[queue_id] |= is_fpu_type(fu_type);
  }

  // create issue queues based on configuration
  issue_queues.reserve(NUM_RS);
  POWER_TOTAL_RS_SIZE = 0;
  POWER_TOTAL_INT_RS_SIZE = 0;
  POWER_TOTAL_FP_RS_SIZE = 0;
  const char* p = RS_SIZES;
  for (uns16 i = 0; i < NUM_RS; ++i) {
    char* end = nullptr;
    uns64 size = strtoull(p, &end, 10);
    p = end;

    issue_queues.emplace_back(proc_id, i, size, std::move(fu_connection[i]));

    POWER_TOTAL_RS_SIZE += size;
    POWER_TOTAL_INT_RS_SIZE += connected_to_int[i] ? size : 0;
    POWER_TOTAL_FP_RS_SIZE += connected_to_fp[i] ? size : 0;
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
    num_fill_rs++;

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

void IssueQueues::issued(Op* op) {
  uns16 queue_id = op->queue_id;
  ASSERT(proc_id, queue_id < issue_queues.size());
  IssueQueue& queue = issue_queues[queue_id];
  ASSERT(proc_id, op->queue_entry_id != MAX_UNS16);
  queue.issued(op->queue_entry_id);
}

void IssueQueues::reject(Op* op) {
  uns16 queue_id = op->queue_id;
  ASSERT(proc_id, queue_id < issue_queues.size());
  IssueQueue& queue = issue_queues[queue_id];
  ASSERT(proc_id, op->queue_entry_id != MAX_UNS16);
  queue.reject(op->queue_entry_id);
}

bool IssueQueues::has_ready_ops() const {
  for (const IssueQueue& queue : issue_queues) {
    if (queue.has_ready_ops()) {
      return true;
    }
  }
  return false;
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
  // dispatch ops from the reorder buffer to the corresponding issue queues
  issue_queues->dispatch();

  // schedule ready ops in the issue queue to the functional units
  issue_queues->schedule();
}

void issue_queue_wakeup(Op* op) {
  // wake up ops and insert them into the ready list for scheduling
  issue_queues->wakeup(op);
}

void issue_queue_issued(Op* op) {
  // release the entries of ops that are scheduled
  issue_queues->issued(op);
}

void issue_queue_reject(Op* op) {
  // update the entries of ops that are rejected during the execution stage
  issue_queues->reject(op);
}

Flag issue_queue_has_ready_ops() {
  return issue_queues->has_ready_ops();
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
