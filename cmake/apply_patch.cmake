if(NOT DEFINED PATCH_SOURCE_DIR OR NOT DEFINED PATCH_FILE OR
   NOT DEFINED PATCH_EXECUTABLE)
    message(FATAL_ERROR
        "PATCH_SOURCE_DIR, PATCH_FILE, and PATCH_EXECUTABLE are required")
endif()

execute_process(
    COMMAND "${PATCH_EXECUTABLE}" --forward --dry-run --silent -p1
        -i "${PATCH_FILE}"
    WORKING_DIRECTORY "${PATCH_SOURCE_DIR}"
    RESULT_VARIABLE patch_check_result
    OUTPUT_QUIET
    ERROR_QUIET)

if(patch_check_result EQUAL 0)
    execute_process(
        COMMAND "${PATCH_EXECUTABLE}" --forward --silent -p1
            -i "${PATCH_FILE}"
        WORKING_DIRECTORY "${PATCH_SOURCE_DIR}"
        RESULT_VARIABLE patch_result
        OUTPUT_VARIABLE patch_output
        ERROR_VARIABLE patch_error)
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR
            "Could not apply ${PATCH_FILE}: ${patch_output}${patch_error}")
    endif()
    return()
endif()

execute_process(
    COMMAND "${PATCH_EXECUTABLE}" --dry-run --silent -R -p1
        -i "${PATCH_FILE}"
    WORKING_DIRECTORY "${PATCH_SOURCE_DIR}"
    RESULT_VARIABLE reverse_check_result
    OUTPUT_QUIET
    ERROR_QUIET)

if(NOT reverse_check_result EQUAL 0)
    message(FATAL_ERROR
        "${PATCH_FILE} does not apply cleanly to ${PATCH_SOURCE_DIR}")
endif()
