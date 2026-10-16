# ============================================================================
# redis module for the dummyscheme VM -- distribution CMake build.
#
# Builds only scheme_module.so (the module + the embedded scheme core).  No
# redis source is needed: the module API header is vendored in include/.
#
#   cmake -S . -B build
#   cmake --build build
#
# The scheme core is the host dummyscheme project (vm.cpp & friends), one
# directory above this one by default; override with -DSCHEME_SRC_DIR=...
#
# Local development (redis build, Xcode debug with redis sources) lives in
# the Makefile, which drives dev/CMakeLists.txt -- see README.
# ============================================================================
cmake_minimum_required(VERSION 3.16)

project(scheme_redis C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Language standards.  The host dummyscheme project targets C++98 for
# multi-platform portability, but this redis module is compiled to a .so for
# one platform and does not need that constraint, so we use modern standards:
#   - C  : C11 -- matches redis itself (redis's Makefile uses -std=gnu11)
#           and every mainstream redis module (RediSearch, RedisJSON, ...);
#           the module API (redismodule.h) is a C API, no reason to go older.
#   - C++: C++20 -- the host sources (10 vm.cpp-family files) compile cleanly
#           under both C++17 and C++20 (verified with AppleClang 21); if a
#           host update ever breaks C++20, drop back to 17 by changing the
#           line below.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Locations.  SCHEME_SRC_DIR defaults to the host project one level up (the
# typical dummy/ + redis-module/ checkout layout) but is a normal CMake cache
# variable, so it is not hard-coded: override with
#   -DSCHEME_SRC_DIR=/path/to/dummy
# ---------------------------------------------------------------------------
set(SCHEME_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.."
    CACHE PATH "dummyscheme host project root (vm.cpp & friends)")
set(MODULE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Scheme interpreter (host project sources, compiled as C++20 -- the host's
# C++98 portability constraint does not apply to this platform-bound .so; see
# the standard notes at the top)
# ---------------------------------------------------------------------------
set(SCHEME_CORE_SRCS
    vm.cpp scmapi.cpp scmbasic.cpp scmmath.cpp scmstring.cpp
    scmcompiler.cpp scmport.cpp scmvector.cpp scmtable.cpp scminit.cpp)
foreach(f IN LISTS SCHEME_CORE_SRCS)
    list(APPEND SCHEME_CORE_ABS "${SCHEME_SRC_DIR}/${f}")
endforeach()

add_library(scheme_core STATIC ${SCHEME_CORE_ABS})
target_include_directories(scheme_core PUBLIC "${SCHEME_SRC_DIR}")

# ---------------------------------------------------------------------------
# C bridge over the VM (extern "C" API in redis_scheme_runtime.h)
# ---------------------------------------------------------------------------
add_library(redis_scheme_runtime STATIC
    "${MODULE_SRC_DIR}/redis_scheme_runtime.cpp")
target_include_directories(redis_scheme_runtime PUBLIC
    "${MODULE_SRC_DIR}")
target_link_libraries(redis_scheme_runtime PUBLIC scheme_core)

# ---------------------------------------------------------------------------
# The redis module itself (pure C, dlopen'ed by redis-server)
# ---------------------------------------------------------------------------
add_library(redis_scheme_module MODULE
    "${MODULE_SRC_DIR}/redis_scheme_module.c")
target_include_directories(redis_scheme_module PUBLIC
    "${MODULE_SRC_DIR}")
target_link_libraries(redis_scheme_module PRIVATE redis_scheme_runtime)

# the module resolves RM_* at load time from the redis-server process; on
# macOS undefined symbols must be explicitly allowed at link time
set_target_properties(redis_scheme_module PROPERTIES
    PREFIX ""
    OUTPUT_NAME "scheme_module")
if(APPLE)
    target_link_options(redis_scheme_module PRIVATE
        "-Wl,-undefined,dynamic_lookup")
endif()
# note: the module pulls in the scheme core (C++); CMake adds the C++ standard
# library automatically for targets that link C++ objects
