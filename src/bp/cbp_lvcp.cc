
// Developed by Yang Man and Lingrui Gou.
// This is the submission code to CBP 2025.

#include "cbp_lvcp.h"

#include <algorithm>
#include <cstdint>
#include <iostream>

#include "../globals/assert.h"
#include "../globals/global_defs.h"

#include "cbp_to_scarab.h"

void LVCP::setup() {
}

void LVCP::dump_stats() {
  std::cout
      << "-----------------------------------Load Value Correlated Predictor Stats-----------------------------------"
      << "\n";
  std::cout << "Branch Entangling Table Touch: " << stats.branch_entangling_table_touch << "\n";
  std::cout << "Branch Entangling Table Insert: " << stats.branch_entangling_table_insert << "\n";
  std::cout << "Branch Entangling Table Load PC Changed: " << stats.branch_entangling_table_load_pc_changed << "\n";
  std::cout << "Load-Branch Entangling Table Touch: " << stats.load_branch_entangling_table_touch << "\n";
  std::cout << "Load-Branch Entangling Table Insert: " << stats.load_branch_entangling_table_insert << "\n";
  std::cout << "Load-Branch Entangling Table Load PC Changed: " << stats.load_branch_entangling_table_branch_pc_changed
            << "\n";
  std::cout << "Load Value Correlated Predictor Predicted: " << stats.lvcp_predicted << "\n";
  std::cout << "Load Value Correlated Predictor Predicted By Value: " << stats.lvcp_predicted_by_value << "\n";
  std::cout << "Load Value Correlated Predictor Predicted By Addr: " << stats.lvcp_predicted_by_addr << "\n";
  std::cout << "Load Value Correlated Predictor Not Predicted: " << stats.lvcp_not_predicted << "\n";
  std::cout << "Load Value Correlated Predictor Not Predicted Due to Entangling Table Miss: "
            << stats.lvcp_not_predicted_due_to_entangling_table_miss << "\n";
  std::cout << "Load Value Correlated Predictor Not Predicted Due to Correlation Table Miss: "
            << stats.lvcp_not_predicted_due_to_correlation_table_miss << "\n";
  std::cout << "Load Value Correlated Predictor Not Predicted Due to Load Value Tracking Miss: "
            << stats.lvcp_not_predicted_due_to_load_value_tracking_miss << "\n";
  std::cout << "Load Value Correlated Predictor No Load Or Value Late: " << stats.lvcp_no_load_or_value_late << "\n";
  std::cout << "Load Value Correlated Predictor Load Value Provided by DVTAGE: "
            << stats.lvcp_load_value_provided_by_dvtage << "\n";
  std::cout << "Load Value Correlated Predictor Not Predicted Due to TAGE High Confidence: "
            << stats.lvcp_not_predicted_due_to_tage_highconf << "\n";
  std::cout << "Load Value Correlated Predictor Not Predicted Due to Low Correlation Confidence: "
            << stats.lvcp_not_predicted_due_to_low_corr_confidence << "\n";
  std::cout << "Load Value Correlated Predictor Correct: " << stats.lvcp_correct << "\n";
  std::cout << "Load Value Correlated Predictor Incorrect: " << stats.lvcp_incorrect << "\n";
  std::cout << "Load Value Correlated Predictor Correct TAGE Correct: " << stats.lvcp_correct_tage_correct << "\n";
  std::cout << "Load Value Correlated Predictor Correct TAGE Incorrect: " << stats.lvcp_correct_tage_incorrect << "\n";
  std::cout << "Load Value Correlated Predictor Incorrect TAGE Correct: " << stats.lvcp_incorrect_tage_correct << "\n";
  std::cout << "Load Value Correlated Predictor Incorrect TAGE Incorrect: " << stats.lvcp_incorrect_tage_incorrect
            << "\n";
  std::cout << "Load Inst Bitmap Miss: " << stats.load_bitmap_potential_miss << "\n";
  std::cout << "Load Inst Bitmap Hit: " << stats.load_bitmap_potential_hit << "\n";
  std::cout << "Load Inst Bitmap Max Usage: " << stats.load_bitmap_max_usage << "\n";

  // Compound stats
  // assert(stats.lvcp_predicted == stats.lvcp_correct + stats.lvcp_incorrect);
  double accuracy = stats.lvcp_predicted == 0 ? 0.0 : (double)stats.lvcp_correct / stats.lvcp_predicted;
  std::cout << "Load Value Correlated Predictor Accuracy: "
            << (stats.lvcp_predicted == 0 ? "Nan" : std::to_string(accuracy)) << "\n";

  double entangling_table_load_pc_changed_ratio =
      (double)stats.branch_entangling_table_load_pc_changed / stats.branch_entangling_table_touch;
  std::cout << "Branch Entangling Table Load PC Changed Ratio: "
            << (stats.branch_entangling_table_touch == 0 ? "Nan"
                                                         : std::to_string(entangling_table_load_pc_changed_ratio))
            << "\n";

  double entangling_table_branch_pc_changed_ratio =
      (double)stats.load_branch_entangling_table_branch_pc_changed / stats.load_branch_entangling_table_touch;
  std::cout << "Load Branch Entangling Table Load PC Changed Ratio: "
            << (stats.load_branch_entangling_table_touch == 0
                    ? "Nan"
                    : std::to_string(entangling_table_branch_pc_changed_ratio))
            << "\n";

  std::cout
      << "-----------------------------------------------------------------------------------------------------------"
      << std::endl;

  hbt.dump_stats();
  correlation_table.dump_stats();
}

