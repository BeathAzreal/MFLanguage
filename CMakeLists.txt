cmake_minimum_required(VERSION 3.16)

project(MFL
    VERSION 0.0.1
    DESCRIPTION "MFL - Materials & Manufacturing Language"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(MFL_BUILD_TESTS "Build MFL tests" ON)
option(MFL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Core library. Sources from src/lexer, src/parser, src/compiler, src/vm and
# src/runtime will be added here as they are implemented.
# ---------------------------------------------------------------------------
add_library(mfl_core
    src/version.c
)
target_include_directories(mfl_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

# Warning flags shared by every MFL target. MFL_WARNINGS_AS_ERRORS adds
# /WX (MSVC) or -Werror (GCC, Clang and other GCC-compatible compilers).
function(mfl_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(MFL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(MFL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

mfl_set_warnings(mfl_core)

if(NOT MSVC)
    # libm will be required by the numerical runtime.
    find_library(MFL_LIBM m)
    if(MFL_LIBM)
        target_link_libraries(mfl_core PUBLIC ${MFL_LIBM})
    endif()
endif()

# ---------------------------------------------------------------------------
# Command-line driver
# ---------------------------------------------------------------------------
add_executable(mfl src/main.c)
target_link_libraries(mfl PRIVATE mfl_core)
mfl_set_warnings(mfl)

# ---------------------------------------------------------------------------
# Tests (smoke tests only; language conformance tests are registered as
# soon as the interpreter can execute scripts).
# ---------------------------------------------------------------------------
if(MFL_BUILD_TESTS)
    enable_testing()
    # Must match MFL_VERSION_STRING in include/mfl/mfl.h.
    set(MFL_VERSION_STRING "${PROJECT_VERSION}-dev")
    # Escape regex metacharacters so the version's dots are literal.
    string(REGEX REPLACE "([][.^$*+?()|\\\\])" "\\\\\\1"
           MFL_VERSION_REGEX "${MFL_VERSION_STRING}")
    # Anchored: the whole output must be "mfl <version>" plus one line
    # ending (LF, or CRLF on Windows). This rejects e.g. "mfl 0.0.10-dev".
    add_test(NAME cli_version COMMAND mfl --version)
    set_tests_properties(cli_version PROPERTIES
        PASS_REGULAR_EXPRESSION "^mfl ${MFL_VERSION_REGEX}\r?\n$")
    add_test(NAME cli_help COMMAND mfl --help)
endif()
