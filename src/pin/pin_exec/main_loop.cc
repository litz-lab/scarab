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

#include "main_loop.h"

#include "scarab_interface.h"

#include "../pin_lib/decoder.h"

namespace {
INT64 checkpoint_index(UINT64 uid);
void undo_mem(const ProcState& undo_state) {
  for(uint i = 0; i < undo_state.num_mem_state; i++) {
    void*  write_addr = (void*)undo_state.mem_state_list[i].mem_addr;
    UINT32 write_size = undo_state.mem_state_list[i].mem_size;
    void*  prev_data  = undo_state.mem_state_list[i].mem_data_ptr;
    PIN_SafeCopy(write_addr, prev_data, write_size);
  }
}

void recover_to_past_checkpoint(UINT64 uid, bool is_redirect_recover,
                                bool enter_ff) {
  capture_pending = false;  // PIN no longer stands right after that op
  INT64 idx = checkpoints.get_tail_index();
  bool redirect_ctxt_found = false;

  scarab_clear_all_buffers();

  while(!checkpoints.empty()) {
    if(checkpoints[idx].uid == uid) {
      // Resumes execution at the saved context
      // reset uid counter to one past the recovered uid
      uid_ctr = uid + 1;

      ADDRINT this_eip;

      on_wrongpath          = checkpoints[idx].wrongpath;
      on_wrongpath_nop_mode = checkpoints[idx].wrongpath_nop_mode;
      generate_dummy_nops   = (generate_dummy_nops && on_wrongpath_nop_mode);
      // A branch's POST context already includes its own execution (and memory writes):
      // redirect from it, or resume after it without re-executing it.
      const bool post = checkpoints[idx].ctxt_kind == CTXT_POST;
      if (post && !on_wrongpath_nop_mode)
        PIN_SaveContext(&(checkpoints[idx].ctxt), &last_ctxt);
      if(is_redirect_recover) {
        ASSERTM(0, post || on_wrongpath_nop_mode || redirect_ctxt_found,
                "Redirect: no context after uid %" PRIu64 " (wrongpath=%d, ctxt_kind=%d, wpnm_eip=%" PRIx64 ")\n", uid,
                on_wrongpath, checkpoints[idx].ctxt_kind, (uint64_t)checkpoints[idx].wpnm_eip);
        return;
      } else if (post && !on_wrongpath_nop_mode) {
        ASSERTM(0, enter_ff, "Recover before uid %" PRIu64 " needs a pre-execution context\n", uid);
        trace_cause = TC_EXECUTE_AT, execute_at_calls++;
        PIN_ExecuteAt(&last_ctxt);
        ASSERTM(0, false, "PIN_ExecuteAt did not redirect execution.\n");
      } else {
        undo_mem(checkpoints[idx]);

        if(!on_wrongpath_nop_mode) {
          ASSERTM(0, checkpoints[idx].ctxt_kind == CTXT_PRE, "Recover: no context for uid %" PRIu64 "\n", uid);
          PIN_SaveContext(&(checkpoints[idx].ctxt), &last_ctxt);
          if(enter_ff) {
            excp_ff = true;
            fast_forward_count += 2;
            // pin skips (ffc - 1) instructions
          } else {
            idx = checkpoints.remove_from_cir_buf_tail();
          }
          trace_cause = TC_EXECUTE_AT, execute_at_calls++;
          PIN_ExecuteAt(&last_ctxt);
          ASSERTM(0, false, "PIN_ExecuteAt did not redirect execution.\n");
        } else {
          ADDRINT prev_eip = checkpoints[idx].wpnm_eip;
          if (checkpoints[idx].ctxt_kind == CTXT_PRE)
            PIN_GetContextRegval(&(checkpoints[idx].ctxt), REG_INST_PTR, (UINT8*)&prev_eip);
          this_eip = checkpoints[idx].wpnm_eip;
          next_eip = ADDR_MASK(enter_ff ? this_eip : prev_eip);
          return;
        }
      }
      ASSERTM(0, false,
              "Recover: found uid %" PRIu64 ", but eip not changed.\n", uid);
    }

    undo_mem(checkpoints[idx]);

    if (is_redirect_recover && checkpoints[idx].uid == uid + 1 && checkpoints[idx].ctxt_kind == CTXT_PRE) {
      PIN_SaveContext(&(checkpoints[idx].ctxt), &last_ctxt);
      redirect_ctxt_found = true;
    }

    idx = checkpoints.remove_from_cir_buf_tail();
  }
  ASSERTM(0, false, "Checkpoint %" PRIu64 " not found. \n", uid);
}

void redirect_to_inst(ADDRINT inst_addr, UINT64 uid) {
  recover_to_past_checkpoint(uid, true, false);
  scarab_clear_all_buffers();
  PIN_SetContextRegval(&last_ctxt, REG_INST_PTR, (const UINT8*)(&inst_addr));

  on_wrongpath = true;  // redirect ALWAYS sets to wrongpath==true
  if(on_wrongpath && !instrumented_rip_tracker.contains(inst_addr)) {
    on_wrongpath_nop_mode     = true;
    wrongpath_nop_mode_reason = WPNM_REASON_REDIRECT_TO_NOT_INSTRUMENTED;
    DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
              "Entering from redirect WPNM targetaddr=%" PRIx64 "\n",
              (uint64_t)inst_addr);
  }
  if(on_wrongpath_nop_mode) {
    next_eip = ADDR_MASK(inst_addr);
  } else {
    trace_cause = TC_EXECUTE_AT, execute_at_calls++;
    PIN_ExecuteAt(&last_ctxt);
    ASSERTM(0, false, "PIN_ExecuteAt did not redirect execution.\n");
  }
}

