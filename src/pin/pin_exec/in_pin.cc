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
 * File         : pin/pin_exec/in_pin.cc
 * Author       : Litz Lab
 * Description  : Scarab linked into the PIN tool. PIN (instrumented application) and
 *                scarab (its own simulation loop) are coroutines on one thread: each
 *                runs until it needs a message the other has not produced yet, then
 *                switches stacks in user space. No socket, no kernel context switch.
 ***************************************************************************************/

#include <deque>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <vector>

#include "pin.H"
#undef UNUSED  // name conflicts between PIN and scarab
#undef WARNING

#include "../pin_lib/message_queue_interface_lib.h"
#include "../pin_lib/pin_scarab_common_lib.h"

extern "C" int scarab_main(int argc, char* argv[], char* envp[]);
extern "C" const char* scarab_params_file;  // param_parser.c

KNOB<std::string> KnobScarabArgs(KNOB_MODE_WRITEONCE, "pintool", "scarab_args", "",
                                 "scarab's command line (scarab linked into the tool)");
KNOB<std::string> KnobScarabDir(KNOB_MODE_WRITEONCE, "pintool", "scarab_dir", ".",
                                "scarab's run directory: PARAMS.in and outputs");

// Saves the callee-saved registers and the FP control state (MXCSR, x87 control word) on the current
// stack, stores its stack pointer in *from and resumes the coroutine whose stack pointer is to.
extern "C" void in_pin_switch(void** from, void* to);
asm(R"(
  .text
  .globl in_pin_switch
  .type in_pin_switch, @function
in_pin_switch:
  pushq %rbp
  pushq %rbx
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  subq $16, %rsp
  stmxcsr 8(%rsp)
  fnstcw 12(%rsp)
  movq %rsp, (%rdi)
  movq %rsi, %rsp
  ldmxcsr 8(%rsp)
  fldcw 12(%rsp)
  addq $16, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbx
  popq %rbp
  ret
  .size in_pin_switch, .-in_pin_switch
)");

namespace {
const size_t SCARAB_STACK_SIZE = 256ul << 20;

std::deque<std::vector<char>> q_to_pin;  // scarab's commands
std::deque<ScarabOpBuffer_type> q_ops;
void* pin_sp;
void* scarab_sp;
bool scarab_started, scarab_done;
int scarab_rc;
std::string params_path;
std::vector<std::string> args;
std::vector<char*> argv;

// Entered by in_pin_switch's ret on scarab's fresh stack; never returns.
void scarab_entry() {
  std::istringstream ss(KnobScarabArgs.Value());
  args.push_back("scarab");
  for (std::string a; ss >> a;)
    args.push_back(a);
  args.push_back("--output_dir=" + KnobScarabDir.Value());
  for (std::string& a : args)
    argv.push_back(&a[0]);
  argv.push_back(nullptr);
  params_path = KnobScarabDir.Value() + "/PARAMS.in";
  scarab_params_file = params_path.c_str();
  char* envp[] = {nullptr};
  scarab_rc = scarab_main((int)args.size(), argv.data(), envp);
  scarab_done = true;
  void* unused;
  in_pin_switch(&unused, pin_sp);
  __builtin_unreachable();
}

void start_scarab() {
  char* stack = (char*)mmap(nullptr, SCARAB_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERTX(stack != MAP_FAILED);
  ASSERTX(mprotect(stack, 4096, PROT_NONE) == 0);  // guard page: an overflow faults
  // in_pin_switch restores the FP control state and six registers, then returns into scarab_entry,
  // which sees the stack alignment of a function entry (rsp = 8 mod 16)
  void** sp =
      (void**)(((uintptr_t)(stack + SCARAB_STACK_SIZE) & ~(uintptr_t)15) - 16 - 2 * sizeof(void*) - 6 * sizeof(void*));
  ((uint32_t*)sp)[2] = 0x1F80;  // MXCSR default
  ((uint16_t*)sp)[6] = 0x037F;  // x87 control word default
  for (int i = 2; i < 8; i++)
    sp[i] = nullptr;
  sp[8] = (void*)scarab_entry;
  scarab_sp = sp;
  scarab_started = true;
}

std::vector<char> pop(std::deque<std::vector<char>>& q) {
  std::vector<char> m = std::move(q.front());
  q.pop_front();
  return m;
}
}  // namespace

namespace in_pin {
void to_pin(const std::vector<char>& m) {
  q_to_pin.push_back(m);
}

void send_ops(ScarabOpBuffer_type& ops) {
  q_ops.emplace_back();
  q_ops.back().swap(ops);
}

void receive_ops(ScarabOpBuffer_type& ops) {
  while (q_ops.empty())
    in_pin_switch(&scarab_sp, pin_sp);
  ops.swap(q_ops.front());
  q_ops.pop_front();
}

std::vector<char> from_scarab() {
  if (!scarab_started)
    start_scarab();
  while (q_to_pin.empty()) {
    in_pin_switch(&pin_sp, scarab_sp);
    if (scarab_done)
      PIN_ExitApplication(scarab_rc);
  }
  return pop(q_to_pin);
}
}  // namespace in_pin
