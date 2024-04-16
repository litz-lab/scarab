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
 * Description  : Register Unconsumed Producer Instructions Optimization
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

/* Registe Consume Table
 * - Predictor
 *   --- Consume Info Collecting
 *   --- Unconsume Decision
 * - Optimization
 *   --- Precommit Mechanism
 *   --- Misprediction Flushing
 */

/**************************************************************************************/
/* Static Inline Prototypes */

static inline uns64             consume_table_get_sign(Op*);
static inline Reg_Consume_Node* consume_table_get_node(Op*);
static inline Reg_Consume_Conf  consume_table_get_conf(uns64);

static inline void              consume_table_train_update(uns);
static inline void              consume_table_unconsume_predict(Op*);
static inline void              consume_table_precommit_issue(Op*);

static inline void              consume_table_print_hash_entry(void*, void*);
static inline void              consume_table_print_inst_info(Inst_Info*);
static inline void              consume_table_print_whole_list(void);

/**************************************************************************************/
/* Static Internal Methods */

/* return the designated signiture */
static inline uns64 consume_table_get_sign(Op *op) {
  ASSERT(0, REG_CONSUME_TABLE_ENABLE && consume_table != NULL && op != NULL);

  uns64 sign = 0;
  switch (consume_table->sign_type) {
    case REG_CONSUME_SIGH_PC:
      sign = op->inst_info->addr;
      break;
    case REG_CONSUME_SIGH_MEM:
      sign = op->oracle_info.va;
      break;
    default:
      break;
  }

  return sign;
}

/* return the current node of the op */
static inline Reg_Consume_Node* consume_table_get_node(Op* op) {
  ASSERT(0, REG_CONSUME_TABLE_ENABLE && consume_table != NULL);
  ASSERT(0, op != NULL && op->table_info->num_dest_regs != 0);

  Reg_Consume_Node *node;
  if (consume_table->cur_write_node != NULL) {
    // same op share same node
    node = consume_table->cur_write_node;
  } else {
    // allocate a new node for each new op
    node = (Reg_Consume_Node *)malloc(sizeof(Reg_Consume_Node));
    node->op_num = op->op_num;
    node->off_path = op->off_path;
    node->sign = consume_table_get_sign(op);
    node->inst_info = *op->inst_info;
    node->in_degree = 0;
    node->out_degree = 0;
    node->if_pred_unconsume = op->if_pred_unconsume;

    // store the node into list when doing the trace analysis
    if (REG_CONSUME_TABLE_TRACE_ANALYSIS) {
      Reg_Consume_Node **node_p = dl_list_add_tail(&consume_table->consume_node_list);
      *node_p = node;
    }

    // set the node as the current node for writing
    consume_table->cur_write_node = node;
  }

  // update current node when the op has written all of the destination registers
  if (++consume_table->cur_write_num == op->table_info->num_dest_regs) {
    consume_table->cur_write_node = NULL;
    consume_table->cur_write_num = 0;
  }

  return node;
}

/* return the confidence of the signiture based on the collecting info */
static inline Reg_Consume_Conf consume_table_get_conf(uns64 sign) {
  ASSERT(0, REG_CONSUME_TABLE_ENABLE && consume_table != NULL);

  // return if it is a new signiture
  Reg_Consume_Hash_Entry *entry = (Reg_Consume_Hash_Entry *)hash_table_access(&consume_table->sign_hash, sign);
  if (!entry)
    return REG_CONSUME_CONF_SKEPT;

  // low fetching/execution count
  if (entry->num_unconsumed < REG_CONSUME_COUNT_THRESH)
    return REG_CONSUME_CONF_SKEPT;

  // calculate the confidence interval
  uns ratio = entry->num_unconsumed * 100 / entry->num_all_produced;
  if (ratio < REG_CONSUME_CONF_THRESH_AMBIV)
    return REG_CONSUME_CONF_SKEPT;
  else if (ratio < REG_CONSUME_CONF_THRESH_LIKE)
    return REG_CONSUME_CONF_AMBIV;
  else if (ratio < REG_CONSUME_CONF_THRESH_CERT)
    return REG_CONSUME_CONF_LIKE;
  else
    return REG_CONSUME_CONF_CERT;
}

