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
#include <fstream>

#include "globals/assert.h"
#include "globals/global_types.h"
#include "globals/global_defs.h"
#include "xed-interface.h"

/**************************************************************************************/

/*
  Description:
  --- the operand instance corresponding to each op_num
  Identifier:
  --- op_num
*/
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

/*
  Description:
  --- the dependency relationship of register data from the src instruction to the dst instruction
  Identifier:
  --- pair<src_inst_pc, dst_inst_pc>
*/
class DependencyEdge {
 private:
  // instruction info
  const uns64 src_inst_pc_;
  const uns64 dst_inst_pc_;
  // chain info
  const uns64 dep_tag_;           // sharing identical depedency tags indicates belonging to the same chain
  const uns64 dep_len_;           // the dependency length from the head of the chain
  Counter dep_exec_cnt_ = 0;      // the execution count of this dependency edge
  Counter dep_avg_latency_ = 0;   // the average latency of consuming

 public:
  explicit DependencyEdge(uns64 src_inst_pc, uns64 dst_inst_pc, uns64 dep_tag, uns64 dep_len)
    : src_inst_pc_(src_inst_pc), dst_inst_pc_(dst_inst_pc), dep_tag_(dep_tag), dep_len_(dep_len) {}

  uns64 GetSrcInstPC() const { return src_inst_pc_; }
  uns64 GetDstInstPC() const { return dst_inst_pc_; }

  uns64 GetDepTag() const { return dep_tag_; }
  uns64 GetDepLen() const { return dep_len_; }

  void IncreaseDepExecCount() { ++dep_exec_cnt_; }
  Counter GetDepExecCount() const { return dep_exec_cnt_; }

  void CalDepAvgLatency(Counter curr_latency) { dep_avg_latency_ = (dep_avg_latency_ * (dep_exec_cnt_ - 1) + curr_latency) / dep_exec_cnt_; }
  Counter GetDepAvgLatency() const { return dep_avg_latency_; }
};

/*
  Description:
  --- the instruction with the same program counter (PC)
  Identifier:
  --- inst_pc
*/
class InstructionNode {
 private:
  // static info
  const uns64 inst_pc_;
  const uns16 op_code_;
  // dynamic info
  std::vector<std::shared_ptr<OperandInfo>> op_info_list_;
  std::unordered_set<uns64> block_pc_set_;
  // dependency info
  std::vector<std::shared_ptr<DependencyEdge>> src_edge_list_;
  std::vector<std::shared_ptr<DependencyEdge>> dst_edge_list_;

 public:
  explicit InstructionNode(uns64 inst_pc, uns16 op_code)
    : inst_pc_(inst_pc), op_code_(op_code) {}

  uns64 GetInstPC() const { return inst_pc_; }
  uns64 GetInstOpCode() const { return op_code_; }

  void AddOpInfo(Counter op_num, const Inst_Info& inst_info) { op_info_list_.push_back(std::make_shared<OperandInfo>(op_num, inst_info)); }
  const std::vector<std::shared_ptr<OperandInfo>>& GetOpInfoList() const { return op_info_list_; }

  void AddBlockPC(uns64 block_pc) { block_pc_set_.insert(block_pc); }
  const std::unordered_set<uns64>& GetBlockPCSet() const { return block_pc_set_; }

  void AddSrcEdge(const std::shared_ptr<DependencyEdge>& edge) { src_edge_list_.push_back(edge); }
  const std::vector<std::shared_ptr<DependencyEdge>>& GetSrcEdgeList() const { return src_edge_list_; }

  void AddDstEdge(const std::shared_ptr<DependencyEdge>& edge) { dst_edge_list_.push_back(edge); }
  const std::vector<std::shared_ptr<DependencyEdge>>& GetDstEdgeList() const { return dst_edge_list_; }

