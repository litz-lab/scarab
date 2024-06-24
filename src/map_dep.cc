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
 * Description  : Instruction Register Dependency Graph
 ***************************************************************************************/

#include "map_dep.h"

#include <vector>
#include <list>
#include <string>
#include <unordered_map>
#include <memory>
#include <sstream>
#include <iostream>
#include <fstream>
#include <functional>

extern "C" {
#include "globals/assert.h"
#include "globals/global_types.h"
#include "globals/global_defs.h"

#include "isa/isa_macros.h"
#include "xed-interface.h"
}

/**************************************************************************************/
/* Structures for Tracking */

/*
  Description:
  --- using the concatenation of PC, op code, etc. as the id of each instruction
*/
class InstructionIdentifier {
 private:
  const uns64 inst_pc_;
  const uns16 op_code_;
  const uns8 cf_type_;
  const uns8 mem_type_;
  const std::vector<uns16> src_reg_list_;
  const std::vector<uns16> dst_reg_list_;

 public:
  InstructionIdentifier()
    : inst_pc_(0), op_code_(0), cf_type_(0), mem_type_(0) {}

  explicit InstructionIdentifier(const Inst_Info& inst_info)
    : inst_pc_(inst_info.addr),
      op_code_(inst_info.table_info->true_op_type),
      cf_type_(inst_info.table_info->cf_type),
      mem_type_(inst_info.table_info->mem_type),
      src_reg_list_([&inst_info]() {
        std::vector<uns16> reg_list;
        for (uns ii = 0; ii < inst_info.table_info->num_src_regs; ++ii)
          reg_list.push_back(inst_info.srcs[ii].id);
        return reg_list;
      }()),
      dst_reg_list_([&inst_info]() {
        std::vector<uns16> reg_list;
        for (uns ii = 0; ii < inst_info.table_info->num_dest_regs; ++ii)
          reg_list.push_back(inst_info.dests[ii].id);
        return reg_list;
      }()) {}

  uns64 GetInstPC() const { return inst_pc_; }
  uns16 GetOpCode() const { return op_code_; }
  uns8 GetCfType() const { return cf_type_; }
  uns8 GetMemType() const { return mem_type_; }
  const std::vector<uns16>& GetSrcRegList() const { return src_reg_list_; }
  const std::vector<uns16>& GetDstRegList() const { return dst_reg_list_; }

  bool operator==(const InstructionIdentifier& other) const {
    return std::tie(inst_pc_, op_code_, cf_type_, mem_type_) ==
           std::tie(other.inst_pc_, other.op_code_, other.cf_type_, other.mem_type_);
  }
};

/*
  Description:
  --- using the src and the dst instruction id as the dependency id
*/
class DependencyIdentifier {
 private:
  // instruction info
  const std::shared_ptr<InstructionIdentifier> src_inst_id_;
  const std::shared_ptr<InstructionIdentifier> dst_inst_id_;
  // control flow info
  const std::list<std::shared_ptr<InstructionIdentifier>> cf_inst_id_list_;
  const uns16 cf_mask_;
  const std::string cf_seq_;

 public:
  explicit DependencyIdentifier(const std::shared_ptr<InstructionIdentifier>& src_inst_id,
                                const std::shared_ptr<InstructionIdentifier>& dst_inst_id,
                                const std::list<std::shared_ptr<InstructionIdentifier>>& cf_inst_id_list,
                                uns16 cf_mask,
                                std::string cf_seq)
    : src_inst_id_(src_inst_id), dst_inst_id_(dst_inst_id), cf_inst_id_list_(cf_inst_id_list), cf_mask_(cf_mask), cf_seq_(cf_seq) {}

  const std::shared_ptr<InstructionIdentifier>& GetSrcInstID() const { return src_inst_id_; }
  const std::shared_ptr<InstructionIdentifier>& GetDstInstID() const { return dst_inst_id_; }
  const std::list<std::shared_ptr<InstructionIdentifier>>& GetCfInstIDList() const { return cf_inst_id_list_; }
  uns16 GetCfMask() const { return cf_mask_; }
  const std::string GetCfSeq() const { return cf_seq_; }

  bool operator==(const DependencyIdentifier& other) const {
    if (std::tie(src_inst_id_, dst_inst_id_) != std::tie(other.src_inst_id_, other.dst_inst_id_))
      return false;
    return cf_inst_id_list_.size() == other.cf_inst_id_list_.size() &&
           std::equal(cf_inst_id_list_.begin(), cf_inst_id_list_.end(), other.cf_inst_id_list_.begin(),
                      [](const std::shared_ptr<InstructionIdentifier>& a, 
                         const std::shared_ptr<InstructionIdentifier>& b) {
                          return *a == *b;
                      });
  }
};

