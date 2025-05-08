#include "lookahead_buffer.h"

#include <cmath>
#include <deque>
#include <iostream>
#include <tuple>
#include <vector>
#include <algorithm>

#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"

#include "frontend/frontend_intf.h"
#include "isa/isa_macros.h"

#include <math.h>
#include <stdbool.h>
#include <cstdint>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"
#include "op.h"
#include "op_pool.h"
#include "thread.h"

#include "thread.h"
#include "ft.h"

#include "confidence/conf.hpp"
#include "ft_info.h"

std::vector<FT> FT_buffer;
std::vector<Addr> deferred_addr;
std::vector<std::pair<FT_Info_Static, std::vector<unsigned long long>>> addr_to_orders;
std::vector<std::pair<FT_Info_Static, std::vector<uint64_t>>> addr_to_buf_pos;

uint64_t insert_order = 0;

FT current_ft_to_push(0);
uns off_path_lookahead = 0;

uint64_t rdptr_lb;
uint64_t rdptr_lb_in_ftq;
uint64_t wrptr_lb;
void addr_to_orders_remove();
void addr_to_orders_insert() ;
bool match_ft_info(const FT_Info_Static& a, const FT_Info_Static& b) ;

bool match_ft_info(const FT_Info_Static& a, const FT_Info_Static& b) {
    return a.start == b.start &&
           a.length == b.length &&
           a.n_uops == b.n_uops;
}

void addr_to_orders_insert() {
    FT_Info_Static inserting_FT = FT_buffer[wrptr_lb].get_ft_info().static_info;

    auto it = std::find_if(addr_to_orders.begin(), addr_to_orders.end(),
        [&](const auto& pair) {
            return match_ft_info(pair.first, inserting_FT);
        });

    if (it != addr_to_orders.end()) {
        it->second.push_back(insert_order);
    } else {
        addr_to_orders.push_back({inserting_FT, {insert_order}});
    }

    auto it_ = std::find_if(addr_to_buf_pos.begin(), addr_to_buf_pos.end(),
        [&](const auto& pair) {
            return match_ft_info(pair.first, inserting_FT);
        });

    if (it_ != addr_to_buf_pos.end()) {
        it_->second.push_back(wrptr_lb);
    } else {
        addr_to_buf_pos.push_back({inserting_FT, {wrptr_lb}});
    }

    insert_order++;
}


void addr_to_orders_remove() {
    FT_Info_Static removing_FT = FT_buffer[rdptr_lb_in_ftq].get_ft_info().static_info;

    auto it = std::find_if(addr_to_orders.begin(), addr_to_orders.end(),
        [&](const auto& pair) {
            return match_ft_info(pair.first, removing_FT);
        });

    if (it != addr_to_orders.end() && !it->second.empty()) {
        it->second.erase(it->second.begin());
        if (it->second.empty()) {
            addr_to_orders.erase(it);
        }
    }

    auto it_ = std::find_if(addr_to_buf_pos.begin(), addr_to_buf_pos.end(),
        [&](const auto& pair) {
            return match_ft_info(pair.first, removing_FT);
        });

    if (it_ != addr_to_buf_pos.end() && !it_->second.empty()) {
        it_->second.erase(it_->second.begin());
        if (it_->second.empty()) {
            addr_to_buf_pos.erase(it_);
        }
    }
}


