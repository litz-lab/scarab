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

#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "map.h"
#include "model.h"
#include "thread.h"

#include "core.param.h"
#include "debug/debug.param.h"
#include "memory/memory.param.h"

#include "cmp_model.h"
#include "libs/hash_lib.h"
#include "statistics.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MAP, ##args)
#define DEBUGU(proc_id, args...) _DEBUGU(proc_id, DEBUG_MAP, ##args)

#define WAKE_UP_ENTRIES_INC 256 /* default 256 */
#define MEM_ADDR_SRC \
  0 /* address for memory instructions calculated off source 0 */

#define MEM_MAP_ENTRY_SIZE_LOG 3
#define MEM_MAP_ENTRY_SIZE (1 << MEM_MAP_ENTRY_SIZE_LOG)
#define MEM_MAP_BYTE_IN_ENTRY(va) ((va) & (MEM_MAP_ENTRY_SIZE - 1))
#define MEM_MAP_ENTRY_ADDR(va) ((va) & ~(Addr)(MEM_MAP_ENTRY_SIZE - 1))

#define MEM_MAP_KEY(va) ((va) >> MEM_MAP_ENTRY_SIZE_LOG)
#define MEM_MAP_BYTE_INDEX(byte, off_path) \
  ((byte) + ((off_path) ? MEM_MAP_ENTRY_SIZE : 0))

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
  uns  byte; /* Byte within entry, can be used by caller */
  uns  last_byte;
  uns  first_entry_first_byte;
  uns  last_entry_last_byte;
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
/* Static prototypes */

static inline void read_reg_map(Op*);
static inline void read_store_map(Op*);
static inline void update_map(Op*);

static inline void expand_wake_up_entries(void);
static inline void update_store_hash(Op* op);
static inline Op*  add_store_deps(Op* op);
static inline void update_map_entry(Op* op, Map_Entry* map_entry);
static inline void recover_mem_map_entry(void* hash_entry, void* arg);

/* memory map hash traversal */
static inline void mem_map_entry_traversal_init(Mem_Map_Traversal* traversal,
                                                Addr va, uns size);
static inline Flag mem_map_entry_traversal_done(Mem_Map_Traversal* traversal);
static inline void mem_map_entry_traversal_next(Mem_Map_Traversal* traversal);
static inline void mem_map_byte_traversal_init(Mem_Map_Traversal* traversal);
static inline Flag mem_map_byte_traversal_done(Mem_Map_Traversal* traversal);
static inline void mem_map_byte_traversal_next(Mem_Map_Traversal* traversal);

/* reg consume track */
static inline void  reg_dep_track_table_init(Reg_Dep_Track_Table**, uns);
static inline Flag  reg_dep_track_table_predict(Reg_Dep_Track_Table*, Op*);
static inline void  reg_dep_track_table_read(Reg_Dep_Track_Table*, Op*, uns);
static inline void  reg_dep_track_table_write(Reg_Dep_Track_Table*, Op*, uns);
static inline void  reg_dep_track_table_release(Reg_Dep_Track_Table*, uns);

/* physical register file */
static inline void reg_file_init(void);
static inline void reg_file_process_renaming(Op*);
static inline void reg_file_rebuild_map(void);

/**************************************************************************************/
/* set_map_data: */

Map_Data* set_map_data(Map_Data* new_map_data) {
  Map_Data* old_map_data = map_data;
  map_data               = new_map_data;
  return old_map_data;
}


/**************************************************************************************/
/* init_map: */

void init_map(uns8 proc_id) {
  uns ii;

  ASSERT(proc_id, map_data == &td->map_data);
  memset(map_data, 0, sizeof(Map_Data));
  map_data->proc_id = proc_id;

  /* Initialize the register "last write" map */
  for(ii = 0; ii < NUM_REG_IDS * 2; ii++) {
    map_data->reg_map[ii].op     = &invalid_op;
    map_data->reg_map[ii].op_num = 0;
  }
  for(ii = 0; ii < NUM_REG_IDS; ii++)
    map_data->map_flags[ii] = FALSE;

  map_data->last_store[0].op     = &invalid_op;
  map_data->last_store[0].op_num = 0;
  map_data->last_store[1].op     = &invalid_op;
  map_data->last_store[1].op_num = 0;

  /* Allocate the wake_up_entry pool. */
  expand_wake_up_entries();

  /* Initialize the memory dependence hash table. The number of
     buckets matters since we scan all entries (and all buckets) on
     branch misprediction recovery, so we want to keep the number of
     buckets large enough to avoid collisions but small enough to
     keep the scan fast. Since the number of entries is roughly at
     most the number of in-flight stores, we set the number of
     buckets to the size of instruction window. */
  init_hash_table(&map_data->oracle_mem_hash, "oracle mem dependence map",
                  NODE_TABLE_SIZE, sizeof(Mem_Map_Entry));

  /* Init Consume Reg Map and Table */
  map_data->track_table = NULL;
  reg_dep_track_table_init(&map_data->track_table, NUM_REG_IDS * 2);

  /* Init Physical Register File */
  map_data->reg_file = NULL;
  reg_file_init();
}


/**************************************************************************************/
/* recover_map: quick recover back to on path state */

void recover_map() {
  uns ii;
  DEBUG(map_data->proc_id, "Recovering register map\n");
  for(ii = 0; ii < NUM_REG_IDS; ii++)
    map_data->map_flags[ii] = FALSE;
  map_data->last_store_flag = FALSE;
  hash_table_scan(&map_data->oracle_mem_hash, recover_mem_map_entry, NULL);
  rebuild_offpath_map();

  reg_file_rebuild_map();
}

/**************************************************************************************/
/* recover_mem_map_entry: */

void recover_mem_map_entry(void* hash_entry, void* arg) {
  Mem_Map_Entry* entry = (Mem_Map_Entry*)hash_entry;
  entry->flag_mask     = 0;
}

/**************************************************************************************/
/* rebuild_offpath_map: rebuild the offpath half of map structures
   using the sequential op list from a Thread. Make sure you recover
   the seq_op_list first */

void rebuild_offpath_map() {
  DEBUGU(map_data->proc_id, "Rebuilding map\n");

  ASSERT(map_data->proc_id, map_data->proc_id == td->proc_id);

  /* First find the oldest offpath op */
  Op** op_p = (Op**)list_start_head_traversal(&td->seq_op_list);
  while(op_p && !(*op_p)->off_path) {
    op_p = (Op**)list_next_element(&td->seq_op_list);
  }

  /* rebuild the map starting with the first offpath op */
  for(; op_p; op_p = (Op**)list_next_element(&td->seq_op_list)) {
    update_map(*op_p);
    if((*op_p)->table_info->mem_type == MEM_ST) {
      update_store_hash(*op_p);
    }
  }
}


/**************************************************************************************/
/* expand_wake_up_pool: */

static inline void expand_wake_up_entries() {
  Wake_Up_Entry* new_pool = (Wake_Up_Entry*)calloc(WAKE_UP_ENTRIES_INC,
                                                   sizeof(Wake_Up_Entry));
  uns            ii;

  DEBUGU(map_data->proc_id, "Expanding wake up pool to size %d\n",
         (map_data->wake_up_entries + WAKE_UP_ENTRIES_INC));
  for(ii = 0; ii < WAKE_UP_ENTRIES_INC - 1; ii++)
    new_pool[ii].next = &new_pool[ii + 1];
  new_pool[ii].next        = map_data->free_list_head;
  map_data->free_list_head = &new_pool[0];
  map_data->wake_up_entries += WAKE_UP_ENTRIES_INC;
  ASSERT(map_data->proc_id,
         map_data->wake_up_entries <= WAKE_UP_ENTRIES_INC * 128);
}


