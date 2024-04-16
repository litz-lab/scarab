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
 * File         : map_consume.c
 * Author       : Y. Zhao, Litz Lab
 * Date         : 4/2024
 * Description  : Register Unconsumed Producer Instructions Elimination
 ***************************************************************************************/

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "thread.h"
#include "xed-interface.h"
#include "map_consume.h"

/**************************************************************************************/
/* Global Variables */

Reg_Consume_Table *consume_table = NULL;

/**************************************************************************************/
/* Static Inline Prototypes */

// train and predict
static inline uns64 consume_table_get_signature(Op*);
static inline Flag  consume_table_if_target(uns64);
static inline void  consume_table_train_update(uns);
static inline void  consume_table_release_entry(uns);

// print analysis info
static inline void  consume_table_print_hash_entry(void*, void*);
static inline void  consume_table_print_inst_info(Inst_Info*);

/**************************************************************************************/
/* Static Internal Methods */

/* return the designated signature (PC) as the key of hash */
static inline uns64 consume_table_get_signature(Op *op) {
  ASSERT(0, REG_CONSUME_ENABLE && consume_table != NULL && op != NULL);

  uns64 signiture = op->inst_info->addr;
  return signiture;
}

/* determine if it is the eliminating target based on the count and ratio */
static inline Flag consume_table_if_target(uns64 sign) {
  ASSERT(0, REG_CONSUME_ENABLE && consume_table != NULL);

  // return if it is a new signature
  Reg_Consume_Hash_Entry *entry = (Reg_Consume_Hash_Entry *)hash_table_access(&consume_table->signature_hash, sign);
  if (!entry)
    return FALSE;

  // do not regard as target when low execution count
  if (entry->num_unconsume < REG_CONSUME_COUNT_THRESH)
    return FALSE;

  // only consider high unconsume ratio
  uns ratio = entry->num_unconsume * 100 / entry->num_produce;
  if (ratio < REG_CONSUME_RATIO_THRESH)
    return FALSE;
  else
    return TRUE;
}

/* update data when all of the destination registers of an instruction are released */
static inline void consume_table_train_update(uns ind) {
  ASSERT(0, REG_CONSUME_ENABLE && consume_table != NULL);

  Reg_Consume_Map_Entry *entry = &consume_table->table_map[ind];
  ASSERT(0, entry->entry_state != REG_CONSUME_MAP_ENTRY_STATE_FREE);

  // do not update statistics if the op is off-path
  if (entry->off_path)
    return;

  // access the entry from the hash map
  Flag if_new_hash_entry = FALSE;
  Reg_Consume_Hash_Entry *hash_entry = (Reg_Consume_Hash_Entry *)hash_table_access_create(
    &consume_table->signature_hash, entry->signature, &if_new_hash_entry
  );
  if (if_new_hash_entry) {
    hash_entry->num_produce = 0;
    hash_entry->num_unconsume = 0;
  }

  // update the statistics counters
  hash_entry->num_produce++;
  STAT_EVENT(0, REG_CONSUME_STAT_PRODUCE);
  if (entry->if_consume)
    return;
  hash_entry->num_unconsume++;
  STAT_EVENT(0, REG_CONSUME_STAT_UNCONSUME);
}

/* release current entry after ptags of other entries with same op when the corresponding release RF entry */
static inline void consume_table_release_entry(uns ind) {
  ASSERT(0, REG_CONSUME_ENABLE && consume_table != NULL);
  Reg_Consume_Map_Entry *entry = &consume_table->table_map[ind];

  entry->signature = 0;
  entry->if_consume = FALSE;
  for (uns ii = 0; ii < MAX_DESTS; ii++)
    entry->dst_ptag[ii] = -1;

  entry->if_eliminate = FALSE;
  entry->op_num = 0;
  entry->off_path = FALSE;
  entry->entry_state = REG_CONSUME_MAP_ENTRY_STATE_FREE;
  memset(&entry->inst_info, 0, sizeof(Inst_Info));
}

/**************************************************************************************/

static inline void consume_table_print_hash_entry(void* hash_entry, void* arg) {
  Reg_Consume_Hash_Entry *entry = (Reg_Consume_Hash_Entry *)hash_entry;
  printf("[%lld, %lld],\n", entry->num_produce, entry->num_unconsume);
}

