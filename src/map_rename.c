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
#include "isa/isa.h"
#include "isa/isa_macros.h"
#include "xed-interface.h"

#include "op.h"
#include "thread.h"
#include "node_stage.h"

#include "map_rename.h"

/**************************************************************************************/
/* Extern Definition */

extern Op invalid_op;

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MAP, ##args)


/**************************************************************************************/
/* Prototypes */

// free list operations
void reg_free_list_init(struct reg_free_list *reg_free_list);
void reg_free_list_free(struct reg_free_list *reg_free_list, struct reg_table_entry *entry);
struct reg_table_entry *reg_free_list_alloc(struct reg_free_list *reg_free_list);

// register entry operations
void reg_table_entry_clear(struct reg_table_entry *entry);
void reg_table_entry_read(struct reg_table_entry *entry, Op *op);
void reg_table_entry_write(struct reg_table_entry *entry, struct reg_table *parent_reg_table, Op *op, int parent_reg_id);

// register table operations
void reg_table_init(struct reg_table *reg_table, uns reg_table_size, struct reg_table *parent_reg_table);
void reg_table_read(struct reg_table *reg_table, Op *op, int parent_reg_id);
int reg_table_alloc(struct reg_table *reg_table, Op *op, int parent_reg_id);
void reg_table_free(struct reg_table *reg_table, struct reg_table_entry *entry);
void reg_table_write_back(struct reg_table *reg_table, int self_reg_id);
void reg_table_flush_mispredict(struct reg_table *reg_table, int self_reg_id);
void reg_table_release_prev(struct reg_table *reg_table, int self_reg_id);

// special init func for the architectural table
void reg_table_arch_init(struct reg_table *reg_table, uns reg_table_size, struct reg_table *parent_reg_table);


/**************************************************************************************/
/* Inline Methods */

static inline enum reg_table_reg_type reg_file_get_reg_type(int reg_id) {
  if (reg_id >= REG_RAX && reg_id < REG_CS)
    return REG_TABLE_REG_TYPE_GENERAL_PURPOSE;

  if (reg_id >= REG_ZMM0 && reg_id < REG_K0)
    return REG_TABLE_REG_TYPE_VECTOR;

  return REG_TABLE_REG_TYPE_OTHER;
}

static inline void reg_file_debug_print_op(Op* op, int state) {
  ASSERT(0, op != NULL);
  if (op->table_info->num_dest_regs == 0)
    return;

  Inst_Info *inst_info = op->inst_info;
  uns16 op_code = inst_info->table_info->true_op_type;

  printf("[%d]\n", state);
  printf("op_num: %lld, off_path: %d, ", op->op_num, op->off_path);
  printf("pc: %lld, opcode: 0x%x(%s), cf: %d, mem: %d\n", inst_info->addr, op_code, xed_iclass_enum_t2str(op_code), inst_info->table_info->cf_type, inst_info->table_info->mem_type);

  printf("src#%d: <", inst_info->table_info->num_src_regs);
  for (int ii = 0; ii < inst_info->table_info->num_src_regs; ii++)
    printf("%d, ", inst_info->srcs[ii].id);
  printf(">, dest#%d: <", inst_info->table_info->num_dest_regs);
  for (int ii = 0; ii < inst_info->table_info->num_dest_regs; ii++)
    printf("%d, ", inst_info->dests[ii].id);
  printf(">\n");

  printf("ptag#%d: <", inst_info->table_info->num_dest_regs);
  for (int ii = 0; ii < inst_info->table_info->num_dest_regs; ii++)
    printf("%d, ", op->dst_reg_ptag[ii]);
  printf(">\n");
}


/**************************************************************************************/
/* register free list operation */

void reg_free_list_init(struct reg_free_list *reg_free_list) {
  ASSERT(0, reg_free_list != NULL);

  reg_free_list->reg_free_num = 0;
  reg_free_list->reg_free_list_head = NULL;
}

/* push the entry to the free list */
void reg_free_list_free(struct reg_free_list *reg_free_list, struct reg_table_entry *entry) {
  ASSERT(0, reg_free_list != NULL && entry->next_free == NULL);

  entry->next_free = reg_free_list->reg_free_list_head;
  reg_free_list->reg_free_list_head = entry;
  ++reg_free_list->reg_free_num;
}

