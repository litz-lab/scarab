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
 * File         : dyn_inst.h
 * Description  : Per-dynamic-instance macro-instruction grouping of its uop ops.
 ***************************************************************************************/

#ifndef __DYN_INST_H__
#define __DYN_INST_H__

#include "globals/global_types.h"

#include "static_inst_info.h"  // STATIC_INST_MAX_UOPS

struct Op_struct;

/* One dynamic execution of a macro-instruction: pointers to the dynamic uop ops it cracked into,
 * shared by all of those uops. Pool-allocated when the macro begins building in an FT and recycled
 * when its end-of-macro (last) uop is freed -- which, because uops retire/free in order, is the last
 * of them to go. Lets any uop reach its siblings -- e.g. the eom -- directly, without walking the FT.
 * Distinct from Static_Inst_Info, which is interned/shared across every dynamic instance of the same
 * PC (and already holds the uop count, so it is not duplicated here). */
typedef struct Dynamic_Inst_struct {
  struct Op_struct* uops[STATIC_INST_MAX_UOPS];  // the dynamic uop ops, indexed by uop_seq_num
  struct Dynamic_Inst_struct* free_list_next;    // pool free-list link (valid only while recycled)
} Dynamic_Inst;

#ifdef __cplusplus
extern "C" {
#endif

// Allocate a fresh, zeroed instance for a macro (all uop slots cleared).
Dynamic_Inst* alloc_dyn_inst(void);
// Link op <-> di: record op at its uop_seq_num slot.
void dyn_inst_attach(Dynamic_Inst* di, struct Op_struct* op);
// Recycle the instance to the pool (called when its eom uop is freed).
void free_dyn_inst(Dynamic_Inst* di);

#ifdef __cplusplus
}
#endif

#endif  // __DYN_INST_H__
