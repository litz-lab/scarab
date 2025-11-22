#pragma once
#ifndef _PREDICTOR_H_
#define _PREDICTOR_H_

// Developed by Yang Man and Lingrui Gou.
// This is the submission code to CBP 2025.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>
#include "../globals/global_types.h"
#include "bp.param.h"
#include "../memory/memory.param.h"
#include "cbp_to_scarab.h"

constexpr static uint64_t load_tracking_queue_size = 16;
constexpr static uint64_t load_tracking_table_size = 16;

class LRUReplacer {
  private:
    std::list<std::size_t> _list;
    std::size_t _capacity;

  public:
    LRUReplacer(std::size_t capacity) : _capacity(capacity) {
        _list.resize(capacity);
        std::iota(_list.begin(), _list.end(), 0);
    }

    void touch(std::size_t index) {
        assert(_capacity > 0 && "LRUReplacer::touch: capacity is zero, maybe uninitialized");
        assert(_list.size() == _capacity && "LRUReplacer::touch: list size does not match capacity");
        assert(index < _capacity && "LRUReplacer::touch: index out of bounds");

        // Move to front
        _list.splice(_list.begin(), _list, std::find(_list.begin(), _list.end(), index));
    }

    // Get the replacement index, does not move the list
    // If you want to move this index to the front, call touch() right after getting the replacement index
    std::size_t get_replacement() {
        assert(_capacity > 0 && "LRUReplacer::touch: capacity is zero, maybe uninitialized");
        assert(_list.size() == _capacity && "LRUReplacer::touch: list size does not match capacity");

        return _list.back();
    }

    auto lru_list() {
        assert(_capacity > 0 && "LRUReplacer::touch: capacity is zero, maybe uninitialized");
        assert(_list.size() == _capacity && "LRUReplacer::touch: list size does not match capacity");

        return _list;
    }
};

class LFSR32 {
  private:
    unsigned int _LFSR; // Holds the 32-bit state

    // Hardcoded taps for x^32 + x^22 + x^2 + x + 1
    // Taps are bit positions (0-indexed from LSB) that are XORed for feedback.
    static const unsigned int TAP_1 = 31; // Corresponds to x^32 (MSB is involved in feedback)
    static const unsigned int TAP_2 = 22; // Corresponds to x^22
    static const unsigned int TAP_3 = 2;  // Corresponds to x^2
    static const unsigned int TAP_4 = 1;  // Corresponds to x^1
                                          // Note: The '+ 1' term implies the LSB (bit 0) is the output.

  public:
    LFSR32() : _LFSR(0x29392939) {
    }

    LFSR32(unsigned int seed) : _LFSR(seed) {
        if (seed == 0) {
            _LFSR = 0x29392939;
        }
    }

    void tick() {
        // Calculate the feedback bit
        // XOR bits at the defined tap positions.
        // Get bit at position P: (register_ >> P) & 1
        unsigned int feedback_bit =
            ((_LFSR >> TAP_1) & 1) ^ ((_LFSR >> TAP_2) & 1) ^ ((_LFSR >> TAP_3) & 1) ^ ((_LFSR >> TAP_4) & 1);

        _LFSR >>= 1;

        // 4. Insert the feedback bit into the MSB position (bit 31)
        // Only set the bit if the feedback is 1
        if (feedback_bit == 1) {
            _LFSR |= (1U << 31);
        }
    }

    unsigned int get() const {
        return _LFSR;
    }
};

class HardBranchTable {
    struct Entry {
        uint64_t tag;
        uint64_t ctr;
    };

    struct Stats {
        uint64_t update_branches;
        uint64_t update_replace_success;
        uint64_t update_replace_failed;
        uint64_t update_decays;
        uint64_t update_decayed_to_zero;
        uint64_t update_saturated;
        uint64_t query_branches;
        uint64_t query_hard;
        uint64_t query_not_hard;
    };

  private:
    // Params
    constexpr static uint64_t capacity     = 128;
    constexpr static uint64_t num_ways     = 16;
    constexpr static uint64_t num_sets     = capacity / num_ways;
    constexpr static uint64_t tag_bits     = 23;
    constexpr static uint64_t ctr_bits     = 5;
    constexpr static uint64_t ctr_max      = (1 << ctr_bits) - 1;
    constexpr static uint64_t ctr_decay    = 1;
    constexpr static uint64_t decay_period = 20000;