void LVCP::try_find_load_instr_on_path_with_br(Addr pc) {
  Addr prev_br_next_pc = pred_prev_br_info.target;
  // get all cache blocks of the basic block [prev_br_next_pc, pc)
  Addr min_cache_block_pc = get_block_aligned_pc(prev_br_next_pc);
  Addr max_cache_block_pc = get_block_aligned_pc(pc);
  uint64_t num_cache_blocks = (max_cache_block_pc - min_cache_block_pc) / ICACHE_LINE_SIZE + 1;
  if (num_cache_blocks >= 100) {
    // printf("[%#llx, %#llx], %lu\n", prev_br_next_pc, pc, num_cache_blocks);
    return;
  }
  assert(num_cache_blocks < 100);
  // then query load_inst_bitmap to get all loads within
  for (uint64_t i = 0; i < num_cache_blocks; i++) {
    Addr block_pc = min_cache_block_pc + i * ICACHE_LINE_SIZE;
    auto set_idx = get_load_inst_bitmap_set_idx(block_pc);
    auto tag = get_load_inst_bitmap_tag(block_pc);
    auto& set = load_marking_table[set_idx];
    auto it = set.find(tag);
    if (it != set.end()) {
      // found bitmap for this cache block
      auto block_load_bitmap = it->second.map;
      assert(block_load_bitmap.size() == (ICACHE_LINE_SIZE >> 2));
      for (std::size_t j = 0; j < (ICACHE_LINE_SIZE >> 2); j++) {
        if (block_load_bitmap[j]) {
          auto load_pc = block_pc + j * 4;
          if (load_pc >= prev_br_next_pc && load_pc < pc) {
            notify_predtime_load_instr(load_pc);
          }
        }
      }
    }
  }
}

void LVCP::notify_predtime_load_instr(Addr pc) {
  load_tracking_queue.push_front({.id = 0, .pc = pc, .prev_taken_br_id = pred_prev_taken_br_info.id});
  while (load_tracking_queue.size() > load_tracking_queue_size) {
    auto [deq_id, deq_pc, deq_prev_taken_br_id] = load_tracking_queue.back();
    distant_load_buffer[(deq_pc >> 2) % load_tracking_table_size] = {
        .id = deq_id, .pc = deq_pc, .prev_taken_br_id = deq_prev_taken_br_id};
    load_tracking_queue.pop_back();
  }
}