/**************************************************************************************/
/* map_op: involves two things: setting up the src array in op_info
   and updating the current map state based on the op's output values.
   note that this function does nothing for memory dependencies.  you
   must call map_mem_dep after oracle_exec to properly handle them. */

void map_op(Op* op) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);

  reg_file_process_renaming(op);

  read_reg_map(op);   /* set reg sources */
  read_store_map(op); /* set addr dependency on last store */
  update_map(op);     /* update reg and last store maps */
}

/**************************************************************************************/
/* read_reg_map: read and set srcs based on registers */

static inline void read_reg_map(Op* op) {
  uns ii;

  if (REG_FILE_PHY_ENABLE)
    return;

  for(ii = 0; ii < op->table_info->num_src_regs; ii++) {
    uns        id        = op->inst_info->srcs[ii].id;
    uns        ind       = id << 1 | map_data->map_flags[id];
    Map_Entry* map_entry = &map_data->reg_map[ind];

    DEBUG(map_data->proc_id,
          "Reading map  op_num:%s  off_path:%d  id:%d  flag:%d  ind:%d\n",
          unsstr64(op->op_num), op->off_path, id, map_data->map_flags[id], ind);

    reg_dep_track_table_read(map_data->track_table, op, ind);

    add_src_from_map_entry(op, map_entry, REG_DATA_DEP);
    /* address predictor is called if op is a load & this is first mem op reg
     * read for this reg instance */
  }
}


/**************************************************************************************/
/* read_store_map: used to make mem ops dependent on the last store
   (no speculative loads) */

static inline void read_store_map(Op* op) {
  if(!MEM_OBEY_STORE_DEP || MEM_OOO_STORES)
    return;

  if(op->table_info->mem_type) {
    uns        ind       = map_data->last_store_flag;
    Map_Entry* map_entry = &map_data->last_store[ind];

    DEBUG(map_data->proc_id,
          "Reading store map  op_num:%s  off_path:%d  flag:%d  ind:%d\n",
          unsstr64(op->op_num), op->off_path, map_data->last_store_flag, ind);
    add_src_from_map_entry(op, map_entry, MEM_ADDR_DEP);
  }
}


/**************************************************************************************/
/* update_map: write the register map and the last store map if necessary */

static inline void update_map(Op* op) {
  int ii;

  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);
  /* update the register map if the op produces a value */
  for(ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    uns        id        = op->inst_info->dests[ii].id;
    uns        ind       = id << 1 | op->off_path;
    Map_Entry* map_entry = &map_data->reg_map[ind];

    DEBUG(map_data->proc_id,
          "Writing map  op_num:%s  off_path:%d  id:%d  flag:%d  ind:%d\n",
          unsstr64(op->op_num), op->off_path, id, map_data->map_flags[id], ind);

    map_entry->op           = op;
    map_entry->op_num       = op->op_num;
    map_entry->unique_num   = op->unique_num;
    map_data->map_flags[id] = op->off_path;

    reg_dep_track_table_release(map_data->track_table, ind);
    reg_dep_track_table_write(map_data->track_table, op, ind);
  }

  /* update the map if the op is a store */
  if(op->table_info->mem_type == MEM_ST) {
    uns        ind       = op->off_path;
    Map_Entry* map_entry = &map_data->last_store[ind];

    DEBUG(map_data->proc_id,
          "Writing store map  op_num:%s  off_path:%d  flag:%d  ind:%d\n",
          unsstr64(op->op_num), op->off_path, map_data->last_store_flag, ind);
    map_entry->op             = op;
    map_entry->op_num         = op->op_num;
    map_entry->unique_num     = op->unique_num;
    map_data->last_store_flag = op->off_path;
  }
}


/**************************************************************************************/
/* update_map_entry */

inline void update_map_entry(Op* op, Map_Entry* map_entry) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, map_entry);
  ASSERT(map_data->proc_id, map_entry->op);
  map_entry->op         = op;
  map_entry->op_num     = op->op_num;
  map_entry->unique_num = op->unique_num;
}


/**************************************************************************************/
/* map_mem_dep */

void map_mem_dep(Op* op) {
  if(!MEM_OBEY_STORE_DEP)
    return;
  if(op->table_info->mem_type == MEM_ST)
    update_store_hash(op);
  if(op->table_info->mem_type == MEM_LD)
    add_store_deps(op);
}

/**************************************************************************************/
/* map_mem_*_traversal_*: these functions traverse the memory map
   entries and bytes within those entries given an access with an
   address and size. */

static inline void mem_map_entry_traversal_init(Mem_Map_Traversal* traversal,
                                                Addr va, uns size) {
  Addr last_va = ADDR_PLUS_OFFSET(va, size - 1);  // last byte in access
  traversal->first_entry_addr       = MEM_MAP_ENTRY_ADDR(va);
  traversal->last_entry_addr        = MEM_MAP_ENTRY_ADDR(last_va);
  traversal->entry_addr             = traversal->first_entry_addr;
  traversal->first_entry_first_byte = MEM_MAP_BYTE_IN_ENTRY(va);
  traversal->last_entry_last_byte   = MEM_MAP_BYTE_IN_ENTRY(last_va);

  if(0 == size) {
    // special case - if the size is 0, we shouldn't do a traversal at all, so
    // force the traversal to be "done"
    traversal->entry_addr = ADDR_PLUS_OFFSET(traversal->last_entry_addr,
                                             MEM_MAP_ENTRY_SIZE);
    ASSERT(get_proc_id_from_cmp_addr(va),
           mem_map_entry_traversal_done(traversal));
  }
}

static inline Flag mem_map_entry_traversal_done(Mem_Map_Traversal* traversal) {
  return traversal->entry_addr ==
         ADDR_PLUS_OFFSET(traversal->last_entry_addr, MEM_MAP_ENTRY_SIZE);
}

static inline void mem_map_entry_traversal_next(Mem_Map_Traversal* traversal) {
  traversal->entry_addr = ADDR_PLUS_OFFSET(traversal->entry_addr,
                                           MEM_MAP_ENTRY_SIZE);
}

static inline void mem_map_byte_traversal_init(Mem_Map_Traversal* traversal) {
  traversal->byte = traversal->entry_addr == traversal->first_entry_addr ?
                      traversal->first_entry_first_byte :
                      0;
  traversal->last_byte = traversal->entry_addr == traversal->last_entry_addr ?
                           traversal->last_entry_last_byte :
                           MEM_MAP_ENTRY_SIZE - 1;
  ASSERT(0, traversal->byte <= traversal->last_byte);
}

static inline Flag mem_map_byte_traversal_done(Mem_Map_Traversal* traversal) {
  return traversal->byte > traversal->last_byte;
}

static inline void mem_map_byte_traversal_next(Mem_Map_Traversal* traversal) {
  traversal->byte++;
}

/**************************************************************************************/
/* delete_store_hash_entry */

void delete_store_hash_entry(Op* op) {
  Addr              va = op->oracle_info.va;
  Mem_Map_Traversal traversal;

  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);

  /* Iterate through each entry that was written to by the op */
  for(mem_map_entry_traversal_init(&traversal, va, op->oracle_info.mem_size);
      !mem_map_entry_traversal_done(&traversal);
      mem_map_entry_traversal_next(&traversal)) {
    Mem_Map_Entry* mem_map_p = (Mem_Map_Entry*)hash_table_access(
      &map_data->oracle_mem_hash, MEM_MAP_KEY(traversal.entry_addr));

    if(!mem_map_p)
      continue;

    /* Iterate through each byte written to by the op (within this entry) */
    for(mem_map_byte_traversal_init(&traversal);
        !mem_map_byte_traversal_done(&traversal);
        mem_map_byte_traversal_next(&traversal)) {
      uns ind = MEM_MAP_BYTE_INDEX(traversal.byte, op->off_path);
      if(TESTBIT(mem_map_p->store_mask, ind) && mem_map_p->op[ind] == op) {
        CLRBIT(mem_map_p->store_mask, ind);
      }
    }
    if(!mem_map_p->store_mask) {
      hash_table_access_delete(&map_data->oracle_mem_hash,
                               MEM_MAP_KEY(traversal.entry_addr));
    }
  }
}

