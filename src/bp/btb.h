#ifndef __BTB_H__
#define __BTB_H__

#include "globals/global_types.h"

// Block-BTB branch slot
typedef struct Blk_Btb_BrSlot_struct {
  Addr addr;     // equivalent to offset, log2(BTB_BLOCK_SIZE) bits
  Addr target;   // sizeof(Addr) bits
  Cf_Type type;  // log2(NUM_CF_TYPES) bits
  Flag valid;    // 1 bit
} Blk_Btb_BrSlot;

#define BLK_BTB_ENTRY_SIZE BTB_NUM_BRSLOT * sizeof(Blk_Btb_BrSlot)

#endif /* #ifndef __BTB_H__ */
