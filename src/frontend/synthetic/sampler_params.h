#ifndef SAMPLER_PARAMS_H
#define SAMPLER_PARAMS_H

typedef enum Sequence_Pick_Strategy_Enum {
  UNIFORM_SEQUENTIAL,  // pick sequentially
  UNIFORM_RANDOM,      // pick sequentially but sequence vector must be shuffled
  NORMAL_RANDOM,       // use the Bell Curve Normal Distribution formula
  DICRETE_RANDOM,      // every sample/element has a discrete probability
  USER_DEFINED
} Sequence_Pick_Strategy;

/* Enum for Kernels */
typedef enum Load_Kernel_Type_Enum {
  DEPENDENCE_CHAIN,
  NO_DEPENDENCE_CHAIN
} Load_Kernel_Type;

typedef enum Limit_Load_To_Enum {
  MLC_LEVEL,
  LLC_LEVEL,
  MEM_LEVEL,
  DCACHE_LEVEL
} Limit_Load_To;

#endif