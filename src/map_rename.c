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
 * Date         : 10/2024
 * Description  : Register Renaming Allocation
 ***************************************************************************************/

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "debug/debug.param.h"

#include "op.h"
#include "thread.h"
#include "node_stage.h"

#include "xed-interface.h"

#include "map_rename.h"

/**************************************************************************************/
/* Global Variables */

Reg_File *reg_file = NULL;

extern Op invalid_op;

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MAP, ##args)

/**************************************************************************************/
/* Methods */

void reg_free_list_init(Reg_Free_List *reg_free_list);
void reg_free_list_insert(Reg_Free_List *reg_free_list, Reg_Map_Entry *entry);
Reg_Map_Entry *reg_free_list_delete(Reg_Free_List *reg_free_list);

void reg_map_entry_clear(Reg_Map_Entry *entry);
void reg_map_entry_read(Reg_Map_Entry *entry, Op *op);
void reg_map_entry_write(Reg_Map_Entry *entry, Reg_Map *prev_reg_map, Op* op, int prev_reg_id);

void reg_map_init(Reg_Map *reg_map, uns reg_map_size, Reg_Map *prev_reg_map);
void reg_map_read(Reg_Map *reg_map, Op *op, int prev_reg_id);
int reg_map_alloc(Reg_Map *reg_map, Op *op, int prev_reg_id);
void reg_map_free(Reg_Map *reg_map, Reg_Map_Entry *entry);
void reg_map_write_back(Reg_Map *reg_map, int reg_id);
void reg_map_flush_mispredict(Reg_Map *reg_map, int reg_id);
void reg_map_release_last(Reg_Map *reg_map, int curr_reg_id);

void architectural_table_init(Reg_Map *reg_map, uns reg_map_size, Reg_Map *prev_reg_map);

/**************************************************************************************/
/* register free list operation */

void reg_free_list_init(Reg_Free_List *reg_free_list) {
  ASSERT(0, reg_free_list != NULL);

  reg_free_list->reg_free_num = 0;
  reg_free_list->reg_free_list_head = NULL;
}

/* push the entry to the free list */
void reg_free_list_insert(Reg_Free_List *reg_free_list, Reg_Map_Entry *entry) {
  ASSERT(0, reg_free_list != NULL && entry->next_free == NULL);

  entry->next_free = reg_free_list->reg_free_list_head;
  reg_free_list->reg_free_list_head = entry;
  ++reg_free_list->reg_free_num;
}

/* pop an entry from the free list */
Reg_Map_Entry *reg_free_list_delete(Reg_Free_List *reg_free_list) {
  ASSERT(0, reg_free_list != NULL && reg_free_list->reg_free_list_head != NULL);

  Reg_Map_Entry *entry = reg_free_list->reg_free_list_head;
  reg_free_list->reg_free_list_head = entry->next_free;
  entry->next_free = NULL;
  --reg_free_list->reg_free_num;

  return entry;
}

Reg_Free_List_Ops reg_free_list_ops = {
  .init = reg_free_list_init,
  .insert = reg_free_list_insert,
  .delete = reg_free_list_delete,
};

/**************************************************************************************/
/* register entry operation */

/* write the invalid value into the entry */
void reg_map_entry_clear(Reg_Map_Entry *entry) {
  ASSERT(0, entry != NULL && entry->curr_reg_id != REG_MAP_INVALID_REG_ID);

  entry->op = &invalid_op;
  entry->op_num = 0;
  entry->unique_num = 0;
  entry->off_path = FALSE;

  entry->reg_state = REG_MAP_ENTRY_STATE_FREE;
  entry->prev_reg_id = REG_MAP_INVALID_REG_ID;
  entry->next_reg_id = REG_MAP_INVALID_REG_ID;
  entry->last_same_arch_id = REG_MAP_INVALID_REG_ID;
}