  int FindSrcInst(uns64 src_inst_pc) const {
    auto it = std::find_if(src_edge_list_.begin(), src_edge_list_.end(),
      [src_inst_pc](const auto& edge) { return edge->GetSrcInstPC() == src_inst_pc; });
    return (it != src_edge_list_.end()) ? std::distance(src_edge_list_.begin(), it) : -1;
  }
  int FindDstInst(uns64 dst_inst_pc) const {
    auto it = std::find_if(dst_edge_list_.begin(), dst_edge_list_.end(),
      [dst_inst_pc](const auto& edge) { return edge->GetDstInstPC() == dst_inst_pc; });
    return (it != dst_edge_list_.end()) ? std::distance(dst_edge_list_.begin(), it) : -1;
  }
};

/*
  Description:
  --- the instruction fused chain composed of a series of dependency edges
  Identifier:
  --- chain_tag
*/
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
  Counter GetChainExecCount() const { return chain_exec_cnt_; }

  void AddEdge(const std::shared_ptr<DependencyEdge>& edge) { edge_list_.push_back(edge); }
  const std::vector<std::shared_ptr<DependencyEdge>>& GetEdgeList() const { return edge_list_; }

  void AddNode(const std::shared_ptr<InstructionNode>& node) { node_list_.push_back(node); }
  const std::vector<std::shared_ptr<InstructionNode>>& GetNodeList() const { return node_list_; }

  void AddLiveIn(const uns64 chain_tag) { live_in_set_.insert(chain_tag); }
  const std::unordered_set<uns64>& GetLiveInSet() const { return live_in_set_; }

  void AddLiveOut(const uns64 chain_tag) { live_out_set_.insert(chain_tag); }
  const std::unordered_set<uns64>& GetLiveOutSet() const { return live_out_set_; }
};

/*
  Description:
  --- the basic block composed of a straight-line instruction sequence with no control flow interruptions
  Identifier:
  --- block_pc
*/
class BasicBlock {
 private:
  const uns64 block_pc_;

  std::vector<std::shared_ptr<DependencyEdge>> edge_list_;
  std::vector<std::shared_ptr<InstructionNode>> node_list_;

  std::unordered_set<uns64> live_in_set_;
  std::unordered_set<uns64> live_out_set_;

public:
  explicit BasicBlock(uns64 block_pc)
    : block_pc_(block_pc) {}

  uns64 GetBlockPC() const { return block_pc_; }

  void AddEdge(const std::shared_ptr<DependencyEdge>& edge) { edge_list_.push_back(edge); }
  const std::vector<std::shared_ptr<DependencyEdge>>& GetEdgeList() const { return edge_list_; }

  void AddNode(const std::shared_ptr<InstructionNode>& node) { node_list_.push_back(node); }
  const std::vector<std::shared_ptr<InstructionNode>>& GetNodeList() const { return node_list_; }

  void AddLiveIn(const uns64 block_pc) { live_in_set_.insert(block_pc); }
  const std::unordered_set<uns64>& GetLiveInSet() const { return live_in_set_; }

  void AddLiveOut(const uns64 block_pc) { live_out_set_.insert(block_pc); }
  const std::unordered_set<uns64>& GetLiveOutSet() const { return live_out_set_; }
};

/*
  Description:
  === Dependency Graph Definition
  --- node: instruction
  --- edge: dependency
  === Entity
  --- chain: a list of depedency
  --- block: a list of instruction
*/
class DependencyGraph {
 private:
  /* register tables for recording producers to track future consuming */
  std::vector<std::shared_ptr<InstructionNode>> node_reg_table_;                // store the dynamic op node in the RF currently by the register index
  std::vector<std::pair<uns64, uns64>> dep_reg_table_;                          // store the tag and len of the current dependency in the register file
  std::vector<Counter> cycle_reg_table_;                                        // store the produce cycle of the op in the RF by the register index

  /* map storing instructions, chains, and blocks */
  std::unordered_map<uns64, std::shared_ptr<InstructionNode>> inst_map_;        // map an instruction pc to the graph node of the op meta info
  std::unordered_map<uns64, std::shared_ptr<InstructionChain>> chain_map_;      // map a dependency tag to an instruction chain
  std::unordered_map<uns64, std::shared_ptr<BasicBlock>> block_map_;            // map a basic block pc to a basic block

 public:
  explicit DependencyGraph(uns reg_file_size)
    : node_reg_table_(reg_file_size, nullptr), dep_reg_table_(reg_file_size, {0, 0}), cycle_reg_table_(reg_file_size, 0) {}