bool LVCP::GetPrediction(Addr PC, int* bp_confidence, Op* op) {
  try_find_load_instr_on_path_with_br(PC);

  auto id = op->unique_num_per_proc;
  bool tage_pred = op->oracle_info.pred;
  bool pred = tage_pred;

  // First check hbt
  auto [is_hard_branch, hbt_index] = hbt.is_hard_branch(PC);
  (void)hbt_index;
  if (!is_hard_branch) {
    pred_time_histories[id] = {.pred_by_lvcp = false, .lvcp_pred_taken = false, .tage_pred_taken = tage_pred};
    return pred;
  }

  bool provided = false;
  bool nearly_provided = false;
  bool nearly_provided_pred = false;
  Addr load_pc = 0;
  uint64_t load_value = 0;
  uint64_t distance = 0;
  // Search correlation table, for each load, from youngest
  for (std::size_t distance = 0; distance < load_tracking_queue.size(); distance++) {
    auto [load_id, load_pc, _] = load_tracking_queue[distance];

    // Check if value ready
    if (load_value_map.find(load_id) == load_value_map.end()) {
      continue;
    }

    load_value = load_value_map[load_id];
    auto [conf_saturated, conf_high] = correlation_table.is_conf_saturated(distance, PC, load_pc, load_value);
    if (conf_saturated) {
      pred = correlation_table.predict_taken(distance, PC, load_pc, load_value);
      provided = true;
      stats.lvcp_predicted_by_value++;
      break;
    }
    if (conf_high) {
      nearly_provided_pred = correlation_table.predict_taken(distance, PC, load_pc, load_value);
      nearly_provided = true;
    }
  }
  for (std::size_t idx = 0; idx < distant_load_buffer.size(); idx++) {
    auto [load_id, load_pc, _] = distant_load_buffer[idx];

    // Check if value ready
    if (load_value_map.find(load_id) == load_value_map.end()) {
      continue;
    }

    load_value = load_value_map[load_id];
    auto [conf_saturated, conf_high] = correlation_table.is_conf_saturated(idx, PC, load_pc, load_value);
    if (conf_saturated) {
      pred = correlation_table.predict_taken(idx, PC, load_pc, load_value);
      provided = true;
      stats.lvcp_predicted_by_value++;
      break;
    }
    if (conf_high) {
      nearly_provided_pred = correlation_table.predict_taken(idx, PC, load_pc, load_value);
      nearly_provided = true;
    }
  }

  pred_time_histories[id] = {.pred_by_lvcp = provided,
                             .lvcp_pred_taken = pred,
                             .tage_pred_taken = tage_pred,
                             .lvcp_nearly_provided = nearly_provided,
                             .lvcp_nearly_provided_pred = nearly_provided_pred,
                             .lvcp_pred_load_pc = load_pc,
                             .lvcp_pred_load_value = load_value,
                             .lvcp_pred_distance = distance};
  return pred;
}

void LVCP::HistoryUpdate(Addr PC, OpType opType, bool resolveDir, Addr branchTarget, Counter id) {
  // maintain path history and previous branch info
  PrevBrInfo temp = {id, PC, branchTarget};
  if (resolveDir) {
    pred_prev_taken_br_info = temp;
  }
  pred_prev_br_info = temp;
}

void LVCP::UpdatePredictor(Addr PC, OpType opType, bool resolveDir, bool predDir, Addr branchTarget, Op* op) {
  HistoryUpdate(PC, opType, resolveDir, branchTarget, op->unique_num_per_proc);
}

void LVCP::TrackOtherInst(Addr PC, OpType opType, bool resolveDir, Addr branchTarget, Op* op) {
  try_find_load_instr_on_path_with_br(PC);
  HistoryUpdate(PC, opType, resolveDir, branchTarget, op->unique_num_per_proc);
}

