# compare_output.cmake - run a program and compare its stdout with a file.
#
# Usage (from CTest):
#   cmake -DPROGRAM=<exe> -DARGS="a;b" -DEXPECTED=<file>
#         [-DEXPECTED_EXIT=<n>] -P compare_output.cmake
#
# Line endings are normalised (CRLF -> LF) on both sides, so the same
# expected files work on Windows and with git autocrlf.

foreach(var PROGRAM EXPECTED)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "compare_output.cmake: ${var} is not set")
    endif()
endforeach()
if(NOT DEFINED EXPECTED_EXIT)
    set(EXPECTED_EXIT 0)
endif()

execute_process(
    COMMAND "${PROGRAM}" ${ARGS}
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE  stderr_text
    RESULT_VARIABLE exit_code)

file(READ "${EXPECTED}" expected)
string(REPLACE "\r\n" "\n" actual "${actual}")
string(REPLACE "\r\n" "\n" expected "${expected}")

if(NOT exit_code STREQUAL "${EXPECTED_EXIT}")
    message(FATAL_ERROR
        "exit code ${exit_code}, expected ${EXPECTED_EXIT}\nstderr:\n${stderr_text}")
endif()
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
        "output differs from ${EXPECTED}\n--- expected\n${expected}\n--- actual\n${actual}")
endif()
