/* Copyright 2020 HPS/SAFARI Research Groups
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
 * File         : map.c
 * Author       : HPS Research Group
 * Date         : 2/16/1999
 * Description  :
 ***************************************************************************************/

#include "map.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "core.param.h"
#include "memory/memory.param.h"

#include "libs/hash_lib.h"

#include "cmp_model.h"
#include "map_rename.h"
#include "model.h"
#include "op_info.h"
#include "statistics.h"
#include "thread.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MAP, ##args)
#define DEBUGU(proc_id, args...) _DEBUGU(proc_id, DEBUG_MAP, ##args)

#define WAKE_UP_ENTRIES_INC 256 /* default 256 */
#define MEM_ADDR_SRC 0          /* address for memory instructions calculated off source 0 */

#define MEM_MAP_ENTRY_SIZE_LOG 3
#define MEM_MAP_ENTRY_SIZE (1 << MEM_MAP_ENTRY_SIZE_LOG)
#define MEM_MAP_BYTE_IN_ENTRY(va) ((va) & (MEM_MAP_ENTRY_SIZE - 1))
#define MEM_MAP_ENTRY_ADDR(va) ((va) & ~(Addr)(MEM_MAP_ENTRY_SIZE - 1))

#define MEM_MAP_KEY(va) ((va) >> MEM_MAP_ENTRY_SIZE_LOG)
#define MEM_MAP_BYTE_INDEX(byte, off_path) ((byte) + ((off_path) ? MEM_MAP_ENTRY_SIZE : 0))

/**************************************************************************************/
/* Types */

typedef struct Mem_Map_Entry_struct {
  Op* op[2 * MEM_MAP_ENTRY_SIZE]; /* last op to write (invalid when committed),
                                   * first half onpath, second half offpath*/
  uns flag_mask;                  /* offpath flags, one per byte */
  uns store_mask;                 /* shows position of all distinct stores
                                   * supplying a partial value to this map entry */
} Mem_Map_Entry;

/* Data structure for easy traversal of memory map hash given an
   access with an address and size */
typedef struct Mem_Map_Traversal_struct {
  Addr entry_addr; /* Entry address, can be used by caller */
  Addr first_entry_addr;
  Addr last_entry_addr;
  uns byte; /* Byte within entry, can be used by caller */
  uns last_byte;
  uns first_entry_first_byte;
  uns last_entry_last_byte;
} Mem_Map_Traversal;

/**************************************************************************************/
/* External variables */

extern Op invalid_op;

/**************************************************************************************/
/* Global Variables */

Map_Data* map_data = NULL;

const char* const dep_type_names[NUM_DEP_TYPES] = {
    "REG_DATA",
    "MEM_ADDR",
    "MEM_DATA",
};

/**************************************************************************************/
/*
 * map_mem_*_traversal_*:
 * these functions traverse the memory map entries/bytes within those entries
 * given an access with an address and size.
 */

static inline Flag mem_map_entry_traversal_done(Mem_Map_Traversal* traversal) {
  return traversal->entry_addr == ADDR_PLUS_OFFSET(traversal->last_entry_addr, MEM_MAP_ENTRY_SIZE);
}

static inline void mem_map_entry_traversal_init(Mem_Map_Traversal* traversal, Addr va, uns size) {
  Addr last_va = ADDR_PLUS_OFFSET(va, size - 1);  // last byte in access
  traversal->first_entry_addr = MEM_MAP_ENTRY_ADDR(va);
  traversal->last_entry_addr = MEM_MAP_ENTRY_ADDR(last_va);
  traversal->entry_addr = traversal->first_entry_addr;
  traversal->first_entry_first_byte = MEM_MAP_BYTE_IN_ENTRY(va);
  traversal->last_entry_last_byte = MEM_MAP_BYTE_IN_ENTRY(last_va);

  // force the traversal to be "done" if the size is 0
  if (size == 0) {
    traversal->entry_addr = ADDR_PLUS_OFFSET(traversal->last_entry_addr, MEM_MAP_ENTRY_SIZE);
    ASSERT(get_proc_id_from_cmp_addr(va), mem_map_entry_traversal_done(traversal));
  }
}

static inline void mem_map_entry_traversal_next(Mem_Map_Traversal* traversal) {
  traversal->entry_addr = ADDR_PLUS_OFFSET(traversal->entry_addr, MEM_MAP_ENTRY_SIZE);
}