/**************************************************************************************/
/* add_store_deps: */

static inline Op* add_store_deps(Op* op) {
  Addr              va            = op->oracle_info.va;
  Op*               last_src_op   = NULL;
  uns               orig_num_srcs = op->oracle_info.num_srcs;
  Mem_Map_Traversal traversal;

  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);

  /* Iterate through each entry that is read by the op */
  for(mem_map_entry_traversal_init(&traversal, va, op->oracle_info.mem_size);
      !mem_map_entry_traversal_done(&traversal);
      mem_map_entry_traversal_next(&traversal)) {
    Mem_Map_Entry* mem_map_p = (Mem_Map_Entry*)hash_table_access(
      &map_data->oracle_mem_hash, MEM_MAP_KEY(traversal.entry_addr));

    if(!mem_map_p)
      continue;

    /* Iterate through each byte read by the op (within this entry) */
    for(mem_map_byte_traversal_init(&traversal);
        !mem_map_byte_traversal_done(&traversal);
        mem_map_byte_traversal_next(&traversal)) {
      uns ind = MEM_MAP_BYTE_INDEX(
        traversal.byte, TESTBIT(mem_map_p->flag_mask, traversal.byte));
      if(!TESTBIT(mem_map_p->store_mask, ind))
        continue; /* ensure mem_map_p info is valid */
      Op* src_op = mem_map_p->op[ind];
      ASSERTM(op->proc_id,
              BYTE_OVERLAP(src_op->oracle_info.va, src_op->oracle_info.mem_size,
                           va, op->oracle_info.mem_size),
              "%d@0x%08x and %d@0x%08x\n", src_op->oracle_info.mem_size,
              (uns32)src_op->oracle_info.va, op->oracle_info.mem_size,
              (uns32)va);
      if(MEM_OOO_STORES && !src_op->marked) {
        add_src_from_op(op, src_op, MEM_DATA_DEP);
        src_op->marked = TRUE;  // mark op to avoid adding duplicate sources
        STAT_EVENT(op->proc_id, FORWARDED_LD);
      }
      if(!last_src_op ||
         last_src_op->op_num <
           src_op->op_num) { /* take latest store dependency only */
        last_src_op = src_op;
      }
    }
  }

  if(!last_src_op) {
    STAT_EVENT(op->proc_id, LD_NO_FORWARD);
    return NULL; /* No dependency found */
  }

  ASSERT(op->proc_id, last_src_op->op_num < op->op_num || op->off_path);
  if(MEM_OOO_STORES) {
    /* unmark all ops we marked earlier */
    for(uns ii = orig_num_srcs; ii < op->oracle_info.num_srcs; ii++) {
      ASSERT(op->proc_id, op->oracle_info.src_info[ii].op->marked);
      op->oracle_info.src_info[ii].op->marked = FALSE;
    }
  } else {
    add_src_from_op(op, last_src_op, MEM_DATA_DEP);
    STAT_EVENT(op->proc_id, FORWARDED_LD);
  }
  return last_src_op;
}


/**************************************************************************************/
/* update_store_hash: */

static inline void update_store_hash(Op* op) {
  Mem_Map_Entry*    mem_map_p;
  Addr              va = op->oracle_info.va;
  Mem_Map_Traversal traversal;

  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);

  /* Iterate through each entry that was written to by the op */
  for(mem_map_entry_traversal_init(&traversal, va, op->oracle_info.mem_size);
      !mem_map_entry_traversal_done(&traversal);
      mem_map_entry_traversal_next(&traversal)) {
    Flag new_entry = FALSE;
    mem_map_p      = (Mem_Map_Entry*)hash_table_access_create(
      &map_data->oracle_mem_hash, MEM_MAP_KEY(traversal.entry_addr),
      &new_entry);

    if(new_entry) {
      mem_map_p->flag_mask  = 0;
      mem_map_p->store_mask = 0;
    }

    /* Iterate through each byte written to by the op (within this entry) */
    for(mem_map_byte_traversal_init(&traversal);
        !mem_map_byte_traversal_done(&traversal);
        mem_map_byte_traversal_next(&traversal)) {
      DEFBIT(mem_map_p->flag_mask, traversal.byte, op->off_path);
      uns ind = MEM_MAP_BYTE_INDEX(traversal.byte, op->off_path);
      SETBIT(mem_map_p->store_mask, ind);
      mem_map_p->op[ind] = op;
    }
  }
}

/**************************************************************************************/
/* wake_up_ops: */

void wake_up_ops(Op* op, Dep_Type type, void (*wake_action)(Op*, Op*, uns8)) {
  Wake_Up_Entry* temp;


  _DEBUG(op->proc_id, DEBUG_REPLAY,
         "Waking up ops from src_op:%s unique:%s type:%s\n",
         unsstr64(op->op_num), unsstr64(op->unique_num), dep_type_names[type]);
  ASSERTM(op->proc_id, !op->wake_up_signaled[type] || op->replay,
          "op_num:%s op:%s off:%d\n", unsstr64(op->op_num), disasm_op(op, TRUE),
          op->off_path);

  ASSERT(op->proc_id, wake_action);
  for(temp = op->wake_up_head; temp; temp = temp->next) {
    Op*     dep_op         = temp->op;
    Counter dep_unique_num = temp->unique_num;

    ASSERT(op->proc_id, dep_op);

    if(temp->dep_type != type)
      continue;
    /* if the stored unique num is not the same as the op pool entry, the op has
           been reclaimed and the wake up should be ignored */
    if(dep_op->unique_num == dep_unique_num && dep_op->op_pool_valid) {
      ASSERTM(op->proc_id, op->proc_id == dep_op->proc_id,
              "dep_op proc_id: %u, valid: %u\n", dep_op->proc_id,
              dep_op->op_pool_valid);
      if(test_not_rdy_bit(dep_op, temp->rdy_bit)) {
        DEBUG(dep_op->proc_id, "Waking up  op_num:%s\n",
              unsstr64(dep_op->op_num));

        ASSERTM(dep_op->proc_id, test_not_rdy_bit(dep_op, temp->rdy_bit),
                "dep_op_num:%s  not_rdy_vector:%x\n", unsstr64(dep_op->op_num),
                dep_op->srcs_not_rdy_vector);

        /* unset the not ready bit for this source */
        clear_not_rdy_bit(dep_op, temp->rdy_bit);

        /* call the wake action function */
        wake_action(op, dep_op, temp->rdy_bit);
      }
    }
  }
  op->wake_up_signaled[type] = TRUE;
}

/**************************************************************************************/
/* add to wake up lists */