void LVCP::notify_instr_decode(Addr pc, Op* op) {
  LFSR.tick();

  auto id = op->unique_num_per_proc;
  // from old to young, find the exact entry(same pc && prev_br_id < id) to
  bool found = false;
  for (auto it = load_tracking_queue.rbegin(); it != load_tracking_queue.rend(); it++) {
    // ignore entries that have been filled
    if (it->id != 0) {
      continue;
    }
    // fill that entry
    // are we sure that it is the right entry?
    if (it->pc == pc) {
      it->id = id;
      found = true;
      break;
    }
    if (it->prev_taken_br_id > id) {
      // reached an unfilled load entry older than current load
      // there must be at least one missing load from pred stage
      // TODO: stats
      break;
    }
  }
  // TODO: also do this to entries in pred_load_table?
  // uint64_t cache_block_idx = pc / IC_BLOCKSIZE;
  // int block_offset = (pc & (IC_BLOCKSIZE - 1)) >> 2;
  if (!found) {
    // printf();
    stats.load_bitmap_potential_miss++;
  } else {
    stats.load_bitmap_potential_hit++;
  }

  // try to record load in load_inst_bitmap (use 4-byte aligned pc for indexing)
  // Addr aligned_pc = pc & ~((Addr)0x3);
  std::size_t block_offset = (pc & (ICACHE_LINE_SIZE - 1)) >> 2;
  assert(block_offset < (ICACHE_LINE_SIZE >> 2));
  auto set_idx = get_load_inst_bitmap_set_idx(pc);
  auto tag = get_load_inst_bitmap_tag(pc);
  auto& set = load_marking_table[set_idx];
  auto map_it = set.find(tag);
  if (map_it == set.end()) {
    // establish new entry
    std::vector<bool> new_bitmap(ICACHE_LINE_SIZE >> 2, false);
    new_bitmap[block_offset] = true;
    Bitmap bm{new_bitmap, 1};  // initialize usage to 1
    set[tag] = bm;

    // replacement
    if (set.size() > ICACHE_ASSOC) {
      // find the entry with the smallest usefulness
      auto min_it = std::min_element(set.begin(), set.end(),
                                     [](const auto& a, const auto& b) { return a.second.useful < b.second.useful; });
      set.erase(min_it);
    }

    // stats
    uint64_t current_bitmap_usage = std::accumulate(load_marking_table.begin(), load_marking_table.end(), uint64_t{0},
                                                    [](uint64_t sum, const auto& map) { return sum + map.size(); });
    if (current_bitmap_usage > stats.load_bitmap_max_usage)
      stats.load_bitmap_max_usage = current_bitmap_usage;
  } else {
    set[tag].map[block_offset] = true;
    // accessed, useful += 1
    if (set[tag].useful != USEFUL_MAX) {
      if ((LFSR.get() & 0x3) == 0) {
        set[tag].useful++;
      }
    }
  }

  // try to do decaying
  static uint64_t load_inst_num = 0;
  static std::size_t decay_lib_idx = 0;
  if (load_inst_num >= bitmap_decay_interval) {
    assert(decay_lib_idx < bitmap_num_sets);
    auto& set = load_marking_table[decay_lib_idx];
    for (auto& e : set) {
      if (e.second.useful > 2)
        e.second.useful -= 2;
      else
        e.second.useful = 0;
    }
    decay_lib_idx++;
    decay_lib_idx %= bitmap_num_sets;
  }
  load_inst_num++;
}

void LVCP::notify_agen_complete(Addr pc, Op* op, uint64_t mem_va, uint64_t mem_sz) {
  (void)pc;
  (void)op;
  (void)mem_va;
  (void)mem_sz;
}