static inline void mem_map_byte_traversal_init(Mem_Map_Traversal* traversal) {
  traversal->byte = traversal->entry_addr == traversal->first_entry_addr ? traversal->first_entry_first_byte : 0;
  traversal->last_byte =
      traversal->entry_addr == traversal->last_entry_addr ? traversal->last_entry_last_byte : MEM_MAP_ENTRY_SIZE - 1;
  ASSERT(0, traversal->byte <= traversal->last_byte);
}

static inline Flag mem_map_byte_traversal_done(Mem_Map_Traversal* traversal) {
  return traversal->byte > traversal->last_byte;
}

static inline void mem_map_byte_traversal_next(Mem_Map_Traversal* traversal) {
  traversal->byte++;
}

static inline void mem_map_entry_recover(void* hash_entry, void* arg) {
  Mem_Map_Entry* entry = (Mem_Map_Entry*)hash_entry;
  entry->flag_mask = 0;
}

/**************************************************************************************/

static inline void map_wakeup_entries_expand() {
  Wake_Up_Entry* new_pool = (Wake_Up_Entry*)calloc(WAKE_UP_ENTRIES_INC, sizeof(Wake_Up_Entry));
  DEBUGU(map_data->proc_id, "Expanding wake up pool to size %d\n", (map_data->wake_up_entries + WAKE_UP_ENTRIES_INC));
  for (uns ii = 0; ii < WAKE_UP_ENTRIES_INC - 1; ii++)
    new_pool[ii].next = &new_pool[ii + 1];
  new_pool[WAKE_UP_ENTRIES_INC - 1].next = map_data->free_list_head;
  map_data->free_list_head = &new_pool[0];
  map_data->wake_up_entries += WAKE_UP_ENTRIES_INC;
  ASSERT(map_data->proc_id, map_data->wake_up_entries <= WAKE_UP_ENTRIES_INC * 128);
}

static inline void map_dep_reset(Map_Dep* map) {
  for (uns ii = 0; ii < NUM_REG_IDS; ii++) {
    map->reg[ii].op = &invalid_op;
    map->reg[ii].op_num = 0;
  }
  map->last_store.op = &invalid_op;
  map->last_store.op_num = 0;
}

static inline void map_dep_remove_store_from(Op* op, Hash_Table* mem_hash) {
  Addr va = op->oracle_info.va;
  Mem_Map_Traversal traversal;

  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);

  /* Iterate through each entry that was written to by the op */
  for (mem_map_entry_traversal_init(&traversal, va, op->oracle_info.mem_size);
       !mem_map_entry_traversal_done(&traversal); mem_map_entry_traversal_next(&traversal)) {
    Mem_Map_Entry* mem_map_p = (Mem_Map_Entry*)hash_table_access(mem_hash, MEM_MAP_KEY(traversal.entry_addr));

    if (!mem_map_p)
      continue;

    /* Iterate through each byte written to by the op (within this entry) */
    for (mem_map_byte_traversal_init(&traversal); !mem_map_byte_traversal_done(&traversal);
         mem_map_byte_traversal_next(&traversal)) {
      uns ind = MEM_MAP_BYTE_INDEX(traversal.byte, op->off_path);
      if (TESTBIT(mem_map_p->store_mask, ind) && mem_map_p->op[ind] == op) {
        CLRBIT(mem_map_p->store_mask, ind);
      }
    }
    if (!mem_map_p->store_mask) {
      hash_table_access_delete(mem_hash, MEM_MAP_KEY(traversal.entry_addr));
    }
  }
}

