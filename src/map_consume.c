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
 * File         : map_consume.c
 * Author       : Y. Zhao, Litz Lab
 * Date         : 4/2024
 * Description  : Register Unconsumed Producer Instructions Elimination
 ***************************************************************************************/

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "thread.h"
#include "xed-interface.h"
#include "node_stage.h"
#include "map_consume.h"

/**************************************************************************************/
/* Global Variables */

Map_Consume_Table *consume_table = NULL;
Map_Consume_Graph *consume_graph = NULL;

/**************************************************************************************/
/* External variables */

extern Op invalid_op;

/**************************************************************************************/
/* Static Inline Prototypes */

// train and predict
static inline uns64 consume_table_get_signature(Op*);
static inline Flag  consume_table_if_target(uns64);
static inline void  consume_table_train_update(uns);
static inline void  consume_table_update_stat(Map_Consume_Reg_Map_Entry*);
static inline void  consume_table_release_entry(uns);

// elimination
static inline void  consume_table_resource_bypass(Op*);
static inline void  consume_table_reset_pos(void);

// print analysis info
static inline void  consume_table_print_hash_entry(void*, void*);
static inline void  consume_table_print_inst_info(Inst_Info*);

/**************************************************************************************/
// transitivity tracking

static inline Map_Consume_Graph_Node* consume_graph_create_node(Op*);
static inline Flag                    consume_graph_if_trans_unconsume(Map_Consume_Graph_Node*);
static inline void                    consume_graph_inqueue(Map_Consume_Graph_Node*);
static inline Map_Consume_Graph_Node* consume_graph_dequeue(void);
static inline void                    consume_graph_backtrack(void);
static inline void                    consume_graph_backward_validate(Map_Consume_Graph_Node*, Flag);
static inline void                    consume_graph_forward_validate(Map_Consume_Graph_Node*, Flag);

/**************************************************************************************/
/* Static Internal Methods */

/* return the designated signature (PC) as the key of hash */
static inline uns64 consume_table_get_signature(Op *op) {
  ASSERT(0, MAP_CONSUME_ENABLE && consume_table != NULL && op != NULL);

  uns64 signiture = op->inst_info->addr;
  return signiture;
}

/* determine if it is the eliminating target based on the count and ratio */
static inline Flag consume_table_if_target(uns64 sign) {
  ASSERT(0, MAP_CONSUME_ENABLE && consume_table != NULL);

  // return if it is a new signature
  Map_Consume_Hash_Entry *entry = (Map_Consume_Hash_Entry *)hash_table_access(&consume_table->table_sign_hash, sign);
  if (!entry)
    return FALSE;

  // do not regard as target when low execution count
  if (entry->num_unconsume < MAP_CONSUME_COUNT_THRESH)
    return FALSE;

  // only consider high unconsume ratio
  uns ratio = entry->num_unconsume * 100 / entry->num_produce;
  if (ratio < MAP_CONSUME_RATIO_THRESH)
    return FALSE;
  else
    return TRUE;
}

/* update data when all of the destination registers of an instruction are released */
static inline void consume_table_train_update(uns ind) {
  ASSERT(0, MAP_CONSUME_ENABLE && consume_table != NULL);

  Map_Consume_Reg_Map_Entry *entry = &consume_table->table_reg_map[ind];
  ASSERT(0, entry->entry_state != MAP_CONSUME_REG_MAP_ENTRY_STATE_FREE);

  // do not update statistics if the op is off-path
  if (entry->off_path)
    return;

  // access the entry from the hash map
  Flag if_new_hash_entry = FALSE;
  Map_Consume_Hash_Entry *hash_entry = (Map_Consume_Hash_Entry *)hash_table_access_create(
    &consume_table->table_sign_hash, entry->signature, &if_new_hash_entry
  );
  if (if_new_hash_entry) {
    hash_entry->num_produce = 0;
    hash_entry->num_unconsume = 0;
  }

  // update the statistics counters
  consume_table_update_stat(entry);

  // update the hash for prediction
  hash_entry->num_produce++;
  if (entry->consume_cnt)
    return;
  hash_entry->num_unconsume++;
}