namespace std {
  // specialization of std::hash for instruction ID
  template <>
  struct hash<InstructionIdentifier> {
    std::size_t operator()(const InstructionIdentifier& inst_id) const {
      return ((std::hash<uns64>()(inst_id.GetInstPC())
               ^ (std::hash<uns16>()(inst_id.GetOpCode()) << 1)) >> 1)
               ^ (std::hash<uns8>()(inst_id.GetCfType()) << 1)
               ^ (std::hash<uns8>()(inst_id.GetMemType()) << 1);
    }
  };

  // specialization of std::hash for dependency edge ID
  template <>
  struct hash<DependencyIdentifier> {
    std::size_t operator()(const DependencyIdentifier& dep_id) const {
      auto src_inst_hash = std::hash<InstructionIdentifier>()(*dep_id.GetSrcInstID());
      auto dst_inst_hash = std::hash<InstructionIdentifier>()(*dep_id.GetDstInstID());
      std::size_t cf_inst_list_hash = 0;
      for (const auto& cf_inst_id : dep_id.GetCfInstIDList()) {
        cf_inst_list_hash ^= std::hash<InstructionIdentifier>()(*cf_inst_id);
      }
      return src_inst_hash ^ (dst_inst_hash << 1) ^ (cf_inst_list_hash << 2);
    }
  };
}

/*
  Description:
  --- the operand instance corresponding to each op_num
*/
class OperandInfo {
 private:
  const Counter op_num_;
  const std::shared_ptr<InstructionIdentifier> inst_id_;

 public:
  explicit OperandInfo(Counter op_num, const std::shared_ptr<InstructionIdentifier>& inst_id)
    : op_num_(op_num), inst_id_(inst_id) {}

  Counter GetOpNum() const { return op_num_; }
  const std::shared_ptr<InstructionIdentifier>& GetInstID() const { return inst_id_; }
};

/*
  Description:
  --- the meta info of the current producer operand in the register file by the ISA register id
*/
class MetaRegTableEntry {
 private:
  std::shared_ptr<InstructionIdentifier> inst_id_;
  Counter produce_op_num_;
  Counter produce_cycle_;

 public:
  explicit MetaRegTableEntry(const std::shared_ptr<InstructionIdentifier>& inst_id, Counter produce_op_num, Counter produce_cycle)
    : inst_id_(inst_id), produce_op_num_(produce_op_num), produce_cycle_(produce_cycle) {}

  std::shared_ptr<InstructionIdentifier> GetInstID() const { return inst_id_; }
  Counter GetProduceOpNum() const { return produce_op_num_; }
  Counter GetProduceCycle() const { return produce_cycle_; }
};

/**************************************************************************************/
/* Structures of the Edge and the Node of the Graph */

/*
  Description:
  --- the dependency relationship of register data from the src instruction to the dst instruction
  Identifier:
  --- pair<src_inst_id, dst_inst_id>
*/
class DependencyEdge {
 private:
  // instruction info
  const std::shared_ptr<DependencyIdentifier> dep_id_;
  // execution info
  Counter dep_exec_cnt_ = 0;              // the execution count of this dependency edge
  Counter dep_avg_latency_ = 0;           // the average latency of consuming

 public:
  explicit DependencyEdge(const std::shared_ptr<DependencyIdentifier>& dep_id)
    : dep_id_(dep_id) {}

  const std::shared_ptr<DependencyIdentifier>& GetDepID() const { return dep_id_; }

  void IncreaseDepExecCount() { ++dep_exec_cnt_; }
  Counter GetDepExecCount() const { return dep_exec_cnt_; }

  void CalDepAvgLatency(Counter curr_latency) { dep_avg_latency_ = (dep_avg_latency_ * (dep_exec_cnt_ - 1) + curr_latency) / dep_exec_cnt_; }
  Counter GetDepAvgLatency() const { return dep_avg_latency_; }
};

/*
  Description:
  --- the instruction with the same pc, op code, etc.
  Identifier:
  --- inst_id
*/
class InstructionNode {
 private:
  // static info
  const std::shared_ptr<InstructionIdentifier> inst_id_;
  // dynamic info
  std::vector<std::shared_ptr<OperandInfo>> op_info_list_;
  // dependency info
  std::vector<std::shared_ptr<DependencyEdge>> src_edge_list_;
  std::vector<std::shared_ptr<DependencyEdge>> dst_edge_list_;

 public:
  explicit InstructionNode(const std::shared_ptr<InstructionIdentifier>& inst_id)
    : inst_id_(inst_id) {}