/* pop the entry from the free list */
struct reg_table_entry *reg_free_list_alloc(struct reg_free_list *reg_free_list) {
  ASSERT(0, reg_free_list != NULL && reg_free_list->reg_free_list_head != NULL);

  struct reg_table_entry *entry = reg_free_list->reg_free_list_head;
  reg_free_list->reg_free_list_head = entry->next_free;
  entry->next_free = NULL;
  --reg_free_list->reg_free_num;

  return entry;
}

struct reg_free_list_ops reg_free_list_ops = {
  .init = reg_free_list_init,
  .free = reg_free_list_free,
  .alloc = reg_free_list_alloc,
};


/**************************************************************************************/
/* register entry operation */

/* set the invalid value into the entry */
void reg_table_entry_clear(struct reg_table_entry *entry) {
  ASSERT(0, entry != NULL && entry->self_reg_id != REG_TABLE_INVALID_REG_ID);

  entry->op = &invalid_op;
  entry->op_num = 0;
  entry->unique_num = 0;
  entry->off_path = FALSE;

  entry->reg_state = REG_TABLE_ENTRY_STATE_FREE;
  entry->parent_reg_id = REG_TABLE_INVALID_REG_ID;
  entry->child_reg_id = REG_TABLE_INVALID_REG_ID;
  entry->prev_tag_of_same_arch_id = REG_TABLE_INVALID_REG_ID;
}