    std::vector<LRUReplacer> _replacer;
    std::array<std::array<Entry, num_ways>, num_sets> _table;
    Stats _stats;
    uint64_t _retired_branches; // Used for decaying

    void saturated_update_ctr(uint64_t &ctr, int64_t delta) {
        if (delta > 0) {
            ctr = std::min(ctr + delta, ctr_max);
        } else {
            ctr = std::max((int64_t)ctr + delta, (int64_t)0);
        }
    }

    uint64_t get_index(Addr pc) {
        return (pc >> 2) & (num_sets - 1);
    }

    uint64_t get_tag(Addr pc) {
        return ((pc >> 2) ^ (pc >> 2 >> tag_bits)) & ((1 << tag_bits) - 1);
    }

  public:
    HardBranchTable() : _table(), _stats(), _retired_branches(0) {
        _replacer.resize(num_sets, LRUReplacer(num_ways));
    }

    void update(Addr branch_pc, bool pred_taken, bool tage_miss) {
        (void)pred_taken;
        // Update retire branches counter
        _retired_branches++;
        _stats.update_branches++;
        // trigger a decay if retired branch is enough
        if (_retired_branches >= decay_period) {
            _retired_branches = 0;
            _stats.update_decays++;
            for (auto &set : _table) {
                for (auto &entry : set) {
                    saturated_update_ctr(entry.ctr, -ctr_decay);
                    if (entry.ctr == 0) {
                        _stats.update_decayed_to_zero++;
                    }
                }
            }
        }

        if (!tage_miss) {
            // Update replacer
            auto index = get_index(branch_pc);
            auto &set  = _table[index];
            for (std::size_t i = 0; i < set.size(); i++) {
                if (set[i].tag == get_tag(branch_pc)) {
                    _replacer[index].touch(i);
                    return;
                }
            }
        }

        // Mispredited

        // Search through table
        auto index = get_index(branch_pc);
        auto &set  = _table[index];
        for (std::size_t i = 0; i < set.size(); i++) {
            if (set[i].tag == get_tag(branch_pc)) {
                saturated_update_ctr(set[i].ctr, 1);
                if (set[i].ctr == ctr_max) {
                    _stats.update_saturated++;
                }
                _replacer[index].touch(i);
                return;
            }
        }
        // Not found, add new entry
        auto alloc_idx      = 0U;
        bool found_low_conf = false;
        // Find zero confidence entry first
        for (std::size_t i = 0; i < set.size(); i++) {
            if (set[i].ctr == 0) {
                alloc_idx      = i;
                found_low_conf = true;
                break;
            }
        }
        if (!found_low_conf) {
            // No replace when all confidence
            _stats.update_replace_failed++;
            return;
        }
        set[alloc_idx] = {.tag = get_tag(branch_pc), .ctr = 1};
        _replacer[index].touch(alloc_idx);
        _stats.update_replace_success++;
    }

    std::pair<bool, std::size_t> is_hard_branch(Addr branch_pc) {
        _stats.query_branches++;
        // Search through table
        auto index = get_index(branch_pc);
        auto &set  = _table[index];
        for (std::size_t i = 0; i < set.size(); i++) {
            if (set[i].tag == get_tag(branch_pc)) {
                bool is_hard = set[i].ctr >= ctr_max / 4 * 3;
                if (is_hard) {
                    _stats.query_hard++;
                } else {
                    _stats.query_not_hard++;
                }
                return {is_hard, i}; // Consider saturated above this theshold
            }
        }
        // Not found, not hard branch
        _stats.query_not_hard++;
        return {false, 0};
    }

    int predictor_size() {
        int STORAGESIZE = 0;
        STORAGESIZE += capacity * (tag_bits + ctr_bits);         // _table
        STORAGESIZE += num_sets * num_ways * (num_ways - 1) / 2; // _replacer(LRU)
        STORAGESIZE += (int)ceil(log2((double)decay_period));    // retired_branches

        fprintf(stdout, " (Hard Branch Table %d) ", STORAGESIZE);
        return STORAGESIZE;
    }