// Retire all instrution upto and including uid
void retire_older_checkpoints(UINT64 uid) {
  INT64 idx       = checkpoints.get_head_index();
  bool  found_uid = false;
  while(!checkpoints.empty()) {
    if(checkpoints[idx].uid <= uid) {
      ASSERTM(0, !checkpoints[idx].wrongpath,
              "Tried to retire wrongpath op %" PRIu64 ".\n", uid);
      if(checkpoints[idx].unretireable_instruction) {
        ADDRINT eip;
        PIN_GetContextRegval(&(checkpoints[idx].ctxt), REG_INST_PTR,
                             (UINT8*)&eip);
        ASSERTM(0, false,
                "Exception by program caused at address 0x%" PRIx64 "\n",
                (uint64_t)eip);
      }
      if(checkpoints[idx].uid == uid) {
        found_uid = true;
      }
      idx = checkpoints.remove_from_cir_buf_head();
    } else {
      break;
    }
  }
  ASSERTM(0, found_uid, "Checkpoint %" PRIu64 " not found. \n", uid);
}

bool do_fe_recover(Scarab_To_Pin_Msg& cmd) {
  DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
            "recover curr_uid=%" PRIu64 ", target_uid=%" PRIu64 "\n", uid_ctr,
            cmd.inst_uid);
  if(pending_syscall && cmd.inst_uid == (uid_ctr - 1)) {
    ASSERTM(0, false,
            "Unexpected Recover to current syscall inst @uid=%" PRIu64 "\n",
            uid_ctr - 1);
  }
  seen_rightpath_exc_mode = false;
  pending_syscall         = false;
  pending_exception       = false;
  buffer_sentinel         = false;
  bool enter_ff           = false;
  if(cmd.type == FE_RECOVER_BEFORE) {
    enter_ff = false;
  } else if(cmd.type == FE_RECOVER_AFTER) {
    enter_ff = true;
  } else {
    ASSERTM(0, false, "Unknown Recover Type (uid=%" PRIu64 ")\n", uid_ctr - 1);
  }
  recover_to_past_checkpoint(cmd.inst_uid, false, enter_ff);
  if(on_wrongpath_nop_mode) {
    wpnm_skip_ckp = true;
  } else {
    ASSERTM(0, false,
            "Recover cmd did not change execution (uid=%" PRIu64 ")\n",
            uid_ctr);
  }
  return on_wrongpath_nop_mode;
}

