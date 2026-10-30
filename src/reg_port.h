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
 * File         : reg_port.h
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 10/2026
 * Description  : Register file read port arbitration, driven by the exec stage
 ***************************************************************************************/

#ifndef __REG_PORT_H__
#define __REG_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "stage_data.h"

/**************************************************************************************/
/* Constexpr */

enum RegPortRegClass {
  REG_PORT_REG_CLASS_GPR,  // REG_RAX - REG_CS
  REG_PORT_REG_CLASS_FLG,  // REG_ZPS, REG_OF, REG_CF, REG_DF
  REG_PORT_REG_CLASS_VEC,  // REG_ZMM0 - REG_K0
  REG_PORT_REG_CLASS_TMP,  // REG_TMP0 - REG_TMP4
  REG_PORT_REG_CLASS_NUM
};

/**************************************************************************************/
/* External Methods */

// return the class of a renamed architectural register
uns reg_port_get_reg_class(uns reg_id);

// return the bitmask of FU slots in src_sd whose ops fail to acquire register read ports
uns64 reg_port_arbitrate(Stage_Data* src_sd);

// vanilla api
void alloc_mem_reg_port(uns num_cores);
void set_reg_port(uns8 proc_id);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __REG_PORT_H__ */
