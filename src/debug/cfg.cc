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

/***************************************************************************************
 * File         : debug/cfg.cc
 * Description  : Control Flow Graph collector implementation.
 *
 * Basic-block (BB) identification at retirement:
 *   - A BB starts at the instruction following any CF instruction (or at the
 *     first retired instruction of the simulation).
 *   - A BB ends at the CF instruction that terminates it.
 *   - Node key  : start_pc of the BB.
 *   - Edge key  : (from_bb_start_pc, to_bb_start_pc) hashed into uint64_t.
 *
 * Multi-uop instructions: only the last uop (eom==TRUE) of a CF instruction
 * triggers a BB transition, ensuring we see the finalised oracle_info.npc.
 ***************************************************************************************/

#include "debug/cfg.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

#include "globals/global_defs.h" /* MAX_NUM_PROCS, Addr, uns, ... */
#include "globals/global_types.h"
#include "globals/utils.h" /* MAX_STR_LENGTH */

#include "op.h"         /* Op, Op_Info, Table_Info, Cf_Type, NOT_CF */
#include "table_info.h" /* Cf_Type names */

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */

struct CfgNode {
  Addr start_pc;   /* first instruction PC of this BB                */
  Addr end_pc;     /* PC of the CF instruction that ends the BB      */
  Cf_Type cf_type; /* control-flow type of the terminating CF        */
  uint64_t count;  /* number of times this BB was retired through    */

  /* Per-uop pipeline stage cycle accumulators (summed over all uops in   *
   * all executions of this BB).  Divide by uop_count for per-uop avg.   */
  uint64_t uop_count;     /* total uops retired through this BB           */
  uint64_t inst_count;    /* total instructions (eom ops) retired         */
  uint64_t fetch_cycles;  /* sum of (decode_cycle  - fetch_cycle)         */
  uint64_t decode_cycles; /* sum of (map_cycle     - decode_cycle)        */
  uint64_t map_cycles;    /* sum of (issue_cycle   - map_cycle)           */
  uint64_t issue_cycles;  /* sum of (sched_cycle   - issue_cycle)  RS wait*/
  uint64_t exec_cycles;   /* sum of (done_cycle    - sched_cycle)         */
  uint64_t rob_cycles;    /* sum of (retire_cycle  - done_cycle)   ROB wait*/
};

struct CfgEdge {
  Addr from_pc;    /* BB start PC of source BB                       */
  Addr to_pc;      /* BB start PC of target BB (oracle_info.npc)     */
  Cf_Type cf_type; /* same cf_type as source node (for easy filter)  */
  uint64_t count;
};

/* Per-proc state */
static Addr current_bb_start[MAX_NUM_PROCS];
static uint64_t total_retired_cf[MAX_NUM_PROCS];

static std::unordered_map<Addr, CfgNode> cfg_nodes[MAX_NUM_PROCS];
static std::unordered_map<uint64_t, CfgEdge> cfg_edges[MAX_NUM_PROCS];

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Knuth multiplicative hash for (from_pc, to_pc) → 64-bit key */
static inline uint64_t edge_key(Addr from_pc, Addr to_pc) {
  return (from_pc * UINT64_C(2654435761)) ^ to_pc;
}

