/*
 * Copyright 2026 University of California Santa Cruz
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
 * File         : decode_cache.h
 * Author       : Litz Lab
 * Date         : 11/2026
 * Description  : Per-core cache of decoded trace instructions, keyed by PC
 ***************************************************************************************/

#ifndef __DECODE_CACHE_H__
#define __DECODE_CACHE_H__

#include <cstdint>

#include "globals/global_types.h"

#include "ctype_pin_inst.h"

// An instruction decoded once and reused on every later visit to its PC.
struct DecodedInst {
  ctype_pin_inst_static inst;
  int num_mem_ops;    // memory records the trace delivers after the instruction record
  bool unknown_type;  // no bytes to decode: inst is a NOP of the reported size
  bool is_rep;

  // PT only: what the PT reader and pt_fe need from the XED decode, so they never re-decode
  uint8_t trace_patch;       // TracePatch: how the record was repaired before decoding
  bool is_nop_category;      // XED_CATEGORY_NOP
  bool is_syscall_category;  // XED_CATEGORY_SYSCALL
  bool is_taken_category;    // unconditional/conditional branch, call or return
  bool changes_cf;           // XED_INS_ChangeControlFlow
  bool is_direct_br;         // XED_INS_IsDirectBranchOrCall
  bool cf_sets_target;       // fill_in_cf_info set inst.branch_target (direct branch path)
  bool is_ret;               // RET_NEAR / RET_FAR
  bool is_roi_marker;        // xchg rcx, rcx
  uint64_t direct_target;    // valid if is_direct_br
  uint8_t num_mem_operands;  // all XED memory operands, used or not
  uint8_t mem_read_mask;     // bit i: memory operand i is read
  uint8_t mem_written_mask;  // bit i: memory operand i is written
};

// Per core: each core's trace has its own address space. Pointers stay valid until erased.
DecodedInst* decode_cache_find(uns proc_id, uint64_t pc);
DecodedInst* decode_cache_insert(uns proc_id, uint64_t pc, const DecodedInst& decoded);
void decode_cache_erase(uns proc_id, uint64_t pc);

#endif
