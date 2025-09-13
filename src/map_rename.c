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
 * Author       : Yinyuan Zhao (Litz Lab)
 * Date         : 03/2024, 07/2025
 * Description  : Register Renaming
 ***************************************************************************************/

#include "map_rename.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"

#include "isa/isa.h"
#include "isa/isa_macros.h"

#include "map_stage.h"
#include "node_stage.h"
#include "op.h"
#include "thread.h"
#include "xed-interface.h"

/**************************************************************************************/
/* Extern Definition */

struct reg_file **reg_file;

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
void reg_table_entry_write(struct reg_table_entry *entry, Op *op, int parent_reg_id);
void reg_table_entry_consume(struct reg_table_entry *entry, Op *op);
void reg_table_entry_produce(struct reg_table_entry *entry, Op *op);

// register table operations
void reg_table_init(struct reg_table *reg_table, struct reg_table *parent_reg_table, uns reg_table_size, int reg_type,
                    int reg_table_type);
int reg_table_read(struct reg_table *reg_table, Op *op, int parent_reg_id);
int reg_table_alloc(struct reg_table *reg_table, Op *op, int parent_reg_id);
void reg_table_free(struct reg_table *reg_table, struct reg_table_entry *entry);
void reg_table_consume(struct reg_table *reg_table, int reg_id, Op *op);
void reg_table_produce(struct reg_table *reg_table, int self_reg_id, Op *op);

// special init func for the architectural table
void reg_table_arch_init(struct reg_table *reg_table, struct reg_table *parent_reg_table, uns reg_table_size,
                         int reg_type, int reg_table_type);

/**************************************************************************************/
/* Inline Methods */

static inline void reg_file_debug_print_entry(struct reg_table_entry *entry, int state) {
  printf("[ENTRY: %d]\n", state);
  printf("op_num: %lld, off_path: %d, state: %d,\n", entry->op_num, entry->off_path, entry->reg_state);
  printf("parent: %d, self: %d, child: %d\n", entry->parent_reg_id, entry->self_reg_id, entry->child_reg_id);
  printf("table type: %d, reg type: %d\n", entry->reg_table_type, entry->reg_type);
  printf("read: %d, consume: %d\n", entry->onpath_consumers_num, entry->onpath_consumed_count);
}

static inline void reg_file_debug_print_op(Op *op, int state) {
  ASSERT(op->proc_id, op != NULL);
  if (op->table_info->num_dest_regs == 0)
    return;

  Inst_Info *inst_info = op->inst_info;
  uns16 op_code = inst_info->table_info->true_op_type;

  printf("[OP: %d]\n", state);
  printf("op_num: %lld, off_path: %d, ", op->op_num, op->off_path);
  printf("pc: %lld, opcode: 0x%x(%s), cf: %d, mem: %d\n", inst_info->addr, op_code, xed_iclass_enum_t2str(op_code),
         inst_info->table_info->cf_type, inst_info->table_info->mem_type);

  printf("src#%d: <", inst_info->table_info->num_src_regs);
  for (int ii = 0; ii < inst_info->table_info->num_src_regs; ii++)
    printf("%d, ", inst_info->srcs[ii].id);
  printf(">, dest#%d: <", inst_info->table_info->num_dest_regs);
  for (int ii = 0; ii < inst_info->table_info->num_dest_regs; ii++)
    printf("%d, ", inst_info->dests[ii].id);
  printf(">\n");

  printf("src_ptag#%d: <", inst_info->table_info->num_src_regs);
  for (int ii = 0; ii < inst_info->table_info->num_src_regs; ii++)
    printf("%d, ", op->src_reg_id[ii][REG_TABLE_TYPE_PHYSICAL]);
  printf(">\n");

  printf("dst_ptag#%d: <", inst_info->table_info->num_dest_regs);
  for (int ii = 0; ii < inst_info->table_info->num_dest_regs; ii++)
    printf("%d, ", op->dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL]);
  printf(">\n");

  printf("prev_ptag#%d: <", inst_info->table_info->num_dest_regs);
  for (int ii = 0; ii < inst_info->table_info->num_dest_regs; ii++)
    printf("%d, ", op->prev_dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL]);
  printf(">\n");
}

static inline void reg_file_debug_print_table(uns reg_table_type) {
  for (uns ii = 0; ii < REG_FILE_REG_TYPE_NUM; ii++) {
    printf("\n");
    printf("Table Type: %d; Reg Type: %d\n", reg_table_type, ii);
    for (uns jj = 0; jj < reg_file[ii]->reg_table[reg_table_type]->size; jj++) {
      struct reg_table_entry *entry = &reg_file[ii]->reg_table[reg_table_type]->entries[jj];
      reg_file_debug_print_entry(entry, 0);
    }
  }
}

// only process general purpose and vector registers for the renaming allocation
static inline int reg_file_get_reg_type(int reg_id) {
  if ((reg_id >= REG_RAX && reg_id < REG_CS) || (reg_id >= REG_TMP0 && reg_id <= REG_TMP4) ||
      (reg_id >= REG_ZPS && reg_id < REG_ZMM0))
    return REG_FILE_REG_TYPE_GENERAL_PURPOSE;

  if (reg_id >= REG_ZMM0 && reg_id < REG_K0)
    return REG_FILE_REG_TYPE_VECTOR;

  return REG_FILE_REG_TYPE_OTHER;
}

static inline Flag reg_file_check_reg_num(uns reg_table_type, uns op_count) {
  for (uns ii = 0; ii < REG_FILE_REG_TYPE_NUM; ++ii) {
    if (reg_file[ii]->reg_table[reg_table_type]->free_list->reg_free_num < REG_FILE_MAX_DESTS[ii] * op_count)
      return FALSE;
  }
  return TRUE;
}

// extract the valid architectural register id into the op from inst info
static inline void reg_file_extract_arch_reg_id(Op *op) {
  ASSERT(op->proc_id, op != &invalid_op);

  // fill the source register id
  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    ASSERT(op->proc_id, op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL] == REG_TABLE_REG_ID_INVALID);
    int reg_type = reg_file_get_reg_type(op->inst_info->srcs[ii].id);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL] = op->inst_info->srcs[ii].id;
  }

  // fill the destination register id
  uns reg_dest_num[REG_FILE_REG_TYPE_NUM] = {0};
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    ASSERT(op->proc_id, op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL] == REG_TABLE_REG_ID_INVALID);
    int reg_type = reg_file_get_reg_type(op->inst_info->dests[ii].id);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL] = op->inst_info->dests[ii].id;
    reg_dest_num[reg_type]++;
  }

  for (uns ii = 0; ii < REG_FILE_REG_TYPE_NUM; ++ii) {
    ASSERT(op->proc_id, reg_dest_num[ii] <= REG_FILE_MAX_DESTS[ii]);
  }
}

static inline void reg_file_collect_rename_stat(Op *op) {
  ASSERT(op->proc_id, op != &invalid_op);
  STAT_EVENT(map_data->proc_id, MAP_STAGE_RENAME_OP_ONPATH + op->off_path);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->inst_info->dests[ii].id);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    STAT_EVENT(map_data->proc_id, MAP_STAGE_RENAME_INT_REG_ONPATH + op->off_path + reg_type * 2);
  }
}

static inline void reg_file_collect_released_entry_stat(struct reg_table_entry *entry) {
  // do not collect the dummy register stat
  if (entry->op_num == 0)
    return;

  // do not collect the stat of virtual table registers
  if (entry->reg_table_type != REG_TABLE_TYPE_PHYSICAL)
    return;

  // do not collect the off-path register stat during flushing
  if (entry->off_path)
    return;

  // set the cycle counts for unconsumed registers
  entry->produced_cycle = entry->produced_cycle == MAX_CTR ? cycle_count : entry->produced_cycle;
  entry->onpath_consumed_cycle = entry->onpath_consumed_cycle == MAX_CTR ? cycle_count : entry->onpath_consumed_cycle;
  ASSERT(map_data->proc_id, entry->produced_cycle >= entry->allocated_cycle);
  ASSERT(map_data->proc_id, entry->onpath_consumed_cycle >= entry->produced_cycle);
  ASSERT(map_data->proc_id, cycle_count >= entry->onpath_consumed_cycle);

  // these cycle counters are only set in some specific schemes
  entry->redefined_cycle = entry->redefined_cycle == MAX_CTR ? cycle_count : entry->redefined_cycle;
  entry->spec_release_cycle = entry->spec_release_cycle == MAX_CTR ? cycle_count : entry->spec_release_cycle;
  entry->nonspec_release_cycle = entry->nonspec_release_cycle == MAX_CTR ? cycle_count : entry->nonspec_release_cycle;

  ASSERT(map_data->proc_id, entry->reg_type < REG_FILE_REG_TYPE_NUM);
  STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_NUM_ALLOC + entry->reg_type);

  if (!entry->is_branch)
    STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_NUM_NONBRANCH + entry->reg_type);
  if (!entry->is_except)
    STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_NUM_NONEXCEPT + entry->reg_type);
  if (entry->is_atomic)
    STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_NUM_ATOMIC + entry->reg_type);

  // cyclecounts between critical events
  if (entry->onpath_consumers_num != 0 && entry->onpath_consumed_cycle - entry->produced_cycle == 1)
    STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_NUM_SHORTLIVE + entry->reg_type);
  if (entry->is_atomic) {
    INC_STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_ATOMIC_REDEFINED_DIST_TOTAL + entry->reg_type,
                   entry->redefined_cycle - entry->allocated_cycle);
    INC_STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_ATOMIC_CONSUMED_DIST_TOTAL + entry->reg_type,
                   entry->onpath_consumed_cycle - entry->allocated_cycle);
    INC_STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_ATOMIC_RELEASE_DIST_TOTAL + entry->reg_type,
                   cycle_count - entry->allocated_cycle);
  }

  // consumer sensitivity
  int cap_consumers = entry->onpath_consumers_num < REG_RENAMING_SCHEME_EARLY_RELEASE_PENDING_CONSUMED_MAX
                          ? entry->onpath_consumers_num
                          : REG_RENAMING_SCHEME_EARLY_RELEASE_PENDING_CONSUMED_MAX;
  int index = cap_consumers * 2 + entry->reg_type;
  STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_USE_COUNT_0 + index);
  INC_STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_CONSUME_DIST_0 + index, entry->onpath_consumed_dist);

  // lifetime cyclecount distribution
  INC_STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_LIFECYCLE_EMPTY + entry->reg_type,
                 entry->produced_cycle - entry->allocated_cycle);
  INC_STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_LIFECYCLE_IN_USE + entry->reg_type,
                 entry->onpath_consumed_cycle - entry->allocated_cycle);
  INC_STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_LIFECYCLE_SPEC_RELEASE + entry->reg_type,
                 entry->spec_release_cycle - entry->allocated_cycle);
  INC_STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_LIFECYCLE_NONSPEC_RELEASE + entry->reg_type,
                 entry->nonspec_release_cycle - entry->allocated_cycle);
  INC_STAT_EVENT(map_data->proc_id, MAP_STAGE_ONPATH_INT_REG_LIFECYCLE_TOTAL + entry->reg_type,
                 cycle_count - entry->allocated_cycle);
}