  const std::shared_ptr<InstructionIdentifier>& GetInstID() const { return inst_id_; }

  void AddOpInfo(std::shared_ptr<OperandInfo> op_info) { op_info_list_.push_back(op_info); }
  const std::vector<std::shared_ptr<OperandInfo>>& GetOpInfoList() const { return op_info_list_; }

  void AddSrcEdge(const std::shared_ptr<DependencyEdge>& edge) { src_edge_list_.push_back(edge); }
  const std::vector<std::shared_ptr<DependencyEdge>>& GetSrcEdgeList() const { return src_edge_list_; }

  void AddDstEdge(const std::shared_ptr<DependencyEdge>& edge) { dst_edge_list_.push_back(edge); }
  const std::vector<std::shared_ptr<DependencyEdge>>& GetDstEdgeList() const { return dst_edge_list_; }
};

/*
  Description:
  --- node: instruction
  --- edge: dependency
*/
class DependencyGraph {
 private:
  /* register tables for recording producers to track the future consuming */
  std::vector<MetaRegTableEntry> meta_reg_table_;                 // store the meta info of the current dynamic op in the RF by the reg id
  std::list<std::shared_ptr<OperandInfo>> cf_op_info_list_;       // store all the control-flow operands by the reverse sequence for comparing

  /* map storing instruction nodes and dependency edges */
  std::unordered_map<InstructionIdentifier, std::shared_ptr<InstructionNode>> inst_map_;
  std::unordered_map<DependencyIdentifier, std::shared_ptr<DependencyEdge>> dep_map_;

 public:
  explicit DependencyGraph(uns reg_file_size)
    : meta_reg_table_(reg_file_size, MetaRegTableEntry(std::make_shared<InstructionIdentifier>(), 0, 0)) {}

  void SetMetaRegTableEntry(const uns16 index, const MetaRegTableEntry& meta_entry) { meta_reg_table_[index] = meta_entry; }
  const MetaRegTableEntry& GetMetaRegTableEntry (uns16 index) const { return meta_reg_table_[index]; }

  void InsertCfOpInfo(const std::shared_ptr<OperandInfo>& cf_op_info) { cf_op_info_list_.push_front(cf_op_info); };
  const std::list<std::shared_ptr<OperandInfo>>& GetCfOpInfoList() const { return cf_op_info_list_; };

  void AddGraphInst(const InstructionIdentifier& inst_id, const std::shared_ptr<InstructionNode>& node) { inst_map_[inst_id] = node; }
  std::shared_ptr<InstructionNode> GetGraphInst(const InstructionIdentifier& inst_id) const {
    auto it = inst_map_.find(inst_id);
    return (it != inst_map_.end()) ? it->second : nullptr;
  }
  const std::unordered_map<InstructionIdentifier, std::shared_ptr<InstructionNode>>& GetGraphInstMap() const { return inst_map_; }

  void AddGraphDep(const DependencyIdentifier& dep_id, const std::shared_ptr<DependencyEdge>& edge) { dep_map_[dep_id] = edge; }
  std::shared_ptr<DependencyEdge> GetGraphDep(const DependencyIdentifier& dep_id) const {
    auto it = dep_map_.find(dep_id);
    return (it != dep_map_.end()) ? it->second : nullptr;
  }
  const std::unordered_map<DependencyIdentifier, std::shared_ptr<DependencyEdge>>& GetGraphDepMap() const { return dep_map_; }
};

/*
  Description:
  --- the writer of the dependency graph for doing ananysis
*/
class DependencyGraphWriter {
 private:
  std::shared_ptr<DependencyGraph> dep_graph_;
  const std::string app_filename_ = "dep_graph";
  const std::string edge_list_filename_ = app_filename_ + ".el";
  const std::string adj_list_filename_ = app_filename_ + ".al";
  const std::string cf_list_filename_ = app_filename_ + ".cl";

 public:
  explicit DependencyGraphWriter(std::shared_ptr<DependencyGraph> dep_graph)
    : dep_graph_(dep_graph) {}

  std::string WriteInstRegList(const std::vector<uns16>& reg_list) const {
    std::ostringstream oss;
    oss << "<";
    for (uns ii = 0; ii < reg_list.size(); ii++) {
      oss << reg_list[ii];
      if (ii != reg_list.size() - 1)
        oss << ", ";
    }
    oss << ">";
    return oss.str();
  }