static const char* cf_type_name(Cf_Type t) {
  switch (t) {
    case NOT_CF:
      return "NOT_CF";
    case CF_BR:
      return "CF_BR";
    case CF_CBR:
      return "CF_CBR";
    case CF_REP:
      return "CF_REP";
    case CF_CALL:
      return "CF_CALL";
    case CF_IBR:
      return "CF_IBR";
    case CF_ICALL:
      return "CF_ICALL";
    case CF_ICO:
      return "CF_ICO";
    case CF_RET:
      return "CF_RET";
    case CF_SYS:
      return "CF_SYS";
    default:
      return "UNKNOWN";
  }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void cfg_init(void) {
  for (uns p = 0; p < MAX_NUM_PROCS; p++) {
    current_bb_start[p] = 0;
    total_retired_cf[p] = 0;
    cfg_nodes[p].clear();
    cfg_edges[p].clear();
  }
}

void cfg_track_inst(Op* op) {
  uns proc_id = op->proc_id;
  if (current_bb_start[proc_id] == 0)
    current_bb_start[proc_id] = op->inst_info->addr;
}

void cfg_accum_uop(Op* op) {
  uns proc_id = op->proc_id;
  if (current_bb_start[proc_id] == 0)
    return;

  auto& node = cfg_nodes[proc_id][current_bb_start[proc_id]];

  node.uop_count++;
  if (op->eom)
    node.inst_count++;

/* Helper: accumulate a stage delta only when both stamps are valid and  *
 * the end stamp is >= the start stamp (guards against unset fields).    */
#define ACCUM_STAGE(field, start, end) \
  if ((end) > 0 && (end) >= (start))   \
    node.field += (end) - (start);

  ACCUM_STAGE(fetch_cycles, op->fetch_cycle, op->decode_cycle)
  ACCUM_STAGE(decode_cycles, op->decode_cycle, op->map_cycle)
  ACCUM_STAGE(map_cycles, op->map_cycle, op->issue_cycle)
  ACCUM_STAGE(issue_cycles, op->issue_cycle, op->sched_cycle)
  ACCUM_STAGE(exec_cycles, op->sched_cycle, op->done_cycle)
  ACCUM_STAGE(rob_cycles, op->done_cycle, op->retire_cycle)

#undef ACCUM_STAGE
}

void cfg_retire_op(Op* op) {
  uns proc_id = op->proc_id;
  Addr bb_start = current_bb_start[proc_id];
  Addr bb_end = op->inst_info->addr;  /* PC of the CF instr      */
  Addr next_pc = op->oracle_info.npc; /* true architectural NPC  */
  Cf_Type cf = op->table_info->cf_type;

  /* --- update / insert node ------------------------------------------- */
  auto& node = cfg_nodes[proc_id][bb_start];
  if (node.count == 0) {
    node.start_pc = bb_start;
    node.cf_type = cf;
  }
  node.end_pc = bb_end; /* update in case warmup shifted the first entry */
  node.count++;

  /* --- update / insert edge ------------------------------------------- */
  uint64_t ek = edge_key(bb_start, next_pc);
  auto& edge = cfg_edges[proc_id][ek];
  if (edge.count == 0) {
    edge.from_pc = bb_start;
    edge.to_pc = next_pc;
    edge.cf_type = cf;
  }
  edge.count++;

  total_retired_cf[proc_id]++;

  /* next BB starts where execution actually continues */
  current_bb_start[proc_id] = next_pc;
}

void cfg_dump(const char* output_dir) {
  for (uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    if (cfg_nodes[proc_id].empty())
      continue;

    /* Build filename: cfg_data.json for proc 0, cfg_data_1.json for proc 1, etc. */
    char fname[64];
    if (proc_id == 0)
      snprintf(fname, sizeof(fname), "cfg_data.json");
    else
      snprintf(fname, sizeof(fname), "cfg_data_%u.json", proc_id);

    char fpath[MAX_STR_LENGTH + 1];
    snprintf(fpath, sizeof(fpath), "%s/%s", output_dir, fname);
    FILE* f = fopen(fpath, "w");
    if (!f) {
      fprintf(stderr, "[cfg] ERROR: could not open %s for writing\n", fpath);
      continue;
    }

    /* --- metadata ------------------------------------------------------- */
    fprintf(f, "{\n");
    fprintf(f, "  \"metadata\": {\n");
    fprintf(f, "    \"proc_id\": %u,\n", proc_id);
    fprintf(f, "    \"total_retired_cf\": %" PRIu64 ",\n", total_retired_cf[proc_id]);
    fprintf(f, "    \"num_nodes\": %zu,\n", cfg_nodes[proc_id].size());
    fprintf(f, "    \"num_edges\": %zu\n", cfg_edges[proc_id].size());
    fprintf(f, "  },\n");

    /* --- nodes ---------------------------------------------------------- */
    fprintf(f, "  \"nodes\": [\n");
    bool first_node = true;
    for (const auto& kv : cfg_nodes[proc_id]) {
      const CfgNode& n = kv.second;
      if (!first_node)
        fprintf(f, ",\n");
      fprintf(f,
              "    {\"start_pc\": \"0x%" PRIx64
              "\","
              " \"end_pc\": \"0x%" PRIx64
              "\","
              " \"cf_type\": \"%s\","
              " \"count\": %" PRIu64
              ","
              " \"uop_count\": %" PRIu64
              ","
              " \"inst_count\": %" PRIu64
              ","
              " \"fetch_cycles\": %" PRIu64
              ","
              " \"decode_cycles\": %" PRIu64
              ","
              " \"map_cycles\": %" PRIu64
              ","
              " \"issue_cycles\": %" PRIu64
              ","
              " \"exec_cycles\": %" PRIu64
              ","
              " \"rob_cycles\": %" PRIu64 "}",
              (uint64_t)n.start_pc, (uint64_t)n.end_pc, cf_type_name(n.cf_type), n.count, n.uop_count, n.inst_count,
              n.fetch_cycles, n.decode_cycles, n.map_cycles, n.issue_cycles, n.exec_cycles, n.rob_cycles);
      first_node = false;
    }
    fprintf(f, "\n  ],\n");

    /* --- edges ---------------------------------------------------------- */
    fprintf(f, "  \"edges\": [\n");
    bool first_edge = true;
    for (const auto& kv : cfg_edges[proc_id]) {
      const CfgEdge& e = kv.second;
      if (!first_edge)
        fprintf(f, ",\n");
      fprintf(f,
              "    {\"from_pc\": \"0x%" PRIx64
              "\","
              " \"to_pc\": \"0x%" PRIx64
              "\","
              " \"cf_type\": \"%s\","
              " \"count\": %" PRIu64 "}",
              (uint64_t)e.from_pc, (uint64_t)e.to_pc, cf_type_name(e.cf_type), e.count);
      first_edge = false;
    }
    fprintf(f, "\n  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    printf("[cfg] Wrote %zu nodes, %zu edges → %s/%s\n", cfg_nodes[proc_id].size(), cfg_edges[proc_id].size(),
           output_dir, fname);
  }
}