void add_to_wake_up_lists(Op* op, Op_Info* op_info,
                          void (*wake_action)(Op*, Op*, uns8)) {
  uns  ii;
  Flag dep_on_in_window_store = FALSE;
  UNUSED(dep_on_in_window_store);

  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, op_info);
  ASSERT(map_data->proc_id, op->proc_id == map_data->proc_id);

  for(ii = 0; ii < op_info->num_srcs; ii++) {
    Src_Info* src_info = &op_info->src_info[ii];
    Op*       src_op   = src_info->op;

    if((OBEY_REG_DEP || src_info->type != REG_DATA_DEP) &&
       src_op->op_pool_valid &&
       src_op->unique_num ==
         src_info->unique_num) {  // CMP proc_id comparison here because it
                                  // happens that an op object can be reused
                                  // with same unique number but different
                                  // proc_id -> One global unique num and unique
                                  // num per core separately.


      /* make sure the source op is still in the machine */
      /* add to the src op's wake up list regardless of whether
             it has already produced a result or not */
      Wake_Up_Entry* wake;

      ASSERTM(
        op->proc_id, op->proc_id == src_op->proc_id,
        "op num: %llu fetch: %llu, src_op num: %llu unique: %llu fetch: %llu\n",
        op->op_num, op->fetch_cycle, src_op->op_num, src_op->unique_num,
        src_op->fetch_cycle);

      if(map_data->free_list_head == NULL) {
        ASSERT(map_data->proc_id,
               map_data->active_wake_up_entries == map_data->wake_up_entries);
        expand_wake_up_entries();
      }

      if(src_info->type == MEM_DATA_DEP)
        dep_on_in_window_store = TRUE;

      wake = map_data->free_list_head;
      map_data->active_wake_up_entries++;
      map_data->free_list_head = wake->next;

      wake->op         = op;
      wake->unique_num = op->unique_num;
      wake->dep_type   = src_info->type;
      wake->rdy_bit    = ii;
      wake->next       = NULL;

      if(src_op->wake_up_tail == NULL) {
        src_op->wake_up_head  = wake;
        src_op->wake_up_tail  = wake;
        src_op->wake_up_count = 1;
      } else {
        ASSERT(map_data->proc_id, src_op->wake_up_head);
        src_op->wake_up_tail->next = wake;
        src_op->wake_up_tail       = wake;
        src_op->wake_up_count++;
      }

      if(TRACK_L1_MISS_DEPS) {
        // An op can occupy multiple entries in the wakeup list of another op
        if(src_op->engine_info.l1_miss &&
           !src_op->engine_info.l1_miss_satisfied)
          op->engine_info.dep_on_l1_miss = TRUE;

        if(src_op->engine_info.dep_on_l1_miss)
          op->engine_info.dep_on_l1_miss = TRUE;
      }

      if(src_op->wake_up_signaled[src_info->type]) {
        clear_not_rdy_bit(op, ii);
        wake_action(src_op, op, ii);
      }

      DEBUG(op->proc_id,
            "Added to wake up list  op_num:%s  src_op_num:%s type:%s\n",
            unsstr64(op->op_num), unsstr64(src_op->op_num),
            dep_type_names[src_info->type]);
    } else {
      /* the src op must have retired already  */
      src_info->op = &invalid_op;
      clear_not_rdy_bit(op, ii);
    }
  }
}


/**************************************************************************************/
/* free_wake_up_list: */

void free_wake_up_list(Op* op) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, op->proc_id == map_data->proc_id);

  if(op->wake_up_tail) {
    ASSERT(map_data->proc_id, op->wake_up_head);
    DEBUG(map_data->proc_id, "Freeing wake up list for op_num:%s\n",
          unsstr64(op->op_num));
    op->wake_up_tail->next   = map_data->free_list_head;
    map_data->free_list_head = op->wake_up_head;
    map_data->active_wake_up_entries -= op->wake_up_count;
    ASSERT(map_data->proc_id, map_data->active_wake_up_entries >= 0);
    op->wake_up_head = NULL;
    op->wake_up_tail = NULL;
  } else {
    DEBUG(map_data->proc_id, "No wake up list for op_num:%s\n",
          unsstr64(op->op_num));
  }
}


/**************************************************************************************/
/* add_src_from_op: . */

void add_src_from_op(Op* op, Op* src_op, Dep_Type type) {
  uns       src_num = op->oracle_info.num_srcs++;
  Src_Info* info    = &op->oracle_info.src_info[src_num];

  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, src_op);
  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);
  ASSERT(op->proc_id, op->proc_id == src_op->proc_id);
  ASSERT(op->proc_id, type < NUM_DEP_TYPES);
  ASSERTM(op->proc_id, src_num < MAX_DEPS, "src_num: %i\n", src_num);
  ASSERTM(op->proc_id, src_op->op_num < op->op_num, "op:%s  src_op:%s\n",
          unsstr64(op->op_num), unsstr64(src_op->op_num));

  info->type       = type;
  info->op         = src_op;
  info->op_num     = src_op->op_num;
  info->unique_num = src_op->unique_num;

  /* for memory dependencies, derived_from_prog_input incremented in track_addr
   */
  set_not_rdy_bit(op, src_num);
  if(type == MEM_DATA_DEP) {
    ASSERT(op->proc_id, src_op->table_info->mem_type == MEM_ST &&
                          op->table_info->mem_type == MEM_LD);
  }
  DEBUG(map_data->proc_id, "Added dep op_num:%s  src_op_num:%s  src_num:%d\n",
        unsstr64(op->op_num), unsstr64(src_op->op_num), src_num);
}


/**************************************************************************************/
/* add_src_from_map_entry: set the src_info array */

void add_src_from_map_entry(Op* op, Map_Entry* map_entry, Dep_Type type) {
  uns       src_num = op->oracle_info.num_srcs++;
  Src_Info* info    = &op->oracle_info.src_info[src_num];

  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, map_data->proc_id == op->proc_id);
  ASSERT(map_data->proc_id, map_entry);
  ASSERTM(map_data->proc_id, map_entry->op,
          "sop_off_path: %u, op: %p, op_num: %llu, unique_num: %llu\n",
          op->off_path, map_entry->op, map_entry->op_num,
          map_entry->unique_num);
  ASSERT(map_data->proc_id, type < NUM_DEP_TYPES);
  ASSERTM(map_data->proc_id, src_num < MAX_DEPS,
          "op_num: %llu, op_type %u, src_num: %u\n", op->op_num,
          op->table_info->op_type, src_num);
  ASSERTM(map_data->proc_id, map_entry->op_num < op->op_num,
          "op:%s  src_op:%s\n", unsstr64(op->op_num),
          unsstr64(map_entry->op->op_num));

  info->type       = type;
  info->op         = map_entry->op;
  info->op_num     = map_entry->op_num;
  info->unique_num = map_entry->unique_num;

  /* always start with the not ready bit set */
  set_not_rdy_bit(op, src_num);
  DEBUG(map_data->proc_id, "Added dep  op_num:%s  src_op_num:%s  src_num:%d\n",
        unsstr64(op->op_num), unsstr64(map_entry->op_num), src_num);
}


/**************************************************************************************/
/* clear_not_rdy_bit: */

void clear_not_rdy_bit(Op* op, uns bit) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, bit < op->oracle_info.num_srcs);
  DEBUG(map_data->proc_id, "Clearing not rdy bit  op_num:%s  bit:%d\n",
        unsstr64(op->op_num), bit);
  op->srcs_not_rdy_vector &= ~(0x1 << bit);
}


/**************************************************************************************/
/* set_not_rdy_bit: */

void set_not_rdy_bit(Op* op, uns bit) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, bit < op->oracle_info.num_srcs);
  /*  this message gets annoying
      DEBUG("Setting not rdy bit  op_num:%s  bit:%d\n", unsstr64(op->op_num),
     bit);
  */
  op->srcs_not_rdy_vector |= (0x1 << bit);
}


/**************************************************************************************/
/* test_not_rdy_bit: */

