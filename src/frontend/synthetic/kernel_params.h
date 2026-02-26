/* Kernel Enum */
typedef enum Kernel_Id_Enum {
#define KERNEL_IMPL(id, name) id,
#include "frontend/synthetic/kernel_table.def"
#undef KERNEL_IMPL
  INVALID
} Kernel_Enum;

extern Kernel_Enum kernel;
extern const char* kernel_names[];