void LVCP::notify_load_instr_execute_resolve(Addr pc, Op* op) {
  (void)pc;
  ASSERTM(0, op->table_info->num_dest_regs != 0, "Load value not available");

  auto id = op->unique_num_per_proc;
  load_value_map[id] = op->dst_val[0];
}

void LVCP::notify_instr_commit(Addr pc, Op* op) {
  auto id = op->unique_num_per_proc;
  // Update load tracking circular buffer witha load instruction
  ASSERTM(0, op->table_info->num_dest_regs != 0, "Load value not available");

  commit_load_tracking_queue.push_front({.id = id, .pc = pc});
  while (commit_load_tracking_queue.size() > load_tracking_queue_size) {
    auto [deq_id, deq_pc, _] = commit_load_tracking_queue.back();
    commit_distant_load_buffer[(deq_pc >> 2) % load_tracking_table_size] = {deq_id, deq_pc};
    commit_load_tracking_queue.pop_back();
  }

  // Done working with load instruction
  return;
}

void LVCP::cond_branch_update(Addr pc, Op* op, const bool pred_dir) {
  auto id = op->unique_num_per_proc;
  // Do cond br training

  auto taken = op->oracle_info.dir;
  auto pred_time_history = pred_time_histories[id];
  auto tage_taken = pred_time_history.tage_pred_taken;
  auto tage_misp = tage_taken != taken;
  auto overall_misp = pred_dir != taken;
  auto allocate = overall_misp;
  auto [is_hard_branch, hbt_index] = hbt.is_hard_branch(pc);
  hbt.update(pc, pred_dir, tage_misp);

  // Update correlation table
  // Only update on hard branch to avoid noise
  if (is_hard_branch) {
    auto allocated = 0;
    bool clear_rest_useful = false;
    for (std::size_t distance = commit_load_tracking_queue.size(); distance-- > 0;) {
      auto [load_id, load_pc, _] = commit_load_tracking_queue[distance];
      assert(load_value_map.find(load_id) != load_value_map.end());
      auto load_value = load_value_map[load_id];

      if (clear_rest_useful) {
        correlation_table.reset_useful(distance, pc, load_pc, load_value);
        continue;
      }

      if (allocated > 3) {
        break;
      }
      allocated += correlation_table.update(distance, pc, load_pc, load_value, taken, tage_taken, allocate);
      if (correlation_table.is_conf_useful_saturated(distance, pc, load_pc, load_value)) {
        auto& set = load_marking_table[get_load_inst_bitmap_set_idx(load_pc)];
        auto tag = get_load_inst_bitmap_tag(load_pc);
        auto map_it = set.find(tag);
        if (map_it != set.end()) {
          map_it->second.useful = USEFUL_MAX;
        }
        clear_rest_useful = true;
      }
    }
    allocated = 0;
    for (std::size_t idx = 0; idx < commit_distant_load_buffer.size(); idx++) {
      auto [load_id, load_pc, _] = commit_distant_load_buffer[idx];
      assert(load_id == 0 || load_value_map.find(load_id) != load_value_map.end());
      auto load_value = load_value_map[load_id];

      if (allocated > 3) {
        break;
      }
      allocated += correlation_table.update(idx, pc, load_pc, load_value, taken, tage_taken, allocate);
    }
  }

  // Update correctness stats
  if (pred_time_history.pred_by_lvcp) {
    if (pred_time_history.lvcp_pred_taken == op->oracle_info.dir) {
      stats.lvcp_correct++;
      if (pred_time_history.tage_pred_taken == pred_time_history.lvcp_pred_taken) {
        stats.lvcp_correct_tage_correct++;
      } else {
        stats.lvcp_correct_tage_incorrect++;
      }
    } else {
      stats.lvcp_incorrect++;
      if (pred_time_history.tage_pred_taken == pred_time_history.lvcp_pred_taken) {
        stats.lvcp_incorrect_tage_incorrect++;
      } else {
        stats.lvcp_incorrect_tage_correct++;
      }
    }
  }
}
