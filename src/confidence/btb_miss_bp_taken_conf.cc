#include "confidence/btb_miss_bp_taken_conf.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CONF, ##args)

BTBMissBPTakenConfStat::BTBMissBPTakenConfStat(uns _proc_id, BTBMissBPTakenConf* _conf_mech)
    : ConfMechStatBase(_proc_id) {
  conf_mech = _conf_mech;
}

void BTBMissBPTakenConfStat::update(Op* op, Conf_Off_Path_Reason reason, bool last_in_ft) {
  ConfMechStatBase::update(op, reason, last_in_ft);
  log_off_path_event(op);
  log_phase_cycles(op);
}

void BTBMissBPTakenConfStat::recover(Op* op) {
  log_resolution(op);
  ConfMechStatBase::recover(op);
}

void BTBMissBPTakenConfStat::print_data() {
  ConfMechStatBase::print_data();
  FILE* fp;
  if (CONF_LOG_PHASE_CYCLES) {
    fp = fopen("phase_cycles_btb_miss.csv", "w");
    fprintf(fp, "cycles_since_rec,cycles_since_event,phase,miss_rate\n");
    for (auto it = btb_miss_event_cycles.begin(); it != btb_miss_event_cycles.end(); ++it) {
      fprintf(fp, "%llu,%llu,%llu,%f", std::get<0>(*it), std::get<1>(*it), std::get<2>(*it), std::get<3>(*it));
      fprintf(fp, "\n");
    }
    fclose(fp);
    fp = fopen("phase_cycles_ibtb_miss.csv", "w");
    fprintf(fp, "cycles_since_rec,cycles_since_event,phase,miss_rate\n");
    for (auto it = ibtb_miss_event_cycles.begin(); it != ibtb_miss_event_cycles.end(); ++it) {
      fprintf(fp, "%llu,%llu,%llu,%f", std::get<0>(*it), std::get<1>(*it), std::get<2>(*it), std::get<3>(*it));
      fprintf(fp, "\n");
    }
    fclose(fp);
    fp = fopen("phase_cycles_mispred.csv", "w");
    fprintf(fp, "cycles_since_rec,cycles_since_event,phase,miss_rate\n");
    for (auto it = mispred_event_cycles.begin(); it != mispred_event_cycles.end(); ++it) {
      fprintf(fp, "%llu,%llu,%llu,%f", std::get<0>(*it), std::get<1>(*it), std::get<2>(*it), std::get<3>(*it));
      fprintf(fp, "\n");
    }
    fclose(fp);
    fp = fopen("phase_cycles_misfetch.csv", "w");
    fprintf(fp, "cycles_since_rec,cycles_since_event,phase,miss_rate\n");
    for (auto it = misfetch_event_cycles.begin(); it != misfetch_event_cycles.end(); ++it) {
      fprintf(fp, "%llu,%llu,%llu,%f", std::get<0>(*it), std::get<1>(*it), std::get<2>(*it), std::get<3>(*it));
      fprintf(fp, "\n");
    }
    fclose(fp);
  }
  if (CONF_LOG_DFE_TO_REC) {
    fp = fopen("off_path_events_cycles.csv", "w");
    fprintf(fp, "op_num,dfe_cycle,resolved_cycle,off_path_reason\n");
    for (const std::pair<const Counter, std::tuple<Counter, Counter, Off_Path_Reason>>& line : resteer_ops_cycles) {
      fprintf(fp, "%llu,%llu,%llu,%d", line.first, std::get<0>(line.second), std::get<1>(line.second),
              std::get<2>(line.second));
      fprintf(fp, "\n");
    }
    fclose(fp);
    fp = fopen("off_path_events_ops.csv", "w");
    fprintf(fp, "op_num,dfe_num_ops,resolved_num_ops,off_path_reason\n");
    for (const std::pair<const Counter, std::tuple<Counter, Counter, Off_Path_Reason>>& line : resteer_ops_ops) {
      fprintf(fp, "%llu,%llu,%llu,%d", line.first, std::get<0>(line.second), std::get<1>(line.second),
              std::get<2>(line.second));
      fprintf(fp, "\n");
    }
    fclose(fp);
  }
}

