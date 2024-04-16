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
 * File         : map_consume.h
 * Author       : Y. Zhao, Litz Lab
 * Date         : 4/2024
 * Description  : Register Unconsumed Producers Elimination
 ***************************************************************************************/

#ifndef __MAP_CONSUME_H__
#define __MAP_CONSUME_H__

#include "libs/hash_lib.h"
#include "libs/list_lib.h"
#include "op.h"

/**************************************************************************************
 * Registe Consume Table Functionality
 *
 * 1. Predictor
 *   1) Unconsume Collection
 *      - Collect the unconsume info by signature (PC) in register ranaming and
 *        store into a hash
 *   2) Elimination Decision
 *      - Based on the unconsuming count and ratio of the signature (PC) to
 *        determine if do elimination
 *
 * 2. Elimination
 *   1) Resource Bypass
 *      - If an op is predicted as ELIMINATION, do not let it go to the RS and FU
 *        in node stage
 *   2) Precommit Mechanism
 *      - Introduce a PRECOMMIT state to enable an op release and retire without COMMIT
 *        leveraging resolve
 *   3) Misprediction Flushing (TODO)
 *      - Propose a new renaming misprediction and schedule a flush when a ELIMINATION
 *        regsiter is consumed
 **************************************************************************************/

/**************************************************************************************
 * Registe Consume Table Structure
 *
 * 1. Meta Table Map
 *    key: index of register file ptag
 *    val: meta info of producers
 *    len: num of physical register file entries
 *
 * 2. Signature Hash Map
 *    key: signature (PC) of producer op
 *    val: counters of producer unconsuming info
 **************************************************************************************/

/**************************************************************************************/
/* Types */

// state of map entry to check if it is being alloc
typedef enum Reg_Consume_Map_Entry_State_enum {
  REG_CONSUME_MAP_ENTRY_STATE_FREE,
  REG_CONSUME_MAP_ENTRY_STATE_ALLOC
} Reg_Consume_Map_Entry_State;

// hash entries for collecting producer information by producer signature
typedef struct Reg_Consume_Hash_Entry_struct {
  Counter num_produce;
  Counter num_unconsume;
} Reg_Consume_Hash_Entry;

// meta map entries for producer tracking by register file index
typedef struct Reg_Consume_Map_Entry_struct {
  /* map entry meta info */
  uns64 signature;
  Flag  if_consume;
  int   dst_ptag[MAX_DESTS];

  /* elimination target prediction flag */
  Flag  if_eliminate;

  /* info for analysis */
  Counter                     op_num;
  Flag                        off_path;
  Reg_Consume_Map_Entry_State entry_state;
  Inst_Info                   inst_info;
} Reg_Consume_Map_Entry;

typedef struct Reg_Consume_Table_struct {
  /* Predictor */
  Reg_Consume_Map_Entry* table_map;         // store meta info of producers by the index of register file ptag
  uns                    table_map_size;    // map size is equal to register file
  Hash_Table             signature_hash;    // collect counters of producer unconsuming info by signature (PC)

  /* Elimination */
  uns   unresolved_br_num;                  // determine if do precommit when fetch
} Reg_Consume_Table;

/**************************************************************************************/
/* Prototypes */

void consume_table_init(uns);

/* Predictor */
void consume_table_track_consume(Op*, uns);     // track that produced value has been consumed when register read src
void consume_table_track_produce(Op*);          // track that destination has been produced when register write dst
void consume_table_train(uns);                  // do training update when all destination registers of an op are released
void consume_table_predict(Op*);                // do prediction to determine if the op is the elimination target

/* Elimination */
void consume_table_fetch(Op*);                  // precommit op when fetch if there is not unresolved branch
void consume_table_resolve(Op*);                // update the precommit state of the op
Flag consume_table_mispredict(uns);             // check if the source register of an elimination target is read
void consume_table_recover(void);               // schedule a misprediction flush

#endif /* #ifndef __MAP_CONSUME_H__ */