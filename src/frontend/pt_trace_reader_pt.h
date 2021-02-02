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
 * File         : frontend/pt_trace_reader_pt.h
 * Author       : Tanvir Ahmed Khan
 * Date         : 12/05/2020
 * Notes        : This code has been adapted from zsim which was released under
 *                GNU General Public License as published by the Free Software
 *                Foundation, version 2.
 * Description  : Interface to read gziped Intel processor trace
 ***************************************************************************************/
#ifndef __PT_TRACE_READER_PT_H__
#define __PT_TRACE_READER_PT_H__
#include <stdlib.h>
#include <zlib.h>

#include <map>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>

#include "frontend/memtrace_trace_reader.h"

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
  InstInfo inst_info_a;
  InstInfo inst_info_b;
  PTInst   pt_inst_a, pt_inst_b;
  bool enable_code_bloat_effect = false;
  bool use_info_a = true; // true when filling info a, false when filling info b
  std::map<uint64_t, uint64_t> *prev_to_new_bbl_address_map = nullptr;

public:
  bool read_next_line(PTInst &inst) {
    if (raw_file == NULL)
      return false;
    char buffer[GZ_BUFFER_SIZE];
    if (gzgets(raw_file, buffer, GZ_BUFFER_SIZE) == Z_NULL)
      return false;
    std::string line = buffer;
    boost::trim_if(line, boost::is_any_of("\n"));
    std::vector<std::string> parsed;
    boost::split(parsed, line, boost::is_any_of(" \n"),
                 boost::token_compress_on);
    if (parsed.size() < 3)
      panic("TraceReaderPT: GZ File line has less than 3 items");
    inst.pc = strtoul(parsed[0].c_str(), NULL, 16);
    inst.size = strtoul(parsed[1].c_str(), NULL, 10);
    for (uint8_t i = 0; i < inst.size; i++) {
      inst.inst_bytes[i] = strtoul(parsed[i + 2].c_str(), NULL, 16);
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
  void processInst(PTInst &next_line) {
    // Get the XED info from the cache, creating it if needed
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
    tie(mem_ops_, unknown_type, cond_branch, std::ignore, std::ignore) =
        xed_tuple;
    xed_ins = std::get<MAP_XED>(xed_tuple).get();
    InstInfo& _info = (use_info_a ? inst_info_a : inst_info_b);
    InstInfo& _prior = (use_info_a ? inst_info_b : inst_info_a);
    _info.pc = next_line.pc;
    _info.ins = xed_ins;
    _info.pid = 1;
    _info.tid = 1;
    _info.target = 0; // Set when the next instruction is evaluated
    _prior.target = _info.pc;
    _info.taken =
        cond_branch; // Patched when the next instruction is evaluated
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
    // TODO add this?
    // _prior.taken = _info.pc != (_prior.pc + _prior.isize);
  }
  TraceReaderPT(
      const std::string &_trace, bool _enable_code_bloat_effect = false,
      std::map<uint64_t, uint64_t> *_prev_to_new_bbl_address_map = nullptr) {
    raw_file = gzopen(_trace.c_str(), "rb");
    if (!raw_file)
      panic("TraceReaderPT: Invalid GZ File");
    enable_code_bloat_effect = _enable_code_bloat_effect;
    prev_to_new_bbl_address_map = _prev_to_new_bbl_address_map;
  }
  const InstInfo *getNextInstruction() override {
    PTInst& next_line = (use_info_a ? pt_inst_a : pt_inst_b);
    if (read_next_line(next_line) == false)
      return &invalid_info_;
    processInst(next_line);
    InstInfo& _prior = (use_info_a ? inst_info_b : inst_info_a);
    static bool should_be_valid = false;
    if(should_be_valid)
        assert(_prior.valid);
    should_be_valid = true;
    use_info_a = !use_info_a;
    return &_prior;
  }
  void binaryGroupPathIs(const std::string &_path) override {
    // do nothing
  }
  bool initTrace() override {
    getNextInstruction(); // fill in info a, will lack BP information (hopefully we won't need it...)
    return true;
  }
  bool locationForVAddr(uint64_t _vaddr, uint8_t **_loc,
                        uint64_t *_size) override {
    // do nothing
    return true;
  }
  ~TraceReaderPT() {
    if (raw_file != NULL)
      gzclose(raw_file);
  }
};

#endif // __PT_TRACE_READER_PT_H__
