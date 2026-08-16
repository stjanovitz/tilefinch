if(NOT DEFINED MAP_FILE OR NOT DEFINED EXPECTED_SHA256
   OR NOT DEFINED EXPECTED_SIZE)
    message(FATAL_ERROR
        "MAP_FILE, EXPECTED_SHA256, and EXPECTED_SIZE are required")
endif()

if(NOT EXISTS "${MAP_FILE}")
    message(FATAL_ERROR "Voice-model map is missing: ${MAP_FILE}")
endif()

file(SIZE "${MAP_FILE}" actual_size)
if(NOT actual_size EQUAL EXPECTED_SIZE)
    message(FATAL_ERROR
        "Voice-model map size drifted for ${MAP_FILE}: "
        "${actual_size} != ${EXPECTED_SIZE}")
endif()

file(SHA256 "${MAP_FILE}" actual_sha256)
if(NOT actual_sha256 STREQUAL EXPECTED_SHA256)
    message(FATAL_ERROR
        "Voice-model map content drifted for ${MAP_FILE}: "
        "${actual_sha256} != ${EXPECTED_SHA256}")
endif()