/* update data when all of the destination registers of an instruction are released */
static inline void consume_table_train_update(uns ind) {
  ASSERT(0, REG_CONSUME_TABLE_ENABLE && consume_table != NULL && consume_table->node_array[ind]);

  // do not record statistics if the op is off-path
  if (consume_table->node_array[ind]->off_path)
    return;

  // access the entry from the hash map
  Flag if_new_entry = FALSE;
  Reg_Consume_Hash_Entry *entry = (Reg_Consume_Hash_Entry *)hash_table_access_create(
    &consume_table->sign_hash, consume_table->node_array[ind]->sign, &if_new_entry
  );
  if (if_new_entry) {
    entry->num_all_produced = 0;
    entry->num_consumed = 0;
    entry->num_unconsumed = 0;
  }

  // update the statistics
  ASSERT(0, entry->num_consumed + entry->num_unconsumed == entry->num_all_produced);
  entry->num_all_produced++;
  consume_table->num_all_produced++;
  if (consume_table->node_array[ind]->in_degree != 0) {
    entry->num_consumed++;
    consume_table->num_consumed++;
  } else {
    entry->num_unconsumed++;
    consume_table->num_unconsumed++;
  }
}

/* do prediction to determine if the op is unconsumed */
static inline void consume_table_unconsume_predict(Op* op) {
  ASSERT(0, REG_CONSUME_TABLE_ENABLE && consume_table != NULL && op != NULL);

  consume_table->num_pred_all++;

  // if it is a control flow op, do not do prediction
  if (op->table_info->cf_type)
    return;

  // do prediction based on the execution count and ratio
  if (consume_table_get_conf(consume_table_get_sign(op)) < REG_CONSUME_CONF_CERT)
    return;
  op->if_pred_unconsume = TRUE;
  consume_table->num_pred_unconsume++;
}

/* precommit op when there is not unresolve branch */
static inline void consume_table_precommit_issue(Op* op) {
  ASSERT(0, REG_CONSUME_TABLE_ENABLE && consume_table != NULL && op != NULL);

  // do not consider off-path branch
  if (op->off_path)
    return;

  // increase the br num
  if (op->table_info->cf_type)
    consume_table->unresolved_br_num++;

  // if there is not unresolved branch, directly precommit op
  if (consume_table->unresolved_br_num)
    return;
  op->if_precommit = TRUE;
}

/**************************************************************************************/

/* print the consume hash entry consume info */
static inline void consume_table_print_hash_entry(void* hash_entry, void* arg) {
  Reg_Consume_Hash_Entry *entry = (Reg_Consume_Hash_Entry *)hash_entry;
  printf("[%lld, %lld, %lld],\n", entry->num_all_produced, entry->num_unconsumed, entry->num_consumed);
}

/* print the instruction info */
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

