/***************************************************************************************
 * File         : uop_cache.h
 * Author       : Peter Braun
 * Date         : 10.28.2020
 * Description  : Interface for interacting with uop cache object.
 *                  Currently caching individual instr. More realistically, should be
 *                  caching full bbl keyed by first instr in bbl
 ***************************************************************************************/

#ifndef __UOP_CACHE_H__
#define __UOP_CACHE_H__

/**************************************************************************************/
/* Prototypes */

#ifdef __cplusplus 
extern "C" {
#endif

    /* Add an instr:uop entry*/
    void insert_uop_cache(Addr pc);
    /* return whether the instr pc is cached */
    int in_uop_cache(Addr pc);

#ifdef __cplusplus
}
#endif

/**************************************************************************************/

#endif /* #ifndef __UOP_CACHE_H__ */
