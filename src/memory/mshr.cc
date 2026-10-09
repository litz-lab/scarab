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
 * File         : memory/mshr.cc
 * Author       : Litz Lab
 * Date         : 10/2026
 * Description  : MSHR file: one entry per line in flight, each with the requests
 *                waiting on that line. Shared by every level, and by the writeback
 *                files.
 ***************************************************************************************/

#include <algorithm>
#include <list>
#include <map>
#include <string>

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "memory/mshr.h"

#include "statistics.h"
}

/* One MSHR: a line in flight, and the requests waiting on it in arrival order. */
struct Mshr_Line_struct {
  Addr addr;
  Counter rdy;
  std::list<Mem_Req*> reqs;
};

struct Mshr_Impl_struct {
  std::string name;
  std::list<Mshr_Line> lines;                             /* in allocation order */
  std::map<Addr, std::list<Mshr_Line>::iterator> by_line; /* line address -> entry */
};

static inline Addr line_of(Addr addr) {
  return addr >> LOG2(L1_LINE_SIZE);
}

void mshr_init(Mshr* mshr, const char* name, Mshr_Type type, uns size) {
  mshr->impl = new Mshr_Impl_struct();
  mshr->impl->name = name;
  mshr->pending_fills = 0;
  mshr->size = size;
  mshr->type = type;
}

void mshr_reset(Mshr* mshr) {
  mshr->impl->lines.clear();
  mshr->impl->by_line.clear();
  mshr->pending_fills = 0;
}

void mshr_destroy(Mshr* mshr) {
  delete mshr->impl;
  mshr->impl = nullptr;
}

const char* mshr_name(Mshr* mshr) {
  return mshr->impl->name.c_str();
}

uns mshr_count(Mshr* mshr) {
  return mshr->impl->lines.size();
}

Mshr_Line* mshr_line(Mshr* mshr, Addr addr) {
  auto it = mshr->impl->by_line.find(line_of(addr));
  return it == mshr->impl->by_line.end() ? nullptr : &*it->second;
}

Flag mshr_full(Mshr* mshr, Addr addr) {
  return !mshr_line(mshr, addr) && mshr_count(mshr) >= mshr->size;
}

Flag mshr_holds(Mshr* mshr, Mem_Req* req) {
  Mshr_Line* line = mshr_line(mshr, req->addr);
  return line && std::find(line->reqs.begin(), line->reqs.end(), req) != line->reqs.end();
}

void mshr_add(Mshr* mshr, Mem_Req* req) {
  Mshr_Impl_struct* impl = mshr->impl;
  ASSERT(req->proc_id, !mshr_holds(mshr, req));
  auto it = impl->by_line.find(line_of(req->addr));
  if (it == impl->by_line.end()) {
    ASSERTM(req->proc_id, impl->lines.size() < mshr->size, "%s: all %u entries taken adding %s\n", impl->name.c_str(),
            mshr->size, Mem_Req_Type_str(req->type));
    impl->lines.push_back(Mshr_Line{line_of(req->addr), 0, {}});
    it = impl->by_line.emplace(line_of(req->addr), std::prev(impl->lines.end())).first;
  }
  it->second->reqs.push_back(req);
}

void mshr_remove(Mshr* mshr, Mem_Req* req) {
  Mshr_Impl_struct* impl = mshr->impl;
  auto it = impl->by_line.find(line_of(req->addr));
  ASSERT(req->proc_id, it != impl->by_line.end());
  std::list<Mem_Req*>& reqs = it->second->reqs;
  auto pos = std::find(reqs.begin(), reqs.end(), req);
  ASSERT(req->proc_id, pos != reqs.end());
  reqs.erase(pos);
  if (reqs.empty()) {
    impl->lines.erase(it->second);
    impl->by_line.erase(it);
  }
}

Mshr_Line* mshr_first(Mshr* mshr) {
  return mshr->impl->lines.empty() ? nullptr : &mshr->impl->lines.front();
}

Mshr_Line* mshr_next(Mshr* mshr, Mshr_Line* line) {
  auto it = std::next(mshr->impl->by_line.at(line->addr));
  return it == mshr->impl->lines.end() ? nullptr : &*it;
}

Counter mshr_line_rdy(Mshr_Line* line) {
  return line->rdy;
}

void mshr_line_set_rdy(Mshr_Line* line, Counter rdy) {
  line->rdy = rdy;
}

uns mshr_line_count(Mshr_Line* line) {
  return line->reqs.size();
}

uns mshr_line_reqs(Mshr_Line* line, Mem_Req** reqs) {
  std::copy(line->reqs.begin(), line->reqs.end(), reqs);
  return line->reqs.size();
}

/* Which request types fold into an outstanding one: the ones that want the line for
   the same requester. */
static Flag mshr_type_folds(Mem_Req_Type have, Mem_Req_Type type, Flag* demand_hit_prefetch) {
  if (have == type)
    return TRUE;
  switch (have) {
    case MRT_IFETCH:
      return type == MRT_IPRF || type == MRT_UOCPRF || type == MRT_FDIPPRFON || type == MRT_FDIPPRFOFF ||
             type == MRT_FDIPPRFALT;
    case MRT_DFETCH:
      return type == MRT_DSTORE || type == MRT_DPRF;
    case MRT_DSTORE:
      return type == MRT_DFETCH || type == MRT_DPRF;
    case MRT_IPRF:
    case MRT_UOCPRF:
    case MRT_FDIPPRFON:
    case MRT_FDIPPRFOFF:
    case MRT_FDIPPRFALT:
      if (type == MRT_IFETCH) {
        *demand_hit_prefetch = TRUE;
        return TRUE;
      }
      return type == MRT_IPRF || type == MRT_UOCPRF || type == MRT_FDIPPRFON || type == MRT_FDIPPRFOFF ||
             type == MRT_FDIPPRFALT;
    case MRT_DPRF:
      if (type == MRT_DFETCH || type == MRT_DSTORE) {
        *demand_hit_prefetch = TRUE;
        return TRUE;
      }
      return FALSE;
    default:
      return FALSE;
  }
}

Mem_Req* mshr_search(Mshr* mshr, uns8 proc_id, Addr addr, Mem_Req_Type type, uns size, Flag* demand_hit_prefetch) {
  ASSERT(proc_id, size % L1_LINE_SIZE == 0);
  *demand_hit_prefetch = FALSE;
  Mshr_Line* line = mshr_line(mshr, addr);
  if (!line)
    return nullptr;
  for (Mem_Req* req : line->reqs) {
    ASSERT(proc_id, req->proc_id == proc_id && req->state != MRS_INV);
    if (mshr_type_folds(req->type, type, demand_hit_prefetch)) {
      STAT_EVENT(req->proc_id, MEM_REQ_MATCH_IFETCH + MIN2(req->type, MRT_WB_NODIRTY));
      return req;
    }
    *demand_hit_prefetch = FALSE;
  }
  return nullptr;
}
