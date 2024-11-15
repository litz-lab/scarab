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
 * File         : map_rename.c
 * Author       : Y. Zhao, Litz Lab
 * Date         : 03/2024
 * Description  : Register Renaming
 ***************************************************************************************/

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "debug/debug.param.h"
#include "xed-interface.h"

#include "op.h"
#include "thread.h"
#include "node_stage.h"

#include "map_rename.h"

/**************************************************************************************/
/* Global Variables */

struct reg_file *reg_file = NULL;

extern Op invalid_op;

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MAP, ##args)


/**************************************************************************************/
/* Prototypes */

// free list operations
static inline void free_list_insert(struct reg_table_entry*);
static inline struct reg_table_entry *free_list_delete(void);

// register entry operations
static inline void reg_table_entry_read(Op*, struct reg_table_entry*);
static inline void reg_table_entry_write(Op*, struct reg_table_entry*, int, uns);

// register table operations
static inline void reg_table_init(uns);
static inline void reg_table_read_src(Op*);
static inline void reg_table_write_dst(Op*);
static inline struct reg_table_entry *reg_table_lookup_entry(uns16);
static inline struct reg_table_entry *reg_table_alloc_entry(void);
static inline void reg_table_release_entry(struct reg_table_entry*);
static inline void reg_table_remove_prev(int);
static inline void reg_table_flush_mispredict(int);
static inline void reg_table_produce_result(int);


/**************************************************************************************/
/* register free list operation */

/* push the entry to the free list */
void free_list_insert(struct reg_table_entry *entry) {
  ASSERT(0, entry->next_free == NULL);

  entry->next_free = reg_file->reg_table_ptag_to_physical->free_list->reg_free_list_head;
  reg_file->reg_table_ptag_to_physical->free_list->reg_free_list_head = entry;

  reg_file->reg_table_ptag_to_physical->free_list->reg_free_num++;
}

/* pop the entry from the free list */
struct reg_table_entry *free_list_delete(void) {
  ASSERT(0, reg_file->reg_table_ptag_to_physical->free_list->reg_free_list_head != NULL);

  struct reg_table_entry *entry = reg_file->reg_table_ptag_to_physical->free_list->reg_free_list_head;
  reg_file->reg_table_ptag_to_physical->free_list->reg_free_list_head = entry->next_free;
  entry->next_free = NULL;

  reg_file->reg_table_ptag_to_physical->free_list->reg_free_num--;

  return entry;
}


/**************************************************************************************/
/* register entry operation */

/*
  read entry:
  --- 1. fill src info from the entry
  --- 2. update not ready bit for wake up
*/
void reg_table_entry_read(Op *op, struct reg_table_entry *entry) {
  ASSERT(0, entry != NULL);
  ASSERT(0, op->op_num != entry->op_num);

  // increase src num
  uns       src_num = op->oracle_info.num_srcs++;
  Src_Info* info    = &op->oracle_info.src_info[src_num];

  // get info from the entry
  info->type       = REG_DATA_DEP;
  info->op         = entry->op;
  info->op_num     = entry->op_num;
  info->unique_num = entry->unique_num;

  // setting waking up signal
  set_not_rdy_bit(op, src_num);
}

/*
  write entry:
  --- 1. write the op into the entry
  --- 2. update the register map table to ensure the latest assignment
  --- 3. put the entry into op
*/
void reg_table_entry_write(Op* op, struct reg_table_entry *entry, int id, uns ii) {
  ASSERT(op->proc_id, entry != NULL);

  // write info to entry
  entry->op = op;
  entry->op_num = op->op_num;
  entry->unique_num = op->unique_num;
  entry->off_path = op->off_path;
  entry->reg_arch_id = id;
  entry->reg_state = REG_TABLE_ENTRY_STATE_ALLOC;

  // update the ptag of the previous regiseter with the same architectural register
  ASSERT(op->proc_id, entry->prev_same_arch_id == REG_TABLE_INVALID_REG_ID);
  entry->prev_same_arch_id = reg_file->reg_table_arch_to_ptag[entry->reg_arch_id];

  // change the ptag in the register map table to point to the latest physical register
  reg_file->reg_table_arch_to_ptag[entry->reg_arch_id] = entry->reg_ptag;

  // put the ptag of the entry into op for call back
  if (op == &invalid_op)
    return;
  ASSERT(op->proc_id, op->dst_reg_ptag[ii] == -1);
  op->dst_reg_ptag[ii] = entry->reg_ptag;
}


/**************************************************************************************/
/* register table operation */