  void SetNodeRegTableEntry(const uns index, const std::shared_ptr<InstructionNode> &node) { node_reg_table_[index] = node; }
  std::shared_ptr<InstructionNode> GetNodeRegTableEntry(const uns index) const { return node_reg_table_[index]; }
  uns GetNodeRegTableSize() const { return node_reg_table_.size(); }

  void SetDepRegTableEntry(const uns index, const uns64 dep_tag, const uns64 dep_len) { dep_reg_table_[index] = {dep_tag, dep_len}; }
  std::pair<uns64, uns64> GetDepRegTableEntry(const uns index) const { return dep_reg_table_[index]; }
  uns GetDepRegTableSize() const { return dep_reg_table_.size(); }

  void SetCycleRegTableEntry(const uns index, const Counter produce_cycle) { cycle_reg_table_[index] = produce_cycle; }
  Counter GetCycleRegTableEntry(const uns index) const { return cycle_reg_table_[index]; }
  uns GetCycleRegTableSize() const { return cycle_reg_table_.size(); }

  void AddGraphInst(const uns64 inst_pc, const std::shared_ptr<InstructionNode>& node) { inst_map_[inst_pc] = node; }
  std::shared_ptr<InstructionNode> GetGraphInst(const uns64 inst_pc) const {
    auto it = inst_map_.find(inst_pc);
    return (it != inst_map_.end()) ? it->second : nullptr;
  }
  const std::unordered_map<uns64, std::shared_ptr<InstructionNode>>& GetGraphInstMap() const { return inst_map_; }

  void AddGraphChain(const std::shared_ptr<InstructionChain>& chain) { chain_map_[chain->GetChainTag()] = chain; }
  std::shared_ptr<InstructionChain> GetGraphChain(const uns64 chain_tag) const {
    auto it = chain_map_.find(chain_tag);
    return (it != chain_map_.end()) ? it->second : nullptr;
  }
  const std::unordered_map<uns64, std::shared_ptr<InstructionChain>>& GetGraphChainMap() const { return chain_map_; }

  void AddGraphBlock(const std::shared_ptr<BasicBlock>& block) { block_map_[block->GetBlockPC()] = block; }
  std::shared_ptr<BasicBlock> GetGraphBlock(const uns64 block_pc) const {
    auto it = block_map_.find(block_pc);
    return (it != block_map_.end()) ? it->second : nullptr;
  }
  const std::unordered_map<uns64, std::shared_ptr<BasicBlock>>& GetGraphBlockMap() const { return block_map_; }
};

/*
  Description:
  --- the writer of the dependency graph for doing ananysis
*/
class DependencyGraphWriter {
 private:
  std::shared_ptr<DependencyGraph> dep_graph_;

  const std::string inst_filename_ = "dep_inst.txt";
  const std::string chain_filename_ = "dep_chain.txt";
  const std::string block_filename_ = "dep_block.txt";

 public:
  explicit DependencyGraphWriter(std::shared_ptr<DependencyGraph> dep_graph)
    : dep_graph_(dep_graph) {}

  std::string WriteDepEdge(const DependencyEdge& edge) const {
    std::ostringstream oss;
    oss << std::hex << "(0x" << edge.GetSrcInstPC()
        << " --> 0x" << edge.GetDstInstPC() << ")"
        << std::dec << " tag: " << edge.GetDepTag()
        << " cnt: " << edge.GetDepExecCount()
        << " len: " << edge.GetDepLen()
        << " latency: " << edge.GetDepAvgLatency();
    return oss.str();
  }

  std::string WriteInstNode(const InstructionNode& node) const {
    std::ostringstream oss;
    oss << "[INST] code: 0x" << std::hex << node.GetInstOpCode()
        << " pc: 0x" << node.GetInstPC()
        << " cnt: " << std::dec << node.GetOpInfoList().size() << std::endl;
    oss << " - src:" << std::endl;
    for (const auto& edge : node.GetSrcEdgeList())
        oss << " --- " << WriteDepEdge(*edge) << std::endl;
    oss << " - dst:" << std::endl;
    for (const auto& edge : node.GetDstEdgeList())
        oss << " --- " << WriteDepEdge(*edge) << std::endl;
    oss << " - op_num:" << std::endl << " --- ";
    for (const auto& op_info : node.GetOpInfoList())
        oss << op_info->GetOpNum() << ", ";
    oss << std::endl;
    return oss.str();
  }

