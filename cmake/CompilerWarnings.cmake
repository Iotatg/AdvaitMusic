function(set_project_warnings project_name)
    option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

    set(CLANG_GCC_WARNINGS
        -Wall
        -Wextra
        -Wshadow
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wpedantic
        -Wnull-dereference
        -Wformat=2
    )

    if(WARNINGS_AS_ERRORS)
        list(APPEND CLANG_GCC_WARNINGS -Werror)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${project_name} PRIVATE ${CLANG_GCC_WARNINGS})
    endif()
endfunction()