void BTBMissBPTakenConfStat::log_phase_cycles(Op* op) {
  if (!CONFIDENCE_ENABLE || !CONF_LOG_PHASE_CYCLES)
    return;
  Off_Path_Reason op_reason = (Off_Path_Reason)op->oracle_info.off_path_reason;
  switch (op_reason) {
    case REASON_NOT_IDENTIFIED: {
      break;
    }
    case REASON_IBTB_MISS: {
      ibtb_miss_event_cycles.push_back(phase_cycles_line(
          cycle_count - conf_mech->last_recover_cycle, cycle_count - conf_mech->last_ibtb_recover_cycle,
          cycle_count - (cycle_count % CONF_IBTB_MISS_SAMPLE_RATE), (double)conf_mech->ibtb_miss_rate));
      break;
    }
    case REASON_BTB_MISS: {
      btb_miss_event_cycles.push_back(phase_cycles_line(
          cycle_count - conf_mech->last_recover_cycle, cycle_count - conf_mech->last_btb_recover_cycle,
          cycle_count - (cycle_count % CONF_BTB_MISS_SAMPLE_RATE), (double)conf_mech->btb_miss_rate));
      break;
    }
    case REASON_BTB_MISS_MISPRED: {
      btb_miss_event_cycles.push_back(phase_cycles_line(
          cycle_count - conf_mech->last_recover_cycle, cycle_count - conf_mech->last_btb_recover_cycle,
          cycle_count - (cycle_count % CONF_BTB_MISS_SAMPLE_RATE), (double)conf_mech->btb_miss_rate));
      mispred_event_cycles.push_back(phase_cycles_line(
          cycle_count - conf_mech->last_recover_cycle, cycle_count - conf_mech->last_mispred_recover_cycle,
          cycle_count - (cycle_count % CONF_MISPRED_SAMPLE_RATE), (double)conf_mech->mispred_rate));
      break;
    }
    case REASON_MISPRED: {
      mispred_event_cycles.push_back(phase_cycles_line(
          cycle_count - conf_mech->last_recover_cycle, cycle_count - conf_mech->last_mispred_recover_cycle,
          cycle_count - (cycle_count % CONF_MISPRED_SAMPLE_RATE), (double)conf_mech->mispred_rate));
      break;
    }
    case REASON_MISFETCH: {
      misfetch_event_cycles.push_back(phase_cycles_line(
          cycle_count - conf_mech->last_recover_cycle, cycle_count - conf_mech->last_misfetch_recover_cycle,
          cycle_count - (cycle_count % CONF_MISFETCH_SAMPLE_RATE), (double)conf_mech->misfetch_rate));
      break;
    }
    default: {
      ASSERT(proc_id, 0);
    }
  }
}

void BTBMissBPTakenConfStat::log_off_path_event(Op* op) {
  if (!CONFIDENCE_ENABLE || !CONF_LOG_DFE_TO_REC)
    return;
  Off_Path_Reason op_reason = (Off_Path_Reason)op->oracle_info.off_path_reason;
  if (!op_reason)
    return;
  std::get<0>(resteer_ops_cycles[op->op_num]) = cycle_count;
  std::get<1>(resteer_ops_cycles[op->op_num]) = 0;
  std::get<2>(resteer_ops_cycles[op->op_num]) = op_reason;

  std::get<0>(resteer_ops_ops[op->op_num]) = *op_count;
  std::get<1>(resteer_ops_ops[op->op_num]) = 0;
  std::get<2>(resteer_ops_ops[op->op_num]) = op_reason;
}

void BTBMissBPTakenConfStat::log_resolution(Op* op) {
  if (!CONFIDENCE_ENABLE || !CONF_LOG_DFE_TO_REC)
    return;
  Off_Path_Reason op_reason = (Off_Path_Reason)op->oracle_info.off_path_reason;
  std::get<1>(resteer_ops_cycles[op->op_num]) = cycle_count;
  std::get<2>(resteer_ops_cycles[op->op_num]) = op_reason;

  std::get<1>(resteer_ops_cycles[op->op_num]) = *op_count;
  std::get<2>(resteer_ops_cycles[op->op_num]) = op_reason;
  DEBUG(proc_id, "Op off-path reason");
}

void BTBMissBPTakenConf::per_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  if (!CONFIDENCE_ENABLE)
    return;

  DEBUG(proc_id, "btb miss rate: %f, cycles since recovery: %llu\n", btb_miss_rate,
        cycle_count - last_btb_recover_cycle);
  new_reason = update_resteer_rate_ctrs(new_reason);
}

// TO-DO: how to handle perfect mispred conf here?
void BTBMissBPTakenConf::per_cf_op_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  if (CONF_BTB_MISS_BP_TAKEN_CONF && !CONF_PERFECT_BTB_MISS_CONF && op->oracle_info.btb_miss &&
      (op->oracle_info.pred_orig == TAKEN) && (op->bp_confidence >= CONF_BTB_MISS_BP_TAKEN_THRESHOLD)) {
    low_confidence_cnt = ~0U;
    ASSERT(proc_id, op->bp_confidence >= 0 && op->bp_confidence <= 3);
    new_reason = static_cast<Conf_Off_Path_Reason>(REASON_BTB_MISS_BP_TAKEN_CONF_0 + op->bp_confidence);
  } else if (CONF_IBTB_MISS_BP_TAKEN_CONF && !CONF_PERFECT_IBTB_MISS_CONF && op->oracle_info.btb_miss &&
             (op->oracle_info.pred_orig == TAKEN) && (op->bp_confidence >= CONF_BTB_MISS_BP_TAKEN_THRESHOLD)) {
    new_reason = (Conf_Off_Path_Reason)(REASON_IBTB_MISS_BP_TAKEN);
  } else if (!CONF_INV_BP_CONF) {  // update confidence
    low_confidence_cnt += 3 - op->bp_confidence;
    if (low_confidence_cnt >= CONF_OFF_PATH_THRESHOLD)
      new_reason = REASON_CONF_THRESHOLD;
  }
}