  std::string WriteInstID(const InstructionIdentifier& inst_id) const {
    std::ostringstream oss;
    oss << "{pc: 0x" << std::hex << inst_id.GetInstPC()
        << ", opcode: 0x" << inst_id.GetOpCode()
        << "(" << xed_iclass_enum_t2str(static_cast<xed_iclass_enum_t>(inst_id.GetOpCode()))
        << "), cf: " << std::dec << static_cast<uns>(inst_id.GetCfType())
        << ", mem: " << static_cast<uns>(inst_id.GetMemType())
        << ", src: " << WriteInstRegList(inst_id.GetSrcRegList())
        << ", dst: " << WriteInstRegList(inst_id.GetDstRegList())
        << "}";
    return oss.str();
  }

  std::string WriteDepID(const DependencyIdentifier& dep_id) const {
    std::ostringstream oss;
    oss << "<" << WriteInstID(*dep_id.GetSrcInstID())
        << "; " << WriteInstID(*dep_id.GetDstInstID())
        << "; cf_num: " << std::dec << dep_id.GetCfInstIDList().size()
        << "; cf_mask: 0x" << std::hex << static_cast<uns>(dep_id.GetCfMask())
        << ">";
    return oss.str();
  }

  std::string WriteDepEdge(const DependencyEdge& edge) const {
    std::ostringstream oss;
    oss << WriteDepID(*edge.GetDepID())
        << " - [cnt: " << std::dec <<  edge.GetDepExecCount()
        << ", latency: " << edge.GetDepAvgLatency()
        << "] - (cf_seq: " << edge.GetDepID()->GetCfSeq()
        << ")";
    return oss.str();
  }

  std::string WriteInstNode(const InstructionNode& node) const {
    std::ostringstream oss;
    oss << "[INST] " << WriteInstID(*node.GetInstID())
        << " - [cnt: " << std::dec << node.GetOpInfoList().size()
        << "]" << std::endl;
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

  void WriteEdgeList() const {
    std::ofstream ofs(edge_list_filename_);
    ASSERT(0, ofs.is_open());
    for (const auto& pair : dep_graph_->GetGraphDepMap())
      ofs << WriteDepEdge(*pair.second) << std::endl;
    ofs.close();
  }

  void WriteAdjList() const {
    std::ofstream ofs(adj_list_filename_);
    ASSERT(0, ofs.is_open());
    for (const auto& pair : dep_graph_->GetGraphInstMap())
      ofs << WriteInstNode(*pair.second);
    ofs.close();
  }

  void WriteCFList() const {
    std::ofstream ofs(cf_list_filename_);
    ASSERT(0, ofs.is_open());
    for (const auto& op_info : dep_graph_->GetCfOpInfoList())
      ofs << op_info->GetOpNum() << ": " << WriteInstID(*op_info->GetInstID()) << std::endl;
    ofs.close();
  }
};

/**************************************************************************************/
/* Global Variables */

// the instance of the dependency graph
std::shared_ptr<DependencyGraph> g_dep_graph = nullptr;

/**************************************************************************************/
/* Static Inline Method */

// create a new dependency edge and append the edge into the src node and the dst node
static inline std::shared_ptr<DependencyEdge> map_dep_create_edge(const DependencyIdentifier& dep_id) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr);

  // create the edge and insert it into the graph dependency map
  auto dep_edge = std::make_shared<DependencyEdge>(std::make_shared<DependencyIdentifier>(dep_id));
  g_dep_graph->AddGraphDep(dep_id, dep_edge);

  // insert the edge to the source node and the destination node
  const std::shared_ptr<InstructionNode>& prev_node = g_dep_graph->GetGraphInst(*dep_id.GetSrcInstID());
  const std::shared_ptr<InstructionNode>& curr_node = g_dep_graph->GetGraphInst(*dep_id.GetDstInstID());
  ASSERT(0, prev_node != nullptr && curr_node != nullptr);
  curr_node->AddSrcEdge(dep_edge);
  prev_node->AddDstEdge(dep_edge);

  return dep_edge;
}

// create a new instruction node and add into the graph instruction map
static inline std::shared_ptr<InstructionNode> map_dep_create_node(const InstructionIdentifier& inst_id) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && inst_id.GetInstPC() != 0);
  auto inst_node = std::make_shared<InstructionNode>(std::make_shared<InstructionIdentifier>(inst_id));
  g_dep_graph->AddGraphInst(inst_id, inst_node);
  return inst_node;
}