/* fill src info from the entry and update not ready bit for wake up */
void reg_table_entry_read(struct reg_table_entry *entry, Op *op) {
  ASSERT(0, entry != NULL && op->op_num != entry->op_num);

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

/* update reg_table entry by setting its key (lookup reg_id) and value (tag and op whose dest is assigned to reg_id) */
void reg_table_entry_write(struct reg_table_entry *entry, struct reg_table *parent_reg_table, Op *op, int parent_reg_id) {
  ASSERT(op->proc_id, entry != NULL && entry->self_reg_id != REG_TABLE_INVALID_REG_ID && entry->child_reg_id == REG_TABLE_INVALID_REG_ID);

  // write the op into the entry
  entry->op = op;
  entry->op_num = op->op_num;
  entry->unique_num = op->unique_num;
  entry->off_path = op->off_path;

  // update the entry meta data
  entry->parent_reg_id = parent_reg_id;
  entry->reg_state = REG_TABLE_ENTRY_STATE_ALLOC;

  // track the previous register id with the same architectural register
  ASSERT(op->proc_id, entry->prev_tag_of_same_arch_id == REG_TABLE_INVALID_REG_ID);
  entry->prev_tag_of_same_arch_id = parent_reg_table->entries[entry->parent_reg_id].child_reg_id;

  DEBUG(0, "(entry write)[%lld]: parent_reg_id: %d, self_reg_id: %d, child_reg_id: %d\n",
    entry->op_num, entry->parent_reg_id, entry->self_reg_id, entry->child_reg_id);
}

struct reg_table_entry_ops reg_table_entry_ops = {
  .clear = reg_table_entry_clear,
  .read = reg_table_entry_read,
  .write = reg_table_entry_write,
};


/**************************************************************************************/
/* register table operation */

void reg_table_init(struct reg_table *reg_table, uns reg_table_size, struct reg_table *parent_reg_table) {
  // set the parent table pointer for tracking
  reg_table->parent_reg_table = parent_reg_table;

  reg_table->free_list = (struct reg_free_list *)malloc(sizeof(struct reg_free_list));
  reg_table->free_list->ops = &reg_free_list_ops;
  reg_table->free_list->ops->init(reg_table->free_list);

  reg_table->size = reg_table_size;
  reg_table->entries = (struct reg_table_entry *)malloc(sizeof(struct reg_table_entry) * reg_table_size);

  // init and insert all the empty entries into the free list
  for (uns ii = 0; ii < reg_table_size; ii++) {
    struct reg_table_entry *entry = &reg_table->entries[ii];
    entry->ops = &reg_table_entry_ops;
    entry->self_reg_id = ii;
    entry->ops->clear(entry);
    reg_table->free_list->ops->free(reg_table->free_list, entry);
  }

  /*
    initialize each general purpose and vector arch_reg_id with a dummy ptag to
    ensure even unused arch regs always have a valid mapping
  */
  for (uns ii = 0; ii < reg_table->parent_reg_table->size; ii++) {
    // do not map the unallocated parent table entry
    if (reg_table->parent_reg_table->entries[ii].reg_state == REG_TABLE_ENTRY_STATE_FREE) {
      continue;
    }

    struct reg_table_entry *entry = reg_table->free_list->ops->alloc(reg_table->free_list);
    entry->ops->write(entry, reg_table->parent_reg_table, &invalid_op, ii);
    reg_table->parent_reg_table->entries[entry->parent_reg_id].child_reg_id = entry->self_reg_id;
  }
}

/* do not duplicately read operand register dependency since it is already tracked during fetching */
void reg_table_read(struct reg_table *reg_table, Op *op, int parent_reg_id) {
  /*
    op_info read is done in reg_map_read() in map.c during fetching
    This module is to manage the allocation of registers in the map_stage
  */
  return;
}

/*
  --- 1. allocate a current register table entry from free list
  --- 2. store the op info into the register entry
  --- 3. update the child_reg_id of the parent table for tracking same architectural id
*/
int reg_table_alloc(struct reg_table *reg_table, Op *op, int parent_reg_id) {
  ASSERT(0, REG_FILE_TYPE && reg_table != NULL && op != NULL);

  // get the entry from the free list and write the metadata
  struct reg_table_entry *entry = reg_table->free_list->ops->alloc(reg_table->free_list);
  entry->ops->write(entry, reg_table->parent_reg_table, op, parent_reg_id);

  // update the parent table to ensure the latest assignment
  reg_table->parent_reg_table->entries[entry->parent_reg_id].child_reg_id = entry->self_reg_id;

  return entry->self_reg_id;
}

/* clear all the info of the entry and insert it to the free list */
void reg_table_free(struct reg_table *reg_table, struct reg_table_entry *entry) {
  ASSERT(0, entry->reg_state == REG_TABLE_ENTRY_STATE_DEAD || entry->off_path);

  // clear the entry value
  entry->ops->clear(entry);

  // append to free list
  reg_table->free_list->ops->free(reg_table->free_list, entry);
}

/* update the register state to indicate the value is produced */
void reg_table_write_back(struct reg_table *reg_table, int self_reg_id) {
  ASSERT(0, REG_FILE_TYPE && self_reg_id != REG_TABLE_INVALID_REG_ID);
  struct reg_table_entry *entry = &reg_table->entries[self_reg_id];

  ASSERT(0, entry->reg_state == REG_TABLE_ENTRY_STATE_ALLOC);
  entry->reg_state = REG_TABLE_ENTRY_STATE_PRODUCED;
}

/* update the parent table and remove the mispredicted entry */
void reg_table_flush_mispredict(struct reg_table *reg_table, int self_reg_id) {
  ASSERT(0, REG_FILE_TYPE && self_reg_id != REG_TABLE_INVALID_REG_ID);
  struct reg_table_entry *entry = &reg_table->entries[self_reg_id];

  ASSERT(0, entry != NULL && entry->off_path);
  ASSERT(0, entry->reg_state == REG_TABLE_ENTRY_STATE_ALLOC || entry->reg_state == REG_TABLE_ENTRY_STATE_PRODUCED);

  // clear the child_reg_id of the parent table by the previous same architectural id
  ASSERT(map_data->proc_id, reg_table->parent_reg_table->entries[entry->parent_reg_id].child_reg_id == self_reg_id);
  reg_table->parent_reg_table->entries[entry->parent_reg_id].child_reg_id = entry->prev_tag_of_same_arch_id;

  // release the current mispredicted register
  reg_table->ops->free(reg_table, entry);
}

/* mark the previous entry with same archituctural id before the committed one as dead and remove it */
void reg_table_release_prev(struct reg_table *reg_table, int self_reg_id) {
  ASSERT(0, REG_FILE_TYPE && self_reg_id != REG_TABLE_INVALID_REG_ID);
  struct reg_table_entry *entry = &reg_table->entries[self_reg_id];

  ASSERT(0, entry != NULL && entry->reg_state == REG_TABLE_ENTRY_STATE_PRODUCED);
  ASSERT(0, reg_table->parent_reg_table->entries[entry->parent_reg_id].child_reg_id != REG_TABLE_INVALID_REG_ID);

  // mark current register as commit when it is retire
  entry->reg_state = REG_TABLE_ENTRY_STATE_COMMIT;

  // mark the op before the committed op as dead and release it
  int prev_tag_of_same_arch_id = entry->prev_tag_of_same_arch_id;
  ASSERT(map_data->proc_id, prev_tag_of_same_arch_id != REG_TABLE_INVALID_REG_ID);

  struct reg_table_entry *prev_entry = &reg_table->entries[prev_tag_of_same_arch_id];
  ASSERT(map_data->proc_id, prev_entry->reg_state == REG_TABLE_ENTRY_STATE_COMMIT || prev_entry->op == &invalid_op);

  // release the previous register with the same arch id
  prev_entry->reg_state = REG_TABLE_ENTRY_STATE_DEAD;
  reg_table->ops->free(reg_table, prev_entry);
}

struct reg_table_ops reg_table_ops = {
  .init = reg_table_init,
  .read = reg_table_read,
  .alloc = reg_table_alloc,
  .free = reg_table_free,
  .write_back = reg_table_write_back,
  .flush_mispredict = reg_table_flush_mispredict,
  .release_prev = reg_table_release_prev,
};


void reg_table_arch_init(struct reg_table *reg_table, uns reg_table_size, struct reg_table *parent_reg_table) {
  reg_table->parent_reg_table = parent_reg_table;
  reg_table->free_list = NULL;
  reg_table->size = reg_table_size;
  reg_table->entries = (struct reg_table_entry *)malloc(sizeof(struct reg_table_entry) * reg_table_size);

  // only need to assign the current register index for the children table to track
  for (uns ii = 0; ii < reg_table->size; ii++) {
    struct reg_table_entry *entry = &reg_table->entries[ii];
    entry->ops = &reg_table_entry_ops;
    entry->self_reg_id = ii;
    entry->ops->clear(entry);

    // only allocate general purpose and vector registers
    if (reg_file_get_reg_type(ii) != REG_TABLE_REG_TYPE_OTHER)
      entry->reg_state = REG_TABLE_ENTRY_STATE_ALLOC;
  }
}

struct reg_table_ops reg_table_ops_arch = {
  .init = reg_table_arch_init,
};


/**************************************************************************************/
/* Global Instance of Register Tables */

/* for realistic renaming */
// map each architectural register id to the latest ptag
struct reg_table *reg_table_arch_to_ptag;

// map ptags to physical register for both speculative and committed op
struct reg_table *reg_table_ptag_to_physical;

/* for late allocation */
// map each architectural register id to the latest virtual register id
struct reg_table *reg_table_arch_to_vtag;

// map the virtual register id to the ptag
struct reg_table *reg_table_vtag_to_ptag;


/**************************************************************************************/
/* Infinite Register File Scheme */

void reg_file_infinite_init(void);
Flag reg_file_infinite_available(uns stage_op_count);
void reg_file_infinite_rename(Op *op);
Flag reg_file_infinite_issue(Op *op);
void reg_file_infinite_execute(Op *op);
void reg_file_infinite_recover(Counter recovery_op_num);
void reg_file_infinite_commit(Op *op);

void reg_file_infinite_init(void) {
  return;
}

Flag reg_file_infinite_available(uns stage_op_count) {
  return TRUE;
}

void reg_file_infinite_rename(Op *op) {
  return;
}

Flag reg_file_infinite_issue(Op *op) {
  return TRUE;
}

void reg_file_infinite_execute(Op *op) {
  return;
}

void reg_file_infinite_recover(Counter recovery_op_num) {
  return;
}

void reg_file_infinite_commit(Op *op) {
  return;
}


/**************************************************************************************/
/* Realistic Register File Scheme */

void reg_file_realistic_init(void);
Flag reg_file_realistic_available(uns stage_op_count);
void reg_file_realistic_rename(Op *op);
Flag reg_file_realistic_issue(Op *op);
void reg_file_realistic_execute(Op *op);
void reg_file_realistic_recover(Counter recovery_op_num);
void reg_file_realistic_commit(Op *op);

// allocate entries and assign the parent-child relationship of the arch table and the physical table
void reg_file_realistic_init(void) {
  /* the physical reg map is the children table of the arch table
     the child_reg_id of the arch table is the index of the physical reg map */
  reg_table_arch_to_ptag = (struct reg_table *)malloc(sizeof(struct reg_table));
  reg_table_arch_to_ptag->ops = &reg_table_ops_arch;
  reg_table_arch_to_ptag->ops->init(reg_table_arch_to_ptag, NUM_REG_IDS, NULL);

  /* the arch table is the parent table of the physical reg map
     the parent_reg_id of the physical table is the index of the arch table */
  reg_table_ptag_to_physical = (struct reg_table *)malloc(sizeof(struct reg_table));
  reg_table_ptag_to_physical->ops = &reg_table_ops;
  reg_table_ptag_to_physical->ops->init(reg_table_ptag_to_physical, REG_TABLE_PHYSICAL_SIZE, reg_table_arch_to_ptag);
}

// check if there are enough register entries
Flag reg_file_realistic_available(uns stage_op_count) {
  ASSERT(0, reg_table_ptag_to_physical != NULL);
  return reg_table_ptag_to_physical->free_list->reg_free_num >= MAX_DESTS * stage_op_count;
}

// allocate physical registers of the op and write the ptag info into the op
void reg_file_realistic_rename(Op *op) {
  ASSERT(0, op != NULL && reg_table_ptag_to_physical != NULL);
  ASSERT(0, op->table_info->num_dest_regs <= reg_table_ptag_to_physical->free_list->reg_free_num);

  /* read register table */
  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    // the register dependency is not read since it is already tracked during fetching
    if (reg_file_get_reg_type(op->inst_info->srcs[ii].id) != REG_TABLE_REG_TYPE_OTHER)
      reg_table_ptag_to_physical->ops->read(reg_table_ptag_to_physical, op, op->inst_info->srcs[ii].id);
  }

  /* write register table */
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    // only allocate general purpose and vector registers
    if (reg_file_get_reg_type(op->inst_info->dests[ii].id) == REG_TABLE_REG_TYPE_OTHER)
      continue;

    // allocate register and write meta info
    int reg_ptag = reg_table_ptag_to_physical->ops->alloc(reg_table_ptag_to_physical, op, op->inst_info->dests[ii].id);

    // update the register id in op
    ASSERT(0, op != &invalid_op && op->dst_reg_ptag[ii] == REG_TABLE_INVALID_REG_ID);
    op->dst_reg_ptag[ii] = reg_ptag;
  }
}

