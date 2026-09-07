# Hash the complete authored/generated set on every build. Only the expensive
# regeneration/byte comparison is memoized, by contents rather than mtimes.
# CTest's tilefinch-bootstrap-generated-check remains an unconditional check.
foreach(required TILEFINCH_ROOT TILEFINCH_BOOTSTRAP_MANIFEST
                 TILEFINCH_BOOTSTRAP_GENERATOR TILEFINCH_BOOTSTRAP_SCRATCH)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "CheckBootstrapGenerated requires ${required}")
    endif()
endforeach()

set(verifier "${CMAKE_CURRENT_LIST_DIR}/VerifyBootstrapManifest.cmake")
function(verify_bootstrap_manifest)
    include("${verifier}")
endfunction()
verify_bootstrap_manifest()

function(bootstrap_verification_key output)
    file(SHA256 "${TILEFINCH_BOOTSTRAP_MANIFEST}" manifest_hash)
    file(SHA256 "${TILEFINCH_BOOTSTRAP_GENERATOR}" generator_hash)
    file(SHA256 "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" checker_hash)
    file(SHA256 "${verifier}" verifier_hash)
    string(SHA256 key
        "v1:${manifest_hash}:${generator_hash}:${checker_hash}:${verifier_hash}")
    set(${output} "${key}" PARENT_SCOPE)
endfunction()

set(stamp "${TILEFINCH_BOOTSTRAP_SCRATCH}/tilefinch-bootstrap-verified.sha256")
# Serialize concurrent invocations in this build directory; generator scratch
# files are already process-unique. A failed check never publishes a stamp.
file(LOCK "${stamp}.lock" GUARD PROCESS TIMEOUT 30)
bootstrap_verification_key(expected)
if(EXISTS "${stamp}")
    file(READ "${stamp}" previous LIMIT 65)
    if(previous STREQUAL "${expected}\n")
        message(STATUS "Reusing content-verified bootstrap bytecode check")
        return()
    endif()
endif()

execute_process(
    COMMAND "${TILEFINCH_BOOTSTRAP_GENERATOR}" --check
        "${TILEFINCH_ROOT}/src/bootstrap"
        "${TILEFINCH_ROOT}/src/generated/js_bootstrap.c"
        "${TILEFINCH_ROOT}/src/generated/js_bootstrap_bytecode.c"
        "${TILEFINCH_BOOTSTRAP_SCRATCH}"
    RESULT_VARIABLE result)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Bootstrap regeneration check failed: ${result}")
endif()
# Do not cache a check performed while its inputs or generator were changing.
verify_bootstrap_manifest()
bootstrap_verification_key(after)
if(NOT after STREQUAL expected)
    message(FATAL_ERROR "Bootstrap inputs changed during verification; retry the build")
endif()
file(WRITE "${stamp}.tmp" "${expected}\n")
file(RENAME "${stamp}.tmp" "${stamp}")
message(STATUS "Recorded content-verified bootstrap bytecode check")