void reg_table_init(uns reg_table_size) {
  uns ii;
  struct reg_table_entry *entry;

  reg_file = (struct reg_file *)malloc(sizeof(struct reg_file));

  /* init the register map table */
  for (ii = 0; ii < NUM_REG_IDS; ii++)
    reg_file->reg_table_arch_to_ptag[ii] = REG_TABLE_INVALID_REG_ID;
  reg_file->reg_table_ptag_to_physical = (struct reg_table *)malloc(sizeof(struct reg_table));

  /* init the free list */
  reg_file->reg_table_ptag_to_physical->free_list = (struct reg_free_list *)malloc(sizeof(struct reg_free_list));
  reg_file->reg_table_ptag_to_physical->free_list->reg_free_num = 0;
  reg_file->reg_table_ptag_to_physical->free_list->reg_free_list_head = NULL;

  /* init the physical register */
  reg_file->reg_table_ptag_to_physical->size = reg_table_size;
  reg_file->reg_table_ptag_to_physical->entries = (struct reg_table_entry *)malloc(sizeof(struct reg_table_entry) * reg_table_size);

  /* init the physical register entry */
  for (ii = 0; ii < reg_table_size; ii++) {
    entry = &reg_file->reg_table_ptag_to_physical->entries[ii];

    entry->op = &invalid_op;
    entry->op_num = 0;
    entry->unique_num = 0;
    entry->off_path = FALSE;

    entry->reg_arch_id = REG_TABLE_INVALID_REG_ID;
    entry->reg_ptag = ii;
    entry->reg_state = REG_TABLE_ENTRY_STATE_FREE;

    entry->prev_same_arch_id = REG_TABLE_INVALID_REG_ID;

    free_list_insert(entry);
  }

  /* init the register map table with aleast one physical register */
  for (ii = 0; ii < NUM_REG_IDS; ii++) {
    entry = reg_table_alloc_entry();
    reg_table_entry_write(&invalid_op, entry, ii, 0);
    ASSERT(map_data->proc_id, reg_file->reg_table_arch_to_ptag[ii] != REG_TABLE_INVALID_REG_ID);
  }
}

/*
  read src:
    1. lookup the latest physical entry from the register map table
    2. get the src_info in from entry
*/
void reg_table_read_src(Op *op) {
  uns ii;
  struct reg_table_entry *entry;

  /* do not duplicately read operand register dependency since it is already tracked during fetching */
  return;

  for (ii = 0; ii < op->table_info->num_src_regs; ii++) {
    entry = reg_table_lookup_entry(op->inst_info->srcs[ii].id);
    reg_table_entry_read(op, entry);
  }
}

/*
  write dst:
    1. allocate an physical register of register file from free list
    2. store self info into the register as destination
*/
void reg_table_write_dst(Op *op) {
  uns ii;
  struct reg_table_entry *entry;

  ASSERT(map_data->proc_id, op->table_info->num_dest_regs <= reg_file->reg_table_ptag_to_physical->free_list->reg_free_num);

  for (ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    entry = reg_table_alloc_entry();
    reg_table_entry_write(op, entry, op->inst_info->dests[ii].id, ii);
  }
}

static inline struct reg_table_entry *reg_table_lookup_entry(uns16 id) {
  int ptag = reg_file->reg_table_arch_to_ptag[id];
  ASSERT(map_data->proc_id, ptag != REG_TABLE_INVALID_REG_ID);

  struct reg_table_entry *entry = &reg_file->reg_table_ptag_to_physical->entries[ptag];
  ASSERT(map_data->proc_id, entry != NULL);
  return entry;
}

/* get the entry from the free list */
struct reg_table_entry *reg_table_alloc_entry(void) {
  return free_list_delete();
}

/* clear all the info of the entry and insert it to the free list */
void reg_table_release_entry(struct reg_table_entry *entry) {
  ASSERT(0, entry->reg_state == REG_TABLE_ENTRY_STATE_DEAD || entry->off_path);
  ASSERT(0, reg_file->reg_table_arch_to_ptag[entry->reg_arch_id] != entry->reg_ptag);

  // clear the storing info of the entry
  entry->op = &invalid_op;
  entry->op_num = 0;
  entry->unique_num = 0;
  entry->off_path = FALSE;
  entry->reg_arch_id = REG_TABLE_INVALID_REG_ID;
  entry->reg_state = REG_TABLE_ENTRY_STATE_FREE;

  // clear the tracking pointers with same architectural register id
  entry->prev_same_arch_id = REG_TABLE_INVALID_REG_ID;

  // append to free list
  free_list_insert(entry);
}

/*
  remove prev:
  --- 1. mark dead for the prev entry with same archituctural id before the committed one
  --- 2. remove the dead entry
*/
void reg_table_remove_prev(int ptag) {
  ASSERT(map_data->proc_id, REG_FILE_TYPE == REG_FILE_TYPE_REALISTIC);
  ASSERT(map_data->proc_id, ptag != REG_TABLE_INVALID_REG_ID);
  struct reg_table_entry *entry = &reg_file->reg_table_ptag_to_physical->entries[ptag];

  ASSERT(map_data->proc_id, entry != NULL);
  ASSERT(map_data->proc_id, entry->reg_state == REG_TABLE_ENTRY_STATE_PRODUCED);
  ASSERT(map_data->proc_id, reg_file->reg_table_arch_to_ptag[entry->reg_arch_id] != REG_TABLE_INVALID_REG_ID);

  // mark current register as commit when it is retire
  entry->reg_state = REG_TABLE_ENTRY_STATE_COMMIT;

  // mark the op before the committed op as dead and release it
  int prev_ptag = entry->prev_same_arch_id;
  ASSERT(map_data->proc_id, prev_ptag != REG_TABLE_INVALID_REG_ID);

  struct reg_table_entry *prev_entry = &reg_file->reg_table_ptag_to_physical->entries[prev_ptag];
  ASSERT(map_data->proc_id, prev_entry->reg_state == REG_TABLE_ENTRY_STATE_COMMIT || prev_entry->op == &invalid_op);

  prev_entry->reg_state = REG_TABLE_ENTRY_STATE_DEAD;
  reg_table_release_entry(prev_entry);
}