  std::string WriteChain(const InstructionChain& chain) const {
    std::ostringstream oss;
    oss << std::dec << "[CHAIN] tag: " << chain.GetChainTag()
        << " len: " << chain.GetChainLen()
        << " cnt: " << chain.GetChainExecCount() << std::endl;
    oss << std::dec << " - live-in #" << chain.GetLiveInSet().size() << ": ";
    for (const auto& live_in : chain.GetLiveInSet())
        oss << live_in << ", ";
    oss << std::endl;
    oss << std::dec << " - live-out #" << chain.GetLiveOutSet().size() << ": ";
    for (const auto& live_out : chain.GetLiveOutSet())
        oss << live_out << ", ";
    oss << std::endl;
    for (const auto& edge : chain.GetEdgeList())
        oss << " - " << WriteDepEdge(*edge) << std::endl;
    return oss.str();
  }

  std::string WriteBlock(const BasicBlock& block) const {
    std::ostringstream oss;
    oss << "[BBL] pc: 0x" << std::hex << block.GetBlockPC() << std::endl;
    oss << std::dec << " - live-in #" << block.GetLiveInSet().size() << ": ";
    for (const auto& live_in : block.GetLiveInSet())
        oss << std::hex << "0x" << live_in << ", ";
    oss << std::endl;
    oss << std::dec << " - live-out #" << block.GetLiveOutSet().size() << ": ";
    for (const auto& live_out : block.GetLiveOutSet())
        oss << std::hex << "0x" << live_out << ", ";
    oss << std::endl;
    oss << std::dec << " - instruction #" << block.GetNodeList().size() << ":" << std::endl;
    for (const auto& node : block.GetNodeList())
        oss << " --- " << std::hex << "0x" << node->GetInstPC() << std::endl;
    return oss.str();
  }

  void WriteInstMap() const {
    std::ofstream ofs(inst_filename_);
    ASSERT(0, ofs.is_open());
    for (const auto& pair : dep_graph_->GetGraphInstMap())
      ofs << WriteInstNode(*pair.second);
    ofs.close();
  }

  void WriteChainMap() const {
    std::ofstream ofs(chain_filename_);
    ASSERT(0, ofs.is_open());
    for (const auto& pair : dep_graph_->GetGraphChainMap())
      ofs << WriteChain(*pair.second);
    ofs.close();
  }

  void WriteBlockMap() const {
    std::ofstream ofs(block_filename_);
    ASSERT(0, ofs.is_open());
    for (const auto& pair : dep_graph_->GetGraphBlockMap())
      ofs << WriteBlock(*pair.second);
    ofs.close();
  }
};

/**************************************************************************************/
/* Global Variables */

// the instance of the dependency graph
std::shared_ptr<DependencyGraph> g_dep_graph = nullptr;

// the global pointers of the current node and the basic blocks
std::shared_ptr<InstructionNode> g_curr_node = nullptr;
std::shared_ptr<BasicBlock> g_curr_block = nullptr;
std::shared_ptr<BasicBlock> g_prev_block = nullptr;

// the current dependency meta info
uns64 g_curr_dep_tag = 0;
uns64 g_curr_dep_len = 0;
uns64 g_last_cf_meta = 0;

/**************************************************************************************/
/* Static Inline Method */

// increase statistics counters when processing
static inline void map_dep_increase_num_stat(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op);

  STAT_EVENT(0, MAP_DEP_STAT_TOTAL);
  if (op->off_path)
    return;
  STAT_EVENT(0, MAP_DEP_STAT_ONPATH);

  if (op->table_info->num_dest_regs)
    STAT_EVENT(0, MAP_DEP_STAT_PRODUCE);
}