/* fill src info from the entry and update not ready bit for wake up */
void reg_map_entry_read(Reg_Map_Entry *entry, Op *op) {
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

/* write the op info and set the prev register index of the previous table */
void reg_map_entry_write(Reg_Map_Entry *entry, Reg_Map *prev_reg_map, Op* op, int prev_reg_id) {
  ASSERT(op->proc_id, entry != NULL && entry->curr_reg_id != REG_MAP_INVALID_REG_ID && entry->next_reg_id == REG_MAP_INVALID_REG_ID);

  // write the op into the entry
  entry->op = op;
  entry->op_num = op->op_num;
  entry->unique_num = op->unique_num;
  entry->off_path = op->off_path;

  // update entry state
  entry->prev_reg_id = prev_reg_id;
  entry->reg_state = REG_MAP_ENTRY_STATE_ALLOC;

  // track the last register id with the same architectural register
  ASSERT(0, entry->last_same_arch_id == REG_MAP_INVALID_REG_ID);
  entry->last_same_arch_id = prev_reg_map->entries[entry->prev_reg_id].next_reg_id;

  DEBUG(0, "(entry write)[%lld]: prev: %d, curr: %d, next: %d, last_same: %d\n",
    entry->op_num, entry->prev_reg_id, entry->curr_reg_id, entry->next_reg_id, entry->last_same_arch_id);
}

Reg_Map_Entry_Ops reg_map_entry_ops = {
  .clear = reg_map_entry_clear,
  .read = reg_map_entry_read,
  .write = reg_map_entry_write,
};

/**************************************************************************************/
/* register file operation */

void reg_map_init(Reg_Map *reg_map, uns reg_map_size, Reg_Map *prev_reg_map) {
  // set the parent table pointer for tracking
  reg_map->prev_reg_map = prev_reg_map;

  reg_map->free_list = (Reg_Free_List *)malloc(sizeof(Reg_Free_List));
  reg_map->free_list->ops = &reg_free_list_ops;
  reg_map->free_list->ops->init(reg_map->free_list);

  reg_map->size = reg_map_size;
  reg_map->entries = (Reg_Map_Entry *)malloc(sizeof(Reg_Map_Entry) * reg_map_size);

  // init and put all the empty entries into the free list
  for (uns ii = 0; ii < reg_map->size; ii++) {
    Reg_Map_Entry *entry = &reg_map->entries[ii];
    entry->ops = &reg_map_entry_ops;
    entry->curr_reg_id = ii;
    entry->ops->clear(entry);
    reg_map->free_list->ops->insert(reg_map->free_list, entry);
  }

  // ensure each entry in the previous register map has aleast one current register map entry
  for (uns ii = 0; ii < reg_map->prev_reg_map->size; ii++) {
    if (reg_map->prev_reg_map->entries[ii].reg_state == REG_MAP_ENTRY_STATE_FREE)
      continue;

    Reg_Map_Entry *entry = reg_map->free_list->ops->delete(reg_map->free_list);
    entry->ops->write(entry, reg_map->prev_reg_map, &invalid_op, reg_map->prev_reg_map->entries[ii].curr_reg_id);
    reg_map->prev_reg_map->entries[entry->prev_reg_id].next_reg_id = entry->curr_reg_id;
  }
}

/* do not duplicately read operand register dependency since it is already tracked during fetching */
void reg_map_read(Reg_Map *reg_map, Op *op, int prev_reg_id) {
  return;
}

/*
  --- 1. allocate an current register map entry from free list
  --- 2. store self info into the register as destination
  --- 3. update the next index of the previous table
*/
int reg_map_alloc(Reg_Map *reg_map, Op *op, int prev_reg_id) {
  ASSERT(0, REG_FILE_TYPE && reg_map != NULL && op != NULL);

  // get the entry from the free list and write the metadata
  Reg_Map_Entry *entry = reg_map->free_list->ops->delete(reg_map->free_list);
  entry->ops->write(entry, reg_map->prev_reg_map, op, prev_reg_id);

  // update the previous table to ensure the latest assignment
  ASSERT(0, prev_reg_id != REG_MAP_INVALID_REG_ID && entry->prev_reg_id < reg_map->prev_reg_map->size);
  reg_map->prev_reg_map->entries[entry->prev_reg_id].next_reg_id = entry->curr_reg_id;

  return entry->curr_reg_id;
}

/* clear all the info of the entry and insert it to the free list */
void reg_map_free(Reg_Map *reg_map, Reg_Map_Entry *entry) {
  ASSERT(0, entry->reg_state == REG_MAP_ENTRY_STATE_DEAD || entry->off_path);

  // clear the entry value
  entry->ops->clear(entry);

  // append to the free list
  reg_map->free_list->ops->insert(reg_map->free_list, entry);
}

/* update the register state to indicate the value is produced */
void reg_map_write_back(Reg_Map *reg_map, int reg_id) {
  ASSERT(0, REG_FILE_TYPE && reg_id != REG_MAP_INVALID_REG_ID);
  Reg_Map_Entry *entry = &reg_map->entries[reg_id];

  ASSERT(0, entry->reg_state == REG_MAP_ENTRY_STATE_ALLOC);
  entry->reg_state = REG_MAP_ENTRY_STATE_PRODUCED;
}

/* update the SRT and remove the mispredicted entry */
void reg_map_flush_mispredict(Reg_Map *reg_map, int reg_id) {
  ASSERT(0, REG_FILE_TYPE && reg_id != REG_MAP_INVALID_REG_ID);
  Reg_Map_Entry *entry = &reg_map->entries[reg_id];

  ASSERT(0, entry != NULL && entry->off_path);
  ASSERT(0, entry->reg_state == REG_MAP_ENTRY_STATE_ALLOC || entry->reg_state == REG_MAP_ENTRY_STATE_PRODUCED);

  // update SRT by the storing last same architectural id
  ASSERT(0, reg_map->prev_reg_map->entries[entry->prev_reg_id].next_reg_id == reg_id);
  reg_map->prev_reg_map->entries[entry->prev_reg_id].next_reg_id = entry->last_same_arch_id;

  // release the current mispredicted register
  reg_map->ops->free(reg_map, entry);
}

/* mark the last entry with same archituctural id before the committed one as dead and remove it */
void reg_map_release_last(Reg_Map *reg_map, int curr_reg_id) {
  ASSERT(0, REG_FILE_TYPE && curr_reg_id != REG_MAP_INVALID_REG_ID);
  Reg_Map_Entry *entry = &reg_map->entries[curr_reg_id];

  ASSERT(0, entry != NULL && entry->reg_state == REG_MAP_ENTRY_STATE_PRODUCED);
  ASSERT(0, reg_map->prev_reg_map->entries[entry->prev_reg_id].next_reg_id != REG_MAP_INVALID_REG_ID);

  // mark current register as commit when it is retire
  entry->reg_state = REG_MAP_ENTRY_STATE_COMMIT;

  // mark the op before the committed op as dead and release it
  int last_same_arch_id = entry->last_same_arch_id;
  ASSERT(0, last_same_arch_id != REG_MAP_INVALID_REG_ID);

  Reg_Map_Entry *last_entry = &reg_map->entries[last_same_arch_id];
  ASSERT(0, last_entry->reg_state == REG_MAP_ENTRY_STATE_COMMIT || last_entry->op == &invalid_op);

  // release the last register with the same arch id
  last_entry->reg_state = REG_MAP_ENTRY_STATE_DEAD;
  reg_map->ops->free(reg_map, last_entry);
}

Reg_Map_Ops reg_map_ops = {
  .init = reg_map_init,
  .read = reg_map_read,
  .alloc = reg_map_alloc,
  .free = reg_map_free,
  .write_back = reg_map_write_back,
  .flush_mispredict = reg_map_flush_mispredict,
  .release_last = reg_map_release_last,
};

void architectural_table_init(Reg_Map *reg_map, uns reg_map_size, Reg_Map *prev_reg_map) {
  reg_map->prev_reg_map = prev_reg_map;
  reg_map->free_list = NULL;
  reg_map->size = reg_map_size;
  reg_map->entries = (Reg_Map_Entry *)malloc(sizeof(Reg_Map_Entry) * reg_map_size);

  // only need to assign the current register index for the children table to track
  for (uns ii = 0; ii < reg_map->size; ii++) {
    Reg_Map_Entry *entry = &reg_map->entries[ii];
    entry->ops = &reg_map_entry_ops;
    entry->curr_reg_id = ii;
    entry->ops->clear(entry);
    entry->reg_state = REG_MAP_ENTRY_STATE_ALLOC;
  }
}

Reg_Map_Ops architectural_table_ops = {
  .init = architectural_table_init,
};

/**************************************************************************************/
/* Renaming Func for Different Renaming Register File */

void realistic_reg_file_init(void);
Flag realistic_reg_file_available(uns stage_op_count);
void realistic_reg_file_rename(Op *op);
void realistic_reg_file_execute(Op *op);
void realistic_reg_file_recover(Counter recovery_op_num);
void realistic_reg_file_commit(Op *op);

void realistic_reg_file_init(void) {
  ASSERT(0, reg_file != NULL);

  /* the physical reg map is the children table of the arch table
     the next_reg_id of the arch table is the index of the physical reg map */
  reg_file->architectural_table = (Reg_Map *)malloc(sizeof(Reg_Map));
  reg_file->architectural_table->ops = &architectural_table_ops;
  reg_file->architectural_table->ops->init(reg_file->architectural_table, NUM_REG_IDS, NULL);

  /* the arch table is the prev_table (parent) of the physical reg map
     the prev_reg_id of the physical table is the index of the arch table */
  reg_file->physical_reg_map = (Reg_Map *)malloc(sizeof(Reg_Map));
  reg_file->physical_reg_map->ops = &reg_map_ops;
  reg_file->physical_reg_map->ops->init(reg_file->physical_reg_map, REG_MAP_PHYSICAL_SIZE, reg_file->architectural_table);
}

Flag realistic_reg_file_available(uns stage_op_count) {
  ASSERT(0, reg_file != NULL && reg_file->physical_reg_map != NULL);
  return reg_file->physical_reg_map->free_list->reg_free_num >= MAX_DESTS * stage_op_count;
}

void realistic_reg_file_rename(Op *op) {
  ASSERT(0, reg_file != NULL && reg_file->physical_reg_map != NULL);
  ASSERT(0, op->table_info->num_dest_regs <= reg_file->physical_reg_map->free_list->reg_free_num);

  /* read register map */
  for (uns ii = 0; ii < op->table_info->num_src_regs; ii++) {
    // the register dependency is not read since it is already tracked during fetching
    reg_file->physical_reg_map->ops->read(reg_file->physical_reg_map, op, op->inst_info->srcs[ii].id);
  }

  /* write register map */
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    // allocate register and write info
    int curr_reg_id = reg_file->physical_reg_map->ops->alloc(reg_file->physical_reg_map, op, op->inst_info->dests[ii].id);

    // update the register id in op
    ASSERT(0, op != &invalid_op && op->dst_reg_file_ptag[ii] == REG_MAP_INVALID_REG_ID && curr_reg_id != REG_MAP_INVALID_REG_ID);
    op->dst_reg_file_ptag[ii] = curr_reg_id;
  }
}

