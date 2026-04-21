/* Copyright 2020 University of Michigan (implemented by Tanvir Ahmed Khan)
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
 * File         : frontend/pt_memtrace/pt_trace_reader_pt.h
 * Author       : Tanvir Ahmed Khan
 * Date         : 12/05/2020
 * Notes        : This code has been adapted from zsim which was released under
 *                GNU General Public License as published by the Free Software
 *                Foundation, version 2.
 * Description  : Interface to read gziped Intel processor trace
 ***************************************************************************************/
#ifndef __PT_TRACE_READER_PT_H__
#define __PT_TRACE_READER_PT_H__
#include <map>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <vector>
#include <zlib.h>

#include "general.param.h"

#include "frontend/pt_memtrace/memtrace_trace_reader.h"

#include "ctype_pin_inst.h"

#define GZ_BUFFER_SIZE 80
#define panic(...) printf(__VA_ARGS__)

struct PTInst {
  uint64_t pc;
  uint8_t size;
  uint8_t inst_bytes[16];
};

class TraceReaderPT : public TraceReader {
 private:
  gzFile raw_file = NULL;
  InstInfo inst_info_a = {};
  InstInfo inst_info_b = {};
  PTInst pt_inst_a = {}, pt_inst_b = {};
  bool enable_code_bloat_effect = false;
  bool use_info_a = true;  // true when filling info a, false when filling info b
  std::map<uint64_t, uint64_t> *prev_to_new_bbl_address_map = nullptr;
  uint64_t num_nops_in_trace = 0, num_inserted_nops = 0;
  uint64_t num_direct_brs_in_trace = 0, num_inserted_direct_brs = 0;
  std::vector<std::string> parsed = {};
  ctype_pin_inst patch_inst_{};

 public:
  bool read_next_line(PTInst &inst) {
    static uns64 num_nops_at_start = 0;
    if (num_nops_at_start <=
        NUM_NOPS) {  // = is because the last one will be overwritten as a JMP to the real instruction stream
      inst.pc = NOPS_BB_START + num_nops_at_start++;
      inst.size = 1;
      inst.inst_bytes[0] = 0x90;
      return true;
    }
    if (raw_file == NULL)
      return false;
    char buffer[GZ_BUFFER_SIZE];
    if (gzgets(raw_file, buffer, GZ_BUFFER_SIZE) == Z_NULL)
      return false;
    std::string line = buffer;

    parsed.clear();
    std::stringstream check1(line);
    std::string intermediate;
    while (getline(check1, intermediate, ' ')) {
      parsed.emplace_back(intermediate);
    }
    if (parsed.size() < 3)
      panic("TraceReaderPT: GZ File line has less than 3 items");
    assert(parsed[1].length() == 0);
    inst.pc = strtoul(parsed[0].c_str(), NULL, 16);
    inst.size = strtoul(parsed[2].c_str(), NULL, 10);

    int found = 0;
    assert(inst.size);
    for (uint8_t i = 3; found < inst.size; i++) {
      if (parsed[i].length() == 0) {
        continue;
      }
      inst.inst_bytes[found++] = strtoul(parsed[i].c_str(), NULL, 16);
    }

    if (enable_code_bloat_effect && (prev_to_new_bbl_address_map != nullptr)) {
      uint64_t result = inst.pc;
      auto it = prev_to_new_bbl_address_map->lower_bound(inst.pc);
      if (it->first == inst.pc) {
        result = it->second;
      } else {
        if (it == prev_to_new_bbl_address_map->begin())
          result = inst.pc;
        else {
          it--;
          result = it->second + (inst.pc - (it->first));
        }
      }
      inst.pc = result;
    }
    return true;
  }

