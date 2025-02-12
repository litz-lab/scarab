#include "prefetcher/fdip_conf.hpp"
#include "prefetcher/pref.param.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_FDIP, ##args)

/* FDIP_Confidence_Info member functions */
void FDIP_Confidence_Info::recover() {
  // set previous reset previous instruction
  prev_op = nullptr;
  // reset counters and event flags (for logging)
  fdip_off_path_event = false;
  fdip_on_conf_off_event = false;
  fdip_off_conf_on_event = false;

  num_conf_0_branches = 0;
  num_conf_1_branches = 0;
  num_conf_2_branches = 0;
  num_conf_3_branches = 0;

  num_cf_br = 0;
  num_cf_cbr = 0;
  num_cf_call = 0;
  num_cf_ibr = 0;
  num_cf_icall = 0;
  num_cf_ico = 0;
  num_cf_ret = 0;
  num_cf_sys = 0;

  num_BTB_misses = 0;
  num_op_dist_incs = 0;
}

void FDIP_Confidence_Info::log_stats_bp_conf_on() {
  if (fdip_off_path()) {
    if (!fdip_off_conf_on_event) {
      DEBUG(proc_id, "prev_op op_num: %llu, cf_type: %i, cur_op op_num: %llu, cf_type: %i\n", prev_op->op_num, prev_op->table_info->cf_type, fdip_get_cur_op()->op_num, fdip_get_cur_op()->table_info->cf_type);
      ASSERT(proc_id, prev_op->table_info->cf_type); // must be a cf as the last on-path op
      fdip_off_conf_on_event = true;
      STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_NUM_EVENTS);
      STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_NOT + off_path_reason);
    }
    STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_PREF_CANDIDATES);
    // FIXME
    STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_NOT_PREF_CANDIDATES + off_path_reason);
  } else {
    STAT_EVENT(proc_id, FDIP_ON_CONF_ON_PREF_CANDIDATES);
  }
}

// FIXME: we never use these, get rid of them?
void FDIP_Confidence_Info::log_stats_bp_conf_off() {
  if (fdip_off_path())
    STAT_EVENT(proc_id, FDIP_OFF_CONF_OFF_PREF_CANDIDATES);
  else {
    STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_PREF_CANDIDATES);
    STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_INVALID_PREF_CANDIDATES + conf_off_path_reason);
    if (!fdip_on_conf_off_event) {
      fdip_on_conf_off_event = true;

      STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_EVENTS);
      STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_INVALID + conf_off_path_reason);

      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CF_BR, num_cf_br);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CF_CBR, num_cf_cbr);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CF_CALL, num_cf_call);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CF_IBR, num_cf_ibr);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CF_ICALL, num_cf_icall);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CF_ICO, num_cf_ico);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CF_RET, num_cf_ret);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CF_SYS, num_cf_sys);

      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CONF_0_BR, num_conf_0_branches);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CONF_1_BR, num_conf_1_branches);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CONF_2_BR, num_conf_2_branches);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_CONF_3_BR, num_conf_3_branches);

      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_BTB_MISS, num_BTB_misses);
      INC_STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_OP_DIST_INC, num_op_dist_incs);
    }
  }
}

