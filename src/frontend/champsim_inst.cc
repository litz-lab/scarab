#include "frontend/champsim_inst.hpp"
#include <cassert>
#include <iostream>
using std::size_t;

int input_instr::get_branch_type() const {
    bool reads_sp, writes_sp, reads_flags, reads_ip, writes_ip, reads_other_reg;
    reads_sp = writes_sp = reads_flags = reads_ip = writes_ip = reads_other_reg = false;
    for(size_t i = 0; i < NUM_INSTR_DESTINATIONS; ++i) {
        switch(destination_registers[i]) {
            case CHAMPSIM_REG_FLAGS:
                assert(0);
                break;
            case CHAMPSIM_REG_STACK_POINTER:
                writes_sp = true;
                break;
            case CHAMPSIM_REG_INSTRUCTION_POINTER:
                writes_ip = true;
        }
    }
    for(size_t i = 0; i < NUM_INSTR_SOURCES; ++i) {
        switch(source_registers[i]) {
            case CHAMPSIM_REG_FLAGS:
                reads_flags = true;
                break;
            case CHAMPSIM_REG_STACK_POINTER:
                reads_sp = true;
                break;
            case CHAMPSIM_REG_INSTRUCTION_POINTER:
                reads_ip = true;
                break;
            default:
                reads_other_reg = true;
                break;
        }
    }
    if(!reads_sp && !reads_flags && writes_ip && !reads_other_reg) {
        return BRANCH_DIRECT_JUMP;
    }
    if(!reads_sp && !reads_flags && writes_ip && reads_other_reg) {
        return BRANCH_INDIRECT;
    }
    if(!reads_sp && reads_ip && !writes_sp && reads_flags && !reads_other_reg) {
        return BRANCH_CONDITIONAL;
    }
    if(reads_sp && reads_ip && writes_sp && writes_ip && !reads_flags && !reads_other_reg) {
        return BRANCH_DIRECT_CALL;
    }
    if(reads_sp && reads_ip && writes_sp && writes_ip && !reads_flags && reads_other_reg) {
        return BRANCH_INDIRECT_CALL;
    }
    if(reads_sp && !reads_ip && writes_sp && writes_ip) {
        return BRANCH_RETURN;
    }
    if(writes_ip)
        return BRANCH_OTHER;
    return NOT_BRANCH;
}

std::ostream& operator<<(std::ostream& os, const champsim_instruction_info& inst) {
    os << std::hex << inst.ip << + inst.is_branch << '\n';
    // TODO: handle "special registers differently
    os << "output registers: ";
    size_t num_regs = 0;
    for(size_t i = 0; i < NUM_INSTR_DESTINATIONS; ++i) {
        auto reg = inst.destination_registers[i];
        if(reg) {
            ++num_regs;
            switch(reg) {
                case CHAMPSIM_REG_FLAGS:
                    os << "flags ";
                    break;
                case CHAMPSIM_REG_STACK_POINTER:
                    os << "sp ";
                    break;
                case CHAMPSIM_REG_INSTRUCTION_POINTER:
                    os << "ip ";
                    break;
            }
        }
        /* os << +insn.destination_registers[i] << ' '; */
    }
    os << num_regs;
    os << " input registers: ";
    num_regs = 0;
    for(size_t i = 0; i < NUM_INSTR_SOURCES; ++i) {
        auto reg = inst.source_registers[i];
        if(reg) {
            ++num_regs;
            switch(reg) {
                case CHAMPSIM_REG_FLAGS:
                    os << "flags ";
                    break;
                case CHAMPSIM_REG_STACK_POINTER:
                    os << "sp ";
                    break;
                case CHAMPSIM_REG_INSTRUCTION_POINTER:
                    os << "ip ";
                    break;
            }
        }
        /* os << +insn.source_registers[i] << ' '; */
    }
    os << num_regs;
    os << " output memory: ";
    size_t num_mem = 0;
    for(size_t i = 0; i < NUM_INSTR_DESTINATIONS; ++i) {
        if(inst.destination_memory[i])
            ++num_mem;
    }
    os << num_mem;
    os << " input memory: ";
    num_mem = 0;
    for(size_t i = 0; i < NUM_INSTR_SOURCES; ++i) {
        if(inst.source_memory[i])
            ++num_mem;
    }
    os << num_mem;
    return os;
}