// update the length, node set, live-in, and live-out of the chain
static inline void map_dep_update_chain_property(std::shared_ptr<InstructionChain>& chain, std::shared_ptr<InstructionNode>& prev_node) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && prev_node != nullptr && g_curr_node != nullptr);

  // insert nodes into the chain
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
}

// update the nodes, live-in, and live-out of the bbl
static inline void map_dep_update_block_property(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op);

  // insert the instruction node
  std::shared_ptr<InstructionNode> inst_node = g_dep_graph->GetGraphInst(op->inst_info->addr);
  auto node_list = g_curr_block->GetNodeList();
  inst_node->AddBlockPC(g_curr_block->GetBlockPC());
  if (std::find(node_list.begin(), node_list.end(), inst_node) == node_list.end())
    g_curr_block->AddNode(inst_node);

  // update live-in and live-out
  if (g_prev_block == nullptr)
    return;
  g_curr_block->AddLiveIn(g_prev_block->GetBlockPC());
  g_prev_block->AddLiveOut(g_curr_block->GetBlockPC());
  g_prev_block = nullptr;
}

// create a new basic block and add it into the block graph
static inline std::shared_ptr<BasicBlock> map_dep_create_block(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op);

  auto block = std::make_shared<BasicBlock>(op->inst_info->addr);
  g_dep_graph->AddGraphBlock(block);
  return block;
}

// create a new chain and add it into the chain graph
static inline std::shared_ptr<InstructionChain> map_dep_create_chain(void) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr);

  auto chain = std::make_shared<InstructionChain>(g_curr_dep_tag);
  g_dep_graph->AddGraphChain(chain);
  return chain;
}

// create a new instruction node and add it into the instruction graph
static inline std::shared_ptr<InstructionNode> map_dep_create_node(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op);

  auto inst_node = std::make_shared<InstructionNode>(op->inst_info->addr, op->inst_info->table_info->true_op_type);
  g_dep_graph->AddGraphInst(op->inst_info->addr, inst_node);
  return inst_node;
}

// create a new dependency edge and if its tag is new create a new chain
static inline std::shared_ptr<DependencyEdge> map_dep_create_edge(std::shared_ptr<InstructionNode>& prev_node) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && prev_node != nullptr && g_curr_node != nullptr);

  // insert the edge to the source node and the destination node
  auto dep_edge = std::make_shared<DependencyEdge>(
    prev_node->GetInstPC(), g_curr_node->GetInstPC(), g_curr_dep_tag, g_curr_dep_len);
  g_curr_node->AddSrcEdge(dep_edge);
  prev_node->AddDstEdge(dep_edge);

  // create a new chain if it is a new tag
  auto chain = g_dep_graph->GetGraphChain(g_curr_dep_tag);
  if (!chain)
    chain = map_dep_create_chain();

  // append the edge to the chain and the current block
  chain->AddEdge(dep_edge);
  if (g_curr_block)
    g_curr_block->AddEdge(dep_edge);

  // update the chain len, node set, live-in and live-out
  map_dep_update_chain_property(chain, prev_node);

  return dep_edge;
}

// set the global pointer of the current instruction node by looking up the graph hash map
static inline void map_dep_set_global_node(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);

  // create a node if it is a new PC or get the node by PC from the hash table
  g_curr_node = g_dep_graph->GetGraphInst(op->inst_info->addr);
  if (g_curr_node == nullptr)
    g_curr_node = map_dep_create_node(op);

  // append the operand info into the node
  g_curr_node->AddOpInfo(op->op_num, *op->inst_info);
}

// set the global pointer of the basic block
static inline void map_dep_set_global_bbl(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);

  // reset the bbl at the very begining or after a control flow
  if (g_curr_block == nullptr) {
    // lookup the hash table by PC
    g_curr_block = g_dep_graph->GetGraphBlock(op->inst_info->addr);
    if (g_curr_block == nullptr)
      g_curr_block = map_dep_create_block(op);
  }

  // update the nodes, live-in, live-out of the bbl
  map_dep_update_block_property(op);
}

