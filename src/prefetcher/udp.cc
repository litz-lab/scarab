#include "prefetcher/udp.hpp"

#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_FDIP, ##args)

/* Bloom_Filter member functions */
void* Bloom_Filter::lookup(Addr line_addr) {
  return (void*)(bloom->contains(line_addr) || bloom2->contains(line_addr >> 1) || bloom4->contains(line_addr >> 2));
}

void Bloom_Filter::insert1(Addr line_addr) {
  STAT_EVENT(proc_id, FDIP_BLOOM_1INSERT);
  if (!bloom2->contains(line_addr >> 1) && !bloom4->contains(line_addr >> 2)) {
    if (cnt_insert_bloom >= FDIP_BLOOM_ENTRIES && (new_prefs > FDIP_BLOOM_CLEAR_CYC_PERIOD * 0.01) &&
        ((float)cnt_unuseful / (float)new_prefs > FDIP_BLOOM_CLEAR_UNUSEFUL_RATIO)) {
      bloom->clear();
      cnt_insert_bloom = 0;
      STAT_EVENT(proc_id, FDIP_BLOOM_CLEAR1);
    }
    bloom->insert(line_addr);
    cnt_insert_bloom++;
  }
}

void Bloom_Filter::insert2(Addr line_addr) {
  if ((line_addr & 1) == 0) {  // 2CL aligned
    if (!bloom4->contains(line_addr >> 2) && !bloom2->contains(line_addr >> 1)) {
      if (cnt_insert_bloom2 >= FDIP_BLOOM2_ENTRIES && (new_prefs > FDIP_BLOOM_CLEAR_CYC_PERIOD * 0.01) &&
          ((float)cnt_unuseful / (float)new_prefs > FDIP_BLOOM_CLEAR_UNUSEFUL_RATIO)) {
        bloom2->clear();
        cnt_insert_bloom2 = 0;
        STAT_EVENT(proc_id, FDIP_BLOOM_CLEAR2);
      }
      bloom2->insert(line_addr >> 1);
      cnt_insert_bloom2++;
    }
    STAT_EVENT(proc_id, FDIP_BLOOM_2INSERT);
  } else {
    insert1(line_addr);
    insert1(line_addr + 1);
  }
}

void Bloom_Filter::insert3(Addr line_addr) {
  if ((line_addr & 1) == 0) {  // 2CL aligned
    insert2(line_addr);
    insert1(line_addr + 2);
  } else {
    insert1(line_addr + 1);
    insert2(line_addr);
  }
}

void Bloom_Filter::insert4(Addr line_addr) {
  ASSERT(0, (line_addr & 3) == 0);
  if (cnt_insert_bloom4 >= FDIP_BLOOM4_ENTRIES && (new_prefs > FDIP_BLOOM_CLEAR_CYC_PERIOD * 0.01) &&
      ((float)cnt_unuseful / (float)new_prefs > FDIP_BLOOM_CLEAR_UNUSEFUL_RATIO)) {
    bloom4->clear();
    cnt_insert_bloom4 = 0;
    STAT_EVENT(proc_id, FDIP_BLOOM_CLEAR4);
  }
  bloom4->insert(line_addr >> 2);
  cnt_insert_bloom4++;
  STAT_EVENT(proc_id, FDIP_BLOOM_4INSERT);
}

void Bloom_Filter::insert_remaining(uint32_t inserted) {
  while (inserted + 4 <= last_prefetch_candidate_counter) {
    insert4(last_prefetch_candidate + inserted);
    inserted += 4;
  }
  if (inserted + 3 == last_prefetch_candidate_counter)
    insert3(last_prefetch_candidate + inserted);
  if (inserted + 2 == last_prefetch_candidate_counter)
    insert2(last_prefetch_candidate + inserted);
  if (inserted + 1 == last_prefetch_candidate_counter)
    insert1(last_prefetch_candidate + inserted);
}

void Bloom_Filter::bloom_insert() {
  uint32_t inserted = 0;
  if (last_prefetch_candidate_counter < 4) {
    insert_remaining(inserted);
    return;
  }
  if ((last_prefetch_candidate & 3) == 0) {
    // 4cl aligned
    insert_remaining(inserted);
  } else if ((last_prefetch_candidate & 3) == 2) {
    // 2cl algned
    insert2(last_prefetch_candidate);
    inserted += 2;
    insert_remaining(inserted);
  } else if ((last_prefetch_candidate & 3) == 1) {
    // cl aligned
    // cl aligned
    insert3(last_prefetch_candidate);
    inserted += 3;
    insert_remaining(inserted);
  } else if ((last_prefetch_candidate & 3) == 3) {
    // cl aligned
    insert1(last_prefetch_candidate);
    inserted += 1;
    insert_remaining(inserted);
  }
  return;
}

void Bloom_Filter::detect_stream(Addr line_addr) {
  if (last_prefetch_candidate_counter == 0) {
    last_prefetch_candidate_counter++;
    last_prefetch_candidate = line_addr;
    return;
  }
  STAT_EVENT(proc_id, FDIP_BLOOM_INSERTED);

  if (line_addr == last_prefetch_candidate + last_prefetch_candidate_counter) {
    last_prefetch_candidate_counter++;
  } else {
    bloom_insert();
    last_prefetch_candidate_counter = 1;
    last_prefetch_candidate = line_addr;
  }
}