bool do_fe_redirect(Scarab_To_Pin_Msg& cmd) {
  DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
            "redirect curr_uid=%" PRIu64 ", target_uid=%" PRIu64
            ", target_eip=%llx\n",
            uid_ctr, cmd.inst_uid, cmd.inst_addr);
  if(pending_syscall && cmd.inst_uid == (uid_ctr - 1)) {
    ASSERTM(0, false,
            "Unexpected Redirect to current syscall inst @uid=%" PRIu64 "\n",
            uid_ctr - 1);
  }
  seen_rightpath_exc_mode = false;
  pending_syscall         = false;
  pending_exception       = false;
  buffer_sentinel         = false;
  if (lazy_ctxt && checkpoints[checkpoint_index(cmd.inst_uid)].ctxt_kind == CTXT_NONE &&
      !checkpoints[checkpoint_index(cmd.inst_uid)].wrongpath_nop_mode) {
    // PIN waits right after the op (always a basic-block head): do_pending_redirect captures the live context
    ASSERTM(0, at_capture_site, "Redirect to uid %" PRIu64 " without a context, away from a capture site\n",
            cmd.inst_uid);
    redirect_pending = true;
    pending_redirect = cmd;
    return true;
  }
  redirect_to_inst(cmd.inst_addr, cmd.inst_uid);
  if(on_wrongpath_nop_mode) {
    if(entered_wpnm) {
      wpnm_skip_ckp = true;
    }
  } else {
    ASSERTM(0, false,
            "Redirect cmd did not change execution (uid=%" PRIu64 ")\n",
            uid_ctr);
  }
  return on_wrongpath_nop_mode;
}

}  // namespace

void finish_before_ins_all(bool from_syscall);  // analysis_functions.cc

// The redirected control-flow op ended its batch (scarab_buffer_full), so PIN has not executed past it:
// the live context is its post-execution state, which a later recovery to it resumes from.
namespace {
INT64 checkpoint_index(UINT64 uid) {
  ASSERTM(0, !checkpoints.empty() && checkpoints[checkpoints.get_head_index()].uid <= uid &&
               uid <= checkpoints.get_tail().uid,
          "uid %" PRIu64 " not checkpointed (%s, head uid %" PRIu64 ", tail uid %" PRIu64 ", capture %d redirect %d)\n",
          uid, checkpoints.empty() ? "empty" : "range", checkpoints.empty() ? 0 : checkpoints[checkpoints.get_head_index()].uid,
          checkpoints.empty() ? 0 : checkpoints.get_tail().uid, capture_pending, redirect_pending);
  INT64 idx = checkpoints.get_tail_index();
  while (checkpoints[idx].uid != uid)
    idx--;  // CirBuf asserts on an index below the head: the uid must still be checkpointed
  return idx;
}

// Nothing after it has executed (ASSERTed), so the live context is the op's post-execution state.
// tail_unexecuted: the current instruction already has its checkpoint but has not run yet.
void capture_post_context(INT64 idx, const CONTEXT* ctxt, bool tail_unexecuted) {
  ProcState& rec = checkpoints[idx];
  if (rec.ctxt_kind != CTXT_NONE || rec.wrongpath_nop_mode)
    return;
  for (INT64 i = idx + 1; i <= checkpoints.get_tail_index(); i++)
    ASSERTM(0, checkpoints[i].wrongpath_nop_mode || checkpoints[i].ctxt_kind == CTXT_PRE ||
                 (tail_unexecuted && i == checkpoints.get_tail_index()),
            "Context for uid %" PRIu64 ": uid %" PRIu64 " after it already executed\n", rec.uid, checkpoints[i].uid);
  PIN_SaveContext(ctxt, &rec.ctxt);
  rec.ctxt_kind = CTXT_POST;
}
}  // namespace

void do_pending_redirect(const CONTEXT* ctxt) {
  if (capture_pending) {
    capture_pending = false;
    capture_post_context(checkpoint_index(capture_uid), ctxt, !redirect_pending);
  }
  if (!redirect_pending)
    return;
  redirect_pending = false;
  const UINT64 uid = pending_redirect.inst_uid;
  const INT64  idx = checkpoint_index(uid);
  capture_post_context(idx, ctxt, false);
  redirect_to_inst(pending_redirect.inst_addr, uid);  // PIN_ExecuteAt unless the target is in WPNM
  // WPNM: this instruction does not execute; continue as its deferred analysis call would have
  next_eip = ADDR_MASK(next_eip);
  checkpoints.append_to_cir_buf();
  checkpoints.get_tail().init(uid_ctr, false, on_wrongpath, on_wrongpath_nop_mode, next_eip, 0);
  uid_ctr++;
  finish_before_ins_all(false);
}