// do not check the reg file when issuing
Flag reg_file_realistic_issue(Op *op) {
  return TRUE;
}

// write back the physical register using the ptag info of the op
void reg_file_realistic_execute(Op *op) {
  ASSERT(0, op != NULL && reg_table_ptag_to_physical != NULL);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    if (op->dst_reg_ptag[ii] != REG_TABLE_INVALID_REG_ID)
      reg_table_ptag_to_physical->ops->write_back(reg_table_ptag_to_physical, op->dst_reg_ptag[ii]);
  }
}

// flush registers of misprediction operands using the ptag info
void reg_file_realistic_recover(Counter recovery_op_num) {
  ASSERT(0, reg_table_ptag_to_physical != NULL);

  // release the register from the youngest to the flush point
  for (Op** op_p = (Op**)list_start_tail_traversal(&td->seq_op_list);
       op_p && (*op_p)->op_num > recovery_op_num; op_p = (Op**)list_prev_element(&td->seq_op_list)) {
    ASSERT(map_data->proc_id, (*op_p)->off_path);

    // release misprediction register
    for (uns ii = 0; ii < (*op_p)->table_info->num_dest_regs; ii++) {
      if ((*op_p)->dst_reg_ptag[ii] != REG_TABLE_INVALID_REG_ID)
        reg_table_ptag_to_physical->ops->flush_mispredict(reg_table_ptag_to_physical, (*op_p)->dst_reg_ptag[ii]);
    }
  }
}