    void dump_stats() {
        std::cout << "------------------------------------------- Hard Branch Table Stats "
                     "--------------------------------------\n";
        std::cout << std::left << std::setw(30) << "  Update branches: " << std::right << std::setw(20)
                  << _stats.update_branches << "\n";
        std::cout << std::left << std::setw(30) << "  Update replaces: " << std::right << std::setw(20)
                  << _stats.update_replace_success << "\n";
        std::cout << std::left << std::setw(30) << "  Update replaces failed: " << std::right << std::setw(20)
                  << _stats.update_replace_failed << "\n";
        std::cout << std::left << std::setw(30) << "  Update decays: " << std::right << std::setw(20)
                  << _stats.update_decays << "\n";
        std::cout << std::left << std::setw(30) << "  Update decayed to zero: " << std::right << std::setw(20)
                  << _stats.update_decayed_to_zero << "\n";
        std::cout << std::left << std::setw(30) << "  Update saturated: " << std::right << std::setw(20)
                  << _stats.update_saturated << "\n";
        std::cout << std::left << std::setw(30) << "  Query branches: " << std::right << std::setw(20)
                  << _stats.query_branches << "\n";
        std::cout << std::left << std::setw(30) << "  Query hard: " << std::right << std::setw(20) << _stats.query_hard
                  << "\n";
        std::cout << std::left << std::setw(30) << "  Query not hard: " << std::right << std::setw(20)
                  << _stats.query_not_hard << "\n";
        std::cout
            << "------------------------------------------------------------------------------------------------\n";
        std::cout << std::endl;
    }
};

class CorrelationTable {

    struct Entry {
        bool valid;
        uint64_t tag;
        bool dir;          // direction, 1 bit
        bool dir_changed;  // 1 bit
        uint32_t dir_conf; // < 32, 5 bit
        uint8_t useful;
    };
    struct Stats {
        uint64_t update_count;
        uint64_t update_allocate;
        uint64_t update_hit;
        uint64_t update_replacement;
        uint64_t update_inserts;
        uint64_t update_insert_failed;
        uint64_t update_dir_changed;
        uint64_t query_miss;
        uint64_t query_hit;
        uint64_t query_conf_saturated;
        uint64_t query_conf_low;
        uint64_t query_dir_changed;
        uint64_t query_high_conf_and_dir_changed;
    };

  private:
    // H2P-Load entangling building table
    constexpr static uint64_t table_sets_bits = 7;
    constexpr static uint64_t table_sets      = 1 << table_sets_bits;
    constexpr static uint64_t tag_bits        = 16;
    constexpr static uint64_t table_ways      = 2;
    constexpr static uint64_t dir_conf_bits   = 5;
    constexpr static uint32_t dir_conf_max    = (1 << dir_conf_bits) - 1;

    std::vector<std::vector<std::vector<Entry>>> _tables; // Set-associative
    std::vector<LRUReplacer> _replacer;
    LFSR32 LFSR;

    Stats _stats;           // Software stats
    std::size_t _table_num; // Software var

    uint64_t tag_hash(Addr branch_pc, Addr load_pc, uint64_t value) {
        uint64_t seed = 0x2939;
        seed ^= branch_pc >> 2;
        seed ^= load_pc << 4;
        seed ^= load_pc << 13;
        seed ^= value;
        seed ^= value << 7;
        seed ^= value << 19;
        return seed;
    }

    uint64_t index_hash(Addr branch_pc, Addr load_pc, uint64_t value) {
        uint64_t seed = 0x9527;
        seed ^= branch_pc >> 2;
        seed ^= load_pc >> 7;
        seed ^= load_pc;
        seed ^= value;
        seed ^= value / table_sets;
        return seed;
    }

    uint64_t get_tag(Addr branch_pc, Addr load_pc, uint64_t value) {
        return (tag_hash(branch_pc, load_pc, value) ^ (tag_hash(branch_pc, load_pc, value) / table_sets)) &
               ((1 << tag_bits) - 1);
    }
    uint64_t get_index(Addr branch_pc, Addr load_pc, uint64_t value) {
        return index_hash(branch_pc, load_pc, value) & (table_sets - 1);
    }
    uint64_t half_fold(uint64_t value) {
        return (value ^ (value >> 32)) & ((1ULL << 32) - 1);
    }

  public:
    CorrelationTable(std::size_t table_num) : _table_num(table_num) {
        // Init replacer
        _replacer.resize(table_sets, LRUReplacer(table_ways));
        // Initialize tables with the specified dimensions
        _tables.resize(table_num, std::vector<std::vector<Entry>>(table_sets, std::vector<Entry>(table_ways)));
    };

