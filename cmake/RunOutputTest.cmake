# Runs one test command and passes only when the command exits 0, its
# output (stdout and stderr together) matches every TILEFINCH_EXPECT
# pattern, and it matches no TILEFINCH_REJECT pattern. CTest's own output
# patterns decide a test by its text alone and ignore the exit status, so a
# run that printed the expected line and then failed, or crashed at
# teardown, would still pass. Usage:
#   cmake -DTILEFINCH_OUTPUT_SPEC=<spec> -P RunOutputTest.cmake -- <command>...
# The spec (written by tilefinch_add_output_test) sets TILEFINCH_EXPECT,
# TILEFINCH_REJECT and TILEFINCH_OUTPUT_TIMEOUT.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED TILEFINCH_OUTPUT_SPEC OR NOT EXISTS "${TILEFINCH_OUTPUT_SPEC}")
    message(FATAL_ERROR "RunOutputTest: TILEFINCH_OUTPUT_SPEC is missing")
endif()
include("${TILEFINCH_OUTPUT_SPEC}")

set(command)
set(after_separator FALSE)
math(EXPR last_argument "${CMAKE_ARGC} - 1")
foreach(index RANGE ${last_argument})
    if(after_separator)
        list(APPEND command "${CMAKE_ARGV${index}}")
    elseif("${CMAKE_ARGV${index}}" STREQUAL "--")
        set(after_separator TRUE)
    endif()
endforeach()
if(NOT command)
    message(FATAL_ERROR "RunOutputTest: no command after --")
endif()

set(timeout_arguments)
if(TILEFINCH_OUTPUT_TIMEOUT)
    set(timeout_arguments TIMEOUT ${TILEFINCH_OUTPUT_TIMEOUT})
endif()
execute_process(COMMAND ${command}
    ${timeout_arguments}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
# Echo the run for CTest's log and --output-on-failure.
message("${output}")

set(problems)
if(NOT "${status}" STREQUAL "0")
    list(APPEND problems "exit status ${status}")
endif()
foreach(pattern IN LISTS TILEFINCH_EXPECT)
    if(NOT output MATCHES "${pattern}")
        list(APPEND problems "missing output /${pattern}/")
    endif()
endforeach()
foreach(pattern IN LISTS TILEFINCH_REJECT)
    if(output MATCHES "${pattern}")
        list(APPEND problems "rejected output /${pattern}/: ${CMAKE_MATCH_0}")
    endif()
endforeach()
if(problems)
    list(JOIN problems "\n  " report)
    message(FATAL_ERROR "RunOutputTest failed:\n  ${report}")
endif()
