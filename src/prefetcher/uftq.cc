#include "prefetcher/uftq.hpp"

#include "prefetcher/pref.param.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_FDIP, ##args)

/* UFTQ member functions */
void UFTQ::cyc_reset() {
  if (cycle_count % FDIP_ADJUSTABLE_FTQ_CYC != 0)
    return;
  if (unuseful_prefetches) {
    utility_ratio = (double)useful_prefetches / ((double)useful_prefetches + (double)unuseful_prefetches);
    adjust = TRUE;
  }
  useful_prefetches = 0;
  unuseful_prefetches = 0;
  DEBUG(proc_id, "Update utility ratio : %lf\n", utility_ratio);
  DEBUG(proc_id, "mshr_prefetch_hits : %llu, icache_prefetch_hits : %llu\n", mshr_prefetch_hits, icache_prefetch_hits);
  if (icache_prefetch_hits &&
      (icache_prefetch_hits + mshr_prefetch_hits > 4)) {  // do not adjust if there are too few samples?
    timeliness_ratio = (double)mshr_prefetch_hits / ((double)mshr_prefetch_hits + (double)icache_prefetch_hits);
    adjust = TRUE;
  }
  mshr_prefetch_hits = 0;
  icache_prefetch_hits = 0;
  DEBUG(proc_id, "Update timeliness ratio : %lf\n", timeliness_ratio);
}

/* Returns a new computed FTQ depth based on the current utility ratio or/and timeliness ratio of
   FDIP prefetches. Three different modes based on FDIP_ADJUSTABLE_FTQ (1: AUR, 2: ATR, 3: AURATR) */
