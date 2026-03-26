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
 * File         : op_info.c
 * Description  : Op dependency source list (src_info) and per-source ready bits.
 ***************************************************************************************/

#include "op_info.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/utils.h"

#include "op.h"

#define SRC_INFO_CAP_TIER1 2
#define SRC_INFO_CAP_TIER2 8
#define SRC_INFO_CAP_TIER3 128

static uns next_src_info_capacity(uns cur_cap, uns need) {
  uns n = cur_cap == 0 ? SRC_INFO_CAP_TIER1 : cur_cap;
  while (n < need) {
    if (n < SRC_INFO_CAP_TIER2)
      n = SRC_INFO_CAP_TIER2;
    else if (n < SRC_INFO_CAP_TIER3)
      n = SRC_INFO_CAP_TIER3;
    else
      n = n * 2;
  }
  return n;
}

static void op_sources_ensure_capacity(Op* op, uns need) {
  uns new_cap;
  uns new_nwords;
  Src_Info* new_si;
  uns64* new_words;
  uns old_nwords;

  ASSERT(0, op);
  if (need == 0)
    return;
  if (op->src_info_cap >= need)
    return;

  new_cap = next_src_info_capacity(op->src_info_cap, need);
  new_nwords = (new_cap + 63) / 64;
  old_nwords = op->srcs_not_rdy_nwords;

  new_si = (Src_Info*)realloc(op->src_info, new_cap * sizeof(Src_Info));
  ASSERTM(op->proc_id, new_si, "realloc src_info cap %u -> %u\n", op->src_info_cap, new_cap);
  if (new_cap > op->src_info_cap)
    memset((char*)new_si + op->src_info_cap * sizeof(Src_Info), 0, (new_cap - op->src_info_cap) * sizeof(Src_Info));
  op->src_info = new_si;

  new_words = (uns64*)realloc(op->srcs_not_rdy_words, new_nwords * sizeof(uns64));
  ASSERTM(op->proc_id, new_words, "realloc rdy words nwords %u -> %u\n", old_nwords, new_nwords);
  if (new_nwords > old_nwords)
    memset(new_words + old_nwords, 0, (new_nwords - old_nwords) * sizeof(uns64));
  op->srcs_not_rdy_words = new_words;
  op->srcs_not_rdy_nwords = new_nwords;
  op->src_info_cap = new_cap;
}

void op_sources_free(Op* op) {
  if (!op)
    return;
  free(op->src_info);
  op->src_info = NULL;
  free(op->srcs_not_rdy_words);
  op->srcs_not_rdy_words = NULL;
  op->src_info_cap = 0;
  op->srcs_not_rdy_nwords = 0;
  op->num_srcs = 0;
}

void op_sources_set_not_rdy(Op* op, uns bit) {
  uns wi = bit / 64;
  uns bi = bit % 64;
  ASSERT(op->proc_id, op);
  ASSERT(op->proc_id, bit < op->num_srcs);
  ASSERT(op->proc_id, op->srcs_not_rdy_words && wi < op->srcs_not_rdy_nwords);
  op->srcs_not_rdy_words[wi] |= (1ULL << bi);
}

uns op_sources_add(Op* op, Dep_Type type, Op* src_op, Counter src_op_num, Counter src_unique_num) {
  uns src_num;
  Src_Info* info;

  ASSERT(op->proc_id, op && src_op);
  ASSERT(op->proc_id, op->proc_id == src_op->proc_id);
  ASSERT(op->proc_id, type < NUM_DEP_TYPES);
  ASSERTM(op->proc_id, src_op_num < op->op_num, "op:%s  src_op:%s\n", unsstr64(op->op_num), unsstr64(src_op_num));

  op_sources_ensure_capacity(op, op->num_srcs + 1);
  src_num = op->num_srcs++;
  info = &op->src_info[src_num];
  ASSERTM(op->proc_id, src_num < op->src_info_cap, "src_num:%u cap:%u\n", src_num, op->src_info_cap);

  info->type = type;
  info->op = src_op;
  info->op_num = src_op_num;
  info->unique_num = src_unique_num;

  op_sources_set_not_rdy(op, src_num);

  if (type == MEM_DATA_DEP) {
    ASSERT(op->proc_id,
           src_op->inst_info->table_info.mem_type == MEM_ST && op->inst_info->table_info.mem_type == MEM_LD);
  }
  return src_num;
}

void op_sources_clear_not_rdy(Op* op, uns bit) {
  uns wi = bit / 64;
  uns bi = bit % 64;
  ASSERT(op->proc_id, op);
  ASSERT(op->proc_id, bit < op->num_srcs);
  ASSERT(op->proc_id, op->srcs_not_rdy_words && wi < op->srcs_not_rdy_nwords);
  op->srcs_not_rdy_words[wi] &= ~(1ULL << bi);
}

Flag op_sources_test_not_rdy(const Op* op, uns bit) {
  uns wi = bit / 64;
  uns bi = bit % 64;
  ASSERT(op->proc_id, op);
  ASSERT(op->proc_id, bit < op->num_srcs);
  ASSERT(op->proc_id, op->srcs_not_rdy_words && wi < op->srcs_not_rdy_nwords);
  return (Flag)((op->srcs_not_rdy_words[wi] >> bi) & 1ULL);
}

Flag op_sources_not_rdy_is_clear(const Op* op) {
  uns i;
  if (!op->srcs_not_rdy_words || op->srcs_not_rdy_nwords == 0)
    return TRUE;
  for (i = 0; i < op->srcs_not_rdy_nwords; i++) {
    if (op->srcs_not_rdy_words[i])
      return FALSE;
  }
  return TRUE;
}