    std::tuple<bool, bool> is_conf_saturated(uint64_t table_idx, Addr pc, Addr load_pc, uint64_t value) {
        auto folded_load_pc = half_fold(load_pc); // we actually use 32 bits folded-pc
        auto folded_value   = half_fold(value);   // we actually use 32 bits folded-value

        auto index  = get_index(pc, folded_load_pc, folded_value);
        auto tag    = get_tag(pc, folded_load_pc, folded_value);
        auto &table = _tables[table_idx];
        auto &set   = table[index];
        for (std::size_t way = 0; way < set.size(); way++) {
            auto &table_entry = set[way];
            if (table_entry.tag == tag) {
                _stats.query_hit++;
                bool conf_saturated = table_entry.dir_conf == dir_conf_max;
                bool conf_high      = table_entry.dir_conf >= dir_conf_max / 4 * 3;
                bool dir_changed    = table_entry.dir_changed;
                if (conf_saturated) {
                    _stats.query_conf_saturated++;
                } else {
                    _stats.query_conf_low++;
                }
                if (dir_changed) {
                    _stats.query_dir_changed++;
                    if (conf_saturated) {
                        _stats.query_high_conf_and_dir_changed++;
                    }
                }
                return {conf_saturated, conf_high};
            }
        }
        _stats.query_miss++;
        return {false, false};
    }
    bool is_conf_useful_saturated(uint64_t table_idx, Addr pc, Addr load_pc, uint64_t value) {
        auto folded_load_pc = half_fold(load_pc); // we actually use 32 bits folded-pc
        auto folded_value   = half_fold(value);   // we actually use 32 bits folded-value

        auto index  = get_index(pc, folded_load_pc, folded_value);
        auto tag    = get_tag(pc, folded_load_pc, folded_value);
        auto &table = _tables[table_idx];
        auto &set   = table[index];
        for (std::size_t way = 0; way < set.size(); way++) {
            auto &table_entry = set[way];
            if (table_entry.tag == tag) {
                bool conf_saturated = table_entry.dir_conf == dir_conf_max;
                bool dir_changed    = table_entry.dir_changed;
                return conf_saturated && !dir_changed && table_entry.useful == 3;
            }
        }
        return false;
    }

    bool predict_taken(uint64_t table_idx, Addr pc, Addr load_pc, uint64_t value) {
        auto folded_load_pc = half_fold(load_pc); // we actually use 32 bits folded-pc
        auto folded_value   = half_fold(value);   // we actually use 32 bits folded-value

        auto index  = get_index(pc, folded_load_pc, folded_value);
        auto tag    = get_tag(pc, folded_load_pc, folded_value);
        auto &table = _tables[table_idx];
        auto &set   = table[index];
        for (std::size_t way = 0; way < set.size(); way++) {
            auto &table_entry = set[way];
            if (table_entry.tag == tag) {
                return table_entry.dir;
            }
        }
        assert(false && "predict_taken: no matching entry found");
        // Will not execute through here
    }

    void reset_useful(uint64_t table_idx, Addr pc, Addr load_pc, uint64_t value) {
        auto folded_load_pc = half_fold(load_pc); // we actually use 32 bits folded-pc
        auto folded_value   = half_fold(value);   // we actually use 32 bits folded-value

        auto index  = get_index(pc, folded_load_pc, folded_value);
        auto tag    = get_tag(pc, folded_load_pc, folded_value);
        auto &table = _tables[table_idx];
        auto &set   = table[index];
        for (std::size_t way = 0; way < set.size(); way++) {
            auto &table_entry = set[way];
            if (table_entry.tag == tag) {
                table_entry.useful   = 0;
                table_entry.dir_conf = 0;
            }
        }
    }

