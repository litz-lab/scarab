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

// Block-BTB Split branch slot
typedef struct Blk_Btb_Split_BrSlot_struct {
  Addr addr;       // equivalent to offset, log2(BTB_BLOCK_SIZE) bits
  Addr target;     // sizeof(Addr) bits
  Cf_Type type;    // log2(NUM_CF_TYPES) bits
  uns8 inst_size;  // 8 bits; the fall-through addr it implies indexes the next chained entry
  Flag valid;      // 1 bit
} Blk_Btb_Split_BrSlot;

/* Block-BTB Split entry
 * This struct facilitates clearner operation on cache entries rather than using the offset.
 * Note the actual size of an entry may differ from the struct size, depending on BTB_NUM_BRSLOT. */
typedef struct Blk_Btb_Split_Entry_struct {
  Flag split;                      // 1 bit; TRUE if this block continues in a chained entry
  Blk_Btb_Split_BrSlot brslots[];  // flexible array member (C99)
} Blk_Btb_Split_Entry;

#define BLK_BTB_SPLIT_ENTRY_SIZE (sizeof(Blk_Btb_Split_Entry) + BTB_NUM_BRSLOT * sizeof(Blk_Btb_Split_BrSlot))

#endif /* #ifndef __BTB_H__ */
