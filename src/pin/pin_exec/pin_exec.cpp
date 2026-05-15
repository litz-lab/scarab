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

#include <algorithm>
#include <cinttypes>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <syscall.h>
#include <unordered_map>

#undef UNUSED
#undef WARNING

#include "pin.H"

#undef UNUSED
#undef WARNING

#include "analysis_functions.h"
#include "exception_handling.h"
#include "globals.h"
#include "read_mem_map.h"
#include "scarab_interface.h"
#include "utils.h"

// scarab files
#include "../pin_lib/decoder.h"
#include "../pin_lib/message_queue_interface_lib.h"
#include "../pin_lib/pin_scarab_common_lib.h"

/* ===================================================================== */

/* ===================================================================== */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "o", "",
                            "specify file name for pintool output");

KNOB<string> KnobSocketPath(KNOB_MODE_WRITEONCE, "pintool", "socket_path",
                            "./pin_exec_driven_fe_socket.temp",
                            "specify socket path to communicate with Scarab");

KNOB<UINT32> KnobCoreId(KNOB_MODE_WRITEONCE, "pintool", "core_id", "0",
                        "The ID of the Scarab core to connect to");

KNOB<UINT32> KnobMaxBufferSize(
  KNOB_MODE_WRITEONCE, "pintool", "max_buffer_size", "8",
  "pintool buffers up to (max_buffer_size-2) instructions for sending");

KNOB<UINT64> KnobHyperFastForwardCount(
  KNOB_MODE_WRITEONCE, "pintool", "hyper_fast_forward_count", "0",
  "pin quickly skips close to hyper_ffc instructions");
KNOB<UINT64> KnobFastForwardCount(
  KNOB_MODE_WRITEONCE, "pintool", "fast_forward_count", "0",
  "After skipping hyper_ffc, pin skips exactly (ffc-1) instructions");
// TODO_b: why is this UINT64 instead of bool?
KNOB<UINT64> KnobFastForwardToStartInst(
  KNOB_MODE_WRITEONCE, "pintool", "fast_forward_to_start_inst", "0",
  "Pin skips instructions until start instruction is found");
KNOB<bool>   KnobHeartbeatEnabled(KNOB_MODE_WRITEONCE, "pintool", "heartbeat",
                                "false",
                                "Periodically output heartbeat messages");
KNOB<UINT64> KnobDebugPrintStartUid(
  KNOB_MODE_WRITEONCE, "pintool", "debug_print_start_uid", "0",
  "Start printing debug prints at this UID (inclusive)");
KNOB<UINT64> KnobDebugPrintEndUid(KNOB_MODE_WRITEONCE, "pintool",
                                  "debug_print_end_uid", "18446744073709551615",
                                  "Stop printing debug prints after this UID");
KNOB<UINT64> KnobStartRip(KNOB_MODE_WRITEONCE, "pintool", "rip", "0",
                          "the starting rip of the program");

KNOB<bool> KnobTrackAtInstrumentation(KNOB_MODE_WRITEONCE, "pintool", "track_at_instr", "true",
                                      "Track RIP at instrumentation time instead of execution time");

KNOB<string> KnobH2PTargetPC(KNOB_MODE_WRITEONCE, "pintool", "h2p_target_pc",
                             "0", "Target PC for H2P branch reg snapshot");
/* ===================================================================== */
/* ===================================================================== */

