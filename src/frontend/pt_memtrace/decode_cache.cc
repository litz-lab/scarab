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

#include "frontend/pt_memtrace/decode_cache.h"

#include <unordered_map>

#include "globals/assert.h"

static std::unordered_map<uint64_t, DecodedInst> decode_cache[MAX_NUM_PROCS];

DecodedInst* decode_cache_find(uns proc_id, uint64_t pc) {
  auto it = decode_cache[proc_id].find(pc);
  return it == decode_cache[proc_id].end() ? nullptr : &it->second;
}

DecodedInst* decode_cache_insert(uns proc_id, uint64_t pc, const ctype_pin_inst_static& inst, int num_mem_ops,
                                 bool unknown_type, bool is_rep) {
  auto inserted = decode_cache[proc_id].emplace(pc, DecodedInst{inst, num_mem_ops, unknown_type, is_rep});
  ASSERT(proc_id, inserted.second);
  return &inserted.first->second;
}

void decode_cache_erase(uns proc_id, uint64_t pc) {
  decode_cache[proc_id].erase(pc);
}