static inline Op* map_dep_find_last_store(Op* op, Hash_Table* mem_hash, Flag add_all) {
  Addr va = op->oracle_info.va;
  Op* last_src_op = NULL;
  Mem_Map_Traversal traversal;

  /* Iterate through each entry that is read by the op */
  for (mem_map_entry_traversal_init(&traversal, va, op->oracle_info.mem_size);
       !mem_map_entry_traversal_done(&traversal); mem_map_entry_traversal_next(&traversal)) {
    Mem_Map_Entry* mem_map_p = (Mem_Map_Entry*)hash_table_access(mem_hash, MEM_MAP_KEY(traversal.entry_addr));

    if (!mem_map_p)
      continue;

    /* Iterate through each byte read by the op (within this entry) */
    for (mem_map_byte_traversal_init(&traversal); !mem_map_byte_traversal_done(&traversal);
         mem_map_byte_traversal_next(&traversal)) {
      uns ind = MEM_MAP_BYTE_INDEX(traversal.byte, TESTBIT(mem_map_p->flag_mask, traversal.byte));
      if (!TESTBIT(mem_map_p->store_mask, ind))
        continue; /* ensure mem_map_p info is valid */
      Op* src_op = mem_map_p->op[ind];
      ASSERTM(op->proc_id,
              BYTE_OVERLAP(src_op->oracle_info.va, src_op->oracle_info.mem_size, va, op->oracle_info.mem_size),
              "%d@0x%08x and %d@0x%08x\n", src_op->oracle_info.mem_size, (uns32)src_op->oracle_info.va,
              op->oracle_info.mem_size, (uns32)va);
      if (add_all && !src_op->marked) {
        op_sources_add(op, MEM_DATA_DEP, src_op, src_op->op_num, src_op->unique_num);
        src_op->marked = TRUE;  // mark op to avoid adding duplicate sources
        STAT_EVENT(op->proc_id, FORWARDED_LD);
        DEBUG(map_data->proc_id, "Added dep op_num:%s  src_op_num:%s  src_num:%d  op:%s  src:%s\n",
              unsstr64(op->op_num), unsstr64(src_op->op_num), op->num_srcs - 1, op->inst->name, src_op->inst->name);
      }
      if (!last_src_op || last_src_op->op_num < src_op->op_num) /* take latest store only */
        last_src_op = src_op;
    }
  }
  return last_src_op;
}

// add store dependencies from mem_hash overlapping op's access
static inline Op* map_dep_read_mem_hash(Op* op, Hash_Table* mem_hash) {
  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);

  uns orig_num_srcs = op->num_srcs;
  Op* last_src_op = map_dep_find_last_store(op, mem_hash, MEM_OOO_STORES);
  if (!last_src_op) {
    STAT_EVENT(op->proc_id, LD_NO_FORWARD);
    return NULL;
  }

  ASSERT(op->proc_id, last_src_op->op_num < op->op_num || op->off_path);
  if (MEM_OOO_STORES) {
    /* unmark all ops map_dep_find_last_store marked */
    for (uns ii = orig_num_srcs; ii < op->num_srcs; ii++) {
      ASSERT(op->proc_id, op->src_info[ii].op->marked);
      op->src_info[ii].op->marked = FALSE;
    }
  } else {
    op_sources_add(op, MEM_DATA_DEP, last_src_op, last_src_op->op_num, last_src_op->unique_num);
    STAT_EVENT(op->proc_id, FORWARDED_LD);
  }
  return last_src_op;
}

static inline void map_dep_write_mem_hash(Op* op, Hash_Table* mem_hash) {
  Mem_Map_Entry* mem_map_p;
  Addr va = op->oracle_info.va;
  Mem_Map_Traversal traversal;

  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);

  /* Iterate through each entry that was written to by the op */
  for (mem_map_entry_traversal_init(&traversal, va, op->oracle_info.mem_size);
       !mem_map_entry_traversal_done(&traversal); mem_map_entry_traversal_next(&traversal)) {
    Flag new_entry = FALSE;
    mem_map_p = (Mem_Map_Entry*)hash_table_access_create(mem_hash, MEM_MAP_KEY(traversal.entry_addr), &new_entry);

    if (new_entry) {
      mem_map_p->flag_mask = 0;
      mem_map_p->store_mask = 0;
    }

    /* Iterate through each byte written to by the op (within this entry) */
    for (mem_map_byte_traversal_init(&traversal); !mem_map_byte_traversal_done(&traversal);
         mem_map_byte_traversal_next(&traversal)) {
      DEFBIT(mem_map_p->flag_mask, traversal.byte, op->off_path);
      uns ind = MEM_MAP_BYTE_INDEX(traversal.byte, op->off_path);
      SETBIT(mem_map_p->store_mask, ind);
      mem_map_p->op[ind] = op;
    }
  }
}

static inline void map_dep_write_entry(Op* op, Map_Entry* map_entry) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, map_entry);
  ASSERT(map_data->proc_id, map_entry->op);
  map_entry->op = op;
  map_entry->op_num = op->op_num;
  map_entry->unique_num = op->unique_num;
}

