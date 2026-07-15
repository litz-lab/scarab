#include <ctype_pin_inst.h>
#include <map>

template <typename Generator>
class KernelGenerator {
  Generator generator;
  std::map<uns64, ctype_pin_inst> kernel;

 public:
  KernelGenerator(Generator generator) : generator(std::move(generator)) { kernel = generator.generate(); }
};

class ILP_KERNEL {
  // whatever state is needed to generate the kernel

 public:
  std::map<uns64, ctype_pin_inst> generate() {
    // code that generates the kernel
  }
};

class some_other_kernel {
  // whatever state is needed to generate the kernel

 public:
  std::map<uns64, ctype_pin_inst> generate() {
    // code that generates the kernel
  }
};

// how its used
void main() {
  ILP_KERNEL ilp_kernel;
  KernelGenerator<ILP_KERNEL> ilp_generator(ilp_kernel);
}