/**************************************************************************************/
/* move elimination:
 *    The destination register of an instruction can be renamed to the same physical register as the source
 *    if all the bellow conditions are met:
 *    - The instruction is a register-to-register move (e.g., XED_ICLASS_MOV)
 *    - It has exactly one source and one destination
 *    - Both source and destination operands are general-purpose or SIMD registers
 *    - There is no memory access involved (i.e., no load or store)
 *    - The source and destination registers have the same width
 *    - The move has no side effects (e.g., it does not update flags)
 */

uns16 MOVE_ELIMINATION_OP_CODE[] = {
    XED_ICLASS_MOV,
};
#define NUM_MOVE_ELIM_OP_CODES (sizeof(MOVE_ELIMINATION_OP_CODE) / sizeof(MOVE_ELIMINATION_OP_CODE[0]))

static inline Flag reg_file_check_move_elim_candidate(uns16 op_code) {
  ASSERT(map_data->proc_id, REG_RENAMING_MOVE_ELIMINATE);

  // TODO: use hashmap when increasing op_code candidates
  for (int ii = 0; ii < NUM_MOVE_ELIM_OP_CODES; ii++) {
    if (op_code == MOVE_ELIMINATION_OP_CODE[ii]) {
      return TRUE;
    }
  }

  return FALSE;
}

static inline Flag reg_file_check_same_reg_width(int src_reg_id, int dst_reg_id) {
  ASSERT(map_data->proc_id, REG_RENAMING_MOVE_ELIMINATE);

  // Match 64-bit GPRs
  if ((src_reg_id >= REG_RAX && src_reg_id <= REG_R15) && (dst_reg_id >= REG_RAX && dst_reg_id <= REG_R15))
    return TRUE;

  // Match 512-bit SIMD (ZMM)
  if ((src_reg_id >= REG_ZMM0 && src_reg_id <= REG_ZMM31) && (dst_reg_id >= REG_ZMM0 && dst_reg_id <= REG_ZMM31))
    return TRUE;

  return FALSE;
}

static inline void reg_file_move_eliminate(Op *op) {
  if (!REG_RENAMING_MOVE_ELIMINATE)
    return;

  if (op->table_info->mem_type)
    return;

  if (op->table_info->num_src_regs != 1 || op->table_info->num_dest_regs != 1)
    return;

  int src_reg_id = op->src_reg_id[0][REG_TABLE_TYPE_ARCHITECTURAL];
  int dst_reg_id = op->dst_reg_id[0][REG_TABLE_TYPE_ARCHITECTURAL];
  if (reg_file_get_reg_type(src_reg_id) == REG_FILE_REG_TYPE_OTHER)
    return;

  if (reg_file_get_reg_type(dst_reg_id) == REG_FILE_REG_TYPE_OTHER)
    return;

  if (!reg_file_check_same_reg_width(src_reg_id, dst_reg_id))
    return;

  if (!reg_file_check_move_elim_candidate(op->inst_info->table_info->true_op_type))
    return;

  op->move_eliminated = TRUE;
  STAT_EVENT(map_data->proc_id, MAP_STAGE_RENAME_MOVE_ELIM_ONPATH + op->off_path);
}

/**************************************************************************************/
/* reg file operation functions for all register schemes */

static inline void reg_file_init_reg_table(int self_reg_table_type, struct reg_table *parent_reg_table,
                                           int reg_table_size, int reg_type, struct reg_table_ops *reg_table_ops) {
  ASSERT(map_data->proc_id, reg_file[reg_type] && reg_table_ops && reg_table_ops->init);
  reg_file[reg_type]->reg_table[self_reg_table_type] = (struct reg_table *)malloc(sizeof(struct reg_table));
  ASSERT(map_data->proc_id, reg_file[reg_type]->reg_table[self_reg_table_type]);

  struct reg_table *self_reg_table = reg_file[reg_type]->reg_table[self_reg_table_type];
  self_reg_table->ops = reg_table_ops;
  self_reg_table->ops->init(self_reg_table, parent_reg_table, reg_table_size, reg_type, self_reg_table_type);
}

// read the src by looking up the parent table, and record the reg id into the op
static inline void reg_file_read_src(Op *op, int self_reg_table_type, int parent_reg_table_type) {
  // the register dependency is not read since it is already tracked in the map module
  ASSERT(op->proc_id, op != &invalid_op);

  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    // lookup the parent table to get the latest register id
    int parent_reg_id = op->src_reg_id[ii][parent_reg_table_type];
    ASSERT(op->proc_id, parent_reg_id != REG_TABLE_REG_ID_INVALID);
    struct reg_table *reg_table = reg_file[reg_type]->reg_table[self_reg_table_type];
    int reg_id = reg_table->ops->read(reg_table, op, parent_reg_id);

    // update the src register id of the self table into the op
    ASSERT(op->proc_id, op->src_reg_id[ii][self_reg_table_type] == REG_TABLE_REG_ID_INVALID);
    op->src_reg_id[ii][self_reg_table_type] = reg_id;
  }
}

// allocate registers from free list, update SRT, and record reg id into the op
static inline void reg_file_write_dst(Op *op, int self_reg_table_type, int parent_reg_table_type) {
  ASSERT(op->proc_id, op != &invalid_op);
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    int parent_reg_id = op->dst_reg_id[ii][parent_reg_table_type];
    ASSERT(op->proc_id, parent_reg_id != REG_TABLE_REG_ID_INVALID);
    struct reg_table *reg_table = reg_file[reg_type]->reg_table[self_reg_table_type];

    // track the previous register id with the same parent table register before allocation
    ASSERT(op->proc_id, op->prev_dst_reg_id[ii][self_reg_table_type] == REG_TABLE_REG_ID_INVALID);
    op->prev_dst_reg_id[ii][self_reg_table_type] = reg_table->parent_reg_table->entries[parent_reg_id].child_reg_id;

    // allocate the dst register and write meta info
    int self_reg_id = reg_table->ops->alloc(reg_table, op, parent_reg_id);

    // increase the reference number for register sharing
    reg_table->entries[self_reg_id].num_refs++;

    // update the parent table to ensure the latest assignment
    reg_table->parent_reg_table->entries[parent_reg_id].child_reg_id = self_reg_id;

    // update the dst register id into the op
    ASSERT(op->proc_id, op->dst_reg_id[ii][self_reg_table_type] == REG_TABLE_REG_ID_INVALID);
    op->dst_reg_id[ii][self_reg_table_type] = self_reg_id;
  }
}

// only update metadata since the register dependency wake up will be done in the map module
static inline void reg_file_consume_src(Op *op, int *reg_table_types, int reg_table_num) {
  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    for (uns jj = 0; jj < reg_table_num; ++jj) {
      int table_type = reg_table_types[jj];
      ASSERT(op->proc_id, table_type > REG_TABLE_TYPE_ARCHITECTURAL && table_type < REG_TABLE_TYPE_NUM);
      int reg_id = op->src_reg_id[ii][table_type];
      ASSERT(op->proc_id, reg_id != REG_TABLE_REG_ID_INVALID);

      struct reg_table *reg_table = reg_file[reg_type]->reg_table[table_type];
      reg_table->ops->consume(reg_table, reg_id, op);
    }
  }
}

static inline void reg_file_produce_dst(Op *op, int *reg_table_types, int reg_table_num) {
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    for (uns jj = 0; jj < reg_table_num; ++jj) {
      int table_type = reg_table_types[jj];
      ASSERT(op->proc_id, table_type > REG_TABLE_TYPE_ARCHITECTURAL && table_type < REG_TABLE_TYPE_NUM);
      int reg_id = op->dst_reg_id[ii][table_type];
      ASSERT(op->proc_id, reg_id != REG_TABLE_REG_ID_INVALID);

      struct reg_table *reg_table = reg_file[reg_type]->reg_table[table_type];
      reg_table->ops->produce(reg_table, reg_id, op);
    }
  }
}

