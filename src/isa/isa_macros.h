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
 * File         : isa/isa_macros.h
 * Author       : HPS Research Group
 * Date         : 3/30/2000
 * Description  :
 ***************************************************************************************/

#ifndef __ISA_MACROS_H__
#define __ISA_MACROS_H__

/**************************************************************************************/
/* ISA definition macros */

#define NUM_INVALID_REGS 1
#define NUM_GENERAL_PURPOSE_REGS 16
#define NUM_SEGMENT_REGS 6
#define NUM_INST_PTR_REGS 1
#define NUM_FLAG_REGS 4
#define NUM_VECTOR_REGS 32
#define NUM_MASK_REGS 8
#define NUM_FP_REGS 8
#define NUM_FP_CTRL_REGS 2
#define NUM_TEMP_REGS 16
#define NUM_OTHER_REGS 1

#define NUM_REG_IDS                                                                                     \
  (NUM_INVALID_REGS + NUM_GENERAL_PURPOSE_REGS + NUM_SEGMENT_REGS + NUM_INST_PTR_REGS + NUM_FLAG_REGS + \
   NUM_VECTOR_REGS + NUM_MASK_REGS + NUM_FP_REGS + NUM_FP_CTRL_REGS + NUM_TEMP_REGS + NUM_OTHER_REGS)

#define IS_CALLSYS(tab) ((tab)->cf_type == CF_SYS)
#define IS_NOP(tab) ((tab)->op_type == OP_NOP)

/**************************************************************************************/

#endif /* #ifndef __ISA_MACROS_H__ */
