# RS-10: a test that is decided by its output must still fail on a
# non-zero exit status. Checks cmake/RunOutputTest.cmake case by case, runs
# the lab's reader-profile scenario into an unwritable output directory
# (it prints the expected line, then fails), and checks that no CMake file
# registers a test through CTest's own output patterns, which ignore the
# exit status.
#   cmake -DTILEFINCH_ROOT=<source> -DTILEFINCH_SCRATCH=<dir>
#         -DTILEFINCH_LAB=<psp-browser-lab> -P test_run_output_test.cmake
cmake_minimum_required(VERSION 3.20)
foreach(required TILEFINCH_ROOT TILEFINCH_SCRATCH TILEFINCH_LAB)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "test_run_output_test needs ${required}")
    endif()
endforeach()
set(runner "${TILEFINCH_ROOT}/cmake/RunOutputTest.cmake")
file(MAKE_DIRECTORY "${TILEFINCH_SCRATCH}")
set(failures 0)

# A command that prints a line and exits with the status given.
set(printer "${TILEFINCH_SCRATCH}/print-and-exit.cmake")
file(WRITE "${printer}" [==[
message("${LINE}")
if(NOT STATUS EQUAL 0)
    message(FATAL_ERROR "exiting with failure")
endif()
]==])

function(expect_runner label wanted spec_text)
    set(spec "${TILEFINCH_SCRATCH}/${label}.spec.cmake")
    file(WRITE "${spec}" "${spec_text}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DTILEFINCH_OUTPUT_SPEC=${spec}
            -P "${runner}" -- ${ARGN}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(status STREQUAL "0")
        set(outcome pass)
    else()
        set(outcome fail)
    endif()
    if(NOT outcome STREQUAL wanted)
        message("FAIL ${label}: runner said ${outcome} (status ${status}), "
                "wanted ${wanted}\n${output}")
        math(EXPR count "${failures} + 1")
        set(failures ${count} PARENT_SCOPE)
    else()
        message("ok ${label}: ${outcome}")
    endif()
endfunction()

set(expect_line "set(TILEFINCH_EXPECT [==[reader profile=none]==])\n")
set(reject_shared "set(TILEFINCH_REJECT [==[CHECK failed]==])\n")
expect_runner(prints-and-succeeds pass "${expect_line}${reject_shared}"
    "${CMAKE_COMMAND}" "-DLINE=reader profile=none" -DSTATUS=0
    -P "${printer}")
expect_runner(prints-then-fails fail "${expect_line}${reject_shared}"
    "${CMAKE_COMMAND}" "-DLINE=reader profile=none" -DSTATUS=1
    -P "${printer}")
expect_runner(succeeds-without-line fail "${expect_line}${reject_shared}"
    "${CMAKE_COMMAND}" "-DLINE=reader profile=other" -DSTATUS=0
    -P "${printer}")
expect_runner(prints-rejected-line fail "${expect_line}${reject_shared}"
    "${CMAKE_COMMAND}" "-DLINE=reader profile=none CHECK failed at x" -DSTATUS=0
    -P "${printer}")
expect_runner(every-expected-line fail
    "set(TILEFINCH_EXPECT [==[reader profile=none;second line]==])\n"
    "${CMAKE_COMMAND}" "-DLINE=reader profile=none" -DSTATUS=0
    -P "${printer}")
expect_runner(lab-fails-after-expected-line fail
    "${expect_line}${reject_shared}"
    "${TILEFINCH_LAB}" --fixture "${TILEFINCH_ROOT}/fixtures/demo.html"
    --limit-mb 24 --output-dir /nonexistent-tilefinch-output/x)

file(GLOB cmake_files "${TILEFINCH_ROOT}/cmake/*.cmake")
file(GLOB_RECURSE list_files "${TILEFINCH_ROOT}/tests/CMakeLists.txt"
    "${TILEFINCH_ROOT}/src/CMakeLists.txt")
foreach(file "${TILEFINCH_ROOT}/CMakeLists.txt" ${cmake_files} ${list_files})
    file(STRINGS "${file}" lines REGEX "(PASS|FAIL)_REGULAR_EXPRESSION")
    foreach(line IN LISTS lines)
        message("FAIL ${file}: CTest output pattern ignores the exit status; "
                "use tilefinch_add_output_test: ${line}")
        math(EXPR failures "${failures} + 1")
    endforeach()
endforeach()

if(NOT failures EQUAL 0)
    message(FATAL_ERROR "${failures} output-test check(s) failed")
endif()
