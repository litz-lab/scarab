/* Copyright 2024 University of California Santa Cruz
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
 * File         : map_dep.h
 * Author       : Y. Zhao, Litz Lab
 * Date         : 6/2024
 * Description  : Dependency Graph for Instruction Chain Tracking
 ***************************************************************************************/

#include "map_dep.h"

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <algorithm>
#include <iterator>
#include <sstream>
#include <iostream>

#include "globals/assert.h"
#include "globals/global_types.h"
#include "globals/global_defs.h"
#include "xed-interface.h"

/**************************************************************************************/

class DependencyEdge {
 private:
  // dependency info
  const uns64 src_signature_;
  const uns64 dst_signature_;
  // chain info
  const uns64 dep_tag_;         // having same depedency tags indicates belonging to a same chain
  const uns64 dep_len_;         // the dependency length from the chain head
  Counter exec_cnt_ = 0;        // the execution count of this dependency

 public:
  explicit DependencyEdge(uns64 src_signature, uns64 dst_signature, uns64 dep_tag, uns64 dep_len)
    : src_signature_(src_signature), dst_signature_(dst_signature), dep_tag_(dep_tag), dep_len_(dep_len) {}

  uns64 GetSrcSignature() const { return src_signature_; }
  uns64 GetDstSignature() const { return dst_signature_; }
  uns64 GetDepTag() const { return dep_tag_; }

  void IncreaseExecCount() { ++exec_cnt_; }
  Counter GetExecCount() const { return exec_cnt_; }

  std::string ToString() const {
    std::ostringstream oss;
    oss << std::hex << "(0x" << src_signature_ << " --> 0x" << dst_signature_ << ")"
      << std::dec << " tag: " << dep_tag_ << " cnt: " << exec_cnt_ << " len: " << dep_len_;
    return oss.str();
  }
};

class OperandInfo {
 private:
  const Counter op_num_;
  const uns8 cf_type_;
  const uns8 mem_type_;

 public:
  explicit OperandInfo(Counter op_num, const Inst_Info& inst_info)
    : op_num_(op_num), cf_type_(inst_info.table_info->cf_type), mem_type_(inst_info.table_info->mem_type) {}

  Counter GetOpNum() const { return op_num_; }
  uns8 GetCfType() const { return cf_type_; }
  uns8 GetMemType() const { return mem_type_; }
};

class InstructionNode {
 private:
  // static info
  const uns64 signature_;
  const uns16 true_op_code_;
  // dynamic info
  std::vector<std::shared_ptr<OperandInfo>> op_info_list_;
  // dependency info
  std::vector<std::shared_ptr<DependencyEdge>> src_edge_list_;
  std::vector<std::shared_ptr<DependencyEdge>> dst_edge_list_;

 public:
  explicit InstructionNode(uns64 signature, uns16 true_op_code)
    : signature_(signature), true_op_code_(true_op_code) {}

  uns64 GetSignature() const { return signature_; }
  void AddOpInfo(Counter op_num, const Inst_Info& inst_info) { op_info_list_.push_back(std::make_shared<OperandInfo>(op_num, inst_info)); }

  void AddSrcEdge(const std::shared_ptr<DependencyEdge>& edge) { src_edge_list_.push_back(edge); }
  std::vector<std::shared_ptr<DependencyEdge>> GetSrcEdgeList() const { return src_edge_list_; }

  void AddDstEdge(const std::shared_ptr<DependencyEdge>& edge) { dst_edge_list_.push_back(edge); }
  std::vector<std::shared_ptr<DependencyEdge>> GetDstEdgeList() const { return dst_edge_list_; }

  int FindSrc(uns64 src_signature) const {
    auto it = std::find_if(src_edge_list_.begin(), src_edge_list_.end(),
      [src_signature](const auto& edge) { return edge->GetSrcSignature() == src_signature; });
    return (it != src_edge_list_.end()) ? std::distance(src_edge_list_.begin(), it) : -1;
  }
  int FindDst(uns64 dst_signature) const {
    auto it = std::find_if(dst_edge_list_.begin(), dst_edge_list_.end(),
      [dst_signature](const auto& edge) { return edge->GetDstSignature() == dst_signature; });
    return (it != dst_edge_list_.end()) ? std::distance(dst_edge_list_.begin(), it) : -1;
  }

