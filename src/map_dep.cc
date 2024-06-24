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

#include "isa/isa_macros.h"
#include "xed-interface.h"

/**************************************************************************************/
/* Structures for Tracking */

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
  --- the meta info of the current dynamic operand in the register file
  Index:
  --- ISA register id
*/
class MetaRegTableEntry {
 private:
  uns64 inst_pc_;
  std::pair<uns64, uns64> dep_meta_;
  Counter produce_cycle_;

 public:
  explicit MetaRegTableEntry(uns64 inst_pc, std::pair<uns64, uns64> dep_meta, Counter produce_cycle)
    : inst_pc_(inst_pc), dep_meta_(dep_meta), produce_cycle_(produce_cycle) {}

  uns64 GetInstPC() const { return inst_pc_; }
  std::pair<uns64, uns64> GetDepMeta() const { return dep_meta_; }
  Counter GetProduceCycle() const { return produce_cycle_; }
};

/**************************************************************************************/
/* Structures of the Edge and the Node of the Graph */

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
  --- the instruction with the same PC
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
  // dependency info
  std::vector<std::shared_ptr<DependencyEdge>> src_edge_list_;
  std::vector<std::shared_ptr<DependencyEdge>> dst_edge_list_;

 public:
  explicit InstructionNode(uns64 inst_pc, uns16 op_code)
    : inst_pc_(inst_pc), op_code_(op_code) {}

  uns64 GetInstPC() const { return inst_pc_; }
  uns64 GetOpCode() const { return op_code_; }

  void AddOpInfo(Counter op_num, const Inst_Info& inst_info) { op_info_list_.push_back(std::make_shared<OperandInfo>(op_num, inst_info)); }
  const std::vector<std::shared_ptr<OperandInfo>>& GetOpInfoList() const { return op_info_list_; }

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
  --- node: instruction
  --- edge: dependency
*/
class DependencyGraph {
 private:
  /* register tables for recording producers to track the future consuming */
  std::vector<MetaRegTableEntry> meta_reg_table_;                                       // store the meta info of the current dynamic op in the RF by the reg id
  std::vector<OperandInfo> cf_op_info_list_;                                    // store all the cf operand

  /* map storing instructions and chains */
  std::unordered_map<uns64, std::shared_ptr<InstructionNode>> inst_map_;        // map an instruction pc to the graph node of the op meta info
  std::unordered_map<uns64, std::shared_ptr<InstructionChain>> chain_map_;      // map a dependency tag to an instruction chain

 public:
  explicit DependencyGraph(uns reg_file_size)
    : meta_reg_table_(reg_file_size, MetaRegTableEntry(0, {0, 0}, 0)) {}

  void SetMetaRegTableEntry(const uns16 index, const MetaRegTableEntry& meta_entry) { meta_reg_table_[index] = meta_entry; }
  MetaRegTableEntry& GetMetaRegTableEntry(const uns16 index) { return meta_reg_table_[index]; }

  void AddCfOpInfo(const OperandInfo& cf_op_info) { cf_op_info_list_.push_back(cf_op_info); };
  Counter GetLastCfOpInfo() { return cf_op_info_list_.empty() ? 0 : cf_op_info_list_.back().GetOpNum(); };

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
    oss << "[INST] code: 0x" << std::hex << node.GetOpCode()
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
};

/**************************************************************************************/
/* Global Variables */

// the instance of the dependency graph
std::shared_ptr<DependencyGraph> g_dep_graph = nullptr;

/**************************************************************************************/
/* Static Inline Method */

// update the length, node set, live-in, and live-out of the chain
static inline void map_dep_update_chain_property(std::shared_ptr<InstructionChain>& chain,
                                                 const std::shared_ptr<InstructionNode>& prev_node,
                                                 const std::shared_ptr<InstructionNode>& curr_node,
                                                 const std::shared_ptr<DependencyEdge>& dep_edge) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && dep_edge != nullptr);

  // append the edge to the chain
  chain->AddEdge(dep_edge);

  // insert nodes into the chain
  const auto& node_list = chain->GetNodeList();
  if (std::find(node_list.begin(), node_list.end(), prev_node) == node_list.end())
    chain->AddNode(prev_node);
  if (std::find(node_list.begin(), node_list.end(), curr_node) == node_list.end())
    chain->AddNode(curr_node);

  // set the chain len
  uns64 chain_len = chain->GetChainLen();
  if (chain_len < dep_edge->GetDepLen())
    chain->SetChainLen(dep_edge->GetDepLen());

  // append live-in and live-out
  for (const auto& edge : prev_node->GetSrcEdgeList()) {
    if (edge->GetDepTag() == chain->GetChainTag())
      continue;
    chain->AddLiveIn(edge->GetDepTag());
    g_dep_graph->GetGraphChain(edge->GetDepTag())->AddLiveOut(chain->GetChainTag());
  }
}

