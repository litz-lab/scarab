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
 * File         : map_rename.h
 * Author       : Y. Zhao, Litz Lab
 * Date         : 10/2024
 * Description  : Register Renaming Allocation
 ***************************************************************************************/

#ifndef __MAP_RENAME_H__
#define __MAP_RENAME_H__

#include "isa/isa_macros.h"
#include "op.h"

/**************************************************************************************/
/* Constexpr */

typedef enum Reg_File_Type_enum {
  REG_FILE_TYPE_INFINITE,
  REG_FILE_TYPE_REALISTIC,
  REG_FILE_TYPE_NUM
} Reg_File_Type;

typedef enum Reg_Map_Entry_State_enum {
  REG_MAP_ENTRY_STATE_FREE,
  REG_MAP_ENTRY_STATE_ALLOC,
  REG_MAP_ENTRY_STATE_PRODUCED,
  REG_MAP_ENTRY_STATE_COMMIT,
  REG_MAP_ENTRY_STATE_DEAD,
  REG_MAP_ENTRY_STATE_NUM
} Reg_Map_Entry_State;

// default reg id in the reg file
const static int REG_MAP_INVALID_REG_ID = -1;

/**************************************************************************************/
/* Types */

struct Reg_Map_Entry_Ops_struct;
struct Reg_Free_List_Ops_struct;
struct Reg_Map_Ops_struct;

typedef struct Reg_Map_Entry_struct {
  // op info (the pointer of op + the deep copy of special val)
  Op       *op;
  Counter  op_num;
  Counter  unique_num;
  Flag     off_path;

  // reg id info
  int prev_reg_id;
  int curr_reg_id;
  int next_reg_id;

  // register state info
  Reg_Map_Entry_State reg_state;

  // tracking the previous register use the same architectural register id
  int last_same_arch_id;

  // tracking free register entries
  struct Reg_Map_Entry_struct *next_free;

  // register entry operation
  struct Reg_Map_Entry_Ops_struct *ops;
} Reg_Map_Entry;

typedef struct Reg_Free_List_struct {
  // stack implementation for free list
  Reg_Map_Entry *reg_free_list_head;
  uns reg_free_num;

  // free list operation
  struct Reg_Free_List_Ops_struct *ops;
} Reg_Free_List;

typedef struct Reg_Map_struct {
  /* map tag to register entries for both speculative and committed op */
  Reg_Map_Entry *entries;
  uns size;

  /* track all free registers */
  Reg_Free_List *free_list;

  /* reserve the prev table pointer for backtrack */
  struct Reg_Map_struct *prev_reg_map;

  /* register file operation */
  struct Reg_Map_Ops_struct *ops;
} Reg_Map;

typedef struct Reg_File_struct {
  /* map each architectural register id to the latest register map entry */
  Reg_Map *architectural_table;

  /* register file storing the values of writing back */
  Reg_Map *physical_reg_map;
} Reg_File;

/**************************************************************************************/
/* Operations */

typedef struct Reg_Map_Entry_Ops_struct {
  void (*clear)(Reg_Map_Entry *entry);
  void (*read)(Reg_Map_Entry *entry, Op* op);
  void (*write)(Reg_Map_Entry *entry, Reg_Map *prev_reg_map, Op* op, int prev_reg_id);
} Reg_Map_Entry_Ops;

typedef struct Reg_Free_List_Ops_struct {
  void (*init)(Reg_Free_List *reg_free_list);
  void (*insert)(Reg_Free_List *reg_free_list, Reg_Map_Entry *entry);
  Reg_Map_Entry *(*delete)(Reg_Free_List *reg_free_list);
} Reg_Free_List_Ops;

typedef struct Reg_Map_Ops_struct {
  void (*init)(Reg_Map *reg_map, uns reg_map_size, Reg_Map *prev_reg_map);
  void (*read)(Reg_Map *reg_map, Op *op, int prev_reg_id);
  int (*alloc)(Reg_Map *reg_map, Op *op, int prev_reg_id);
  void (*free)(Reg_Map *reg_map, Reg_Map_Entry *entry);
  void (*write_back)(Reg_Map *reg_map, int reg_id);
  void (*flush_mispredict)(Reg_Map *reg_map, int reg_id);
  void (*release_last)(Reg_Map *reg_map, int reg_id);
} Reg_Map_Ops;

/**************************************************************************************/
/* External Methods */

void reg_file_init(void);                 // init the register map tables
Flag reg_file_available(uns);             // check if there are enough register entries
void reg_file_rename(Op*);                // read source registers and alloc destination registers
void reg_file_execute(Op*);               // write back into the register
void reg_file_recover(Counter);           // flush registers of misprediction operands
void reg_file_commit(Op*);                // release the last register with same architectural id

#endif /* #ifndef __MAP_RENAME_H__ */