// release the previous register with same architectural id
void reg_file_realistic_commit(Op *op) {
  ASSERT(0, op != NULL && reg_table_ptag_to_physical != NULL);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    if (op->dst_reg_ptag[ii] != REG_TABLE_INVALID_REG_ID)
      reg_table_ptag_to_physical->ops->release_prev(reg_table_ptag_to_physical, op->dst_reg_ptag[ii]);
  }
}


/**************************************************************************************/
/* Virtual Physical Register File Scheme */

void reg_file_late_allocation_init(void);
Flag reg_file_late_allocation_available(uns stage_op_count);
void reg_file_late_allocation_rename(Op *op);
Flag reg_file_late_allocation_issue(Op *op);
void reg_file_late_allocation_execute(Op *op);
void reg_file_late_allocation_recover(Counter recovery_op_num);
void reg_file_late_allocation_commit(Op *op);

// allocate entries and assign the parent-child relationship for arch, vtag, and ptag tables
void reg_file_late_allocation_init(void) {
  ASSERT(0, REG_TABLE_VIRTUAL_SIZE >= REG_TABLE_PHYSICAL_SIZE);

  /* the arch reg table is the parent table of the vtag table
     the child_reg_id of the arch table is the index of the vtag reg map table */
  reg_table_arch_to_vtag = (struct reg_table *)malloc(sizeof(struct reg_table));
  reg_table_arch_to_vtag->ops = &reg_table_ops_arch;
  reg_table_arch_to_vtag->ops->init(reg_table_arch_to_vtag, NUM_REG_IDS, NULL);

  /* the vtag reg table is the parent table of the ptag table
     the child_reg_id of the vtag table is the index of the ptag reg map table */
  reg_table_vtag_to_ptag = (struct reg_table *)malloc(sizeof(struct reg_table));
  reg_table_vtag_to_ptag->ops = &reg_table_ops;
  reg_table_vtag_to_ptag->ops->init(reg_table_vtag_to_ptag, REG_TABLE_VIRTUAL_SIZE, reg_table_arch_to_vtag);

  /* the ptag reg table is the child table of the vtag table
     the parent_reg_id of the ptag table is the index of the vtag reg map table */
  reg_table_ptag_to_physical = (struct reg_table *)malloc(sizeof(struct reg_table));
  reg_table_ptag_to_physical->ops = &reg_table_ops;
  reg_table_ptag_to_physical->ops->init(reg_table_ptag_to_physical, REG_TABLE_PHYSICAL_SIZE, reg_table_vtag_to_ptag);
}

