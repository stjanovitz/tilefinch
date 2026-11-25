# Refuse a QuickJS PGO profile trained in another checkout.
#
# scripts/train-quickjs-pgo.sh records the instrumented build's source
# directory as source-dir= in the profile's PROVENANCE. GCC's per-function
# profile checks hash each engine source's path as the compiler is given it
# (no prefix map changes that), so the profile applies only to builds
# configured from that exact path: another checkout, or the same folder
# reached through a symlink, fails the engine compile with
# -Werror=coverage-mismatch and missing-profile errors although the
# fingerprint (which leaves the checkout out) matches. A profile trained
# before PROVENANCE recorded the folder is accepted with a warning.
#
# TilefinchDependencies.cmake includes this file and calls the function;
# tests run it as a script:
#   cmake -DPROFILE_DIR=DIR -DSOURCE_DIR=DIR -P CheckQuickjsPgoSourceDir.cmake
function(tilefinch_check_quickjs_pgo_source_dir profile_dir source_dir)
    file(STRINGS "${profile_dir}/PROVENANCE" trained REGEX "^source-dir=")
    if(NOT trained)
        message(WARNING "The QuickJS profile in ${profile_dir} does not "
            "record the checkout it was trained in (no source-dir in its "
            "PROVENANCE), so configure cannot check that it was trained in "
            "this one, ${source_dir}. A profile trained elsewhere fails the "
            "engine compile with coverage-mismatch and missing-profile "
            "errors; retraining with scripts/train-quickjs-pgo.sh records "
            "the folder.")
        return()
    endif()
    list(GET trained 0 trained)
    string(REGEX REPLACE "^source-dir=" "" trained "${trained}")
    if(trained STREQUAL source_dir)
        return()
    endif()
    file(REAL_PATH "${trained}" trained_real)
    file(REAL_PATH "${source_dir}" source_real)
    if(trained_real STREQUAL source_real)
        message(FATAL_ERROR "The QuickJS profile in ${profile_dir} was "
            "trained in ${trained}; this build is configured from "
            "${source_dir}, the same folder by another path. GCC's profile "
            "checks use each engine source's path as the compiler is given "
            "it, so the engine compile would fail with coverage-mismatch "
            "and missing-profile errors. Configure from ${trained}, or "
            "train a profile from ${source_dir} with "
            "scripts/train-quickjs-pgo.sh.")
    endif()
    message(FATAL_ERROR "The QuickJS profile in ${profile_dir} was trained "
        "in the checkout ${trained}; this build is configured from "
        "${source_dir}. GCC's profile checks include each engine source's "
        "absolute path, so the engine compile would fail with "
        "coverage-mismatch and missing-profile errors. Train a profile in "
        "this checkout with scripts/train-quickjs-pgo.sh, or build from "
        "${trained}.")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    tilefinch_check_quickjs_pgo_source_dir("${PROFILE_DIR}" "${SOURCE_DIR}")
endif()
