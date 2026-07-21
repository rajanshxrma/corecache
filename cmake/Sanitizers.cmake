# Plumbing for CORECACHE_SANITIZE=thread|address. Applies -fsanitize flags to
# every target created after this is invoked, via a global compile/link option.
function(corecache_enable_sanitizers)
    if(NOT CORECACHE_SANITIZE)
        return()
    endif()

    if(CORECACHE_SANITIZE STREQUAL "thread")
        add_compile_options(-fsanitize=thread -g -O1)
        add_link_options(-fsanitize=thread)
    elseif(CORECACHE_SANITIZE STREQUAL "address")
        add_compile_options(-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "unknown CORECACHE_SANITIZE value: ${CORECACHE_SANITIZE} (expected thread|address)")
    endif()
endfunction()