// check if there are enough registers in the virtual table instead of the physical registers
Flag reg_file_late_allocation_available(uns stage_op_count) {
  ASSERT(0, reg_table_vtag_to_ptag != NULL);
  return reg_table_vtag_to_ptag->free_list->reg_free_num >= MAX_DESTS * stage_op_count;
}

// allocate only virtual registers and write the vtag info into the op
void reg_file_late_allocation_rename(Op *op) {
  ASSERT(0, op != NULL && reg_table_vtag_to_ptag != NULL);
  ASSERT(0, op->table_info->num_dest_regs <= reg_table_vtag_to_ptag->free_list->reg_free_num);

  /* write register table */
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    // only allocate general purpose and vector registers
    if (reg_file_get_reg_type(op->inst_info->dests[ii].id) == REG_TABLE_REG_TYPE_OTHER)
      continue;

    // allocate virtual registers and write meta info
    int reg_vtag = reg_table_vtag_to_ptag->ops->alloc(reg_table_vtag_to_ptag, op, op->inst_info->dests[ii].id);

    // write the register vtag into the op
    ASSERT(0, op != &invalid_op && op->dst_reg_vtag[ii] == REG_TABLE_INVALID_REG_ID && reg_vtag != REG_TABLE_INVALID_REG_ID);
    op->dst_reg_vtag[ii] = reg_vtag;
  }
}

/*
  To avoid deadlock, only allocate physical registers
  if sufficient additional registers exist to serve the oldest op in the ROB
*/
Flag reg_file_late_allocation_issue(Op *op) {
  ASSERT(0, reg_table_vtag_to_ptag != NULL && reg_table_ptag_to_physical != NULL);

  // if the op does not have destination registers, it will no result in deadlock
  if (op == NULL || op->table_info->num_dest_regs == 0)
    return TRUE;

  // find the first op with destination registers from the head of ROB
  Op *reserve_op = node->node_head;
  while (reserve_op != NULL) {
    if (reserve_op->table_info->num_dest_regs != 0) {
      break;
    }
    reserve_op = reserve_op->next_node;
  }
  ASSERT(0, reserve_op != NULL);

  // do not need to reserve if the reserving head has allocated physical register
  if (reserve_op->dst_reg_ptag[0] != REG_TABLE_INVALID_REG_ID) {
    ASSERT(0, reserve_op->op_num <= op->op_num);
    return reg_table_ptag_to_physical->free_list->reg_free_num >= MAX_DESTS;
  }

  // if the reserving head has not allocated but the current op is in the head, allow the head to allocate
  if (op->op_num == reserve_op->op_num) {
    ASSERT(0, reg_table_ptag_to_physical->free_list->reg_free_num >= op->table_info->num_dest_regs);
    return TRUE;
  }

  // reserve registers for the head
  Flag if_available = reg_table_ptag_to_physical->free_list->reg_free_num >= MAX_DESTS + MAX_DESTS;
  if (!if_available)
    STAT_EVENT(0, MAP_STAGE_LATE_ALLOCATE_SEND_BACK);
  return if_available;
}