// clear the bbl ptr and update the cf meta if there is a control flow interruption
static inline void map_dep_set_cf_meta(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);

  if (op->table_info->cf_type == NOT_CF)
    return;

  // clear the current bbl after control flow interruptions
  g_prev_block = g_curr_block;
  g_curr_block = nullptr;

  // record the latest cf
  g_last_cf_meta = op->op_num;
}

// set the global value of the current dependency meta for creating new edges by checking previous tags
static inline void map_dep_set_dep_meta(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);

  Flag if_diff_tag = FALSE;
  Flag if_exist_cf = FALSE;
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

    // check if in the same bbl
    if (prev_dep_tag <= g_last_cf_meta)
      if_exist_cf = TRUE;

    // increase the len from the last producer and select the critical path
    dep_len = dep_len > (prev_dep_len + 1) ? dep_len : (prev_dep_len + 1);
  }

  // if existing previous edges and all their tags are the same and in the same bbl, then inherit the tag
  g_curr_dep_tag = (dep_tag != 0 && !if_diff_tag && !if_exist_cf) ? dep_tag : op->op_num;
  g_curr_dep_len = (dep_tag != 0 && !if_diff_tag && !if_exist_cf) ? dep_len : 1;
}

// write all the instruction, chain, and block graph
static inline void map_dep_write_graph(void) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr);

  uns64 total_edges = 0;
  for (const auto& pair : g_dep_graph->GetGraphChainMap())
    total_edges += pair.second->GetEdgeList().size();

  std::cout << std::endl;
  std::cout << "Graph Edge Num: " << total_edges << std::endl;
  std::cout << "Graph Node Num: " << g_dep_graph->GetGraphInstMap().size() << std::endl;
  std::cout << "Graph Chain Num: " << g_dep_graph->GetGraphChainMap().size() << std::endl;
  std::cout << "Graph Block Num: " << g_dep_graph->GetGraphBlockMap().size() << std::endl;
  std::cout << std::endl;

  if (!MAP_DEP_SERIALIZATION_ENABLE)
    return;

  DependencyGraphWriter graph_writer(g_dep_graph);
  graph_writer.WriteInstMap();
  graph_writer.WriteChainMap();
  graph_writer.WriteBlockMap();
}

/**************************************************************************************/
/* External Function Call */

/*
  Called by:
  --- map.c -> init ptag
  Procedure:
  --- create the graph instance
*/
void map_dep_init(uns reg_file_size) {
  if (!MAP_DEP_ENABLE)
    return;
  g_dep_graph = std::make_shared<DependencyGraph>(reg_file_size);

  g_curr_node = nullptr;
  g_curr_block = nullptr;
  g_prev_block = nullptr;
  g_curr_dep_tag = 0;
  g_curr_dep_len = 0;
  g_last_cf_meta = 0;
}

/*
  Called by:
  --- map.c -> rename_table_process (before read and write)
  Procedure:
  --- process each instruction as a graph node to track dependency
*/
void map_dep_process(Op* op) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr && op);

  // update statistics counters
  map_dep_increase_num_stat(op);

  // only process on-path op
  if (op->off_path)
    return;

  // set the global pointer of the current instruction node by looking up the graph hash map
  map_dep_set_global_node(op);

  // set the global pointer of the basic block
  map_dep_set_global_bbl(op);

  // clear the bbl ptr and update the cf meta if there is a control flow interruption
  map_dep_set_cf_meta(op);
}

/*
  Called by:
  --- map.c -> read ptag
  Procedure:
  --- build/update dependency edges when a destination consumes a source
*/
void map_dep_read(Op* op) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr && op);

  // only track on-path op read src
  if (op->off_path)
    return;
  ASSERT(0, g_curr_node != nullptr && g_curr_node->GetInstPC() == op->inst_info->addr);

  // set the global value of the current dependency meta for creating new edges by checking previous tags
  map_dep_set_dep_meta(op);

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
    int prev_src_dep_ind = g_curr_node->FindSrcInst(prev_node->GetInstPC());
    std::shared_ptr<DependencyEdge> edge;
    if (prev_src_dep_ind == -1)
      edge = map_dep_create_edge(prev_node);
    else
      edge = g_curr_node->GetSrcEdgeList()[static_cast<uns>(prev_src_dep_ind)];

    // increase the execution count of the existing dependency edge and chain
    edge->IncreaseDepExecCount();
    g_dep_graph->GetGraphChain(edge->GetDepTag())->IncreaseChainExecCount();
  }
}