void realistic_reg_file_execute(Op *op) {
  ASSERT(0, reg_file != NULL && reg_file->physical_reg_map != NULL);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++)
    reg_file->physical_reg_map->ops->write_back(reg_file->physical_reg_map, op->dst_reg_file_ptag[ii]);
}

void realistic_reg_file_recover(Counter recovery_op_num) {
  ASSERT(0, reg_file != NULL && reg_file->physical_reg_map != NULL);

  // release the register from the youngest to the oldest
  for (Op** op_p = (Op**)list_start_tail_traversal(&td->seq_op_list);
       op_p && (*op_p)->op_num > recovery_op_num; op_p = (Op**)list_prev_element(&td->seq_op_list)) {
    ASSERT(map_data->proc_id, (*op_p)->off_path);

    // do not release the un-renamed op since dependency mapping during fetching and allocation during renaming are async
    if ((*op_p)->dst_reg_file_ptag[0] == REG_MAP_INVALID_REG_ID)
      continue;

    // release misprediction register
    for (uns ii = 0; ii < (*op_p)->table_info->num_dest_regs; ii++)
      reg_file->physical_reg_map->ops->flush_mispredict(reg_file->physical_reg_map, (*op_p)->dst_reg_file_ptag[ii]);
  }
}

void realistic_reg_file_commit(Op *op) {
  ASSERT(0, reg_file != NULL && reg_file->physical_reg_map != NULL);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++)
    reg_file->physical_reg_map->ops->release_last(reg_file->physical_reg_map, op->dst_reg_file_ptag[ii]);
}