/* update the statistics counters */
static inline void consume_table_update_stat(Map_Consume_Reg_Map_Entry *entry) {
  ASSERT(0, MAP_CONSUME_ENABLE && consume_table != NULL && entry != NULL);

  if (entry->consume_cnt == 0)
    STAT_EVENT(0, MAP_CONSUME_STAT_TABLE_REG_UNCONSUME);

  if (entry->consume_cnt == 1)
    STAT_EVENT(0, MAP_CONSUME_STAT_TABLE_REG_SINGLE);

  if (entry->consume_cnt == 1 && entry->consumer_dst_num != 0)
    STAT_EVENT(0, MAP_CONSUME_STAT_TABLE_REG_SHARED);

  if (entry->consume_cycle - entry->produce_cycle >= 1 && entry->consume_cycle - entry->produce_cycle <= 3)
    STAT_EVENT(0, MAP_CONSUME_STAT_TABLE_REG_SHORT);
}

/* release current entry after ptags of other entries with same op when the corresponding release RF entry */
static inline void consume_table_release_entry(uns ind) {
  ASSERT(0, MAP_CONSUME_ENABLE && consume_table != NULL);
  Map_Consume_Reg_Map_Entry *entry = &consume_table->table_reg_map[ind];

  entry->signature = 0;
  entry->consume_cnt = 0;
  for (uns ii = 0; ii < MAX_DESTS; ii++)
    entry->dst_ptag[ii] = -1;

  entry->if_eliminate = FALSE;
  entry->op_num = 0;
  entry->off_path = FALSE;
  entry->entry_state = MAP_CONSUME_REG_MAP_ENTRY_STATE_FREE;
  memset(&entry->inst_info, 0, sizeof(Inst_Info));

  entry->produce_cycle = 0;
  entry->consume_cycle = 0;
  entry->consumer_dst_num = 0;
}

/* directly set the cycle count of the eliminated op to indicate that the op bypasses the RS and FU */
static inline void consume_table_resource_bypass(Op *op) {
  ASSERT(0, MAP_CONSUME_ENABLE && consume_table != NULL);
  ASSERT(0, op != NULL && !op->table_info->cf_type && op->exec_count == 0 && op->if_eliminate);
  ASSERT(0, op->fetch_cycle != MAX_CTR && op->sched_cycle == MAX_CTR && op->exec_cycle == MAX_CTR && op->done_cycle == MAX_CTR);

  op->sched_cycle = op->fetch_cycle;
  op->exec_cycle  = op->fetch_cycle;
  op->done_cycle  = op->fetch_cycle;
  op->wake_cycle  = op->fetch_cycle;
}

/* clear the reserving pointer and flag when flush */
static inline void consume_table_reset_pos(void) {
  ASSERT(0, MAP_CONSUME_ENABLE && consume_table != NULL);
  consume_table->last_precommit_op = &invalid_op;
  consume_table->if_cf_uncommit = FALSE;
}

/**************************************************************************************/

static inline void consume_table_print_hash_entry(void* hash_entry, void* arg) {
  Map_Consume_Hash_Entry *entry = (Map_Consume_Hash_Entry *)hash_entry;
  printf("[%lld, %lld],\n", entry->num_produce, entry->num_unconsume);
}

static inline void consume_table_print_inst_info(Inst_Info *inst_info) {
  ASSERT(0, inst_info != NULL);

  uns16 op_code = inst_info->table_info->true_op_type;
  printf("[%s] - 0x%x. ", xed_iclass_enum_t2str(op_code), op_code);
  printf("pc: %lld. cf: %d mem: %d. ", inst_info->addr, inst_info->table_info->cf_type, inst_info->table_info->mem_type);

  printf("src#%d: ", inst_info->table_info->num_src_regs);
  for (int ii = 0; ii < inst_info->table_info->num_src_regs; ii++)
    printf("(%d); ", inst_info->srcs[ii].id);
  printf("dest#%d: ", inst_info->table_info->num_dest_regs);
  for (int ii = 0; ii < inst_info->table_info->num_dest_regs; ii++)
    printf("(%d); ", inst_info->dests[ii].id);
  printf("\n");
}

/**************************************************************************************/

