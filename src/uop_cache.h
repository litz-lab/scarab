/***************************************************************************************
 * File         : uop_cache.h
 * Author       : Peter Braun
 * Date         : 10.28.2020
 * Description  : Interface for interacting with uop cache object
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
    /* Retrieve uops for given PC. Maximum length  */
    Op** get_ops_uop_cache();

#ifdef __cplusplus
}
#endif

/**************************************************************************************/

#endif /* #ifndef __UOP_CACHE_H__ */
