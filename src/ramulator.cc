/* Copyright 2020 HPS/SAFARI Research Groups
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
 * File         : ramulator.cc
 * Author       : SAFARI research group
 * Date         : 6/12/2018
 * Description  : Defines an interface to Ramulator
 ***************************************************************************************/

#include <deque>
#include <list>
#include <map>
#include <utility>

extern "C" {
#include "globals/assert.h"

#include "general.param.h"
#include "memory/memory.param.h"
#include "ramulator.param.h"

#include "memory/memory.h"

#include "ramulator.h"
#include "statistics.h"
}

#include "dram_backend.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MEMORY, ##args)

/**************************************************************************************/

DramBackend* backend = NULL;

void enqueue_response(long addr);

std::deque<std::pair<long, Mem_Req*>> resp_queue;  // completed reads and acknowledged writes that need to
                                                   // be sent back to Scarab

std::map<long, std::list<Mem_Req*>> inflight_read_reqs;

/* Request accounting, printed at finish: every read the backend accepted completes or is
   still in flight, and every acknowledged write reaches Scarab or is still queued. */
Counter glue_reads_sent = 0, glue_reads_completed = 0;
Counter glue_writes_sent = 0, glue_writes_acked = 0;

void ramulator_init() {
  ASSERTM(0, ICACHE_LINE_SIZE == DCACHE_LINE_SIZE,
          "Ramulator"
          "integration currently support only equal instruction and"
          "data cache line sizes! Currently, ICACHE_LINE_SIZE=%d, "
          "DCACHE_LINE_SIZE=%d \n",
          ICACHE_LINE_SIZE, DCACHE_LINE_SIZE);
  /* Every read in DRAM holds an l1_mshr entry, so the file bounds each channel's read
     queue and a read is never refused. */
  ASSERTM(0, L1_MSHRS <= RAMULATOR_READQ_ENTRIES, "L1_MSHRS=%u exceeds RAMULATOR_READQ_ENTRIES=%u\n", L1_MSHRS,
          RAMULATOR_READQ_ENTRIES);

  backend = ramulator1_backend_create(&enqueue_response);

  DPRINTF("Initialized Ramulator. \n");
}

void ramulator_finish() {
  backend->finish();

  Counter writes_queued = 0;
  for (auto& resp : resp_queue)
    writes_queued += resp.second->type == MRT_WB;
  DPRINTF("DRAM glue: reads sent %llu completed %llu inflight %zu; writes sent %llu acked %llu inflight %llu\n",
          glue_reads_sent, glue_reads_completed, inflight_read_reqs.size(), glue_writes_sent, glue_writes_acked,
          writes_queued);

  delete backend;
}

int ramulator_send(Mem_Req* scarab_req) {
  ASSERTM(scarab_req->proc_id, scarab_req->state == MRS_MEM_NEW,
          "A"
          " request in state %d cannot be issued to Ramulator\n",
          scarab_req->state);

  // only MRT_WB should result in a DRAM write. A plain store miss should still
  // result in a DRAM read
  bool is_write = false;
  if (scarab_req->type == MRT_WB)
    is_write = true;
  else if (scarab_req->type == MRT_DFETCH || scarab_req->type == MRT_DSTORE || scarab_req->type == MRT_IFETCH ||
           scarab_req->type == MRT_IPRF || scarab_req->type == MRT_DPRF || scarab_req->type == MRT_UOCPRF ||
           scarab_req->type == MRT_FDIPPRFON || scarab_req->type == MRT_FDIPPRFOFF ||
           scarab_req->type == MRT_FDIPPRFALT)
    is_write = false;
  else
    ASSERTM(scarab_req->proc_id, false, "Ramulator: Currently unsupported Scarab request type: %d\n", scarab_req->type);

  long addr = scarab_req->phys_addr;

  // printf("Ramulator: Received a (%s) request to address %llu\n",
  // Mem_Req_Type_str(scarab_req->type), scarab_req->addr);

  // does inflight_read_reqs have the proc_id in the req?
  auto it_scarab_req = inflight_read_reqs.find(addr);
  if (it_scarab_req != inflight_read_reqs.end() && !is_write) {
    DEBUG(scarab_req->proc_id, "Ramulator: Duplicate (%s) request to address %llx\n",
          Mem_Req_Type_str(scarab_req->type), scarab_req->addr);
    // Can have duplicate Ifetch and Dfetch requests, but only one of each
    ASSERT(0, it_scarab_req->second.size() <= 1);

    /* save it as an inflight request so later it will be moved to the resp_queue
     * at the same time with the older request */
    inflight_read_reqs[addr].push_back(scarab_req);

    scarab_req->mem_queue_cycle = cycle_count;
    return true;  // a request to the same address is already issued
  }

  bool is_sent = backend->send(addr, is_write, scarab_req->proc_id);
  ASSERTM(scarab_req->proc_id, is_sent || is_write,
          "Ramulator refused a read the l1_mshr bound should have made room for\n");

  if (is_sent) {
    STAT_EVENT(scarab_req->proc_id, POWER_MEMORY_CTRL_ACCESS);

    if (!is_write) {
      ASSERTM(0, inflight_read_reqs.find(addr) == inflight_read_reqs.end(),
              "ERROR: A read request to the same address shouldn't be sent "
              "multiple times to Ramulator\n");
      inflight_read_reqs[addr].push_back(scarab_req);
      STAT_EVENT(scarab_req->proc_id, POWER_MEMORY_CTRL_READ);
      glue_reads_sent++;
    } else {
      STAT_EVENT(scarab_req->proc_id, POWER_MEMORY_CTRL_WRITE);
      /* Writes are posted: acknowledged once the controller accepts them, returning the way read data does. */
      resp_queue.push_back(std::make_pair(addr, scarab_req));
      glue_writes_sent++;
    }

    scarab_req->mem_queue_cycle = cycle_count;
  }

  if (is_sent) {
    DEBUG(scarab_req->proc_id, "Ramulator: The request has been enqueued.\n");
  } else {
    DEBUG(scarab_req->proc_id, "Ramulator: The request has been rejected. Queue full?\n");
  }

  return (int)is_sent;
}