void init_lookahead_buffer(){
    if (!LOOKAHEAD_BUF_SIZE)
        return;

    FT_buffer.resize(LOOKAHEAD_BUF_SIZE);
    for (uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {

        rdptr_lb = 0;
        wrptr_lb = 0;
        rdptr_lb_in_ftq = 0;

        current_ft_to_push = FT();
        current_ft_to_push.set_ft_started_by(FT_STARTED_BY_APP);

        for (uint i = 0; i < LOOKAHEAD_BUF_SIZE; i++) {
            FT_buffer_insert_FT(proc_id);
        }
    }
}
uns off_path_ft = 0;
void FT_buffer_insert_FT(uns8 proc_id){
    if (off_path_lookahead){
        off_path_ft +=1;
        return;
    }
        

    FT_Ended_By ft_ended_by = FT_NOT_ENDED;
    Counter buffer_op_count = 0;

    while (ft_ended_by == FT_NOT_ENDED) {
        if(!frontend_can_fetch_op(proc_id))
            return;
        Op* op = alloc_op(proc_id);
        frontend_fetch_op(proc_id, op);
        op->op_num = buffer_op_count++;

        if (op->eom) {
            uns offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size) -
                         ROUND_DOWN(op->inst_info->addr, ICACHE_LINE_SIZE);
            bool end_of_icache_line = offset >= ICACHE_LINE_SIZE;
            bool cf_taken = op->table_info->cf_type && op->oracle_info.dir == TAKEN;
            bool bar_fetch = IS_CALLSYS(op->table_info) || op->table_info->bar_type & BAR_FETCH;

            if (op->exit) {
                ft_ended_by = FT_APP_EXIT;
            } else if (bar_fetch) {
                ft_ended_by = FT_BAR_FETCH;
            } else if (cf_taken) {
                ft_ended_by = FT_TAKEN_BRANCH;
            } else if (end_of_icache_line) {
                ft_ended_by = FT_ICACHE_LINE_BOUNDARY;
            }
        }

        current_ft_to_push.add_op(op, ft_ended_by);
    }

    if (ft_ended_by != FT_NOT_ENDED) {
        current_ft_to_push.set_per_op_ft_info();

        if (FT_buffer.size() > 0 && wrptr_lb > 0) {
            Op* last_op = FT_buffer[wrptr_lb - 1].peek_last_op();
            if (FT_buffer[wrptr_lb - 1].get_ft_info().dynamic_info.ended_by == FT_TAKEN_BRANCH) {
                ASSERT(proc_id, last_op->oracle_info.npc == current_ft_to_push.get_ft_info().static_info.start);
            } else if (FT_buffer[wrptr_lb - 1].get_ft_info().dynamic_info.ended_by == FT_BAR_FETCH) {
                ASSERT(proc_id, last_op->oracle_info.npc == current_ft_to_push.get_ft_info().static_info.start ||
                               last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size ==
                               current_ft_to_push.get_ft_info().static_info.start);
            } else {
                ASSERT(proc_id, last_op->inst_info->addr + last_op->inst_info->trace_info.inst_size ==
                               current_ft_to_push.get_ft_info().static_info.start);
            }
        }
        FT_buffer[wrptr_lb] = current_ft_to_push;
        addr_to_orders_insert();
        current_ft_to_push = FT(proc_id);

        if (ft_ended_by == FT_ICACHE_LINE_BOUNDARY) {
            current_ft_to_push.set_ft_started_by(FT_STARTED_BY_ICACHE_LINE_BOUNDARY);
        } else if (ft_ended_by == FT_TAKEN_BRANCH) {
            current_ft_to_push.set_ft_started_by(FT_STARTED_BY_TAKEN_BRANCH);
        } else if (ft_ended_by == FT_BAR_FETCH) {
            current_ft_to_push.set_ft_started_by(FT_STARTED_BY_BAR_FETCH);
        }
    }
    // printf("FT buffer insert wrptr %lu\n", wrptr_lb);
    wrptr_lb = (wrptr_lb + 1) % LOOKAHEAD_BUF_SIZE;
}