static inline void reg_file_flush_mispredict(Op *op, int *reg_table_types, int reg_table_num) {
  ASSERT(op->proc_id, op->off_path);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    int reg_type = reg_file_get_reg_type(op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    for (uns jj = 0; jj < reg_table_num; ++jj) {
      int table_type = reg_table_types[jj];
      ASSERT(op->proc_id, table_type > REG_TABLE_TYPE_ARCHITECTURAL && table_type < REG_TABLE_TYPE_NUM);
      int reg_id = op->dst_reg_id[ii][table_type];
      if (reg_id == REG_TABLE_REG_ID_INVALID)
        continue;

      // release the current mispredicted register entry
      struct reg_table *reg_table = reg_file[reg_type]->reg_table[table_type];
      struct reg_table_entry *entry = &reg_table->entries[reg_id];
      ASSERT(op->proc_id, entry != NULL);

      entry->num_refs--;
      ASSERT(op->proc_id, entry->off_path || entry->num_refs > 0);
      if (entry->num_refs > 0) {
        continue;
      }

      ASSERT(op->proc_id, entry->reg_state != REG_TABLE_ENTRY_STATE_FREE);
      reg_table->ops->free(reg_table, entry);
    }
  }
}

// mark the previous entry with same archituctural id before the committed one as dead and remove it
static inline void reg_file_release_prev(Op *op, int *reg_table_types, int reg_table_num) {
  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    for (uns jj = 0; jj < reg_table_num; ++jj) {
      int table_type = reg_table_types[jj];
      ASSERT(op->proc_id, table_type > REG_TABLE_TYPE_ARCHITECTURAL && table_type < REG_TABLE_TYPE_NUM);
      int reg_id = op->src_reg_id[ii][table_type];
      ASSERT(op->proc_id, reg_id != REG_TABLE_REG_ID_INVALID);

      struct reg_table *reg_table = reg_file[reg_type]->reg_table[table_type];
      struct reg_table_entry *entry = &reg_table->entries[reg_id];

      ASSERT(op->proc_id, entry != NULL && entry->onpath_consumers_num > 0);
    }
  }

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    for (uns jj = 0; jj < reg_table_num; ++jj) {
      int table_type = reg_table_types[jj];
      ASSERT(op->proc_id, table_type > REG_TABLE_TYPE_ARCHITECTURAL && table_type < REG_TABLE_TYPE_NUM);
      int reg_id = op->dst_reg_id[ii][table_type];
      ASSERT(op->proc_id, reg_id != REG_TABLE_REG_ID_INVALID);

      struct reg_table *reg_table = reg_file[reg_type]->reg_table[table_type];
      struct reg_table_entry *entry = &reg_table->entries[reg_id];
      ASSERT(op->proc_id, entry != NULL);
      ASSERT(op->proc_id, entry->reg_state == REG_TABLE_ENTRY_STATE_PRODUCED || REG_RENAMING_MOVE_ELIMINATE);

      // mark register as committed at retirement
      ASSERT(op->proc_id,
             reg_table->parent_reg_table->entries[entry->parent_reg_id].child_reg_id != REG_TABLE_REG_ID_INVALID);
      entry->reg_state = REG_TABLE_ENTRY_STATE_COMMIT;

      int prev_reg_id = op->prev_dst_reg_id[ii][table_type];
      ASSERT(op->proc_id, prev_reg_id != REG_TABLE_REG_ID_INVALID);

      struct reg_table_entry *prev_entry = &reg_table->entries[prev_reg_id];
      prev_entry->num_refs--;
      ASSERT(op->proc_id, prev_entry->num_refs >= 0);
      if (prev_entry->num_refs > 0)
        continue;

      // release the previous op before the current committed op
      ASSERT(op->proc_id, prev_entry->reg_state == REG_TABLE_ENTRY_STATE_COMMIT || prev_entry->op == &invalid_op);
      ASSERT(op->proc_id, prev_entry->onpath_consumed_count == prev_entry->onpath_consumers_num);
      reg_table->ops->free(reg_table, prev_entry);
    }
  }
}

/**************************************************************************************/
/* checkpoint management */

static inline void reg_file_init_checkpoint() {
  for (uns ii = 0; ii < REG_FILE_REG_TYPE_NUM; ++ii) {
    reg_file[ii]->reg_checkpoint = (struct reg_checkpoint *)malloc(sizeof(struct reg_checkpoint));
    reg_file[ii]->reg_checkpoint->entries = (struct reg_table_entry *)malloc(
        sizeof(struct reg_table_entry) * reg_file[ii]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL]->size);
    reg_file[ii]->reg_checkpoint->is_valid = FALSE;
  }
}

/*
  Scarab currently does not support early flushes and will only trigger a flush if the
  oldest mispredicted branch is resolved
  Therefore, only maintain one checkpoint of that mispredicted branch for recovering SRT
*/
static inline void reg_file_snapshot_srt() {
  for (uns ii = 0; ii < REG_FILE_REG_TYPE_NUM; ++ii) {
    struct reg_table *srt = reg_file[ii]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];
    struct reg_checkpoint *checkpoint = reg_file[ii]->reg_checkpoint;
    memcpy(checkpoint->entries, srt->entries, sizeof(struct reg_table_entry) * srt->size);

    ASSERT(map_data->proc_id, !checkpoint->is_valid);
    checkpoint->is_valid = TRUE;
  }
}

/*
  Scarab currently does not support early flushes and will only trigger a flush if the oldest
  mispredicted branch is resolved
  Therefore, only need to recover the SRT to the checkpoint without off_path operands before
  the mispredicted branch
*/
static inline void reg_file_rollback_srt() {
  for (uns ii = 0; ii < REG_FILE_REG_TYPE_NUM; ++ii) {
    struct reg_table *srt = reg_file[ii]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];
    struct reg_checkpoint *checkpoint = reg_file[ii]->reg_checkpoint;
    memcpy(srt->entries, checkpoint->entries, sizeof(struct reg_table_entry) * srt->size);

    ASSERT(map_data->proc_id, checkpoint->is_valid);
    checkpoint->is_valid = FALSE;
  }
}

/**************************************************************************************/
/* register free list operation */

void reg_free_list_init(struct reg_free_list *reg_free_list) {
  ASSERT(map_data->proc_id, reg_free_list != NULL);

  reg_free_list->reg_free_num = 0;
  reg_free_list->reg_free_list_head = NULL;
}

/* push the entry to the free list */
void reg_free_list_free(struct reg_free_list *reg_free_list, struct reg_table_entry *entry) {
  ASSERT(map_data->proc_id, reg_free_list != NULL && entry->next_free == NULL);

  entry->next_free = reg_free_list->reg_free_list_head;
  reg_free_list->reg_free_list_head = entry;
  ++reg_free_list->reg_free_num;
}

/* pop the entry from the free list */
struct reg_table_entry *reg_free_list_alloc(struct reg_free_list *reg_free_list) {
  ASSERT(map_data->proc_id, reg_free_list != NULL && reg_free_list->reg_free_list_head != NULL);

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
  ASSERT(map_data->proc_id, entry != NULL && entry->self_reg_id != REG_TABLE_REG_ID_INVALID);

  entry->op = &invalid_op;
  entry->op_num = 0;
  entry->unique_num = 0;
  entry->off_path = FALSE;

  entry->reg_state = REG_TABLE_ENTRY_STATE_FREE;
  entry->parent_reg_id = REG_TABLE_REG_ID_INVALID;
  entry->child_reg_id = REG_TABLE_REG_ID_INVALID;

  entry->allocated_cycle = MAX_CTR;
  entry->produced_cycle = MAX_CTR;
  entry->spec_consumed_cycle = MAX_CTR;
  entry->onpath_consumed_cycle = MAX_CTR;
  entry->redefined_cycle = MAX_CTR;
  entry->spec_release_cycle = MAX_CTR;
  entry->nonspec_release_cycle = MAX_CTR;

  entry->num_refs = 0;

  entry->onpath_consumers_num = 0;
  entry->onpath_consumed_count = 0;
  entry->onpath_consumed_dist = 0;

  entry->redefined_rename = FALSE;
  entry->redefined_precommit = FALSE;

  entry->lastuse_op_num = 0;
  entry->lastuse_committed = FALSE;

  entry->is_branch = FALSE;
  entry->is_except = FALSE;
  entry->is_atomic = TRUE;
  entry->atomic_pending_consumed = 0;
}

/* update the metadata when it is read during renaming */
void reg_table_entry_read(struct reg_table_entry *entry, Op *op) {
  /*
    traditionally, fill src info from the entry and update not ready bit for wake up
    since the dependency is tracked in the map module, only update the metadata for the research reg file schemes
  */

  if (entry->atomic_pending_consumed != REG_RENAMING_SCHEME_EARLY_RELEASE_PENDING_CONSUMED_MAX) {
    entry->atomic_pending_consumed++;
  }

  if (op->off_path)
    return;

  entry->onpath_consumers_num++;
  entry->lastuse_op_num = op->op_num;
  entry->lastuse_committed = FALSE;
}

/* update reg_table entry by setting its key (lookup reg_id) and value (tag and op whose dest is assigned to reg_id) */
void reg_table_entry_write(struct reg_table_entry *entry, Op *op, int parent_reg_id) {
  ASSERT(op->proc_id, entry != NULL && entry->self_reg_id != REG_TABLE_REG_ID_INVALID);
  ASSERT(op->proc_id, entry->child_reg_id == REG_TABLE_REG_ID_INVALID);

  // write the op into the entry
  entry->op = op;
  entry->op_num = op->op_num;
  entry->unique_num = op->unique_num;
  entry->off_path = op->off_path;

  // update the entry meta data
  entry->parent_reg_id = parent_reg_id;
  entry->reg_state = REG_TABLE_ENTRY_STATE_ALLOC;
  entry->allocated_cycle = cycle_count;

  DEBUG(0, "(entry write)[%lld]: parent_reg_id: %d, self_reg_id: %d, child_reg_id: %d\n", entry->op_num,
        entry->parent_reg_id, entry->self_reg_id, entry->child_reg_id);
}