namespace {

INT32 Usage() {
  cerr << "Pintool based exec frontend for scarab simulator" << endl << endl;
  cerr << KNOB_BASE::StringKnobSummary() << endl;
  return -1;
}

// ==================== H2P reg snapshot logging ====================
static constexpr size_t H2P_WINDOW = 1000;

struct InstSnapshot {
  uint64_t uid;
  uint64_t pc;
  uint64_t rax, rbx, rcx, rdx;
  uint64_t rsi, rdi, rbp, rsp;
  uint64_t r8,  r9,  r10, r11;
  uint64_t r12, r13, r14, r15;
  uint64_t rflags;
};

static InstSnapshot h2p_ring[H2P_WINDOW];
static size_t   h2p_ring_head = 0;
static uint64_t h2p_ring_fill = 0;
static FILE*    h2p_fp = nullptr;

// H2P PCs — hardcoded array (from h2p_branches.csv)
static uint64_t h2p_target_pc = 0;

static bool is_h2p_pc(uint64_t pc) {
  return h2p_target_pc != 0 && pc == h2p_target_pc;
}

static void init_h2p_pcs() {
  // no-op: static array is initialized at compile time
  h2p_target_pc = strtoull(KnobH2PTargetPC.Value().c_str(), nullptr, 0);
}

static uint64_t last_recorded_uid = 0;

static void record_inst_snapshot(
    ADDRINT ip,
    ADDRINT rax, ADDRINT rbx, ADDRINT rcx, ADDRINT rdx,
    ADDRINT rsi, ADDRINT rdi, ADDRINT rbp, ADDRINT rsp,
    ADDRINT r8,  ADDRINT r9,  ADDRINT r10, ADDRINT r11,
    ADDRINT r12, ADDRINT r13, ADDRINT r14, ADDRINT r15,
    ADDRINT rflags) {
// static void record_inst_snapshot(
//     ADDRINT ip,
//     ADDRINT rax, ADDRINT rdx, ADDRINT rdi) {
  if (on_wrongpath) return;
  if (hyper_ff) return;
  if (fast_forward_count > 0) return;

  if (uid_ctr <= last_recorded_uid) return;
  last_recorded_uid = uid_ctr;

  InstSnapshot& s = h2p_ring[h2p_ring_head];
  s.uid = uid_ctr;
  s.pc  = (uint64_t)ip;
  s.rax = rax; s.rbx = rbx; s.rcx = rcx; s.rdx = rdx;
  s.rsi = rsi; s.rdi = rdi; s.rbp = rbp; s.rsp = rsp;
  s.r8  = r8;  s.r9  = r9;  s.r10 = r10; s.r11 = r11;
  s.r12 = r12; s.r13 = r13; s.r14 = r14; s.r15 = r15;
  s.rflags = rflags;
  // s.uid    = uid_ctr;
  // s.pc     = (uint64_t)ip;
  // s.rax    = (uint64_t)rax; 
  // s.rdx    = (uint64_t)rdx;
  // s.rdi    = (uint64_t)rdi; 

  h2p_ring_head = (h2p_ring_head + 1) % H2P_WINDOW;
  h2p_ring_fill++;
}

static void dump_branch_window(ADDRINT ip, BOOL taken) {
  if (on_wrongpath) return;
  if (hyper_ff) return;
  if (fast_forward_count > 0) return;
  if (!is_h2p_pc((uint64_t)ip)) return;

  static uint64_t last_dumped_uid = 0;
  if (uid_ctr <= last_dumped_uid) return;
  last_dumped_uid = uid_ctr;

  if (!h2p_fp) {
    string out_path = KnobOutputFile.Value();
    string h2p_path = "branch_context.txt";
    size_t slash = out_path.find_last_of('/');
    if (slash != string::npos) {
        h2p_path = out_path.substr(0, slash + 1) + "branch_context.txt";
    }
    h2p_fp = fopen(h2p_path.c_str(), "w");

    if (!h2p_fp) return;
    fprintf(h2p_fp,
      "branch_uid,branch_pc,taken,delta,past_uid,past_pc,"
      "rax,rbx,rcx,rdx,rsi,rdi,rbp,rsp,"
      "r8,r9,r10,r11,r12,r13,r14,r15,rflags\n");
    // fprintf(h2p_fp,
    //   "branch_uid,branch_pc,taken,delta,past_uid,past_pc,"
    //   "rax,rdi,rdx\n");
  }
  if (!h2p_fp) return;

  size_t valid = (h2p_ring_fill < H2P_WINDOW) ? h2p_ring_fill : H2P_WINDOW;
  if (valid == 0) return;

  for (size_t i = 0; i < valid; i++) {
    size_t idx = (h2p_ring_head + H2P_WINDOW - valid + i) % H2P_WINDOW;
    const InstSnapshot& s = h2p_ring[idx];
    int delta = -(int)(valid - i);

    fprintf(h2p_fp,
      "%llu,0x%llx,%d,%d,%llu,0x%llx,"
      "0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,"
      "0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx\n",
      (unsigned long long)uid_ctr,
      (unsigned long long)ip,
      (int)taken, delta,
      (unsigned long long)s.uid, (unsigned long long)s.pc,
      (unsigned long long)s.rax, (unsigned long long)s.rbx,
      (unsigned long long)s.rcx, (unsigned long long)s.rdx,
      (unsigned long long)s.rsi, (unsigned long long)s.rdi,
      (unsigned long long)s.rbp, (unsigned long long)s.rsp,
      (unsigned long long)s.r8,  (unsigned long long)s.r9,
      (unsigned long long)s.r10, (unsigned long long)s.r11,
      (unsigned long long)s.r12, (unsigned long long)s.r13,
      (unsigned long long)s.r14, (unsigned long long)s.r15,
      (unsigned long long)s.rflags);
    // fprintf(h2p_fp,
    //   "%llu,0x%llx,%d,%d,%llu,0x%llx,"
    //   "0x%llx,0x%llx,0x%llx\n",
    //   (unsigned long long)uid_ctr,
    //   (unsigned long long)ip,
    //   (int)taken, delta,
    //   (unsigned long long)s.uid, (unsigned long long)s.pc,
    //   (unsigned long long)s.rax, (unsigned long long)s.rdi,
    //   (unsigned long long)s.rdx);
  }
}
// ==================================================================

void insert_logging(const INS& ins) {
  if(INS_Category(ins) == XED_CATEGORY_COND_BR) {
    INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(logging), IARG_ADDRINT,
                   INS_NextAddress(ins), IARG_ADDRINT, INS_Address(ins),
                   IARG_BOOL, INS_HasFallThrough(ins), IARG_BRANCH_TAKEN,
                   IARG_END);
  } else {
    INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(logging), IARG_ADDRINT,
                   INS_NextAddress(ins), IARG_ADDRINT, INS_Address(ins),
                   IARG_BOOL, INS_HasFallThrough(ins), IARG_BOOL, false,
                   IARG_END);
  }
}

