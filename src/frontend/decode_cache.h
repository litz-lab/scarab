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

#include <stdint.h>

#include "globals/global_types.h"

#include "ctype_pin_inst.h"
#include "static_inst_info.h"

// The uops that instances of one memory shape (num_ld << 8 | num_st) and cf_type crack into,
// filled by the uop decoder on the first such instance. Most instructions have one variant;
// gathers/scatters, instances without memory records and PIN's CF_SYS conversions add more.
typedef struct Uop_Variant_struct {
  uint16_t mem_shape;
  uns8 cf_type;
  uns8 num_uop;  // 0 until the uop decoder fills uops[]
  struct Static_Op_Info_struct* uops[STATIC_INST_MAX_UOPS];
} Uop_Variant;

typedef struct DecodedInst DecodedInst;

#ifdef __cplusplus
extern "C" {
#endif
// The decoded instruction at st's PC, inserting st when the PC has no entry or one with other bytes
// (frontends that do not decode into the cache, e.g. PIN). Pointers stay valid for the whole run.
DecodedInst* decode_cache_static(uns proc_id, const ctype_pin_inst_static* st);
const ctype_pin_inst_static* decode_cache_entry_static(const DecodedInst* decoded);
// The entry's uop variant for {mem_shape, cf_type}, added empty (num_uop == 0) on first use.
Uop_Variant* decode_cache_uops(DecodedInst* decoded, uint16_t mem_shape, uns8 cf_type);
#ifdef __cplusplus
}

#include <vector>

// An instruction decoded once and reused on every later visit to its PC.
struct DecodedInst {
  ctype_pin_inst_static inst;  // first: InstInfo's decoded and static_info alias it
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

  // uop decoder: the uops of each memory shape seen so far
  Uop_Variant uops;
  std::vector<Uop_Variant>* more_uops;
};

// Per core (each core's trace has its own address space), one entry per {PC, instruction bytes}:
// a re-encoded PC (JIT) gets a second entry and the first stays valid for the ops that point to
// it. Entries are never freed. inst.instruction_addr holds the core's address
// (convert_to_cmp_addr); frontends overwrite it per instance.
// The PC's current encoding: the last one inserted or made current.
DecodedInst* decode_cache_find(uns proc_id, uint64_t pc);
DecodedInst* decode_cache_find_encoding(uns proc_id, uint64_t pc, uint64_t lsb, uint64_t msb, uint8_t size);
// Insert a new encoding (keyed by decoded.inst's bytes) and make it the PC's current one.
DecodedInst* decode_cache_insert(uns proc_id, uint64_t pc, const DecodedInst& decoded);
void decode_cache_make_current(uns proc_id, uint64_t pc, DecodedInst* decoded);
// Wrong-path visibility: records the core fetching the PC on the path as inst inst_uid.
void decode_cache_note_fetch(uns proc_id, uint64_t pc, uint64_t inst_uid);
// The PC's current encoding if the core had fetched it on the path by inst inst_uid, else null.
DecodedInst* decode_cache_find_fetched(uns proc_id, uint64_t pc, uint64_t inst_uid);
#endif

#endif