/* update the metadata when it is consumed during execution */
void reg_table_entry_consume(struct reg_table_entry *entry, Op *op) {
  if (entry->atomic_pending_consumed != REG_RENAMING_SCHEME_EARLY_RELEASE_PENDING_CONSUMED_MAX) {
    entry->atomic_pending_consumed--;
  }

  STAT_EVENT(map_data->proc_id, MAP_STAGE_INT_REG_ISSUE_READ_TOTAL_CONSUMED + entry->reg_type);
  if (entry->spec_consumed_cycle != MAX_CTR) {
    STAT_EVENT(map_data->proc_id, MAP_STAGE_INT_REG_ISSUE_READ_MULTI_CONSUMED + entry->reg_type);
    if (entry->spec_consumed_cycle == cycle_count) {
      STAT_EVENT(map_data->proc_id, MAP_STAGE_INT_REG_ISSUE_READ_SAME_CYCLE_CONSUMED + entry->reg_type);
    }
  }
  entry->spec_consumed_cycle = cycle_count;

  if (op->off_path)
    return;

  entry->onpath_consumed_count++;

  STAT_EVENT(map_data->proc_id, MAP_STAGE_INT_REG_ISSUE_READ_TOTAL_CONSUMED_ONPATH + entry->reg_type);
  if (entry->onpath_consumed_cycle != MAX_CTR) {
    entry->onpath_consumed_dist += (cycle_count - entry->onpath_consumed_cycle);
    STAT_EVENT(map_data->proc_id, MAP_STAGE_INT_REG_ISSUE_READ_MULTI_CONSUMED_ONPATH + entry->reg_type);
    if (entry->onpath_consumed_cycle == cycle_count) {
      STAT_EVENT(map_data->proc_id, MAP_STAGE_INT_REG_ISSUE_READ_SAME_CYCLE_CONSUMED_ONPATH + entry->reg_type);
    }
  }
  entry->onpath_consumed_cycle = cycle_count;
}

/* update the register state to indicate the value is produced during execution*/
void reg_table_entry_produce(struct reg_table_entry *entry, Op *op) {
  if (op->move_eliminated) {
    return;
  }
  ASSERT(map_data->proc_id, entry->reg_state == REG_TABLE_ENTRY_STATE_ALLOC);

  entry->reg_state = REG_TABLE_ENTRY_STATE_PRODUCED;
  entry->produced_cycle = cycle_count;
}

struct reg_table_entry_ops reg_table_entry_ops = {
    .clear = reg_table_entry_clear,
    .read = reg_table_entry_read,
    .write = reg_table_entry_write,
    .consume = reg_table_entry_consume,
    .produce = reg_table_entry_produce,
};

/**************************************************************************************/
/* register table operation */

void reg_table_init(struct reg_table *reg_table, struct reg_table *parent_reg_table, uns reg_table_size, int reg_type,
                    int reg_table_type) {
  // set the parent table pointer for tracking
  reg_table->parent_reg_table = parent_reg_table;

  reg_table->free_list = (struct reg_free_list *)malloc(sizeof(struct reg_free_list));
  reg_table->free_list->ops = &reg_free_list_ops;
  reg_table->free_list->ops->init(reg_table->free_list);

  reg_table->reg_type = reg_type;
  reg_table->reg_table_type = reg_table_type;

  reg_table->size = reg_table_size;
  reg_table->entries = (struct reg_table_entry *)malloc(sizeof(struct reg_table_entry) * reg_table_size);

  // init and insert all the empty entries into the free list
  for (uns ii = 0; ii < reg_table_size; ii++) {
    struct reg_table_entry *entry = &reg_table->entries[ii];
    entry->ops = &reg_table_entry_ops;
    entry->self_reg_id = ii;
    entry->reg_type = reg_type;
    entry->reg_table_type = reg_table_type;
    entry->ops->clear(entry);
    entry->next_free = NULL;
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
    entry->ops->write(entry, &invalid_op, ii);
    entry->num_refs++;
    reg_table->parent_reg_table->entries[entry->parent_reg_id].child_reg_id = entry->self_reg_id;
  }
}

/* do not duplicately read operand register dependency since it is already tracked */
int reg_table_read(struct reg_table *reg_table, Op *op, int parent_reg_id) {
  /*
    since op_info read is done in reg_map_read() in map.c
    do not duplicately read register dependency during renaming
    this module is to manage the allocation of registers in the map_stage
  */

  // lookup the parent table to get the latest self reg id
  int self_reg_id = reg_table->parent_reg_table->entries[parent_reg_id].child_reg_id;
  ASSERT(map_data->proc_id, self_reg_id != REG_TABLE_REG_ID_INVALID);

  struct reg_table_entry *entry = &reg_table->entries[self_reg_id];
  entry->ops->read(entry, op);
  return self_reg_id;
}

/*
  --- 1. allocate a current register table entry from free list
  --- 2. store the op info into the register entry
*/
int reg_table_alloc(struct reg_table *reg_table, Op *op, int parent_reg_id) {
  ASSERT(map_data->proc_id, REG_RENAMING_SCHEME && reg_table != NULL && op != NULL);

  if (op->move_eliminated) {
    ASSERT(op->proc_id, REG_RENAMING_MOVE_ELIMINATE);
    ASSERT(op->proc_id, op->table_info->num_src_regs == 1);
    return op->src_reg_id[0][reg_table->reg_table_type];
  }

  // get the entry from the free list and write the metadata
  struct reg_table_entry *entry = reg_table->free_list->ops->alloc(reg_table->free_list);
  entry->ops->write(entry, op, parent_reg_id);

  return entry->self_reg_id;
}

/* clear all the info of the entry and insert it to the free list */
void reg_table_free(struct reg_table *reg_table, struct reg_table_entry *entry) {
  // collect the counter stat before clearing
  reg_file_collect_released_entry_stat(entry);

  // clear the entry value
  entry->ops->clear(entry);

  // append to free list
  reg_table->free_list->ops->free(reg_table->free_list, entry);
}

/* read the src reg when the dep op is executed */
void reg_table_consume(struct reg_table *reg_table, int reg_id, Op *op) {
  /*
    the dependency wake up will be done in the map module
    only update the metadata of the source entry
  */

  struct reg_table_entry *entry = &reg_table->entries[reg_id];
  entry->ops->consume(entry, op);
}

/* update the register state to indicate the value is produced */
void reg_table_produce(struct reg_table *reg_table, int self_reg_id, Op *op) {
  ASSERT(map_data->proc_id, REG_RENAMING_SCHEME && self_reg_id != REG_TABLE_REG_ID_INVALID);
  struct reg_table_entry *entry = &reg_table->entries[self_reg_id];

  entry->ops->produce(entry, op);
}

struct reg_table_ops reg_table_ops = {
    .init = reg_table_init,
    .read = reg_table_read,
    .alloc = reg_table_alloc,
    .free = reg_table_free,
    .consume = reg_table_consume,
    .produce = reg_table_produce,
};

void reg_table_arch_init(struct reg_table *reg_table, struct reg_table *parent_reg_table, uns reg_table_size,
                         int reg_type, int reg_table_type) {
  reg_table->parent_reg_table = parent_reg_table;
  reg_table->free_list = NULL;
  reg_table->reg_type = reg_type;
  reg_table->reg_table_type = reg_table_type;
  reg_table->size = reg_table_size;
  reg_table->entries = (struct reg_table_entry *)malloc(sizeof(struct reg_table_entry) * reg_table_size);

  // only need to assign the current register index for the children table to track
  for (uns ii = 0; ii < reg_table->size; ii++) {
    struct reg_table_entry *entry = &reg_table->entries[ii];
    entry->ops = &reg_table_entry_ops;
    entry->self_reg_id = ii;
    entry->reg_type = reg_type;
    entry->reg_table_type = reg_table_type;
    entry->ops->clear(entry);

    /* only allocate an architectural register for supported register types (int/vector)
     * and only if the current reg table matches */
    if (reg_file_get_reg_type(ii) == reg_table->reg_type)
      entry->reg_state = REG_TABLE_ENTRY_STATE_ALLOC;
  }
}

struct reg_table_ops reg_table_ops_arch = {
    .init = reg_table_arch_init,
};

/**************************************************************************************/
/* Infinite Register Scheme */

void reg_renaming_scheme_infinite_init(void);
Flag reg_renaming_scheme_infinite_available(uns stage_op_count);
void reg_renaming_scheme_infinite_rename(Op *op);
Flag reg_renaming_scheme_infinite_issue(Op *op);
void reg_renaming_scheme_infinite_consume(Op *op);
void reg_renaming_scheme_infinite_produce(Op *op);
void reg_renaming_scheme_infinite_recover(Op *op);
void reg_renaming_scheme_infinite_precommit(Op *op);
void reg_renaming_scheme_infinite_commit(Op *op);

void reg_renaming_scheme_infinite_init(void) {
  return;
}

Flag reg_renaming_scheme_infinite_available(uns stage_op_count) {
  return TRUE;
}

void reg_renaming_scheme_infinite_rename(Op *op) {
  return;
}

Flag reg_renaming_scheme_infinite_issue(Op *op) {
  return TRUE;
}

void reg_renaming_scheme_infinite_consume(Op *op) {
  return;
}

void reg_renaming_scheme_infinite_produce(Op *op) {
  return;
}

void reg_renaming_scheme_infinite_recover(Op *op) {
  return;
}

void reg_renaming_scheme_infinite_precommit(Op *op) {
  return;
}

void reg_renaming_scheme_infinite_commit(Op *op) {
  return;
}

/**************************************************************************************/
/* Realistic Register Scheme */

void reg_renaming_scheme_realistic_init(void);
Flag reg_renaming_scheme_realistic_available(uns stage_op_count);
void reg_renaming_scheme_realistic_rename(Op *op);
Flag reg_renaming_scheme_realistic_issue(Op *op);
void reg_renaming_scheme_realistic_consume(Op *op);
void reg_renaming_scheme_realistic_produce(Op *op);
void reg_renaming_scheme_realistic_recover(Op *op);
void reg_renaming_scheme_realistic_precommit(Op *op);
void reg_renaming_scheme_realistic_commit(Op *op);