/* for each unique op, create a node object */
static inline Map_Consume_Graph_Node* consume_graph_create_node(Op *op) {
  ASSERT(0, MAP_CONSUME_TRANSITIVE_ENABLE && consume_graph != NULL);

  // op static info
  Map_Consume_Graph_Node *node = (Map_Consume_Graph_Node *)malloc(sizeof(Map_Consume_Graph_Node));
  node->op_num = op->op_num;
  node->off_path = op->off_path;
  node->signature = consume_table_get_signature(op);
  node->inst_info = *op->inst_info;

  // topological sort
  node->in_degree = 0;
  node->out_degree = 0;
  node->if_trans_unconsume = FALSE;
  for (int ii = 0; ii < MAX_SRCS; ii++)
    node->prev_reg_node[ii] = NULL;
  node->next_node = NULL;

  // validation
  node->if_forward_validated = FALSE;
  node->if_backward_validated = FALSE;
  node->dep_reg_num = 0;
  for (int ii = 0; ii < MAX_SRCS; ii++)
    node->dep_reg_node[ii] = NULL;

  Map_Consume_Graph_Node **node_p = dl_list_add_tail(&consume_graph->graph_node_set);
  *node_p = node;
  return node;
};

/* determine if the node is transitive unconsumed */
static inline Flag consume_graph_if_trans_unconsume(Map_Consume_Graph_Node *node) {
  ASSERT(0, MAP_CONSUME_TRANSITIVE_ENABLE && consume_graph != NULL);
  ASSERT(0, node && node->off_path == 0);

  // return false if this op still has other destination registers
  if (node->out_degree != 0)
    return FALSE;

  // return false if this op is consumed
  if (node->in_degree != 0)
    return FALSE;

  return TRUE;
}

/* push node into queue for backtracking */
static inline void consume_graph_inqueue(Map_Consume_Graph_Node *node) {
  ASSERT(0, MAP_CONSUME_TRANSITIVE_ENABLE && consume_graph != NULL);
  ASSERT(0, node);
  node->next_node = consume_graph->backtrack_queue_head;
  consume_graph->backtrack_queue_head = node;
}

/* pop node from queue for backtracking */
static inline Map_Consume_Graph_Node* consume_graph_dequeue(void) {
  ASSERT(0, MAP_CONSUME_TRANSITIVE_ENABLE && consume_graph != NULL);
  ASSERT(0, consume_graph->backtrack_queue_head);
  Map_Consume_Graph_Node *node = consume_graph->backtrack_queue_head;
  consume_graph->backtrack_queue_head = consume_graph->backtrack_queue_head->next_node;
  return node;
}

/* topological sort for transitivity tracking */
static inline void consume_graph_backtrack(void) {
  ASSERT(0, MAP_CONSUME_TRANSITIVE_ENABLE && consume_graph != NULL);

  Map_Consume_Graph_Node *node;
  Map_Consume_Graph_Node *prev_node;
  Table_Info* table_info;

  // collect directly unconsume producers
  table_info = consume_graph->backtrack_queue_head->inst_info.table_info;
  ASSERT(0, table_info);
  if (table_info->num_dest_regs)
    STAT_EVENT(0, MAP_CONSUME_STAT_GRAPH_REG_UNCONSUME);

  while (consume_graph->backtrack_queue_head) {
    // pop node from backtracking queue
    node = consume_graph_dequeue();
    // update node trans flag
    ASSERT(0, node->if_trans_unconsume == FALSE && node->in_degree == 0 && node->out_degree == 0 && !node->off_path);
    node->if_trans_unconsume = TRUE;

    // collect transitive unconsume producers
    table_info = node->inst_info.table_info;
    ASSERT(0, table_info && table_info->num_dest_regs);
    STAT_EVENT(0, MAP_CONSUME_STAT_GRAPH_REG_TRANS);

    // push the prev trans node into queue
    for (uns ii = 0; ii < node->inst_info.table_info->num_src_regs; ii++) {
      prev_node = node->prev_reg_node[ii];
      if (prev_node == NULL)
        continue;
      // backtrack by decrease previous node in degree
      --prev_node->in_degree;
      // push node into queue when it is transitive unconsumed
      if (consume_graph_if_trans_unconsume(prev_node))
        consume_graph_inqueue(prev_node);
    }
  }
}