/* print the target trace info and its context */
static inline void consume_table_print_whole_list(void) {
  ASSERT(0, REG_CONSUME_TABLE_ENABLE && consume_table != NULL);
  if (!REG_CONSUME_TABLE_TRACE_ANALYSIS)
    return;

  Reg_Consume_Node **node_p;
  Reg_Consume_Hash_Entry *entry;

  // record the target for printing
  Hash_Table target_trace_hash;
  init_hash_table(&target_trace_hash, "target trace hash", NODE_TABLE_SIZE, sizeof(uns64));

  // select the target trace and add the into hash table
  node_p = (Reg_Consume_Node **)list_start_head_traversal(&consume_table->consume_node_list);
  for (; node_p; node_p = (Reg_Consume_Node **)list_next_element(&consume_table->consume_node_list)) {
    if (consume_table_get_conf((*node_p)->sign) == REG_CONSUME_CONF_SKEPT)
      continue;

    Flag if_new_entry = FALSE;
    uns64 *target_sign = (uns64 *)hash_table_access_create(&target_trace_hash, (*node_p)->op_num, &if_new_entry);
    *target_sign = (*node_p)->sign;
  }

  // print the previous N and subsequent N traces of the target
  int print_counter = 0;
  node_p = (Reg_Consume_Node **)list_start_head_traversal(&consume_table->consume_node_list);
  for (; node_p; node_p = (Reg_Consume_Node **)list_next_element(&consume_table->consume_node_list)) {
    if ((*node_p)->off_path)
      continue;

    // determine if the trace should be printed by checking if it is target or context
    uns64 *target_sign = (uns64 *)hash_table_access(&target_trace_hash, (*node_p)->op_num);
    Flag *if_print = (Flag *)hash_table_access(&target_trace_hash,
      (*node_p)->op_num + REG_CONSUME_TABLE_TRACE_ANALYSIS_CONTEXT_NUM);

    // update print counter
    if (print_counter <= 0) {
      if ((*node_p)->op_num > REG_CONSUME_TABLE_TRACE_ANALYSIS_CONTEXT_NUM) {
        if (!if_print)
          continue;
        print_counter = REG_CONSUME_TABLE_TRACE_ANALYSIS_CONTEXT_NUM * 2;
      } else {
        if (!target_sign)
          continue;
        print_counter = REG_CONSUME_TABLE_TRACE_ANALYSIS_CONTEXT_NUM;
      }
    }
    print_counter--;

    // add prefix for different traces
    if (target_sign && *target_sign == (*node_p)->sign) {
      if (consume_table_get_conf((*node_p)->sign) == REG_CONSUME_CONF_CERT)
        printf(" * ");
      else if (consume_table_get_conf((*node_p)->sign) == REG_CONSUME_CONF_LIKE)
        printf(" + ");
      else if (consume_table_get_conf((*node_p)->sign) == REG_CONSUME_CONF_AMBIV)
        printf(" ? ");
    } else {
      printf(" - ");
    }

    // print trace info
    consume_table_print_inst_info(&(*node_p)->inst_info);
    printf(" --- op_num: %lld, off_path: %d, if_consumed: %d, ",
      (*node_p)->op_num, (*node_p)->off_path, !(*node_p)->in_degree);

    // print consume info
    entry = (Reg_Consume_Hash_Entry *)hash_table_access(&consume_table->sign_hash, (*node_p)->sign);
    if (entry) {
      consume_table_print_hash_entry(entry, NULL);
      printf("\n");
    } else {
      printf("\n\n");
    }
  }

  hash_table_clear(&target_trace_hash);
}

/**************************************************************************************/
/* External Function Call */

/*
  Called by:
  --- map.c --> init
  Procedure:
  --- init the consume table for unconsumed producers training and prediction
*/
void consume_table_init(uns array_size) {
  if (!REG_CONSUME_TABLE_ENABLE)
    return;

  /* init the table array */
  consume_table = (Reg_Consume_Table *)malloc(sizeof(Reg_Consume_Table));
  consume_table->node_array = (Reg_Consume_Node **)malloc(sizeof(Reg_Consume_Node *) * array_size);
  consume_table->array_size = array_size;

  /* init the hash map for tracking unconsumed producer */
  for (uns ii = 0; ii < array_size; ii++)
    consume_table->node_array[ii] = NULL;

  /* init the current written node */
  consume_table->cur_write_node = NULL;
  consume_table->cur_write_num = 0;

  /* init the list for recording resolved op num */
  consume_table->unresolved_br_num = 0;

  /* init the misprediction flag */
  consume_table->if_mispredict = FALSE;

  /* init the hash for collecting unconsumed producers and its signiture type */
  init_hash_table(&consume_table->sign_hash, "reg consume hash", NODE_TABLE_SIZE, sizeof(Reg_Consume_Hash_Entry));
  consume_table->sign_type = REG_CONSUME_TABLE_SIGN_TYPE;

  /* init the list for collecting trace info */
  init_list(&consume_table->consume_node_list, "reg consume list", sizeof(Reg_Consume_Node *), TRUE);

  /* init statistics counters */
  consume_table->num_all_produced = 0;
  consume_table->num_consumed = 0;
  consume_table->num_unconsumed = 0;
  consume_table->num_pred_all = 0;
  consume_table->num_pred_unconsume = 0;
  consume_table->num_read_unconsume = 0;
  consume_table->num_stall_unconsume = 0;
}