// allocate entries and assign the parent-child relationship of the arch table and the physical table
void reg_renaming_scheme_realistic_init(void) {
  int reg_file_physical_size[REG_FILE_REG_TYPE_NUM] = {REG_TABLE_INTEGER_PHYSICAL_SIZE, REG_TABLE_VECTOR_PHYSICAL_SIZE};

  for (uns ii = 0; ii < REG_FILE_REG_TYPE_NUM; ++ii) {
    ASSERT(map_data->proc_id, reg_file[ii] == NULL);
    reg_file[ii] = (struct reg_file *)malloc(sizeof(struct reg_file));
    reg_file[ii]->reg_type = ii;

    /*
     * the physical reg map is the children table of the arch table
     * the child_reg_id of the arch table is the index of the physical reg map
     */
    reg_file_init_reg_table(REG_TABLE_TYPE_ARCHITECTURAL, NULL, NUM_REG_IDS, ii, &reg_table_ops_arch);

    /*
     * the arch table is the parent table of the physical reg map
     * the parent_reg_id of the physical table is the index of the arch table
     */
    reg_file_init_reg_table(REG_TABLE_TYPE_PHYSICAL, reg_file[ii]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL],
                            reg_file_physical_size[ii], ii, &reg_table_ops);
  }

  reg_file_init_checkpoint();
}

// check if there are enough register entries
Flag reg_renaming_scheme_realistic_available(uns stage_op_count) {
  return reg_file_check_reg_num(REG_TABLE_TYPE_PHYSICAL, stage_op_count);
}

// allocate physical registers of the op and write the ptag info into the op
void reg_renaming_scheme_realistic_rename(Op *op) {
  // read the physical register table by looking up the arch register table
  reg_file_read_src(op, REG_TABLE_TYPE_PHYSICAL, REG_TABLE_TYPE_ARCHITECTURAL);

  // write the physical register table and update the arch register table
  reg_file_write_dst(op, REG_TABLE_TYPE_PHYSICAL, REG_TABLE_TYPE_ARCHITECTURAL);

  // checkpoint the speculative register table for recovering
  if (!op->off_path && op->table_info->cf_type && op->oracle_info.recover_at_exec)
    reg_file_snapshot_srt();
}

// do not check the reg file when issuing
Flag reg_renaming_scheme_realistic_issue(Op *op) {
  return TRUE;
}

// consume the src registers
void reg_renaming_scheme_realistic_consume(Op *op) {
  int reg_table_types[] = {REG_TABLE_TYPE_PHYSICAL};

  // consume the src register in the physical reg table
  reg_file_consume_src(op, reg_table_types, sizeof(reg_table_types) / sizeof(reg_table_types[0]));
}

// produce the dst registers
void reg_renaming_scheme_realistic_produce(Op *op) {
  int reg_table_types[] = {REG_TABLE_TYPE_PHYSICAL};

  // write back the physical register table
  reg_file_produce_dst(op, reg_table_types, sizeof(reg_table_types) / sizeof(reg_table_types[0]));
}

// flush registers of misprediction operands using the ptag info
void reg_renaming_scheme_realistic_recover(Op *op) {
  // do not need to do flushing if it is a decoding flush
  ASSERT(op->proc_id, op->table_info->cf_type);
  if (op->oracle_info.recover_at_decode)
    return;

  // rollback to the status that does not contain any off_path entries
  reg_file_rollback_srt();

  // release the registers from the youngest to the flush point
  int reg_table_types[] = {REG_TABLE_TYPE_PHYSICAL};
  for (Op **op_p = (Op **)list_start_tail_traversal(&td->seq_op_list); op_p && (*op_p)->op_num > op->op_num;
       op_p = (Op **)list_prev_element(&td->seq_op_list)) {
    reg_file_flush_mispredict(*op_p, reg_table_types, sizeof(reg_table_types) / sizeof(reg_table_types[0]));
  }
}

void reg_renaming_scheme_realistic_precommit(Op *op) {
  return;
}

// release the previous register with same architectural id
void reg_renaming_scheme_realistic_commit(Op *op) {
  int reg_table_types[] = {REG_TABLE_TYPE_PHYSICAL};
  reg_file_release_prev(op, reg_table_types, sizeof(reg_table_types) / sizeof(reg_table_types[0]));
}

/**************************************************************************************/
/* Virtual Physical Register Scheme */

void reg_renaming_scheme_late_allocation_init(void);
Flag reg_renaming_scheme_late_allocation_available(uns stage_op_count);
void reg_renaming_scheme_late_allocation_rename(Op *op);
Flag reg_renaming_scheme_late_allocation_issue(Op *op);
void reg_renaming_scheme_late_allocation_consume(Op *op);
void reg_renaming_scheme_late_allocation_produce(Op *op);
void reg_renaming_scheme_late_allocation_recover(Op *op);
void reg_renaming_scheme_late_allocation_precommit(Op *op);
void reg_renaming_scheme_late_allocation_commit(Op *op);

// allocate entries and assign the parent-child relationship for arch, vtag, and ptag tables
void reg_renaming_scheme_late_allocation_init(void) {
  ASSERT(map_data->proc_id, REG_TABLE_INTEGER_VIRTUAL_SIZE >= REG_TABLE_INTEGER_PHYSICAL_SIZE);
  ASSERT(map_data->proc_id, REG_TABLE_VECTOR_VIRTUAL_SIZE >= REG_TABLE_VECTOR_PHYSICAL_SIZE);
  int reg_file_physical_size[REG_FILE_REG_TYPE_NUM] = {REG_TABLE_INTEGER_PHYSICAL_SIZE, REG_TABLE_VECTOR_PHYSICAL_SIZE};
  int reg_file_virtual_size[REG_FILE_REG_TYPE_NUM] = {REG_TABLE_INTEGER_VIRTUAL_SIZE, REG_TABLE_VECTOR_VIRTUAL_SIZE};

  for (uns ii = 0; ii < REG_FILE_REG_TYPE_NUM; ++ii) {
    ASSERT(map_data->proc_id, reg_file[ii] == NULL);
    reg_file[ii] = (struct reg_file *)malloc(sizeof(struct reg_file));
    reg_file[ii]->reg_type = ii;

    /*
     * the arch reg table is the parent table of the vtag table
     * the child_reg_id of the arch table is the index of the vtag reg map table
     */
    reg_file_init_reg_table(REG_TABLE_TYPE_ARCHITECTURAL, NULL, NUM_REG_IDS, ii, &reg_table_ops_arch);

    /*
     * the vtag reg table is the parent table of the ptag table
     * the child_reg_id of the vtag table is the index of the ptag reg map table
     */
    reg_file_init_reg_table(REG_TABLE_TYPE_VIRTUAL, reg_file[ii]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL],
                            reg_file_virtual_size[ii], ii, &reg_table_ops);

    /*
     * the ptag reg table is the child table of the vtag table
     * the parent_reg_id of the ptag table is the index of the vtag reg map table
     */
    reg_file_init_reg_table(REG_TABLE_TYPE_PHYSICAL, reg_file[ii]->reg_table[REG_TABLE_TYPE_VIRTUAL],
                            reg_file_physical_size[ii], ii, &reg_table_ops);
  }

  reg_file_init_checkpoint();
}

// check if there are enough registers in the virtual table instead of the physical registers
Flag reg_renaming_scheme_late_allocation_available(uns stage_op_count) {
  return reg_file_check_reg_num(REG_TABLE_TYPE_VIRTUAL, stage_op_count);
}

// allocate only virtual registers and write the vtag info into the op
void reg_renaming_scheme_late_allocation_rename(Op *op) {
  // read the virtaul register table by looking up the arch register table
  reg_file_read_src(op, REG_TABLE_TYPE_VIRTUAL, REG_TABLE_TYPE_ARCHITECTURAL);

  // write the virtaul register table and update the arch register table
  reg_file_write_dst(op, REG_TABLE_TYPE_VIRTUAL, REG_TABLE_TYPE_ARCHITECTURAL);

  // checkpoint the speculative register table for recovering
  if (!op->off_path && op->table_info->cf_type && op->oracle_info.recover_at_exec)
    reg_file_snapshot_srt();
}

/*
  To avoid deadlock, only allocate physical registers
  if sufficient additional registers exist to serve the oldest op in the ROB
*/
Flag reg_renaming_scheme_late_allocation_issue(Op *op) {
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
  ASSERT(op->proc_id, reserve_op != NULL);

  // do not need to reserve if the reserving head has allocated physical register
  if (reserve_op->dst_reg_id[0][REG_TABLE_TYPE_PHYSICAL] != REG_TABLE_REG_ID_INVALID) {
    ASSERT(op->proc_id, reserve_op->op_num <= op->op_num);
    return reg_file_check_reg_num(REG_TABLE_TYPE_PHYSICAL, 1);
  }

  // if the reserving head has not allocated but the current op is in the head, allow the head to allocate
  if (op->op_num == reserve_op->op_num) {
    return TRUE;
  }

  // reserve registers for the head
  Flag if_available =
      reg_file_check_reg_num(REG_TABLE_TYPE_PHYSICAL, REG_RENAMING_SCHEME_LATE_ALLOCATION_RESERVE_NUM + 1);
  if (!if_available)
    STAT_EVENT(0, MAP_STAGE_LATE_ALLOCATE_SEND_BACK);
  return if_available;
}