    bool update(uint64_t table_idx, Addr pc, Addr load_pc, uint64_t value, bool taken, bool tage_taken,
                bool allocate) {
        LFSR.tick();
        auto folded_load_pc = half_fold(load_pc); // we actually use 32 bits folded-pc
        auto folded_value   = half_fold(value);   // we actually use 32 bits folded-value

        auto index     = get_index(pc, folded_load_pc, folded_value);
        auto tag       = get_tag(pc, folded_load_pc, folded_value);
        bool match     = false;
        auto &table    = _tables[table_idx];
        auto &set      = table[index];
        auto match_way = 0;
        bool allocated = false;
        for (std::size_t way = 0; way < set.size(); way++) {
            auto &table_entry = set[way];
            if (table_entry.tag == tag) {
                match     = true;
                match_way = way;
            }
        }

        if (match) {
            _stats.update_hit++;
            // Update entry
            auto &entry = table[index][match_way];
            _replacer[index].touch(match_way);
            if (entry.dir == taken && !entry.dir_changed) {
                entry.dir_conf = std::min(entry.dir_conf + 1, dir_conf_max);
                if (tage_taken != taken) {
                    entry.useful = std::min(entry.useful + 1, 3);
                }
            } else {
                // reset if direction changed
                entry.dir         = taken;
                entry.dir_conf    = 0;
                entry.dir_changed = true;
                entry.useful      = 0;
                _stats.update_dir_changed++;
            }
        } else if (allocate) {
            auto replacer_list = _replacer[index].lru_list();
            // Try allocate
            for (auto iter = replacer_list.rbegin(); iter != replacer_list.rend(); ++iter) {
                auto &entry = table[index][*iter];
                if (entry.valid && entry.useful > 0) {
                    // Providing predict, useful
                    continue;
                }
                if (entry.valid && entry.dir_conf > dir_conf_max / 4) {
                    // Training
                    continue;
                }
                // Can be allocated
                if (entry.valid) {
                    _stats.update_replacement++;
                }
                entry = {.valid = true, .tag = tag, .dir = taken, .dir_changed = false, .dir_conf = 1};
                _stats.update_inserts++;
                allocated = true;
                break;
            }
            if (!allocated) {
                _stats.update_insert_failed++;
                // Decay useful
                for (auto &entry : table[index]) {
                    if (entry.valid && entry.useful > 0 && (LFSR.get() & 127) == 0) {
                        entry.useful--;
                    }
                }
            }
        }
        _stats.update_count++;
        if (allocate) {
            _stats.update_allocate++;
        }
        return allocated;
    }

    int predictor_size() {
        int STORAGESIZE = 0;
        // valid, tag, dir, dir_changed, dir_conf, useful
        STORAGESIZE += _table_num * table_sets * table_ways * (1 + tag_bits + 1 + 1 + dir_conf_bits + 2);
        // LRU
        STORAGESIZE += _table_num * table_sets * table_ways * (table_ways - 1) / 2; // _replacer(LRU)
        STORAGESIZE += 32;                                                          // LFSR

        fprintf(stdout, " (Correlation Table %d) ", STORAGESIZE);
        return STORAGESIZE;
    }

    void dump_stats() {
        uint64_t total_storage =
            _table_num * table_sets * table_ways * (1 + tag_bits + dir_conf_bits + 2 + 1); // dir, useful, dir_changed
        std::cout
            << "-------------------------------------- Correlation Table Stats ----------------------------------\n";
        std::cout << std::setw(40) << std::left << "  Total storage: " << std::setw(20) << std::right
                  << (double)total_storage / 1024 / 8 << " KB\n";
        std::cout << std::setw(40) << std::left << "  Total table entries: " << std::setw(20) << std::right
                  << _table_num * table_sets * table_ways << "\n";
        std::cout << std::setw(40) << std::left << "  Update count: " << std::setw(20) << std::right
                  << _stats.update_count << "\n";
        std::cout << std::setw(40) << std::left << "  Update hit: " << std::setw(20) << std::right << _stats.update_hit
                  << "\n";
        std::cout << std::setw(40) << std::left << "  Update allocate: " << std::setw(20) << std::right
                  << _stats.update_allocate << "\n";
        std::cout << std::setw(40) << std::left << "  Update dir changed: " << std::setw(20) << std::right
                  << _stats.update_dir_changed << "\n";
        std::cout << std::setw(40) << std::left << "  Update replacement: " << std::setw(20) << std::right
                  << _stats.update_replacement << "\n";
        std::cout << std::setw(40) << std::left << "  Update inserts: " << std::setw(20) << std::right
                  << _stats.update_inserts << "\n";
        std::cout << std::setw(40) << std::left << "  Update inserts failed: " << std::setw(20) << std::right
                  << _stats.update_insert_failed << "\n";
        std::cout << std::setw(40) << std::left << "  Query hit: " << std::setw(20) << std::right << _stats.query_hit
                  << "\n";
        std::cout << std::setw(40) << std::left << "  Query miss: " << std::setw(20) << std::right << _stats.query_miss
                  << "\n";
        std::cout << std::setw(40) << std::left << "  Query conf saturated: " << std::setw(20) << std::right
                  << _stats.query_conf_saturated << "\n";
        std::cout << std::setw(40) << std::left << "  Query conf low: " << std::setw(20) << std::right
                  << _stats.query_conf_low << "\n";
        std::cout << std::setw(40) << std::left << "  Query direction changed: " << std::setw(20) << std::right
                  << _stats.query_dir_changed << "\n";
        std::cout << std::setw(40) << std::left << "  Query high conf & direction changed: " << std::setw(20)
                  << std::right << _stats.query_high_conf_and_dir_changed << "\n";
        std::cout
            << "-------------------------------------------------------------------------------------------------\n";
    }
};

