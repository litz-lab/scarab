#include "ft_op_buffer.h"

#include <deque>

#include "globals/assert.h"

#include "ft.h"
#include "icache_stage.h"

typedef struct FT_Op_Buffer_Cpp_struct {
  std::deque<Op*> ops;
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
}

void ft_op_buffer_fill_from_ft(Icache_Stage* ic, FT* ft) {
  ASSERT(0, ic && ic->ft_op_buffer);
  ASSERT(ic->proc_id, ft);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  ASSERT(ic->proc_id, buf->ops.empty());
  ASSERT(ic->proc_id, !ft_op_buffer_can_fetch_op(ic));

  while (ft_can_fetch_op(ft)) {
    buf->ops.emplace_back(ft_fetch_op(ft));
  }
}

Flag ft_op_buffer_can_fetch_op(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  return !buf->ops.empty();
}

uns ft_op_buffer_count(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  return (uns)buf->ops.size();
}

Op* ft_op_buffer_peek(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  if (buf->ops.empty()) {
    return NULL;
  }
  return buf->ops.front();
}

Op* ft_op_buffer_pop(Icache_Stage* ic) {
  ASSERT(0, ic && ic->ft_op_buffer);
  FT_Op_Buffer_Cpp* buf = (FT_Op_Buffer_Cpp*)ic->ft_op_buffer;
  ASSERT(ic->proc_id, !buf->ops.empty());
  Op* op = buf->ops.front();
  buf->ops.pop_front();
  return op;
}