static inline void consume_table_print_inst_info(Inst_Info *inst_info) {
  uns16 op_code = inst_info->table_info->true_op_type;
  printf("[%s] - 0x%x. ", xed_iclass_enum_t2str(op_code), op_code);
  printf("pc: %lld. cf: %d. ", inst_info->addr, inst_info->table_info->cf_type);

  printf("src#%d: ", inst_info->table_info->num_src_regs);
  for (int ii = 0; ii < inst_info->table_info->num_src_regs; ii++)
    printf("(%d); ", inst_info->srcs[ii].id);
  printf("dest#%d: ", inst_info->table_info->num_dest_regs);
  for (int ii = 0; ii < inst_info->table_info->num_dest_regs; ii++)
    printf("(%d); ", inst_info->dests[ii].id);
  printf("\n");
}

/**************************************************************************************/
/* External Function Call */

/*
  Called by:
  --- map.c --> init
  Procedure:
  --- init the register consume table for both the predictor and elimination
*/
void consume_table_init(uns array_size) {
  if (!REG_CONSUME_ENABLE)
    return;

  /* consume info collection for predictor */
  consume_table = (Reg_Consume_Table *)malloc(sizeof(Reg_Consume_Table));
  consume_table->table_map = (Reg_Consume_Map_Entry *)malloc(sizeof(Reg_Consume_Map_Entry) * array_size);
  consume_table->table_map_size = array_size;
  for (uns ii = 0; ii < array_size; ii++)
    consume_table_release_entry(ii);
  init_hash_table(&consume_table->signature_hash, "reg consume hash", NODE_TABLE_SIZE, sizeof(Reg_Consume_Hash_Entry));

  /* elimination mechanism */
  consume_table->unresolved_br_num = 0;
}

/*
  Called by:
  --- map.c --> read register source
  Procedure:
  --- track that produced value has been consumed
*/
void consume_table_track_consume(Op *op, uns ind) {
  if (!REG_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && ind < consume_table->table_map_size);

  // skip reading invalid source in the very begining
  Reg_Consume_Map_Entry *entry = &consume_table->table_map[ind];
  if (entry->entry_state == REG_CONSUME_MAP_ENTRY_STATE_FREE)
    return;

  // only track the on-path instruction
  if (op->off_path)
    return;

  // mark all dst registers from a same op as consume
  for (uns ii = 0; ii < MAX_DESTS; ii++) {
    int ind = entry->dst_ptag[ii];
    if (ind == -1)
      break;

    entry = &consume_table->table_map[ind];
    ASSERT(0, entry->entry_state == REG_CONSUME_MAP_ENTRY_STATE_ALLOC);
    entry->if_consume = TRUE;
  }
}

/*
  Called by:
  --- map.c --> write register destination
  Procedure:
  --- track that destination has been produced
*/
void consume_table_track_produce(Op *op) {
  if (!REG_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL);

  Reg_Consume_Map_Entry *entry;
  for (uns ii = 0; ii < op->table_info->num_dest_regs; ii++) {
    int ind = op->dst_reg_file_ptag[ii];
    ASSERT(0, ind != -1 && ind < consume_table->table_map_size);
    entry = &consume_table->table_map[ind];
    ASSERT(0, entry->entry_state == REG_CONSUME_MAP_ENTRY_STATE_FREE);

    // store metadata of op
    entry->signature = consume_table_get_signature(op);
    entry->if_consume = FALSE;
    entry->if_eliminate = op->if_eliminate;
    entry->op_num = op->op_num;
    entry->off_path = op->off_path;
    entry->entry_state = REG_CONSUME_MAP_ENTRY_STATE_ALLOC;
    entry->inst_info = *op->inst_info;

    // insert all destination ptag to all entries from same op
    for (uns jj = 0; jj < op->table_info->num_dest_regs; jj++) {
      ASSERT(0, op->dst_reg_file_ptag[jj] != -1);
      entry->dst_ptag[jj] = op->dst_reg_file_ptag[jj];
    }
  }
}