/**************************************************************************************/
/* Renaming Func Table */

struct reg_file_func {
  Reg_File_Type reg_file_type;
  void (*init)(void);
  Flag (*available)(uns);
  void (*rename)(Op*);
  void (*execute)(Op*);
  void (*recover)(Counter);
  void (*commit)(Op*);
};

struct reg_file_func reg_file_func_table[REG_FILE_TYPE_NUM] = {
  {
    REG_FILE_TYPE_REALISTIC,
    realistic_reg_file_init, realistic_reg_file_available, realistic_reg_file_rename,
    realistic_reg_file_execute, realistic_reg_file_recover, realistic_reg_file_commit
  },
  {
    REG_FILE_TYPE_INFINITE, NULL, NULL, NULL, NULL, NULL, NULL
  }
};

static inline int reg_file_get_index(void) {
  int ii;
  int reg_file_index = -1;

  for (ii = 0; ii < REG_FILE_TYPE_NUM; ii++) {
    if (reg_file_func_table[ii].reg_file_type == REG_FILE_TYPE) {
      reg_file_index = ii;
      break;
    }
    if (reg_file_func_table[ii].reg_file_type == REG_FILE_TYPE_INFINITE)
      break;
  }

  ASSERT(0, reg_file_index != -1);
  return reg_file_index;
}