Flag test_not_rdy_bit(Op* op, uns bit) {
  ASSERT(map_data->proc_id, op);
  ASSERT(map_data->proc_id, bit < op->oracle_info.num_srcs);
  return (op->srcs_not_rdy_vector & (0x1 << bit)) > 0;
}


/**************************************************************************************/
/* simple_wake: wake dest_op based on src_op based only on src_op's exec_cycle
 */

void simple_wake(Op* src_op, Op* dep_op, uns8 rdy_bit) {
  ASSERT(src_op->proc_id, src_op->proc_id == dep_op->proc_id);
  ASSERT(src_op->proc_id, src_op && src_op != &invalid_op);
  ASSERT(src_op->proc_id, dep_op && dep_op != &invalid_op);
  dep_op->rdy_cycle = MAX2(dep_op->rdy_cycle, src_op->wake_cycle);
  if(dep_op->srcs_not_rdy_vector == 0)
    dep_op->state = dep_op->rdy_cycle == cycle_count + 1 ? OS_READY :
                                                           OS_WAIT_FWD;
}

/**************************************************************************************/
/* reset_map: */

void reset_map() {
  uns ii;

  ASSERT(map_data->proc_id, map_data == &td->map_data);

  /* initialize the register "last write" map */
  for(ii = 0; ii < NUM_REG_IDS * 2; ii++) {
    map_data->reg_map[ii].op     = &invalid_op;
    map_data->reg_map[ii].op_num = 0;
  }
  for(ii = 0; ii < NUM_REG_IDS; ii++)
    map_data->map_flags[ii] = FALSE;

  map_data->last_store[0].op     = &invalid_op;
  map_data->last_store[0].op_num = 0;
  map_data->last_store[1].op     = &invalid_op;
  map_data->last_store[1].op_num = 0;
}

/**************************************************************************************
 * Module:      Reg_Dep_Track_Table
 * Description: 
 * --- 1. Track if a producer insturction is consumed
 * --- 2. Collect the info of unconsumed producers
***************************************************************************************/
static inline uns64               reg_dep_track_table_signiture(Reg_Dep_Track_Table*, Op*);
static inline Reg_Dep_Track_Node* reg_dep_track_table_create_node(Reg_Dep_Track_Table*, Op*);
static inline void                reg_dep_track_table_print_hash_entry(void*, void*);
static inline void                rep_dep_track_table_topo_sort(Reg_Dep_Track_Table*, Reg_Dep_Track_Node*);

static inline void reg_dep_track_table_init(Reg_Dep_Track_Table **table, uns array_size) {
  if (!REG_DEP_TRACK_ENABLE)
    return;

  (*table) = (Reg_Dep_Track_Table *)malloc(sizeof(Reg_Dep_Track_Table));
  (*table)->trakcing_array_size = array_size;
  (*table)->op_sign_array = (uns64 *)malloc(sizeof(uns64) * array_size);
  (*table)->state_array = (Reg_Dep_Track_State *)malloc(sizeof(Reg_Dep_Track_State) * array_size);
  (*table)->node_array = (Reg_Dep_Track_Node **)malloc(sizeof(Reg_Dep_Track_Node *) * array_size);

  /* init the map for tracking unconsumed producer */
  for (uns ii = 0; ii < array_size; ii++) {
    (*table)->op_sign_array[ii] = 0;
    (*table)->state_array[ii] = REG_DEP_TRACK_STATE_UNCONSUMED;
    (*table)->node_array[ii] = NULL;
  }

  /* counters for recording producer instructions */
  (*table)->num_reg_all_producer = 0;
  (*table)->num_reg_consumed = 0;
  (*table)->num_reg_unconsumed = 0;
  (*table)->num_reg_topo_unconsumed = 0;

  /* init the hash for collecting unconsumed producers and its signiture type */
  init_hash_table(&(*table)->unconsumed_hash, "reg dep track table", NODE_TABLE_SIZE, sizeof(Counter));
  (*table)->sign_key_type = REG_DEP_TRACK_SIGNITURE;

  /* init the queue for topological sort nodes of in_degree = 0 and the alloc list */
  (*table)->queue_head = NULL;
  (*table)->alloc_head = NULL;
}

/*
  --- determine if the op need to be allocated
*/
static inline Flag reg_dep_track_table_predict(Reg_Dep_Track_Table *table, Op* op) {
  if (!REG_DEP_TRACK_ENABLE)
    return FALSE;
  ASSERT(map_data->proc_id, table != NULL);

  Counter *unconsumed_num = (Counter*)hash_table_access(
    &table->unconsumed_hash,
    reg_dep_track_table_signiture(table, op)
  );

  if (!unconsumed_num)
    return FALSE;

  if (*unconsumed_num <= REG_DEP_TRACK_PREDICT_THRESHOLD)
    return FALSE;

  return TRUE;
}

/**************************************************************************************/
static inline void reg_dep_track_table_read(Reg_Dep_Track_Table *table, Op *op, uns ind) {
  if (!REG_DEP_TRACK_ENABLE)
    return;
  ASSERT(map_data->proc_id, table != NULL && ind < table->trakcing_array_size);
  ASSERT(map_data->proc_id, op != NULL);

  /* single flag mechanism */
  table->state_array[ind] = REG_DEP_TRACK_STATE_CONSUMED;

  /* topological mechanism */
  if (!REG_DEP_TRACK_ANSCESTOR_ENABLE)
    return;

  if (table->node_array[ind] == NULL)
    return;
  if (op->reg_dep_track == NULL)
    op->reg_dep_track = reg_dep_track_table_create_node(table, op);

  // point src node and increase the in degree of src node
  ASSERT(map_data->proc_id, op->reg_dep_track->out_degree < MAX_SRCS);
  table->node_array[ind]->in_degree++;
  op->reg_dep_track->src_node[op->reg_dep_track->out_degree++] = table->node_array[ind];
}

static inline void reg_dep_track_table_write(Reg_Dep_Track_Table *table, Op *op, uns ind) {
  if (!REG_DEP_TRACK_ENABLE)
    return;
  ASSERT(map_data->proc_id, table != NULL && ind < table->trakcing_array_size);
  ASSERT(map_data->proc_id, table->node_array[ind] == NULL);
  ASSERT(map_data->proc_id, table->op_sign_array[ind] == 0);

  table->op_sign_array[ind] = reg_dep_track_table_signiture(table, op);

  /* single flag mechanism */
  table->state_array[ind] = REG_DEP_TRACK_STATE_UNCONSUMED;

  /* topological mechanism */
  if (!REG_DEP_TRACK_ANSCESTOR_ENABLE)
    return;

  if (op->reg_dep_track == NULL) 
    op->reg_dep_track = reg_dep_track_table_create_node(table, op);
  table->node_array[ind] = op->reg_dep_track;
}

/* stat update */
static inline void reg_dep_track_table_release(Reg_Dep_Track_Table *table, uns ind) {
  if (!REG_DEP_TRACK_ENABLE)
    return;
  ASSERT(map_data->proc_id, table != NULL && ind < table->trakcing_array_size);

  /* single flag mechanism */
  table->num_reg_all_producer++;
  if (table->state_array[ind] == REG_DEP_TRACK_STATE_CONSUMED)
    table->num_reg_consumed++;
  if (table->state_array[ind] == REG_DEP_TRACK_STATE_UNCONSUMED) {
    Flag new_entry = FALSE;
    Counter *unconsumed_num = (Counter*)hash_table_access_create(
      &table->unconsumed_hash, table->op_sign_array[ind], &new_entry
    );
    (*unconsumed_num)++;
    table->num_reg_unconsumed++;
  }
  table->op_sign_array[ind] = 0;

  /* topological mechanism */
  if (!REG_DEP_TRACK_ANSCESTOR_ENABLE)
    return;

  Reg_Dep_Track_Node *track_node = table->node_array[ind];
  if (track_node == NULL)
    return;
  if (track_node->in_degree == 0) {
    track_node->next_in_queue = table->queue_head;
    table->queue_head = track_node;
  }

  while (table->queue_head != NULL) {
    track_node = table->queue_head;
    track_node->next_in_queue = NULL;
    table->queue_head = table->queue_head->next_in_queue;
    rep_dep_track_table_topo_sort(table, track_node);
  }
  table->node_array[ind] = NULL;
}

