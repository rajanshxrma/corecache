# Strict warnings for first-party targets only. FetchContent'd third-party
# targets (GoogleTest, Google Benchmark) are deliberately left alone.
function(corecache_set_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
    )
endfunction()
