# Runs PROGRAM with ARGS in WORKDIR and checks the results. List values use
# '|' as separator (they pass through CMake function arguments intact).
#
#   EXPECTED_EXIT    exit code
#   STDERR_REGEX     regular expression the standard error must match (optional)
#   MUST_EXIST       list of files that must exist afterwards (optional)
#   MUST_NOT_EXIST   list of files that must not exist afterwards (optional)
#   CLEAN            list of files removed before running (optional)
#   MKDIR            directory created before running (optional)
foreach(v ARGS CLEAN MUST_EXIST MUST_NOT_EXIST)
    if(DEFINED ${v})
        string(REPLACE "|" ";" ${v} "${${v}}")
    endif()
endforeach()
foreach(f IN LISTS CLEAN)
    if(NOT IS_DIRECTORY "${f}")
        file(REMOVE "${f}")
    endif()
endforeach()
if(DEFINED MKDIR AND NOT MKDIR STREQUAL "")
    file(MAKE_DIRECTORY "${MKDIR}")
endif()
if(NOT DEFINED WORKDIR)
    set(WORKDIR ".")
endif()
execute_process(
    COMMAND ${PROGRAM} ${ARGS}
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT exit_code STREQUAL "${EXPECTED_EXIT}")
    message(FATAL_ERROR "exit code ${exit_code}, expected ${EXPECTED_EXIT}\nstdout:\n${out}\nstderr:\n${err}")
endif()
if(DEFINED STDERR_REGEX AND NOT err MATCHES "${STDERR_REGEX}")
    message(FATAL_ERROR "stderr does not match '${STDERR_REGEX}':\n${err}")
endif()
foreach(f IN LISTS MUST_EXIST)
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "expected file '${f}' does not exist\nstderr:\n${err}")
    endif()
endforeach()
foreach(f IN LISTS MUST_NOT_EXIST)
    if(EXISTS "${f}" AND NOT IS_DIRECTORY "${f}")
        message(FATAL_ERROR "file '${f}' should not exist")
    endif()
endforeach()
message(STATUS "exit ${exit_code} as expected\n${err}")
