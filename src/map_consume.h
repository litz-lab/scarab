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
 * Description  : Register Unconsumed Producer Instructions Optimization
 ***************************************************************************************/

#ifndef __MAP_CONSUME_H__
#define __MAP_CONSUME_H__

#include "libs/hash_lib.h"
#include "libs/list_lib.h"
#include "op.h"

/**************************************************************************************/
// To be changed to configurable params

#define REG_CONSUME_TABLE_ENABLE            FALSE
#define REG_CONSUME_TABLE_SIGN_TYPE         REG_CONSUME_SIGH_PC

#define REG_CONSUME_TABLE_TRACE_ANALYSIS                FALSE
#define REG_CONSUME_TABLE_TRACE_ANALYSIS_CONTEXT_NUM    20

#define REG_CONSUME_TABLE_OPT_ENABLE        FALSE

#define REG_CONSUME_COUNT_THRESH        16
#define REG_CONSUME_CONF_THRESH_AMBIV   45
#define REG_CONSUME_CONF_THRESH_LIKE    55
#define REG_CONSUME_CONF_THRESH_CERT    90

/**************************************************************************************/
/* Types */

typedef enum Reg_Consume_Signiture_enum {
  REG_CONSUME_SIGH_PC,
  REG_CONSUME_SIGH_MEM,
  REG_CONSUME_SIGH_NUM
} Reg_Consume_Signiture;

typedef enum Reg_Consume_Conf_enum {
  REG_CONSUME_CONF_SKEPT,
  REG_CONSUME_CONF_AMBIV,
  REG_CONSUME_CONF_LIKE,
  REG_CONSUME_CONF_CERT
} Reg_Consume_Conf;

typedef struct Reg_Consume_Hash_Entry_struct {
  Counter num_all_produced;
  Counter num_consumed;
  Counter num_unconsumed;
} Reg_Consume_Hash_Entry;

typedef struct Reg_Consume_Node_struct {
  /* op static info */
  Counter   op_num;
  Flag      off_path;
  uns64     sign;
  Inst_Info inst_info;

  /* consuming tracking counter  */
  uns       in_degree;    // the dependency of source register read by other op
  uns       out_degree;   // the allocation of destination register of this op

  /* unconsume prediction flag */
  Flag      if_pred_unconsume;
} Reg_Consume_Node;

typedef struct Reg_Consume_Table_struct {
  /* producer tracking info */
  Reg_Consume_Node**  node_array;
  uns                 array_size;

  /* store the current node */
  Reg_Consume_Node*   cur_write_node;
  uns                 cur_write_num;

  /* track the resolved op num */
  uns unresolved_br_num;

  /* if a unconsume op is read */
  Flag if_mispredict;

  /* collect the unconsumed producer instructions by signiture */
  Hash_Table            sign_hash;
  Reg_Consume_Signiture sign_type;

  /* record the trace info between the instructions with the same dsts */
  List consume_node_list;

  /* statistics of the producer instructions */
  Counter num_all_produced;
  Counter num_consumed;
  Counter num_unconsumed;

  /* statistics of the unconsuming prediction optimization */
  Counter num_pred_all;
  Counter num_pred_unconsume;
  Counter num_read_unconsume;
  Counter num_stall_unconsume;
} Reg_Consume_Table;

/**************************************************************************************/
/* Prototypes */

void consume_table_init(uns);
void consume_table_read(Op*, uns);
void consume_table_write(Op*, uns);
void consume_table_release(uns);

void consume_table_process(Op*);
void consume_table_resolve(Op*);
Flag consume_table_mispredict(void);
void consume_table_recover(void);

void consume_table_update_stat(void);
void consume_table_print_stat(void);

#endif /* #ifndef __MAP_CONSUME_H__ */