/*
  flush mispredict:
  --- 1. update the register map table (SRT)
  --- 2. remove the mispredicted entry
*/
void reg_table_flush_mispredict(int ptag) {
  ASSERT(map_data->proc_id, REG_FILE_TYPE == REG_FILE_TYPE_REALISTIC);
  ASSERT(map_data->proc_id, ptag != REG_TABLE_INVALID_REG_ID);
  struct reg_table_entry *entry = &reg_file->reg_table_ptag_to_physical->entries[ptag];

  ASSERT(map_data->proc_id, entry != NULL);
  ASSERT(map_data->proc_id, entry->reg_state == REG_TABLE_ENTRY_STATE_ALLOC || entry->reg_state == REG_TABLE_ENTRY_STATE_PRODUCED);
  ASSERT(map_data->proc_id, entry->off_path);

  // update register map table by prev
  ASSERT(map_data->proc_id, reg_file->reg_table_arch_to_ptag[entry->reg_arch_id] == ptag);
  reg_file->reg_table_arch_to_ptag[entry->reg_arch_id] = entry->prev_same_arch_id;

  // release register
  reg_table_release_entry(entry);
}

/*
  produce result:
  --- update the register state
*/
void reg_table_produce_result(int ptag) {
  ASSERT(map_data->proc_id, REG_FILE_TYPE == REG_FILE_TYPE_REALISTIC);
  ASSERT(map_data->proc_id, ptag != REG_TABLE_INVALID_REG_ID);
  struct reg_table_entry *entry = &reg_file->reg_table_ptag_to_physical->entries[ptag];

  ASSERT(map_data->proc_id, entry->reg_state == REG_TABLE_ENTRY_STATE_ALLOC);
  entry->reg_state = REG_TABLE_ENTRY_STATE_PRODUCED;
}


/**************************************************************************************/
/* External Calling of Register Renaming Table */

void reg_file_init(void) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;

  reg_file = NULL;
  reg_table_init(REG_TABLE_PHYSICAL_SIZE);
}

/*
  process:
  --- 1. read src: look up src register and fill the entry into src_info
  --- 2. write dst: alloc entry and store self info into register as dst
*/
void reg_file_rename(Op *op) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;

  reg_table_read_src(op);
  reg_table_write_dst(op);
}

/*
  Procedure:
  --- update the register entry state to indicate the results in the register is produced
*/
void reg_file_execute(Op *op) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;
  ASSERT(map_data->proc_id, op != NULL);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++)
    reg_table_produce_result(op->dst_reg_ptag[ii]);
}

/*
  Called by:
  --- icache_stage.c -> in ic wait for renaming
  --- icache_stage.c -> icache_issue_ops
  Procedure:
  --- check if there are enough registers
*/
Flag reg_file_available(uns stage_max_op_count) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return TRUE;
  ASSERT(map_data->proc_id, reg_file != NULL);

  return reg_file->reg_table_ptag_to_physical->free_list->reg_free_num >= MAX_DESTS * stage_max_op_count;
}

/*
  Called by:
  --- node_stage.c -> node_retire
  Procedure:
  --- free the register entry of the ops which is before this op
*/
void reg_file_commit(Op *op) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;
  ASSERT(map_data->proc_id, op != NULL);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++)
    reg_table_remove_prev(op->dst_reg_ptag[ii]);
}

/*
  Called by:
  --- thread.c -> recover_thread
  Procedure:
  --- free the register entry of the ops which is mispredicted
*/
void reg_file_recover(Counter recovery_op_num) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;

  // release the register from the youngest to the oldest
  for (Op** op_p = (Op**)list_start_tail_traversal(&td->seq_op_list);
       op_p && (*op_p)->op_num > recovery_op_num; op_p = (Op**)list_prev_element(&td->seq_op_list)) {
    ASSERT(map_data->proc_id, (*op_p)->off_path);

    // do not release the un-renamed op since dependency mapping during fetching and allocation during renaming are async
    if ((*op_p)->dst_reg_ptag[0] == REG_TABLE_INVALID_REG_ID)
      continue;

    // release misprediction register
    for (uns ii = 0; ii < (*op_p)->table_info->num_dest_regs; ii++)
      reg_table_flush_mispredict((*op_p)->dst_reg_ptag[ii]);
  }
}

