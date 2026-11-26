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
 * Description  : Static per-uop facts (Static_Op_Info), interned once by the uop decoder.
 *                The per-instruction static part is the frontend's ctype_pin_inst_static
 *                (op->inst); dynamic per-execution state lives on Op (see op.h).
 ***************************************************************************************/

#ifndef __STATIC_INST_INFO_H__
#define __STATIC_INST_INFO_H__

#include "globals/global_types.h"

#include "ctype_pin_inst.h"  // Reg_Type
#include "table_info.h"

/* Static register operand: id/type only. The runtime value lives in Op::src_val /
 * Op::dst_val (filled by the uop generator), never on the shared static struct. 6 B: every
 * interned Static_Op_Info holds MAX_SRCS + MAX_DESTS of these, so padding here is paid per
 * distinct uop -- over a million of them in a Google sim. */
typedef struct Static_Reg_Info_struct {
  uns16 reg;  // register number within the register set
  uns16 id;   // flattened register number (unique across sets)
  uns8 type;  // Reg_Type (integer, floating point, extra), stored in one byte
} Static_Reg_Info;

// Max uops per macro-instruction. Observed max is 7 (SPEC gcc); 16 leaves headroom. The decoder
// ASSERTs num_uop <= this and prints the offending PC, so if a bigger macro shows up we bump it.
#define STATIC_INST_MAX_UOPS 16

/* Per-uop static info: one instance per uop. Interned by {addr, opcode bytes, uop_idx, memory shape}. */
typedef struct Static_Op_Info_struct {
  uns uop_seq_num;  // index of this uop within its macro-instruction
  uns8 num_uop;     // uops this instance of the macro cracks into (per memory shape)

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

/* Fake (wrong-path-nop) ops and trace patches cannot share the decoded instruction at their PC --
 * their addresses are synthesized or they replace it -- so each op gets its own static part,
 * recycled through a free list (same pattern as the Dynamic_Inst pool): fake ops are 40-70% of all
 * ops, so per-op malloc churn dominated allocator time. The returned struct is uninitialized. */
ctype_pin_inst_static* alloc_op_static_inst(void);
void free_op_static_inst(ctype_pin_inst_static* st);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __STATIC_INST_INFO_H__ */