Op* FT_buffer_read_Op(uns proc_id){
    if (!FT_buffer[rdptr_lb].can_fetch_op()) {
        FT_buffer[rdptr_lb].set_consumed();
        if(deferred_addr.size() > 0 && FT_buffer[rdptr_lb].get_ft_info().static_info.start == deferred_addr[0]){
            // printf("ft matched addr %llx and removed from deferred addr\n", FT_buffer[rdptr_lb_in_ftq].get_ft_info().static_info.start);
            addr_to_orders_remove();
            FT_buffer[rdptr_lb_in_ftq] = FT();
            rdptr_lb_in_ftq = (rdptr_lb_in_ftq + 1) % LOOKAHEAD_BUF_SIZE;
            FT_buffer_insert_FT(proc_id);
            deferred_addr.erase(deferred_addr.begin());
           
        }
        // addr_to_orders_remove(); 
        // FT_buffer[rdptr_lb] = FT();
        
        rdptr_lb = (rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE;
        // FT_buffer_insert_FT(proc_id);
        // printf("rdptr_lb %lu and rdptr_lb_in_ftq %lu\n", rdptr_lb, rdptr_lb_in_ftq);
        
        
    }

    if (FT_buffer[rdptr_lb].can_fetch_op()) {
        return FT_buffer[rdptr_lb].fetch_op();
    } else {
        ASSERT(proc_id, 0);
        return NULL;
    }
}

void lookahead_buffer_ft_removed_from_ftq(uns proc_id, Addr addr){
    if(FT_buffer[rdptr_lb_in_ftq].get_ft_info().static_info.start == addr){
        if(FT_buffer[rdptr_lb_in_ftq].is_consumed()){
            // printf("FT buffer addr match and consumed dptr_lb %lu and rdptr_lb_in_ftq %lu\n", rdptr_lb, rdptr_lb_in_ftq);
            addr_to_orders_remove();
            FT_buffer[rdptr_lb_in_ftq] = FT();
            rdptr_lb_in_ftq = (rdptr_lb_in_ftq + 1) % LOOKAHEAD_BUF_SIZE;
            FT_buffer_insert_FT(proc_id);

        }else{
            deferred_addr.push_back(addr);
            // printf("ft matched addr %llx and added to deferred addr\n", addr);
        }

        
    // }else{
    //     printf("FT buffer not match\n");
    }

    
    return;
}

Addr lookahead_buffer_next_addr(){
    if (FT_buffer[rdptr_lb].can_fetch_op()) {
        return FT_buffer[rdptr_lb].peek_next_op()->inst_info->addr;
    }

    return FT_buffer[(rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE].get_ft_info().static_info.start;
}

Flag lookahead_buffer_can_fetch_op(uns proc_id){
    // can fetch from FT buffer if the current ft have op left or
    // next ft is not consumed and valid
    return FT_buffer[rdptr_lb].can_fetch_op() || (!FT_buffer[(rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE].is_consumed() && FT_buffer[(rdptr_lb + 1) % LOOKAHEAD_BUF_SIZE].get_ft_info().static_info.start);
} 

Op* lookahead_buffer_fetch_op(uns proc_id, Op* new_op) {
    if (off_path_lookahead) {
        new_op = alloc_op(proc_id);
        frontend_fetch_op(proc_id, new_op);
        new_op->exit = false;
        return new_op;
    }
    // if still can read from FT buffer
    // current ft have op left or
    // next ft is not consumed and valid
    if(!off_path_lookahead && (FT_buffer[rdptr_lb].can_fetch_op() || !FT_buffer[(rdptr_lb +1) % LOOKAHEAD_BUF_SIZE].is_consumed()))
        return FT_buffer_read_Op(proc_id);
    
    ASSERT(proc_id, 0);
    return nullptr;
}

uint64_t lookahead_buffer_FT_search(FT_Info_Static static_info) {
    auto it = std::find_if(addr_to_orders.begin(), addr_to_orders.end(),
        [&](const auto& pair) {
            return match_ft_info(pair.first, static_info);
        });

    if (it != addr_to_orders.end() && !it->second.empty()) {
        return it->second[0];
    } else {
        return 0;
    }
}

uint64_t lookahead_buffer_FT_search_buf_pos(FT_Info_Static static_info) {
    auto it = std::find_if(addr_to_buf_pos.begin(), addr_to_buf_pos.end(),
        [&](const auto& pair) {
            return match_ft_info(pair.first, static_info);
        });

    if (it != addr_to_buf_pos.end() && !it->second.empty()) {
        return it->second[0] + 1;
    } else {
        return 0;
    }
}

FT_Info_Static lookahead_buffer_ptr_search(uint64_t ptr_pos) {
    if(FT_buffer[ptr_pos].get_ft_info().static_info.start)
        return FT_buffer[ptr_pos].get_ft_info().static_info;
    else 
        return FT_Info_Static();
}

void lookahead_buffer_redirect(){
    // add additional frontend call for execution-driven support here
    off_path_lookahead = 1;
}

void lookahead_buffer_recover(){
    // add additional frontend call for execution-driven support here
    off_path_lookahead = 0;
    for(uns i =0; i<off_path_ft; i++){
        FT_buffer_insert_FT(0);
    }
    off_path_ft = 0;
}

uint64_t lookahead_buffer_rdptr(){
    return rdptr_lb_in_ftq;
}