// get the current instruction node by looking up the graph hash map
static inline std::shared_ptr<InstructionNode> map_dep_process_curr_node(Op *op) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);

  // get the node by the identifier from the hash table
  auto inst_id = InstructionIdentifier(*op->inst_info);
  auto curr_node = g_dep_graph->GetGraphInst(inst_id);

  // create a node if it is a new identifier
  if (curr_node == nullptr)
    curr_node = map_dep_create_node(inst_id);

  // append the operand info into the node
  std::shared_ptr<OperandInfo> op_info = std::make_shared<OperandInfo>(op->op_num, std::make_shared<InstructionIdentifier>(inst_id));
  curr_node->AddOpInfo(op_info);

  // update the control flow list if there is a control flow interruption
  if (op->table_info->cf_type)
    g_dep_graph->InsertCfOpInfo(op_info);

  return curr_node;
}

// build or update dependency edges when a destination consumes a source
void map_dep_read_reg_dep(Op* op, std::shared_ptr<InstructionNode>& curr_node) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);
  ASSERT(0, curr_node != nullptr && curr_node->GetInstID() != nullptr);
  ASSERT(0, curr_node->GetInstID()->GetInstPC() == op->inst_info->addr);

  // append the source op to the current node for all source registers
  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    uns16 ind = op->inst_info->srcs[ii].id;
    const auto& prev_inst_id = g_dep_graph->GetMetaRegTableEntry(ind).GetInstID();

    // skip reading nullptr at the very beginning
    if (prev_inst_id->GetInstPC() == 0)
      continue;

    auto prev_node = g_dep_graph->GetGraphInst(*prev_inst_id);
    ASSERT(0, prev_node != nullptr);

    // get the control flow info
    Counter produce_op_num = g_dep_graph->GetMetaRegTableEntry(ind).GetProduceOpNum();
    std::list<std::shared_ptr<InstructionIdentifier>> cf_inst_id_list;
    uns16 cf_mask = 0;
    std::string cf_seq;
    for (const auto& cf_op_info : g_dep_graph->GetCfOpInfoList()) {
      if (produce_op_num > cf_op_info->GetOpNum())
        break;
      cf_inst_id_list.push_front(cf_op_info->GetInstID());
      uns8 cf_type = cf_op_info->GetInstID()->GetCfType();
      cf_seq = std::to_string(static_cast<uns>(cf_type)) + cf_seq;
      if (cf_type != 0)
        cf_mask = cf_mask | (1 << (cf_type - 1)) ;
    }

    // if cannot find the edge, create a new dependency edge
    auto dep_id = DependencyIdentifier(prev_node->GetInstID(), curr_node->GetInstID(), cf_inst_id_list, cf_mask, cf_seq);
    std::shared_ptr<DependencyEdge> edge = g_dep_graph->GetGraphDep(dep_id);
    if (edge == nullptr)
      edge = map_dep_create_edge(dep_id);

    // increase the execution count of the existing dependency edge
    edge->IncreaseDepExecCount();

    // update the average latency
    Counter produce_cycle = g_dep_graph->GetMetaRegTableEntry(ind).GetProduceCycle();
    edge->CalDepAvgLatency(op->exec_cycle - produce_cycle);
  }
}

// record the producer information for future consuming
void map_dep_write_reg_dep(Op* op, std::shared_ptr<InstructionNode>& curr_node) {
  ASSERT(0, MAP_DEP_ENABLE && g_dep_graph != nullptr && op && !op->off_path);
  ASSERT(0, curr_node != nullptr && curr_node->GetInstID() != nullptr);
  ASSERT(0, curr_node->GetInstID()->GetInstPC() == op->inst_info->addr);

  // write the current op meta info into the register map for all destination registers
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    uns16 ind = op->inst_info->dests[ii].id;
    g_dep_graph->SetMetaRegTableEntry(ind, MetaRegTableEntry(curr_node->GetInstID(), op->op_num, op->wake_cycle));
  }
}

/**************************************************************************************/
/* External Function Call */

/*
  Called by:
  --- cmp_model.c -> cmp_init
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

  // get the current instruction node by looking up the graph hash map
  auto curr_node = map_dep_process_curr_node(op);

  // build or update dependency edges when a destination consumes a source
  map_dep_read_reg_dep(op, curr_node);

  // record the producer information for the future consuming
  map_dep_write_reg_dep(op, curr_node);
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

  std::cout << std::endl;
  std::cout << "Graph Edge Num: " << g_dep_graph->GetGraphDepMap().size() << std::endl;
  std::cout << "Graph Node Num: " << g_dep_graph->GetGraphInstMap().size() << std::endl;
  std::cout << std::endl;

  if (!MAP_DEP_SERIALIZATION_ENABLE)
    return;

  DependencyGraphWriter graph_writer(g_dep_graph);
  graph_writer.WriteAdjList();
  graph_writer.WriteEdgeList();
  graph_writer.WriteCFList();
}