/*
  Called by:
  --- map.c --> read register source
  Procedure:
  --- update the in-degree of the source node from the entry
*/
void consume_table_read(Op *op, uns ind) {
  if (!REG_CONSUME_TABLE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && ind < consume_table->array_size);

  // skip reading invalid source in the very begining
  if (consume_table->node_array[ind] == NULL)
    return;

  // only track the on-path instruction
  if (!op->off_path)
    ++consume_table->node_array[ind]->in_degree;

  // unconsume misprediction if a unconsume op is read
  if (!consume_table->node_array[ind]->if_pred_unconsume)
    return;
  if (!REG_CONSUME_TABLE_OPT_ENABLE)
    return;
  consume_table->num_read_unconsume++;
  consume_table->if_mispredict = TRUE;
}

/*
  Called by:
  --- map.c --> write register destination
  Procedure:
  --- insert the destination node into the entry and update its out-degree
*/
void consume_table_write(Op *op, uns ind) {
  if (!REG_CONSUME_TABLE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && ind < consume_table->array_size);

  // same op share same node or create a new node for a new op
  consume_table->node_array[ind] = consume_table_get_node(op);

  // increase the current allocation num in reg file
  ++consume_table->node_array[ind]->out_degree;
}

/*
  Called by:
  --- map.c --> release register
  Procedure:
  --- remove the node from the entry and free the node when the out-degree of the node is zero
*/
void consume_table_release(uns ind) {
  if (!REG_CONSUME_TABLE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && ind < consume_table->array_size);

  // skip releasing void in the very begining
  if (consume_table->node_array[ind] == NULL)
    return;

  // collect the info when the node is not in the array
  if (--consume_table->node_array[ind]->out_degree != 0)
    return;
  consume_table_train_update(ind);

  // free the node when not doing the trace analysis
  if (!REG_CONSUME_TABLE_TRACE_ANALYSIS)
    free(consume_table->node_array[ind]);
  consume_table->node_array[ind] = NULL;
}

/*
  Called by:
  --- map.c --> process
  Procedure:
  --- update the precommit and unconsume state of the op
*/
void consume_table_process(Op* op) {
  if (!REG_CONSUME_TABLE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && op != NULL);

  consume_table_precommit_issue(op);
  consume_table_unconsume_predict(op);
}

/*
  Called by:
  --- bp.c --> resolve
  Procedure:
  --- update the precommit and unconsume state of the op
*/
void consume_table_resolve(Op* op) {
  if (!REG_CONSUME_TABLE_ENABLE)
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
  --- check if the source register of an unconsumed op is read
*/
Flag consume_table_mispredict(void) {
  if (!REG_CONSUME_TABLE_ENABLE)
    return FALSE;
  ASSERT(0, consume_table != NULL);

  return consume_table->if_mispredict;
}

void consume_table_recover(void) {
  if (!REG_CONSUME_TABLE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL && REG_CONSUME_TABLE_OPT_ENABLE);

  // recover the misprediction flag
  consume_table->if_mispredict = FALSE;

  // TODO
  return;
}

/**************************************************************************************/

void consume_table_update_stat(void) {
  ASSERT(0, REG_CONSUME_TABLE_ENABLE && REG_CONSUME_TABLE_OPT_ENABLE && consume_table != NULL);
  consume_table->num_stall_unconsume++;
}

void consume_table_print_stat(void) {
  if (!REG_CONSUME_TABLE_ENABLE)
    return;
  ASSERT(0, consume_table != NULL);

  printf("** ALL: %lld, UNCONSUMED: %lld, CONSUMED: %lld\n",
    consume_table->num_all_produced,
    consume_table->num_unconsumed,
    consume_table->num_consumed
  );

  printf("** PRED ALL: %lld, PRED UNCONSUME: %lld, READ UNCONSUME: %lld, STALL UNCONSUME: %lld\n",
    consume_table->num_pred_all,
    consume_table->num_pred_unconsume,
    consume_table->num_read_unconsume,
    consume_table->num_stall_unconsume
  );

  consume_table_print_whole_list();
}