/* recursively validate if the transitive consuming is correct */
static inline void consume_graph_backward_validate(Map_Consume_Graph_Node* node, Flag if_unconsume) {
  ASSERT(0, MAP_CONSUME_TRANSITIVE_ENABLE && consume_graph != NULL);
  ASSERT(0, node);

  // do not replicately search
  if (node->if_backward_validated)
    return;

  // only do assert for consume when backward
  if (if_unconsume)
    return;

  // if the future node is consumed, the prev node must be consumed
  if (!if_unconsume)
    ASSERT(0, !node->if_trans_unconsume);
  node->if_backward_validated = TRUE;

  // search all previous node
  Map_Consume_Graph_Node* prev_node;
  for (uns ii = 0; ii < node->inst_info.table_info->num_src_regs; ii++) {
    prev_node = node->prev_reg_node[ii];
    if (prev_node == NULL)
      continue;
    consume_graph_backward_validate(prev_node, node->if_trans_unconsume);
  }
}

/* recursively validate if the transitive unconsuming is correct */
static inline void consume_graph_forward_validate(Map_Consume_Graph_Node* node, Flag if_unconsume) {
  ASSERT(0, MAP_CONSUME_TRANSITIVE_ENABLE && consume_graph != NULL);
  ASSERT(0, node);

  // do not replicately search
  if (node->if_forward_validated)
    return;

  // only do assert for unconsume when forward
  if (!if_unconsume)
    return;

  // if the prev node is unconsumed, the curr node must be unconsumed
  if (if_unconsume)
    ASSERT(0, node->if_trans_unconsume);
  node->if_forward_validated = TRUE;

  Map_Consume_Graph_Node* dep_node;
  for (uns ii = 0; ii < node->dep_reg_num; ii++) {
    dep_node = node->dep_reg_node[ii];
    if (dep_node == NULL)
      continue;
    consume_graph_forward_validate(dep_node, node->if_trans_unconsume);
  }
}

/**************************************************************************************/
/* External Function Call */

/*
  Called by:
  --- map.c --> init
  Procedure:
  --- init the register consume table and transitive graph
*/
void map_consume_init(uns reg_file_size) {
  if (!MAP_CONSUME_ENABLE)
    return;
  consume_table = (Map_Consume_Table *)malloc(sizeof(Map_Consume_Table));
  // unconsumed info collection for predictor
  consume_table->table_reg_map = (Map_Consume_Reg_Map_Entry *)malloc(sizeof(Map_Consume_Reg_Map_Entry) * reg_file_size);
  consume_table->table_reg_map_size = reg_file_size;
  for (uns ii = 0; ii < reg_file_size; ii++)
    consume_table_release_entry(ii);
  init_hash_table(&consume_table->table_sign_hash, "reg consume hash", NODE_TABLE_SIZE, sizeof(Map_Consume_Hash_Entry));
  // last precommit op and cf info for elimination
  consume_table_reset_pos();

  if (!MAP_CONSUME_TRANSITIVE_ENABLE)
    return;
  consume_graph = (Map_Consume_Graph *)malloc(sizeof(Map_Consume_Graph));
  init_list(&consume_graph->graph_node_set, "PRODUCE NODE LIST", sizeof(Map_Consume_Graph_Node*), TRUE);
  consume_graph->curr_node = NULL;
  consume_graph->backtrack_queue_head = NULL;
  consume_graph->graph_reg_map = (Map_Consume_Graph_Node **)malloc(sizeof(Map_Consume_Graph_Node) * reg_file_size);
  consume_graph->graph_reg_map_size = reg_file_size;
}

/*
  Called by:
  --- map.c --> process op
  Procedure:
  --- collect statistics and process each op as a node to track transitivity
*/
void map_consume_process(Op* op) {
  if (!MAP_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && op != NULL);

  STAT_EVENT(0, MAP_CONSUME_STAT_TOTAL);
  // only process on-path op
  if (op->off_path)
    return;
  STAT_EVENT(0, MAP_CONSUME_STAT_ONPATH);

  // collect producer statistics
  if (op->table_info->num_dest_regs)
    STAT_EVENT(0, MAP_CONSUME_STAT_REG_PRODUCE);

  // transitivity tracking
  if (!MAP_CONSUME_TRANSITIVE_ENABLE)
    return;
  ASSERT(0, consume_graph != NULL);

  // create node for every op
  consume_graph->curr_node = consume_graph_create_node(op);
}

/**************************************************************************************/

