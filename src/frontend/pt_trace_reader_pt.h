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

#include "general.param.h"

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
  uint64_t num_nops_in_trace = 0, num_inserted_nops = 0;
  uint64_t num_direct_brs_in_trace = 0, num_inserted_direct_brs = 0;

public:
  bool read_next_line(PTInst &inst) {
      static uns64 num_nops_at_start = 0;
      if(num_nops_at_start <= NUM_NOPS) { // = is because the last one will be overwritten as a JMP to the real instruction stream
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

  // TODO: Move this to memtrace_trace_reader.h
  xed_decoded_inst_t* createJmp(uint64_t displacement) {
      xed_encoder_instruction_t inst;
      xed_state_t state;
      state.mmode = XED_MACHINE_MODE_LONG_64;
      xed_encoder_request_t req;
      xed_inst1(&inst, state, XED_ICLASS_JMP, 64,  xed_relbr(displacement - 5, 32)); // -5 is due to this jump insn being 5 bytes large (1 op, 4 32 bit disp)
      xed_encoder_request_zero_set_mode(&req, &state);
      if(!xed_convert_to_encoder_request(&req, &inst)) {
          panic("Encoder conversion failed! Is the displacement too large?");
          return nullptr;
      }
      xed_uint8_t encodedBytes[15];
      unsigned int numBytesUsed = 0;
      xed_error_enum_t error = xed_encode(&req, encodedBytes, sizeof(encodedBytes), &numBytesUsed);
      if(error != XED_ERROR_NONE) {
          panic("Failed to encode due to: %s\n", xed_error_enum_t2str(error));
          return nullptr;
      }
      xed_decoded_inst_t* decoded_inst = new xed_decoded_inst_t;
      xed_decoded_inst_zero(decoded_inst);
      xed_decoded_inst_set_mode(decoded_inst, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);
      error = xed_decode(decoded_inst, encodedBytes, numBytesUsed);
      if(error == XED_ERROR_NONE) {
          return decoded_inst;
      }
      delete decoded_inst;
      panic("Could not decode due to %s\n", xed_error_enum_t2str(error));
      return nullptr;
  }

  xed_decoded_inst_t * createNop(uint64_t length) {
      xed_state_t state;
      state.mmode = XED_MACHINE_MODE_LONG_64;
      uint8_t buf[10];
      xed_error_enum_t res = xed_encode_nop(buf, length);
      if(res != XED_ERROR_NONE) {
          panic("Failed to encode due to %s\n", xed_error_enum_t2str(res));
          return nullptr;
      }
      xed_decoded_inst_t* decoded_inst = new xed_decoded_inst_t;
      xed_decoded_inst_zero_set_mode(decoded_inst, &state);
      res = xed_decode(decoded_inst, buf, sizeof(buf));
      if(res != XED_ERROR_NONE) {
          panic("XED NOP decode error! %s\n", xed_error_enum_t2str(res));
          delete decoded_inst;
          return nullptr;
      }
      return decoded_inst;
  }

  // ret true when insn is a syscall (and thus should be skipped)
  bool processInst(PTInst &next_line) {
      /* std::cout << "Processing Inst w/ PC: " << std::hex << next_line.pc << std::endl; */
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
      auto& ins = _prior; // have to do this for the macros to work
      if(ins.ins) {
          if(INS_Category(ins) == XED_CATEGORY_NOP) {
                  ++num_nops_in_trace;
          } else if (INS_IsDirectBranchOrCall(ins)) {
              ++num_direct_brs_in_trace;
          }
      }
      bool inserted_nop = false;
      if (_prior.valid && INS_IsRep(ins)) {
        // repz insns aren't supported, so just nop them
        auto length = xed_decoded_inst_get_length(_prior.ins);
        // std::cout << xed_iclass_enum_t2str(INS_Opcode(ins)) << " with PC " << std::hex << _prior.pc << " will become a nop of length " << length << std::endl;
        _prior.ins = createNop(length);
        inserted_nop = true;
        if(_prior.pc == next_line.pc) {
            _info = _prior; // skip prior insn
            return true;
        }
      }
      bool changes_cf = ins.ins && INS_ChangeControlFlow(ins);
      bool incorrect_branch = ins.ins && INS_IsDirectBranchOrCall(ins) && next_line.pc != INS_DirectBranchOrCallTargetAddress(ins) && next_line.pc != (ins.pc + INS_Size(ins));
    if(incorrect_branch) {
        // std::cout << "branch " << INS_Address(ins) << " is incorrect!" << std::endl;
        // std::cout << "xed target: " << INS_DirectBranchOrCallTargetAddress(ins) << " next pc: " << next_line.pc << std::endl;
    }
    if(_prior.valid && (!changes_cf || INS_Category(ins) == XC(SYSCALL) || incorrect_branch) && next_line.pc != _prior.pc + xed_decoded_inst_get_length(_prior.ins)) {
        // std::cout << xed_iclass_enum_t2str(INS_Opcode(ins)) << " with PC " << std::hex << _prior.pc << " will become a jump to " << std::hex << next_line.pc << std::endl;
        int64_t diff = std::max(next_line.pc, _prior.pc) - std::min(next_line.pc, _prior.pc);
        if(next_line.pc < _prior.pc)
            diff *= -1;
        // std::cout << "Jump: " << diff << std::endl;
        xed_decoded_inst_t* new_inst = createJmp(diff);
        _prior.ins = new_inst;
        inserted_nop = false; // replaced nop with a jmp, so we really inserted a jmp instead of a nop
        ++num_inserted_direct_brs;
        _prior.static_target = next_line.pc;
    } else if (_prior.valid && xed_decoded_inst_get_attribute(ins.ins, XED_ATTRIBUTE_REP) > 0) {
        // repz insns aren't supported, so just nop them
        auto length = xed_decoded_inst_get_length(_prior.ins);
        // std::cout << xed_iclass_enum_t2str(INS_Opcode(ins)) << " with PC " << std::hex << _prior.pc << " will become a nop of length " << length << std::endl;
        _prior.ins = createNop(length);
        ++num_inserted_nops;
        if(_prior.pc == next_line.pc) {
            _info = _prior; // skip prior insn
            return true;
        }
    }
    if(inserted_nop)
        ++num_inserted_nops;
    _info.pc = next_line.pc;
    _info.ins = xed_ins;
    _info.pid = 1;
    _info.tid = 1;
    _info.target = 0; // Set when the next instruction is evaluated
    _info.static_target = 0; // Set when the next instruction is evaluated
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
    if(_prior.valid)
        _prior.taken = _info.pc != (_prior.pc + INS_Size(ins));
    return false;
  }
  TraceReaderPT(
      const std::string &_trace, bool _enable_code_bloat_effect = false,
      std::map<uint64_t, uint64_t> *_prev_to_new_bbl_address_map = nullptr) {
    raw_file = gzopen(_trace.c_str(), "rb");
    if (!raw_file) {
      panic("TraceReaderPT: Invalid GZ File");
      throw "Could not open file";
    }
    enable_code_bloat_effect = _enable_code_bloat_effect;
    prev_to_new_bbl_address_map = _prev_to_new_bbl_address_map;
  }
  const InstInfo *getNextInstruction() override {
    PTInst& next_line = (use_info_a ? pt_inst_a : pt_inst_b);
    do {
        if (read_next_line(next_line) == false)
          return &invalid_info_;
    } while(processInst(next_line));
    // todo: have process inst ret if I should use this info or not
    // want to be able to skip syscalls for now
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
      std::cout << std::dec << "num trace nops: " << num_nops_in_trace << " , num added nops: " << num_inserted_nops << ", ratio: " << double(num_inserted_nops) / double(num_nops_in_trace) << std::endl;
      std::cout << "num trace direct brs: " << num_direct_brs_in_trace << " , num added direct brs: " << num_inserted_direct_brs << ", ratio: " << double(num_inserted_direct_brs) / double(num_direct_brs_in_trace) << std::endl;
    if (raw_file != NULL)
      gzclose(raw_file);
  }
};

#endif // __PT_TRACE_READER_PT_H__