/* FDIP_Conf member functions */
void FDIP_Conf::recover() {
  Off_Path_Reason op_reason = conf_info->off_path_reason;
  switch (op_reason) {
    case REASON_NOT: {
      // if a resteer happens right after another resteer it might not be in the FTQ
      // TODO: this should NOT happen if using eval(op) ?
      break;
    }
    case REASON_IBTB_MISS: {
      inc_cnt_ibtb_miss();
      last_ibtb_recover_cycle = cycle_count;
      break;
    }
    case REASON_BTB_MISS: {
      inc_cnt_btb_miss();
      last_btb_recover_cycle = cycle_count;
      break;
    }
    case REASON_BTB_MISS_MISPRED: {
      inc_cnt_btb_miss();
      inc_cnt_mispred();
      last_btb_recover_cycle = cycle_count;
      last_mispred_recover_cycle = cycle_count;
      break;
    }
    case REASON_MISPRED: {
      inc_cnt_mispred();
      last_mispred_recover_cycle = cycle_count;
      break;
    }
    case REASON_MISFETCH: {
      inc_cnt_misfetch();
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
  cf_op_distance = 0.0;
  conf_info->recover();
}

void FDIP_Conf::set_prev_op(Op* prev_op, Flag off_path) {
  conf_info->prev_op = prev_op;
  DEBUG(proc_id, "Set prev_op off_path:%i, op_num:%llu, cf_type:%i\n", conf_info->prev_op->off_path, conf_info->prev_op->op_num, conf_info->prev_op->table_info->cf_type);
  conf_info->fdip_off_path_event = off_path;
  conf_info->off_path_reason = eval_off_path_reason(prev_op);
}

void FDIP_Conf::update(Op* op) {
  if (FDIP_BP_PERFECT_CONFIDENCE) {
    if (fdip_off_path())
      low_confidence_cnt = ~0U;
    if(low_confidence_cnt == ~0U)
      ASSERT(proc_id, fdip_off_path());
    cf_op_distance = 0.0;
  } else if (FDIP_FINE_GRAINED_CONF) {
    fine_grained_conf_update(op);
  } else {
    default_conf_update(op);
  }
}

void FDIP_Conf::cyc_reset() {
  if (cycle_count % FDIP_BTB_MISS_SAMPLE_RATE == 0) {
    btb_miss_rate = (double)cnt_btb_miss / (double)FDIP_BTB_MISS_SAMPLE_RATE;
    cnt_btb_miss = 0;
  }
  if (cycle_count % FDIP_IBTB_MISS_SAMPLE_RATE == 0) {
    ibtb_miss_rate = (double)cnt_ibtb_miss / (double)FDIP_IBTB_MISS_SAMPLE_RATE;
    cnt_ibtb_miss = 0;
  }
  if (cycle_count % FDIP_MISFETCH_SAMPLE_RATE == 0) {
    misfetch_rate = (double)cnt_misfetch / (double)FDIP_MISFETCH_SAMPLE_RATE;
    cnt_misfetch = 0;
  }
  if (cycle_count % FDIP_MISPRED_SAMPLE_RATE == 0) {
    mispred_rate = (double)cnt_mispred / (double)FDIP_MISPRED_SAMPLE_RATE;
    cnt_mispred = 0;
  }
}

void FDIP_Conf::inc_br_conf_counters(int conf) {
  switch(conf) {
    case 0:
      conf_info->num_conf_0_branches += 1;
      break;
    case 1:
      conf_info->num_conf_1_branches += 1;
      break;
    case 2:
      conf_info->num_conf_2_branches += 1;
      break;
    case 3:
      conf_info->num_conf_3_branches += 1;
      break;
    default:
      DEBUG(proc_id, "inc_br_conf_counters: invalid conf value\n");
      break;
  }
}

void FDIP_Conf::inc_cf_type_counters(Cf_Type cf_type){
  switch(cf_type) {
    case NOT_CF:
      DEBUG(proc_id, "inc_cf_type_counters: instruction is not a cf inst.\n");
      break;
    case CF_BR:
      conf_info->num_cf_br += 1;
      break;
    case CF_CBR:
      conf_info->num_cf_cbr += 1;
      break;
    case CF_CALL:
      conf_info->num_cf_call += 1;
      break;
    case CF_IBR:
      conf_info->num_cf_ibr += 1;
      break;
    case CF_ICALL:
      conf_info->num_cf_icall += 1;
      break;
    case CF_ICO:
      conf_info->num_cf_ico += 1;
      break;
    case CF_RET:
      conf_info->num_cf_ret += 1;
      break;
    case CF_SYS:
      conf_info->num_cf_sys += 1;
      break;
    default:
      DEBUG(proc_id, "inc_cf_type_counters: instruction is not a valid cf inst.\n");
      break;
  }
}

// default conf mechanism
void FDIP_Conf::default_conf_update(Op* op) {
  DEBUG(proc_id, "default_conf_update\n");
  //prevent wrap around
  if (low_confidence_cnt != ~0U) {
    if (op->table_info->cf_type) {
      low_confidence_cnt += 3 - op->bp_confidence + (double)FDIP_BTB_MISS_RATE_WEIGHT*btb_miss_rate; //3 is highest bp_confidence
      cf_op_distance = 0.0;
      //log stats
      if(op->oracle_info.btb_miss)
        conf_info->num_BTB_misses += 1;
      inc_br_conf_counters(op->bp_confidence);
      inc_cf_type_counters(op->table_info->cf_type);
      DEBUG(proc_id, "op->bp_confidence: %d, low_confidence_cnt: %d, off_path: %d\n", op->bp_confidence, low_confidence_cnt, op->off_path? 1:0);
    } else if (cf_op_distance >= FDIP_OFF_PATH_THRESHOLD) {
      low_confidence_cnt += FDIP_OFF_PATH_CONF_INC + (double)FDIP_BTB_MISS_RATE_WEIGHT*btb_miss_rate;
      cf_op_distance = 0.0;
      conf_info->num_op_dist_incs += 1;
    } else {
      cf_op_distance += (1.0+(double)FDIP_BTB_MISS_RATE_WEIGHT*btb_miss_rate);
    }
  }
}

void FDIP_Conf::fine_grained_conf_update(Op* op) {
  if (!FDIP_BP_CONFIDENCE)
    return;
  if (low_confidence_cnt == ~0U)
    return;
  Conf_Off_Path_Reason conf_op_reason = REASON_INVALID;
  if (op->table_info->cf_type) {
    // IBTB miss and BP Predicts taken
    if (ENABLE_IBP && (op->table_info->cf_type == CF_IBR || op->table_info->cf_type == CF_ICALL) &&
        op->oracle_info.btb_miss && op->oracle_info.ibp_miss && op->oracle_info.pred_orig == TAKEN) {
      conf_op_reason = REASON_IBTB_MISS_BP_TAKEN;
    }
    // BTB miss and BP Predicts taken
    else if (op->oracle_info.btb_miss && (op->oracle_info.pred_orig == TAKEN) &&
             (op->bp_confidence >= FDIP_BTB_MISS_BP_TAKEN_CONF_THRESHOLD)) {
      conf_op_reason = (Conf_Off_Path_Reason)(REASON_BTB_MISS_BP_TAKEN_CONF_0 + op->bp_confidence);
    } else {  // update low bp confidence counter
      low_confidence_cnt += 3 - op->bp_confidence;
      if (low_confidence_cnt > FDIP_OFF_PATH_THRESHOLD) {
        conf_op_reason = REASON_INV_CONF_INC;
      }
    }
    //log stats
    if (op->oracle_info.btb_miss) {
      conf_info->num_BTB_misses += 1;
    }
    inc_br_conf_counters(op->bp_confidence);
    inc_cf_type_counters(op->table_info->cf_type);
    DEBUG(proc_id, "op->bp_confidence: %d, low_confidence_cnt: %d, off_path: %d\n", op->bp_confidence, low_confidence_cnt, op->off_path? 1:0);
  } else {
    conf_op_reason = update_resteer_rate_ctrs(conf_op_reason);
  }
  if (conf_op_reason != REASON_INVALID) {
    low_confidence_cnt = ~0U;
    STAT_EVENT(proc_id, FDIP_CONF_OP_REASON_INVALID + conf_op_reason);
    // set conf off-path event flag (used for logging)
    if (!conf_info->fdip_on_conf_off_event)
      conf_info->conf_off_path_reason = conf_op_reason;
  }
}

// update based on cycles since resteer of type X * rate of X
Conf_Off_Path_Reason FDIP_Conf::update_resteer_rate_ctrs(Conf_Off_Path_Reason conf_op_reason) {
  Conf_Off_Path_Reason ctrs_op_reason = conf_op_reason;
  if (((double)(cycle_count - last_btb_recover_cycle) * btb_miss_rate) >= FDIP_BTB_MISS_RATE_CYCLES_THRESHOLD) {
    ctrs_op_reason = REASON_BTB_MISS_RATE;
  } else if (((double)(cycle_count - last_ibtb_recover_cycle) * ibtb_miss_rate) >=
             FDIP_IBTB_MISS_RATE_CYCLES_THRESHOLD) {
    ctrs_op_reason = REASON_IBTB_MISS_RATE;
  } else if (((double)(cycle_count - last_misfetch_recover_cycle) * misfetch_rate) >=
             FDIP_MISFETCH_RATE_CYCLES_THRESHOLD) {
    ctrs_op_reason = REASON_MISFETCH_RATE;
  } else if (((double)(cycle_count - last_mispred_recover_cycle) * mispred_rate) >=
             FDIP_MISPRED_RATE_CYCLES_THRESHOLD) {
    ctrs_op_reason = REASON_MISPRED_RATE;
  }
  return ctrs_op_reason;
}

Off_Path_Reason FDIP_Conf::eval_off_path_reason(Op* op) {
  // if the instruction is not a resteer op no reason to log
  if (!(op->oracle_info.recover_at_decode || op->oracle_info.recover_at_exec)) {
    return REASON_NOT;
  }
  // mispred
  if (op->oracle_info.pred_orig != op->oracle_info.dir && !op->oracle_info.btb_miss) {
    return REASON_MISPRED;
  }
  // misfetch
  else if (!op->oracle_info.btb_miss && op->oracle_info.pred_orig == op->oracle_info.dir &&
           op->oracle_info.pred_npc != op->oracle_info.npc) {
    return REASON_MISFETCH;
  }
  // ibtb miss
  else if (ENABLE_IBP && (op->table_info->cf_type == CF_IBR || op->table_info->cf_type == CF_ICALL) &&
           op->oracle_info.btb_miss && op->oracle_info.ibp_miss && op->oracle_info.pred_orig == TAKEN) {
    return REASON_IBTB_MISS;
  }
  // btb miss and mispred (would have been incorrect with or without btb miss)
  else if (op->oracle_info.pred_orig != op->oracle_info.dir && op->oracle_info.btb_miss) {
    return REASON_BTB_MISS_MISPRED;
  }
  // true btb miss
  else if (op->oracle_info.btb_miss) {
    return REASON_BTB_MISS;
  } else {
    // shouldn't happen
    ASSERT(proc_id, FALSE);
  }
}

void FDIP_Conf::log_stats_bp_conf() {
  if (low_confidence_cnt < FDIP_OFF_PATH_THRESHOLD)
    conf_info->log_stats_bp_conf_on();
  else
    conf_info->log_stats_bp_conf_off();
}

void FDIP_Conf::log_stats_bp_conf_emitted() {
  //conf on
  if(low_confidence_cnt < FDIP_OFF_PATH_THRESHOLD){
    //actually off
    if(fdip_off_path()) {
      STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_EMITTED);
      STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_NOT_EMITTED + conf_info->off_path_reason);
    } else {
      STAT_EVENT(proc_id, FDIP_ON_CONF_ON_EMITTED);
    }
  } else {
    if(fdip_off_path()) {
      STAT_EVENT(proc_id, FDIP_OFF_CONF_OFF_EMITTED);
    } else {
      STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_EMITTED);
      STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_INVALID_EMITTED + conf_info->conf_off_path_reason);
    }
  }
}