// allocate the physical reg during execution using the vtag info of the op and write back the virtual and physical reg
void reg_renaming_scheme_late_allocation_consume(Op *op) {
  // late allocation for physical register entries
  reg_file_read_src(op, REG_TABLE_TYPE_PHYSICAL, REG_TABLE_TYPE_VIRTUAL);
  reg_file_write_dst(op, REG_TABLE_TYPE_PHYSICAL, REG_TABLE_TYPE_VIRTUAL);

  // consume for both register tables
  int reg_table_types[] = {REG_TABLE_TYPE_VIRTUAL, REG_TABLE_TYPE_PHYSICAL};
  reg_file_consume_src(op, reg_table_types, sizeof(reg_table_types) / sizeof(reg_table_types[0]));
}

void reg_renaming_scheme_late_allocation_produce(Op *op) {
  // produce for both register tables
  int reg_table_types[] = {REG_TABLE_TYPE_VIRTUAL, REG_TABLE_TYPE_PHYSICAL};
  reg_file_produce_dst(op, reg_table_types, sizeof(reg_table_types) / sizeof(reg_table_types[0]));
}

void reg_renaming_scheme_late_allocation_recover(Op *op) {
  // do not need to do flushing if it is a decoding flush
  ASSERT(op->proc_id, op->table_info->cf_type);
  if (op->oracle_info.recover_at_decode)
    return;

  // rollback to the status that does not contain any off_path entries
  reg_file_rollback_srt();

  // release the registers from the youngest to the flush point for both register tables
  int reg_table_types[] = {REG_TABLE_TYPE_VIRTUAL, REG_TABLE_TYPE_PHYSICAL};
  for (Op **op_p = (Op **)list_start_tail_traversal(&td->seq_op_list); op_p && (*op_p)->op_num > op->op_num;
       op_p = (Op **)list_prev_element(&td->seq_op_list)) {
    reg_file_flush_mispredict(*op_p, reg_table_types, sizeof(reg_table_types) / sizeof(reg_table_types[0]));
  }
}

void reg_renaming_scheme_late_allocation_precommit(Op *op) {
  return;
}

void reg_renaming_scheme_late_allocation_commit(Op *op) {
  /*
    the previous same architectural id of ptag may be invalid due to the out-of-order allocation
    therefore, a lazy assignment is done here, e.g., tracking the prev_ptag when the op is committed and the prev_ptag
    is to be released
  */
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    int prev_vtag = op->prev_dst_reg_id[ii][REG_TABLE_TYPE_VIRTUAL];
    ASSERT(op->proc_id, prev_vtag != REG_TABLE_REG_ID_INVALID);
    int prev_ptag = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_VIRTUAL]->entries[prev_vtag].child_reg_id;
    ASSERT(op->proc_id, prev_ptag != REG_TABLE_REG_ID_INVALID);
    op->prev_dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL] = prev_ptag;
  }

  int reg_table_types[] = {REG_TABLE_TYPE_VIRTUAL, REG_TABLE_TYPE_PHYSICAL};
  reg_file_release_prev(op, reg_table_types, sizeof(reg_table_types) / sizeof(reg_table_types[0]));
}

/**************************************************************************************/
/* Early Release Common Func */

static void reg_early_release_clear(struct reg_table_entry *entry) {
  if (entry->op_num == 0)
    return;

  // if the producer op of the entry is not yet committed, the op in the entry is still valid
  Op *op = entry->op;
  ASSERT(op->proc_id, op->op_num == entry->op_num && op->unique_num == entry->unique_num);

  // find and clear the corresponding register information inside the operands
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    if (entry->parent_reg_id != op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL])
      continue;

    op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL] = REG_TABLE_REG_ID_INVALID;
    op->dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL] = REG_TABLE_REG_ID_INVALID;
    return;
  }
}

static void reg_early_release_free(struct reg_table *reg_table, struct reg_table_entry *entry) {
  /*
   * if the producer op is still in the ROB, the corresponding register entry info
   * inside that op should be cleared to prevent invalid access;
   * if the producer op has committed, do not clear the information inside that op.
   */
  if (entry->reg_state != REG_TABLE_ENTRY_STATE_COMMIT) {
    reg_early_release_clear(entry);
  }

  reg_table->ops->free(reg_table, entry);
}

/**************************************************************************************/
/* Spec Early Release Register Scheme */

void reg_renaming_scheme_early_release_spec_rename(Op *op);
void reg_renaming_scheme_early_release_spec_consume(Op *op);
void reg_renaming_scheme_early_release_spec_commit(Op *op);

void reg_renaming_scheme_early_release_spec_rename(Op *op) {
  reg_renaming_scheme_realistic_rename(op);

  if (op->off_path)
    return;

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    int prev_ptag = op->prev_dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id, prev_ptag != REG_TABLE_REG_ID_INVALID);
    struct reg_table_entry *prev_entry = &reg_table->entries[prev_ptag];

    // speculative early release mechanisms provide backup storage for recovering, which allows aggressively redefining
    prev_entry->redefined_rename = TRUE;
    prev_entry->redefined_cycle = cycle_count;

    // do register early release
    if (prev_entry->onpath_consumers_num == prev_entry->onpath_consumed_count && prev_entry->redefined_rename) {
      reg_early_release_free(reg_table, prev_entry);
    }
  }
}

void reg_renaming_scheme_early_release_spec_consume(Op *op) {
  reg_renaming_scheme_realistic_consume(op);

  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    int src_reg_id = op->src_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id, src_reg_id != REG_TABLE_REG_ID_INVALID);
    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    struct reg_table_entry *src_entry = &reg_table->entries[src_reg_id];

    // do register early release
    if (src_entry->onpath_consumers_num == src_entry->onpath_consumed_count && src_entry->redefined_rename) {
      reg_early_release_free(reg_table, src_entry);
    }
  }
}

void reg_renaming_scheme_early_release_spec_commit(Op *op) {
  /* the physical register is already released during renaming or execution */

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    // if the corresponding entry is early released, the reg info of this op is cleared
    int reg_type = reg_file_get_reg_type(op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    int reg_id = op->dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id, reg_id != REG_TABLE_REG_ID_INVALID);

    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    struct reg_table_entry *entry = &reg_table->entries[reg_id];
    ASSERT(op->proc_id, entry != NULL && entry->reg_state == REG_TABLE_ENTRY_STATE_PRODUCED);

    // mark register as committed at retirement to avoid accessing the op object of this entry
    ASSERT(op->proc_id,
           reg_table->parent_reg_table->entries[entry->parent_reg_id].child_reg_id != REG_TABLE_REG_ID_INVALID);
    entry->reg_state = REG_TABLE_ENTRY_STATE_COMMIT;

    // make sure the redefined one is early freed
    int prev_reg_id = op->prev_dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id,
           prev_reg_id != REG_TABLE_REG_ID_INVALID || REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_EARLY_RELEASE_ATOMIC);
    if (prev_reg_id == REG_TABLE_REG_ID_INVALID)
      continue;

    struct reg_table_entry *prev_entry = &reg_table->entries[prev_reg_id];
    ASSERT(op->proc_id, prev_entry->op_num == 0 || prev_entry->op_num > op->op_num);
  }
}

/**************************************************************************************/
/* Non-Spec Early Release Register Scheme */

/*
 * In this mechanism, physical registers are freed once all the consumers execute if the redefining
 * instruction becomes non-speculative. A counter is used to track the pending consumed.
 *
 * This ALGO will do the register early release if the following holds:
 *    (1) the redefine-instruction of the producer needs to be precommitted
 *    (2) the pending consumed count needs to be zero
 */

void reg_renaming_scheme_early_release_nonspec_precommit(Op *op);
void reg_renaming_scheme_early_release_nonspec_consume(Op *op);

void reg_renaming_scheme_early_release_nonspec_precommit(Op *op) {
  ASSERT(op->proc_id, !op->off_path);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->inst_info->dests[ii].id);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    int prev_ptag = op->prev_dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id,
           prev_ptag != REG_TABLE_REG_ID_INVALID || REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_EARLY_RELEASE_ATOMIC);
    if (prev_ptag == REG_TABLE_REG_ID_INVALID)
      continue;

    struct reg_table_entry *prev_entry = &reg_table->entries[prev_ptag];
    prev_entry->redefined_precommit = TRUE;
    prev_entry->redefined_cycle = cycle_count;

    // do register early release
    if (prev_entry->onpath_consumers_num == prev_entry->onpath_consumed_count && prev_entry->redefined_precommit) {
      reg_early_release_free(reg_table, prev_entry);
    }
  }
}

void reg_renaming_scheme_early_release_nonspec_consume(Op *op) {
  reg_renaming_scheme_realistic_consume(op);

  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    int src_reg_id = op->src_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id, src_reg_id != REG_TABLE_REG_ID_INVALID);
    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    struct reg_table_entry *src_entry = &reg_table->entries[src_reg_id];

    // do register early release
    if (src_entry->onpath_consumers_num == src_entry->onpath_consumed_count && src_entry->redefined_precommit) {
      reg_early_release_free(reg_table, src_entry);
    }
  }
}

/**************************************************************************************/
/* Last-Use Early Release Register Scheme */

/*
 * In this mechanism, physical registers are freed once the last instruction reading the register
 * has committed if the redefining instruction becomes non-speculative. A last-use table is employed
 * to track the last consumer.
 *
 * This technique was first introduced in:
 *    "Hardware schemes for early register release," in ICPP, IEEE, 2002.
 *
 * This ALGO will do the register early release if the following holds:
 *    (1) the redefine-instruction of the producer needs to be precommitted
 *    (2) the last-use instruction of the producer needs to be committed
 */

void reg_renaming_scheme_early_release_lastuse_precommit(Op *op);
void reg_renaming_scheme_early_release_lastuse_commit(Op *op);

void reg_renaming_scheme_early_release_lastuse_precommit(Op *op) {
  ASSERT(op->proc_id, !op->off_path);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    int prev_ptag = op->prev_dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id, prev_ptag != REG_TABLE_REG_ID_INVALID);

    struct reg_table_entry *prev_entry = &reg_table->entries[prev_ptag];
    prev_entry->redefined_precommit = TRUE;

    // directly early release for unconsumed producers
    if (prev_entry->onpath_consumers_num == 0) {
      prev_entry->lastuse_committed = TRUE;
    }

    // do register early release
    if (prev_entry->lastuse_committed && prev_entry->redefined_precommit) {
      reg_early_release_free(reg_table, prev_entry);
    }
  }
}

