cmake_minimum_required(VERSION 3.21)

project(corecache
    VERSION 0.1.0
    DESCRIPTION "A header-only, sharded, policy-based concurrent cache library for C++20"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

option(CORECACHE_BUILD_TESTS "Build corecache tests" ON)
option(CORECACHE_BUILD_BENCH "Build corecache benchmarks" OFF)
set(CORECACHE_SANITIZE "" CACHE STRING "Sanitizer to build with: thread|address")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)
corecache_enable_sanitizers()

# ---------------------------------------------------------------------------
# corecache: header-only interface library
# ---------------------------------------------------------------------------
add_library(corecache INTERFACE)
add_library(corecache::corecache ALIAS corecache)

target_include_directories(corecache INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(corecache INTERFACE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(corecache INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# Smoke-instantiation target: compiles the templates into one real TU so
# warnings/errors in the header bodies surface even though the lib itself
# is INTERFACE-only.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/instantiate_smoke.cpp)
    add_library(corecache_instantiate_smoke OBJECT src/instantiate_smoke.cpp)
    target_link_libraries(corecache_instantiate_smoke PRIVATE corecache)
    corecache_set_warnings(corecache_instantiate_smoke)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/quickstart.cpp)
    add_executable(corecache_quickstart examples/quickstart.cpp)
    target_link_libraries(corecache_quickstart PRIVATE corecache)
    corecache_set_warnings(corecache_quickstart)
endif()

if(CORECACHE_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
    enable_testing()
    add_subdirectory(test)
endif()

if(CORECACHE_BUILD_BENCH AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt)
    add_subdirectory(bench)
endif()
