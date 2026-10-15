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
 * File         : map.h
 * Author       : HPS Research Group
 * Date         : 2/16/1999
 * Description  :
 ***************************************************************************************/

#ifndef __MAP_H__
#define __MAP_H__

#include "isa/isa_macros.h"
#include "libs/hash_lib.h"

#include "map_rename.h"
#include "op.h"

/**************************************************************************************/
/* Types */

typedef struct Map_Entry_struct {
  Op* op;             /* last op to write (invalid when committed) */
  Counter op_num;     /* op number of the last op to write (not cleared, only overwritten) */
  Counter unique_num; /* unique number of the last op to write (not cleared, only overwritten) */
} Map_Entry;

typedef struct Map_Dep_struct {
  // last op to write each register, read as REG_DATA_DEP
  Map_Entry reg[NUM_REG_IDS];

  // last store, read as MEM_ADDR_DEP by mem ops (only when !MEM_OOO_STORES)
  Map_Entry last_store;
} Map_Dep;

typedef struct Map_Data_struct {
  uns8 proc_id;

  /* Fetch map for ops in fetch order, on-path and off-path writers kept apart */
  Map_Dep onpath_map;
  Map_Dep offpath_map;
  Hash_Table oracle_mem_hash;

  /* Set when the latest writer is in offpath_map, cleared on recovery */
  Flag offpath_reg_valid[NUM_REG_IDS];
  Flag offpath_last_store_valid;

  /* Lookahead map for on-path ops only, mapped in program order when their FT is built */
  Map_Dep lookahead_map;
  Hash_Table lookahead_mem_hash;

  Wake_Up_Entry* free_list_head;
  uns wake_up_entries;
  uns active_wake_up_entries;

  /* register files for INT/FP with arch/physical tables */
  Reg_File* reg_file[REG_FILE_REG_TYPE_NUM];
} Map_Data;

/**************************************************************************************/
/* External Variables */

extern Map_Data* map_data;

/**************************************************************************************/
/* Prototypes */

// vanilla api
Map_Data* set_map_data(Map_Data*);
void init_map(uns8);
void recover_map(void);
void reset_map(void);

// dependency tracking
void map_op_lookahead(Op*);
void map_op(Op*);
void delete_store_hash_entry(Op*);

// wake up management
void wake_up_ops(Op*, Dep_Type, void (*)(Op*, Op*, uns));
void free_wake_up_list(Op*);
void add_to_wake_up_lists(Op*, void (*)(Op*, Op*, uns));
void simple_wake(Op*, Op*, uns);

/**************************************************************************************/

#endif /* #ifndef __MAP_H__ */
