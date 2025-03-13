#include "prefetcher/fdip_conf.hpp"
#include "prefetcher/pref.param.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_FDIP, ##args)

/* FDIP_Confidence_Info member functions */
void FDIP_Confidence_Info::recover() {
  // set previous reset previous instruction
  prev_op = nullptr;
  // reset counters and event flags
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
      if (prev_op->oracle_info.mispred) {
        off_path_reason = REASON_MISPRED;
        STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_BP_INCORRECT);
        STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_BP_INCORRECT_0_CONF + prev_op->bp_confidence);
      } else if (prev_op->oracle_info.btb_miss) { // if off path due to btb miss
        off_path_reason = REASON_BTB_MISS;
        STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_BTB_MISS);
        STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_BTB_MISS_NOT_CF + prev_op->table_info->cf_type);
      } else if (prev_op->oracle_info.no_target) { // if off path due to no target
        off_path_reason = REASON_NO_TARGET;
        STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_NO_TARGET);
      } else if (prev_op->oracle_info.misfetch) { // if off path due to misfetch
        off_path_reason = REASON_MISFETCH;
        STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_MISFETCH);
      } else { // if some other reason (shouldn't happen)
        DEBUG(proc_id, "fdip off conf on event, unrecognized off path reason: op type: %u\n", prev_op->table_info->op_type);
        //ASSERT(proc_id, false); //Disable for now
      }
    }
    STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_PREF_CANDIDATES);
    STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_BTB_MISS_PREF_CANDIDATES + off_path_reason);
  } else {
    STAT_EVENT(proc_id, FDIP_ON_CONF_ON_PREF_CANDIDATES);
  }
}

void FDIP_Confidence_Info::log_stats_bp_conf_off() {
  if (fdip_off_path())
    STAT_EVENT(proc_id, FDIP_OFF_CONF_OFF_PREF_CANDIDATES);
  else {
    STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_PREF_CANDIDATES);
    STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_BTB_MISS_BP_TAKEN_CONF_0_PREF_CANDIDATES + conf_off_path_reason);
    if (!fdip_on_conf_off_event) {
      fdip_on_conf_off_event = true;

      STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_NUM_EVENTS);

      STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_BTB_MISS_BP_TAKEN_CONF_0 + conf_off_path_reason);

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
  low_confidence_cnt = 0;
  cf_op_distance = 0.0;
  conf_info->recover();
}

void FDIP_Conf::set_prev_op(Op* prev_op, Flag off_path) {
  conf_info->prev_op = prev_op;
  DEBUG(proc_id, "Set prev_op off_path:%i, op_num:%llu, cf_type:%i\n", conf_info->prev_op->off_path, conf_info->prev_op->op_num, conf_info->prev_op->table_info->cf_type);
  conf_info->fdip_off_path_event = off_path;
}

void FDIP_Conf::update(Op* op) {
  if (FDIP_BP_PERFECT_CONFIDENCE) {
    if (fdip_off_path())
      low_confidence_cnt = ~0U;
    if(low_confidence_cnt == ~0U)
      ASSERT(proc_id, fdip_off_path());
    cf_op_distance = 0.0;
  } else if (FDIP_BTB_MISS_BP_TAKEN_CONF) {
    btb_miss_bp_taken_conf_update(op);
  } else {
    default_conf_update(op);
  }
}

void FDIP_Conf::cyc_reset() {
  if (cycle_count % FDIP_BTB_MISS_SAMPLE_RATE == 0) {
    btb_miss_rate = (double)cnt_btb_miss / (double)FDIP_BTB_MISS_SAMPLE_RATE;
    cnt_btb_miss = 0;
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
//if btb miss and high enough bp confidence set confidence to off path
void FDIP_Conf::btb_miss_bp_taken_conf_update(Op* op) {
  if (!FDIP_BP_CONFIDENCE)
    return;
  if (low_confidence_cnt == ~0U)
    return;
  if (op->table_info->cf_type) {
    if (op->oracle_info.btb_miss &&
        (op->oracle_info.pred_orig == TAKEN) &&
        (op->bp_confidence >= FDIP_BTB_MISS_BP_TAKEN_CONF_THRESHOLD)) {
      low_confidence_cnt = ~0U;
      if (!conf_info->fdip_on_conf_off_event && op->bp_confidence >= 0 && op->bp_confidence <= 3)
        conf_info->conf_off_path_reason = static_cast<Conf_Off_Path_Reason>(REASON_BTB_MISS_BP_TAKEN_CONF_0 + op->bp_confidence);
    } else { // update confidence
      low_confidence_cnt += 3 - op->bp_confidence;
      if (!conf_info->fdip_on_conf_off_event)
        conf_info->conf_off_path_reason = REASON_INV_CONF_INC;
    }
    //log stats
    if (op->oracle_info.btb_miss)
      conf_info->num_BTB_misses += 1;
    inc_br_conf_counters(op->bp_confidence);
    inc_cf_type_counters(op->table_info->cf_type);
    DEBUG(proc_id, "op->bp_confidence: %d, low_confidence_cnt: %d, off_path: %d\n", op->bp_confidence, low_confidence_cnt, op->off_path? 1:0);
  } else { // update confidence based on number of cycles elapsed and btb miss rate
           //if number of cycles times btb miss rate is greater than 1 we have probably seen a btb miss
    DEBUG(proc_id, "btb miss rate: %f, cycles since recovery: %llu", btb_miss_rate, cycle_count - fdip_get_last_recover_cycle());
    if ((double)((cycle_count - fdip_get_last_recover_cycle()) * btb_miss_rate)  >= FDIP_BTB_MISS_RATE_CYCLES_THRESHOLD) {
      low_confidence_cnt = ~0U;
      STAT_EVENT(proc_id, FDIP_BTB_NUM_CYCLES_OFF_PATH_EVENT);
      if (!conf_info->fdip_on_conf_off_event)
        conf_info->conf_off_path_reason = REASON_BTB_MISS_RATE;
    }
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
      STAT_EVENT(proc_id, FDIP_OFF_CONF_ON_BTB_MISS_EMITTED + conf_info->off_path_reason);
    } else {
      STAT_EVENT(proc_id, FDIP_ON_CONF_ON_EMITTED);
    }
  } else {
    if(fdip_off_path()) {
      STAT_EVENT(proc_id, FDIP_OFF_CONF_OFF_EMITTED);
    } else {
      STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_EMITTED);
      STAT_EVENT(proc_id, FDIP_ON_CONF_OFF_BTB_MISS_BP_TAKEN_CONF_0_EMITTED + conf_info->conf_off_path_reason);
    }
  }
}
