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
 * Date         : 03/2024
 * Description  : Register Renaming
 ***************************************************************************************/

#ifndef __MAP_RENAME_H__
#define __MAP_RENAME_H__

#include "isa/isa_macros.h"
#include "op.h"

/**************************************************************************************/
/* Constexpr */

// default reg id in the reg file
const static int REG_FILE_INVALID_REG_ID = -1;

// register state for releasing
typedef enum Reg_File_Entry_State_enum {
  REG_FILE_ENTRY_STATE_FREE,
  REG_FILE_ENTRY_STATE_ALLOC,
  REG_FILE_ENTRY_STATE_PRODUCED,
  REG_FILE_ENTRY_STATE_COMMIT,
  REG_FILE_ENTRY_STATE_DEAD,
  REG_FILE_ENTRY_STATE_NUM
} Reg_File_Entry_State;

/**************************************************************************************/
/* Types */

typedef struct Reg_File_Entry_struct {
  // op info (the pointer of op + the deep copy of special val)
  Op       *op;
  Counter  op_num;
  Counter  unique_num;
  Flag     off_path;

  // register info
  int                  reg_arch_id;
  int                  reg_ptag;
  Reg_File_Entry_State reg_state;

  // tracking free physical register
  struct Reg_File_Entry_struct *next_free;

  // tracking the ops use the same architectural register
  int prev_same_arch_id;
} Reg_File_Entry;

typedef struct Merged_Reg_File_struct {
  /* map each architectural register to the latest physical register */
  int             reg_map_table[NUM_REG_IDS];

  /* map ptags to physical registers (register entries) for both speculative and committed op */
  Reg_File_Entry* reg_file;
  uns             reg_file_size;

  /* track all free physical registers */
  Reg_File_Entry* reg_free_list_head;
  uns             reg_free_num;
} Merged_Reg_File;

typedef struct Reg_Renaming_Table_struct {
  Merged_Reg_File *merged_rf;
} Reg_Renaming_Table;

/**************************************************************************************/
/* External Methods */

void rename_table_init(void);
void rename_table_process(Op*);               // lookup src reg and alloc dst reg
void rename_table_produce(Op*);               // update flag to indicate writing back
Flag rename_table_available(uns);             // check if enough physical register
void rename_table_commit(Op*);                // release the previous reg with same arch id
void rename_table_recover(Counter);           // flush reg of misprediction op

#endif /* #ifndef __MAP_RENAME_H__ */