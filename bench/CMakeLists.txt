include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(corecache_bench_single bench_single_threaded.cpp)
target_link_libraries(corecache_bench_single PRIVATE corecache benchmark::benchmark)

add_executable(corecache_bench_multi bench_multi_threaded.cpp)
target_link_libraries(corecache_bench_multi PRIVATE corecache benchmark::benchmark)