class LVCP {
  private:
    struct Stats {
        uint64_t branch_entangling_table_touch = 0; // When correlation table confidence reaches saturation, entangling
                                                    // table is touched
        uint64_t branch_entangling_table_insert                     = 0;
        uint64_t branch_entangling_table_load_pc_changed            = 0;
        uint64_t load_branch_entangling_table_touch                 = 0;
        uint64_t load_branch_entangling_table_insert                = 0;
        uint64_t load_branch_entangling_table_branch_pc_changed     = 0;
        uint64_t lvcp_predicted                                     = 0;
        uint64_t lvcp_predicted_by_value                            = 0;
        uint64_t lvcp_predicted_by_addr                             = 0;
        uint64_t lvcp_not_predicted                                 = 0; // Use TAGE predict as output
        uint64_t lvcp_not_predicted_due_to_confidence               = 0;
        uint64_t lvcp_not_predicted_due_to_entangling_table_miss    = 0;
        uint64_t lvcp_not_predicted_due_to_correlation_table_miss   = 0;
        uint64_t lvcp_not_predicted_due_to_load_value_tracking_miss = 0;
        uint64_t lvcp_no_load_or_value_late                         = 0;
        uint64_t lvcp_load_value_provided_by_dvtage                 = 0;
        uint64_t lvcp_not_predicted_due_to_tage_highconf            = 0;
        uint64_t lvcp_not_predicted_due_to_low_corr_confidence      = 0;
        uint64_t lvcp_correct                                       = 0;
        uint64_t lvcp_incorrect                                     = 0;
        uint64_t lvcp_correct_tage_incorrect                        = 0;
        uint64_t lvcp_correct_tage_correct                          = 0;
        uint64_t lvcp_incorrect_tage_correct                        = 0;
        uint64_t lvcp_incorrect_tage_incorrect                      = 0;
        uint64_t load_bitmap_potential_miss                         = 0;
        uint64_t load_bitmap_potential_hit                          = 0;
        uint64_t load_bitmap_max_usage                              = 0;
    };

    struct LoadTrackingEntry {
        uint64_t id;
        Addr pc;
        uint64_t prev_taken_br_id;
    };

  struct PrevBrInfo {
    uint64_t id;
    Addr br_pc;
    Addr target;
  };

    struct Bitmap {
        std::vector<bool> map;
        uint8_t useful;
    };

    Stats stats; // Software stats

    CorrelationTable correlation_table = CorrelationTable(load_tracking_queue_size);
    HardBranchTable hbt;
    LFSR32 LFSR;
    PrevBrInfo pred_prev_taken_br_info;
    PrevBrInfo pred_prev_br_info;

    std::deque<LoadTrackingEntry> load_tracking_queue;
    std::vector<LoadTrackingEntry> distant_load_buffer;
    std::deque<LoadTrackingEntry> commit_load_tracking_queue;
    std::vector<LoadTrackingEntry> commit_distant_load_buffer;
    std::vector<std::unordered_map<uint64_t, Bitmap>> load_marking_table; // per cache line
    std::unordered_map<uint64_t, uint64_t> load_value_map;                // ID -> value

    constexpr static uint64_t bitmap_decay_interval = 10000;
    uint64_t bitmap_num_entries                     = ICACHE_SIZE / ICACHE_LINE_SIZE / 2;
    uint64_t bitmap_num_sets                        = bitmap_num_entries / ICACHE_ASSOC;
    constexpr static uint64_t bitmap_tag_width      = 16;
    constexpr static uint8_t USEFUL_WIDTH           = 3;
    constexpr static uint8_t USEFUL_MAX             = (1 << USEFUL_WIDTH) - 1;

