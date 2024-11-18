#include "mp.hpp"
#include "core.param.h"
#include "memory/memory.param.h"

using namespace std;

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

/* MP_Info member functions */
MP_Info::MP_Info() {
  mp_dist_offpath = 0;
  runahead_dist_at_resteer = 0;
  runahead_dist_after_merge = 0;
  merge_point_pc = 0;
  overlapped_dist = 0;
  op_num_mp = 0;
  op_num_mp_old = 0;
  merged_or_diverged = 0;
}

/* MP member functions */
MP::MP(uns _proc_id) : proc_id(_proc_id), candidate_mps() {
  init();
}

void MP::init() {
  warmed_up = FALSE;
  last_cl_unuseful = 0;
  mp_info = new MP_Info();
  mp_cache = (Cache*)malloc(sizeof(Cache));
  init_cache(mp_cache, "MERGE_POINT_CACHE", MP_CACHE_SIZE, MP_CACHE_ASSOC, ICACHE_LINE_SIZE,
             0, REPL_TRUE_LRU);
}

void MP::insert_mp_candidate(FT_Info* ft_info, uns64 ghist) {
  Addr line_addr = ft_info->static_info.start & ~0x3F;
  Addr line_addr2 = (ft_info->static_info.start + ft_info->static_info.length) & ~0x3F;
  candidate_mps.emplace_back(make_pair(move(line_addr), make_tuple(ghist, cycle_count, FALSE)));
  DEBUG(proc_id, "Insert a candidate_mp line_addr %llx cyc: %lld\n", line_addr, cycle_count);
  if (line_addr != line_addr2) {
    candidate_mps.emplace_back(make_pair(move(line_addr2), make_tuple(ghist, cycle_count, FALSE)));
    DEBUG(proc_id, "Insert a candidate_mp line_addr2 %llx cyc: %lld\n", line_addr2, cycle_count);
  }
}

void MP::clear_old_fts() {
  if (cycle_count <= MP_CANDIDATE_HOLD_CYC || !candidate_mps.size())
    return;

  INC_STAT_EVENT(proc_id, MP_CANDIDATE_SIZE_ACCUMULATED, candidate_mps.size());
  Counter cnt_old = 0;
  auto candidate = candidate_mps.front();
  while(!candidate_mps.empty() && get<1>(candidate.second) < cycle_count - MP_CANDIDATE_HOLD_CYC) {
    cnt_old++;
    candidate_mps.pop_front();
    if (get<2>(candidate.second)) {
      insert_mp(candidate.first, get<0>(candidate.second));
    }

    candidate = candidate_mps.front();
  }
  DEBUG(proc_id, "Clear %llu mp candidates among %ld at cycle %llu\n", cnt_old, candidate_mps.size(), cycle_count);
  ASSERT(proc_id, candidate_mps.size() >= 0);
}

void MP::search_mp_candidate(Addr line_addr) {
  DEBUG(proc_id, "Search a mp candidate for %llx. candidate_mps.size(): %ld\n", line_addr, candidate_mps.size());
  for (auto it = candidate_mps.begin();
      it != candidate_mps.end(); ++it) {
    if (it->first == line_addr) {
      DEBUG(proc_id, "Hit candidate_mps for line addr: %llx cyc: %lld\n", line_addr, get<1>(it->second));
      get<2>(it->second) = TRUE;
      STAT_EVENT(proc_id, DFE_MP_CANDIDATE_HIT);
      return;
    }
  }
  STAT_EVENT(proc_id, DFE_MP_CANDIDATE_MISS);
  return;
}

Addr MP::get_hashed_line_addr(Addr line_addr, uns64 ghist) {
  return line_addr ^ ((ghist >> (32-MP_GHIST_BITS))<<(64-MP_GHIST_BITS));
}

void MP::insert_mp_to_inf_hash(Addr line_addr, uns64 ghist) {
  Addr hashed_line_addr = MP_GHIST_HASHING ? get_hashed_line_addr(line_addr, ghist) : line_addr;
  auto iter = found_mps.find(hashed_line_addr);
  DEBUG(proc_id, "found_mps size %ld\n", found_mps.size());
  if (iter == found_mps.end()) {
    DEBUG(proc_id, "%llx merge point line new insert by the hased line addr %llx\n", line_addr, hashed_line_addr);
    STAT_EVENT(proc_id, MERGE_POINTS);
    found_mps.insert(make_pair(move(hashed_line_addr), 1));
  } else
    iter->second++;
  DEBUG(proc_id, "found_mps size after inserted %ld\n", found_mps.size());

  if (warmed_up) {
    auto iter = found_mps_aw.find(hashed_line_addr);
    if (iter == found_mps_aw.end())
      found_mps_aw.insert(make_pair(move(hashed_line_addr), 1));
    else
      iter->second++;
  }
}

void MP::insert_mp_to_cache(Addr line_addr, uns64 ghist) {
  Addr hashed_line_addr = MP_GHIST_HASHING ? get_hashed_line_addr(line_addr, ghist) : line_addr;
  Addr mp_cache_line_addr = 0;
  Addr repl_mp_cache_line_addr = 0;
  void* cnt = (void*)cache_access(mp_cache, hashed_line_addr, &mp_cache_line_addr, TRUE);
  if (!cnt)
    cache_insert_replpos(mp_cache, proc_id, hashed_line_addr, &mp_cache_line_addr,
        &repl_mp_cache_line_addr, (Cache_Insert_Repl)MP_CACHE_INSERT_REPLPOL, FALSE);
  UNUSED(mp_cache_line_addr);
  if (repl_mp_cache_line_addr)
    STAT_EVENT(proc_id, MP_CACHE_REPLACEMENT);
}

void MP::insert_mp(Addr line_addr, uns64 ghist) {
  if (FULL_WARMUP && warmup_dump_done[proc_id] && !warmed_up)
    warmed_up = TRUE;

  switch(MP_DATA_TYPE) {
    case 0:
      insert_mp_to_inf_hash(line_addr, ghist);
      break;
    case 1:
      insert_mp_to_cache(line_addr, ghist);
      break;
  }
}

Flag MP::lookup_inf_hash(Addr line_addr, uns64 ghist) {
  Addr hashed_line_addr = MP_GHIST_HASHING ? get_hashed_line_addr(line_addr, ghist) : line_addr;
  auto iter = found_mps.find(hashed_line_addr);
  if (iter == found_mps.end()) {
    STAT_EVENT(proc_id, MP_CACHE_MISS);
    return FALSE;
  }
  STAT_EVENT(proc_id, MP_CACHE_HIT);
  return TRUE;
}

Flag MP::lookup_cache(Addr line_addr, uns64 ghist) {
  Addr hashed_line_addr = MP_GHIST_HASHING ? get_hashed_line_addr(line_addr, ghist) : line_addr;
  Addr mp_line_addr;
  void* mp_found = (void*)cache_access(mp_cache, hashed_line_addr, &mp_line_addr, TRUE);
  if (mp_found)
    STAT_EVENT(proc_id, MP_CACHE_HIT);
  else
    STAT_EVENT(proc_id, MP_CACHE_MISS);
  return mp_found ? TRUE : FALSE;
}

Flag MP::lookup(Addr line_addr, uns64 ghist) {
  Flag found = FALSE;
  switch(MP_DATA_TYPE) {
    case 0:
      found = lookup_inf_hash(line_addr, ghist);
      break;
    case 1:
      found = lookup_cache(line_addr, ghist);
  }
  return found;
}
