/* Copyright 2020 University of California Santa Cruz
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
 * File         : frontend/pt_memtrace/memtrace_fe.cc
 * Author       : Heiner Litz
 * Date         : 05/15/2020
 * Description  : Frontend to simulate traces in memtrace format
 ***************************************************************************************/

 extern "C" {
  #include "debug/debug.param.h"
  #include "debug/debug_macros.h"
  #include "globals/assert.h"
  #include "globals/global_defs.h"
  #include "globals/global_types.h"
  #include "globals/global_vars.h"
  #include "globals/utils.h"
  }
  
  #include "bp/bp.h"
  #include "bp/bp.param.h"
  #include "ctype_pin_inst.h"
  #include "frontend/pt_memtrace/memtrace_fe.h"
  #include "isa/isa.h"
  #include "memory/memory.param.h"
  #include "pin/pin_lib/gather_scatter_addresses.h"
  #include "pin/pin_lib/uop_generator.h"
  #include "pin/pin_lib/x86_decoder.h"
  #include "statistics.h"
  #include "uop_cache.h"
  // #include "libs/cpp_cache.tpp"
  
  #define DR_DO_NOT_DEFINE_int64
  
  #define UOP_CACHE_LINE_SIZE ICACHE_LINE_SIZE
  
  #include <map>
  #include <unordered_map>
  #include <algorithm> 
  
  #include "frontend/pt_memtrace/memtrace_trace_reader_memtrace.h"
  /**************************************************************************************/
  /* Global Variables */
  
  static char*    trace_files[MAX_NUM_PROCS];
  TraceReader*    trace_readers[MAX_NUM_PROCS];
  //TODO: Make per proc?
  uint64_t        ins_id    = 0;
  uint64_t        ins_id_fetched = 0;
  uint64_t        prior_tid = 0;
  uint64_t        prior_pid = 0;
  uint64_t        rdptr = 0;
  uint64_t        wrptr = 0;
  
  extern scatter_info_map                          scatter_info_storage;
  
  Flag roi_dump_began = FALSE;
  Counter roi_dump_ID = 0;
  std::vector<ctype_pin_inst> circ_buf;
  std::unordered_map<Addr, Counter> buf_map;
  const int CLINE = ~0x3F;
  
  // key is timestamp, value is addr
  std::map<unsigned long long, Addr> order_to_addr;
  std::unordered_map<Addr, std::vector<unsigned long long>> addr_to_orders;
  std::unordered_map<Addr, std::vector<uint64_t>> addr_to_buf_pos;
  
  unsigned long long insert_order = 0;
  unsigned long long remove_order = 0;
  /**************************************************************************************/
  /* Private Functions */
  int memtrace_trace_read_internal(int proc_id, ctype_pin_inst* next_onpath_pi);
  void buf_map_insert();
  void buf_map_remove();
  static bool add_to_ft_map(Addr start_addr, uns n_uop, Addr end_addr);
  static const std::vector<std::pair<uns, Addr>>& lookup_ft_map(Addr start_addr);

  // Private global data structure - only visible in this .cpp file
static std::unordered_map<Addr, std::vector<std::pair<uns, Addr>>> FT_addr_mapping;

static bool add_to_ft_map(Addr start_addr, uns n_uop, Addr end_addr) {
    auto& entries = FT_addr_mapping[start_addr];
    
    // Check if this pair already exists
    for (const auto& pair : entries) {
        if (pair.first == n_uop && pair.second == end_addr) {
            return false; // Pair already exists
        }
    }
    
    // Add the new pair
    entries.push_back({n_uop, end_addr});
    return true;
}

