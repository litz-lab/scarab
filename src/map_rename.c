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

#include "op.h"
#include "thread.h"

#include "xed-interface.h"

#include "map_rename.h"

/**************************************************************************************/
/* Global Variables */

Reg_Renaming_Table *rename_table = NULL;

extern Op invalid_op;

/**************************************************************************************/
/* Static prototypes */

// renaming process
static inline void rename_table_read(Op*);
static inline void rename_table_write(Op*);

// merged register file operations
static inline void            merged_reg_file_init(uns);
static inline Reg_File_Entry* merged_reg_file_lookup_entry(uns16);
static inline void            merged_reg_file_read_entry(Op*, Reg_File_Entry*);
static inline void            merged_reg_file_write_entry(Op*, Reg_File_Entry*, int, uns);
static inline Reg_File_Entry* merged_reg_file_alloc_entry(void);
static inline void            merged_reg_file_release_entry(Reg_File_Entry*);
static inline void            merged_reg_file_free_list_insert(Reg_File_Entry*);
static inline Reg_File_Entry* merged_reg_file_free_list_delete(void);

// register releasing/update
static inline void            merged_reg_file_remove_prev(int);
static inline void            merged_reg_file_flush_mispredict(int);
static inline void            merged_reg_file_produce_result(int);

/**************************************************************************************/
/* Renaming Process */

/*
  --- 1. lookup the latest physical entry from the register map table
  --- 2. get the src_info in from entry
*/
void rename_table_read(Op *op) {
  ASSERT(0, REG_RENAMING_ENABLE && rename_table != NULL && op != NULL);

  for (uns ii = 0; ii < op->table_info->num_src_regs; ii++) {
    Reg_File_Entry *entry = merged_reg_file_lookup_entry(op->inst_info->srcs[ii].id);
    merged_reg_file_read_entry(op, entry);
  }
}

/*
  --- 1. allocate an physical register of register file from free list
  --- 2. store self info into the register as destination
*/
void rename_table_write(Op *op) {
  ASSERT(0, REG_RENAMING_ENABLE && rename_table != NULL && op != NULL);
  ASSERT(0, op->table_info->num_dest_regs <= rename_table->merged_rf->reg_free_num);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    Reg_File_Entry *entry = merged_reg_file_alloc_entry();
    merged_reg_file_write_entry(op, entry, op->inst_info->dests[ii].id, ii);
  }
}

/**************************************************************************************/
/* Operations of Physical Reisger Entries in Merged Register File */

void merged_reg_file_init(uns array_size) {
  uns ii;
  Reg_File_Entry *entry;

  rename_table->merged_rf = (Merged_Reg_File *)malloc(sizeof(Merged_Reg_File));

  /* init the register map table */
  for (ii = 0; ii < NUM_REG_IDS; ii++)
    rename_table->merged_rf->reg_map_table[ii] = REG_FILE_INVALID_REG_ID;

  /* init the free list */
  rename_table->merged_rf->reg_free_num = 0;
  rename_table->merged_rf->reg_free_list_head = NULL;

  /* init the physical register */
  rename_table->merged_rf->reg_file_size = array_size;
  rename_table->merged_rf->reg_file = (Reg_File_Entry *)malloc(sizeof(Reg_File_Entry) * array_size);

  /* init the physical register entry */
  for (ii = 0; ii < array_size; ii++) {
    entry = &rename_table->merged_rf->reg_file[ii];

    entry->op = &invalid_op;
    entry->op_num = 0;
    entry->unique_num = 0;
    entry->off_path = FALSE;

    entry->reg_arch_id = REG_FILE_INVALID_REG_ID;
    entry->reg_ptag = ii;
    entry->reg_state = REG_FILE_ENTRY_STATE_FREE;

    entry->prev_same_arch_id = REG_FILE_INVALID_REG_ID;

    merged_reg_file_free_list_insert(entry);
  }

  /* init the register map table with aleast one physical register */
  for (ii = 0; ii < NUM_REG_IDS; ii++) {
    entry = merged_reg_file_alloc_entry();
    merged_reg_file_write_entry(&invalid_op, entry, ii, 0);
    ASSERT(0, rename_table->merged_rf->reg_map_table[ii] != REG_FILE_INVALID_REG_ID);
  }
}

static inline Reg_File_Entry* merged_reg_file_lookup_entry(uns16 id) {
  int ptag = rename_table->merged_rf->reg_map_table[id];
  ASSERT(0, ptag != REG_FILE_INVALID_REG_ID);

  Reg_File_Entry *entry = &rename_table->merged_rf->reg_file[ptag];
  ASSERT(0, entry != NULL);
  return entry;
}