  std::string ToString() const {
    std::ostringstream oss;
    oss << "code: 0x" << std::hex << true_op_code_;
    oss << " pc: 0x" << signature_ << " cnt: " << std::dec << op_info_list_.size() << "\n";
    oss << "src:\n";
    for (const auto& edge : src_edge_list_)
      oss << " - " << edge->ToString() << "\n";
    oss << "dst:\n";
    for (const auto& edge : dst_edge_list_)
      oss << " - " << edge->ToString() << "\n";
    oss << "op_num:\n" << " - ";
    for (const auto& op : op_info_list_)
      oss << op->GetOpNum() << ", ";
    oss << "\n\n";
    return oss.str();
  }
};

class InstructionChain {
 private:
  const uns64 chain_tag_;
  uns64 chain_len_ = 0;
  Counter chain_exec_cnt_ = 0;
  std::vector<std::shared_ptr<DependencyEdge>> edge_list_;
  std::vector<std::shared_ptr<InstructionNode>> node_list_;

  std::unordered_set<uns64> live_in_set_;
  std::unordered_set<uns64> live_out_set_;

 public:
  explicit InstructionChain(uns64 chain_tag)
    : chain_tag_(chain_tag) {}

  uns64 GetChainTag() const { return chain_tag_; }

  void SetChainLen(uns64 chain_len) { chain_len_ = chain_len; }
  uns64 GetChainLen() const { return chain_len_; }

  void IncreaseChainExecCount() { ++chain_exec_cnt_; }
  Counter GetChainExecCount() { return chain_exec_cnt_; }

  void AddEdge(const std::shared_ptr<DependencyEdge>& edge) { edge_list_.push_back(edge); }
  std::vector<std::shared_ptr<DependencyEdge>> GetEdgeList() const { return edge_list_; }

  void AddNode(const std::shared_ptr<InstructionNode>& node) { node_list_.push_back(node); }
  std::vector<std::shared_ptr<InstructionNode>> GetNodeList() const { return node_list_; }

  void AddLiveIn(const uns64 chain_tag) { live_in_set_.insert(chain_tag); }
  std::unordered_set<uns64> GetLiveInSet() const { return live_in_set_; }

  void AddLiveOut(const uns64 chain_tag) { live_out_set_.insert(chain_tag); }
  std::unordered_set<uns64> GetLiveOutSet() const { return live_out_set_; }

  std::string ToString() const {
    std::ostringstream oss;
    oss << std::dec << "tag: " << chain_tag_ << " len: " << chain_len_ << " cnt: " << chain_exec_cnt_ << std::endl;
    oss << std::dec << " - live-in #" << live_in_set_.size() << ": ";
    for (const auto& live_in : live_in_set_)
      oss << live_in << ", ";
    oss << std::endl;
    oss << std::dec << " - live-out #" << live_out_set_.size() << ": ";
    for (const auto& live_out : live_out_set_)
      oss << live_out << ", ";
    oss << std::endl;
    for (const auto& edge : edge_list_)
      oss << " - " << edge->ToString() << std::endl;
    return oss.str();
  }
};

class BasicBlock {
 private:
  const uns64 bbl_tag_;

  std::vector<std::shared_ptr<DependencyEdge>> edge_list_;
  std::vector<std::shared_ptr<InstructionNode>> node_list_;

  std::unordered_set<uns64> live_in_set_;
  std::unordered_set<uns64> live_out_set_;

public:
  explicit BasicBlock(uns64 bbl_tag)
    : bbl_tag_(bbl_tag) {}

  uns64 GetBlockTag() const { return bbl_tag_; }

  void AddEdge(const std::shared_ptr<DependencyEdge>& edge) { edge_list_.push_back(edge); }
  const std::vector<std::shared_ptr<DependencyEdge>>& GetEdgeList() const { return edge_list_; }

  void AddNode(const std::shared_ptr<InstructionNode>& node) { node_list_.push_back(node); }
  const std::vector<std::shared_ptr<InstructionNode>>& GetNodeList() const { return node_list_; }

  void AddLiveIn(const uns64 chain_tag) { live_in_set_.insert(chain_tag); }
  std::unordered_set<uns64> GetLiveInSet() const { return live_in_set_; }

  void AddLiveOut(const uns64 chain_tag) { live_out_set_.insert(chain_tag); }
  std::unordered_set<uns64> GetLiveOutSet() const { return live_out_set_; }
};

class DependencyGraph {
 private:
  std::vector<std::shared_ptr<InstructionNode>> node_reg_table_;                      // store the dynamic op node in the RF currently by the register index
  std::vector<std::pair<uns64, uns64>> dep_reg_table_;                                // store the tag and len of the current dependency in the register file
  std::unordered_map<uns64, std::shared_ptr<InstructionNode>> node_map_;              // map the instruction signature to the graph node of the op meta info
  std::unordered_map<uns64, std::shared_ptr<InstructionChain>> chain_map_;            // map the dependency tag to an instruction chain
  std::unordered_map<uns64, std::shared_ptr<BasicBlock>> bbl_map_;                    // map the block tag to a basic block

