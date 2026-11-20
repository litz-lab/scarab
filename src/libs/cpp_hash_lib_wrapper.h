#ifndef __HASH_MAP_WRAPPER__
#define __HASH_MAP_WRAPPER__

#ifdef __cplusplus
extern "C" {
#endif

#include "static_inst_info.h"

// class cpp_hash_lib_wrapper {

// Per-uop static info, interned by {addr, binary, op_idx, mem_shape, cf_type}.
Static_Op_Info* cpp_static_op_access_create(int core, uint64_t addr, uint64_t lsb_bytes, uint64_t msb_bytes,
                                            uint8_t op_idx, uint16_t mem_shape, uint8_t cf_type,
                                            unsigned char* new_entry);

#ifdef __cplusplus
}
#endif

#endif
