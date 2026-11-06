# compare_output.cmake - run a program and compare its stdout with a file.
#
# Usage (from CTest):
#   cmake -DPROGRAM=<exe> -DARGS="a;b" -DEXPECTED=<file>
#         [-DEXPECTED_EXIT=<n>] [-DEXPECTED_STDERR=<file>]
#         [-DWORKDIR=<dir>] -P compare_output.cmake
#
# EXPECTED is compared with stdout; EXPECTED_STDERR, if given, with
# stderr. WORKDIR lets tests pass relative paths, so messages that echo
# the file name are the same on every machine.
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
if(NOT DEFINED WORKDIR)
    set(WORKDIR ".")
endif()

execute_process(
    COMMAND "${PROGRAM}" ${ARGS}
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE  stderr_text
    RESULT_VARIABLE exit_code
    WORKING_DIRECTORY "${WORKDIR}")

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
if(DEFINED EXPECTED_STDERR)
    file(READ "${EXPECTED_STDERR}" expected_err)
    string(REPLACE "\r\n" "\n" expected_err "${expected_err}")
    string(REPLACE "\r\n" "\n" stderr_text "${stderr_text}")
    if(NOT stderr_text STREQUAL expected_err)
        message(FATAL_ERROR
            "stderr differs from ${EXPECTED_STDERR}\n--- expected\n${expected_err}\n"
            "--- actual\n${stderr_text}")
    endif()
endif()
