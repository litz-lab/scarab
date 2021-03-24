#ifndef CHAMPSIM_TRACE_READER_HPP
#include <string>
#include <memory>
#include <lzma.h>
#include "champsim_inst.hpp"

class ChampsimTraceReader_t {
    public:
        ChampsimTraceReader_t(std::string&& filename);
        // returns the next instruction from the trace
        champsim_instruction_info* nextInstruction();
        ~ChampsimTraceReader_t();
        ChampsimTraceReader_t(const ChampsimTraceReader_t&) = delete;
    private:
         // parse the stream
        bool read_next_line();
        // transform stream representation into champsim_instruction_info
        bool processInst();
        // reads new data from file into input buffer
        bool refresh_input();
        lzma_stream strm;
        /* FILE* file; */
        void* mmap_region;
        size_t size;
        /* uint8_t input_buffer[BUFSIZ]; // bufsize recommended from LZMA */
        input_instr insn_a, insn_b; // double-buffer design to keep a 1-lookahead
        bool use_insn_a;
        champsim_instruction_info info_a, info_b;
        lzma_ret ret;
};
#endif
