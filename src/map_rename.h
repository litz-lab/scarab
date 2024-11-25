/* 
 * Copyright (c) 2024 University of California, Santa Cruz
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
 * File         : map_rename.h
 * Author       : Y. Zhao, Litz Lab
 * Date         : 03/2024
 * Description  : Register Renaming
 ***************************************************************************************/

#ifndef __MAP_RENAME_H__
#define __MAP_RENAME_H__

#include "op.h"

/**************************************************************************************/
/* Constexpr */

enum reg_file_type {
  REG_FILE_TYPE_INFINITE,
  REG_FILE_TYPE_REALISTIC,
  REG_FILE_TYPE_LATE_ALLOCATION,
  REG_FILE_TYPE_NUM
};

enum reg_table_entry_state {
  REG_TABLE_ENTRY_STATE_FREE,
  REG_TABLE_ENTRY_STATE_ALLOC,
  REG_TABLE_ENTRY_STATE_PRODUCED,
  REG_TABLE_ENTRY_STATE_COMMIT,
  REG_TABLE_ENTRY_STATE_DEAD,
  REG_TABLE_ENTRY_STATE_NUM
};

enum reg_table_reg_type {
  REG_TABLE_REG_TYPE_GENERAL_PURPOSE,
  REG_TABLE_REG_TYPE_VECTOR,
  REG_TABLE_REG_TYPE_OTHER,
  REG_TABLE_REG_TYPE_NUM
};

const static int REG_TABLE_INVALID_REG_ID = -1;


/**************************************************************************************/
/* Types */

struct reg_table_entry {
  // op info (the pointer of op + the deep copy of special val)
  Op       *op;
  Counter  op_num;
  Counter  unique_num;
  Flag     off_path;

  // reg id info
  int parent_reg_id;
  int self_reg_id;
  int child_reg_id;

  // register state info
  enum reg_table_entry_state reg_state;

  // tracking the previous register use the same architectural register id
  int prev_tag_of_same_arch_id;

  // tracking free register entries
  struct reg_table_entry *next_free;

  // register entry operation
  struct reg_table_entry_ops *ops;
};

struct reg_free_list {
  // stack implementation for free list
  struct reg_table_entry *reg_free_list_head;
  uns reg_free_num;

  // free list operation
  struct reg_free_list_ops *ops;
};

struct reg_table {
  // map reg id to register entries for both speculative and committed op
  struct reg_table_entry *entries;
  uns size;

  // track all free registers
  struct reg_free_list *free_list;

  // reserve the parent table pointer for backtracking
  struct reg_table *parent_reg_table;

  // register table operation
  struct reg_table_ops *ops;
};


/**************************************************************************************/
/* Operations */

struct reg_table_entry_ops {
  void (*clear)(struct reg_table_entry *entry);
  void (*read)(struct reg_table_entry *entry, Op *op);
  void (*write)(struct reg_table_entry *entry, struct reg_table *parent_reg_table, Op *op, int parent_reg_id);
};

struct reg_free_list_ops {
  void (*init)(struct reg_free_list *reg_free_list);
  void (*free)(struct reg_free_list *reg_free_list, struct reg_table_entry *entry);
  struct reg_table_entry *(*alloc)(struct reg_free_list *reg_free_list);
};

struct reg_table_ops {
  void (*init)(struct reg_table *reg_table, uns reg_table_size, struct reg_table *parent_reg_table);
  void (*read)(struct reg_table *reg_table, Op *op, int parent_reg_id);
  int (*alloc)(struct reg_table *reg_table, Op *op, int parent_reg_id);
  void (*free)(struct reg_table *reg_table, struct reg_table_entry *entry);
  void (*write_back)(struct reg_table *reg_table, int reg_id);
  void (*flush_mispredict)(struct reg_table *reg_table, int reg_id);
  void (*release_prev)(struct reg_table *reg_table, int reg_id);
};


/**************************************************************************************/
/* External Methods */

void reg_file_init(void);                           // init the register file and its register map tables
Flag reg_file_available(uns stage_op_count);        // check if there are enough register entries
void reg_file_rename(Op *op);                       // alloc destination registers for the operand
Flag reg_file_issue(Op *op);                        // check the op before being issued into the FU
void reg_file_execute(Op *op);                      // write back into the register
void reg_file_recover(Counter recovery_op_num);     // flush registers of misprediction operands
void reg_file_commit(Op *op);                       // release the previous register with same architectural register id


#endif /* #ifndef __MAP_RENAME_H__ */
