if(NOT DEFINED TILEFINCH_ROOT OR NOT DEFINED TILEFINCH_BOOTSTRAP_MANIFEST)
    message(FATAL_ERROR
        "VerifyBootstrapManifest requires TILEFINCH_ROOT and "
        "TILEFINCH_BOOTSTRAP_MANIFEST")
endif()

file(STRINGS "${TILEFINCH_BOOTSTRAP_MANIFEST}" manifest_lines)
set(manifest_paths)
foreach(line IN LISTS manifest_lines)
    if(line MATCHES "^[ \t]*$" OR line MATCHES "^[ \t]*#")
        continue()
    endif()
    if(NOT line MATCHES
       "^([0-9a-fA-F]+)[ \t]+(src/bootstrap/[^ \t]+|src/generated/js_bootstrap(_bytecode)?[.]c)$")
        message(FATAL_ERROR "invalid bootstrap manifest line: ${line}")
    endif()
    string(TOLOWER "${CMAKE_MATCH_1}" expected)
    string(LENGTH "${expected}" expected_length)
    if(NOT expected_length EQUAL 64)
        message(FATAL_ERROR "invalid bootstrap SHA-256: ${line}")
    endif()
    set(relative_path "${CMAKE_MATCH_2}")
    if(relative_path MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "unsafe bootstrap manifest path: ${relative_path}")
    endif()
    set(full_path "${TILEFINCH_ROOT}/${relative_path}")
    if(NOT EXISTS "${full_path}")
        message(FATAL_ERROR "bootstrap manifest input is missing: ${relative_path}")
    endif()
    file(SHA256 "${full_path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "bootstrap artifact is stale: ${relative_path}\n"
            "expected ${expected}\nactual   ${actual}\n"
            "Regenerate the bootstrap and its manifest with a host build.")
    endif()
    list(APPEND manifest_paths "${relative_path}")
endforeach()

file(GLOB authored_sources RELATIVE "${TILEFINCH_ROOT}"
    "${TILEFINCH_ROOT}/src/bootstrap/*.js")
list(APPEND authored_sources
    "src/bootstrap/sources.def"
    "src/generated/js_bootstrap.c"
    "src/generated/js_bootstrap_bytecode.c")
list(SORT authored_sources)
list(SORT manifest_paths)
if(NOT authored_sources STREQUAL manifest_paths)
    message(FATAL_ERROR
        "bootstrap manifest does not cover the complete source/artifact set\n"
        "expected: ${authored_sources}\nmanifest: ${manifest_paths}")
endif()
message(STATUS "Verified PSP bootstrap source/artifact manifest")
