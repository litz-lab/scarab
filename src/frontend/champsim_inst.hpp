#ifndef INSTRUCTION_H
#define INSTRUCTION_H

/*
 * A significant portion of this header comes from the ChampSim simulator.
 * The code was released as free software under the GNU General Public License as published by the Free Software Foundation version 2.
 */
#include <cstdint>
#include <iosfwd>


// TODO: convert these to constexpr?
// instruction format
#define ROB_SIZE 352
#define LQ_SIZE 128
#define SQ_SIZE 72
#define NUM_INSTR_DESTINATIONS_SPARC 4
#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES 4

// special registers that help us identify branches
#define CHAMPSIM_REG_STACK_POINTER 6
#define CHAMPSIM_REG_FLAGS 25
#define CHAMPSIM_REG_INSTRUCTION_POINTER 26

// branch types
#define NOT_BRANCH           0
#define BRANCH_DIRECT_JUMP   1
#define BRANCH_INDIRECT      2
#define BRANCH_CONDITIONAL   3
#define BRANCH_DIRECT_CALL   4
#define BRANCH_INDIRECT_CALL 5
#define BRANCH_RETURN        6
#define BRANCH_OTHER         7


// this is the instruction stored in ChampSim traces
class input_instr {
  public:

    // instruction pointer or PC (Program Counter)
    uint64_t ip;

    // branch info
    uint8_t is_branch;
    uint8_t branch_taken;

    uint8_t destination_registers[NUM_INSTR_DESTINATIONS]; // output registers
    uint8_t source_registers[NUM_INSTR_SOURCES]; // input registers

    uint64_t destination_memory[NUM_INSTR_DESTINATIONS]; // output memory
    uint64_t source_memory[NUM_INSTR_SOURCES]; // input memory

    input_instr() {
        ip = 0;
        is_branch = 0;
        branch_taken = 0;

        for (uint32_t i=0; i<NUM_INSTR_SOURCES; i++) {
            source_registers[i] = 0;
            source_memory[i] = 0;
        }

        for (uint32_t i=0; i<NUM_INSTR_DESTINATIONS; i++) {
            destination_registers[i] = 0;
            destination_memory[i] = 0;
        }
    }
    int get_branch_type() const;
};

// this is the instruction emitted by the champsim trace reader to the champsim frontend
// it contains extra metadata to make the frontend's task of converting to ctype_pin_inst
// easier
struct champsim_instruction_info {
    // instruction pointer or PC (Program Counter)
    uint64_t ip;

    // branch info
    uint8_t is_branch; // value is one of the BRANCH* values above
    uint8_t branch_taken;

    uint8_t destination_registers[NUM_INSTR_DESTINATIONS]; // output registers
    uint8_t source_registers[NUM_INSTR_SOURCES]; // input registers

    uint64_t destination_memory[NUM_INSTR_DESTINATIONS]; // output memory
    uint64_t source_memory[NUM_INSTR_SOURCES]; // input memory

    uint64_t next_ip;
    bool actually_taken;
    int scarab_op_type;
};
std::ostream& operator<<(std::ostream&, const champsim_instruction_info&);
#endif
