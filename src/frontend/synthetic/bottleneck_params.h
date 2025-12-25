/* Bottleneck Enum */
typedef enum BottleNeck_Id_enum {
#define BOTTLENECK_IMPL(id, name) id,
#include "frontend/synthetic/bottlenecks_table.def"
#undef BOTTLENECK_IMPL
  INVALID
} BottleNeck_enum;

extern BottleNeck_enum bottleneck;
extern const char* bottleneckNames[];