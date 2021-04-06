#include "op.h"
#include "champsim_trace_reader.hpp"
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <iostream>

off_t fsize(const std::string& filename) {
    struct stat st;
    if(!stat(filename.c_str(), &st))
        return st.st_size;
    throw std::runtime_error("Could not stat file: " + filename);
}

ChampsimTraceReader_t::ChampsimTraceReader_t(std::string&& filename): strm(LZMA_STREAM_INIT), /*file(nullptr),*/ use_insn_a(true) {
    off_t size_o = fsize(filename);
    size = size_o;
    if(size != size_o)
        throw std::runtime_error("Could not get size of file into a size_t!");
    int fd = open(filename.c_str(), O_RDONLY);
    if(fd == -1)
        throw std::runtime_error("Could not open file: " + filename);
    mmap_region = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    strm = LZMA_STREAM_INIT;
    ret = lzma_stream_decoder(&strm, UINT64_MAX, 0);
    if(ret != LZMA_OK) {
        throw std::runtime_error("Could not init stream!");
    }
    strm.next_in = (const uint8_t*)mmap_region;
    strm.avail_in = size;
    close(fd);
    auto i = nextInstruction(); // read in one insn, so the one-lookahead can work
    assert(!i); // shouldn't be valid
    /*
    try {
        file = fopen(filename.data(), "r");
        if(!file) {
            throw std::runtime_error("Could not open file for champsim reading!");
        }
        ret = lzma_stream_decoder(&strm, UINT64_MAX, 0);
        if(ret != LZMA_OK)
            throw std::runtime_error("Could not init lzma decoder!");
        if(!refresh_input()) {
            throw std::runtime_error("Could not read any data from the file!");
        }
        auto i = nextInstruction(); // read in one insn, so the one-lookahead can work
        assert(!i); // shouldn't be valid
    } catch(...) {
        lzma_end(&strm);
        if(file)
            fclose(file);
        throw;
    }
    */
}

ChampsimTraceReader_t::~ChampsimTraceReader_t() {
    lzma_end(&strm);
    munmap(mmap_region, size);
    /* fclose(file); */
}

champsim_instruction_info* ChampsimTraceReader_t::nextInstruction() {
    bool success = true;
    success = success && read_next_line();
    success = success && processInst();
    /* std::cout << "insn a ip: " << std::hex << insn_a.ip << " insn b ip: " << std::hex << insn_b.ip << std::endl; */
    use_insn_a = !use_insn_a;
    if(!success)
        return nullptr;
    return use_insn_a ? &info_a : &info_b;
}

bool ChampsimTraceReader_t::read_next_line() {
    if(ret == LZMA_STREAM_END || !strm.avail_in)
        return false;
    strm.next_out = reinterpret_cast<uint8_t*>(use_insn_a ? &insn_a : &insn_b);
    strm.avail_out = sizeof(input_instr);
    if(!strm.avail_in && !refresh_input())
        return false;

    assert(strm.next_out == (uint8_t*)&insn_a || strm.next_out == (uint8_t*)&insn_b);
    ret = lzma_code(&strm, LZMA_RUN);
    if(ret == LZMA_STREAM_END) {
        std::cerr << "stream end!" << std::endl;
    }
    if(ret != LZMA_OK && ret != LZMA_STREAM_END) {
			const char *msg;
			switch (ret) {
			case LZMA_MEM_ERROR:
				msg = "Memory allocation failed";
				break;

			case LZMA_FORMAT_ERROR:
				// .xz magic bytes weren't found.
				msg = "The input is not in the .xz format";
				break;

			case LZMA_OPTIONS_ERROR:
				// For example, the headers specify a filter
				// that isn't supported by this liblzma
				// version (or it hasn't been enabled when
				// building liblzma, but no-one sane does
				// that unless building liblzma for an
				// embedded system). Upgrading to a newer
				// liblzma might help.
				//
				// Note that it is unlikely that the file has
				// accidentally became corrupt if you get this
				// error. The integrity of the .xz headers is
				// always verified with a CRC32, so
				// unintentionally corrupt files can be
				// distinguished from unsupported files.
				msg = "Unsupported compression options";
				break;

			case LZMA_DATA_ERROR:
				msg = "Compressed file is corrupt";
				break;

			case LZMA_BUF_ERROR:
				// Typically this error means that a valid
				// file has got truncated, but it might also
				// be a damaged part in the file that makes
				// the decoder think the file is truncated.
				// If you prefer, you can use the same error
				// message for this as for LZMA_DATA_ERROR.
				msg = "Compressed file is truncated or "
						"otherwise corrupt";
				break;

			default:
				// This is most likely LZMA_PROG_ERROR.
				msg = "Unknown error, possibly a bug";
				break;
			}

            std::cerr << "Decoder error: " << msg << " (error code " << ret << ")\n";
        throw std::runtime_error("LZMA error");
    }
    return true;
}

bool ChampsimTraceReader_t::processInst() {
    input_instr& insn = use_insn_a ? insn_a : insn_b;
    champsim_instruction_info& output = use_insn_a ? info_a : info_b;
    champsim_instruction_info& prior = use_insn_a ? info_b : info_a;
    output.ip = insn.ip ;//& 0x0000FFFFFFFFFFFF; // remove any leading ffff values, as these don't play nicely with scarab's "cmp addr" type. Hopefully a trace doesn't have any aliasing insns due to this!
    if(insn.is_branch)
        output.actually_taken = output.branch_taken = insn.branch_taken;
    else
        output.actually_taken = output.branch_taken = insn.branch_taken = 0;
    output.is_branch = insn.get_branch_type();
    output.scarab_op_type = output.is_branch == NOT_BRANCH ? OP_NOP : OP_CF;
    memcpy(output.destination_registers, insn.destination_registers, sizeof(output.destination_registers));
    memcpy(output.source_registers, insn.source_registers, sizeof(output.source_registers));
    memcpy(output.destination_memory, insn.destination_memory, sizeof(output.destination_memory));
    memcpy(output.source_memory, insn.source_memory, sizeof(output.source_memory));
    static bool have_prior = false;
    if(have_prior) {
        prior.next_ip = output.ip;
    }
    bool had_prior = have_prior;
    have_prior = true;
    return had_prior; // only the prior info is ready at this point
}

bool ChampsimTraceReader_t::refresh_input() {
    assert(0);
    /*
    std::cout << "refreshing input" << std::endl;
    if(feof(file))
        return false;
    strm.next_in = input_buffer;
    strm.avail_in = fread(input_buffer, 1, sizeof(input_buffer), file);
    std::cout << "first byte that we read was: " << +input_buffer[0] << std::endl;
    if(ferror(file))
        throw std::runtime_error("Couldn't read any data!");
    return true;
    */
}
