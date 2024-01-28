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
#include "op.h"

/**************************************************************************************/
/* Unconsumed Producer Tracking */

// To be changed to a configurable para
#define REG_CONSUME_TRACKING_ENABLE FALSE
#define REG_CONSUME_PREDICT_SIGN REG_CONSUME_SIGH_PC
#define REG_CONSUME_PREDICT_THRESHOLD 5

typedef enum Map_Reg_Consume_State_enum {
  REG_CONSUME_STATE_UNCONSUMED,
  REG_CONSUME_STATE_CONSUMED,
  REG_CONSUME_STATE_VOID,
  REG_CONSUME_STATE_NUM
} Reg_Consume_State;

typedef enum Map_Reg_Consume_Signiture_enum {
  REG_CONSUME_SIGH_PC,
  REG_CONSUME_SIGH_MEM,
  REG_CONSUME_SIGH_NUM
} Reg_Consume_Signiture;

typedef struct Reg_Consume_Table_struct {
  /* track if the producer is consumed */
  Reg_Consume_State *tracking_array;
  uns trakcing_array_size;

  /* count the producer instructions */
  Counter num_reg_all_producer;
  Counter num_reg_consumed;
  Counter num_reg_unconsumed;

  /* collect the unconsumed producer instructions by signiture */
  Hash_Table            unconsumed_hash;
  Reg_Consume_Signiture unconsumed_hash_key_tpye;
} Reg_Consume_Table;

/**************************************************************************************/
/* Reg File Hardware Implementation */

// To be change to configurable val
#define REG_FILE_PHY_ENABLE FALSE
#define REG_FILE_PHY_NUM 160

const static int REG_FILE_REG_INVALID_ID = -1;

typedef struct Reg_File_Phy_Map_struct {
  /* isa map */
  Reg_File_Phy_Entry* reg_isa_map[NUM_REG_IDS];

  /* physical map */
  Reg_File_Phy_Entry *reg_phy_array;
  uns                 reg_phy_size;

  /* free list */
  Reg_File_Phy_Entry *reg_free_list_head;
  uns                 reg_free_num;

  /* unconsumed producer insturction tracking */
  Reg_Consume_Table *phy_reg_consume_table;
} Reg_File_Phy_Map;

typedef struct Reg_File_struct {
  Reg_File_Phy_Map *phy_map;
  Op *stall_op;
} Reg_File;

/**************************************************************************************/
/* Types */

typedef struct Map_Entry_struct {
  Op*     op;         /* last op to write (invalid when committed) */
  Counter op_num;     /* op number of the last op to write (not cleared, only
                         overwritten) */
  Counter unique_num; /* unique number of the last op to write (not cleared,
                         only overwritten) */
} Map_Entry;

typedef struct Map_Data_struct {
  /* store information about the last op to write each register */
  uns8      proc_id;
  Map_Entry reg_map[NUM_REG_IDS * 2];
  Flag      map_flags[NUM_REG_IDS];

  Map_Entry last_store[2];
  Flag      last_store_flag;

  Hash_Table oracle_mem_hash;

  Wake_Up_Entry* free_list_head;
  uns            wake_up_entries;
  uns            active_wake_up_entries;

  /* unconsumed producer insturction tracking */
  Reg_Consume_Table *reg_consume_table;

  /* register file for renaming based on hardware implementation */
  Reg_File *reg_file;
} Map_Data;


/**************************************************************************************/
/* External Variables */

extern Map_Data* map_data;


/**************************************************************************************/
/* Prototypes */

Map_Data* set_map_data(Map_Data*);
void      init_map(uns8);
void      recover_map(void);
void      rebuild_offpath_map(void);
void      reset_map(void);
void      map_op(Op*);
void      map_mem_dep(Op*);
void      wake_up_ops(Op*, Dep_Type, void (*)(Op*, Op*, uns8));
void      free_wake_up_list(Op*);
void      add_to_wake_up_lists(Op*, Op_Info*, void (*)(Op*, Op*, uns8));

void add_src_from_op(Op*, Op*, Dep_Type);
void add_src_from_map_entry(Op*, Map_Entry*, Dep_Type);

void simple_wake(Op*, Op*, uns8);
void delete_store_hash_entry(Op*);

void clear_not_rdy_bit(Op*, uns);
Flag test_not_rdy_bit(Op*, uns);
void set_not_rdy_bit(Op*, uns);

/* external functions of the unconsumed producer table */
void reg_consume_table_print_debug_stat(void);

/* external functions of the physical register file */
Flag reg_file_check_stall(void);
Flag reg_file_remove_stall(void);
void reg_file_remove_dead(Reg_File_Phy_Entry *);
void reg_file_print_map(int);

/**************************************************************************************/

#endif /* #ifndef __MAP_H__ */