/*
  Called by:
  --- map.c --> read register source
  Procedure:
  --- track that produced value has been consumed
*/
void consume_table_track_consume(Op *op, uns ind) {
  if (!MAP_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && ind < consume_table->table_reg_map_size);

  Map_Consume_Reg_Map_Entry *entry = &consume_table->table_reg_map[ind];
  if (entry->entry_state == MAP_CONSUME_REG_MAP_ENTRY_STATE_FREE)
    return;

  // only track the on-path instruction
  if (op->off_path)
    return;

  // mark all dst registers from a same op as consume
  for (uns ii = 0; ii < MAX_DESTS; ii++) {
    int ind = entry->dst_ptag[ii];
    if (ind == -1)
      break;

    entry = &consume_table->table_reg_map[ind];
    ASSERT(0, entry->entry_state == MAP_CONSUME_REG_MAP_ENTRY_STATE_ALLOC);
    entry->consume_cnt++;
  }
}

/*
  Called by:
  --- map.c --> write register destination
  Procedure:
  --- track that destination has been produced
*/
void consume_table_track_produce(Op *op) {
  if (!MAP_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL);

  Map_Consume_Reg_Map_Entry *entry;
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    int ind = op->dst_reg_file_ptag[ii];
    ASSERT(0, ind != -1 && ind < consume_table->table_reg_map_size);
    entry = &consume_table->table_reg_map[ind];
    ASSERT(0, entry->entry_state == MAP_CONSUME_REG_MAP_ENTRY_STATE_FREE);

    // store metadata of op
    entry->signature = consume_table_get_signature(op);
    entry->consume_cnt = 0;
    entry->if_eliminate = op->if_eliminate;
    entry->op_num = op->op_num;
    entry->off_path = op->off_path;
    entry->entry_state = MAP_CONSUME_REG_MAP_ENTRY_STATE_ALLOC;
    entry->inst_info = *op->inst_info;

    // insert all destination ptag to all entries from same op
    for (uns jj = 0; jj < op->table_info->num_dest_regs; jj++) {
      ASSERT(0, op->dst_reg_file_ptag[jj] != -1);
      entry->dst_ptag[jj] = op->dst_reg_file_ptag[jj];
    }
  }
}

/*
  Called by:
  --- map.c --> release register
  Procedure:
  --- do training update when all destination registers of an op are released
*/
void consume_table_train(uns ind) {
  if (!MAP_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && ind < consume_table->table_reg_map_size);

  // skip releasing invalid source in the very begining
  Map_Consume_Reg_Map_Entry *entry = &consume_table->table_reg_map[ind];
  if (entry->entry_state == MAP_CONSUME_REG_MAP_ENTRY_STATE_FREE)
    return;

  // remove ptag of current entry
  int curr_ind = -1;
  for (uns ii = 0; ii < MAX_DESTS; ii++) {
    if (entry->dst_ptag[ii] == ind) {
      curr_ind = ii;
      entry->dst_ptag[ii] = -1;
      break;
    }
  }

  // remove self ptag from other destination registers of same op
  Flag if_update = TRUE;
  for (uns ii = 0; ii < MAX_DESTS; ii++) {
    if (entry->dst_ptag[ii] == -1)
      continue;

    Map_Consume_Reg_Map_Entry *entry_same_op = &consume_table->table_reg_map[entry->dst_ptag[ii]];
    ASSERT(0, entry_same_op->dst_ptag[curr_ind] == ind);
    entry_same_op->dst_ptag[curr_ind] = -1;
    if_update = FALSE;
  }

  // do update only all destination registers are released
  if (if_update)
    consume_table_train_update(ind);

  // release the current entry
  consume_table_release_entry(ind);
}

/*
  Called by:
  --- map.c --> process op
  Procedure:
  --- do prediction to determine if the op is the elimination target
*/
void consume_table_predict(Op* op) {
  if (!MAP_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && op != NULL);

  // avoid doing elimination if it is a control flow op
  if (op->table_info->cf_type)
    return;

  // do prediction based on the execution count and ratio
  if (!consume_table_if_target(consume_table_get_signature(op)))
    return;
  op->if_eliminate = TRUE;
  STAT_EVENT(0, MAP_CONSUME_STAT_ELIMINATE);
}

