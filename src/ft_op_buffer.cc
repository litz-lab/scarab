#include "ft_op_buffer.h"

#include <vector>

#include "globals/assert.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"

#include "bp/bp.h"

#include "ft.h"
#include "icache_stage.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_ICACHE_STAGE, ##args)

typedef struct FT_Op_Buffer_Cpp_struct {
  std::vector<Op*> ops;
  size_t head = 0;
} FT_Op_Buffer_Cpp;

void ft_op_buffer_init(Icache_Stage* ic) {
  ASSERT(0, ic);
  if (ic->ft_op_buffer)
    return;
  ic->ft_op_buffer = new FT_Op_Buffer_Cpp();
}

void ft_op_buffer_reset(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  buf->ops.clear();
  buf->head = 0;
}

void ft_op_buffer_fill_from_ft(Icache_Stage* ic, FT* ft) {
  ASSERT(0, ic && ic->ft_op_buffer);
  ASSERT(ic->proc_id, ft);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  ASSERT(ic->proc_id, buf->head == buf->ops.size());
  ASSERT(ic->proc_id, !ft_op_buffer_can_fetch_op(ic));

  buf->ops.clear();
  buf->head = 0;
  while (ft_can_fetch_op(ft)) {
    buf->ops.emplace_back(ft_fetch_op(ft));
  }
}

Flag ft_op_buffer_can_fetch_op(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  return buf->head < buf->ops.size();
}

uns ft_op_buffer_count(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  return (uns)(buf->ops.size() - buf->head);
}

Op* ft_op_buffer_peek(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  if (buf->head >= buf->ops.size()) {
    return NULL;
  }
  return buf->ops[buf->head];
}

Op* ft_op_buffer_pop(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  ASSERT(ic->proc_id, buf->head < buf->ops.size());
  return buf->ops[buf->head++];
}

void recover_ft_op_buffer(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  Flag flushed = FALSE;
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  std::vector<Op*> survivors;
  survivors.reserve(ft_op_buffer_count(ic));
  for (size_t ii = buf->head; ii < buf->ops.size(); ii++) {
    Op* op = buf->ops[ii];
    ASSERT(ic->proc_id, op);
    if (IS_FLUSHING_OP(op)) {
      op_select_bp_pred_info(op, BP_PRED_MAIN);
      DEBUG(ic->proc_id, "Recovery op found in FT buffer idx:%llu op_num:%llu off_path:%u addr:0x%llx\n",
            (unsigned long long)ii, (unsigned long long)op->op_num, op->off_path,
            (unsigned long long)op->inst_info->addr);
    }
    if (FLUSH_OP(op)) {
      DEBUG(ic->proc_id, "Icache buffer flushing op_num:%llu off_path:%u\n", (unsigned long long)op->op_num,
            op->off_path);
      flushed = TRUE;
      ASSERT(ic->proc_id, op->off_path);
      ASSERT(ic->proc_id, op->parent_FT);
      ft_free_op(op);
    } else {
      survivors.emplace_back(op);
    }
  }
  buf->ops.swap(survivors);
  buf->head = 0;

  if (buf->ops.size() && flushed) {
    Op* op = buf->ops[buf->ops.size() - 1];
    assert_ft_after_recovery(ic->proc_id, op, bp_recovery_info->recovery_fetch_addr);
  }
}