void main_loop() {
  trace_cause = TC_FLOW;  // diagnostics: execution reached analysis code
  bool buffer_ready                    = false;
  bool syscall_has_been_sent_to_scarab = false;
  bool have_consumed_op                = false;
  bool need_scarab_cmd                 = false;
  // lazy_ctxt: a batch ending at a control-flow op was sent; do not run past it until scarab redirects
  // or asks for more ops (it passed the op)
  bool   awaiting_cf_decision = false;
  UINT64 undecided_cf_uid     = 0;
  compressed_op undecided_op  = {};

  DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
            "main loop next_eip=%" PRIx64 "\n", (uint64_t)next_eip);

  while(true) {
    Scarab_To_Pin_Msg cmd = {};
    cmd.type = FE_NULL;
    if(excp_rewind_msg) {
      excp_rewind_msg = false;
      cmd             = saved_cmd;
    } else if(need_scarab_cmd) {
      cmd = get_scarab_cmd();
    }

    if(cmd.type == FE_RECOVER_BEFORE || cmd.type == FE_RECOVER_AFTER) {
      if (do_fe_recover(cmd)) {
        break;
      }
    } else if(cmd.type == FE_REDIRECT) {
      if (do_fe_redirect(cmd)) {
        break;
      }
    } else if(cmd.type == FE_RETIRE) {
      if(do_fe_retire(cmd)) {
        break;
      }
    } else if(cmd.type == FE_FETCH_OP) {
      do_fe_fetch_op(syscall_has_been_sent_to_scarab);
      // scarab fetches past the op before deciding (a branch inside a fetch target); a right-path exception's
      // dummy jump already has its context, a WPNM record needs none
      if (awaiting_cf_decision && checkpoints[checkpoint_index(undecided_cf_uid)].ctxt_kind == CTXT_NONE &&
          !checkpoints[checkpoint_index(undecided_cf_uid)].wrongpath_nop_mode) {
        ASSERTM(0, at_capture_site, "Capture for uid %" PRIu64 " away from a capture site, eip %" PRIx64 " op %s @%" PRIx64 " fake %d wp %d\n", undecided_cf_uid, (uint64_t)next_eip, undecided_op.st.pin_iclass, (uint64_t)undecided_op.st.instruction_addr, (int)undecided_op.st.fake_inst, (int)on_wrongpath);
        capture_pending = true;
        capture_uid     = undecided_cf_uid;
      }
      awaiting_cf_decision = false;
    } else if(cmd.type == FE_NULL) {
      do_fe_null(have_consumed_op);
    }

    buffer_ready               = scarab_buffer_full() || pending_syscall;
    bool send_buffer_to_scarab = buffer_ready && pending_fetch_op &&
                                 have_consumed_op;
    if(send_buffer_to_scarab) {
      awaiting_cf_decision = scarab_buffer_ends_at_cf() && !pending_syscall;
      if (awaiting_cf_decision)
        undecided_cf_uid = scarab_op_buffer.back().inst_uid, undecided_op = scarab_op_buffer.back();
      scarab_send_buffer();
      pending_fetch_op                = false;
      syscall_has_been_sent_to_scarab = pending_syscall;
    }

    if(have_consumed_op && !pending_syscall && !scarab_buffer_full() && !awaiting_cf_decision)
      break;

    if(have_consumed_op)
      need_scarab_cmd = true;
  }
}

bool do_fe_retire(Scarab_To_Pin_Msg& cmd) {
  DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
            "retire curr_uid=%" PRIu64 ", target_uid=%" PRIu64 ", addr=%llx\n",
            uid_ctr, cmd.inst_uid, cmd.inst_addr);
  if(cmd.inst_addr) {
    // A non-zero inst address in a retire command signifies to early exit the
    // app.
    PIN_ExitApplication(0);
    ASSERTM(0, false,
            "PIN could not safely terminate app upon scarab request.\n");
  }
  retire_older_checkpoints(cmd.inst_uid);
  return buffer_sentinel && checkpoints.empty();  // PIN recvd termination
                                                  // retire
}