 public:
  explicit DependencyGraph(uns reg_file_size)
    : node_reg_table_(reg_file_size, nullptr), dep_reg_table_(reg_file_size, {0, 0}) {}

  void SetNodeRegTableEntry(const uns index, const std::shared_ptr<InstructionNode> &node) { node_reg_table_[index] = node; }
  std::shared_ptr<InstructionNode> GetNodeRegTableEntry(const uns index) const { return node_reg_table_[index]; }
  uns GetNodeRegTableSize() const { return node_reg_table_.size(); }

  void SetDepRegTableEntry(const uns index, const uns64 dep_tag, const uns64 dep_len) { dep_reg_table_[index] = {dep_tag, dep_len}; }
  std::pair<uns64, uns64> GetDepRegTableEntry(const uns index) const { return dep_reg_table_[index]; }
  uns GetDepRegTableSize() const { return dep_reg_table_.size(); }

  void AddGraphNode(const uns64 signature, const std::shared_ptr<InstructionNode>& node) { node_map_[signature] = node; }
  std::shared_ptr<InstructionNode> GetGraphNode(const uns64 signature) const {
    auto it = node_map_.find(signature);
    return (it != node_map_.end()) ? it->second : nullptr;
  }
  uns64 GetNodeMapSize() const { return node_map_.size(); }

  void AddGraphChain(const std::shared_ptr<InstructionChain>& chain) { chain_map_[chain->GetChainTag()] = chain; }
  std::shared_ptr<InstructionChain> GetGraphChain(const uns64 chain_tag) const {
    auto it = chain_map_.find(chain_tag);
    return (it != chain_map_.end()) ? it->second : nullptr;
  }
  std::unordered_map<uns64, std::shared_ptr<InstructionChain>> GetGraphChainMap() const { return chain_map_; }

  void AddGraphBlock(const std::shared_ptr<BasicBlock>& bbl) { bbl_map_[bbl->GetBlockTag()] = bbl; }
  std::shared_ptr<BasicBlock> GetGraphBlock(const uns64 bbl_tag) const {
    auto it = bbl_map_.find(bbl_tag);
    return (it != bbl_map_.end()) ? it->second : nullptr;
  }
  std::unordered_map<uns64, std::shared_ptr<BasicBlock>> GetGraphBlockMap() const { return bbl_map_; }

  uns64 CalEdgeNum() const {
    uns64 total_edges = 0;
    for (const auto& pair : chain_map_) {
      total_edges += pair.second->GetEdgeList().size();
    }
    return total_edges;
  }

  std::string ToString() const {
    std::ostringstream oss;

    oss << "-----------Node Info-----------" << std::endl;
    for (const auto& pair : node_map_)
      oss << pair.second->ToString();
    oss << std::endl;

    oss << "-----------Chain Info-----------" << std::endl;
    for (const auto& pair : chain_map_)
      oss << pair.second->ToString() << std::endl;
    oss << std::endl;

    return oss.str();
  }
};

/**************************************************************************************/
/* Global Variables */

// the instance of the dependency graph
std::shared_ptr<DependencyGraph> g_dep_graph = nullptr;

// the current node and basic block pointer
std::shared_ptr<InstructionNode> g_curr_node = nullptr;
std::shared_ptr<BasicBlock> g_prev_bbl = nullptr;
std::shared_ptr<BasicBlock> g_curr_bbl = nullptr;

// the current dependency meta info
uns64 g_curr_dep_tag = 0;
uns64 g_curr_dep_len = 0;

/**************************************************************************************/
/* Static Inline Method */

// update statistics counters when processing
static inline void map_dep_update_stat(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op);

  STAT_EVENT(0, MAP_DEP_STAT_TOTAL);
  if (op->off_path)
    return;
  STAT_EVENT(0, MAP_DEP_STAT_ONPATH);

  if (op->table_info->num_dest_regs)
    STAT_EVENT(0, MAP_DEP_STAT_PRODUCE);
}

// create a new basic block
static inline std::shared_ptr<BasicBlock> map_dep_create_block(Op *op) {
  auto bbl = std::make_shared<BasicBlock>(op->op_num);
  g_dep_graph->AddGraphBlock(bbl);
  return bbl;
}

