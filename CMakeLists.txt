cmake_minimum_required(VERSION 3.16)

project(MFL
    VERSION 0.3.0
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
# Numerical library (modules/numerical): plain C11 numerics in SI doubles,
# independent of the parser, the compiler and the VM. The language binding
# (src/modules) and the embedding API call it.
# ---------------------------------------------------------------------------
add_library(mfl_numerical STATIC
    modules/numerical/vector.c
    modules/numerical/heat1d.c
    modules/numerical/heat1d_export.c
    src/runtime/number.c
)
target_include_directories(mfl_numerical
    PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/modules/numerical>
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Core library: lexer, parser, checker, compiler, VM, runtime, module
# bindings and the embedding API.
# ---------------------------------------------------------------------------
add_library(mfl_core
    src/version.c
    src/lexer/lexer.c
    src/parser/ast.c
    src/parser/parser.c
    src/runtime/arena.c
    src/runtime/units.c
    src/compiler/checker.c
    src/compiler/chunk.c
    src/compiler/compiler.c
    src/compiler/loader.c
    src/modules/natives.c
    src/modules/heat_natives.c
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

target_link_libraries(mfl_core PUBLIC mfl_numerical)

# Reproducible floating point: no fused multiply-add contraction (Clang
# contracts a + b*c by default on ARM64, e.g. macOS runners), so the solver
# and the VM give bit-identical results on every platform. MSVC does not
# contract by default (/fp:precise).
if(NOT MSVC)
    target_compile_options(mfl_numerical PRIVATE -ffp-contract=off)
    target_compile_options(mfl_core PRIVATE -ffp-contract=off)
endif()

mfl_set_warnings(mfl_core)
mfl_set_warnings(mfl_numerical)

if(NOT MSVC)
    # libm will be required by the numerical runtime.
    find_library(MFL_LIBM m)
    if(MFL_LIBM)
        target_link_libraries(mfl_numerical PUBLIC ${MFL_LIBM})
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
    # v0.3: the heated-bar case from C (same solver as the CLI).
    add_executable(mfl_heat_host examples/embedding/heat_bar_host.c)
    target_link_libraries(mfl_heat_host PRIVATE mfl_core)
    mfl_set_warnings(mfl_heat_host)
endif()

# ---------------------------------------------------------------------------
# Installation / binary package (cmake --install build --prefix <dir>).
# The CI archives the installed tree for Windows, Linux and macOS.
# ---------------------------------------------------------------------------
install(TARGETS mfl RUNTIME DESTINATION bin)
install(TARGETS mfl_core mfl_numerical ARCHIVE DESTINATION lib)
install(FILES include/mfl/mfl.h DESTINATION include/mfl)
if(MFL_BUILD_EXAMPLES)
    install(TARGETS mfl_heat_host RUNTIME DESTINATION bin)
endif()
install(DIRECTORY examples/ DESTINATION examples)
install(FILES docs/language-spec.md docs/heat-conduction.md docs/embedding.md docs/units.md
        DESTINATION docs)
install(FILES LICENSE DESTINATION .)
install(FILES packaging/README-package.md DESTINATION . RENAME README.md)

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
    add_test(NAME api_unit COMMAND test_api ${PROJECT_SOURCE_DIR})

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

    # Numerical library tests (modules/numerical, no interpreter involved).
    add_executable(test_vector tests/numerical/test_vector.c)
    target_link_libraries(test_vector PRIVATE mfl_numerical)
    mfl_set_warnings(test_vector)
    add_test(NAME numerical_vector COMMAND test_vector)
    add_executable(test_heat1d tests/numerical/test_heat1d.c)
    target_link_libraries(test_heat1d PRIVATE mfl_numerical)
    mfl_set_warnings(test_heat1d)
    add_test(NAME numerical_heat1d COMMAND test_heat1d)
    add_executable(test_heat1d_export tests/numerical/test_heat1d_export.c)
    target_link_libraries(test_heat1d_export PRIVATE mfl_numerical)
    mfl_set_warnings(test_heat1d_export)
    add_test(NAME numerical_heat1d_export COMMAND test_heat1d_export)

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
    set_tests_properties(error_rt_range_step PROPERTIES TIMEOUT 5)

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
    # v0.3 imports: stdout, stderr and exit code of each case.
    foreach(case "ok|0" "missing|1" "cycle|1" "visibility|1" "runtime_in_import|1" "duplicate|1")
        string(REPLACE "|" ";" parts "${case}")
        list(GET parts 0 case_name)
        list(GET parts 1 case_exit)
        mfl_add_program_test(import_${case_name} tests/language/imports/${case_name}.mfl
            tests/language/imports/${case_name}.stdout tests/language/imports/${case_name}.stderr
            ${case_exit})
    endforeach()

    foreach(base power_as_speed bad_return index_out_of_range)
        mfl_add_program_test(acceptance_${base} tests/language/acceptance/${base}.mfl
            tests/language/acceptance/${base}.stdout tests/language/acceptance/${base}.stderr 1)
    endforeach()

    # ------------------------------------------------------------------
    # v0.3: heat module, export and CLI (docs/heat-conduction.md).
    # ------------------------------------------------------------------
    mfl_add_run_test(heat_conformance tests/language/heat/conformance_heat.mfl
        tests/language/heat/conformance_heat.expected)
    foreach(base unstable wrong_dimension difference_at_runtime non_finite bad_mesh too_few_nodes
            mesh_short negative missing_parameter no_run not_a_node not_a_sample)
        mfl_add_program_test(heat_error_${base} tests/language/heat/${base}.mfl
            tests/language/heat/${base}.stdout tests/language/heat/${base}.stderr 1)
    endforeach()

    add_executable(check_heat_csv tests/numerical/check_heat_csv.c)
    if(MFL_LIBM)
        target_link_libraries(check_heat_csv PRIVATE ${MFL_LIBM})
    endif()
    mfl_set_warnings(check_heat_csv)

    set(MFL_OUT ${CMAKE_CURRENT_BINARY_DIR}/export)
    function(mfl_add_export_test name exit_code regex)
        add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:mfl>
                -DWORKDIR=${PROJECT_SOURCE_DIR}
                -DEXPECTED_EXIT=${exit_code}
                "-DSTDERR_REGEX=${regex}"
                ${ARGN}
                -P ${PROJECT_SOURCE_DIR}/tests/cmake/run_export.cmake)
    endfunction()

    # The acceptance example: export, then check the files independently.
    mfl_add_export_test(export_bar 0 "exported 61 samples x 21 nodes"
        "-DARGS=examples/thermal/bar_heated_end.mfl|--csv|${MFL_OUT}/bar_heated_end.csv"
        -DMKDIR=${MFL_OUT}
        "-DCLEAN=${MFL_OUT}/bar_heated_end.csv|${MFL_OUT}/bar_heated_end.json"
        "-DMUST_EXIST=${MFL_OUT}/bar_heated_end.csv|${MFL_OUT}/bar_heated_end.json"
        "-DMUST_NOT_EXIST=${MFL_OUT}/bar_heated_end.csv.partial|${MFL_OUT}/bar_heated_end.json.partial")
    set_tests_properties(export_bar PROPERTIES FIXTURES_SETUP bar_csv)
    # Header, 1281 rows, t = 0 and 600 s, uniform positions 0..0.1 m, size and
    # hash against the metadata; the hot end is exactly 773.15 K at every
    # time; at t = 0 the rest of the bar is exactly 293.15 K.
    add_test(NAME export_bar_csv_check
        COMMAND check_heat_csv ${MFL_OUT}/bar_heated_end.csv ${MFL_OUT}/bar_heated_end.json
            --end 773.15 0 --value 0 0.05 293.15 0 --value 0 0.1 293.15 0)
    set_tests_properties(export_bar_csv_check PROPERTIES FIXTURES_REQUIRED bar_csv)
    # Demonstration output: threshold reached (first sample at or above it)
    # and threshold outside the simulated range.
    add_test(NAME demo_bar_heated_end COMMAND $<TARGET_FILE:mfl> examples/thermal/bar_heated_end.mfl
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(demo_bar_heated_end PROPERTIES PASS_REGULAR_EXPRESSION
        "center reaches 150 degC at the sample t = 90 s [|] previous sample: 80 s -> 146[.][0-9]+ degC\ncenter does not reach 600 degC within 600 s")
    mfl_add_run_test(run_bar_heated_end examples/thermal/bar_heated_end.mfl
        tests/language/run/bar_heated_end.expected)
    # Physical model of the virtual calorimetry lab (examples/lab).
    mfl_add_run_test(run_lab_model examples/lab/check_model.mfl tests/language/run/lab_model.expected)

    # Analytical case through the CLI: every CSV value against the formula.
    # Tolerance 0.035 K = the bound declared for 17 nodes in test_heat1d.c.
    mfl_add_export_test(export_sine 0 "exported 21 samples x 17 nodes"
        "-DARGS=tests/language/heat/sine_export.mfl|--csv|${MFL_OUT}/sine.csv|--meta|${MFL_OUT}/sine_meta.json"
        -DMKDIR=${MFL_OUT}
        "-DCLEAN=${MFL_OUT}/sine.csv|${MFL_OUT}/sine_meta.json"
        "-DMUST_EXIST=${MFL_OUT}/sine.csv|${MFL_OUT}/sine_meta.json")
    set_tests_properties(export_sine PROPERTIES FIXTURES_SETUP sine_csv)
    add_test(NAME export_sine_analytical
        COMMAND check_heat_csv ${MFL_OUT}/sine.csv ${MFL_OUT}/sine_meta.json --sine 300 50 0.035)
    set_tests_properties(export_sine_analytical PROPERTIES FIXTURES_REQUIRED sine_csv)

    # The small export kept in the repository is reproduced byte for byte.
    mfl_add_export_test(export_small_bar 0 "exported 6 samples x 5 nodes"
        "-DARGS=examples/thermal/small_bar.mfl|--csv|${MFL_OUT}/small_bar.csv"
        -DMKDIR=${MFL_OUT}
        "-DMUST_EXIST=${MFL_OUT}/small_bar.csv|${MFL_OUT}/small_bar.json")
    set_tests_properties(export_small_bar PROPERTIES FIXTURES_SETUP small_csv)
    add_test(NAME export_small_bar_reproduced
        COMMAND ${CMAKE_COMMAND} -E compare_files ${MFL_OUT}/small_bar.csv
            ${PROJECT_SOURCE_DIR}/examples/thermal/output/small_bar.csv)
    set_tests_properties(export_small_bar_reproduced PROPERTIES FIXTURES_REQUIRED small_csv)
    add_test(NAME export_small_bar_check
        COMMAND check_heat_csv ${MFL_OUT}/small_bar.csv ${MFL_OUT}/small_bar.json
            --end 373.15 0 --value 0 0.01 293.15 0)
    set_tests_properties(export_small_bar_check PROPERTIES FIXTURES_REQUIRED small_csv)

    # Write failures: exit code 4, no metadata (the completion marker) and
    # no temporary files left.
    mfl_add_export_test(export_fail_missing_dir 4
        "export failed: cannot write .*no metadata file was written"
        "-DARGS=examples/thermal/bar_heated_end.mfl|--csv|${MFL_OUT}/no/such/dir/bar.csv"
        "-DMUST_NOT_EXIST=${MFL_OUT}/no/such/dir/bar.json|${MFL_OUT}/no/such/dir/bar.csv.partial")
    mfl_add_export_test(export_fail_target_is_directory 4
        "export failed: cannot rename .*incomplete"
        "-DARGS=examples/thermal/bar_heated_end.mfl|--csv|${MFL_OUT}/a_directory|--meta|${MFL_OUT}/dir_case.json"
        -DMKDIR=${MFL_OUT}/a_directory
        "-DCLEAN=${MFL_OUT}/dir_case.json|${MFL_OUT}/a_directory"
        "-DMUST_NOT_EXIST=${MFL_OUT}/dir_case.json|${MFL_OUT}/dir_case.json.partial|${MFL_OUT}/a_directory.partial")
    mfl_add_export_test(export_no_simulation 1 "did not run a simulation"
        "-DARGS=examples/basic/hello.mfl|--csv|${MFL_OUT}/nothing.csv"
        "-DMUST_NOT_EXIST=${MFL_OUT}/nothing.csv|${MFL_OUT}/nothing.json")
    mfl_add_export_test(export_unstable_no_files 1 "runtime error: heat.run: unstable time step"
        "-DARGS=tests/language/heat/unstable.mfl|--csv|${MFL_OUT}/unstable.csv"
        "-DMUST_NOT_EXIST=${MFL_OUT}/unstable.csv|${MFL_OUT}/unstable.json")
    mfl_add_export_test(cli_meta_needs_csv 1 "--meta needs --csv"
        "-DARGS=examples/basic/hello.mfl|--meta|x.json")
    mfl_add_export_test(cli_csv_needs_name 1 "'--csv' needs a file name"
        "-DARGS=examples/basic/hello.mfl|--csv")
    mfl_add_export_test(cli_unknown_argument 1 "unknown argument '--cvs'"
        "-DARGS=examples/basic/hello.mfl|--cvs|x.csv")

    if(MFL_BUILD_EXAMPLES)
        # CLI and C host give the same results for the same case (exit 0
        # only if the final profiles agree within 1e-9 K).
        add_test(NAME heat_host_matches_cli
            COMMAND mfl_heat_host examples/thermal/bar_heated_end.mfl
                ${MFL_OUT}/bar_heated_end.csv
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(heat_host_matches_cli PROPERTIES
            FIXTURES_REQUIRED bar_csv
            PASS_REGULAR_EXPRESSION "max [|]C API - model[|] = 0 K.*max [|]C API - CLI CSV[|] = 0 K.*same results: OK")
        add_test(NAME run_embed_example
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:mfl_embed_example>
                -DEXPECTED=${PROJECT_SOURCE_DIR}/tests/language/run/embed_example.expected
                -DEXPECTED_STDERR=${PROJECT_SOURCE_DIR}/tests/cmake/empty.txt
                -P ${PROJECT_SOURCE_DIR}/tests/cmake/compare_output.cmake)
    endif()
endif()
