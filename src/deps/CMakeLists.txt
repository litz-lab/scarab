find_package(Git)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
                    RESULT_VARIABLE GIT_SUBMOD_RESULT)
    if(NOT GIT_SUBMOD_RESULT EQUAL "0")
        message(FATAL_ERROR "Updating git submodules failed: ${GIT_SUBMOD_RESULT}")
    endif()
else()
  message(FATAL_ERROR "Could not find git binary in the system")
endif()

find_package(Python3)
if(Python3_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/xed/mfile.py
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/xed"
        COMMAND_ECHO STDOUT
    )
else()
  message(FATAL_ERROR "Could not find python3 in the system")
endif()

set($ENV{TARGET} ia32)
add_library(xed INTERFACE)
target_include_directories(xed INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/xed/include/public/xed/)
target_include_directories(xed INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/xed/obj/)
target_link_libraries(xed INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/xed/obj/libxed.a)

if(DEFINED ENV{SCARAB_ENABLE_PT_MEMTRACE})
  add_subdirectory(dynamorio)
  add_library(pt_memtrace INTERFACE)

  target_include_directories(pt_memtrace
    INTERFACE
    ${CMAKE_BINARY_DIR}/deps/dynamorio/include/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/ext/drcovlib/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/ext/drmgr/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/ext/drcontainers/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/core/lib/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/clients/drcachesim/reader/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/clients/drcachesim/common/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/clients/drcachesim/tracer/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/clients/drcachesim/scheduler/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/clients/drcachesim/
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamorio/third_party/zlib/contrib/
    )

  target_link_libraries(pt_memtrace
    INTERFACE
    drmemtrace_raw2trace
    directory_iterator
    drfrontendlib
    drutil_static
    drmgr_static
    drmemfuncs

    drlibc
    drcovlib_static
    drx_static
    drreg_static
    drcontainers
    drmemtrace_analyzer
    minizip
    xed
    snappy
    dl
    config++
    z
    rt
    pthread
    )
endif()

# Ramulator 2 (src/deps/ramulator2) and the two libraries it needs, yaml-cpp and fmt,
# all pinned as submodules. Ramulator 2 is built from its sources here rather than with
# its own CMakeLists.txt, which makes a shared library (written into its source tree),
# fetches yaml-cpp/fmt over the network at configure time, and defines a target named
# "ramulator". An OBJECT library keeps every object in the link: implementations
# register themselves from static initializers, which a static archive would drop.
set(YAML_CPP_BUILD_CONTRIB OFF)
set(YAML_CPP_BUILD_TOOLS OFF)
set(YAML_CPP_BUILD_TESTS OFF)
set(YAML_CPP_INSTALL OFF)
set(YAML_BUILD_SHARED_LIBS OFF)
add_subdirectory(yaml-cpp EXCLUDE_FROM_ALL)

set(FMT_INSTALL OFF)
set(FMT_TEST OFF)
set(FMT_DOC OFF)
add_subdirectory(fmt EXCLUDE_FROM_ALL)

file(GLOB_RECURSE ramulator2_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ramulator2/src/ramulator/*.cpp)
# The Python bindings need nanobind and are not part of the C++ library.
list(FILTER ramulator2_srcs EXCLUDE REGEX "/src/ramulator/python/")
add_library(ramulator2 OBJECT ${ramulator2_srcs})
set_target_properties(ramulator2 PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(ramulator2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ramulator2/src)
target_link_libraries(ramulator2 PUBLIC yaml-cpp::yaml-cpp fmt::fmt)