// create a new instruction node
static inline std::shared_ptr<InstructionNode> map_dep_create_node(Op *op) {
  auto inst_node = std::make_shared<InstructionNode>(op->inst_info->addr, op->inst_info->table_info->true_op_type);
  g_dep_graph->AddGraphNode(op->inst_info->addr, inst_node);
  return inst_node;
}

// create a new dependency edge
static inline std::shared_ptr<DependencyEdge> map_dep_create_edge(const std::shared_ptr<InstructionNode>& prev_node) {
  // insert the edge to the source node and the destination node
  auto dep_edge = std::make_shared<DependencyEdge>(
    prev_node->GetSignature(), g_curr_node->GetSignature(), g_curr_dep_tag, g_curr_dep_len);
  g_curr_node->AddSrcEdge(dep_edge);
  prev_node->AddDstEdge(dep_edge);

  // append the edge to a chain
  auto chain = g_dep_graph->GetGraphChain(g_curr_dep_tag);
  if (!chain) {
    chain = std::make_shared<InstructionChain>(g_curr_dep_tag);
    g_dep_graph->AddGraphChain(chain);
  }
  chain->AddEdge(dep_edge);

  // add node into the chain
  auto node_list = chain->GetNodeList();
  if (std::find(node_list.begin(), node_list.end(), prev_node) == node_list.end())
    chain->AddNode(prev_node);
  if (std::find(node_list.begin(), node_list.end(), g_curr_node) == node_list.end())
    chain->AddNode(g_curr_node);

  // set the chain len
  uns64 chain_len = chain->GetChainLen();
  if (chain_len < g_curr_dep_len)
    chain->SetChainLen(g_curr_dep_len);

  // append live-in and live-out
  for (const auto& edge : prev_node->GetSrcEdgeList()) {
    if (edge->GetDepTag() == chain->GetChainTag())
      continue;
    chain->AddLiveIn(edge->GetDepTag());
    g_dep_graph->GetGraphChain(edge->GetDepTag())->AddLiveOut(chain->GetChainTag());
  }

  return dep_edge;
}

// update the dependency chain info for creating new edge
static inline void map_dep_process_dep_info(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);

  Flag if_diff_tag = FALSE;
  uns64 dep_tag = 0;
  uns64 dep_len = 1;

  for (uns ii = 0; ii < op->oracle_info.num_srcs; ++ii) {
    ASSERT(0, op->oracle_info.src_info[ii].type == REG_DATA_DEP);
    int ind = op->oracle_info.src_info[ii].reg_ptag;
    ASSERT(0, ind != -1 && static_cast<uns>(ind) < g_dep_graph->GetDepRegTableSize());

    // skip reading nullptr at the very beginning
    if (g_dep_graph->GetNodeRegTableEntry(static_cast<uns>(ind)) == nullptr)
      continue;

    // read dependency meta info from previous edge
    uns64 prev_dep_tag = g_dep_graph->GetDepRegTableEntry(static_cast<uns>(ind)).first;
    uns64 prev_dep_len = g_dep_graph->GetDepRegTableEntry(static_cast<uns>(ind)).second;
    ASSERT(0, prev_dep_tag != 0 && prev_dep_len != 0);

    // check if all the tag are the same
    if (dep_tag == 0)
      dep_tag = prev_dep_tag;
    else if (dep_tag != prev_dep_tag)
      if_diff_tag = TRUE;

    // increase the len from the last producer and select the critical path
    dep_len = dep_len > (prev_dep_len + 1) ? dep_len : (prev_dep_len + 1);
  }

  g_curr_dep_tag = (dep_tag != 0 && !if_diff_tag) ? dep_tag : op->op_num;
  g_curr_dep_len = (dep_tag != 0 && !if_diff_tag) ? dep_len : 1;
}

// update the current node info
static inline void map_dep_process_node_info(Op *op) {
  // create a node if it is a new PC or get the node by PC from the hash table
  g_curr_node = g_dep_graph->GetGraphNode(op->inst_info->addr);
  if (g_curr_node == nullptr)
    g_curr_node = map_dep_create_node(op);

  // append the operand info into the node
  g_curr_node->AddOpInfo(op->op_num, *op->inst_info);
}

// update the current basic block info
static inline void map_dep_process_bbl_info(Op *op) {
  // create a new bbl at the very begining or after a control flow
  if (g_curr_bbl == NULL)
    g_curr_bbl = map_dep_create_block(op);

  // prepare creating a new bbl for next op if the current op is control-flow
  g_prev_bbl = g_curr_bbl;
  if (op->table_info->cf_type)
    g_curr_bbl =  NULL;
}