  // ret true when insn is a syscall (and thus should be skipped)
  bool processInst(PTInst &next_line) {
    auto xed_map_iter = xed_map_.find(next_line.pc);
    if (xed_map_iter == xed_map_.end()) {
      fillCache(next_line.pc, next_line.size, next_line.inst_bytes);
      xed_map_iter = xed_map_.find(next_line.pc);
      assert((xed_map_iter != xed_map_.end()));
    }
    bool unknown_type, cond_branch;
    int mem_ops_;
    xed_decoded_inst_t *xed_ins;
    auto &xed_tuple = (*xed_map_iter).second;
    tie(mem_ops_, unknown_type, cond_branch, std::ignore, std::ignore) = xed_tuple;
    xed_ins = std::get<MAP_XED>(xed_tuple).get();
    InstInfo &_info = (use_info_a ? inst_info_a : inst_info_b);
    InstInfo &_prior = (use_info_a ? inst_info_b : inst_info_a);
    bool inserted_nop = false;
    if (_prior.valid && !_prior.fake_inst) {
      auto &ins = _prior;
      if (ins.ins) {
        if (XED_INS_Category(ins.ins) == XED_CATEGORY_NOP) {
          ++num_nops_in_trace;
        } else if (XED_INS_IsDirectBranchOrCall(ins.ins)) {
          ++num_direct_brs_in_trace;
        }
      }

      bool orig_is_rep = ins.ins && XED_INS_IsRep(ins.ins);
      bool orig_changes_cf = ins.ins && XED_INS_ChangeControlFlow(ins.ins);
      bool orig_is_direct_br = ins.ins && XED_INS_IsDirectBranchOrCall(ins.ins);
      uint64_t orig_direct_target = orig_is_direct_br ? XED_INS_DirectBranchOrCallTargetAddress(ins.pc, ins.ins) : 0;
      uint8_t orig_size = ins.ins ? (uint8_t)XED_INS_Size(ins.ins) : 0;
      auto orig_category = ins.ins ? XED_INS_Category(ins.ins) : 0;

      if (orig_is_rep) {
        patch_inst_ = create_dummy_nop(_prior.pc, WPNM_FAKE_NOP, orig_size);
        _prior.info = &patch_inst_;
        _prior.fake_inst = true;
        inserted_nop = true;
        if (_prior.pc == next_line.pc) {
          _info = _prior;
          return true;
        }
      }

      bool changes_cf = inserted_nop ? false : orig_changes_cf;
      bool incorrect_branch = !inserted_nop && orig_is_direct_br && next_line.pc != orig_direct_target &&
                              next_line.pc != (ins.pc + orig_size);
      uint8_t prior_size = inserted_nop ? patch_inst_.size : orig_size;

      if (_prior.valid && (!changes_cf || orig_category == XC(SYSCALL) || incorrect_branch) &&
          next_line.pc != _prior.pc + prior_size) {
        patch_inst_ = create_dummy_jump(_prior.pc, next_line.pc, XED_ICLASS_JMP);
        _prior.info = &patch_inst_;
        _prior.fake_inst = true;
        inserted_nop = false;
        ++num_inserted_direct_brs;
        _prior.static_target = next_line.pc;
      } else if (!inserted_nop && orig_is_rep) {
        patch_inst_ = create_dummy_nop(_prior.pc, WPNM_FAKE_NOP, orig_size);
        _prior.info = &patch_inst_;
        _prior.fake_inst = true;
        ++num_inserted_nops;
        if (_prior.pc == next_line.pc) {
          _info = _prior;
          return true;
        }
      }
    }
    if (inserted_nop)
      ++num_inserted_nops;
    _info.pc = next_line.pc;
    _info.ins = xed_ins;
    _info.fake_inst = false;
    _info.pid = 1;
    _info.tid = 1;
    _info.target = 0;
    _info.static_target = 0;
    _prior.target = _info.pc;
    xed_category_enum_t category = xed_decoded_inst_get_category(xed_ins);
    _info.taken = category == XED_CATEGORY_UNCOND_BR || category == XED_CATEGORY_COND_BR ||
                  category == XED_CATEGORY_CALL || category == XED_CATEGORY_RET;
    _info.mem_addr[0] = 0x4040;
    _info.mem_addr[1] = 0x8080;
    _info.mem_used[0] = false;
    _info.mem_used[1] = false;
    _info.unknown_type = unknown_type;
    _info.valid = true;

    for (int i = 0; i < mem_ops_; i++) {
      if (i >= 2)
        break;
      _info.mem_used[i] = true;
    }
    if (_prior.valid) {
      if (_prior.fake_inst)
        _prior.taken = _info.pc != (_prior.pc + _prior.info->size);
      else
        _prior.taken = _info.pc != (_prior.pc + XED_INS_Size(_prior.ins));
    }
    return false;
  }
  TraceReaderPT(const std::string &_trace, bool _enable_code_bloat_effect = false,
                std::map<uint64_t, uint64_t> *_prev_to_new_bbl_address_map = nullptr) {
    raw_file = gzopen(_trace.c_str(), "rb");
    if (!raw_file) {
      panic("TraceReaderPT: Invalid GZ File");
      throw "Could not open file";
    }
    enable_code_bloat_effect = _enable_code_bloat_effect;
    prev_to_new_bbl_address_map = _prev_to_new_bbl_address_map;
    has_trace_encodings_ = true;
    inst_info_a.valid = false;
    inst_info_a.fake_inst = false;
    inst_info_b.valid = false;
    inst_info_b.fake_inst = false;
    init("");
    initTrace();
  }
  int i = 0;
  const InstInfo *getNextInstruction() override {
    PTInst &next_line = (use_info_a ? pt_inst_a : pt_inst_b);
    do {
      if (read_next_line(next_line) == false)
        return &invalid_info_;
    } while (processInst(next_line));
    // todo: have process inst ret if I should use this info or not
    // want to be able to skip syscalls for now
    InstInfo &_prior = (use_info_a ? inst_info_b : inst_info_a);
    static bool should_be_valid = false;
    if (should_be_valid)
      assert(_prior.valid);
    should_be_valid = true;
    use_info_a = !use_info_a;
    return &_prior;
  }
  bool initTrace() override {
    getNextInstruction();  // fill in info a, will lack BP information (hopefully we won't need it...)
    return true;
  }
  bool locationForVAddr(uint64_t _vaddr, uint8_t **_loc, uint64_t *_size) override {
    // do nothing
    return true;
  }
  ~TraceReaderPT() {
    std::cout << std::dec << "num trace nops: " << num_nops_in_trace << " , num added nops: " << num_inserted_nops
              << ", ratio: " << double(num_inserted_nops) / double(num_nops_in_trace) << std::endl;
    std::cout << "num trace direct brs: " << num_direct_brs_in_trace
              << " , num added direct brs: " << num_inserted_direct_brs
              << ", ratio: " << double(num_inserted_direct_brs) / double(num_direct_brs_in_trace) << std::endl;
    if (raw_file != NULL)
      gzclose(raw_file);
  }
};

#endif  // __PT_TRACE_READER_PT_H__