/*
  Called by:
  --- bp.c --> resolve
  Procedure:
  --- set the cycle count of the elimination op
*/
void consume_table_resolve(Op* op) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_ELIMINATE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && op != NULL);

  op = op->next_node;
  while (op) {
    // from the current resolved branch to the next branch
    if (op->table_info->cf_type)
      break;

    if (op->if_eliminate)
      consume_table_resource_bypass(op);

    op = op->next_node;
  }
}

/*
  Called by:
  --- node_stage.c --> before op retire
  Procedure:
  --- mark an op as precommit when its exec cycle exceeds current cycle
*/
void consume_table_precommit(void) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_ELIMINATE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL);

  Op *op = node->node_head;
  if (consume_table->last_precommit_op->op_num)
    op = consume_table->last_precommit_op->next_node;

  for (; op != NULL; op = op->next_node) {
    if (op->table_info->cf_type)
      consume_table->if_cf_uncommit = TRUE;

    if (op->if_eliminate && op->exec_cycle == -1)
      consume_table_resource_bypass(op);

    if (consume_table->if_cf_uncommit && op->exec_cycle > cycle_count)
      return;

    op->if_precommit = TRUE;
    consume_table->last_precommit_op = op;
  }

  if (op == NULL)
    consume_table->if_cf_uncommit = FALSE;
}

/*
  Called by:
  --- node_stage.c --> flush ROB
  Procedure:
  --- clear the reserving pointer that points to the off-path op
*/
void consume_table_flush(void) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_ELIMINATE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL);

  consume_table_reset_pos();
}

/*
  Called by:
  --- node_stage.c --> during op retire
  Procedure:
  --- remove the pointer if it points to the retire op
*/
void consume_table_retire(Op *op) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_ELIMINATE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL);
  ASSERT(0, op && !op->off_path && op->if_precommit);

  if (!consume_table->last_precommit_op->op_num)
    return;
  ASSERT(0, consume_table->last_precommit_op->op_num >= op->op_num);

  // reset the last precommit op when it points to the retire op
  if (consume_table->last_precommit_op->op_num == op->op_num)
    consume_table_reset_pos();
}

/*
  Called by:
  --- map.c --> read register source
  Procedure:
  --- check if the source register of an elimination target is read
*/
Flag consume_table_mispredict(uns ind) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_ELIMINATE_ENABLE)
    return FALSE;
  ASSERT(0, consume_table != NULL && ind < consume_table->table_reg_map_size);

  Map_Consume_Reg_Map_Entry *entry = &consume_table->table_reg_map[ind];
  if (!MAP_CONSUME_ELIMINATE_ENABLE || !entry->if_eliminate)
    return FALSE;
  STAT_EVENT(0, MAP_CONSUME_STAT_MISPREDICT);
  return TRUE;
}

/*
  Called by:
  --- map.c --> read register source
  Procedure:
  --- schedule a misprediction flush
*/
void consume_table_recover(void) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_ELIMINATE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL);

  // TODO
  return;
}

/*
  Called by:
  --- map.c --> produce register value
  Procedure:
  --- update meta entry info when the value is executed
*/
void consume_table_exec(Op* op) {
  if (!MAP_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && op != NULL);

  if (op->off_path)
    return;

  // update produce meta info
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    int ptag = op->dst_reg_file_ptag[ii];
    ASSERT(0, ptag != REG_FILE_INVALID_REG_ID);

    Map_Consume_Reg_Map_Entry *entry = &consume_table->table_reg_map[ptag];
    ASSERT(0, entry->entry_state != MAP_CONSUME_REG_MAP_ENTRY_STATE_FREE && entry->produce_cycle == 0 && entry->consume_cycle == 0);
    entry->produce_cycle = op->wake_cycle;
  }

  // update consume meta info
  for (uns ii = 0; ii < op->oracle_info.num_srcs; ii++) {
    if (op->oracle_info.src_info[ii].type != REG_DATA_DEP)
      continue;

    int ind = op->oracle_info.src_info[ii].reg_ptag;
    ASSERT(0, ind != -1 && ind < consume_table->table_reg_map_size);

    Map_Consume_Reg_Map_Entry *entry = &consume_table->table_reg_map[ind];
    if (entry->entry_state == MAP_CONSUME_REG_MAP_ENTRY_STATE_FREE)
      continue;
    ASSERT(0, entry->produce_cycle != 0);
    entry->consume_cycle = op->wake_cycle;
    ASSERT(0, entry->consume_cycle > entry->produce_cycle);
    entry->consumer_dst_num = op->table_info->num_dest_regs;
  }
}