static inline void map_dep_write_offpath_valid(Op* op) {
  for (uns ii = 0; ii < op->uop->num_dest_regs; ii++)
    map_data->offpath_reg_valid[op->uop->dests[ii].id] = op->off_path;
  if (op->uop->mem_type == MEM_ST)
    map_data->offpath_last_store_valid = op->off_path;
}

static inline void map_dep_read_map(Op* op, Map_Dep* map, Map_Dep* offpath_map, Hash_Table* mem_hash) {
  // read reg data dependency from src registers
  for (uns ii = 0; ii < op->uop->num_src_regs; ii++) {
    uns id = op->uop->srcs[ii].id;
    ASSERT(map_data->proc_id, id < NUM_REG_IDS);

    Flag is_offpath = offpath_map && map_data->offpath_reg_valid[id];
    Map_Entry* map_entry = is_offpath ? &offpath_map->reg[id] : &map->reg[id];
    ;
    op_sources_add(op, REG_DATA_DEP, map_entry->op, map_entry->op_num, map_entry->unique_num);
  }

  // read mem addr dependency from last store (no speculative loads)
  if (MEM_OBEY_STORE_DEP && !MEM_OOO_STORES && op->uop->mem_type) {
    Flag is_offpath = offpath_map && map_data->offpath_last_store_valid;
    Map_Entry* map_entry = is_offpath ? &offpath_map->last_store : &map->last_store;
    op_sources_add(op, MEM_ADDR_DEP, map_entry->op, map_entry->op_num, map_entry->unique_num);
  }

  // read store data dependency from store hash
  if (MEM_OBEY_STORE_DEP && op->uop->mem_type == MEM_LD)
    map_dep_read_mem_hash(op, mem_hash);
}

// write the op as the producer of its dests and store
static inline void map_dep_write_map(Op* op, Map_Dep* map, Hash_Table* mem_hash) {
  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);

  // dest registers
  for (uns ii = 0; ii < op->uop->num_dest_regs; ii++) {
    uns id = op->uop->dests[ii].id;
    ASSERT(map_data->proc_id, id < NUM_REG_IDS);
    map_dep_write_entry(op, &map->reg[id]);
  }

  // last store
  if (op->uop->mem_type == MEM_ST) {
    map_dep_write_entry(op, &map->last_store);
  }

  // store hash
  if (MEM_OBEY_STORE_DEP && op->uop->mem_type == MEM_ST)
    map_dep_write_mem_hash(op, mem_hash);
}

static void map_dep_check_lookahead(Op* op) {
  uns8 proc_id = map_data->proc_id;
  uns src_num = 0;

  ASSERT(proc_id, !op->off_path);

  // register sources
  for (uns ii = 0; ii < op->uop->num_src_regs; ii++, src_num++) {
    uns id = op->uop->srcs[ii].id;
    ASSERT(proc_id, !map_data->offpath_reg_valid[id]);
    Map_Entry* map_entry = &map_data->onpath_map.reg[id];
    ASSERT(proc_id, src_num < op->num_srcs);
    Src_Info* src_info = &op->src_info[src_num];
    ASSERT(proc_id, src_info->type == REG_DATA_DEP);
    ASSERT(proc_id, src_info->op_num == map_entry->op_num && src_info->unique_num == map_entry->unique_num);
  }

  // address dependency on the last store
  if (MEM_OBEY_STORE_DEP && !MEM_OOO_STORES && op->uop->mem_type) {
    ASSERT(proc_id, !map_data->offpath_last_store_valid);
    Map_Entry* map_entry = &map_data->onpath_map.last_store;
    ASSERT(proc_id, src_num < op->num_srcs);
    Src_Info* src_info = &op->src_info[src_num++];
    ASSERT(proc_id, src_info->type == MEM_ADDR_DEP);
    ASSERT(proc_id, src_info->op_num == map_entry->op_num && src_info->unique_num == map_entry->unique_num);
  }
}

/**************************************************************************************/
/*
 * External Dependency Tracking Functions:
 *    only track the dependencies, including set src_info and maintain the map
 *    register allocation is done at the renaming stage
 */

void map_dep_lookahead(Op* op) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);
  ASSERT(map_data->proc_id, !op->off_path);

  map_dep_read_map(op, &map_data->lookahead_map, NULL, &map_data->lookahead_mem_hash);
  map_dep_write_map(op, &map_data->lookahead_map, &map_data->lookahead_mem_hash);
}

