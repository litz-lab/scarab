#include "globals/global_types.h"

#include "ctype_pin_inst.h"

#ifndef SYNTHETIC_BASIC_APIS_H
#define SYNTHETIC_BASIC_APIS_H
/*  Basic one Line APIs */
ctype_pin_inst generate_generic_load(uns64 ip, uns64 uid, uns64 vaddr, uns8 inst_size, uns8 ld_addr_reg, uns8 dest_reg);
ctype_pin_inst generate_alu_type_inst(uns64 ip, uns64 uid, uns8 inst_size, uns8 regDest, uns8 regSrc1, uns8 regSrc2);
ctype_pin_inst generate_conditional_branch(uns64 ip, uns64 uid, uns64 tgtAddr, bool direction, uns8 inst_size);
ctype_pin_inst generate_unconditional_branch(uns64 ip, uns64 uid, uns64 tgt, uns8 inst_size);
ctype_pin_inst generate_indirect_branch(uns64 ip, uns64 uid, uns64 tgtAddr, uns64 vaddr, uns8 inst_size);
ctype_pin_inst generate_nop(uns64 ip, uns64 uid, uns64 inst_size, bool fake);
#endif