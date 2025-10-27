/* Copyright 2020 HPS/SAFARI Research Groups
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
 * File         : node_stage.h
 * Author       : HPS Research Group
 * Date         : 1/28/1999
 * Description  :
 ***************************************************************************************/

#ifndef __NODE_STAGE_H__
#define __NODE_STAGE_H__

#include "exec_stage.h"
#include "stage_data.h"

/**************************************************************************************/
// Types

typedef struct Reservation_Station_struct {
  uns proc_id;
  char name[EXEC_PORTS_MAX_NAME_LEN];  // unique name of the RS, from exec_ports.def
  uns32 size;                          // 0 is infinite
  Func_Unit** connected_fus;           // FUs that this reservation station is connected to.
  uns32 num_fus;                       // number of fus that this rs is connected to.
  uns32 rs_op_count;                   // number of ops in this reservation station
} Reservation_Station;

typedef struct Node_Stage_struct {
  uns proc_id;
  Stage_Data sd;  // stage interface data

  Op* node_head;       // linked-list of ops in the node stage
  Op* node_tail;       // linked-list of ops in the node stage
  Op* node_precommit;  // the pre-commit pointer in the ROB
  int32 node_count;    // number of ops in the node table

  Flag prev_op_fusable;  // if the next dispatched op is macro-fusable

  /* linked-list of ops that are ready to schedule. Ops are put in here when they are issued,
   * or after they are issued and another op wakes them up. */
  Op* rdy_head;

  Counter ret_op;                // next op number to retire
  Counter last_scheduled_opnum;  // op num of the last scheduled op

  Op* next_op_into_rs;      // oldest issued op not yet in the scheduling window (RS)
  Reservation_Station* rs;  // information about all of the reservation stations

  int32* fu_to_rs_map;  // mapping from FU ID to RS ID (-1 for unconnected FUs)

  Flag mem_blocked;      // are we out of mem req buffers for this core
  uns mem_block_length;  // length of the current memory block
  uns ret_stall_length;  // length of the current retirement stall
} Node_Stage;

/**************************************************************************************/
// External Variables

extern Node_Stage* node;

/**************************************************************************************/
// Prototypes

void set_node_stage(Node_Stage*);
void init_node_stage(uns8, const char*);
void reset_node_stage(void);
void reset_all_ops_node_stage(void);
void recover_node_stage(void);
void debug_node_stage(void);
void update_node_stage(Stage_Data*);
Flag is_node_stage_stalled(void);

/**************************************************************************************/

#endif /* #ifndef __NODE_STAGE_H__ */