void map_dep_fetch(Op* op) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);

  // read src_info from the producers
  if (op->off_path || !MAP_LOOKAHEAD) {
    map_dep_read_map(op, &map_data->onpath_map, &map_data->offpath_map, &map_data->oracle_mem_hash);
  } else {
    map_dep_check_lookahead(op);
  }

  // write the op for its consumers
  map_dep_write_map(op, op->off_path ? &map_data->offpath_map : &map_data->onpath_map, &map_data->oracle_mem_hash);
  map_dep_write_offpath_valid(op);
}

void map_dep_remove_store(Op* op) {
  map_dep_remove_store_from(op, &map_data->oracle_mem_hash);
  if (MAP_LOOKAHEAD && !op->off_path)
    map_dep_remove_store_from(op, &map_data->lookahead_mem_hash);
}

/**************************************************************************************/
/* External Wakeup Functions */

void map_wakeup_signal(Op* op, Dep_Type type, void (*wake_action)(Op*, Op*, uns)) {
  ASSERT(op->proc_id, !op->wake_up_signaled[type] || op->replay);

  // write back the register value for the dependent ops
  reg_file_produce(op);

  ASSERT(op->proc_id, wake_action);
  for (Wake_Up_Entry* temp = op->wake_up_head; temp; temp = temp->next) {
    Op* dep_op = temp->op;
    ASSERT(op->proc_id, dep_op);

    Counter dep_unique_num = temp->unique_num;
    if (temp->dep_type != type)
      continue;

    // if the op has been reclaimed, the wake up should be ignored
    if (dep_op->unique_num != dep_unique_num || !dep_op->op_pool_valid)
      continue;
    ASSERT(op->proc_id, op->proc_id == dep_op->proc_id);

    // source already woken
    if (!op_sources_test_not_rdy(dep_op, temp->rdy_bit))
      continue;
    ASSERT(dep_op->proc_id, op_sources_test_not_rdy(dep_op, temp->rdy_bit));

    // unset the not ready bit for this source
    op_sources_clear_not_rdy(dep_op, temp->rdy_bit);

    // call the wake action function
    wake_action(op, dep_op, temp->rdy_bit);
  }

  op->wake_up_signaled[type] = TRUE;
}

// wake dest_op based on src_op based only on src_op's exec_cycle
void map_wakeup_action(Op* src_op, Op* dep_op, uns rdy_bit) {
  ASSERT(src_op->proc_id, src_op && src_op != &invalid_op);
  ASSERT(src_op->proc_id, dep_op && dep_op != &invalid_op);
  ASSERT(src_op->proc_id, src_op->proc_id == dep_op->proc_id);
  UNUSED(rdy_bit);

  op_set_rdy_cycle(dep_op, MAX2(op_get_rdy_cycle(dep_op), op_get_wake_cycle(src_op)));
  if (op_sources_not_rdy_is_clear(dep_op))
    dep_op->state = op_get_rdy_cycle(dep_op) == cycle_count + 1 ? OS_READY : OS_WAIT_FWD;
}

void map_wakeup_list_add(Op* op, void (*wake_action)(Op*, Op*, uns)) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, op->proc_id == map_data->proc_id);

  for (uns ii = 0; ii < op->num_srcs; ii++) {
    Src_Info* src_info = &op->src_info[ii];
    Op* src_op = src_info->op;

    if ((!OBEY_REG_DEP && src_info->type == REG_DATA_DEP) || !src_op->op_pool_valid ||
        src_op->unique_num != src_info->unique_num) {
      // the source op have already retired so it is not in the machine
      src_info->op = &invalid_op;
      op_sources_clear_not_rdy(op, ii);
      continue;
    }
    ASSERT(op->proc_id, op->proc_id == src_op->proc_id);

    // grow the pool when no free entry is left
    if (map_data->free_list_head == NULL) {
      ASSERT(map_data->proc_id, map_data->active_wake_up_entries == map_data->wake_up_entries);
      map_wakeup_entries_expand();
    }

    // pop an entry off the free list
    Wake_Up_Entry* wake = map_data->free_list_head;
    map_data->active_wake_up_entries++;
    map_data->free_list_head = wake->next;

    // record op and which source bit to clear on wake up
    wake->op = op;
    wake->unique_num = op->unique_num;
    wake->dep_type = src_info->type;
    wake->rdy_bit = ii;
    wake->next = NULL;

    // add to its wake up list regardless of whether it has already produced a result or not
    if (src_op->wake_up_tail == NULL) {
      src_op->wake_up_head = wake;
      src_op->wake_up_tail = wake;
      src_op->wake_up_count = 1;
    } else {
      ASSERT(map_data->proc_id, src_op->wake_up_head);
      src_op->wake_up_tail->next = wake;
      src_op->wake_up_tail = wake;
      src_op->wake_up_count++;
    }

    // inherit src_op's pending L1 miss
    if (TRACK_L1_MISS_DEPS) {
      // An op can occupy multiple entries in the wakeup list of another op
      if (src_op->engine_info.l1_miss && !src_op->engine_info.l1_miss_satisfied)
        op->engine_info.dep_on_l1_miss = TRUE;

      if (src_op->engine_info.dep_on_l1_miss)
        op->engine_info.dep_on_l1_miss = TRUE;
    }

    // wake op directly if src_op already signaled
    if (src_op->wake_up_signaled[src_info->type]) {
      op_sources_clear_not_rdy(op, ii);
      wake_action(src_op, op, ii);
    }
  }
}