// create a new dependency edge and if its tag is new create a new chain
static inline std::shared_ptr<DependencyEdge> map_dep_create_edge(std::shared_ptr<InstructionNode>& prev_node,
                                                                  std::shared_ptr<InstructionNode>& curr_node,
                                                                  const std::pair<uns64, uns64>& dep_meta) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && prev_node != nullptr && curr_node != nullptr);

  // insert the edge to the source node and the destination node
  auto dep_edge = std::make_shared<DependencyEdge>(
    prev_node->GetInstPC(), curr_node->GetInstPC(), dep_meta.first, dep_meta.second);
  curr_node->AddSrcEdge(dep_edge);
  prev_node->AddDstEdge(dep_edge);

  // create a new chain if it is a new tag
  auto chain = g_dep_graph->GetGraphChain(dep_meta.first);
  if (!chain) {
    chain = std::make_shared<InstructionChain>(dep_meta.first);
    g_dep_graph->AddGraphChain(chain);
  }

  // update the chain len, node set, live-in and live-out
  map_dep_update_chain_property(chain, prev_node, curr_node, dep_edge);

  return dep_edge;
}

// get the current instruction node of the current instruction node by looking up the graph hash map
static inline std::shared_ptr<InstructionNode> map_dep_process_curr_node(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);

  // create a node if it is a new signature or get the node by signature from the hash table
  auto curr_node = g_dep_graph->GetGraphInst(op->inst_info->addr);
  if (curr_node == nullptr) {
    curr_node = std::make_shared<InstructionNode>(op->inst_info->addr, op->inst_info->table_info->true_op_type);
    g_dep_graph->AddGraphInst(op->inst_info->addr, curr_node);
  }

  // append the operand info into the node
  curr_node->AddOpInfo(op->op_num, *op->inst_info);

  // update the cf meta if there is a control flow interruption
  if (op->table_info->cf_type)
    g_dep_graph->AddCfOpInfo(OperandInfo(op->op_num, *op->inst_info));

  return curr_node;
}

// set the global value of the current dependency meta for creating new edges by checking previous tags
static inline std::pair<uns64, uns64> map_dep_process_dep_meta(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);

  Flag if_diff_tag = FALSE;
  Flag if_exist_cf = FALSE;
  uns64 dep_tag = 0;
  uns64 dep_len = 1;

  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    uns16 ind = op->inst_info->srcs[ii].id;

    // skip reading nullptr at the very beginning
    if (g_dep_graph->GetMetaRegTableEntry(ind).GetInstPC() == 0)
      continue;

    // read dependency meta info from previous edge
    uns64 prev_dep_tag = g_dep_graph->GetMetaRegTableEntry(ind).GetDepMeta().first;
    uns64 prev_dep_len = g_dep_graph->GetMetaRegTableEntry(ind).GetDepMeta().second;
    ASSERT(0, prev_dep_tag != 0 && prev_dep_len != 0);

    // check if all the tag are the same
    if (dep_tag == 0)
      dep_tag = prev_dep_tag;
    else if (dep_tag != prev_dep_tag)
      if_diff_tag = TRUE;

    // check if in the same bbl
    if (prev_dep_tag <= g_dep_graph->GetLastCfOpInfo())
      if_exist_cf = TRUE;

    // increase the len from the last producer and select the critical path
    dep_len = dep_len > (prev_dep_len + 1) ? dep_len : (prev_dep_len + 1);
  }

  // if existing previous edges and all their tags are the same and in the same bbl, then inherit the tag
  uns64 curr_dep_tag = (dep_tag != 0 && !if_diff_tag && !if_exist_cf) ? dep_tag : op->op_num;
  uns64 curr_dep_len = (dep_tag != 0 && !if_diff_tag && !if_exist_cf) ? dep_len : 1;

  return {curr_dep_tag, curr_dep_len};
}