/*
  Called by:
  --- map.c --> release register
  Procedure:
  --- do training update when all destination registers of an op are released
*/
void consume_table_train(uns ind) {
  if (!REG_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && ind < consume_table->table_map_size);

  // skip releasing invalid source in the very begining
  Reg_Consume_Map_Entry *entry = &consume_table->table_map[ind];
  if (entry->entry_state == REG_CONSUME_MAP_ENTRY_STATE_FREE)
    return;

  // remove ptag of current entry
  int curr_ind = -1;
  for (uns ii = 0; ii < MAX_DESTS; ii++) {
    if (entry->dst_ptag[ii] == ind) {
      curr_ind = ii;
      entry->dst_ptag[ii] = -1;
      break;
    }
  }

  // remove self ptag from other destination registers of same op
  Flag if_update = TRUE;
  for (uns ii = 0; ii < MAX_DESTS; ii++) {
    if (entry->dst_ptag[ii] == -1)
      continue;

    Reg_Consume_Map_Entry *entry_same_op = &consume_table->table_map[entry->dst_ptag[ii]];
    ASSERT(0, entry_same_op->dst_ptag[curr_ind] == ind);
    entry_same_op->dst_ptag[curr_ind] = -1;
    if_update = FALSE;
  }

  // do update only all destination registers are released
  if (if_update)
    consume_table_train_update(ind);

  // release the current entry
  consume_table_release_entry(ind);
}

/*
  Called by:
  --- map.c --> process op
  Procedure:
  --- do prediction to determine if the op is the elimination target
*/
void consume_table_predict(Op* op) {
  if (!REG_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && op != NULL);

  STAT_EVENT(0, REG_CONSUME_STAT_TOTAL);

  // if it is a control flow op, do not do prediction
  if (op->table_info->cf_type)
    return;

  // do prediction based on the execution count and ratio
  if (!consume_table_if_target(consume_table_get_signature(op)))
    return;
  op->if_eliminate = TRUE;
  STAT_EVENT(0, REG_CONSUME_STAT_ELIMINATE);
}

/*
  Called by:
  --- map.c --> process op
  Procedure:
  --- precommit op if there is not unresolved branch when fetch
*/
void consume_table_fetch(Op* op) {
  if (!REG_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && op != NULL);

  // do not consider off-path branch
  if (op->off_path)
    return;

  // increase the unsolved br num
  if (op->table_info->cf_type)
    consume_table->unresolved_br_num++;

  // if there is not unresolved branch, directly precommit op
  if (consume_table->unresolved_br_num)
    return;
  op->if_precommit = TRUE;
}

/*
  Called by:
  --- bp.c --> resolve
  Procedure:
  --- update the precommit state of the op
*/
void consume_table_resolve(Op* op) {
  if (!REG_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && op != NULL);

  // do not consider off-path branch
  if (op->off_path)
    return;

  // find the oldest unresolved op
  Op** op_p = (Op**)list_start_head_traversal(&td->seq_op_list);
  while(op_p && (*op_p)->op_num < op->op_num) {
    ASSERT(0, !(*op_p)->off_path);

    if (!(*op_p)->if_precommit)
      break;
    op_p = (Op**)list_next_element(&td->seq_op_list);
  }

  // precommit the current op
  op->if_precommit = TRUE;

  // check if it is out-of-order resolve
  ASSERT(0, op->op_num >= (*op_p)->op_num);
  if (op->op_num != (*op_p)->op_num)
    return;

  // do precommit from the oldest resolved op to the next unresolved op
  while (op_p) {
    if ((*op_p)->off_path)
      break;

    if ((*op_p)->table_info->cf_type) {
      if (!(*op_p)->if_precommit)
        break;

      // decrease the unresolved branch couter
      ASSERT(0, consume_table->unresolved_br_num > 0);
      consume_table->unresolved_br_num--;
    }

    (*op_p)->if_precommit = TRUE;
    op_p = (Op**)list_next_element(&td->seq_op_list);
  }
}

/*
  Called by:
  --- map.c --> read register source
  Procedure:
  --- check if the source register of an elimination target is read
*/
Flag consume_table_mispredict(uns ind) {
  if (!REG_CONSUME_ENABLE)
    return FALSE;
  ASSERT(0, consume_table != NULL && ind < consume_table->table_map_size);

  Reg_Consume_Map_Entry *entry = &consume_table->table_map[ind];
  if (!REG_CONSUME_ELIMINATE_ENABLE || !entry->if_eliminate)
    return FALSE;
  STAT_EVENT(0, REG_CONSUME_STAT_MISPREDICT);
  return TRUE;
}

/*
  Called by:
  --- map.c --> read register source
  Procedure:
  --- schedule a misprediction flush
*/
void consume_table_recover(void) {
  if (!REG_CONSUME_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && REG_CONSUME_ELIMINATE_ENABLE);

  // TODO
  return;
}
