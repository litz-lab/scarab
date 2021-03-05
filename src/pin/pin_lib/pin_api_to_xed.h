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
 * File         : pin/pin_lib/pin_api_to_xed.h
 * Author       : Heiner Litz
 * Date         : 05/15/2020
 * Notes        : This code has been adapted from zsim which was released under
 *                GNU General Public License as published by the Free Software
 *                Foundation, version 2.
 * Description  :
 ***************************************************************************************/

/*
 * Wrapper file to enable support for different instruction decoders.
 * Currently we support PIN (execution driven) and Intel xed (trace driven)
 */

#ifndef THIRD_PARTY_ZSIM_SRC_WRAPPED_PIN_H_
#define THIRD_PARTY_ZSIM_SRC_WRAPPED_PIN_H_

extern "C" {
  #include "extras/xed-intel64/include/xed/xed-interface.h"

}

enum class CustomOp : uint8_t {
  NONE,
  PREFETCH_CODE
};

struct InstInfo {
  uint64_t pc;                    // instruction address
  const xed_decoded_inst_t *ins;  // XED info
  uint64_t pid;                   // process ID
  uint64_t tid;                   // thread ID
  uint64_t target;                // branch target
  uint64_t mem_addr[2];           // memory addresses
  bool mem_used[2];               // mem address usage flags
  CustomOp custom_op;             // Special or non-x86 ISA instruction
  bool taken;                     // branch taken
  bool unknown_type;              // No available decode info (presents a nop)
  bool valid;                     // True until the end of the sequence
};

#define INS InstInfo
#define REG xed_reg_enum_t

#define XED_OP_NAME(ins, op) xed_operand_name(xed_inst_operand(xed_decoded_inst_inst(ins), op))

#define INS_Nop(ins) (INS_Category(ins)) == XED_CATEGORY_NOP || INS_Category(ins) == XED_CATEGORY_WIDENOP)
#define INS_LEA(ins) (INS_Opcode(ins)) == XO(LEA))
#define INS_Opcode(ins) xed_decoded_inst_get_iclass(ins.ins)
#define INS_Category(ins) xed_decoded_inst_get_category(ins.ins)
#define INS_IsAtomicUpdate(ins) xed_decoded_inst_get_attribute(ins.ins), XED_ATTRIBUTE_LOCKED)
//FIXME: Check if REPs are translated correctly
#define INS_IsRep(ins) xed_decoded_inst_get_attribute((ins.ins), XED_ATTRIBUTE_REP)
#define INS_HasRealRep(ins) xed_operand_values_has_real_rep(xed_decoded_inst_operands((xed_decoded_inst_t *)ins.ins))
#define INS_OperandCount(ins) xed_decoded_inst_noperands(ins.ins)
#define INS_OperandIsImmediate(ins, op) XED_IS_IMM(ins, op)
#define INS_OperandRead(ins, op) xed_operand_read(xed_inst_operand(xed_decoded_inst_inst(ins.ins), op))
#define INS_OperandWritten(ins, op) INS_OperandIsMemory(ins, op) ? xed_decoded_inst_mem_written(ins.ins, op) : xed_operand_written(xed_inst_operand(xed_decoded_inst_inst(ins.ins), op))
#define INS_OperandIsReg(ins, op) xed_operand_is_register(xed_operand_name(xed_inst_operand(xed_decoded_inst_inst(ins.ins), op)))
#define INS_OperandIsMemory(ins, op) XED_MEM(ins, op)
//((xed_decoded_inst_mem_read(ins.ins, op) | xed_decoded_inst_mem_written(ins.ins, op)))
#define INS_IsMemory(ins) (xed_decoded_inst_number_of_memory_operands(ins.ins))
#define INS_OperandReg(ins, op) xed_decoded_inst_get_reg(ins.ins, xed_operand_name (xed_inst_operand(xed_decoded_inst_inst(ins.ins), op)))
#define INS_OperandMemoryBaseReg(ins, op) xed_decoded_inst_get_base_reg(ins.ins, op)
#define INS_OperandMemoryIndexReg(ins, op) xed_decoded_inst_get_index_reg(ins.ins, op)
#define INS_LockPrefix(ins) xed_decoded_inst_get_attribute(ins.ins, XED_ATTRIBUTE_LOCKED)
#define INS_OperandWidth(ins, op) xed_decoded_inst_operand_length_bits(ins.ins, op)
#define INS_MemoryOperandIsRead(ins, op) XED_MEM_READ(ins, op)
#define INS_MemoryOperandIsWritten(ins, op) XED_MEM_WRITTEN(ins, op)
#define INS_MemoryOperandCount(ins) xed_decoded_inst_number_of_memory_operands(ins.ins)
#define INS_IsDirectBranch(ins) xed3_operand_get_brdisp_width(ins.ins)
#define INS_Size(ins) xed_decoded_inst_get_length(ins.ins)
#define INS_Valid(ins) xed_decoded_inst_valid(ins.ins)
/* Just like PIN we break BBLs on a number of additional instructions such as REP */
#define INS_ChangeControlFlow(ins) (INS_Category(ins) == XC(COND_BR) || INS_Category(ins) == XC(UNCOND_BR) || INS_Category(ins) == XC(CALL) || INS_Category(ins) == XC(RET) || INS_Category(ins) == XC(SYSCALL) || INS_Category(ins) == XC(SYSRET) || INS_Opcode(ins) == XO(CPUID) || INS_Opcode(ins) == XO(POPF) || INS_Opcode(ins) == XO(POPFD) || INS_Opcode(ins) == XO(POPFQ) || INS_IsRep(ins))
#define REG_FullRegName(reg) xed_get_largest_enclosing_register(reg)

