if(NOT DEFINED TILEFINCH_ROOT OR NOT DEFINED TILEFINCH_BOOTSTRAP_MANIFEST)
    message(FATAL_ERROR
        "WriteBootstrapManifest requires TILEFINCH_ROOT and "
        "TILEFINCH_BOOTSTRAP_MANIFEST")
endif()

file(GLOB bootstrap_paths RELATIVE "${TILEFINCH_ROOT}"
    "${TILEFINCH_ROOT}/src/bootstrap/*.js")
list(APPEND bootstrap_paths
    "src/bootstrap/sources.def"
    "src/generated/js_bootstrap.c"
    "src/generated/js_bootstrap_bytecode.c")
list(SORT bootstrap_paths)

string(CONCAT manifest
    "# Authored bootstrap inputs and generated C artifacts. This file is\n"
    "# derived by regenerate_tilefinch_bootstrap; do not refresh it by hand.\n")
foreach(relative_path IN LISTS bootstrap_paths)
    set(full_path "${TILEFINCH_ROOT}/${relative_path}")
    if(NOT EXISTS "${full_path}")
        message(FATAL_ERROR
            "bootstrap manifest input is missing: ${relative_path}")
    endif()
    file(SHA256 "${full_path}" digest)
    string(APPEND manifest "${digest}  ${relative_path}\n")
endforeach()
file(WRITE "${TILEFINCH_BOOTSTRAP_MANIFEST}" "${manifest}")