/**************************************************************************************/
/* Internal Call */

/* return the corresponding signiture */
static inline uns64 reg_dep_track_table_signiture(Reg_Dep_Track_Table *table, Op* op) {
  if (!REG_DEP_TRACK_ENABLE)
    return FALSE;

  ASSERT(map_data->proc_id, table != NULL);

  uns64 sign = 0;

  switch (table->sign_key_type) {
    case REG_DEP_TRACK_SIGH_PC:
      sign = op->oracle_info.inst_info->addr;
      break;

    case REG_DEP_TRACK_SIGH_MEM:
      sign = op->oracle_info.va;
      break;

    default:
      break;
  }

  return sign;
}

/* create tracking node for op */
static inline Reg_Dep_Track_Node *reg_dep_track_table_create_node(Reg_Dep_Track_Table *table, Op *op) {
  Reg_Dep_Track_Node *track_node = (Reg_Dep_Track_Node *)malloc(sizeof(Reg_Dep_Track_Node));

  track_node->in_degree = 0;
  track_node->out_degree = 0;
  for (uns ii = 0; ii < MAX_SRCS; ii++)
    track_node->src_node[ii] = NULL;

  track_node->next_in_queue = NULL;
  track_node->next_alloc = table->alloc_head;
  table->alloc_head = track_node;

  return track_node;
}

/* print the debug info */
static inline void reg_dep_track_table_print_hash_entry(void* hash_entry, void* arg) {
  Counter *unconsumed_num = (Counter*)hash_entry;
  printf("%lld, ", *unconsumed_num);
}

/* topological sort */
static inline void rep_dep_track_table_topo_sort(Reg_Dep_Track_Table *table, Reg_Dep_Track_Node *track_node) {
  ASSERT(map_data->proc_id, track_node->in_degree == 0);
  ASSERT(map_data->proc_id, track_node->next_in_queue == NULL);
  table->num_reg_topo_unconsumed++;

  for (uns ii = 0; ii < track_node->out_degree; ii++) {
    ASSERT(map_data->proc_id, track_node->src_node[ii] != NULL);

    track_node->src_node[ii]->in_degree--;
    if (track_node->src_node[ii]->in_degree == 0) {
      track_node->src_node[ii]->next_in_queue = table->queue_head;
      table->queue_head = track_node->src_node[ii];
    }
  }
}

/**************************************************************************************/
/* external functions of the unconsumed producer table */
void reg_dep_track_table_print_debug_stat(void) {
  if (!REG_DEP_TRACK_ENABLE)
    return;

  Reg_Dep_Track_Table *table = NULL;

  if (!REG_FILE_PHY_ENABLE)
    table = map_data->track_table;
  else if (map_data->reg_file != NULL)
    table = map_data->reg_file->phy_map->phy_track_table;

  if (table == NULL)
    return;

  printf("=======================================================================\n");
  printf("\nREG MAP\n");

  printf("UNCONSUMED: %lld, CONSUMED: %lld, ALL: %lld, TOPO_UNCONSUMED: %lld\n",
    table->num_reg_unconsumed,
    table->num_reg_consumed,
    table->num_reg_all_producer,
    table->num_reg_topo_unconsumed
  );

  printf("-----------------------------------------------------------------------\n");
  hash_table_scan(&table->unconsumed_hash, reg_dep_track_table_print_hash_entry, NULL);
  printf("\n=======================================================================\n");
}

/**************************************************************************************
 * Module:      Reg_File
 * Description: Implementation of register renaming stage in hardware mechanism
***************************************************************************************/
// reg file procedure
static inline void reg_file_look_up_src(Op*);
static inline void reg_file_alloc_dest(Op*);

// reg file rebuild
static inline void reg_file_rebuild_flush(void);
static inline void reg_file_rebuild_remap(void);
static inline void reg_file_rebuild_recover(void);

// physical map init 
static inline void reg_file_phy_map_init(uns);

// physical map basic operations
static inline void reg_file_phy_map_read_entry(Op*, Reg_File_Phy_Entry*, Dep_Type);
static inline void reg_file_phy_map_write_entry(Op*, Reg_File_Phy_Entry*, uns);
static inline Reg_File_Phy_Entry *reg_file_phy_map_access_entry(uns16);
static inline Flag reg_file_phy_map_can_alloc(uns);
static inline Reg_File_Phy_Entry *reg_file_phy_map_alloc_entry(void);
static inline void reg_file_phy_map_free_entry(Reg_File_Phy_Entry*);
static inline void reg_file_phy_map_free_list_insert(Reg_File_Phy_Entry *entry);
static inline Reg_File_Phy_Entry *reg_file_phy_map_free_list_delete(void);

/*
  --- 1. init op pool for stalling
  --- 2. init physical impl of map
*/
void reg_file_init(void) {
  if (!REG_FILE_PHY_ENABLE)
    return;

  map_data->reg_file = (Reg_File *)malloc(sizeof(Reg_File));
  map_data->reg_file->stall_op = NULL;

  map_data->reg_file->phy_map = NULL;
  reg_file_phy_map_init(REG_FILE_PHY_NUM);
}

/*
  --- 1. look up register as src and fill the entry into src_info
  --- 2. alloc entry and store self info into register as dest
*/
void reg_file_process_renaming(Op *op) {
  if (!REG_FILE_PHY_ENABLE)
    return;

  reg_file_look_up_src(op);
  reg_file_alloc_dest(op);
}

/*
  --- 1. flush off-path op for error spec
  --- 2. remap phy to isa
  --- 3. recover op from snapshot
*/
void reg_file_rebuild_map(void) {
  if (!REG_FILE_PHY_ENABLE)
    return;

  if (map_data->reg_file == NULL)
    return;

  DEBUG(map_data->proc_id, "*** REBUILD ***\n");
  reg_file_rebuild_flush();
  reg_file_rebuild_remap();
  reg_file_rebuild_recover();
}

/**************************************************************************************/
/* Internal Function */

/*
  --- 1. search the entry in map
  --- 2. read info from map
*/
void reg_file_look_up_src(Op *op) {
  uns ii;
  Reg_File_Phy_Entry *entry;

  for (ii = 0; ii < op->table_info->num_src_regs; ii++) {
    uns16 id = op->inst_info->srcs[ii].id;
    entry = reg_file_phy_map_access_entry(id);
    reg_file_phy_map_read_entry(op, entry, REG_DATA_DEP);
  }
}

/*
  --- 1. check if enough register for allocation
  --- 2. fill the info into free entries
*/
void reg_file_alloc_dest(Op *op) {
  uns ii;
  Reg_File_Phy_Entry *entry;

  // check if enough reg
  if (!reg_file_phy_map_can_alloc(op->table_info->num_dest_regs)) {
    DEBUG(map_data->proc_id, "Map File Physical Renaming Stall: %lld\n", op->unique_num);
    if (map_data->reg_file->stall_op == NULL)
      map_data->reg_file->stall_op = op;
    return;
  }

  // remove stall
  if (map_data->reg_file->stall_op) {
    DEBUG(map_data->proc_id, "Map File Physical Renaming Recover: %lld\n", op->unique_num);
    map_data->reg_file->stall_op = NULL;
  }

  // alloc reg for all dests
  for (ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    entry = reg_file_phy_map_alloc_entry();
    reg_file_phy_map_write_entry(op, entry, ii);
  }
}

