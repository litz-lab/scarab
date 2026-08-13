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

/* Per-x86-instruction static info: one instance per macro-instruction, shared by
 * every uop the macro cracks into. Interned by {addr, opcode bytes}. */
typedef struct Static_Inst_Info_struct {
  Addr addr;           // instruction address (PC)
  uns64 opcode_lsb;    // raw instruction bytes, first 8B (was ctype_pin_inst.inst_binary_lsb)
  uns64 opcode_msb;    // raw instruction bytes, last 8B
  uns8 inst_size;      // x86 instruction length in bytes
  uns8 num_uop;        // number of uops this macro cracks into
  Addr branch_target;  // static (decoded) branch target, if any

  Flag has_load;   // any contained uop is a load
  Flag has_store;  // any contained uop is a store
  Flag has_cf;     // any contained uop is a control-flow uop

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

#endif /* #ifndef __STATIC_INST_INFO_H__ */