// print all the node info in the graph by pc
static inline void map_dep_print_graph(void) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr);

  std::cout << std::endl;
  std::cout << "Graph Node Num: " << g_dep_graph->GetNodeMapSize() << std::endl;
  std::cout << "Graph Edge Num: " << g_dep_graph->CalEdgeNum() << std::endl;
  std::cout << "Graph Chain Num: " << g_dep_graph->GetGraphChainMap().size() << std::endl;
  std::cout << "Graph Block Num: " << g_dep_graph->GetGraphBlockMap().size() << std::endl;
  std::cout << std::endl;

  // std::cout << g_dep_graph->ToString() << std::endl;
}

/**************************************************************************************/
/* External Function Call */

void map_dep_init(uns reg_file_size) {
  if (!MAP_DEP_ENABLE)
    return;
  g_dep_graph = std::make_shared<DependencyGraph>(reg_file_size);
  g_curr_node = nullptr;
  g_curr_bbl = nullptr;
  g_curr_dep_tag = 0;
  g_curr_dep_len = 0;
}

// process each instruction as a graph node to track dependency
void map_dep_process(Op* op) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr && op);

  // update statistics counters
  map_dep_update_stat(op);

  // only process on-path op
  if (op->off_path)
    return;

  // update the current node info
  map_dep_process_node_info(op);

  // update the current basic block info
  map_dep_process_bbl_info(op);
}

// update the dependency when a register is readed
void map_dep_read(Op* op) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr);

  // only track on-path op read src
  ASSERT(0, op);
  if (op->off_path)
    return;
  ASSERT(0, g_curr_node != nullptr && g_curr_node->GetSignature() == op->inst_info->addr);

  // update the dependency chain info for creating new edge
  map_dep_process_dep_info(op);

  // append the source op to the current node for all source registers
  for (uns ii = 0; ii < op->oracle_info.num_srcs; ++ii) {
    ASSERT(0, op->oracle_info.src_info[ii].type == REG_DATA_DEP);
    int ind = op->oracle_info.src_info[ii].reg_ptag;
    ASSERT(0, ind != -1 && static_cast<uns>(ind) < g_dep_graph->GetNodeRegTableSize());
    auto prev_node = g_dep_graph->GetNodeRegTableEntry(static_cast<uns>(ind));

    // skip reading nullptr at the very beginning
    if (prev_node == nullptr)
      continue;

    // if cannot find prev node, create a new dependency edge
    int prev_src_dep_ind = g_curr_node->FindSrc(prev_node->GetSignature());
    std::shared_ptr<DependencyEdge> edge;
    if (prev_src_dep_ind == -1)
      edge = map_dep_create_edge(prev_node);
    else
      edge = g_curr_node->GetSrcEdgeList()[static_cast<uns>(prev_src_dep_ind)];

    // increase the execution count of the existing dependency edge and chain
    edge->IncreaseExecCount();
    g_dep_graph->GetGraphChain(edge->GetDepTag())->IncreaseChainExecCount();
  }
}

// track the producer when a register is written
void map_dep_write(Op* op) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr);

  // only track on-path op write dst
  ASSERT(0, op);
  if (op->off_path)
    return;
  ASSERT(0, g_curr_node != nullptr && g_curr_node->GetSignature() == op->inst_info->addr);

  // put the current node and dep meta info into the register map for all destination registers
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int ind = op->dst_reg_file_ptag[ii];
    ASSERT(0, ind != -1 && static_cast<uns>(ind) < g_dep_graph->GetNodeRegTableSize());
    ASSERT(0, g_dep_graph->GetNodeRegTableEntry(static_cast<uns>(ind)) == NULL);

    g_dep_graph->SetNodeRegTableEntry(static_cast<uns>(ind), g_curr_node);
    g_dep_graph->SetDepRegTableEntry(static_cast<uns>(ind), g_curr_dep_tag, g_curr_dep_len);
  }
}

// release the register and clear the dep meta info
void map_dep_release(uns ind) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr);

  // skip clear empty entry at the very begining
  std::shared_ptr<InstructionNode> node = g_dep_graph->GetNodeRegTableEntry(ind);
  if (node == nullptr)
    return;

  g_dep_graph->SetNodeRegTableEntry(static_cast<uns>(ind), nullptr);
  g_dep_graph->SetDepRegTableEntry(static_cast<uns>(ind), 0, 0);
}


// do ananysis after generating the whole graph
void map_dep_done(void) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr);

  map_dep_print_graph();
}
