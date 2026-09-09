cmake_minimum_required(VERSION 3.21)

project(psp_browser_tilefinch VERSION 0.1.17 LANGUAGES C)

# Build directories inside the source tree must be named in CMakePresets.json
# or in the short allowlist below. Sixty-one ad hoc trees (12 GB) accumulated
# in two months, and one of them ran stale binaries under ctest while
# reporting green. The check runs only on a fresh configure (no generated
# build files yet), so existing trees keep working; pass
# -DTILEFINCH_ALLOW_BUILD_DIR=ON for a deliberate experiment.
option(TILEFINCH_ALLOW_BUILD_DIR
    "Admit a build directory that CMakePresets.json does not name" OFF)
if(NOT TILEFINCH_ALLOW_BUILD_DIR
   AND NOT EXISTS "${CMAKE_BINARY_DIR}/Makefile"
   AND NOT EXISTS "${CMAKE_BINARY_DIR}/build.ninja")
    file(RELATIVE_PATH tilefinch_build_relative
         "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
    if(NOT tilefinch_build_relative MATCHES "^\.\./"
       AND NOT IS_ABSOLUTE "${tilefinch_build_relative}")
        file(READ "${CMAKE_SOURCE_DIR}/CMakePresets.json" tilefinch_presets)
        string(REGEX MATCHALL "\"binaryDir\": \"[^\"]*\""
               tilefinch_preset_dirs "${tilefinch_presets}")
        set(tilefinch_allowed_build_dirs
            # Device validation and update/e2e trees the scripts default to.
            build-preset-psp-validation
            build-psp-update-proof
            build-preset-psp-update-e2e
            # scripts/dev.sh and the lab scripts' default trees.
            build-dev
            build-release
            build-dev-sanitize
            build-bellard-current
            build-bellard-clean-impersonate
            # Hosts the fetched WAMR source the other trees reference.
            build-challenge-compression)
        foreach(entry IN LISTS tilefinch_preset_dirs)
            string(REGEX REPLACE ".*/([^/\"]+)\"$" "\\1" entry "${entry}")
            list(APPEND tilefinch_allowed_build_dirs "${entry}")
        endforeach()
        get_filename_component(tilefinch_build_name "${CMAKE_BINARY_DIR}" NAME)
        if(NOT tilefinch_build_name IN_LIST tilefinch_allowed_build_dirs)
            message(FATAL_ERROR
                "Build directory '${tilefinch_build_name}' is not a preset "
                "binaryDir (see CMakePresets.json). Configure through a preset, "
                "or pass -DTILEFINCH_ALLOW_BUILD_DIR=ON for a deliberate "
                "experiment and delete the tree when it is done.")
        endif()
    endif()
endif()

set(TILEFINCH_RELEASE_SEQUENCE "18" CACHE STRING
    "Monotonic signed release sequence compiled into this build")
set(TILEFINCH_UPDATE_ROOT_V1 "" CACHE FILEPATH
    "Public binary Tilefinch root-v1 metadata embedded into updater builds")
set(TILEFINCH_UPDATE_REPOSITORY_OWNER "stjanovitz" CACHE STRING
    "Public GitHub owner from which signed Tilefinch releases are fetched")
set(TILEFINCH_UPDATE_REPOSITORY_NAME "tilefinch" CACHE STRING
    "Public GitHub repository from which signed Tilefinch releases are fetched")
set(TILEFINCH_VOICE_COMPONENT_REPOSITORY_OWNER
    "${TILEFINCH_UPDATE_REPOSITORY_OWNER}" CACHE STRING
    "Public GitHub owner from which signed voice components are fetched")
set(TILEFINCH_VOICE_COMPONENT_REPOSITORY_NAME "tilefinch-models" CACHE STRING
    "Public GitHub repository from which signed voice components are fetched")
set(TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER
    "${TILEFINCH_UPDATE_REPOSITORY_OWNER}" CACHE STRING
    "Public GitHub owner from which signed glyph packs are fetched")
set(TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME "tilefinch-models" CACHE STRING
    "Public GitHub repository from which signed glyph packs are fetched")
if(NOT TILEFINCH_RELEASE_SEQUENCE MATCHES "^[0-9]+$")
    message(FATAL_ERROR "TILEFINCH_RELEASE_SEQUENCE must be an unsigned integer")
endif()
foreach(update_repository_part IN ITEMS
        TILEFINCH_UPDATE_REPOSITORY_OWNER
        TILEFINCH_UPDATE_REPOSITORY_NAME
        TILEFINCH_VOICE_COMPONENT_REPOSITORY_OWNER
        TILEFINCH_VOICE_COMPONENT_REPOSITORY_NAME
        TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER
        TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME)
    string(LENGTH "${${update_repository_part}}" update_repository_part_length)
    if(update_repository_part_length GREATER 63
       OR NOT "${${update_repository_part}}" MATCHES
              "^[A-Za-z0-9][A-Za-z0-9_.-]*$")
        message(FATAL_ERROR
            "${update_repository_part} is not a safe GitHub path component")
    endif()
endforeach()
configure_file(
    include/tilefinch/build_version.h.in
    generated/tilefinch/build_version.h @ONLY)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(TILEFINCH_UPDATE_ROOT_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/tilefinch_update_root_data.h")
if(TILEFINCH_UPDATE_ROOT_V1)
    if(NOT EXISTS "${TILEFINCH_UPDATE_ROOT_V1}")
        message(FATAL_ERROR
            "TILEFINCH_UPDATE_ROOT_V1 does not exist: ${TILEFINCH_UPDATE_ROOT_V1}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${TILEFINCH_UPDATE_ROOT_V1}")
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${TILEFINCH_UPDATE_ROOT_V1}"
            "-DOUTPUT=${TILEFINCH_UPDATE_ROOT_HEADER}"
            -DNAME=tilefinch_update_root_v1
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake"
        COMMAND_ERROR_IS_FATAL ANY)
    message(STATUS
        "Secure updater: embedded public root ${TILEFINCH_UPDATE_ROOT_V1}")
else()
    file(WRITE "${TILEFINCH_UPDATE_ROOT_HEADER}"
"/* No release trust root was configured for this build. */
#include <stddef.h>
static const unsigned char tilefinch_update_root_v1[1] = {0};
static const size_t tilefinch_update_root_v1_length = 0;
")
    message(STATUS
        "Secure updater: disabled until TILEFINCH_UPDATE_ROOT_V1 is configured")
endif()


# The build graph is split by ownership. These modules intentionally share
# directory scope so existing cache options and target variables remain stable.
include(cmake/TilefinchDependencies.cmake)
include(cmake/TilefinchCore.cmake)
include(cmake/TilefinchTargets.cmake)
include(cmake/PspSwdec.cmake)
include(cmake/TilefinchTests.cmake)
