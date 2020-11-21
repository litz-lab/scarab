/***************************************************************************************
 * File         : uop_cache.h
 * Author       : Peter Braun
 * Date         : 10.28.2020
 * Description  : Interface for interacting with uop cache object.
 *                  Currently caching individual instr. More realistically, should be
 *                  caching full bbl keyed by first instr in bbl
 *                  . instr comes from icache which we speculatively execute. Theoretically
 *                  we have higher BW with uop cache
 ***************************************************************************************/

#ifndef __UOP_CACHE_H__
#define __UOP_CACHE_H__

/**************************************************************************************/
/* Prototypes */

// #ifdef __cplusplus 
// extern "C" {
// #endif

// use singleton, not OO (only one instance of uop cache)
// Add cache struct overriding library including:
// current_pw

void init_uop_cache(uns8 proc_id);

/* return whether the instr pc is cached (this does not consider that the whole PW 
    could already have been fetched, potentially introducing 1 incorrect cycle of latency)*/
int in_uop_cache(Addr pc); 

/* accumulate uop into buffer. If terminating condition reached, call insert_uop_cache */
void accumulate_op(Op* op);

// #ifdef __cplusplus
// }
// #endif

/**************************************************************************************/

#endif /* #ifndef __UOP_CACHE_H__ */
