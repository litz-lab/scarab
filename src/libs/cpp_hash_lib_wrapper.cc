#include "libs/cpp_hash_lib_wrapper.h"

#include <iostream>
#include <unordered_map>
#include <vector>

extern "C" {
#include "globals/global_defs.h"
}

struct key {
  uint64_t addr;
  uint64_t lsb_bytes;
  uint64_t msb_bytes;
  uint8_t op_idx;

  key() : addr(0), lsb_bytes(0), msb_bytes(0), op_idx(0){};
  key(uint64_t _addr, uint64_t _lsb_bytes, uint64_t _msb_bytes, uint8_t _op_idx)
      : addr(_addr), lsb_bytes(_lsb_bytes), msb_bytes(_msb_bytes), op_idx(_op_idx){};

  bool operator==(const key &p) const {
    return addr == p.addr && lsb_bytes == p.lsb_bytes && msb_bytes == p.msb_bytes && op_idx == p.op_idx;
  }
};

// The specialized hash function for `unordered_map` keys
struct hash_fn {
  // template <class T1, class T2>
  std::size_t operator()(const key &key) const {
    // Knuth multiplicative hashing to reduce collisions for similar keys.
    // Constant is derived from the golden ratio: 2^64 / phi.
    const uint64_t KNUTH64 = 11400714819323198485ull;

    uint64_t h = key.addr;
    h *= KNUTH64;
    h += key.lsb_bytes;
    h *= KNUTH64;
    h += key.msb_bytes;
    h *= KNUTH64;
    h += static_cast<uint64_t>(key.op_idx);

    // Mix down to size_t in a platform-agnostic way.
    return static_cast<std::size_t>(h ^ (h >> 32));
  }
};

// Per-core hash maps for instruction info
std::unordered_map<key, Inst_Info, hash_fn> per_core_hash_map[MAX_NUM_PROCS];

Inst_Info *cpp_hash_table_access_create(int core, uint64_t addr, uint64_t lsb_bytes, uint64_t msb_bytes, uint8_t op_idx,
                                        unsigned char *new_entry) {
  *new_entry = false;
  key _key(addr, lsb_bytes, msb_bytes, op_idx);
  auto &hash_map = per_core_hash_map[core];
  auto lookup = hash_map.find(_key);
  if (lookup != hash_map.end()) {
    return &lookup->second;
  } else {
    auto inserted = hash_map.emplace(_key, Inst_Info{});
    *new_entry = inserted.second;
    return &inserted.first->second;
  }
}
