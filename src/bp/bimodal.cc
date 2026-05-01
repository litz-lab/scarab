/* Copyright 2026 Litz Lab
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

#include "bp/bimodal.h"

#include <vector>

#include "bp/cbp_to_scarab.h"

extern "C" {
#include "globals/assert.h"
#include "globals/utils.h"

#include "bp/bp.param.h"
#include "core.param.h"

#include "statistics.h"
}

#define PHT_INIT_VALUE (0x1 << (PHT_CTR_BITS - 1)) /* weakly taken */
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_BP_DIR, ##args)

namespace {

struct Bimodal_State {
  std::vector<uns8> pht;
};

std::vector<Bimodal_State> bimodal_state_all_cores;

inline uns32 get_pht_index(const Addr addr) {
  return (addr ^ (addr >> 2)) & (BIMODAL_ENTRIES - 1);
}

inline void update_pht_entry(std::vector<uns8>& pht, const uns32 pht_index, const Flag dir) {
  const uns8 pht_entry = pht[pht_index];
  if (dir) {
    pht[pht_index] = SAT_INC(pht_entry, N_BIT_MASK(PHT_CTR_BITS));
  } else {
    pht[pht_index] = SAT_DEC(pht_entry, 0);
  }
}

}  // namespace

void bp_bimodal_timestamp(Op* op) {
  (void)op;
}

void bp_bimodal_recover(Recovery_Info* info) {
  (void)info;
}

void bp_bimodal_spec_update(Op* op, Bp_Pred_Level pred_level) {
  (void)op;
  (void)pred_level;
}

void bp_bimodal_retire(Op* op) {
  (void)op;
}

uns8 bp_bimodal_full(Bp_Data* bp_data) {
  return 0;
}

void bp_bimodal_init() {
  ASSERTM(0, is_power_of_2(BIMODAL_ENTRIES), "BIMODAL_ENTRIES must be power-of-two\n");
  bimodal_state_all_cores.resize(NUM_CORES);
  for (auto& bimodal_state : bimodal_state_all_cores) {
    bimodal_state.pht.resize(BIMODAL_ENTRIES, PHT_INIT_VALUE);
  }
}

uns8 bp_bimodal_pred(Op* op, Bp_Pred_Level pred_level) {
  (void)pred_level;
  if (op->off_path && SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_N_ON) {
    return op->oracle_info.dir;
  }

  const uns proc_id = op->proc_id;
  const auto& bimodal_state = bimodal_state_all_cores.at(proc_id);

  const Addr addr = op->inst_info->addr;
  const uns32 pht_index = get_pht_index(addr);
  const uns8 pht_entry = bimodal_state.pht[pht_index];
  const uns8 pred = pht_entry >> (PHT_CTR_BITS - 1) & 0x1;

  DEBUG(proc_id, "Predicting with bimodal for op_num:%s index:%u\n", unsstr64(op->op_num), pht_index);
  DEBUG(proc_id, "Predicting addr:%s pht:%u pred:%u dir:%u\n", hexstr64s(addr), pht_index, pred, op->oracle_info.dir);

  return pred;
}

void bp_bimodal_update(Op* op, Bp_Pred_Level pred_level) {
  (void)pred_level;
  if (op->inst_info->table_info.cf_type != CF_CBR && op->inst_info->table_info.cf_type != CF_REP) {
    // If op is not a conditional branch/REP, we do not interact with bimodal.
    return;
  }

  // Train at resolve stage only.
  if (op->off_path) {
    return;
  }

  const uns proc_id = op->proc_id;
  auto& bimodal_state = bimodal_state_all_cores.at(proc_id);
  const Addr addr = op->inst_info->addr;
  const uns32 pht_index = get_pht_index(addr);

  DEBUG(proc_id, "Writing bimodal PHT for op_num:%s index:%u dir:%u\n", unsstr64(op->op_num), pht_index,
        op->oracle_info.dir);

  update_pht_entry(bimodal_state.pht, pht_index, op->oracle_info.dir);

  DEBUG(proc_id, "Updating addr:%s pht:%u ent:%u dir:%u\n", hexstr64s(addr), pht_index, bimodal_state.pht[pht_index],
        op->oracle_info.dir);
}