#define UINT32 uint32_t
#define INS_Mnemonic(ins) std::string(xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins.ins)))
#define INS_Address(ins) ins.pc
#define INS_NextAddress(ins) ins.target
#define XED_IS_REG(ins, op)(XED_OP_NAME(ins.ins, op) >= XED_OPERAND_REG0 && XED_OP_NAME(ins.ins, op) <= XED_OPERAND_REG8)
#define XED_MEM(ins, op)(XED_OP_NAME(ins.ins, op) == XED_OPERAND_MEM0 || XED_OP_NAME(ins.ins, op) == XED_OPERAND_MEM1)
#define XED_LEA(ins, op)(XED_OP_NAME(ins.ins, op) == XED_OPERAND_AGEN)
#define XED_IS_IMM(ins, op)(XED_OP_NAME(ins.ins, op) == XED_OPERAND_IMM0 || XED_OP_NAME(ins.ins, op) == XED_OPERAND_IMM1)
#define XED_MEM_READ(ins, op) xed_decoded_inst_mem_read(ins.ins, op)
#define XED_MEM_WRITTEN(ins, op) xed_decoded_inst_mem_written(ins.ins, op)
#define INS_MemoryReadSize(ins) (xed_operand_values_get_effective_operand_width(xed_decoded_inst_operands((xed_decoded_inst_t *)ins.ins)) / 8)
#define INS_MemoryWriteSize(ins) INS_MemoryReadSize(ins)
#define INS_IsRet(ins) (INS_Category(ins) == XED_CATEGORY_RET)
//TODO: Double check that below works calls and branches
#define INS_IsDirectBranchOrCall(ins) INS_IsDirectBranch(ins)
#define INS_IsIndirectBranchOrCall(ins) !INS_IsDirectBranchOrCall(ins)
#define INS_IsSyscall(ins) (INS_Category(ins) == XED_CATEGORY_SYSCALL)
#define INS_IsSysret(ins) (INS_Category(ins) == XED_CATEGORY_SYSRET)
#define INS_IsInterrupt(ins) (INS_Category(ins) == XED_CATEGORY_INTERRUPT)
#define INS_DirectBranchOrCallTargetAddress(ins) ins.pc + INS_Size(ins) + xed_operand_values_get_branch_displacement_int32(ins.ins)
#define INS_BranchIsTaken(ins) ins.taken

#define REG_GR_BASE REG_RDI
//Used in decoder.h. zsim (pin) requires REG_LAST whereas zsim_trace requires XED_REG_LAST
#define REG_LAST XED_REG_LAST

//XED expansion macros (enable us to type opcodes at a reasonable speed)
#define XC(cat) (XED_CATEGORY_##cat)
#define XO(opcode) (XED_ICLASS_##opcode)

#define ADDRINT uint64_t
#define THREADID uint32_t
#define BOOL bool

#endif  // THIRD_PARTY_ZSIM_SRC_WRAPPED_PIN_H_