void reg_renaming_scheme_early_release_lastuse_commit(Op *op) {
  /* when the last-use consumer is committed, early release the producer instruction
   * if the redefine-instruction of the producer is precommitted */
  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    int reg_id = op->src_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id, reg_id != REG_TABLE_REG_ID_INVALID);

    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    struct reg_table_entry *entry = &reg_table->entries[reg_id];

    // the last-use metadata is overwritten for every on-path read of the producer during renaming
    if (entry->lastuse_op_num == op->op_num) {
      entry->lastuse_committed = TRUE;
    }

    // do register early release
    if (entry->lastuse_committed && entry->redefined_precommit) {
      reg_early_release_free(reg_table, entry);
    }
  }

  reg_renaming_scheme_early_release_spec_commit(op);
}

/**************************************************************************************/
/* Atomic Early Release Register Scheme */

/*
 * In this mechanism, a physical register is freed once all of its consumer instructions
 * have executed when there are not conditional branches or exception-causing instructions
 * between the renaming instruction and the redefining instruction. A 3-bit counter is
 * used to track the number of pending consumers.
 *
 * This technique was first introduced in:
 *    "ATR: Out-of-Order Register Release Exploiting Atomic Regions"
 *    Y. Zhao, S. Oh, M. Xu, and H. Litz
 *    (MICRO) International Symposium on Microarchitecture, 2025
 *
 * This ALGO will do the register early release if the following holds:
 *    (1) the register have been redefined
 *    (2) the register is atomic
 *    (3) all of its consumer instructions have executed
 */

static void reg_early_release_atomic_identify(Op *op) {
  Flag is_branch = FALSE;
  Flag is_except = FALSE;

  // control flows may lead to mispredictions
  if (op->table_info->cf_type) {
    is_branch = TRUE;
  }

  // store/load/div may lead to exceptions
  if ((op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_ST) ||
      (op->inst_info->table_info->true_op_type >= XED_ICLASS_DIV &&
       op->inst_info->table_info->true_op_type <= XED_ICLASS_DIVSS)) {
    is_except = TRUE;
  }

  if (!is_branch && !is_except)
    return;

  // deactivate the atomic entries if there are branches or exception-causing instructions
  for (uns ii = 0; ii < REG_FILE_REG_TYPE_NUM; ++ii) {
    struct reg_table *reg_table_arch = reg_file[ii]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];
    for (uns jj = 0; jj < reg_table_arch->size; ++jj) {
      int ptag = reg_table_arch->entries[jj].child_reg_id;
      if (ptag == REG_TABLE_REG_ID_INVALID)
        continue;

      struct reg_table_entry *entry = &reg_file[ii]->reg_table[REG_TABLE_TYPE_PHYSICAL]->entries[ptag];
      ASSERT(op->proc_id, entry->self_reg_id == ptag && entry->parent_reg_id == jj);
      ASSERT(op->proc_id, entry->reg_state != REG_TABLE_ENTRY_STATE_FREE);
      ASSERT(op->proc_id, !entry->redefined_rename || !entry->is_atomic);

      if (!is_branch && !is_except)
        continue;

      entry->atomic_pending_consumed = REG_RENAMING_SCHEME_EARLY_RELEASE_PENDING_CONSUMED_MAX;
      entry->is_atomic = FALSE;
      if (is_branch)
        entry->is_branch = TRUE;
      if (is_except)
        entry->is_except = TRUE;
    }
  }
}

void reg_renaming_scheme_early_release_atomic_rename(Op *op);
void reg_renaming_scheme_early_release_atomic_consume(Op *op);
void reg_renaming_scheme_early_release_atomic_commit(Op *op);

void reg_renaming_scheme_early_release_atomic_rename(Op *op) {
  reg_renaming_scheme_realistic_rename(op);

  reg_early_release_atomic_identify(op);

  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    int prev_ptag = op->prev_dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id, prev_ptag != REG_TABLE_REG_ID_INVALID);

    // update metadata for assertion only
    struct reg_table_entry *prev_entry = &reg_table->entries[prev_ptag];
    prev_entry->redefined_rename = TRUE;
    prev_entry->redefined_cycle = cycle_count;

    int arch_id = op->inst_info->dests[ii].id;
    int ptag = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL]->entries[arch_id].child_reg_id;
    Flag redefined = (prev_entry->self_reg_id != ptag);
    Flag atomic = (prev_entry->atomic_pending_consumed != REG_RENAMING_SCHEME_EARLY_RELEASE_PENDING_CONSUMED_MAX);

    if (redefined && atomic) {
      ASSERT(op->proc_id, prev_entry->redefined_rename && prev_entry->is_atomic);

      // avoid multiple releasing when this op is committed
      op->prev_dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL] = REG_TABLE_REG_ID_INVALID;

      // early release the prev reg if: 1. it is redefined; 2. it is atomic; 3. no more pending consumers
      if (prev_entry->atomic_pending_consumed == 0) {
        reg_early_release_free(reg_table, prev_entry);
      }
    }
  }
}

void reg_renaming_scheme_early_release_atomic_consume(Op *op) {
  reg_renaming_scheme_realistic_consume(op);

  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    int src_reg_id = op->src_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id, src_reg_id != REG_TABLE_REG_ID_INVALID);
    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    struct reg_table_entry *src_entry = &reg_table->entries[src_reg_id];

    int arch_id = op->inst_info->srcs[ii].id;
    int ptag = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL]->entries[arch_id].child_reg_id;
    Flag redefined = (src_entry->self_reg_id != ptag);

    // early release the src reg if: 1. it is redefined; 2. it is atomic; 3. no more pending consumers
    if (redefined && src_entry->atomic_pending_consumed == 0) {
      ASSERT(op->proc_id, src_entry->redefined_rename && src_entry->is_atomic);
      reg_early_release_free(reg_table, src_entry);
    }
  }
}

void reg_renaming_scheme_early_release_atomic_commit(Op *op) {
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->inst_info->dests[ii].id);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;
    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];

    // do conventional register release if the register is not atomic (the prev_reg_id is not cleared)
    int prev_reg_id = op->prev_dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    if (prev_reg_id != REG_TABLE_REG_ID_INVALID) {
      struct reg_table_entry *prev_entry = &reg_table->entries[prev_reg_id];
      ASSERT(op->proc_id,
             prev_entry->onpath_consumed_count == prev_entry->onpath_consumers_num || !prev_entry->is_atomic);
      reg_table->ops->free(reg_table, prev_entry);
    }

    // mark register as committed at retirement for metadata clear
    int reg_id = op->dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    if (reg_id != REG_TABLE_REG_ID_INVALID) {
      struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
      struct reg_table_entry *entry = &reg_table->entries[reg_id];
      ASSERT(op->proc_id, entry != NULL && entry->reg_state == REG_TABLE_ENTRY_STATE_PRODUCED);

      ASSERT(op->proc_id,
             reg_table->parent_reg_table->entries[entry->parent_reg_id].child_reg_id != REG_TABLE_REG_ID_INVALID);
      entry->reg_state = REG_TABLE_ENTRY_STATE_COMMIT;
    }
  }
}

/**************************************************************************************/
/* Non-Spec Atomic Early Release Register Scheme */

/*
 * This mechanism combines both the atomic early release scheme and the non-spec early
 * release scheme.
 *
 * This ALGO will do the register early release if the following holds:
 *    (1) the register have been redefined
 *    (2) all of its consumer instructions have executed
 *    (3) the register is atomic or its redefining instruction becomes non-speculative
 */

void reg_renaming_scheme_early_release_nonspec_atomic_consume(Op *op);

void reg_renaming_scheme_early_release_nonspec_atomic_consume(Op *op) {
  reg_renaming_scheme_realistic_consume(op);

  for (uns ii = 0; ii < op->table_info->num_src_regs; ++ii) {
    int reg_type = reg_file_get_reg_type(op->src_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL]);
    if (reg_type == REG_FILE_REG_TYPE_OTHER)
      continue;

    int src_reg_id = op->src_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    ASSERT(op->proc_id, src_reg_id != REG_TABLE_REG_ID_INVALID);
    struct reg_table *reg_table = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL];
    struct reg_table_entry *src_entry = &reg_table->entries[src_reg_id];

    int arch_id = op->inst_info->srcs[ii].id;
    int ptag = reg_file[reg_type]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL]->entries[arch_id].child_reg_id;
    Flag redefined = (src_entry->self_reg_id != ptag);

    // early release the atomic register
    if (redefined && src_entry->atomic_pending_consumed == 0) {
      ASSERT(op->proc_id, src_entry->redefined_rename && src_entry->is_atomic);
      reg_early_release_free(reg_table, src_entry);
      continue;
    }

    // early release the nonspec register
    if (src_entry->onpath_consumers_num == src_entry->onpath_consumed_count && src_entry->redefined_precommit) {
      reg_early_release_free(reg_table, src_entry);
    }
  }
}

/**************************************************************************************/
/* Register File Function Driven Table */

struct reg_renaming_scheme_func {
  void (*init)(void);
  Flag (*available)(uns stage_op_count);
  void (*rename)(Op *op);
  Flag (*issue)(Op *op);
  void (*consume)(Op *op);
  void (*produce)(Op *op);
  void (*recover)(Op *op);
  void (*precommit)(Op *op);
  void (*commit)(Op *op);
};