void do_fe_fetch_op(bool& syscall_has_been_sent_to_scarab) {
  DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
            "fetch curr_uid=%" PRIu64 "\n", uid_ctr);
  pending_fetch_op = true;

  if(syscall_has_been_sent_to_scarab) {
    // syscall is finished when we recieve the first FETCH_OP after sending the
    // syscall to scarab
    ASSERTM(0, checkpoints.empty(),
            "Scarab has not retired all instructions since receiving a Sys "
            "Call (uid=%" PRIu64 ")\n",
            uid_ctr - 1);
    pending_syscall   = false;
    pending_exception = false;
  }
}

void do_fe_null(bool& have_consumed_op) {
  DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
            "fenull curr_uid=%" PRIu64 "\n", uid_ctr);
  if(!have_consumed_op) {
    compressed_op* cop;
    compressed_op  dummy_nop;
    if(!pending_exception) {
      if(generate_dummy_nops) {
        next_eip  = ADDR_MASK(next_eip);
        dummy_nop = create_dummy_nop((uint64_t)next_eip, wrongpath_nop_mode_reason, DUMMY_NOP_SIZE);
        cop       = &dummy_nop;
      } else {
        cop = pin_decoder_get_latest_inst();
        // hack to get SYSENTER (which is only used for 32bit
        // binaries) to work. Basically, because of the way
        // SYSENTER is used by Linux, control will return to
        // the user code 11 bytes after the SYSENTER
        // instruction, even though SYSENTER is only a 2 byte
        // instruction...
        if (strcmp(cop->st.pin_iclass, "SYSENTER") == 0) {
          cop->st.size = 11;
        }
        DBG_PRINT(uid_ctr, dbg_print_start_uid, dbg_print_end_uid,
                  "consuming instruction eip:%" PRIx64
                  ", opcode:%s, ifetch_barrier:%u, cf_type:%u, op_type:%u, "
                  "num_ld: %u, num_st: %u, exit: %u, ins_size: %u\n",
                  cop->st.instruction_addr, cop->st.pin_iclass, cop->st.is_ifetch_barrier, cop->st.cf_type,
                  cop->st.op_type, cop->st.num_ld, cop->st.num_st, cop->exit, cop->st.size);
      }
      cop->inst_uid = uid_ctr;

      if (cop->st.is_repeat && cop->st.instruction_addr == prior_rep_eip)
        cop->fetched_instruction = 0;
      else
        cop->fetched_instruction = 1;

      prior_rep_eip = cop->st.is_repeat ? cop->st.instruction_addr : 0;

      if(pending_syscall && exit_syscall_found) {
        exit_syscall_found = false;
        buffer_sentinel    = true;
      }
    } else {
      ASSERTM(0, !generate_dummy_nops,
              "Dummy NOP generated exception @uid %" PRIu64 ".\n", uid_ctr);
      ASSERTM(0, op_mailbox_full,
              "Expected full mailbox for exc @ %" PRIu64 ".\n", uid_ctr);
      cop                    = &op_mailbox;
      op_mailbox_full        = false;
      cop->st.cf_type = CF_SYS;
      cop->st.is_ifetch_barrier = 1;
      // cop->fake_inst = 1;
      cop->instruction_next_addr = saved_excp_next_eip;
    }

    if(op_mailbox_full) {
      op_mailbox.instruction_next_addr = cop->st.instruction_addr;
      if (op_mailbox.st.cf_type && op_mailbox.actually_taken) {
        op_mailbox.st.branch_target = cop->st.instruction_addr;
      }
      insert_scarab_op_in_buffer(op_mailbox);
      op_mailbox_full = false;
    }

    if(pending_syscall) {
      // bypass mailbox
      if(buffer_sentinel) {
        cop->exit = 1;
      }
      insert_scarab_op_in_buffer(*cop);
      if(buffer_sentinel) {
        compressed_op sentinel_cop = create_sentinel();
        insert_scarab_op_in_buffer(sentinel_cop);
      }
      if(!pending_exception) {
        // only syscalls create checkpoints BEFORE being sent,
        // so we update uid_ctr here to prep. for next instruction
        uid_ctr++;
      }
    } else if(pending_magic_inst == SCARAB_END) {
      cop->exit = 1;
      insert_scarab_op_in_buffer(*cop);
      compressed_op sentinel_cop = create_sentinel();
      insert_scarab_op_in_buffer(sentinel_cop);
      pending_magic_inst = NOT_MAGIC;
    } else {
      op_mailbox      = *cop;
      op_mailbox_full = true;
    }
    have_consumed_op = true;
  }
}