void UFTQ::set_ftq_ft_num() {
  uint64_t cur_ftq_ft_num = decoupled_fe_get_ftq_num();
  if (!FDIP_ADJUSTABLE_FTQ || !adjust)
    return;

  uint64_t new_ftq_ft_num = cur_ftq_ft_num;
  uint64_t new_qdaur = qdaur;
  uint64_t new_qdatr = qdatr;
  if (FDIP_ADJUSTABLE_FTQ == 1) {  // utility-based adjustment UFTQ-AUR
    DEBUG(proc_id, "Current utility ratio : %lf, current FTQ ft num : %lu\n", utility_ratio, cur_ftq_ft_num);
    if (utility_ratio < UTILITY_RATIO_THRESHOLD) {
      new_ftq_ft_num -= round(cur_ftq_ft_num * (UTILITY_RATIO_THRESHOLD - utility_ratio));
      if (new_ftq_ft_num < UFTQ_MIN_FTQ_BLOCK_NUM)
        new_ftq_ft_num = UFTQ_MIN_FTQ_BLOCK_NUM;
    } else if (utility_ratio > UTILITY_RATIO_THRESHOLD) {
      new_ftq_ft_num += round(cur_ftq_ft_num * (utility_ratio - UTILITY_RATIO_THRESHOLD));
      if (new_ftq_ft_num > UFTQ_MAX_FTQ_BLOCK_NUM)
        new_ftq_ft_num = UFTQ_MAX_FTQ_BLOCK_NUM;
    }
    DEBUG(proc_id, "New FTQ ft num : %lu\n", new_ftq_ft_num);
    adjust = FALSE;
  } else if (FDIP_ADJUSTABLE_FTQ == 2) {  // timeliness-based adjustment UFTQ-ATR
    DEBUG(proc_id, "Current timeliness ratio : %lf, current FTQ ft num : %lu\n", timeliness_ratio, cur_ftq_ft_num);
    if (timeliness_ratio < TIMELINESS_RATIO_THRESHOLD) {
      new_ftq_ft_num += round(cur_ftq_ft_num * (TIMELINESS_RATIO_THRESHOLD - timeliness_ratio));
      if (new_ftq_ft_num > UFTQ_MAX_FTQ_BLOCK_NUM)
        new_ftq_ft_num = UFTQ_MAX_FTQ_BLOCK_NUM;
    } else if (timeliness_ratio > TIMELINESS_RATIO_THRESHOLD) {
      new_ftq_ft_num -= round(cur_ftq_ft_num * (timeliness_ratio - TIMELINESS_RATIO_THRESHOLD));
      if (new_ftq_ft_num < UFTQ_MIN_FTQ_BLOCK_NUM)
        new_ftq_ft_num = UFTQ_MIN_FTQ_BLOCK_NUM;
    }
    DEBUG(proc_id, "New FTQ ft num : %lu\n", new_ftq_ft_num);
    adjust = FALSE;
  } else if (FDIP_ADJUSTABLE_FTQ == 3) {  // combined method UFTQ-ATR-AUR
    DEBUG(proc_id, "Current utility ratio : %lf, timeliness ratio : %lf, current FTQ ft num : %lu\n", utility_ratio,
          timeliness_ratio, cur_ftq_ft_num);
    if (!new_qdaur) {  // First, find QDAUR
      if (utility_ratio < UTILITY_RATIO_THRESHOLD) {
        new_ftq_ft_num -= round(cur_ftq_ft_num * (UTILITY_RATIO_THRESHOLD - utility_ratio));
        if (new_ftq_ft_num < UFTQ_MIN_FTQ_BLOCK_NUM)
          new_ftq_ft_num = UFTQ_MIN_FTQ_BLOCK_NUM;
      } else if (utility_ratio > UTILITY_RATIO_THRESHOLD) {
        new_ftq_ft_num += round(cur_ftq_ft_num * (utility_ratio - UTILITY_RATIO_THRESHOLD));
        if (new_ftq_ft_num > UFTQ_MAX_FTQ_BLOCK_NUM)
          new_ftq_ft_num = UFTQ_MAX_FTQ_BLOCK_NUM;
      }
      if (new_ftq_ft_num == cur_ftq_ft_num)  // set qdaur when ftq size plateaus
        qdaur = new_ftq_ft_num;
    } else if (!new_qdatr) {  // Then find QDATR after QDAUR found
      if (timeliness_ratio < TIMELINESS_RATIO_THRESHOLD) {
        new_ftq_ft_num += round(cur_ftq_ft_num * (TIMELINESS_RATIO_THRESHOLD - timeliness_ratio));
        if (new_ftq_ft_num > UFTQ_MAX_FTQ_BLOCK_NUM)
          new_ftq_ft_num = UFTQ_MAX_FTQ_BLOCK_NUM;
      } else if (timeliness_ratio > TIMELINESS_RATIO_THRESHOLD) {
        new_ftq_ft_num -= round(cur_ftq_ft_num * (timeliness_ratio - TIMELINESS_RATIO_THRESHOLD));
        if (new_ftq_ft_num < UFTQ_MIN_FTQ_BLOCK_NUM)
          new_ftq_ft_num = UFTQ_MIN_FTQ_BLOCK_NUM;
      }
      if (new_ftq_ft_num == cur_ftq_ft_num)  // set qdatr when ftq size plateaus
        qdatr = new_ftq_ft_num;
    } else {
      new_ftq_ft_num = round(-0.34 * new_qdaur - 0.64 * new_qdatr + 0.008 * new_qdaur * new_qdaur +
                             0.01 * new_qdatr * new_qdatr + 0.008 * new_qdaur * new_qdatr);
      if (new_ftq_ft_num < UFTQ_MIN_FTQ_BLOCK_NUM)
        new_ftq_ft_num = UFTQ_MIN_FTQ_BLOCK_NUM;
      else if (new_ftq_ft_num > UFTQ_MAX_FTQ_BLOCK_NUM)
        new_ftq_ft_num = UFTQ_MAX_FTQ_BLOCK_NUM;

      // update qdaur and qdatr for the future use
      if (utility_ratio < UTILITY_RATIO_THRESHOLD) {
        new_qdaur -= round(new_qdaur * (UTILITY_RATIO_THRESHOLD - utility_ratio));
        if (new_qdaur < UFTQ_MIN_FTQ_BLOCK_NUM)
          new_qdaur = UFTQ_MIN_FTQ_BLOCK_NUM;
      } else if (utility_ratio > UTILITY_RATIO_THRESHOLD) {
        new_qdaur += round(new_qdaur * (utility_ratio - UTILITY_RATIO_THRESHOLD));
        if (new_qdaur > UFTQ_MAX_FTQ_BLOCK_NUM)
          new_qdaur = UFTQ_MAX_FTQ_BLOCK_NUM;
      }
      if (timeliness_ratio < TIMELINESS_RATIO_THRESHOLD) {
        new_qdatr += round(new_qdatr * (TIMELINESS_RATIO_THRESHOLD - timeliness_ratio));
        if (new_qdatr > UFTQ_MAX_FTQ_BLOCK_NUM)
          new_qdatr = UFTQ_MAX_FTQ_BLOCK_NUM;
      } else if (timeliness_ratio > TIMELINESS_RATIO_THRESHOLD) {
        new_qdatr -= round(new_qdatr * (timeliness_ratio - TIMELINESS_RATIO_THRESHOLD));
        if (new_qdatr < UFTQ_MIN_FTQ_BLOCK_NUM)
          new_qdatr = UFTQ_MIN_FTQ_BLOCK_NUM;
      }

      qdaur = new_qdaur;
      qdatr = new_qdatr;
    }
    DEBUG(proc_id, "New FTQ ft num : %lu\n", new_ftq_ft_num);
    adjust = FALSE;
  }

  if (cur_ftq_ft_num == new_ftq_ft_num)
    STAT_EVENT(proc_id, FDIP_UFTQ_STAY_SAME_FTQ_NUM);
  else if (cur_ftq_ft_num > new_ftq_ft_num)
    STAT_EVENT(proc_id, FDIP_UFTQ_DEC_FTQ_NUM);
  else
    STAT_EVENT(proc_id, FDIP_UFTQ_INC_FTQ_NUM);
  return decoupled_fe_set_ftq_num(new_ftq_ft_num);
}