/*
  read entry:
  --- 1. fill src info from the entry
  --- 2. update not ready bit for wake up
*/
void merged_reg_file_read_entry(Op *op, Reg_File_Entry *entry) {
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
void merged_reg_file_write_entry(Op* op, Reg_File_Entry *entry, int id, uns ii) {
  ASSERT(op->proc_id, entry != NULL);

  // write info to entry
  entry->op = op;
  entry->op_num = op->op_num;
  entry->unique_num = op->unique_num;
  entry->off_path = op->off_path;
  entry->reg_arch_id = id;
  entry->reg_state = REG_FILE_ENTRY_STATE_ALLOC;

  // update the ptag of the previous regiseter with the same architectural register
  ASSERT(op->proc_id, entry->prev_same_arch_id == REG_FILE_INVALID_REG_ID);
  entry->prev_same_arch_id = rename_table->merged_rf->reg_map_table[entry->reg_arch_id];

  // change the ptag in the register map table to point to the latest physical register
  rename_table->merged_rf->reg_map_table[entry->reg_arch_id] = entry->reg_ptag;

  // put the ptag of the entry into op for call back
  if (op == &invalid_op)
    return;
  ASSERT(op->proc_id, op->dst_reg_file_ptag[ii] == -1);
  op->dst_reg_file_ptag[ii] = entry->reg_ptag;
}

/*
  alloc entry:
  --- get the entry from the free list
*/
Reg_File_Entry *merged_reg_file_alloc_entry(void) {
  return merged_reg_file_free_list_delete();
}

/*
  release entry:
  --- clear all the info of the entry and insert it to the free list
*/
void merged_reg_file_release_entry(Reg_File_Entry *entry) {
  ASSERT(0, entry->reg_state == REG_FILE_ENTRY_STATE_DEAD || entry->off_path);
  ASSERT(0, rename_table->merged_rf->reg_map_table[entry->reg_arch_id] != entry->reg_ptag);

  // clear the storing info of the entry
  entry->op = &invalid_op;
  entry->op_num = 0;
  entry->unique_num = 0;
  entry->off_path = FALSE;
  entry->reg_arch_id = REG_FILE_INVALID_REG_ID;
  entry->reg_state = REG_FILE_ENTRY_STATE_FREE;

  // clear the tracking pointers with same architectural register id
  entry->prev_same_arch_id = REG_FILE_INVALID_REG_ID;

  // append to free list
  merged_reg_file_free_list_insert(entry);
}

/*
  free list insert:
  --- push the entry to the free list
*/
void merged_reg_file_free_list_insert(Reg_File_Entry *entry) {
  ASSERT(0, entry->next_free == NULL);

  entry->next_free = rename_table->merged_rf->reg_free_list_head;
  rename_table->merged_rf->reg_free_list_head = entry;

  rename_table->merged_rf->reg_free_num++;
}

/*
  free list delete:
  --- pop the entry from the free list
*/
Reg_File_Entry *merged_reg_file_free_list_delete(void) {
  ASSERT(0, rename_table->merged_rf->reg_free_list_head != NULL);

  Reg_File_Entry *entry = rename_table->merged_rf->reg_free_list_head;
  rename_table->merged_rf->reg_free_list_head = entry->next_free;
  entry->next_free = NULL;

  rename_table->merged_rf->reg_free_num--;

  return entry;
}

/**************************************************************************************/

/*
  remove prev:
  --- 1. mark dead for the prev entry with same archituctural id before the committed one
  --- 2. remove the dead entry
*/
void merged_reg_file_remove_prev(int ptag) {
  ASSERT(0, REG_RENAMING_ENABLE);
  ASSERT(0, ptag != REG_FILE_INVALID_REG_ID);
  Reg_File_Entry *entry = &rename_table->merged_rf->reg_file[ptag];

  ASSERT(0, entry != NULL);
  ASSERT(0, entry->reg_state == REG_FILE_ENTRY_STATE_PRODUCED);
  ASSERT(0, rename_table->merged_rf->reg_map_table[entry->reg_arch_id] != REG_FILE_INVALID_REG_ID);

  // mark current register as commit when it is retire
  entry->reg_state = REG_FILE_ENTRY_STATE_COMMIT;

  // mark the op before the committed op as dead and release it
  int prev_ptag = entry->prev_same_arch_id;
  ASSERT(0, prev_ptag != REG_FILE_INVALID_REG_ID);

  Reg_File_Entry *prev_entry = &rename_table->merged_rf->reg_file[prev_ptag];
  ASSERT(0, prev_entry->reg_state == REG_FILE_ENTRY_STATE_COMMIT || prev_entry->op == &invalid_op);

  prev_entry->reg_state = REG_FILE_ENTRY_STATE_DEAD;
  merged_reg_file_release_entry(prev_entry);
}

/*
  flush mispredict:
  --- 1. update the register map table (SRT)
  --- 2. remove the mispredicted entry
*/
void merged_reg_file_flush_mispredict(int ptag) {
  ASSERT(0, REG_RENAMING_ENABLE);
  ASSERT(0, ptag != REG_FILE_INVALID_REG_ID);
  Reg_File_Entry *entry = &rename_table->merged_rf->reg_file[ptag];

  ASSERT(0, entry != NULL);
  ASSERT(0, entry->reg_state == REG_FILE_ENTRY_STATE_ALLOC || entry->reg_state == REG_FILE_ENTRY_STATE_PRODUCED);
  ASSERT(0, entry->off_path);

  // update register map table by prev
  ASSERT(0, rename_table->merged_rf->reg_map_table[entry->reg_arch_id] == ptag);
  rename_table->merged_rf->reg_map_table[entry->reg_arch_id] = entry->prev_same_arch_id;

  // release register
  merged_reg_file_release_entry(entry);
}

/*
  produce result:
  --- update the register state
*/
void merged_reg_file_produce_result(int ptag) {
  ASSERT(0, REG_RENAMING_ENABLE);
  ASSERT(0, ptag != REG_FILE_INVALID_REG_ID);
  Reg_File_Entry *entry = &rename_table->merged_rf->reg_file[ptag];

  ASSERT(0, entry->reg_state == REG_FILE_ENTRY_STATE_ALLOC);
  entry->reg_state = REG_FILE_ENTRY_STATE_PRODUCED;
}

/**************************************************************************************/
/* External Calling of Register Renaming Table */

/*
  Called by:
  --- map.c -> when map init
  Procedure:
  --- allocate the physical register file by the config size
*/
void rename_table_init(void) {
  if (!REG_RENAMING_ENABLE)
    return;

  rename_table = NULL;
  rename_table = (Reg_Renaming_Table *)malloc(sizeof(Reg_Renaming_Table));
  rename_table->merged_rf = NULL;
  merged_reg_file_init(REG_RENAMING_PHYSICAL_REG_SIZE);
}

/*
  Called by:
  --- map_stage.c -> when an op is fetched from i-cache
  Procedure:
  --- look up src register and fill the entry into src_info
  --- alloc entry and store self info into register as dst
*/
void rename_table_process(Op *op) {
  if (!REG_RENAMING_ENABLE)
    return;
  ASSERT(0, rename_table != NULL && op != NULL);

  // read operand register dependency
  if (REG_RENAMING_COUPLE)
    rename_table_read(op);

  // allocate register and write info
  rename_table_write(op);
}


/*
  Called by:
  --- map.c -> when the op is executed
  Procedure:
  --- update the register entry state to indicate the results in the register is produced
*/
void rename_table_produce(Op *op) {
  if (!REG_RENAMING_ENABLE)
    return;
  ASSERT(0, rename_table != NULL && op != NULL);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++)
    merged_reg_file_produce_result(op->dst_reg_file_ptag[ii]);
}