void insert_check_for_magic_instructions(const INS& ins) {
  if(INS_IsXchg(ins) && INS_OperandReg(ins, 0) == REG_GCX &&
     INS_OperandReg(ins, 1) == REG_GCX) {
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)handle_scarab_marker,
                   IARG_REG_VALUE, REG_RCX, IARG_END);
  }
}

void insert_processing_for_syscalls(const INS& ins) {
  INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(process_syscall), IARG_INST_PTR,
                 IARG_SYSCALL_NUMBER, IARG_SYSARG_VALUE, 0, IARG_SYSARG_VALUE,
                 1, IARG_SYSARG_VALUE, 2, IARG_SYSARG_VALUE, 3,
                 IARG_SYSARG_VALUE, 4, IARG_SYSARG_VALUE, 5, IARG_CONTEXT,
                 IARG_BOOL, INS_IsSyscall(ins), IARG_END);
}

void insert_checks_for_control_flow(const INS& ins) {
  if(INS_IsRet(ins)) {
    INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(check_ret_control_ins),
                   IARG_MEMORYREAD_EA, IARG_MEMORYREAD_SIZE, IARG_CONTEXT,
                   IARG_END);
  } else if(INS_IsBranchOrCall(ins)) {
    if(INS_IsDirectBranchOrCall(ins)) {
      if(INS_Category(ins) == XED_CATEGORY_COND_BR) {
        INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(check_nonret_control_ins),
                       IARG_BRANCH_TAKEN, IARG_ADDRINT,
                       INS_DirectBranchOrCallTargetAddress(ins), IARG_END);
      } else {
        INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(check_nonret_control_ins),
                       IARG_BOOL, true, IARG_ADDRINT,
                       INS_DirectBranchOrCallTargetAddress(ins), IARG_END);
      }
    } else if(INS_IsMemoryRead(ins)) {
      INS_InsertCall(ins, IPOINT_BEFORE,
                     AFUNPTR(check_nonret_control_mem_target), IARG_BOOL, true,
                     IARG_MEMORYREAD_EA, IARG_MEMORYREAD_SIZE, IARG_END);
    } else if(INS_MaxNumRRegs(ins) > 0) {
      INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(check_nonret_control_ins),
                     IARG_BOOL, true, IARG_REG_VALUE, INS_RegR(ins, 0),
                     IARG_END);
    } else {
      // Force WPNM
      INS_InsertCall(ins, IPOINT_BEFORE,
                     AFUNPTR(check_nonret_control_mem_target), IARG_BOOL, true,
                     IARG_ADDRINT, 0, IARG_UINT32, 0, IARG_END);
    }
  }
}