void map_wakeup_list_free(Op* op) {
  ASSERT(map_data->proc_id, op && op->proc_id == map_data->proc_id);
  if (!op->wake_up_tail) {
    return;
  }

  ASSERT(map_data->proc_id, op->wake_up_head);
  op->wake_up_tail->next = map_data->free_list_head;
  map_data->free_list_head = op->wake_up_head;
  map_data->active_wake_up_entries -= op->wake_up_count;

  op->wake_up_head = NULL;
  op->wake_up_tail = NULL;
}

/**************************************************************************************/
/* External Vanilla API */

void recover_map() {
  ASSERT(map_data->proc_id, map_data->proc_id == td->proc_id);

  for (uns ii = 0; ii < NUM_REG_IDS; ii++)
    map_data->offpath_reg_valid[ii] = FALSE;
  map_data->offpath_last_store_valid = FALSE;

  hash_table_scan(&map_data->oracle_mem_hash, mem_map_entry_recover, NULL);

  /* Rebuild the offpath half of map structures */
  // find the oldest offpath op
  Op** op_p = (Op**)list_start_head_traversal(&td->seq_op_list);
  while (op_p && !(*op_p)->off_path) {
    op_p = (Op**)list_next_element(&td->seq_op_list);
  }

  // rebuild the map starting with the first offpath op
  for (; op_p; op_p = (Op**)list_next_element(&td->seq_op_list)) {
    map_dep_write_map(*op_p, &map_data->offpath_map, &map_data->oracle_mem_hash);
    map_dep_write_offpath_valid(*op_p);
  }
}

Map_Data* set_map_data(Map_Data* new_map_data) {
  Map_Data* old_map_data = map_data;
  map_data = new_map_data;
  return old_map_data;
}

void init_map(uns8 proc_id) {
  ASSERT(proc_id, map_data == &td->map_data);
  memset(map_data, 0, sizeof(Map_Data));
  map_data->proc_id = proc_id;

  // Initialize the register and last store "last write" maps
  map_dep_reset(&map_data->onpath_map);
  map_dep_reset(&map_data->offpath_map);
  map_dep_reset(&map_data->lookahead_map);
  for (uns ii = 0; ii < NUM_REG_IDS; ii++)
    map_data->offpath_reg_valid[ii] = FALSE;

  // Allocate the wake_up_entry pool
  map_wakeup_entries_expand();

  // Recovery scans every bucket, so size to the ROB, which also bounds in-flight stores
  init_hash_table(&map_data->oracle_mem_hash, "oracle mem hash", NODE_TABLE_SIZE, sizeof(Mem_Map_Entry));
  init_hash_table(&map_data->lookahead_mem_hash, "lookahead mem hash", NODE_TABLE_SIZE, sizeof(Mem_Map_Entry));

  // Init the physical register renaming table
  reg_file_init();
}

void reset_map() {
  ASSERT(map_data->proc_id, map_data == &td->map_data);

  map_dep_reset(&map_data->onpath_map);
  map_dep_reset(&map_data->offpath_map);
  map_dep_reset(&map_data->lookahead_map);

  for (uns ii = 0; ii < NUM_REG_IDS; ii++)
    map_data->offpath_reg_valid[ii] = FALSE;
  map_data->offpath_last_store_valid = FALSE;
}