/**************************************************************************************/

/*
  Called by:
  --- map.c --> read register source
  Procedure:
  --- append the source op to current node for backtracking and increase in-degree
*/
void consume_graph_track_reg_read(Op *op) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_TRANSITIVE_ENABLE)
    return;
  ASSERT(0, consume_graph != NULL);

  // only track on-path unconsumed transitive op
  if (op->off_path)
    return;

  // get the current node to append prev node pointer
  Map_Consume_Graph_Node *node = consume_graph->curr_node;
  ASSERT(0, node != NULL);

  // append the source op to current node for all source registers
  for (uns ii = 0; ii < op->oracle_info.num_srcs; ii++) {
    ASSERT(0, op->oracle_info.src_info[ii].type == REG_DATA_DEP);
    int ind = op->oracle_info.src_info[ii].reg_ptag;
    ASSERT(0, ind != -1 && ind < consume_table->table_reg_map_size);

    Map_Consume_Graph_Node *prev_node = consume_graph->graph_reg_map[ind];
    if (prev_node == NULL)
      continue;

    // increase in-degree of previous node to indicate it is consumed
    prev_node->in_degree++;
    node->prev_reg_node[ii] = prev_node;
    prev_node->dep_reg_node[node->dep_reg_num++] = node;
  }
}

/*
  Called by:
  --- map.c --> write register destination
  Procedure:
  --- put the current node to the register map for tracking consuming and increase out-degree
*/
void consume_graph_track_reg_write(Op *op) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_TRANSITIVE_ENABLE)
    return;
  ASSERT(0, consume_graph != NULL);

  // only track on-path unconsumed transitive op
  if (op->off_path)
    return;

  Map_Consume_Graph_Node *node = consume_graph->curr_node;
  ASSERT(0, node != NULL);

  // put the current node to the register map for all destination register
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    int ind = op->dst_reg_file_ptag[ii];
    ASSERT(0, ind != -1 && ind < consume_graph->graph_reg_map_size && consume_graph->graph_reg_map[ind] == NULL);

    // increase out-degree of current node to indicate it has produced
    node->out_degree++;
    consume_graph->graph_reg_map[ind] = node;
  }
}

/*
  Called by:
  --- map.c --> write register destination
  Procedure:
  --- start backtrack if all the destination registers of this op are released
*/
void consume_graph_track_reg_release(uns ind) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_TRANSITIVE_ENABLE)
    return;
  ASSERT(0, consume_graph != NULL);

  ASSERT(0, ind != -1 && ind < consume_graph->graph_reg_map_size);
  Map_Consume_Graph_Node *node = consume_graph->graph_reg_map[ind];
  consume_graph->graph_reg_map[ind] = NULL;

  if (node == NULL)
    return;
  ASSERT(0, node->off_path == 0);

  // decrease out-degree to indicate it is released
  --node->out_degree;

  // push this node into queue when it is transitive unconsumed
  if (!consume_graph_if_trans_unconsume(node))
    return;
  consume_graph_inqueue(node);

  // start backtrack the nodes in queue
  consume_graph_backtrack();
}

/*
  Called by:
  --- // TODO
  Procedure:
  --- do forward and backward validation for the transitive graph
*/
void consume_graph_validate(void) {
  if (!MAP_CONSUME_ENABLE || !MAP_CONSUME_TRANSITIVE_ENABLE)
    return;
  ASSERT(0, consume_graph != NULL);

  Map_Consume_Graph_Node** node_p;
  node_p = (Map_Consume_Graph_Node**)list_start_tail_traversal(&consume_graph->graph_node_set);
  while (node_p) {
    consume_graph_backward_validate(*node_p, (*node_p)->if_trans_unconsume);
    node_p = (Map_Consume_Graph_Node**)list_prev_element(&consume_graph->graph_node_set);
  }

  node_p = (Map_Consume_Graph_Node**)list_start_head_traversal(&consume_graph->graph_node_set);
  while (node_p) {
    consume_graph_forward_validate(*node_p, (*node_p)->if_trans_unconsume);
    node_p = (Map_Consume_Graph_Node**)list_next_element(&consume_graph->graph_node_set);
  }
}