void insert_processing_for_nonsyscall_instructions(const INS& ins) {
  if(!INS_IsMemoryWrite(ins)) {
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)before_ins_no_mem, IARG_CONTEXT,
                   IARG_END);
  } else {
    if(INS_hasKnownMemorySize(ins)) {
      // Single memory op
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)before_ins_one_mem,
                     IARG_CONTEXT, IARG_MEMORYWRITE_EA, IARG_MEMORYWRITE_SIZE,
                     IARG_END);
    } else {
      // Multiple memory ops
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)before_ins_multi_mem,
                     IARG_CONTEXT, IARG_MULTI_MEMORYACCESS_EA, IARG_BOOL,
                     INS_IsVscatter(ins), IARG_END);
    }
  }
}

void instrumentation_func_per_trace(TRACE trace, void* v) {
#ifdef DEBUG_PRINT
  stringstream instructions_ss;

  for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
    for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
      instructions_ss << "0x" << hex << INS_Address(ins) << endl;
    }
  }

  DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
            "Instrumenting Trace at address 0x%p. Instructions:\n%s\n",
            (void*)TRACE_Address(trace), instructions_ss.str().c_str());
#endif

  // used to be IPOINT_ANYWHERE
  if(hyper_ff) {
    for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
      UINT32 non_rep_count = 0;
      for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
        if (INS_HasRealRep(ins)) {
          INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)docount_rep, IARG_FAST_ANALYSIS_CALL, IARG_INST_PTR, IARG_END);
        } else {
          non_rep_count++;
        }
      }

      if (non_rep_count > 0) {
        BBL_InsertCall(bbl, IPOINT_BEFORE, (AFUNPTR)docount, IARG_FAST_ANALYSIS_CALL, IARG_UINT32, non_rep_count,
                       IARG_END);
      }
    }
  }
}

void track_rip_at_execution(ADDRINT ip) {
  if (!on_wrongpath) {
    instrumented_rip_tracker.insert(ip);
  }
}

void instrumentation_func_per_instruction(INS ins, void* v) {
  if(!started) {
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)redirect, IARG_CONTEXT,
                   IARG_END);
  } else {
    if(!hyper_ff) {
      if (track_at_instr)
        instrumented_rip_tracker.insert(INS_Address(ins));
      else
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)track_rip_at_execution, IARG_INST_PTR, IARG_END);

      DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
                "Instrument from Instruction() eip=%" PRIx64 "\n",
                (uint64_t)INS_Address(ins));

      // ==================== H2P snapshot ====================
      if (INS_Category(ins) == XED_CATEGORY_COND_BR) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)dump_branch_window,
                       IARG_CALL_ORDER, CALL_ORDER_FIRST + 10,
                       IARG_INST_PTR,
                       IARG_BRANCH_TAKEN,
                       IARG_END);
      }

      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)record_inst_snapshot,
                     IARG_CALL_ORDER, CALL_ORDER_FIRST + 20,
                     IARG_INST_PTR,
                     IARG_REG_VALUE, REG_RAX, IARG_REG_VALUE, REG_RBX,
                     IARG_REG_VALUE, REG_RCX, IARG_REG_VALUE, REG_RDX,
                     IARG_REG_VALUE, REG_RSI, IARG_REG_VALUE, REG_RDI,
                     IARG_REG_VALUE, REG_RBP, IARG_REG_VALUE, REG_RSP,
                     IARG_REG_VALUE, REG_R8,  IARG_REG_VALUE, REG_R9,
                     IARG_REG_VALUE, REG_R10, IARG_REG_VALUE, REG_R11,
                     IARG_REG_VALUE, REG_R12, IARG_REG_VALUE, REG_R13,
                     IARG_REG_VALUE, REG_R14, IARG_REG_VALUE, REG_R15,
                     IARG_REG_VALUE, REG_RFLAGS,
                     IARG_END);
      // INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)record_inst_snapshot,
      //           IARG_CALL_ORDER, CALL_ORDER_FIRST + 20,
      //           IARG_INST_PTR,
      //           IARG_REG_VALUE, REG_RAX, IARG_REG_VALUE, REG_RDX,
      //           IARG_REG_VALUE, REG_RDI,
      //           IARG_END);
      // ======================================================

      insert_logging(ins);
      insert_check_for_magic_instructions(ins);

      // Inserting functions to create a compressed op
      pin_decoder_insert_analysis_functions(ins);

      xed_decoded_inst_t* xed_ins = INS_XedDec(ins);
      if(INS_IsSyscall(ins) || is_ifetch_barrier(xed_ins)) {
        insert_processing_for_syscalls(ins);
      } else {
        insert_checks_for_control_flow(ins);
        insert_processing_for_nonsyscall_instructions(ins);
      }

