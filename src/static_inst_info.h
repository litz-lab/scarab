/* Copyright 2024 Litz Lab
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
 * File         : static_inst_info.h
 * Description  : Static (read-only after decode) instruction information, split into
 *                per-x86-instruction facts (Static_Inst_Info, one per macro-inst,
 *                shared by all its uops) and per-uop facts (Static_Op_Info, one per
 *                uop). Populated once at decode/intern time; never mutated during
 *                simulation. Dynamic per-execution state lives on Op (see op.h).
 ***************************************************************************************/

#ifndef __STATIC_INST_INFO_H__
#define __STATIC_INST_INFO_H__

#include "globals/global_types.h"

#include "ctype_pin_inst.h"  // Reg_Type
#include "table_info.h"

/* Static register operand: id/type only. The runtime value lives in Op::src_val /
 * Op::dst_val (populated from Trace_Uop), never on the shared static struct. */
typedef struct Static_Reg_Info_struct {
  uns16 reg;      // register number within the register set
  Reg_Type type;  // integer, floating point, extra
  uns16 id;       // flattened register number (unique across sets)
} Static_Reg_Info;

// Max uops per macro-instruction that Static_Inst_Info keeps pointers for. Kept small on purpose --
// this struct is interned by value, so every pointer here is paid per distinct static instruction.
// Observed max is 7 (SPEC gcc); 16 leaves headroom. The decoder ASSERTs num_uop <= this and prints
// the offending PC, so if a bigger macro shows up we bump it (the hard ceiling is MAX_PUP).
#define STATIC_INST_MAX_UOPS 16

struct Static_Op_Info_struct;  // forward decl for the uops[] back-pointers below

/* Per-x86-instruction static info: one instance per macro-instruction, shared by
 * every uop the macro cracks into. Interned by {addr, opcode bytes}. */
typedef struct Static_Inst_Info_struct {
  Addr addr;           // instruction address (PC)
  uns64 opcode_lsb;    // raw instruction bytes, first 8B (was ctype_pin_inst.inst_binary_lsb)
  uns64 opcode_msb;    // raw instruction bytes, last 8B
  uns8 inst_size;      // x86 instruction length in bytes
  uns8 num_uop;        // number of uops this macro cracks into (<= STATIC_INST_MAX_UOPS)
  Addr branch_target;  // static (decoded) branch target, if any

  struct Static_Op_Info_struct* uops[STATIC_INST_MAX_UOPS];  // this macro's uops; num_uop valid entries

  uns16 true_op_type;  // opcode class from PIN (not for Scarab timing)
  char name[16];       // mnemonic

  Flag is_simd;
  uns8 num_simd_lanes;
  uns8 lane_width_bytes;
  Flag is_gather_scatter;

  Flag fake_inst;  // PIN-synthesized op (exceptions / uninstrumented code)
  Wrongpath_Nop_Mode_Reason fake_inst_reason;

  uns8 type;        // format type code (legacy; zeroed in PIN path but still read)
  uns8 qualifiers;  // FP qualifier bit string (legacy; zeroed in PIN path but still read)

  struct Static_Inst_Info_struct* free_list_next;  // fake-op pool free-list link (valid only while recycled)
} Static_Inst_Info;

/* Per-uop static info: one instance per uop. Interned by {addr, opcode bytes, uop_idx}. */
typedef struct Static_Op_Info_struct {
  uns uop_seq_num;  // index of this uop within its macro-instruction

  Op_Type op_type;
  Mem_Type mem_type;
  Cf_Type cf_type;
  Bar_Type bar_type;

  uns num_src_regs;
  uns num_dest_regs;
  Static_Reg_Info srcs[MAX_SRCS];    // runtime values live in Op::src_val
  Static_Reg_Info dests[MAX_DESTS];  // runtime values live in Op::dst_val

  uns mem_size;  // static bytes read/written (dynamic REP size lives in oracle_info)
  int latency;
  int extra_ld_latency;

  uns8 load_seq_num;  // 0 = first load uop of the macro, 1 = second, ...
  uns store_seq_num;

  Flag trigger_op_fetched_hook;  // fire the model's fetch hook for this uop
} Static_Op_Info;

#ifdef __cplusplus
extern "C" {
#endif

/* Fake (wrong-path-nop) ops cannot be interned by {addr, opcode bytes} -- their addresses are
 * synthesized and mostly unique -- so each one needs its own Static_Inst_Info, recycled through a
 * free list (same pattern as the Dynamic_Inst pool) instead of being malloc'd per op: fake ops are
 * 40-70% of all ops, so that churn dominated allocator time. The returned struct is uninitialized;
 * the caller copies a per-kind template over it. Their Static_Op_Info is shared, not allocated. */
Static_Inst_Info* alloc_fake_static_inst(void);
void free_fake_static_inst(Static_Inst_Info* si);

#ifdef __cplusplus
}
#endif

/* Does any uop of this macro do a load / store / control transfer? Derived from uops[] so there is
 * a single source of truth (num_uop is small, typically 1-3). */
static inline Flag static_inst_has_load(const Static_Inst_Info* inst) {
  for (uns i = 0; i < inst->num_uop; i++)
    if (inst->uops[i]->mem_type == MEM_LD)
      return TRUE;
  return FALSE;
}

static inline Flag static_inst_has_store(const Static_Inst_Info* inst) {
  for (uns i = 0; i < inst->num_uop; i++)
    if (inst->uops[i]->mem_type == MEM_ST)
      return TRUE;
  return FALSE;
}

static inline Flag static_inst_has_cf(const Static_Inst_Info* inst) {
  for (uns i = 0; i < inst->num_uop; i++)
    if (inst->uops[i]->cf_type != NOT_CF)
      return TRUE;
  return FALSE;
}

#endif /* #ifndef __STATIC_INST_INFO_H__ */