/*
  Called by:
  --- map.c -> write ptag
  Procedure:
  --- record the producer information for future consuming
*/
void map_dep_write(Op* op) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr && op);

  // only track on-path op write dst
  if (op->off_path)
    return;
  ASSERT(0, g_curr_node != nullptr && g_curr_node->GetInstPC() == op->inst_info->addr);

  // insert the current node and dep meta info into the register map for all destination registers
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int ind = op->dst_reg_file_ptag[ii];
    ASSERT(0, ind != -1 && static_cast<uns>(ind) < g_dep_graph->GetNodeRegTableSize());
    ASSERT(0, g_dep_graph->GetNodeRegTableEntry(static_cast<uns>(ind)) == NULL);

    g_dep_graph->SetNodeRegTableEntry(static_cast<uns>(ind), g_curr_node);
    g_dep_graph->SetDepRegTableEntry(static_cast<uns>(ind), g_curr_dep_tag, g_curr_dep_len);
  }
}

/*
  Called by:
  --- map.c -> produce ptag
  Procedure:
  --- update the produce cycle when the value is produced
*/
void map_dep_produce(Op* op) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr && op);

  // only track on-path op
  if (op->off_path)
    return;

  // assign the wake cycle of the current op 
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    int ind = op->dst_reg_file_ptag[ii];
    ASSERT(0, ind != -1 && static_cast<uns>(ind) < g_dep_graph->GetCycleRegTableSize());
    ASSERT(0, g_dep_graph->GetCycleRegTableEntry(ind) == 0 && g_dep_graph->GetNodeRegTableEntry(ind)->GetInstPC() == op->inst_info->addr);
    g_dep_graph->SetCycleRegTableEntry(ind, op->wake_cycle);
  }
}

/*
  Called by:
  --- map.c -> consume ptag
  Procedure:
  --- update the consume cycle when the value is consumed
*/
void map_dep_consume(Op* op) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr && op);

  // only track on-path op
  if (op->off_path)
    return;

  for (uns ii = 0; ii < op->oracle_info.num_srcs; ++ii) {
    if (op->oracle_info.src_info[ii].type != REG_DATA_DEP)
      continue;

    // get the ptag from the source info
    int ind = op->oracle_info.src_info[ii].reg_ptag;
    ASSERT(0, ind != -1 && static_cast<uns>(ind) < g_dep_graph->GetCycleRegTableSize());

    // get the produce cycle by the ptag
    Counter produce_cycle = g_dep_graph->GetCycleRegTableEntry(ind);

    // skip reading cycle in the very begining
    if (produce_cycle == 0)
      continue;

    // get the previous node from the node reg table by the ptag
    auto prev_node = g_dep_graph->GetNodeRegTableEntry(ind);
    ASSERT(0, prev_node != nullptr);

    // get the current node from the inst map
    auto curr_node = g_dep_graph->GetGraphInst(op->inst_info->addr);
    ASSERT(0, curr_node != nullptr);

    // get the edge of the current node by the previous node
    int prev_src_dep_ind = curr_node->FindSrcInst(prev_node->GetInstPC());
    ASSERT(0, prev_src_dep_ind != -1);
    auto edge = curr_node->GetSrcEdgeList()[static_cast<uns>(prev_src_dep_ind)];

    // update the latency
    ASSERT(0, op->exec_cycle > produce_cycle);
    Counter curr_latency = op->exec_cycle - produce_cycle;
    edge->CalDepAvgLatency(curr_latency);
  }
}

/*
  Called by:
  --- map.c -> release ptag
  Procedure:
  --- clear the dep info in the meta register table
*/
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
  g_dep_graph->SetCycleRegTableEntry(ind, 0);
}

/*
  Called by:
  --- cmp_model.c -> cmp_done
  Procedure:
  --- write the graph for doing ananysis after building
*/
void map_dep_done(void) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr);

  map_dep_write_graph();
}
