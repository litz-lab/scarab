file(GLOB srcs CONFIGURE_DEPENDS *.cpp *.h)
add_library(ramulator1 STATIC ${srcs})
target_compile_definitions(ramulator1 PRIVATE RAMULATOR)
target_compile_options(ramulator1 PRIVATE ${ramulator_warnings})
