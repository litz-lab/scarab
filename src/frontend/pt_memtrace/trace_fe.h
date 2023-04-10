/* Copyright 2020 University of California Santa Cruz
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
 * File         : frontend/trace_fe.h
 * Author       : Heiner Litz
 * Date         :
 * Description  :
 ***************************************************************************************/

#ifndef __TRACE_FE_H__
#define __TRACE_FE_H__

/**************************************************************************************/
/* Forward Declarations */

struct Trace_Uop_struct;
typedef struct Trace_Uop_struct Trace_Uop;
struct Op_struct;

/**************************************************************************************/
/* Prototypes */

#include <unordered_map>
#include "ctype_pin_inst.h"
#include "pin/pin_lib/uop_generator.h"
#include "pin/pin_lib/x86_decoder.h"


#ifdef __cplusplus
extern "C" {
#endif
  
#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "globals/global_types.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_TRACE_READ, ##args)

  /* Globals */
static ctype_pin_inst next_onpath_pi[MAX_NUM_PROCS];
static ctype_pin_inst next_offpath_pi[MAX_NUM_PROCS];
static bool            off_path_mode[MAX_NUM_PROCS] = {false};
static uint64_t        off_path_addr[MAX_NUM_PROCS] = {0};
static std::unordered_map<uint64_t, ctype_pin_inst> pc_to_inst;


void off_path_generate_inst(uns proc_id, uint64_t *off_path_addr, ctype_pin_inst *inst);

/* Implementing the frontend interface */
Addr ext_trace_next_fetch_addr(uns proc_id);
Flag ext_trace_can_fetch_op(uns proc_id);
void ext_trace_fetch_op(uns proc_id, Op* op);
void ext_trace_redirect(uns proc_id, uns64 inst_uid, Addr fetch_addr);
void ext_trace_recover(uns proc_id, uns64 inst_uid);
void ext_trace_retire(uns proc_id, uns64 inst_uid);
  void ext_trace_init();

#ifdef __cplusplus
}
#endif

#endif
