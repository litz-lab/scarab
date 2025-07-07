/***************************************************************************************
 * File         : uop_cache.h
 * Author       : Peter Braun
 * Date         : 10.28.2020
 * Description  : Interface for interacting with uop cache object.
 *                  Following Kotra et. al.'s MICRO 2020 description of uop cache baseline
 *                  Instr comes from icache. Theoretically
 *                  we have higher BW fetching direct with uop cache
 ***************************************************************************************/

#ifndef __UOP_CACHE_H__
#define __UOP_CACHE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "decoupled_frontend.h"
#include "op.h"
#include "stage_data.h"

// Uop Cache Data
typedef struct Uop_Cache_Data_struct {
  Addr line_start;
  // number of uops in this line
  uns n_uops;
  // the offset for calculating the next line
  Addr offset;
  FT_Info_Dynamic ft_info_dynamic;
  // is this line the end of the FT?
  Flag end_of_ft;

  Counter used;
  Flag priority;
} Uop_Cache_Data;

typedef struct Uop_Cache_Stage_struct {
  uns8 proc_id;
  Stage_Data sd;
  uns8 lookups_per_cycle_count;
  FT* current_ft;
} Uop_Cache_Stage;

/**************************************************************************************/
/* External Variables */

extern Uop_Cache_Stage* uc;

/**************************************************************************************/
/* Prototypes */

void set_uop_cache_stage(Uop_Cache_Stage* new_uc);
void init_uop_cache_stage(uns8 proc_id, const char* name);
void alloc_mem_uop_cache(uns num_cores);
void recover_uop_cache(void);

Flag uop_cache_lookup_ft_and_fill_lookup_buffer(FT_Info ft_info, Flag offpath);
Uop_Cache_Data uop_cache_consume_uops_from_lookup_buffer(uns requested);
void uop_cache_clear_lookup_buffer(void);
Uop_Cache_Data* uop_cache_lookup_line(Addr line_start, FT_Info ft_info, Flag update_repl);

void uop_cache_accumulation_buffer_clear();
void uop_cache_accumulation_buffer_update(Op* op);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __UOP_CACHE_H__ */
