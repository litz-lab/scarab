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
 * Author       : Yinyuan Zhao (Litz Lab)
 * Date         : 03/2024, 07/2025
 * Description  : Register Renaming
 ***************************************************************************************/

#ifndef __MAP_RENAME_H__
#define __MAP_RENAME_H__

#include "op.h"

/**************************************************************************************/
/* Constexpr */

enum reg_renaming_scheme {
  REG_RENAMING_SCHEME_INFINITE,
  REG_RENAMING_SCHEME_REALISTIC,
  REG_RENAMING_SCHEME_LATE_ALLOCATION,
  REG_RENAMING_SCHEME_EARLY_RELEASE_SPEC,
  REG_RENAMING_SCHEME_EARLY_RELEASE_NONSPEC,
  REG_RENAMING_SCHEME_EARLY_RELEASE_LASTUSE,
  REG_RENAMING_SCHEME_EARLY_RELEASE_ATOMIC,
  REG_RENAMING_SCHEME_EARLY_RELEASE_NONSPEC_ATOMIC,
  REG_RENAMING_SCHEME_NUM
};

enum reg_table_entry_state {
  REG_TABLE_ENTRY_STATE_FREE,
  REG_TABLE_ENTRY_STATE_ALLOC,
  REG_TABLE_ENTRY_STATE_PRODUCED,
  REG_TABLE_ENTRY_STATE_COMMIT,
  REG_TABLE_ENTRY_STATE_NUM
};

enum reg_file_reg_type {
  REG_FILE_REG_TYPE_GENERAL_PURPOSE,
  REG_FILE_REG_TYPE_VECTOR,
  REG_FILE_REG_TYPE_NUM,
};

const static int REG_TABLE_REG_ID_INVALID = -1;
const static int REG_TABLE_TYPE_INVALID = -1;
const static int REG_FILE_REG_TYPE_OTHER = -1;
const static uns REG_RENAMING_SCHEME_LATE_ALLOCATION_RESERVE_NUM = 1;
const static int REG_RENAMING_SCHEME_EARLY_RELEASE_PENDING_CONSUMED_MAX = 7;

// CPUID instruction will need 4 int register destination
const static uns REG_FILE_MAX_DESTS[] = {4, 2};

/**************************************************************************************/
/* Types */

struct reg_table_entry {
  // op info (the pointer of op + the deep copy of special val)
  Op *op;
  Counter op_num;
  Counter unique_num;
  Flag off_path;

  // reg id info
  int parent_reg_id;
  int self_reg_id;
  int child_reg_id;

  // type info
  int reg_type;        // INT or VEC
  int reg_table_type;  // arch, physical, or virtual

  // register state info
  enum reg_table_entry_state reg_state;

  // tracking free register entries
  struct reg_table_entry *next_free;

  // register entry operation
  struct reg_table_entry_ops *ops;

  // lifecycle counter
  Counter allocated_cycle;
  Counter produced_cycle;
  Counter spec_consumed_cycle;
  Counter onpath_consumed_cycle;
  Counter redefined_cycle;
  Counter spec_release_cycle;
  Counter nonspec_release_cycle;

  // reference num for shared physical registers
  int num_refs;  // the number of in-flight operands using this entry

  // consumer counter
  int onpath_consumers_num;   // the number of registered (at rename) consumers of a registers
  int onpath_consumed_count;  // the number of issued (at execute) consumers of a register
  int onpath_consumed_dist;   // the cyclecout between the first consumer and the last consumer

  // metadata for spec/nonspec early release
  Flag redefined_rename;     // indicate if it is overwritten by an instruction with the same arch id during renaming
  Flag redefined_precommit;  // indicate if the redefine-instruction is precommitted

  // metadata for last-use early release
  Counter lastuse_op_num;
  Flag lastuse_committed;

  /* metadata for atomic early release */
  // only used for software assertion and stat collection
  Flag is_branch;
  Flag is_except;
  Flag is_atomic;

  // 3-bit consumer counter maintained by hardware
  int atomic_pending_consumed;
};

struct reg_free_list {
  // stack implementation for free list
  struct reg_table_entry *reg_free_list_head;
  uns reg_free_num;

  // free list operation
  struct reg_free_list_ops *ops;
};

struct reg_table {
  // the type of the corresponding reg file and reg table
  int reg_type;        // INT or FP
  int reg_table_type;  // arch, physical, or virtual

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

struct reg_checkpoint {
  // metadata for validation of the special checkpoint mechanism in Scarab
  Flag is_valid;

  // only map on-path op for recovery
  struct reg_table_entry *entries;
};

struct reg_file {
  /* properties */
  int reg_type;
  struct reg_checkpoint *reg_checkpoint;

  /* register table instances */
  struct reg_table *reg_table[REG_TABLE_TYPE_NUM];
};

typedef struct reg_file Reg_File;

/**************************************************************************************/
/* Operations */

struct reg_table_entry_ops {
  void (*clear)(struct reg_table_entry *entry);
  void (*read)(struct reg_table_entry *entry, Op *op);
  void (*write)(struct reg_table_entry *entry, Op *op, int parent_reg_id);
  void (*consume)(struct reg_table_entry *entry, Op *op);
  void (*produce)(struct reg_table_entry *entry, Op *op);
};

struct reg_free_list_ops {
  void (*init)(struct reg_free_list *reg_free_list);
  void (*free)(struct reg_free_list *reg_free_list, struct reg_table_entry *entry);
  struct reg_table_entry *(*alloc)(struct reg_free_list *reg_free_list);
};

struct reg_table_ops {
  void (*init)(struct reg_table *reg_table, struct reg_table *parent_reg_table, uns reg_table_size, int reg_type,
               int reg_table_type);
  int (*read)(struct reg_table *reg_table, Op *op, int parent_reg_id);
  int (*alloc)(struct reg_table *reg_table, Op *op, int parent_reg_id);
  void (*free)(struct reg_table *reg_table, struct reg_table_entry *entry);
  void (*consume)(struct reg_table *reg_table, int reg_id, Op *op);
  void (*produce)(struct reg_table *reg_table, int reg_id, Op *op);
};

/**************************************************************************************/
/* External Methods */

void reg_file_init(void);                     // init the register file and its register map tables
Flag reg_file_available(uns stage_op_count);  // check if there are enough register entries
void reg_file_rename(Op *op);                 // alloc destination registers for the operand
Flag reg_file_issue(Op *op);                  // check the op before being issued into the FU
void reg_file_consume(Op *op);                // consume the src registers
void reg_file_produce(Op *op);                // write back the dst registers
void reg_file_recover(Op *op);                // flush registers of misprediction operands
void reg_file_precommit(Op *op);              // update the register metadata when an op is non-spec
void reg_file_commit(Op *op);                 // release the previous register with same architectural register id

#endif /* #ifndef __MAP_RENAME_H__ */