    struct PredTimeHistory {
        bool pred_by_lvcp;
    bool lvcp_pred_taken;
    bool tage_pred_taken;
    bool lvcp_nearly_provided;
    bool lvcp_nearly_provided_pred;
    Addr lvcp_pred_load_pc;
    uint64_t lvcp_pred_load_value;
    uint64_t lvcp_pred_distance;
  };
    std::unordered_map<uint64_t, PredTimeHistory> pred_time_histories; // only for software stats, not used in hardware

  public:
    LVCP() {
        distant_load_buffer.resize(load_tracking_table_size);
        commit_distant_load_buffer.resize(load_tracking_table_size);
        load_marking_table.resize(bitmap_num_sets);
        predictor_size();
    }

    void setup();

    void terminate() {
        dump_stats();
    }

    void dump_stats();

    // sample function to get unique instruction id
    uint64_t get_load_inst_buffer_idx(Addr pc) {
        return ((pc & ~((Addr)0x3)) / ICACHE_LINE_SIZE) & (ICACHE_SIZE / ICACHE_LINE_SIZE - 1);
    }

    uint64_t get_load_inst_bitmap_set_idx(Addr pc) {
        return ((pc & ~((Addr)0x3)) / ICACHE_LINE_SIZE) & (bitmap_num_sets - 1);
    }

    uint64_t get_load_inst_bitmap_tag(Addr pc) {
        return ((pc & ~((Addr)0x3)) / ICACHE_LINE_SIZE / bitmap_num_sets) & ((1 << bitmap_tag_width) - 1);
    }

    uint64_t get_block_aligned_pc(Addr pc) {
        return (pc & ~((Addr)0x3)) & ~(ICACHE_LINE_SIZE - 1);
    }

    void try_find_load_instr_on_path_with_br(Addr pc);

    void notify_predtime_load_instr(Addr pc); // enqueue pred_load_queue

    bool GetPrediction(Addr PC, int* bp_confidence, Op* op);

    // void spec_update(Addr pc, Op* op, const bool resolve_dir, const bool pred_dir, const Addr next_pc);

    void HistoryUpdate(Addr PC, OpType opType, bool resolveDir, Addr branchTarget, Counter id);

    void UpdatePredictor(Addr PC, OpType opType, bool resolveDir, bool predDir, Addr branchTarget, Op* op);

    void TrackOtherInst(Addr PC, OpType opType, bool resolveDir, Addr branchTarget, Op* op);

    void notify_instr_decode(Addr pc, Op* op);

    void notify_agen_complete(Addr pc, Op* op, uint64_t mem_va, uint64_t mem_sz);

    void notify_load_instr_execute_resolve(Addr pc, Op* op);

    void notify_instr_commit(Addr pc, Op* op);
    void cond_branch_update(Addr pc, Op* op, const bool pred_dir);

    uns8 IsFull() {
        return 0;
    }

    int predictor_size() {
        int STORAGESIZE = 0;
        int inter       = 0;

        // hard branch table
        STORAGESIZE += hbt.predictor_size();
        // correlation table
        STORAGESIZE += correlation_table.predictor_size();
        // lvcp
        inter += 2 * (10 + 64 + 64); // pred_prev_taken_br_info, pred_prev_br_info, 2 * (id, br_pc, target)
        // TODO: merge load value into pred_load_queue
        inter += load_tracking_queue_size *
                 (10 + 32 + 10 + 32); // load_tracing_queue, (id, pc, prev_taken_br_id, load_value)
        inter += load_tracking_table_size * (10 + 32 + 32); // distant_load_buffer, (id, pc, load_value)
        inter += load_tracking_table_size * (10 + 32);      // commit_load_tracking_queue, (id, pc)
        inter += load_tracking_table_size * (10 + 32);      // commit_distant_load_buffer, (id, pc)
        inter += bitmap_num_entries *
                 (bitmap_tag_width + ICACHE_LINE_SIZE / 4 + USEFUL_WIDTH); // load_marking_table, (tag, bitmap, useful)
        inter += (int)ceil(log2((double)bitmap_decay_interval));       // decay counter
        inter += (int)ceil(log2((double)bitmap_num_sets));             // decay index
        inter += 32;                                                   // LFSR
        STORAGESIZE += inter;
        fprintf(stdout, " (Load Value Correlated Predictor Top %d) ", inter);
        fprintf(stdout, "\n (Load Value Correlated Predictor TOTAL %lf KB) \n", (double)STORAGESIZE / 1024 / 8);
        return STORAGESIZE;
    }
};
// =================
// Predictor End
// =================
#endif
