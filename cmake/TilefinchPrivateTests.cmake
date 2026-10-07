# Optional local suites are not part of the public source distribution.
option(TILEFINCH_ENABLE_PRIVATE_TESTS
    "Build and register local private tests (requires their source directory)" OFF)
set(TILEFINCH_PRIVATE_TESTS_DIR
    "${PROJECT_SOURCE_DIR}/.private-investigations/tests" CACHE PATH
    "Local private-test source directory containing CMakeLists.txt")

# Use this with tilefinch_add_test_binary so private executables participate
# in the same build-before-CTest dependency gate as public executables.
function(tilefinch_add_private_test name)
    cmake_parse_arguments(PARSE_ARGV 1 test "" "TIMEOUT" "COMMAND;LABELS")
    if(test_UNPARSED_ARGUMENTS OR test_KEYWORDS_MISSING_VALUES OR NOT test_COMMAND)
        message(FATAL_ERROR "Invalid private test registration: ${name}")
    endif()
    if(NOT test_TIMEOUT)
        set(test_TIMEOUT 30)
    endif()
    add_test(NAME ${name} COMMAND ${test_COMMAND})
    set_tests_properties(${name} PROPERTIES
        LABELS "tilefinch;private;${test_LABELS}" TIMEOUT ${test_TIMEOUT})
endfunction()

if(TILEFINCH_ENABLE_PRIVATE_TESTS)
    if(PSP)
        message(FATAL_ERROR "Private tests are a host-only gate")
    endif()
    # Let add_subdirectory read the manifest and fail normally if absent.
    # A redundant EXISTS/readability precheck can false-fail on macOS files
    # written by another process context even when opening the file succeeds.
    add_subdirectory("${TILEFINCH_PRIVATE_TESTS_DIR}"
        "${PROJECT_BINARY_DIR}/private-tests")
endif()