void BTBMissBPTakenConf::per_ft_update(Op* op, Conf_Off_Path_Reason& new_reason) {
  return;
}

void BTBMissBPTakenConf::per_cycle_update(Conf_Off_Path_Reason& new_reason) {
  reset_counters();
  new_reason = update_resteer_rate_ctrs(new_reason);
}

Conf_Off_Path_Reason BTBMissBPTakenConf::update_resteer_rate_ctrs(Conf_Off_Path_Reason conf_op_reason) {
  int multiplier = 1;
  if (CONF_IPC_RATE_CONF) {
    multiplier = effective_ipc;
  }
  Conf_Off_Path_Reason ctrs_op_reason = conf_op_reason;
  if (CONF_BTB_MISS_RATE_CONF && !CONF_PERFECT_BTB_MISS_CONF &&
      ((double)(cycle_count - last_btb_recover_cycle) * btb_miss_rate * multiplier) >=
          CONF_BTB_MISS_RATE_CYCLES_THRESHOLD) {
    ctrs_op_reason = REASON_BTB_MISS_RATE;
  } else if (CONF_IBTB_MISS_RATE_CONF && !CONF_PERFECT_IBTB_MISS_CONF &&
             ((double)(cycle_count - last_ibtb_recover_cycle) * ibtb_miss_rate * multiplier) >=
                 CONF_IBTB_MISS_RATE_CYCLES_THRESHOLD) {
    ctrs_op_reason = REASON_IBTB_MISS_RATE;
  } else if (CONF_MISFETCH_RATE_CONF && !CONF_PERFECT_MISFETCH_CONF &&
             ((double)(cycle_count - last_misfetch_recover_cycle) * misfetch_rate * multiplier) >=
                 CONF_MISFETCH_RATE_CYCLES_THRESHOLD) {
    ctrs_op_reason = REASON_MISFETCH_RATE;
  } else if (CONF_MISPRED_RATE_CONF && !CONF_PERFECT_MISPRED_CONF &&
             ((double)(cycle_count - last_mispred_recover_cycle) * mispred_rate * multiplier) >=
                 CONF_MISPRED_RATE_CYCLES_THRESHOLD) {
    ctrs_op_reason = REASON_MISPRED_RATE;
  }
  return ctrs_op_reason;
}

void BTBMissBPTakenConf::reset_counters() {
  if (cycle_count % CONF_BTB_MISS_SAMPLE_RATE == 0) {
    btb_miss_rate = (double)cnt_btb_miss / (double)CONF_BTB_MISS_SAMPLE_RATE;
    cnt_btb_miss = 0;
  }
  if (cycle_count % CONF_IBTB_MISS_SAMPLE_RATE == 0) {
    ibtb_miss_rate = (double)cnt_ibtb_miss / (double)CONF_IBTB_MISS_SAMPLE_RATE;
    cnt_ibtb_miss = 0;
  }
  if (cycle_count % CONF_MISFETCH_SAMPLE_RATE == 0) {
    misfetch_rate = (double)cnt_misfetch / (double)CONF_MISFETCH_SAMPLE_RATE;
    cnt_misfetch = 0;
  }
  if (cycle_count % CONF_MISPRED_SAMPLE_RATE == 0) {
    mispred_rate = (double)cnt_mispred / (double)CONF_MISPRED_SAMPLE_RATE;
    cnt_mispred = 0;
  }
  if (cycle_count % CONF_IPC_SAMPLE_RATE == 0) {
    effective_ipc = (double)cnt_on_path_instructions / (double)CONF_IPC_SAMPLE_RATE;
    cnt_on_path_instructions = 0;
  }
}

void BTBMissBPTakenConf::update_state_perfect_conf(Op* op) {
  return;
}

void BTBMissBPTakenConf::recover(Op* op) {
  Off_Path_Reason op_reason = (Off_Path_Reason)op->oracle_info.off_path_reason;
  switch (op_reason) {
    case REASON_NOT_IDENTIFIED: {
      ASSERT(proc_id, 0);
    }
    case REASON_IBTB_MISS: {
      cnt_ibtb_miss++;
      last_ibtb_recover_cycle = cycle_count;
      break;
    }
    case REASON_BTB_MISS: {
      cnt_btb_miss++;
      last_btb_recover_cycle = cycle_count;
      break;
    }
    case REASON_BTB_MISS_MISPRED: {
      cnt_btb_miss++;
      cnt_mispred++;
      last_btb_recover_cycle = cycle_count;
      last_mispred_recover_cycle = cycle_count;
      break;
    }
    case REASON_MISPRED: {
      cnt_mispred++;
      last_mispred_recover_cycle = cycle_count;
      break;
    }
    case REASON_MISFETCH: {
      cnt_misfetch++;
      last_misfetch_recover_cycle = cycle_count;
      break;
    }
    default: {
      // shouldn't happen!
      DEBUG(proc_id, "no off path reason match: \n");
      ASSERT(proc_id, 0);
    }
  }
  low_confidence_cnt = 0;
  last_recover_cycle = cycle_count;
}

void BTBMissBPTakenConf::resolve_cf(Op* op) {
  return;
}