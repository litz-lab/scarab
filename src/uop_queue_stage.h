// The uop queue is populated by uops from either the icache or the uop cache.

#ifndef __UOP_QUEUE_STAGE_H_
#define __UOP_QUEUE_STAGE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stage_data.h"


/**************************************************************************************/
/* External Variables */

typedef struct UOp_Queue_Stage_struct {
  uns proc_id;
  Stage_Data* last_sd;
} UOp_Queue_Stage;

extern UOp_Queue_Stage *uop_queue_stage;


/**************************************************************************************/
/* External Methods */

void init_uop_queue_stage(void);
void update_uop_queue_stage(Stage_Data* src_sd);
void recover_uop_queue_stage(void);

Stage_Data* uop_queue_stage_get_latest_sd(void);
int get_uop_queue_stage_length(void);

#ifdef __cplusplus
}
#endif

#endif