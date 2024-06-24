/* Copyright 2024 University of California Santa Cruz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : map_dep.h
 * Author       : Y. Zhao, Litz Lab
 * Date         : 6/2024
 * Description  : Dependency Graph for Instruction Chain Tracking
 ***************************************************************************************/

#ifndef __MAP_DEP_H__
#define __MAP_DEP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "globals/global_types.h"
#include "op.h"

void map_dep_init(uns);               // create the graph instance
void map_dep_process(Op*);            // process each instruction as a graph node to track dependency
void map_dep_read(Op*);               // build/update dependency edges when a destination consumes a source
void map_dep_write(Op*);              // record the producer information for future consuming
void map_dep_release(uns);            // clear the dep info in the meta register table
void map_dep_produce(Op*);            // update the produce cycle when the value is produced
void map_dep_consume(Op*);            // update the consume cycle when the value is consumed
void map_dep_done(void);              // write the graph for doing ananysis after building

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __MAP_DEP_H__ */