/* Called by the backend when the read to addr completes: every Scarab request merged onto it
   is answered together. */
void enqueue_response(long addr) {
  ASSERTM(0, inflight_read_reqs.find(addr) != inflight_read_reqs.end(),
          "ERROR: A corresponding Scarab request was not found for the "
          "Ramulator request that read address: %lu\n",
          addr);

  auto it_scarab_req = inflight_read_reqs.find(addr);
  for (auto req : it_scarab_req->second)
    resp_queue.push_back(std::make_pair(it_scarab_req->first, req));
  inflight_read_reqs.erase(it_scarab_req);
  glue_reads_completed++;
}

void ramulator_tick() {
  backend->tick();
  STAT_EVENT(0, DRAM_CYCLES);

  /* A returning line already holds its LLC MSHR, which is what bounded how many could be
     in DRAM at once, so nothing can refuse it here. */
  if (resp_queue.size() > 0) {
    Mem_Req* req = resp_queue.front().second;
    DEBUG(req->proc_id, "Ramulator: Completing a (%s) request to address %llx\n", Mem_Req_Type_str(req->type),
          req->addr);
    glue_writes_acked += req->type == MRT_WB;
    mem_complete_bus_in_access(req, 0);
    resp_queue.pop_front();
  }
}

int ramulator_get_chip_width() {
  return backend->get_chip_width();
}

int ramulator_get_chip_size() {
  return backend->get_chip_size();
}

int ramulator_get_num_chips() {
  return backend->get_num_chips();
}

int ramulator_get_chip_row_buffer_size() {
  return backend->get_chip_row_buffer_size();
}

Mem_Req* ramulator_search_queue(long phys_addr, Mem_Req_Type type) {
  ASSERTM(0,
          (type == MRT_IFETCH) || (type == MRT_DFETCH) || (type == MRT_IPRF) || (type == MRT_DPRF) ||
              (type == MRT_DSTORE) || (type == MRT_MIN_PRIORITY) || (type == MRT_FDIPPRFON) ||
              (type == MRT_FDIPPRFOFF) || (type == MRT_FDIPPRFALT) || (type == MRT_UOCPRF),
          "Ramulator: Cannot search write requests in Ramulator request queue\n");
  auto it_req = inflight_read_reqs.find(phys_addr);

  // Search request queue
  if (it_req != inflight_read_reqs.end()) {
    for (auto req : it_req->second) {
      if ((req->type == MRT_IFETCH || req->type == MRT_IPRF || req->type == MRT_FDIPPRFON ||
           req->type == MRT_FDIPPRFOFF || req->type == MRT_FDIPPRFALT || req->type == MRT_UOCPRF) &&
          (type == MRT_IFETCH || type == MRT_IPRF || type == MRT_FDIPPRFON || type == MRT_FDIPPRFOFF ||
           type == MRT_FDIPPRFALT || type == MRT_UOCPRF))
        return req;
      else if ((req->type == MRT_DFETCH || req->type == MRT_DPRF || req->type == MRT_DSTORE) &&
               (type == MRT_DFETCH || type == MRT_DPRF || type == MRT_DSTORE))
        return req;
    }
  }

  // Search response queue
  for (auto resp : resp_queue) {
    if (resp.first == phys_addr) {
      if ((resp.second->type == MRT_IFETCH || resp.second->type == MRT_IPRF || resp.second->type == MRT_FDIPPRFON ||
           resp.second->type == MRT_FDIPPRFOFF || resp.second->type == MRT_FDIPPRFALT ||
           resp.second->type == MRT_UOCPRF) &&
          (type == MRT_IFETCH || type == MRT_IPRF || type == MRT_FDIPPRFON || type == MRT_FDIPPRFOFF ||
           type == MRT_FDIPPRFALT || type == MRT_UOCPRF))
        return resp.second;
      else if ((resp.second->type == MRT_DFETCH || resp.second->type == MRT_DPRF || resp.second->type == MRT_DSTORE) &&
               (type == MRT_DFETCH || type == MRT_DPRF || type == MRT_DSTORE))
        return resp.second;
    }
  }

  return NULL;
}
