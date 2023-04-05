#include "decoupled_frontend.h"
#include "prefetcher/fdip_new.h"

extern "C" {
#include "op.h"
#include "prefetcher/pref.param.h"
#include "memory/memory.h"
#include "memory/memory.param.h"
}

#include <iostream>

decoupled_fe_iter* iter;
const int MAX_PREF_CYC = 2;
uint64_t last_line_addr = 0;
int fdip_proc_id;
Icache_Stage *ic_ref;

void init_fdip() {
  iter = decoupled_fe_new_ftq_iter();
}

void set_fdip(int _proc_id, Icache_Stage *_ic) {
  fdip_proc_id = _proc_id;
  ic_ref = _ic;
}

void update_fdip() {
  if (!FDIP_ENABLE)
    return;

  int prefetch_per_cycle = 0;

  do {
    Op *op = decoupled_fe_ftq_iter_get(iter);
    if (!op) {
      std::cout << "FTQ Empty" << std::endl;
      break;
    }
    if (prefetch_per_cycle == MAX_PREF_CYC) {
      break;
    }
    if (!mem_can_allocate_req_buffer(fdip_proc_id, MRT_FDIPPRF, FALSE)) {
      break;
    }

    uint64_t pc_addr = op->inst_info->addr;
    Addr line_addr = op->inst_info->addr && ~0x3F;
    if (line_addr != last_line_addr) {
      Flag demand_hit_prefetch = FALSE;
      Flag demand_hit_writeback = FALSE;
      Mem_Queue_Entry* queue_entry = NULL;
      Flag ramulator_match = FALSE;
      bool line = (Inst_Info**)cache_access(&ic_ref->icache, pc_addr, &line_addr, TRUE);
      Mem_Req* mem_req = mem_search_reqbuf_wrapper(ic_ref->proc_id, get_cache_line_addr(&ic_ref->icache, pc_addr),
                                                   MRT_FDIPPRF, ICACHE_LINE_SIZE, &demand_hit_prefetch, &demand_hit_writeback,
                                                   QUEUE_MLC | QUEUE_L1 | QUEUE_BUS_OUT |
                                                   QUEUE_MEM | QUEUE_L1FILL | QUEUE_MLC_FILL,
                                                   &queue_entry, &ramulator_match);
      if (line || mem_req) {
        //std::cout << "Line in cache or prefetched " << std::hex << line_addr << std::endl;
      }
      else {
        bool success = new_mem_req(MRT_FDIPPRF, fdip_proc_id, line_addr,
                                   ICACHE_LINE_SIZE, 0, NULL, instr_fill_line, unique_count++, 0);
        if (!success) {
          std::cout << "Enqueue MemReq failed" << std::endl;
        }
      }
    }
    last_line_addr = line_addr;

  } while (decoupled_fe_ftq_iter_advance(iter));
}