/*
  --- remove all off path ops
*/
void reg_file_rebuild_flush(void) {
  uns ii;
  Reg_File_Phy_Entry *entry;

  for (ii = 0; ii < map_data->reg_file->phy_map->reg_phy_size; ii++) {
    entry = &map_data->reg_file->phy_map->reg_phy_array[ii];
    if (entry->reg_state == REG_FILE_PHY_STATE_FREE)
      continue;

    if (!entry->off_path)
      continue;

    reg_file_phy_map_free_entry(entry);
  }
}

/*
  --- point the current latest phy to isa
*/
void reg_file_rebuild_remap(void) {
  uns ii;
  Reg_File_Phy_Entry *entry;

  for (ii = 0; ii < map_data->reg_file->phy_map->reg_phy_size; ii++) {
    entry = &map_data->reg_file->phy_map->reg_phy_array[ii];
    if (entry->reg_state == REG_FILE_PHY_STATE_FREE)
      continue;

    ASSERT(map_data->proc_id, !entry->off_path);

    if (entry->next_same_isa != NULL) 
      continue;

    if (map_data->reg_file->phy_map->reg_isa_map[entry->reg_isa_id] == NULL)
      map_data->reg_file->phy_map->reg_isa_map[entry->reg_isa_id] = entry;
  }
}

/*
  --- re alloc dest for current off-path op
*/
void reg_file_rebuild_recover(void) {
  uns ii;
  Reg_File_Phy_Entry *entry;

  Op** op_p = (Op**)list_start_head_traversal(&td->seq_op_list);
  while (op_p && !(*op_p)->off_path) {
    op_p = (Op**)list_next_element(&td->seq_op_list);
  }

  for (; op_p; op_p = (Op**)list_next_element(&td->seq_op_list)) {
    ASSERT(map_data->proc_id, reg_file_phy_map_can_alloc((*op_p)->table_info->num_dest_regs));
    ASSERT(map_data->proc_id, map_data->reg_file->stall_op == NULL);
    for (ii = 0; ii < (*op_p)->table_info->num_dest_regs; ii++) {
      entry = reg_file_phy_map_alloc_entry();
      reg_file_phy_map_write_entry((*op_p), entry, ii);
    }
  }
}

/**************************************************************************************/
/* Operations of Physical Map in Register File */

/*
  --- 1. init isa map
  --- 2. init physical arry and entry
  --- 3. init tracking table
*/
void reg_file_phy_map_init(uns array_size) {
  uns ii;
  Reg_File_Phy_Entry *entry;

  map_data->reg_file->phy_map = (Reg_File_Phy_Map *)malloc(sizeof(Reg_File_Phy_Map));

  /* init the isa map */
  for (ii = 0; ii < NUM_REG_IDS; ii++)
    map_data->reg_file->phy_map->reg_isa_map[ii] = NULL;

  /* init the free list */
  map_data->reg_file->phy_map->reg_free_num = 0;
  map_data->reg_file->phy_map->reg_free_list_head = NULL;

  /* init the physical register map array */
  map_data->reg_file->phy_map->reg_phy_size = array_size;
  map_data->reg_file->phy_map->reg_phy_array = (Reg_File_Phy_Entry *)malloc(sizeof(Reg_File_Phy_Entry) * array_size);

  /* init the physical register map entry */
  for (ii = 0; ii < array_size; ii++) {
    entry = &map_data->reg_file->phy_map->reg_phy_array[ii];

    entry->op = &invalid_op;
    entry->op_num = 0;
    entry->unique_num = 0;
    entry->off_path = FALSE;
    entry->reg_isa_id = REG_FILE_REG_INVALID_ID;

    entry->reg_phy_id = ii;
    entry->reg_state = REG_FILE_PHY_STATE_FREE;

    entry->prev_same_isa = NULL;
    entry->next_same_isa = NULL;

    reg_file_phy_map_free_list_insert(entry);
  }

  /* init the table for tracking unconsumed producers */
  map_data->reg_file->phy_map->phy_track_table = NULL;
  reg_dep_track_table_init(&map_data->reg_file->phy_map->phy_track_table, array_size);
}

/*
  --- 1. fill src info of op from entry
  --- 2. update consumer tracking info
*/
void reg_file_phy_map_read_entry(Op *op, Reg_File_Phy_Entry *entry, Dep_Type type) {
  if (entry == NULL)
    return;

  // increase src num
  uns       src_num = op->oracle_info.num_srcs++;
  Src_Info* info    = &op->oracle_info.src_info[src_num];

  // get info from entry
  info->type       = type;
  info->op         = entry->op;
  info->op_num     = entry->op_num;
  info->unique_num = entry->unique_num;

  // setting waking up siganl
  set_not_rdy_bit(op, src_num);

  // consume the reg dep track
  reg_dep_track_table_read(map_data->reg_file->phy_map->phy_track_table, op, entry->reg_phy_id);
}

/*
  --- 1. fill src info of entry from op
  --- 2. update isa map
*/
void reg_file_phy_map_write_entry(Op* op, Reg_File_Phy_Entry *entry, uns index) {
  ASSERT(op->proc_id, entry != NULL && entry->next_same_isa == NULL && entry->prev_same_isa == NULL);

  // set info to entry
  entry->op = op;
  entry->op_num = op->op_num;
  entry->unique_num = op->unique_num;
  entry->off_path = op->off_path;
  entry->reg_isa_id = op->inst_info->dests[index].id;
  entry->reg_state = REG_FILE_PHY_STATE_ALLOC;

  // produce the reg dep track
  reg_dep_track_table_write(map_data->reg_file->phy_map->phy_track_table, op, entry->reg_phy_id);

  // change the pointer in isa to the latest one
  Reg_File_Phy_Entry *prev_entry = map_data->reg_file->phy_map->reg_isa_map[entry->reg_isa_id];
  if (prev_entry != NULL && prev_entry->reg_phy_id != entry->reg_phy_id) {
    ASSERT(op->proc_id, prev_entry->next_same_isa == NULL);
    entry->prev_same_isa = prev_entry;
    prev_entry->next_same_isa = entry;
  }
  map_data->reg_file->phy_map->reg_isa_map[entry->reg_isa_id] = entry;

  // insert entry info to op
  ASSERT(op->proc_id, op->reg_dest_num < MAX_DESTS);
  op->reg_dest_entry[op->reg_dest_num++] = entry;
}

/*
  --- get latest entry from isa map
*/
Reg_File_Phy_Entry *reg_file_phy_map_access_entry(uns16 id) {
  return map_data->reg_file->phy_map->reg_isa_map[id];
}

/*
  --- check if enough free register num for allocation
*/
Flag reg_file_phy_map_can_alloc(uns num) {
  return map_data->reg_file->phy_map->reg_free_num >= num;
}

/*
  --- get the allocating entry from free list
*/
Reg_File_Phy_Entry *reg_file_phy_map_alloc_entry(void) {
  return reg_file_phy_map_free_list_delete();
}

