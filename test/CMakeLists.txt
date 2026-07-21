include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)

set(CORECACHE_STRESS_OPS "200000" CACHE STRING
    "Ops per thread in the concurrency stress test (lower for TSan runs)")

function(corecache_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE corecache GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        CORECACHE_STRESS_OPS=${CORECACHE_STRESS_OPS})
    corecache_set_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_lru_policy.cpp)
    corecache_add_test(test_lru_policy)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_arc_policy.cpp)
    corecache_add_test(test_arc_policy)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_cache_api.cpp)
    corecache_add_test(test_cache_api)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shard_distribution.cpp)
    corecache_add_test(test_shard_distribution)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_concurrency_stress.cpp)
    corecache_add_test(test_concurrency_stress)
endif()