// build or update dependency edges when a destination consumes a source
void map_dep_read_reg_dep(Op* op, std::shared_ptr<InstructionNode>& curr_node, const std::pair<uns64, uns64>& dep_meta) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);
  ASSERT(0, curr_node != nullptr && curr_node->GetInstPC() == op->inst_info->addr);

  // append the source op to the current node for all source registers
  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    uns16 ind = op->inst_info->srcs[ii].id;
    auto prev_node = g_dep_graph->GetGraphInst(g_dep_graph->GetMetaRegTableEntry(ind).GetInstPC());

    // skip reading nullptr at the very beginning
    if (prev_node == nullptr)
      continue;

    // if cannot find prev node, create a new dependency edge
    int prev_src_dep_ind = curr_node->FindSrcInst(prev_node->GetInstPC());
    std::shared_ptr<DependencyEdge> edge;
    if (prev_src_dep_ind == -1)
      edge = map_dep_create_edge(prev_node, curr_node, dep_meta);
    else
      edge = curr_node->GetSrcEdgeList()[static_cast<uns>(prev_src_dep_ind)];

    // increase the execution count of the existing dependency edge and chain
    edge->IncreaseDepExecCount();
    g_dep_graph->GetGraphChain(edge->GetDepTag())->IncreaseChainExecCount();

    // update the average latency
    Counter produce_cycle = g_dep_graph->GetMetaRegTableEntry(ind).GetProduceCycle();
    edge->CalDepAvgLatency(op->exec_cycle - produce_cycle);
  }
}

// record the producer information for future consuming
void map_dep_write_reg_dep(Op* op, std::shared_ptr<InstructionNode>& curr_node, std::pair<uns64, uns64>& dep_meta) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);
  ASSERT(0, curr_node != nullptr && curr_node->GetInstPC() == op->inst_info->addr);

  // write the current op meta info into the register map for all destination registers
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    uns16 ind = op->inst_info->dests[ii].id;
    g_dep_graph->SetMetaRegTableEntry(ind, MetaRegTableEntry(curr_node->GetInstPC(), dep_meta, op->wake_cycle));
  }
}

/**************************************************************************************/
/* External Function Call */

/*
  Called by:
  --- map.c -> init reg map
  Procedure:
  --- create the graph instance and init the register map table
*/
void map_dep_init(void) {
  if (!MAP_DEP_ENABLE)
    return;
  g_dep_graph = std::make_shared<DependencyGraph>(NUM_REG_IDS);
}

/*
  Called by:
  --- node_stage.c -> op commit
  Procedure:
  --- process each instruction as a graph node to track dependency
*/
void map_dep_process(Op* op) {
  if (!MAP_DEP_ENABLE)
    return;
  ASSERT(0, g_dep_graph != nullptr && op && !op->off_path);

  // get the current instruction node of the current instruction node by looking up the graph hash map
  auto curr_node = map_dep_process_curr_node(op);

  // calculate the current dependency meta for creating new edges by checking previous tags
  auto dep_meta = map_dep_process_dep_meta(op);

  // build or update dependency edges when a destination consumes a source
  map_dep_read_reg_dep(op, curr_node, dep_meta);

  // record the producer information for the future consuming
  map_dep_write_reg_dep(op, curr_node, dep_meta);
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

  uns64 total_edges = 0;
  for (const auto& pair : g_dep_graph->GetGraphChainMap())
    total_edges += pair.second->GetEdgeList().size();

  std::cout << std::endl;
  std::cout << "Graph Edge Num: " << total_edges << std::endl;
  std::cout << "Graph Node Num: " << g_dep_graph->GetGraphInstMap().size() << std::endl;
  std::cout << "Graph Chain Num: " << g_dep_graph->GetGraphChainMap().size() << std::endl;
  std::cout << std::endl;

  if (!MAP_DEP_SERIALIZATION_ENABLE)
    return;

  DependencyGraphWriter graph_writer(g_dep_graph);
  graph_writer.WriteInstMap();
  graph_writer.WriteChainMap();
}