#ifdef DEBUG_PRINT
      stringstream ss;
      if(INS_IsDirectBranchOrCall(ins)) {
        ss << "0x" << hex << INS_DirectBranchOrCallTargetAddress(ins);
      } else {
        ss << "(not a direct branch or call)";
      }

      DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
                "Leaving Instrument from Instruction() eip=%" PRIx64
                ", %s, direct target: %s\n",
                (uint64_t)INS_Address(ins), INS_Mnemonic(ins).c_str(),
                ss.str().c_str());
#endif
    }
  }
}

}  // namespace

void Fini(INT32 code, void* v) {
  DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
            "Fini reached, app exit code=%d\n.", code);
  *out << "End of program reached, disconnect from Scarab.\n" << endl;
  if (h2p_fp) {
    fflush(h2p_fp);
    fclose(h2p_fp);
    h2p_fp = nullptr;
    *out << "[H2P] branch_context.csv closed" << endl;
  }
  scarab->disconnect();
  *out << "Pintool Fini Reached.\n" << endl;
}

int main(int argc, char* argv[]) {
#if DEBUG_PRINT
  setbuf(stdout, NULL);
#endif

  // Read memmap for process
  page_table = new pageTableStruct();
  update_page_table(page_table);

  if(PIN_Init(argc, argv)) {
    return Usage();
  }
  // if no start EIP was specified, then we don't need to redirect,
  // and so we have "started"
  started   = (0 == KnobStartRip.Value());
  start_rip = KnobStartRip.Value();

  track_at_instr = KnobTrackAtInstrumentation.Value();

  heartbeat_enabled = KnobHeartbeatEnabled.Value();
  max_buffer_size   = KnobMaxBufferSize.Value();

  fast_forward_count = KnobFastForwardCount.Value();
  fast_forward_to_pin_start =
    (fast_forward_count = KnobFastForwardToStartInst.Value());
  hyper_fast_forward_count = KnobHyperFastForwardCount.Value() -
                             hyper_fast_forward_delta;
  orig_hyper_fast_forward_count = KnobHyperFastForwardCount.Value();

  dbg_print_start_uid = KnobDebugPrintStartUid.Value();
  dbg_print_end_uid   = KnobDebugPrintEndUid.Value();

  register_signal_handlers();

  hyper_ff = false;
  if(hyper_fast_forward_count > 0) {
    hyper_ff = true;
    *out << "Entering Hyper Fast Forward Mode: " << hyper_fast_forward_count
         << " ins remaining" << endl;
  } else if(fast_forward_count > 0) {
    if(fast_forward_to_pin_start) {
      *out << "Entering Fast Forward Mode: looking for start instruction"
           << endl;
    } else {
      *out << "Entering Fast Forward Mode: " << fast_forward_count
           << " ins remaining" << endl;
    }
  }

  max_buffer_size = KnobMaxBufferSize.Value();
  string fileName = KnobOutputFile.Value();

  if(!fileName.empty()) {
    out = new std::ofstream(fileName.c_str());
  }

  pin_decoder_init(true, out);

  // Register function to be called to instrument traces
  TRACE_AddInstrumentFunction(instrumentation_func_per_trace, 0);
  INS_AddInstrumentFunction(instrumentation_func_per_instruction, 0);

  // Register function to be called when the application exits
  PIN_AddFiniFunction(Fini, 0);

  scarab = new Client(KnobSocketPath, KnobCoreId);

  init_h2p_pcs();
  *out << "[H2P] init_h2p_pcs() called" << endl;

  // Start the program, never returns
  PIN_StartProgram();
  return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