/*
  --- free the entry and insert to free list
*/
void reg_file_phy_map_free_entry(Reg_File_Phy_Entry *entry) {
  ASSERT(map->proc_id, entry->reg_state != REG_FILE_PHY_STATE_FREE);

  // clear entry in isa
  Reg_File_Phy_Entry *isa_entry = map_data->reg_file->phy_map->reg_isa_map[entry->reg_isa_id];
  if (isa_entry != NULL && isa_entry->reg_phy_id == entry->reg_phy_id)
    map_data->reg_file->phy_map->reg_isa_map[entry->reg_isa_id] = NULL;

  // release reg dep track
  reg_dep_track_table_release(map_data->reg_file->phy_map->phy_track_table, entry->reg_phy_id);

  // clear entry
  entry->op = &invalid_op;
  entry->op_num = 0;
  entry->unique_num = 0;
  entry->off_path = FALSE;
  entry->reg_isa_id = REG_FILE_REG_INVALID_ID;
  entry->reg_state = REG_FILE_PHY_STATE_FREE;

  // append to free list
  reg_file_phy_map_free_list_insert(entry);

  // clear same isa pointer
  if (entry->prev_same_isa != NULL)
    entry->prev_same_isa->next_same_isa = NULL;
  if (entry->next_same_isa != NULL)
    entry->next_same_isa->prev_same_isa = NULL;
  entry->prev_same_isa = NULL;
  entry->next_same_isa = NULL;
}

/*
  --- 1. isnert the entry to list
  --- 2. increase the free num
*/
void reg_file_phy_map_free_list_insert(Reg_File_Phy_Entry *entry) {
  entry->next_free = map_data->reg_file->phy_map->reg_free_list_head;
  map_data->reg_file->phy_map->reg_free_list_head = entry;
  map_data->reg_file->phy_map->reg_free_num++;
}

/*
  --- 1. delete the entry from list
  --- 2. decrease the free num
*/
Reg_File_Phy_Entry *reg_file_phy_map_free_list_delete(void) {
  Reg_File_Phy_Entry *entry;

  if (map_data->reg_file->phy_map->reg_free_list_head == NULL) {
    entry = NULL;
  } else {
    entry = map_data->reg_file->phy_map->reg_free_list_head;
    map_data->reg_file->phy_map->reg_free_list_head = entry->next_free;
    entry->next_free = NULL;
    map_data->reg_file->phy_map->reg_free_num--;
  }

  return entry;
}

/**************************************************************************************/
/* External Calling of Register File */

/*
  Called by:
  --- icache_stage.c -> icache_issue_ops
  Procedure:
  --- check if there is stalling op
*/
Flag reg_file_check_stall(void) {
  if (!REG_FILE_PHY_ENABLE)
    return FALSE;

  if (map_data->reg_file == NULL)
    return FALSE;

  return map_data->reg_file->stall_op != NULL;
}

/*
  Called by:
  --- icache_stage.c -> update_icache_stage
  Procedure:
  --- 1. check if there is stalling op
  --- 2. if true, try to alloc the stalling op
  --- 3. check if there is stalling op
*/
Flag reg_file_remove_stall(void) {
  if (!REG_FILE_PHY_ENABLE)
    return FALSE;

  if (map_data->reg_file == NULL)
    return FALSE;

  if (map_data->reg_file->stall_op != NULL) {
    DEBUG(map_data->proc_id, "Map File Physical Renaming Try To Remove Stall: %lld\n",
      map_data->reg_file->stall_op->unique_num);
    reg_file_alloc_dest(map_data->reg_file->stall_op);
  }

  return map_data->reg_file->stall_op != NULL;
}

/*
  Called by:
  --- op_pool.c -> free_op
  Procedure:
  --- 1. mark dead for all prev entry before the committed one
  --- 2. remove dead from the oldest op
*/
void reg_file_remove_dead(Reg_File_Phy_Entry *entry) {
  if (!REG_FILE_PHY_ENABLE)
    return;

  if (entry->reg_state == REG_FILE_PHY_STATE_DEAD || entry->reg_state == REG_FILE_PHY_STATE_COMMIT)
    return;

  ASSERT(map_data->proc_id, entry != NULL &&
    entry->reg_state != REG_FILE_PHY_STATE_FREE && !entry->op->op_pool_valid);
  if (entry->next_same_isa == NULL) {
    ASSERT(map_data->proc_id, map_data->reg_file->phy_map->reg_isa_map[entry->reg_isa_id] != NULL);
    ASSERT(map_data->proc_id, map_data->reg_file->phy_map->reg_isa_map[entry->reg_isa_id]->reg_phy_id == entry->reg_phy_id);
  }

  // mark current register as commit when it is free
  entry->reg_state = REG_FILE_PHY_STATE_COMMIT;

  // mark all the op before the committed op as dead
  Reg_File_Phy_Entry *curr_entry;
  curr_entry = entry;
  while (curr_entry->prev_same_isa != NULL) {
    ASSERT(map_data->proc_id, curr_entry->reg_state != REG_FILE_PHY_STATE_FREE);
    curr_entry = curr_entry->prev_same_isa;
    curr_entry->reg_state = REG_FILE_PHY_STATE_DEAD;
  }

  // remove dead from the oldest to the latest
  Reg_File_Phy_Entry *next_entry;
  while (curr_entry != NULL && curr_entry->reg_state == REG_FILE_PHY_STATE_DEAD) {
    next_entry = curr_entry->next_same_isa;
    reg_file_phy_map_free_entry(curr_entry);
    curr_entry = next_entry;
  }
}

/*
  --- printf for debug
*/
void reg_file_print_map(int isa_id) {
  uns ii;
  Reg_File_Phy_Entry *entry;

  printf("\n-------------------------\n");
  printf("Physical Array (proc: %d)\n", map_data->proc_id);
  for (ii = 0; ii < map_data->reg_file->phy_map->reg_phy_size; ii++) {
    entry = &map_data->reg_file->phy_map->reg_phy_array[ii];

    if (isa_id != REG_FILE_REG_INVALID_ID && isa_id != entry->reg_isa_id) {
      continue;
    }

    printf("P[%d]  \t(i: %d, \ts: %d,\tn: %lld, \tu: %lld, \to: %d, \tv: %d)\n",
      entry->reg_phy_id, entry->reg_isa_id, entry->reg_state, entry->op_num,
      entry->unique_num, entry->off_path, entry->op->op_pool_valid);

    printf("Prev: ->");
    entry = entry->prev_same_isa;
    while (entry != NULL) {
      printf("%d->", entry->reg_phy_id);
      entry = entry->prev_same_isa;
    }
    printf("\n");

    entry = &map_data->reg_file->phy_map->reg_phy_array[ii];
    printf("Next: ->");
    entry = entry->next_same_isa;
    while (entry != NULL) {
      printf("%d->", entry->reg_phy_id);
      entry = entry->next_same_isa;
    }
    printf("\n");
  }

  printf("-------------------------\n");
  printf("ISA Map (proc: %d)\n", map_data->proc_id);
  for (ii = 0; ii < NUM_REG_IDS; ii++) {
    entry = map_data->reg_file->phy_map->reg_isa_map[ii];

    if (isa_id != REG_FILE_REG_INVALID_ID && isa_id != ii) {
      continue;
    }

    if (entry == NULL)
      continue;

    printf("I[%d] \t(phy: %d, \ts: %d,\tn: %lld, \tu: %lld, \to: %d, \tv: %d)\n",
      entry->reg_isa_id, entry->reg_phy_id, entry->reg_state, entry->op_num,
      entry->unique_num, entry->off_path, entry->op->op_pool_valid);
  }

  printf("-------------------------\n");
  printf("Free List (proc: %d)\n", map_data->proc_id);
  for (entry = map_data->reg_file->phy_map->reg_free_list_head; entry != NULL; entry = entry->next_free) {
    printf("F[%d], ", entry->reg_phy_id);
  }
  printf("Free Num: %d\n", map_data->reg_file->phy_map->reg_free_num);

  printf("-------------------------\n\n");
}