// allocate the physical registers during execution using the vtag info of the op and write back the virtual and physical registers
void reg_file_late_allocation_execute(Op *op) {
  ASSERT(0, op != NULL && reg_table_vtag_to_ptag != NULL && reg_table_ptag_to_physical != NULL);
  ASSERT(0, op->table_info->num_dest_regs <= reg_table_ptag_to_physical->free_list->reg_free_num);

  // late allocation for physical register entries
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    if (op->dst_reg_vtag[ii] == REG_TABLE_INVALID_REG_ID)
      continue;

    // allocate physical register and write info
    int reg_ptag = reg_table_ptag_to_physical->ops->alloc(reg_table_ptag_to_physical, op, op->dst_reg_vtag[ii]);

    // write the register ptag in the op
    ASSERT(0, op != &invalid_op && op->dst_reg_ptag[ii] == REG_TABLE_INVALID_REG_ID && reg_ptag != REG_TABLE_INVALID_REG_ID);
    op->dst_reg_ptag[ii] = reg_ptag;
  }

  // write back to update the register state
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    if (op->dst_reg_vtag[ii] == REG_TABLE_INVALID_REG_ID)
      continue;

    reg_table_vtag_to_ptag->ops->write_back(reg_table_vtag_to_ptag, op->dst_reg_vtag[ii]);
    reg_table_ptag_to_physical->ops->write_back(reg_table_ptag_to_physical, op->dst_reg_ptag[ii]);
  }
}

void reg_file_late_allocation_recover(Counter recovery_op_num) {
  ASSERT(0, reg_table_vtag_to_ptag != NULL && reg_table_ptag_to_physical != NULL);

  // release the register from the youngest to the flush point
  for (Op** op_p = (Op**)list_start_tail_traversal(&td->seq_op_list);
       op_p && (*op_p)->op_num > recovery_op_num; op_p = (Op**)list_prev_element(&td->seq_op_list)) {
    ASSERT((*op_p)->proc_id, (*op_p)->off_path);

    for (uns ii = 0; ii < (*op_p)->table_info->num_dest_regs; ++ii) {
      if ((*op_p)->dst_reg_ptag[ii] != REG_TABLE_INVALID_REG_ID)
        reg_table_ptag_to_physical->ops->flush_mispredict(reg_table_ptag_to_physical, (*op_p)->dst_reg_ptag[ii]);
    }

    for (uns ii = 0; ii < (*op_p)->table_info->num_dest_regs; ++ii) {
      if ((*op_p)->dst_reg_vtag[ii] != REG_TABLE_INVALID_REG_ID)
        reg_table_vtag_to_ptag->ops->flush_mispredict(reg_table_vtag_to_ptag, (*op_p)->dst_reg_vtag[ii]);
    }
  }
}

void reg_file_late_allocation_commit(Op *op) {
  ASSERT(0, op != NULL && reg_table_vtag_to_ptag != NULL && reg_table_ptag_to_physical != NULL);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    if (op->dst_reg_vtag[ii] == REG_TABLE_INVALID_REG_ID)
      continue;

    /*
      the previous same architectural id of ptag may be invalid due to the out-of-order allocation
      therefore, a lazy assignment is done here, e.g., tracking the prev_ptag when the op is committed and the prev_ptag is to be released
    */
    struct reg_table_entry *entry = &reg_table_ptag_to_physical->entries[op->dst_reg_ptag[ii]];
    int vtag = entry->parent_reg_id;
    int prev_vtag = reg_table_vtag_to_ptag->entries[vtag].prev_tag_of_same_arch_id;
    entry->prev_tag_of_same_arch_id = reg_table_vtag_to_ptag->entries[prev_vtag].child_reg_id;

    reg_table_ptag_to_physical->ops->release_prev(reg_table_ptag_to_physical, op->dst_reg_ptag[ii]);
    reg_table_vtag_to_ptag->ops->release_prev(reg_table_vtag_to_ptag, op->dst_reg_vtag[ii]);
  }
}


/**************************************************************************************/
/* Register File Function Driven Table */

