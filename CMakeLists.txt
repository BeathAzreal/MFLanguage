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
option(MFL_BUILD_EXAMPLES "Build the C embedding example" ON)

# ---------------------------------------------------------------------------
# Core library. Sources from src/lexer, src/parser, src/compiler, src/vm and
# src/runtime will be added here as they are implemented.
# ---------------------------------------------------------------------------
add_library(mfl_core
    src/version.c
    src/lexer/lexer.c
    src/parser/ast.c
    src/parser/parser.c
    src/runtime/arena.c
    src/runtime/units.c
    src/runtime/number.c
    src/compiler/checker.c
    src/compiler/chunk.c
    src/compiler/compiler.c
    src/vm/vm.c
    src/api/mfl_api.c
)
# include/ holds the public C API; src/ holds internal headers
# (e.g. lexer/lexer.h) used by the core, the CLI and the unit tests.
target_include_directories(mfl_core
    PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

# Warning flags shared by every MFL target. MFL_WARNINGS_AS_ERRORS adds
# /WX (MSVC) or -Werror (GCC, Clang and other GCC-compatible compilers).
function(mfl_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        # fopen/strcpy etc. are standard C11; silence MSVC's C4996.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
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
target_include_directories(mfl PRIVATE ${PROJECT_SOURCE_DIR}/src)
mfl_set_warnings(mfl)

# ---------------------------------------------------------------------------
# Embedding example (uses only the public header include/mfl/mfl.h)
# ---------------------------------------------------------------------------
if(MFL_BUILD_EXAMPLES)
    add_executable(mfl_embed_example examples/embedding/heat_input_host.c)
    target_link_libraries(mfl_embed_example PRIVATE mfl_core)
    mfl_set_warnings(mfl_embed_example)
endif()

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

    # Lexer unit tests.
    add_executable(test_lexer tests/unit/test_lexer.c)
    target_link_libraries(test_lexer PRIVATE mfl_core)
    target_include_directories(test_lexer PRIVATE ${PROJECT_SOURCE_DIR}/src)
    mfl_set_warnings(test_lexer)
    add_test(NAME lexer_unit COMMAND test_lexer)

    # Parser unit tests.
    add_executable(test_parser tests/unit/test_parser.c)
    target_link_libraries(test_parser PRIVATE mfl_core)
    target_include_directories(test_parser PRIVATE ${PROJECT_SOURCE_DIR}/src)
    mfl_set_warnings(test_parser)
    add_test(NAME parser_unit COMMAND test_parser)

    # Units and semantic checker unit tests.
    add_executable(test_checker tests/unit/test_checker.c)
    target_link_libraries(test_checker PRIVATE mfl_core)
    target_include_directories(test_checker PRIVATE ${PROJECT_SOURCE_DIR}/src)
    mfl_set_warnings(test_checker)
    add_test(NAME checker_unit COMMAND test_checker)

    # Number parsing/formatting and compiler + VM unit tests.
    # API test: public header only (no src/ include directory).
    add_executable(test_api tests/unit/test_api.c)
    target_link_libraries(test_api PRIVATE mfl_core)
    mfl_set_warnings(test_api)
    add_test(NAME api_unit COMMAND test_api)

    # The public header must also compile as C++ (game engines are C++).
    include(CheckLanguage)
    check_language(CXX)
    if(CMAKE_CXX_COMPILER)
        enable_language(CXX)
        add_executable(test_api_cpp tests/unit/test_api_cpp.cpp)
        set_target_properties(test_api_cpp PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
        target_link_libraries(test_api_cpp PRIVATE mfl_core)
        add_test(NAME api_cpp COMMAND test_api_cpp)
    else()
        message(STATUS "MFL: no C++ compiler found; skipping the C++ header test")
    endif()

    foreach(t number vm)
        add_executable(test_${t} tests/unit/test_${t}.c)
        target_link_libraries(test_${t} PRIVATE mfl_core)
        target_include_directories(test_${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
        mfl_set_warnings(test_${t})
        add_test(NAME ${t}_unit COMMAND test_${t})
    endforeach()

    # Token-stream golden tests: `mfl --tokens <name>.mfl` must print
    # exactly tests/language/tokens/<name>.tokens.
    function(mfl_add_token_test name source expected_exit)
        add_test(NAME lexer_tokens_${name}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:mfl>
                "-DARGS=--tokens;${source}"
                -DEXPECTED=${PROJECT_SOURCE_DIR}/tests/language/tokens/${name}.tokens
                -DEXPECTED_EXIT=${expected_exit}
                -P ${PROJECT_SOURCE_DIR}/tests/cmake/compare_output.cmake)
    endfunction()
    mfl_add_token_test(heat_input
        ${PROJECT_SOURCE_DIR}/examples/welding/heat_input.mfl 0)
    mfl_add_token_test(errors
        ${PROJECT_SOURCE_DIR}/tests/language/tokens/errors.mfl 1)

    # Syntax-tree golden tests: `mfl --ast <file>` is run from the source
    # directory with a relative path; stdout must equal <name>.ast and
    # stderr <name>.stderr (an empty file when there are no errors).
    function(mfl_add_ast_test name source expected_exit)
        add_test(NAME parser_ast_${name}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:mfl>
                "-DARGS=--ast;${source}"
                -DEXPECTED=${PROJECT_SOURCE_DIR}/tests/language/ast/${name}.ast
                -DEXPECTED_STDERR=${PROJECT_SOURCE_DIR}/tests/language/ast/${name}.stderr
                -DEXPECTED_EXIT=${expected_exit}
                -DWORKDIR=${PROJECT_SOURCE_DIR}
                -P ${PROJECT_SOURCE_DIR}/tests/cmake/compare_output.cmake)
    endfunction()
    mfl_add_ast_test(heat_input examples/welding/heat_input.mfl 0)
    mfl_add_ast_test(expressions tests/language/ast/expressions.mfl 0)
    mfl_add_ast_test(errors tests/language/ast/errors.mfl 1)

    # Semantic-check golden tests: `mfl --types <file>`; stdout must equal
    # <name>.types (inferred types) and stderr <name>.stderr.
    function(mfl_add_check_test name source expected_exit)
        add_test(NAME checker_types_${name}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:mfl>
                "-DARGS=--types;${source}"
                -DEXPECTED=${PROJECT_SOURCE_DIR}/tests/language/check/${name}.types
                -DEXPECTED_STDERR=${PROJECT_SOURCE_DIR}/tests/language/check/${name}.stderr
                -DEXPECTED_EXIT=${expected_exit}
                -DWORKDIR=${PROJECT_SOURCE_DIR}
                -P ${PROJECT_SOURCE_DIR}/tests/cmake/compare_output.cmake)
    endfunction()
    mfl_add_check_test(heat_input examples/welding/heat_input.mfl 0)
    mfl_add_check_test(steel_properties examples/thermal/steel_properties.mfl 0)
    mfl_add_check_test(errors tests/language/check/errors.mfl 1)

    # Execution tests: `mfl <file>` (or `mfl --bytecode <file>`) is run
    # from the source directory; stdout must equal the expected file and
    # stderr must be empty.
    function(mfl_add_run_test name source expected)
        add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:mfl>
                "-DARGS=${source}"
                -DEXPECTED=${PROJECT_SOURCE_DIR}/${expected}
                -DEXPECTED_STDERR=${PROJECT_SOURCE_DIR}/tests/cmake/empty.txt
                -DWORKDIR=${PROJECT_SOURCE_DIR}
                -P ${PROJECT_SOURCE_DIR}/tests/cmake/compare_output.cmake)
    endfunction()

    # Language conformance tests: every tests/language/conformance_*.mfl
    # must print exactly its .expected file.
    file(GLOB MFL_CONFORMANCE CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/tests/language/conformance_*.mfl)
    foreach(src ${MFL_CONFORMANCE})
        get_filename_component(base ${src} NAME_WE)
        mfl_add_run_test(${base} tests/language/${base}.mfl tests/language/${base}.expected)
    endforeach()

    mfl_add_run_test(run_heat_input examples/welding/heat_input.mfl
        tests/language/run/heat_input.expected)
    mfl_add_run_test(run_steel_properties examples/thermal/steel_properties.mfl
        tests/language/run/steel_properties.expected)
    mfl_add_run_test(run_hello examples/basic/hello.mfl tests/language/run/hello.expected)
    mfl_add_run_test(bytecode_heat_input "--bytecode;examples/welding/heat_input.mfl"
        tests/language/run/heat_input.bytecode)

    # Program tests with expected stdout, stderr and exit code (run from
    # the source directory, so messages show relative paths).
    function(mfl_add_program_test name source stdout stderr exit_code)
        add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:mfl>
                "-DARGS=${source}"
                -DEXPECTED=${PROJECT_SOURCE_DIR}/${stdout}
                -DEXPECTED_STDERR=${PROJECT_SOURCE_DIR}/${stderr}
                -DEXPECTED_EXIT=${exit_code}
                -DWORKDIR=${PROJECT_SOURCE_DIR}
                -P ${PROJECT_SOURCE_DIR}/tests/cmake/compare_output.cmake)
    endfunction()

    # v0.2 error tests: compile-time and run-time errors with positions.
    file(GLOB MFL_ERROR_TESTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/language/errors/*.mfl)
    foreach(src ${MFL_ERROR_TESTS})
        get_filename_component(base ${src} NAME_WE)
        mfl_add_program_test(error_${base} tests/language/errors/${base}.mfl
            tests/language/errors/${base}.stdout tests/language/errors/${base}.stderr 1)
    endforeach()

    # v0.2 acceptance demonstration (examples/welding/heat_input_v02.mfl).
    set(MFL_DEMO examples/welding/heat_input_v02.mfl)
    mfl_add_program_test(acceptance_demo_output ${MFL_DEMO}
        tests/language/run/heat_input_v02.expected tests/cmake/empty.txt 0)
    foreach(case
            "5mm_s_gives_480|v = 5 mm/s -> 480 J/mm\n"
            "0_005m_s_gives_480|v = 0[.]005 m/s -> 480 J/mm\n"
            "10mm_s_gives_240|v = 10 mm/s -> 240 J/mm\n"
            "zero_speed_branch|v = 0 mm/s -> arc not moving: heat input not defined, skipped\n")
        string(REPLACE "|" ";" parts "${case}")
        list(GET parts 0 case_name)
        list(GET parts 1 case_regex)
        add_test(NAME acceptance_${case_name}
            COMMAND $<TARGET_FILE:mfl> ${MFL_DEMO}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(acceptance_${case_name} PROPERTIES
            PASS_REGULAR_EXPRESSION "${case_regex}")
    endforeach()
    foreach(base power_as_speed bad_return index_out_of_range)
        mfl_add_program_test(acceptance_${base} tests/language/acceptance/${base}.mfl
            tests/language/acceptance/${base}.stdout tests/language/acceptance/${base}.stderr 1)
    endforeach()

    if(MFL_BUILD_EXAMPLES)
        add_test(NAME run_embed_example
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:mfl_embed_example>
                -DEXPECTED=${PROJECT_SOURCE_DIR}/tests/language/run/embed_example.expected
                -DEXPECTED_STDERR=${PROJECT_SOURCE_DIR}/tests/cmake/empty.txt
                -P ${PROJECT_SOURCE_DIR}/tests/cmake/compare_output.cmake)
    endif()
endif()