/* UDP member functions */
UDP::UDP(uns _proc_id)
    : proc_id(_proc_id),
      last_cl_unuseful(0),
      last_bbl_start_addr(0),
      fdip_uc(nullptr),
      fdip_uc_unuseful(nullptr),
      fdip_uc_signed(nullptr) {
  if (FDIP_UC_SIZE) {
    fdip_uc = (Cache*)malloc(sizeof(Cache));
    fdip_uc_unuseful = (Cache*)malloc(sizeof(Cache));
    fdip_uc_signed = (Cache*)malloc(sizeof(Cache));
    ASSERT(proc_id, !FDIP_BLOOM_FILTER);
    init_cache(fdip_uc, "FDIP_USEFULNESS_CACHE", FDIP_UC_SIZE, FDIP_UC_ASSOC, ICACHE_LINE_SIZE, 0,
               REPL_TRUE_LRU);  // Data size = 2 byte
    init_cache(fdip_uc_unuseful, "FDIP_USEFULNESS_CACHE_UNUSEFUL", FDIP_UC_SIZE, FDIP_UC_ASSOC, ICACHE_LINE_SIZE, 0,
               REPL_TRUE_LRU);  // Data size = 2 byte
    init_cache(fdip_uc_signed, "FDIP_USEFULNESS_CACHE_SIGNED", FDIP_UC_SIZE, FDIP_UC_ASSOC, ICACHE_LINE_SIZE,
               sizeof(int32_t), REPL_TRUE_LRU);
  }
  if (FDIP_BLOOM_FILTER) {
    ASSERT(proc_id, !FDIP_UC_SIZE && !FDIP_UTILITY_HASH_ENABLE);
    bf = new Bloom_Filter(proc_id);
    bloom_parameters bloom1_parameters;
    bloom1_parameters.projected_element_count = FDIP_BLOOM_ENTRIES;
    bloom1_parameters.false_positive_probability = 0.005;
    bloom1_parameters.compute_optimal_parameters();
    bf->bloom = new bloom_filter(bloom1_parameters);

    bloom_parameters bloom2_parameters;
    bloom2_parameters.projected_element_count = FDIP_BLOOM2_ENTRIES;
    bloom2_parameters.false_positive_probability = 0.005;
    bloom2_parameters.compute_optimal_parameters();
    bf->bloom2 = new bloom_filter(bloom2_parameters);

    bloom_parameters bloom4_parameters;
    bloom4_parameters.projected_element_count = FDIP_BLOOM4_ENTRIES;
    bloom4_parameters.false_positive_probability = 0.005;
    bloom4_parameters.compute_optimal_parameters();
    bf->bloom4 = new bloom_filter(bloom4_parameters);

    bf->last_prefetch_candidate_counter = 0;
    bf->last_clear_cycle_count = 0;
    bf->new_prefs = 0;
    bf->cnt_unuseful = 0;
    bf->cnt_insert_bloom = 0;
    bf->cnt_insert_bloom2 = 0;
    bf->cnt_insert_bloom4 = 0;
  }
}

void UDP::cyc_reset() {
  last_cl_unuseful = 0;
  if (FDIP_BLOOM_FILTER && (cycle_count - bf->last_clear_cycle_count > FDIP_BLOOM_CLEAR_CYC_PERIOD)) {
    bf->last_clear_cycle_count = cycle_count;
    bf->new_prefs = 0;
    bf->cnt_unuseful = 0;
  }
}

void UDP::clear_old_seniority_ftq() {
  if (!FDIP_UTILITY_HASH_ENABLE && !FDIP_UC_SIZE && !FDIP_BLOOM_FILTER)
    return;
  Counter cnt_old = 0;
  for (auto it = seniority_ftq.begin(); it != seniority_ftq.end(); ++it) {
    if (cycle_count <= FDIP_SENIORITY_FTQ_HOLD_CYC)
      break;
    if ((cycle_count > FDIP_SENIORITY_FTQ_HOLD_CYC) && (get<1>(*it) >= cycle_count - FDIP_SENIORITY_FTQ_HOLD_CYC))
      break;
    cnt_old++;
  }
  DEBUG(proc_id, "Clear %llu entries among %ld at cyc %llu\n", cnt_old, seniority_ftq.size(), cycle_count);
  seniority_ftq.erase(seniority_ftq.begin(), seniority_ftq.begin() + cnt_old);
}

void UDP::set_last_bbl_start_addr(Addr addr) {
  if (!last_bbl_start_addr) {
    last_bbl_start_addr = addr;
    DEBUG(proc_id, "reset last_bbl_start_addr: %llx\n", last_bbl_start_addr);
  }
}

void* UDP::bloom_lookup(Addr uc_line_addr) {
  Addr line_addr = uc_line_addr >> 6;
  return bf->lookup(line_addr);
}

void UDP::detect_stream(Addr uc_line_addr) {
  Addr line_addr = uc_line_addr >> 6;
  bf->detect_stream(line_addr);
}