struct reg_file_func {
  enum reg_file_type reg_file_type;
  void (*init)(void);
  Flag (*available)(uns stage_op_count);
  void (*rename)(Op *op);
  Flag (*issue)(Op *op);
  void (*execute)(Op *op);
  void (*recover)(Counter recovery_op_num);
  void (*commit)(Op *op);
};

struct reg_file_func reg_file_func_table[REG_FILE_TYPE_NUM] = {
  {
    REG_FILE_TYPE_INFINITE,
    reg_file_infinite_init, reg_file_infinite_available, reg_file_infinite_rename, reg_file_infinite_issue,
    reg_file_infinite_execute, reg_file_infinite_recover, reg_file_infinite_commit
  },
  {
    REG_FILE_TYPE_REALISTIC,
    reg_file_realistic_init, reg_file_realistic_available, reg_file_realistic_rename, reg_file_realistic_issue,
    reg_file_realistic_execute, reg_file_realistic_recover, reg_file_realistic_commit
  },
  {
    REG_FILE_TYPE_LATE_ALLOCATION,
    reg_file_late_allocation_init, reg_file_late_allocation_available, reg_file_late_allocation_rename, reg_file_late_allocation_issue,
    reg_file_late_allocation_execute, reg_file_late_allocation_recover, reg_file_late_allocation_commit
  }
};


/**************************************************************************************/
/* External Calling */

/*
  Called by:
  --- map.c -> when map init
  Procedure:
  --- allocate the register table entries by the config size
*/
void reg_file_init(void) {
  ASSERT(0, REG_FILE_TYPE >= REG_FILE_TYPE_INFINITE && REG_FILE_TYPE < REG_FILE_TYPE_NUM);
  reg_file_func_table[REG_FILE_TYPE].init();
}

/*
  Called by:
  --- map_stage.c -> before an op is fetched from cache stage data
  Procedure:
  --- check if there are enough register entries
*/
Flag reg_file_available(uns stage_op_count) {
  ASSERT(0, REG_FILE_TYPE >= REG_FILE_TYPE_INFINITE && REG_FILE_TYPE < REG_FILE_TYPE_NUM);
  return reg_file_func_table[REG_FILE_TYPE].available(stage_op_count);
}

/*
  Called by:
  --- map_stage.c -> when an op is fetched from cache stage data
  Procedure:
  --- look up src register and fill the entry into src_info
  --- allocate an entry and store the op info into the register entry
*/
void reg_file_rename(Op *op) {
  ASSERT(0, REG_FILE_TYPE >= REG_FILE_TYPE_INFINITE && REG_FILE_TYPE < REG_FILE_TYPE_NUM);
  reg_file_func_table[REG_FILE_TYPE].rename(op);
}

/*
  Called by:
  --- exec_stage.c -> when the op is going to be executed
  Procedure:
  --- do additional checking before execution
*/
Flag reg_file_issue(Op *op) {
  ASSERT(0, REG_FILE_TYPE >= REG_FILE_TYPE_INFINITE && REG_FILE_TYPE < REG_FILE_TYPE_NUM);
  return reg_file_func_table[REG_FILE_TYPE].issue(op);
}

/*
  Called by:
  --- map.c -> when the op is executed
  Procedure:
  --- write back when the results in the registers are produced
*/
void reg_file_execute(Op *op) {
  ASSERT(0, REG_FILE_TYPE >= REG_FILE_TYPE_INFINITE && REG_FILE_TYPE < REG_FILE_TYPE_NUM);
  reg_file_func_table[REG_FILE_TYPE].execute(op);
}

/*
  Called by:
  --- thread.c -> when a misprediction occurs, it should happen before the op list flush
  Procedure:
  --- flush registers of misprediction operands
*/
void reg_file_recover(Counter recovery_op_num) {
  ASSERT(0, REG_FILE_TYPE >= REG_FILE_TYPE_INFINITE && REG_FILE_TYPE < REG_FILE_TYPE_NUM);
  reg_file_func_table[REG_FILE_TYPE].recover(recovery_op_num);
}

/*
  Called by:
  --- node_stage.c -> when the op is retired
  Procedure:
  --- release the previous register with same architectural id
*/
void reg_file_commit(Op *op) {
  ASSERT(0, REG_FILE_TYPE >= REG_FILE_TYPE_INFINITE && REG_FILE_TYPE < REG_FILE_TYPE_NUM);
  reg_file_func_table[REG_FILE_TYPE].commit(op);
}