// clang-format off
struct reg_renaming_scheme_func reg_renaming_scheme_func_table[REG_RENAMING_SCHEME_NUM] = {
  // REG_RENAMING_SCHEME_INFINITE
  {
    .init = reg_renaming_scheme_infinite_init,
    .available = reg_renaming_scheme_infinite_available,
    .rename = reg_renaming_scheme_infinite_rename,
    .issue = reg_renaming_scheme_infinite_issue,
    .consume = reg_renaming_scheme_infinite_consume,
    .produce = reg_renaming_scheme_infinite_produce,
    .recover = reg_renaming_scheme_infinite_recover,
    .precommit = reg_renaming_scheme_infinite_precommit,
    .commit = reg_renaming_scheme_infinite_commit
  },
  // REG_RENAMING_SCHEME_REALISTIC
  {
    .init = reg_renaming_scheme_realistic_init,
    .available = reg_renaming_scheme_realistic_available,
    .rename = reg_renaming_scheme_realistic_rename,
    .issue = reg_renaming_scheme_realistic_issue,
    .consume = reg_renaming_scheme_realistic_consume,
    .produce = reg_renaming_scheme_realistic_produce,
    .recover = reg_renaming_scheme_realistic_recover,
    .precommit = reg_renaming_scheme_realistic_precommit,
    .commit = reg_renaming_scheme_realistic_commit
  },
  // REG_RENAMING_SCHEME_LATE_ALLOCATION
  {
    .init = reg_renaming_scheme_late_allocation_init,
    .available = reg_renaming_scheme_late_allocation_available,
    .rename = reg_renaming_scheme_late_allocation_rename,
    .issue = reg_renaming_scheme_late_allocation_issue,
    .consume = reg_renaming_scheme_late_allocation_consume,
    .produce = reg_renaming_scheme_late_allocation_produce,
    .recover = reg_renaming_scheme_late_allocation_recover,
    .precommit = reg_renaming_scheme_late_allocation_precommit,
    .commit = reg_renaming_scheme_late_allocation_commit
  },
  // REG_RENAMING_SCHEME_EARLY_RELEASE_SPEC
  {
    .init = reg_renaming_scheme_realistic_init,
    .available = reg_renaming_scheme_realistic_available,
    .rename = reg_renaming_scheme_early_release_spec_rename,
    .issue = reg_renaming_scheme_realistic_issue,
    .consume = reg_renaming_scheme_early_release_spec_consume,
    .produce = reg_renaming_scheme_realistic_produce,
    .recover = reg_renaming_scheme_realistic_recover,
    .precommit = reg_renaming_scheme_realistic_precommit,
    .commit = reg_renaming_scheme_early_release_spec_commit
  },
  // REG_RENAMING_SCHEME_EARLY_RELEASE_NONSPEC
  {
    .init = reg_renaming_scheme_realistic_init,
    .available = reg_renaming_scheme_realistic_available,
    .rename = reg_renaming_scheme_realistic_rename,
    .issue = reg_renaming_scheme_realistic_issue,
    .consume = reg_renaming_scheme_early_release_nonspec_consume,
    .produce = reg_renaming_scheme_realistic_produce,
    .recover = reg_renaming_scheme_realistic_recover,
    .precommit = reg_renaming_scheme_early_release_nonspec_precommit,
    .commit = reg_renaming_scheme_early_release_spec_commit
  },
  // REG_RENAMING_SCHEME_EARLY_RELEASE_LASTUSE
  {
    .init = reg_renaming_scheme_realistic_init,
    .available = reg_renaming_scheme_realistic_available,
    .rename = reg_renaming_scheme_realistic_rename,
    .issue = reg_renaming_scheme_realistic_issue,
    .consume = reg_renaming_scheme_realistic_consume,
    .produce = reg_renaming_scheme_realistic_produce,
    .recover = reg_renaming_scheme_realistic_recover,
    .precommit = reg_renaming_scheme_early_release_lastuse_precommit,
    .commit = reg_renaming_scheme_early_release_lastuse_commit
  },
  // REG_RENAMING_SCHEME_EARLY_RELEASE_ATOMIC
  {
    .init = reg_renaming_scheme_realistic_init,
    .available = reg_renaming_scheme_realistic_available,
    .rename = reg_renaming_scheme_early_release_atomic_rename,
    .issue = reg_renaming_scheme_realistic_issue,
    .consume = reg_renaming_scheme_early_release_atomic_consume,
    .produce = reg_renaming_scheme_realistic_produce,
    .recover = reg_renaming_scheme_realistic_recover,
    .precommit = reg_renaming_scheme_realistic_precommit,
    .commit = reg_renaming_scheme_early_release_atomic_commit
  },
  // REG_RENAMING_SCHEME_EARLY_RELEASE_NONSPEC_ATOMIC
  {
    .init = reg_renaming_scheme_realistic_init,
    .available = reg_renaming_scheme_realistic_available,
    .rename = reg_renaming_scheme_early_release_atomic_rename,
    .issue = reg_renaming_scheme_realistic_issue,
    .consume = reg_renaming_scheme_early_release_nonspec_atomic_consume,
    .produce = reg_renaming_scheme_realistic_produce,
    .recover = reg_renaming_scheme_realistic_recover,
    .precommit = reg_renaming_scheme_early_release_nonspec_precommit,
    .commit = reg_renaming_scheme_early_release_spec_commit
  },
};
// clang-format on

/**************************************************************************************/
/* External Calling */

/*
  Called by:
  --- map.c -> when map init
  Procedure:
  --- allocate the register table entries by the config size
*/
void reg_file_init(void) {
  ASSERT(map_data->proc_id,
         REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_INFINITE && REG_RENAMING_SCHEME < REG_RENAMING_SCHEME_NUM);
  reg_file = map_data->reg_file;
  reg_renaming_scheme_func_table[REG_RENAMING_SCHEME].init();
}

/*
  Called by:
  --- map_stage.c -> before an op is fetched from cache stage data
  Procedure:
  --- check if there are enough register entries
*/
Flag reg_file_available(uns stage_op_count) {
  ASSERT(map_data->proc_id,
         REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_INFINITE && REG_RENAMING_SCHEME < REG_RENAMING_SCHEME_NUM);
  reg_file = map_data->reg_file;
  return reg_renaming_scheme_func_table[REG_RENAMING_SCHEME].available(stage_op_count);
}

/*
  Called by:
  --- map_stage.c -> when an op is fetched from cache stage data
  Procedure:
  --- look up src register and fill the entry into src_info
  --- allocate an entry and store the op info into the register entry
*/
void reg_file_rename(Op *op) {
  ASSERT(map_data->proc_id,
         REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_INFINITE && REG_RENAMING_SCHEME < REG_RENAMING_SCHEME_NUM);
  reg_file = map_data->reg_file;

  // update the arch register id in the op for child tables processing
  reg_file_extract_arch_reg_id(op);

  // check if the op can be move eliminated before inlining
  reg_file_move_eliminate(op);

  // allocate physical registers
  reg_renaming_scheme_func_table[REG_RENAMING_SCHEME].rename(op);

  reg_file_collect_rename_stat(op);
}

/*
  Called by:
  --- exec_stage.c -> when the op is going to be executed
  Procedure:
  --- do additional checking before execution
*/
Flag reg_file_issue(Op *op) {
  ASSERT(map_data->proc_id,
         REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_INFINITE && REG_RENAMING_SCHEME < REG_RENAMING_SCHEME_NUM);
  reg_file = map_data->reg_file;
  return reg_renaming_scheme_func_table[REG_RENAMING_SCHEME].issue(op);
}

/*
  Called by:
  --- exec.c -> when the op is going to be executed
  Procedure:
  --- consume the src registers
*/
void reg_file_consume(Op *op) {
  ASSERT(map_data->proc_id,
         REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_INFINITE && REG_RENAMING_SCHEME < REG_RENAMING_SCHEME_NUM);
  reg_file = map_data->reg_file;
  reg_renaming_scheme_func_table[REG_RENAMING_SCHEME].consume(op);
}

/*
  Called by:
  --- map.c -> when the op is waking up the dependent ops
  Procedure:
  --- write back the dst registers
*/
void reg_file_produce(Op *op) {
  ASSERT(map_data->proc_id,
         REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_INFINITE && REG_RENAMING_SCHEME < REG_RENAMING_SCHEME_NUM);
  reg_file = map_data->reg_file;
  reg_renaming_scheme_func_table[REG_RENAMING_SCHEME].produce(op);
}

/*
  Called by:
  --- cmp.c -> when a misprediction occurs, it should happen before the op list flush
  Procedure:
  --- flush registers of misprediction operands
*/
void reg_file_recover(Op *op) {
  ASSERT(map_data->proc_id,
         REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_INFINITE && REG_RENAMING_SCHEME < REG_RENAMING_SCHEME_NUM);
  reg_file = map_data->reg_file;
  reg_renaming_scheme_func_table[REG_RENAMING_SCHEME].recover(op);
}

/*
  Called by:
  --- node_stage.c -> when the op is precommitted
  Procedure:
  --- update the register metadata when an op is non-spec
*/
void reg_file_precommit(Op *op) {
  ASSERT(map_data->proc_id,
         REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_INFINITE && REG_RENAMING_SCHEME < REG_RENAMING_SCHEME_NUM);
  reg_file = map_data->reg_file;
  reg_renaming_scheme_func_table[REG_RENAMING_SCHEME].precommit(op);
}

/*
  Called by:
  --- node_stage.c -> when the op is retired
  Procedure:
  --- release the previous register with same architectural id
*/
void reg_file_commit(Op *op) {
  ASSERT(map_data->proc_id,
         REG_RENAMING_SCHEME >= REG_RENAMING_SCHEME_INFINITE && REG_RENAMING_SCHEME < REG_RENAMING_SCHEME_NUM);
  reg_file = map_data->reg_file;
  reg_renaming_scheme_func_table[REG_RENAMING_SCHEME].commit(op);
}
