#ifndef __FT_OP_BUFFER_H__
#define __FT_OP_BUFFER_H__

#include "globals/global_types.h"

#include "op.h"

typedef struct FT FT;
typedef struct Icache_Stage_struct Icache_Stage;

#ifdef __cplusplus
extern "C" {
#endif

void ft_op_buffer_init(Icache_Stage* ic);
void ft_op_buffer_reset(Icache_Stage* ic);
void ft_op_buffer_fill_from_ft(Icache_Stage* ic, FT* ft);
Flag ft_op_buffer_can_fetch_op(Icache_Stage* ic);
uns ft_op_buffer_count(Icache_Stage* ic);
Op* ft_op_buffer_peek(Icache_Stage* ic);
Op* ft_op_buffer_pop(Icache_Stage* ic);
void recover_ft_op_buffer(Icache_Stage* ic);

#ifdef __cplusplus
}
#endif

#endif /* __FT_OP_BUFFER_H__ */