// Make sure the const qualifier matches the header declaration
static const std::vector<std::pair<uns, Addr>>& lookup_ft_map(Addr start_addr) {
    static const std::vector<std::pair<uns, Addr>> empty;
    auto it = FT_addr_mapping.find(start_addr);
    return (it != FT_addr_mapping.end()) ? it->second : empty;
}
  
  void fill_in_dynamic_info(ctype_pin_inst* info, const InstInfo* insi) {
    uint8_t ld = 0;
    uint8_t st = 0;
  
    // Note: should be overwritten for a taken control flow instruction
    info->instruction_addr      = insi->pc;
    info->instruction_next_addr = insi->target;
    info->actually_taken        = insi->taken;
    info->branch_target         = insi->target;
    info->inst_uid              = ins_id;
    info->last_inst_from_trace  = insi->last_inst_from_trace;
    info->fetched_instruction   = insi->fetched_instruction;
  
  #ifdef PRINT_INSTRUCTION_INFO
    std::cout << std::hex << info->instruction_addr << " Next "
              << info->instruction_next_addr << " size " << (uint32_t)info->size
              << " taken " << (uint32_t)info->actually_taken << " target "
              << info->branch_target << " pid " << insi->pid << " tid "
              << insi->tid << " asm "
              << std::string(
                   xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(insi->ins)))
              << " uid " << std::dec << info->inst_uid << std::endl;
  #endif
  
    if(xed_decoded_inst_get_iclass(insi->ins) == XED_ICLASS_RET_FAR ||
       xed_decoded_inst_get_iclass(insi->ins) == XED_ICLASS_RET_NEAR)
      info->actually_taken = 1;
  
    for(uint8_t op = 0;
        op < xed_decoded_inst_number_of_memory_operands(insi->ins); op++) {
      // predicated true ld/st are handled just as regular ld/st
      if(xed_decoded_inst_mem_read(insi->ins, op) && !insi->mem_used[op]) {
        // Handle predicated stores specially?
        info->ld_vaddr[ld++] = insi->mem_addr[op];
      } else if(xed_decoded_inst_mem_read(insi->ins, op)) {
        info->ld_vaddr[ld++] = insi->mem_addr[op];
      }
      if(xed_decoded_inst_mem_written(insi->ins, op) && !insi->mem_used[op]) {
        // Handle predicated stores specially?
        info->st_vaddr[st++] = insi->mem_addr[op];
      } else if(xed_decoded_inst_mem_written(insi->ins, op)) {
        info->st_vaddr[st++] = insi->mem_addr[op];
      }
    }
  }
  
  int ffwd(const xed_decoded_inst_t* ins) {
    if(!FAST_FORWARD) {
      return 0;
    }
    if(XED_INS_Opcode(ins) == XED_ICLASS_XCHG &&
       XED_INS_OperandReg(ins, 0) == XED_REG_RCX &&
       XED_INS_OperandReg(ins, 1) == XED_REG_RCX) {
      return 0;
    }
    if((USE_FETCHED_COUNT ? ins_id_fetched : ins_id) == FAST_FORWARD_TRACE_INS) {
      return 0;
    }
    return 1;
  }
  
  int roi(const xed_decoded_inst_t* ins) {
    if(XED_INS_Opcode(ins) == XED_ICLASS_XCHG &&
       XED_INS_OperandReg(ins, 0) == XED_REG_RCX &&
       XED_INS_OperandReg(ins, 1) == XED_REG_RCX) {
      return 1;
    }
    return 0;
  }
  
  void buf_map_insert() {
    // Addr line_addr = circ_buf[wrptr].instruction_addr & CLINE;
    Addr inst_addr = circ_buf[wrptr].instruction_addr;
    auto it = addr_to_orders.find(inst_addr);
    if (it != addr_to_orders.end()) {
      // Address exists, add new order to its vector
      it->second.push_back(insert_order);
    } else {
      // New address, create vector with first order
      std::vector<unsigned long long> orders;
      orders.push_back(insert_order);
      addr_to_orders[inst_addr] = orders;
    }

    auto it__ = addr_to_buf_pos.find(inst_addr);
    if (it__ != addr_to_buf_pos.end()) {
      // Address exists, add new order to its vector
      it__->second.push_back(wrptr);
    } else {
      // New address, create vector with first order
      std::vector<uint64_t> orders;
      orders.push_back(wrptr);
      addr_to_buf_pos[inst_addr] = orders;
    }

    order_to_addr[insert_order] = inst_addr;
    insert_order++;
  
    Addr line_addr = circ_buf[wrptr].instruction_addr & CLINE;
    auto it_ = buf_map.find(line_addr);
    if (it_ != buf_map.end()) {
      it_->second++;
    } else {
      buf_map.insert(std::pair<Addr, Counter>(line_addr, 1));
    }
  
    wrptr = (wrptr + 1) % MEMTRACE_BUF_SIZE;
  }
  
  void buf_map_remove() {
    // Addr line_addr = circ_buf[rdptr].instruction_addr & CLINE;
    Addr inst_addr = circ_buf[rdptr].instruction_addr;
    auto it = addr_to_orders.find(inst_addr);
    if (it != addr_to_orders.end() && !it->second.empty()) {
      // Remove oldest order (first in vector) for this address
      unsigned long long oldest_order = it->second[0];
      order_to_addr.erase(oldest_order);
      it->second.erase(it->second.begin());
  
      // Only erase address if no more orders left
      if (it->second.empty()) {
        addr_to_orders.erase(it);
      }
    }

    auto it__ = addr_to_buf_pos.find(inst_addr);

    if (it__ != addr_to_buf_pos.end() && !it__->second.empty()) {
      // Remove oldest order (first in vector) for this address
      // unsigned long long oldest_order = it__->second[0];
      it__->second.erase(it__->second.begin());
  
      // Only erase address if no more orders left
      if (it__->second.empty()) {
        addr_to_buf_pos.erase(it__);
      }
    }

    Addr line_addr = circ_buf[rdptr].instruction_addr & CLINE;
    auto it_ = buf_map.find(line_addr);
    assert(it_ != buf_map.end());
    if (it_->second > 1) {
      it_->second--;
    } else {
      buf_map.erase(it_);
    }
    rdptr = (rdptr + 1) % MEMTRACE_BUF_SIZE;
  }
  
  bool buf_map_find(Addr line_addr) {
    return buf_map.find(line_addr) != buf_map.end();
  }
  
  int memtrace_trace_read(int proc_id, ctype_pin_inst* next_onpath_pi) {
    if (!MEMTRACE_BUF_SIZE) {
      return memtrace_trace_read_internal(proc_id, next_onpath_pi);
    }
    else {
      *next_onpath_pi = circ_buf[rdptr];
      buf_map_remove();
      int ret = memtrace_trace_read_internal(proc_id, &circ_buf[wrptr]);
      buf_map_insert();
      return ret;
    }
  }
  
  int memtrace_trace_read_internal(int proc_id, ctype_pin_inst* next_onpath_pi) {
    InstInfo* insi;
  
    do {
      insi = const_cast<InstInfo*>(trace_readers[proc_id]->nextInstruction());
  
      if(prior_pid == 0) {
        ASSERT(proc_id, prior_tid == 0);
        ASSERT(proc_id, insi->valid);
        prior_pid = insi->pid;
        prior_tid = insi->tid;
        ASSERT(proc_id, prior_tid);
        ASSERT(proc_id, prior_pid);
      }
      if(insi->valid) {
        ins_id++;
        if(insi->fetched_instruction) {
          ins_id_fetched++;
        }
      } else {
        return 0;  // end of trace
      }
    } while(insi->pid != prior_pid || insi->tid != prior_tid);
  
    memset(next_onpath_pi, 0, sizeof(ctype_pin_inst));
    fill_in_dynamic_info(next_onpath_pi, insi);
    fill_in_basic_info(next_onpath_pi, insi->ins);
    if(XED_INS_IsVgather(insi->ins) || XED_INS_IsVscatter(insi->ins)) {
      xed_category_enum_t category           = XED_INS_Category(insi->ins);
      scatter_info_storage[insi->pc] = add_to_gather_scatter_info_storage(
        insi->pc, XED_INS_IsVgather(insi->ins), XED_INS_IsVscatter(insi->ins), category);
    }
    uint32_t max_op_width = add_dependency_info(next_onpath_pi, insi->ins);
    fill_in_simd_info(next_onpath_pi, insi->ins, max_op_width);
    apply_x87_bug_workaround(next_onpath_pi, insi->ins);
    fill_in_cf_info(next_onpath_pi, insi->ins);
    print_err_if_invalid(next_onpath_pi, insi->ins);
  
    if (next_onpath_pi->scarab_marker_roi_begin == true) {
      assert(!roi_dump_began);
      // reset stats
      std::cout << "Reached roi dump begin marker, reset stats" << std::endl;
      reset_stats(TRUE);
      roi_dump_began = TRUE;
    } else if (next_onpath_pi->scarab_marker_roi_end == true) {
      assert(roi_dump_began);
      // dump stats
      std::cout << "Reached roi dump end marker, dump stats between" << std::endl;
      dump_stats(proc_id, TRUE, global_stat_array[proc_id], NUM_GLOBAL_STATS);
      roi_dump_began = FALSE;
      roi_dump_ID ++;
    }
  
    // End of ROI
    if(roi(insi->ins))
      return 0;
  
    return 1;
  }
  
  
  /**************************************************************************************/
  /* trace_init() */
  
  void memtrace_init(void) {
    uop_generator_init(NUM_CORES);
    init_x86_decoder(nullptr);
    init_x87_stack_delta();
  
    //next_onpath_pi = (ctype_pin_inst*)malloc(NUM_CORES * sizeof(ctype_pin_inst));
  
    /* temp variable needed for easy initialization syntax */
    char* tmp_trace_files[MAX_NUM_PROCS] = {
      CBP_TRACE_R0,  CBP_TRACE_R1,  CBP_TRACE_R2,  CBP_TRACE_R3,  CBP_TRACE_R4,
      CBP_TRACE_R5,  CBP_TRACE_R6,  CBP_TRACE_R7,  CBP_TRACE_R8,  CBP_TRACE_R9,
      CBP_TRACE_R10, CBP_TRACE_R11, CBP_TRACE_R12, CBP_TRACE_R13, CBP_TRACE_R14,
      CBP_TRACE_R15, CBP_TRACE_R16, CBP_TRACE_R17, CBP_TRACE_R18, CBP_TRACE_R19,
      CBP_TRACE_R20, CBP_TRACE_R21, CBP_TRACE_R22, CBP_TRACE_R23, CBP_TRACE_R24,
      CBP_TRACE_R25, CBP_TRACE_R26, CBP_TRACE_R27, CBP_TRACE_R28, CBP_TRACE_R29,
      CBP_TRACE_R30, CBP_TRACE_R31, CBP_TRACE_R32, CBP_TRACE_R33, CBP_TRACE_R34,
      CBP_TRACE_R35, CBP_TRACE_R36, CBP_TRACE_R37, CBP_TRACE_R38, CBP_TRACE_R39,
      CBP_TRACE_R40, CBP_TRACE_R41, CBP_TRACE_R42, CBP_TRACE_R43, CBP_TRACE_R44,
      CBP_TRACE_R45, CBP_TRACE_R46, CBP_TRACE_R47, CBP_TRACE_R48, CBP_TRACE_R49,
      CBP_TRACE_R50, CBP_TRACE_R51, CBP_TRACE_R52, CBP_TRACE_R53, CBP_TRACE_R54,
      CBP_TRACE_R55, CBP_TRACE_R56, CBP_TRACE_R57, CBP_TRACE_R58, CBP_TRACE_R59,
      CBP_TRACE_R60, CBP_TRACE_R61, CBP_TRACE_R62, CBP_TRACE_R63,
    };
    if(DUMB_CORE_ON) {
      // avoid errors by specifying a trace known to be good
      tmp_trace_files[DUMB_CORE] = tmp_trace_files[0];
    }
  
    for(uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
      trace_files[proc_id] = tmp_trace_files[proc_id];
    }
    for(uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
      memtrace_setup(proc_id);
    }
  }
  
  uns buffer_find_replace(const std::vector<Addr>& line_addrs, const std::vector<Addr>& end_line_addrs, const std::vector<uns>& lengths,  const std::vector<Addr>& byte_length, const std::vector<uns>& n_uop, uns num_ft) {
      // Debug print for input sizes
      // std::cout << "Input sizes - line_addrs: " << line_addrs.size() 
      //           << ", lengths: " << lengths.size() 
      //           << ", num_ft: " << num_ft << std::endl;
  
      // ASSERT(0, line_addrs.size() == num_ft + 1);  // +1 for the inserting address
      ASSERT(0, line_addrs.size() == lengths.size());
      ASSERT(0, line_addrs.size() == byte_lengths.size());
      ASSERT(0, line_addrs.size() == end_line_addrs.size());

      for (size_t i = 0; i < line_addrs.size(); ++i) {
        for (size_t j = i + 1; j < line_addrs.size(); ++j) { 
            if (line_addrs[i] == line_addrs[j] && end_line_addrs[i] == end_line_addrs[j]) {
              ASSERT(0, 1); // should not happen
            }
        }
    }
      
      if (line_addrs.empty() || num_ft == 0) {
          return 1;
      }
  
      uns offset_bits = LOG2(UOP_CACHE_LINE_SIZE);
      uns num_sets = UOP_CACHE_LINES / UOP_CACHE_ASSOC;
      uns target_set = (line_addrs[0] >> offset_bits) % num_sets;
      for(uns i=0; i<line_addrs.size(); i++)
        add_to_ft_map(line_addrs[i], n_uop[i], end_line_addrs[i]);
      
  
      // First address is the new one to insert
      Addr new_addr = line_addrs[0];
      uns required_length = lengths[0];
      Addr new_addr_orders = find_valid_ft_timestamp(new_addr, end_line_addrs[0],lengths[0], n_uop[0]);
      // auto new_addr_orders = addr_to_orders.find(new_addr);
  
      // Debug print for new address details
      // std::cout << "Inserting New addr: 0x" << std::hex << new_addr 
      //           << ", required length: " << std::dec << required_length << std::endl;
      // if (new_addr_orders != addr_to_orders.end() && !new_addr_orders->second.empty()) {
      if (new_addr_orders !=0) {
          STAT_EVENT(0, BELADY_BYPASS_FIRST_FOUND);
          // unsigned long long new_next_use = new_addr_orders->second[0];
          unsigned long long new_next_use = new_addr_orders;
          
          // Debug print for timestamp
          // std::cout << "New addr next use timestamp: " << new_next_use << std::endl;
  
          Addr furthest_idx = 0;
          unsigned long long furthest_use = 0;
          uns not_found_cnt = 0;
          uns available_space = UOP_CACHE_ASSOC;
  
          // Check existing addresses
          for (size_t i = 1; i < line_addrs.size(); i++) {
              Addr existing_addr = line_addrs[i];
              // auto existing_orders = addr_to_orders.find(existing_addr);
  
              auto existing_orders = find_valid_ft_timestamp(existing_addr, end_line_addrs[i],lengths[i], n_uop[i]);
              // auto existing_orders = addr_to_orders.find(existing_addr);
  
              // Debug print for each existing address
              // std::cout << "Checking in set addr[" << i << "]: 0x" << std::hex << existing_addr << std::dec;
  
              if (existing_orders != 0) {
              // if (existing_orders != addr_to_orders.end() && !existing_orders->second.empty()) {
                  
                  STAT_EVENT(0, BELADY_BYPASS_FOUND);
                  // if (existing_orders->second[0] >= new_next_use) {
                  if (existing_orders >= new_next_use) {
                     
                      if (furthest_use < existing_orders) {
                        furthest_idx = i;
                          furthest_use = existing_orders;
                      }
                  // }else if(existing_orders->second[0] < new_next_use){
                  }else if(existing_orders < new_next_use){
                    // std::cout << " Found, next use: " << existing_orders->second[0];
                    // std::cout << " length: " << lengths[i];
                    available_space -= lengths[i];
                    if(available_space < required_length)
                      return 0;
                  }
              } else {
                  // std::cout << "in set Not found";
                  STAT_EVENT(0, BELADY_BYPASS_NOT_FOUND);
                  not_found_cnt++;
                  // if (furthest_use < std::numeric_limits<unsigned long long>::max()) {
                    furthest_idx = i;
                      furthest_use = std::numeric_limits<unsigned long long>::max();
                  // }
              }
              // std::cout << std::endl;
          }
  
          // Debug print summary
          // std::cout << "Summary - Not found count: " << not_found_cnt 
          //           << ", Available space: " << available_space 
          //           << ", Furthest addr: 0x" << std::hex << furthest_addr 
          //           << ", Furthest use: " << std::dec << furthest_use << std::endl;
  
          // Rest of the code remains the same...
          bool should_insert;
          // if(!not_found_cnt){
          //   should_insert = (new_next_use < furthest_use) && (available_space >= required_length);
          // }else{
            
          // }
          // if(available_space < required_length)
          //   STAT_EVENT(0, BUF_MAP_BYPASS_SET_USED);
          // Additional check for multiple not-found addresses
          if (available_space >= required_length) {
            // if(not_found_cnt){
              STAT_EVENT(0, BUF_MAP_SET_INST_NOT_FOUND);
              // uns reuse_count = 0;
  
              for (const auto& entry : order_to_addr) {
                  if (entry.first > new_next_use) break;
  
                  Addr curr_addr = entry.second;
                  uns curr_set = (curr_addr >> offset_bits) % num_sets;
  
                  if (curr_set == target_set) {
                      bool is_in_input = false;
                      for (const auto& addr : line_addrs) {
                          if (addr == curr_addr ) {
                              is_in_input = true;
                              break;
                          }
                      }
  
                      if (!is_in_input) {
                          auto addr_orders = addr_to_orders.find(curr_addr);
                          // get_timestamps_for_cache_line(curr_addr& ~0x3f);
                          if (addr_orders != addr_to_orders.end() &&  addr_orders->second.size() > 1) {
                              // if (reuse_count <= not_found_cnt - 1)
                              //   reuse_count++;
                              // if (reuse_count >= not_found_cnt - 1) {
                              //     STAT_EVENT(0, BUF_MAP_SET_INST_NOT_FOUND_OTHER_FOUND);
                              //     // break;
                              // }
                              if(addr_orders->second[1]<new_next_use){
                                auto it = FT_addr_mapping.find(curr_addr);
                                if (it == FT_addr_mapping.end() || it->second.empty()) {
                                    return 0;  // Or any default value indicating no valid entry
                                }

                                uns max_uns = 0;
                                for (const auto& p : it->second) {
                                    if (p.first > max_uns) {
                                        max_uns = p.first;
                                    }
                                }
                                available_space -= (max_uns != 0) ? max_uns : 2;
                                // available_space -=  8;
                                if(available_space < required_length)
                                  break;
                              }
                          }
                      }
                  }
              }
              if(!not_found_cnt){
              STAT_EVENT(0, BUF_MAP_SET_INST_ALL_FOUND);
          }
          if(available_space < required_length)
            STAT_EVENT(0, BUF_MAP_BYPASS_SET_AND_OUT_USED);
      }
      should_insert = (available_space >= required_length);
      STAT_EVENT(0, BUF_MAP_BYPASS_DECISION_NO + should_insert);
        // std::cout << "Bypass decision - Should insert: " << (should_insert ? "true" : "false")
        //    << " (new_next_use: " << new_next_use 
        //    << ", furthest_use: " << furthest_use
        //    << ", available_space: " << available_space 
        //    << ", required_length: " << required_length << ")" << std::endl;
          // return should_insert;
          return should_insert ? furthest_idx : 0;
  
      } else {
          STAT_EVENT(0, BELADY_BYPASS_FIRST_NOT_FOUND);
          // std::cout << "Bypass decision No - Not found in addr_to_orders" << std::endl;
          return 0;
      }
  
  }


  uns buffer_find_replace_start(const std::vector<Addr>& line_addrs, const std::vector<Addr>& end_line_addrs, const std::vector<uns>& lengths,  const std::vector<Addr>& byte_length, const std::vector<uns>& n_uop, uns num_ft) {
    // Debug print for input sizes
    // std::cout << "Input sizes - line_addrs: " << line_addrs.size() 
    //           << ", lengths: " << lengths.size() 
    //           << ", num_ft: " << num_ft << std::endl;

    // ASSERT(0, line_addrs.size() == num_ft + 1);  // +1 for the inserting address
    ASSERT(0, line_addrs.size() == lengths.size());
    ASSERT(0, line_addrs.size() == byte_lengths.size());
    ASSERT(0, line_addrs.size() == end_line_addrs.size());

    for (size_t i = 0; i < line_addrs.size(); ++i) {
      for (size_t j = i + 1; j < line_addrs.size(); ++j) { 
          if (line_addrs[i] == line_addrs[j] && end_line_addrs[i] != end_line_addrs[j]) {
            ASSERT(0, 1); // should not happen
          }
      }
  }
    
    if (line_addrs.empty() || num_ft == 0) {
        return 1;
    }

    uns offset_bits = LOG2(UOP_CACHE_LINE_SIZE);
    uns num_sets = UOP_CACHE_LINES / UOP_CACHE_ASSOC;
    uns target_set = (line_addrs[0] >> offset_bits) % num_sets;
    for(uns i=0; i<line_addrs.size(); i++)
      add_to_ft_map(line_addrs[i], n_uop[i], end_line_addrs[i]);
    

    // First address is the new one to insert
    Addr new_addr = line_addrs[0];
    uns required_length = lengths[0];
    Addr new_addr_orders = buffer_find_timestamp(new_addr);
    // auto new_addr_orders = addr_to_orders.find(new_addr);

    // Debug print for new address details
    // std::cout << "Inserting New addr: 0x" << std::hex << new_addr 
    //           << ", required length: " << std::dec << required_length << std::endl;
    // if (new_addr_orders != addr_to_orders.end() && !new_addr_orders->second.empty()) {
    if (new_addr_orders !=0) {
        STAT_EVENT(0, BELADY_BYPASS_FIRST_FOUND);
        // unsigned long long new_next_use = new_addr_orders->second[0];
        unsigned long long new_next_use = new_addr_orders;
        
        // Debug print for timestamp
        // std::cout << "New addr next use timestamp: " << new_next_use << std::endl;

        // Addr furthest_addr = 0;
        unsigned long long furthest_use = 0;
        uns not_found_cnt = 0;
        uns available_space = UOP_CACHE_ASSOC;

        // Check existing addresses
        for (size_t i = 1; i < line_addrs.size(); i++) {
            Addr existing_addr = line_addrs[i];
            // auto existing_orders = addr_to_orders.find(existing_addr);

            auto existing_orders = buffer_find_timestamp(existing_addr);
            // auto existing_orders = addr_to_orders.find(existing_addr);

            // Debug print for each existing address
            // std::cout << "Checking in set addr[" << i << "]: 0x" << std::hex << existing_addr << std::dec;

            if (existing_orders != 0) {
            // if (existing_orders != addr_to_orders.end() && !existing_orders->second.empty()) {
                
                STAT_EVENT(0, BELADY_BYPASS_FOUND);
                // if (existing_orders->second[0] >= new_next_use) {
                if (existing_orders >= new_next_use) {
                   
                    if (furthest_use < existing_orders) {
                        // furthest_addr = existing_addr;
                        furthest_use = existing_orders;
                    }
                // }else if(existing_orders->second[0] < new_next_use){
                }else if(existing_orders < new_next_use){
                  // std::cout << " Found, next use: " << existing_orders->second[0];
                  // std::cout << " length: " << lengths[i];
                  available_space -= lengths[i];
                }
            } else {
                // std::cout << "in set Not found";
                STAT_EVENT(0, BELADY_BYPASS_NOT_FOUND);
                not_found_cnt++;
                // if (furthest_use < std::numeric_limits<unsigned long long>::max()) {
                    // furthest_addr = existing_addr;
                    furthest_use = std::numeric_limits<unsigned long long>::max();
                // }
            }
            // std::cout << std::endl;
        }

        // Debug print summary
        // std::cout << "Summary - Not found count: " << not_found_cnt 
        //           << ", Available space: " << available_space 
        //           << ", Furthest addr: 0x" << std::hex << furthest_addr 
        //           << ", Furthest use: " << std::dec << furthest_use << std::endl;

        // Rest of the code remains the same...
        bool should_insert;
        // if(!not_found_cnt){
        //   should_insert = (new_next_use < furthest_use) && (available_space >= required_length);
        // }else{
          
        // }
        // if(available_space < required_length)
        //   STAT_EVENT(0, BUF_MAP_BYPASS_SET_USED);
        // Additional check for multiple not-found addresses
        if (available_space >= required_length) {
          if(not_found_cnt){
            STAT_EVENT(0, BUF_MAP_SET_INST_NOT_FOUND);
            // uns reuse_count = 0;

            for (const auto& entry : order_to_addr) {
                if (entry.first > new_next_use) break;

                Addr curr_addr = entry.second;
                uns curr_set = (curr_addr >> offset_bits) % num_sets;

                if (curr_set == target_set) {
                    bool is_in_input = false;
                    for (const auto& addr : line_addrs) {
                        if ((addr & ~0x3f) == (curr_addr & ~0x3f)) {
                            is_in_input = true;
                            break;
                        }
                    }

                    if (!is_in_input) {
                        auto addr_orders = addr_to_orders.find(curr_addr);
                        // get_timestamps_for_cache_line(curr_addr& ~0x3f);
                        if (addr_orders != addr_to_orders.end() &&  addr_orders->second.size() > 1) {
                            // if (reuse_count <= not_found_cnt - 1)
                            //   reuse_count++;
                            // if (reuse_count >= not_found_cnt - 1) {
                            //     STAT_EVENT(0, BUF_MAP_SET_INST_NOT_FOUND_OTHER_FOUND);
                            //     // break;
                            // }
                            if(addr_orders->second[1]<new_next_use){
                              uns ft_len = get_uop_length(curr_addr);
                              available_space -= (ft_len != 0) ? ft_len : 8;
                              if(available_space < required_length)
                                break;
                            }
                        }
                    }
                }
            }
        } else {
            STAT_EVENT(0, BUF_MAP_SET_INST_ALL_FOUND);
        }
        if(available_space < required_length)
          STAT_EVENT(0, BUF_MAP_BYPASS_SET_AND_OUT_USED);
    }
    should_insert = (available_space >= required_length);
    STAT_EVENT(0, BUF_MAP_BYPASS_DECISION_NO + should_insert);
      // std::cout << "Bypass decision - Should insert: " << (should_insert ? "true" : "false")
      //    << " (new_next_use: " << new_next_use 
      //    << ", furthest_use: " << furthest_use
      //    << ", available_space: " << available_space 
      //    << ", required_length: " << required_length << ")" << std::endl;
        return should_insert;
        // return should_insert ? furthest_addr : line_addrs[0];

    } else {
        STAT_EVENT(0, BELADY_BYPASS_FIRST_NOT_FOUND);
        // std::cout << "Bypass decision No - Not found in addr_to_orders" << std::endl;
        return 0;
    }

}
  
  Addr buffer_find_timestamp(Addr lookup_addr) {
  
      // Look up the address in addr_to_orders
      auto it = addr_to_orders.find(lookup_addr);
      
      // If address found and it has orders
      if (it != addr_to_orders.end() && !it->second.empty()) {
          // Return the first (smallest/earliest) order
          return it->second[0];
      }
      
      // Return 0 if address not found or no orders
      return 0;
  }
  
  Addr find_valid_ft_timestamp(Addr addr, Addr end_addr, uns length, uns n_uop) {
    
    STAT_EVENT(0, BELADY_FT_CHECKED);
    // if((end_addr) == (addr)) {//same line
    //   STAT_EVENT(0, BELADY_FT_ONE_LINE);
    //   return buffer_find_timestamp(addr);
    // }
    // bool use_scan = 1;
    bool found_match = false;
    if(USE_SCAN_BUF_FT){
      auto begin_it = addr_to_buf_pos.find(addr);
      auto begin_ts = addr_to_orders.find(addr);
      if (begin_it == addr_to_buf_pos.end()  || begin_it->second.empty()) 
        return 0;
      if (begin_ts == addr_to_orders.end()  || begin_ts->second.empty()) 
        return 0;
      auto all_ft_patterns =  lookup_ft_map(addr);
      // Check if (n_uop, end_addr) exists in all_ft_patterns
    bool found = false;
    for (const auto& [uns_value, addr_value] : all_ft_patterns) {
        if (uns_value == n_uop && addr_value == end_addr) {
            found = true;
            break;
        }
    }

    // Assert if the pair is not found
    if(!found)
      ASSERT(0, 1);
      STAT_EVENT(0, BELADY_FT_START_FOUND);
      for (uns j = 0; j < begin_it->second.size(); j++) {
        uint64_t ts = begin_it->second[j];
        if (ts >= MEMTRACE_BUF_SIZE) {
            continue;  // Skip invalid index
        }
        
        
        // Check if we find an exact match for our target pattern
        for (uns i = 0; i < n_uop; i++) {
          STAT_EVENT(0, BELADY_FT_END_ITER);
            if (circ_buf[(ts + i) % MEMTRACE_BUF_SIZE].instruction_addr == end_addr) {
                found_match = true;
                // STAT_EVENT(0, BELADY_FT_END_FOUND);
                // Now check if this is part of a larger pattern
                bool is_part_of_larger_pattern = false;
                
                for (const auto& pattern : all_ft_patterns) {
                    uns other_n_uop = pattern.first;
                    Addr other_end_addr = pattern.second;

                    
                    // Only check longer patterns
                    if (other_n_uop > n_uop) {
                      STAT_EVENT(0, BELADY_ALTER_FT_LONGER_CHECK_EXIST);
                      ASSERT(0, end_addr != other_end_addr);
                      //  STAT_EVENT(0, BELADY_ALTER_FT_LONGER_CHECK_EXIST);
                        // Check if a longer pattern that starts at the same position
                        // contains our end address later in its sequence
                        for (uns k = i; k < (other_n_uop); k++) {
                            if (circ_buf[(ts + k) % MEMTRACE_BUF_SIZE].instruction_addr == other_end_addr) {
                                // Found a longer pattern that contains our target pattern
                                // STAT_EVENT(0, BELADY_ALTER_FT_LONGER_FOUND);
                                is_part_of_larger_pattern = true;
                                break;
                            }
                        }
                        
                        if (is_part_of_larger_pattern) {
                            break; // No need to check other patterns
                        }
                    }
                    }
                
                // If we found our pattern and it's not part of a larger one, return it
                if (!is_part_of_larger_pattern) {
                  STAT_EVENT(0, BELADY_FT_VALID_END_FOUND);
                    return begin_ts->second[j];
                } else {
                    // This match is part of a larger pattern, continue to next j
                    // STAT_EVENT(0, BELADY_ALTER_FT_LONGER_FOUND);
                    break;
                }
                // if we found a match skip the index
                continue;
            }
        }
      }
      if(found_match)   
        STAT_EVENT(0, BELADY_FT_END_FOUND_ALL_LONG);
      STAT_EVENT(0, BELADY_FT_END_NOT_FOUND);
      return 0;

    }else{
          if((end_addr) == (addr)) {//same line
      // STAT_EVENT(0, BELADY_FT_ONE_LINE);
      return buffer_find_timestamp(addr);
    }
      // Lookup timestamps for both the start and end line address
      auto begin_it = addr_to_orders.find(addr);
      auto end_it = addr_to_orders.find(end_addr);
      // auto end_it = addr_to_orders.find(end_addr);
    
      if ( end_it == addr_to_orders.end() || end_it->second.empty() ) {
      // if (end_it.empty() ) {
            STAT_EVENT(0, BELADY_FT_END_NOT_FOUND);
      }
      if (begin_it == addr_to_orders.end()  || 
          begin_it->second.empty()) {
            STAT_EVENT(0, BELADY_FT_BEGIN_NOT_FOUND);
          // return 0;  // If either address is missing or has no timestamps, return no result
      }
      if (begin_it == addr_to_orders.end()  ||end_it == addr_to_orders.end() ||
          begin_it->second.empty() || end_it->second.empty()) {
            STAT_EVENT(0, BELADY_FT_BOTH_END_NOT_FOUND);
          return 0;  // If either address is missing or has no timestamps, return no result
      }
      STAT_EVENT(0, BELADY_FT_BOTH_END_FOUND);
      const std::vector<unsigned long long>& begin_timestamps = begin_it->second;
      const std::vector<unsigned long long>& end_timestamps = end_it->second;
    
      std::vector<unsigned long long> sorted_end_timestamps = end_timestamps;  
      std::sort(sorted_end_timestamps.begin(), sorted_end_timestamps.end());  // Sort in ascending order
      
      std::vector<unsigned long long> sorted_begin_timestamps = begin_timestamps;  
      std::sort(sorted_begin_timestamps.begin(), sorted_begin_timestamps.end());
      
      for (unsigned long long ts : sorted_begin_timestamps) {
          auto it = std::lower_bound(sorted_end_timestamps.begin(), sorted_end_timestamps.end(), ts);
      
          if (it != sorted_end_timestamps.end() && (*it >= ts) &&  (*it - ts) <= UOP_CACHE_ASSOC) {
              STAT_EVENT(0, BELADY_FT_END_NEAR_FOUND);
              return ts;
          }
      }
      
    
    
      return 0;  // No valid timestamp found
    }
    return 0;
  }


  std::vector<unsigned long long> get_timestamps_for_cache_line(Addr addr) {
    Addr line_addr = addr & CLINE;  // Get base address of the cache line
    std::vector<unsigned long long> all_timestamps;
  
    // std::cout << "Fetching timestamps for cache line: " << std::hex << line_addr << std::dec << std::endl;
  
    // Iterate over all stored addresses and collect timestamps for those in the same cache line
    for (const auto& entry : addr_to_orders) {
        Addr stored_line_addr = entry.first & CLINE;  // Extract cache line of stored address
        if (stored_line_addr == line_addr) {
            // std::cout << "  Including address: " << std::hex << entry.first << std::dec << std::endl;
            all_timestamps.insert(all_timestamps.end(), entry.second.begin(), entry.second.end());
        }
    }
  
    // Sort timestamps to maintain order
    std::sort(all_timestamps.begin(), all_timestamps.end());
  
    // std::cout << "Collected timestamps: ";
    // for (auto ts : all_timestamps) std::cout << ts << " ";
    // std::cout << std::endl;
  
    return all_timestamps;
  }
  
  
  void memtrace_setup(uns proc_id) {
    std::string path(trace_files[proc_id]);
    std::string trace(path);
    std::string binaries(MEMTRACE_MODULES_LOG);
  
    trace_readers[proc_id] = new TraceReaderMemtrace(trace, binaries, 1);
  
    if(FAST_FORWARD) {
      ASSERT(proc_id, !MEMTRACE_ROI_BEGIN && !MEMTRACE_ROI_END);
      uint64_t inst_count_to_use = USE_FETCHED_COUNT ?
                                    ins_id_fetched : ins_id;
      std::cout << "Enter fast forward " << inst_count_to_use << std::endl;
      // FFWD the first instruction and as many as later ffwding parameters specify.
      // insi is invalid once end of trace is reached.
      // Reaching the end of the trace breaks out of the loop and segfaults later in this function.
      const InstInfo *insi;
      do {
        insi = trace_readers[proc_id]->nextInstruction();
        ins_id++;
        if(insi->fetched_instruction) {
          ins_id_fetched++;
        }
  
        inst_count_to_use = USE_FETCHED_COUNT ? ins_id_fetched : ins_id;
  
        if((inst_count_to_use % 10000000) == 0)
          std::cout << "Fast forwarded " << inst_count_to_use << " instructions."
          << (insi->valid ? " Valid" : " Invalid") << " instr." << std::endl;
      } while(ffwd(insi->ins));
      std::cout << "Exit fast forward " << inst_count_to_use << std::endl;
    }
  
    if (MEMTRACE_BUF_SIZE) {
      circ_buf.resize(MEMTRACE_BUF_SIZE);
      rdptr = 0;
      wrptr = 0;
      for (uint i = 0; i < MEMTRACE_BUF_SIZE; i++) {
        memtrace_trace_read_internal(proc_id, &circ_buf[wrptr]);
        buf_map_insert();
      }
    }
  }
  