/**************************************************************************************/
/* External Calling of Register Renaming Table */

/*
  Called by:
  --- thread.c -> when map init
  Procedure:
  --- allocate the register map entries by the config size
*/
void reg_file_init(void) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;

  reg_file = (Reg_File *)malloc(sizeof(Reg_File));
  reg_file_func_table[reg_file_get_index()].init();
}

/*
  Called by:
  --- map_stage.c -> before an op is fetched from cache stage data
  Procedure:
  --- check if there are enough register entries
*/
Flag reg_file_available(uns stage_op_count) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return TRUE;

  return reg_file_func_table[reg_file_get_index()].available(stage_op_count);
}

/*
  Called by:
  --- map_stage.c -> when an op is fetched from cache stage data
  Procedure:
  --- look up src register and fill the entry into src_info
  --- alloc entry and store self info into register as dst
*/
void reg_file_rename(Op *op) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;

  reg_file_func_table[reg_file_get_index()].rename(op);
}

/*
  Called by:
  --- exec_stage.c -> when the op is executed
  Procedure:
  --- write back when the results in the registers is produced
*/
void reg_file_execute(Op *op) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;

  reg_file_func_table[reg_file_get_index()].execute(op);
}

/*
  Called by:
  --- thread.c -> when a misprediction occurs, it should happen before the op list flush
  Procedure:
  --- flush registers of misprediction operands
*/
void reg_file_recover(Counter recovery_op_num) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;

  reg_file_func_table[reg_file_get_index()].recover(recovery_op_num);
}

/*
  Called by:
  --- node_stage.c -> when the op is retired
  Procedure:
  --- release the last register with same architectural id
*/
void reg_file_commit(Op *op) {
  if (REG_FILE_TYPE == REG_FILE_TYPE_INFINITE)
    return;

  reg_file_func_table[reg_file_get_index()].commit(op);
}
