#ifndef SAMPLER_PARAMS_H
#define SAMPLER_PARAMS_H

typedef enum Sampling_Strategy_Enum {
#define SAMPLING_STRATEGY_IMPL(id, name) id,
#include "frontend/synthetic/sampling_strategy_table.def"
#undef SAMPLING_STRATEGY_IMPL
} Sampling_Strategy;

/* Enum for Kernels */
typedef enum Load_Kernel_Type_Enum {
  LATENCY,
  BANDWIDTH
} Load_Kernel_Type;

typedef enum Limit_Load_To_Enum {
#define MEMORY_HIERACHY_LEVEL_IMPL(id, name) id,
#include "frontend/synthetic/mem_hierachy_level_table.def"
#undef MEMORY_HIERACHY_LEVEL_IMPL
} Limit_Load_To;

extern const char* mem_hierachy_level_names[];
extern const char* sampling_strategy_names[];

#endif