/*
  Called by:
  --- map_stage.c -> before an op is fetched from i-cache
  Procedure:
  --- check if there are enough registers
*/
Flag rename_table_available(uns stage_max_op_count) {
  if (!REG_RENAMING_ENABLE)
    return TRUE;
  ASSERT(0, rename_table != NULL);

  return rename_table->merged_rf->reg_free_num >= MAX_DESTS * stage_max_op_count;
}

/*
  Called by:
  --- node_stage.c -> when the op is retired
  Procedure:
  --- free the register entry of the ops which is before this op
*/
void rename_table_commit(Op *op) {
  if (!REG_RENAMING_ENABLE)
    return;
  ASSERT(0, op != NULL);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++)
    merged_reg_file_remove_prev(op->dst_reg_file_ptag[ii]);
}

/*
  Called by:
  --- thread.c -> when a misprediction occurs, it should happen before the op list flush
  Procedure:
  --- free the register entry of the ops which is mispredicted
*/
void rename_table_recover(Counter recovery_op_num) {
  if (!REG_RENAMING_ENABLE)
    return;

  // start from the youngest op
  Op** op_p = (Op**)list_start_tail_traversal(&td->seq_op_list);

  // sync fetch and map when decouple
  if (!REG_RENAMING_COUPLE) {
    while (op_p && (*op_p)->op_num > recovery_op_num) {
      ASSERT(0, (*op_p)->off_path);
      if ((*op_p)->dst_reg_file_ptag[0] != -1)
        break;
      op_p = (Op**)list_prev_element(&td->seq_op_list);
    }
  }

  // release the register from the youngest to the oldest
  while (op_p && (*op_p)->op_num > recovery_op_num) {
    ASSERT(0, (*op_p)->off_path);

    for (uns ii = 0; ii < (*op_p)->table_info->num_dest_regs; ii++)
      merged_reg_file_flush_mispredict((*op_p)->dst_reg_file_ptag[ii]);

    op_p = (Op**)list_prev_element(&td->seq_op_list);
  }
}
