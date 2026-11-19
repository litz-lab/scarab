/*
 * Copyright 2026 University of California Santa Cruz
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

/***************************************************************************************
 * File         : decode_cache.cc
 * Author       : Litz Lab
 * Date         : 11/2026
 * Description  : Per-core cache of decoded trace instructions, keyed by PC
 ***************************************************************************************/

#include "frontend/decode_cache.h"

#include <cstddef>
#include <unordered_map>

#include "globals/assert.h"
#include "globals/utils.h"

#include "statistics.h"

// {PC, instruction bytes}: the first and last 8 bytes and the size, as fill_in_basic_info stores them
struct EncodingKey {
  uint64_t pc, lsb, msb;
  uint8_t size;
  bool operator==(const EncodingKey& o) const { return pc == o.pc && lsb == o.lsb && msb == o.msb && size == o.size; }
};
struct EncodingKeyHash {
  size_t operator()(const EncodingKey& k) const {
    uint64_t h = k.pc * 0x9E3779B97F4A7C15ULL;
    h ^= k.lsb + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= k.msb + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h ^ k.size;
  }
};
static_assert(offsetof(DecodedInst, inst) == 0, "InstInfo aliases DecodedInst* with its inst");
static std::unordered_map<EncodingKey, DecodedInst, EncodingKeyHash> encodings[MAX_NUM_PROCS];
static std::unordered_map<uint64_t, DecodedInst*> current[MAX_NUM_PROCS];  // each PC's current encoding

DecodedInst* decode_cache_find(uns proc_id, uint64_t pc) {
  auto it = current[proc_id].find(pc);
  return it == current[proc_id].end() ? nullptr : it->second;
}

DecodedInst* decode_cache_find_encoding(uns proc_id, uint64_t pc, uint64_t lsb, uint64_t msb, uint8_t size) {
  auto it = encodings[proc_id].find({pc, lsb, msb, size});
  return it == encodings[proc_id].end() ? nullptr : &it->second;
}

void decode_cache_make_current(uns proc_id, uint64_t pc, DecodedInst* decoded) {
  DecodedInst*& cur = current[proc_id][pc];
  if (cur && cur != decoded)
    STAT_EVENT(proc_id, DECODE_CACHE_REENCODE);
  cur = decoded;
}

DecodedInst* decode_cache_insert(uns proc_id, uint64_t pc, const DecodedInst& decoded) {
  const ctype_pin_inst_static& st = decoded.inst;
  auto inserted = encodings[proc_id].emplace(EncodingKey{pc, st.inst_binary_lsb, st.inst_binary_msb, st.size}, decoded);
  ASSERT(proc_id, inserted.second);
  DecodedInst* d = &inserted.first->second;
  d->inst.instruction_addr = convert_to_cmp_addr(proc_id, pc);
  decode_cache_make_current(proc_id, pc, d);
  return d;
}

DecodedInst* decode_cache_static(uns proc_id, const ctype_pin_inst_static* st) {
  uint64_t pc = st->instruction_addr;  // the trace PC, not yet convert_to_cmp_addr'd
  DecodedInst* d = decode_cache_find_encoding(proc_id, pc, st->inst_binary_lsb, st->inst_binary_msb, st->size);
  if (d)
    return d;
  DecodedInst fresh = {};
  fresh.inst = *st;
  return decode_cache_insert(proc_id, pc, fresh);
}

const ctype_pin_inst_static* decode_cache_entry_static(const DecodedInst* decoded) {
  return &decoded->inst;
}

Uop_Variant* decode_cache_uops(DecodedInst* decoded, uint16_t mem_shape, uns8 cf_type) {
  Uop_Variant* first = &decoded->uops;
  if (first->num_uop == 0 || (first->mem_shape == mem_shape && first->cf_type == cf_type)) {
    first->mem_shape = mem_shape;
    first->cf_type = cf_type;
    return first;
  }
  if (!decoded->more_uops)
    decoded->more_uops = new std::vector<Uop_Variant>();  // lives as long as the entry
  for (Uop_Variant& v : *decoded->more_uops)
    if (v.mem_shape == mem_shape && v.cf_type == cf_type)
      return &v;
  decoded->more_uops->push_back(Uop_Variant{mem_shape, cf_type, 0, {}});
  return &decoded->more_uops->back();
}
