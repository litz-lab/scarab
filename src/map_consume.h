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
 * File         : map_consume.h
 * Author       : Y. Zhao, Litz Lab
 * Date         : 4/2024
 * Description  : Register Unconsumed Producers Elimination
 ***************************************************************************************/

#ifndef __MAP_CONSUME_H__
#define __MAP_CONSUME_H__

#include "libs/hash_lib.h"
#include "libs/list_lib.h"
#include "op.h"

/**************************************************************************************
 * Map Consume Functionality
 *
 * 1. Prediction
 *   1) Unconsumed Info Collection
 *      - Gather the unconsumed info identified by the signature (PC) during the renaming
 *        stage and store it in a hash
 *   2) Elimination Decision
 *      - Determine whether perform elimination based on the unconsumed count and ratio
 *        of the signature (PC)
 *
 * 2. Elimination
 *   1) Resource Bypass
 *      - Prevent an op that is predicted for elimination from proceeding to the RS and FU
 *        in the node stage
 *   2) Precommit Mechanism
 *      - Introduce a precommit state that allows an elimination op to be retired when it
 *        is overwritten by an resolved op
 *   3) Misprediction Flushing (TODO)
 *      - Establish a new type of misprediction recorvery during the renaming stage that
 *        schedules a flush when an elimination regsiter is consumed
 **************************************************************************************/

/**************************************************************************************
 * Map Consume Components
 **************************************************************************************
 * 1. Table Register Map
 *    + Desc
 *      - tracking unconsumed registers based on a hardware table
 *    + Struct
 *      - key: index of the ptag in the register file
 *      - val: meta info of the producer
 *      - len: num of entries in the register file
 *
 * 2. Signature Hash
 *    + Desc
 *      - doing training and prediction for dynamic dead code elimination
 *    + Struct
 *      - key: signature (PC) of the producer op
 *      - val: counter tracking the unconsuming info of the producer
 **************************************************************************************
 * 3. Transitivity Graph
 *    + Desc
 *      - transitively tracking unconsumed producers leveraging software methods
 *    + Struct
 *      - vertex: each op_num op
 *      - edge: dependency (dep_op --> src_op)
 **************************************************************************************/

/**************************************************************************************/
/* Unconsume Register Table */

// state of register map entry to check if it is being alloc
typedef enum Map_Consume_Reg_Map_Entry_State_enum {
  MAP_CONSUME_REG_MAP_ENTRY_STATE_FREE,
  MAP_CONSUME_REG_MAP_ENTRY_STATE_ALLOC
} Map_Consume_Reg_Map_Entry_State;

// hash entries for collecting producer information by producer signature
typedef struct Map_Consume_Hash_Entry_struct {
  Counter num_produce;
  Counter num_unconsume;
} Map_Consume_Hash_Entry;

// meta map entries for producer tracking by register file index
typedef struct Map_Consume_Reg_Map_Entry_struct {
  /* map entry meta info */
  uns64 signature;
  uns   consume_cnt;
  int   dst_ptag[MAX_DESTS];

  /* elimination target prediction flag */
  Flag  if_eliminate;

  /* info for analysis */
  Counter                         op_num;
  Flag                            off_path;
  Map_Consume_Reg_Map_Entry_State entry_state;
  Inst_Info                       inst_info;

  /* info for chain forwarding */
  Counter                         produce_cycle;
  Counter                         consume_cycle;
  uns                             consumer_dst_num;
} Map_Consume_Reg_Map_Entry;

typedef struct Map_Consume_Table_struct {
  /* Predictor */
  Map_Consume_Reg_Map_Entry* table_reg_map;       // store meta info of producers by the index of register file ptag
  uns                        table_reg_map_size;  // map size is equal to register file
  Hash_Table                 table_sign_hash;     // collect counters of producer unconsuming info by signature (PC)

  /* Elimination */
  Op*                        last_precommit_op;   // reserve the last precommit op to avoid unnecessary scan
  Flag                       if_cf_uncommit;      // eliminated op can be directly precommitted if there is not cf in the rob
} Map_Consume_Table;

/**************************************************************************************/
/* Transitivity Graph */

// vertex node for topological graph
typedef struct Map_Consume_Graph_Node_struct {
  /* op info */
  uns64     signature;
  Counter   op_num;
  Flag      off_path;
  Inst_Info inst_info;

  /* topological tracking */
  uns in_degree;                                    // counts of being consumed
  uns out_degree;                                   // numbers of register destination and memory address storing
  Flag if_trans_unconsume;
  struct Map_Consume_Graph_Node_struct* prev_reg_node[MAX_SRCS];
  struct Map_Consume_Graph_Node_struct* next_node;

  /* for validation traversal */
  Flag if_forward_validated;
  Flag if_backward_validated;
  uns dep_reg_num;
  struct Map_Consume_Graph_Node_struct* dep_reg_node[MAX_SRCS];
} Map_Consume_Graph_Node;

typedef struct Map_Consume_Graph_struct {
  List                      graph_node_set;         // set storing nodes for each op_num
  Map_Consume_Graph_Node*   curr_node;              // pointer for current processing op_num node
  Map_Consume_Graph_Node*   backtrack_queue_head;   // queue for backtracking transitive unconsumed nodes
  Map_Consume_Graph_Node**  graph_reg_map;          // meta info of producers by the index of register file ptag
  uns                       graph_reg_map_size;     // map size is equal to register file
} Map_Consume_Graph;

/**************************************************************************************/
/* Prototypes */

void map_consume_init(uns);
void map_consume_process(Op*);                  // collect statistics and process each op as a node to track transitivity

/**************************************************************************************/
/* Unconsume Register Table */

/* Predictor */
void consume_table_track_consume(Op*, uns);     // track that produced value has been consumed when register read src
void consume_table_track_produce(Op*);          // track that destination has been produced when register write dst
void consume_table_train(uns);                  // do training update when all destination registers of an op are released
void consume_table_predict(Op*);                // do prediction to determine if the op is the elimination target

/* Elimination */
void consume_table_resolve(Op*);                // set the cycle count of the elimination op
void consume_table_precommit(void);             // update the precommit states for all op
void consume_table_flush(void);                 // clear the reserving pointer that points to the off-path op
void consume_table_retire(Op*);                 // remove the pointer if it points to the retire op
Flag consume_table_mispredict(uns);             // check if the source register of an elimination target is read
void consume_table_recover(void);               // schedule a misprediction flush

/* Forwarding */
void consume_table_exec(Op*);                   // update meta entry info when the value is executed

/**************************************************************************************/
/* Transitivity Graph */

void consume_graph_track_reg_read(Op*);         // append the source op to current node for backtracking and increase in-degree
void consume_graph_track_reg_write(Op*);        // put the current node to the register map for tracking consuming and increase out-degree
void consume_graph_track_reg_release(uns);      // start backtrack if all the destination registers of this op are released
void consume_graph_validate(void);              // do forward and backward validation for the transitive graph

#endif /* #ifndef __MAP_CONSUME_H__ */