set(TILEFINCH_SWDEC_SOURCE_DIR "" CACHE PATH
    "Prepared swdec workspace containing the patched FFmpeg tree and PSP libraries")
option(TILEFINCH_PSP_ENABLE_SWDEC_COMPONENT
    "Build the optional H.264 High-profile software-decoder PRX" OFF)

if(NOT PSP OR NOT TILEFINCH_PSP_ENABLE_SWDEC_COMPONENT)
    return()
endif()

if(NOT TILEFINCH_SWDEC_SOURCE_DIR)
    message(FATAL_ERROR
        "TILEFINCH_PSP_ENABLE_SWDEC_COMPONENT requires TILEFINCH_SWDEC_SOURCE_DIR")
endif()

set(_swdec_ffmpeg_source "${TILEFINCH_SWDEC_SOURCE_DIR}/ffmpeg")
# A validation tree's component carries the decoder's diagnostics: logging,
# the per-access-unit stage report (swdec_me.c, SWDEC_STAGE_DIAG), fault
# injection and the measurement hooks, linked against the FFmpeg build that
# has them (libs/asm22-diag). Any other tree builds the release component,
# with none of it, against libs/asm22. See src/swdec/swdec_diag.h.
if(TILEFINCH_PSP_VALIDATION_LOG)
    set(_swdec_diagnostics 1)
    set(_swdec_ffmpeg_libraries "${TILEFINCH_SWDEC_SOURCE_DIR}/libs/asm22-diag")
else()
    set(_swdec_diagnostics 0)
    set(_swdec_ffmpeg_libraries "${TILEFINCH_SWDEC_SOURCE_DIR}/libs/asm22")
endif()
foreach(_swdec_required IN ITEMS
        "${_swdec_ffmpeg_source}/libavcodec/avcodec.h"
        "${_swdec_ffmpeg_libraries}/libavcodec.a"
        "${_swdec_ffmpeg_libraries}/libavutil.a")
    if(NOT EXISTS "${_swdec_required}")
        message(FATAL_ERROR "Missing prepared swdec input: ${_swdec_required}")
    endif()
endforeach()

set(_swdec_build "${CMAKE_CURRENT_BINARY_DIR}/swdec-component")
set(_swdec_meload_build "${CMAKE_CURRENT_BINARY_DIR}/swdec-meload")
set(_swdec_bundle "${CMAKE_CURRENT_BINARY_DIR}/tilefinch-swdec-addon")
set(_swdec_source "${CMAKE_CURRENT_SOURCE_DIR}/src/swdec")
find_program(TILEFINCH_SWDEC_MAKE_EXECUTABLE make REQUIRED)
file(MAKE_DIRECTORY "${_swdec_build}" "${_swdec_meload_build}")
file(STRINGS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/tilefinch/swdec_component.h"
    _swdec_abi_line
    REGEX "^#define TILEFINCH_SWDEC_COMPONENT_ABI_VERSION [0-9]+u$")
if(NOT _swdec_abi_line)
    message(FATAL_ERROR "Could not derive the software-decoder ABI version")
endif()
string(REGEX REPLACE ".* ([0-9]+)u$" "\\1"
    TILEFINCH_SWDEC_COMPONENT_ABI "${_swdec_abi_line}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SwdecComponentInfo.in"
    "${_swdec_build}/component-info.txt" @ONLY)

add_custom_command(
    OUTPUT "${_swdec_meload_build}/swdec-meload.prx"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_swdec_meload_build}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_swdec_source}/meload/exports.exp"
        "${_swdec_meload_build}/exports.exp"
    COMMAND ${TILEFINCH_SWDEC_MAKE_EXECUTABLE}
        -f "${_swdec_source}/meload/Makefile.psp"
        "MELOAD_SOURCE_DIR=${_swdec_source}/meload"
    WORKING_DIRECTORY "${_swdec_meload_build}"
    DEPENDS
        "${_swdec_source}/meload/main.c"
        "${_swdec_source}/meload/mestub.S"
        "${_swdec_source}/meload/exports.exp"
        "${_swdec_source}/meload/Makefile.psp"
    COMMENT "Building the resident swdec Media Engine helper"
    VERBATIM)

add_custom_target(tilefinch-swdec-meload
    DEPENDS "${_swdec_meload_build}/swdec-meload.prx")

set(_swdec_component_sources
    "${_swdec_source}/swdec_component_psp.c"
    "${_swdec_source}/swdec.c"
    "${_swdec_source}/swdec.h"
    "${_swdec_source}/swdec_bounds.h"
    "${_swdec_source}/swdec_diag.h"
    "${_swdec_source}/swdec_csc_mask.h"
    "${_swdec_source}/swdec_csc.c"
    "${_swdec_source}/swdec_csc.h"
    "${_swdec_source}/swdec_arena.c"
    "${_swdec_source}/swdec_arena.h"
    "${_swdec_source}/swdec_me.c"
    "${_swdec_source}/swdec_me.h"
    "${_swdec_source}/swdec_recon_diag.c"
    "${_swdec_source}/swdec_recon_diag.h"
    "${_swdec_source}/swdec_chroma_replay.c"
    "${_swdec_source}/swdec_chroma_replay.h"
    "${_swdec_source}/swdec_meload.S"
    "${_swdec_source}/Makefile.psp"
    "${_swdec_source}/exports.exp"
)

add_custom_command(
    OUTPUT "${_swdec_build}/tilefinch-swdec.prx"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_swdec_build}"
    COMMAND ${TILEFINCH_SWDEC_MAKE_EXECUTABLE}
        -f "${_swdec_source}/Makefile.psp"
        "SWDEC_SOURCE_DIR=${_swdec_source}"
        "TILEFINCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "FFMPEG_SOURCE_DIR=${_swdec_ffmpeg_source}"
        "FFMPEG_LIBRARY_DIR=${_swdec_ffmpeg_libraries}"
        "SWDEC_DIAGNOSTICS=${_swdec_diagnostics}"
    WORKING_DIRECTORY "${_swdec_build}"
    DEPENDS
        tilefinch-swdec-meload
        ${_swdec_component_sources}
        "${_swdec_ffmpeg_libraries}/libavcodec.a"
        "${_swdec_ffmpeg_libraries}/libavutil.a"
    COMMENT "Building the optional swdec High-profile decoder component"
    VERBATIM)

add_custom_target(tilefinch-swdec-component
    DEPENDS "${_swdec_build}/tilefinch-swdec.prx")

# The official browser package deliberately never contains this component.
# An opt-in build emits a separate directory which the user copies to the
# installation-wide components/swdec directory; A/B app updates then leave it
# in place until the ABI marker says a rebuild is necessary.
add_custom_command(
    OUTPUT "${_swdec_bundle}/component-info.txt"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_swdec_bundle}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_swdec_bundle}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_swdec_build}/tilefinch-swdec.prx"
        "${_swdec_bundle}/tilefinch-swdec.prx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_swdec_meload_build}/swdec-meload.prx"
        "${_swdec_bundle}/swdec-meload.prx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_swdec_build}/component-info.txt"
        "${_swdec_bundle}/component-info.txt"
    DEPENDS tilefinch-swdec-component
        "${_swdec_build}/component-info.txt"
        "${_swdec_build}/tilefinch-swdec.prx"
        "${_swdec_meload_build}/swdec-meload.prx"
    COMMENT "Staging the user-installed software-decoder add-on"
    VERBATIM)
add_custom_target(tilefinch-swdec-bundle
    DEPENDS "${_swdec_bundle}/component-info.txt")

# Validation trees also build the release component (no diagnostics,
# libs/asm22) beside the diagnostic one, as tilefinch-swdec-addon-release:
# performance acceptance runs use it with the validation browser, whose
# steady-window lines need nothing from the component, and profiling runs
# use the diagnostic add-on. Same sources, same ABI marker.
if(TILEFINCH_PSP_VALIDATION_LOG)
    set(_swdec_release_build "${CMAKE_CURRENT_BINARY_DIR}/swdec-component-release")
    set(_swdec_release_bundle "${CMAKE_CURRENT_BINARY_DIR}/tilefinch-swdec-addon-release")
    set(_swdec_release_libraries "${TILEFINCH_SWDEC_SOURCE_DIR}/libs/asm22")
    file(MAKE_DIRECTORY "${_swdec_release_build}")
    add_custom_command(
        OUTPUT "${_swdec_release_build}/tilefinch-swdec.prx"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_swdec_release_build}"
        COMMAND ${TILEFINCH_SWDEC_MAKE_EXECUTABLE}
            -f "${_swdec_source}/Makefile.psp"
            "SWDEC_SOURCE_DIR=${_swdec_source}"
            "TILEFINCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "FFMPEG_SOURCE_DIR=${_swdec_ffmpeg_source}"
            "FFMPEG_LIBRARY_DIR=${_swdec_release_libraries}"
            "SWDEC_DIAGNOSTICS=0"
        WORKING_DIRECTORY "${_swdec_release_build}"
        DEPENDS
            tilefinch-swdec-meload
            ${_swdec_component_sources}
            "${_swdec_release_libraries}/libavcodec.a"
            "${_swdec_release_libraries}/libavutil.a"
        COMMENT "Building the release swdec component for acceptance runs"
        VERBATIM)
    add_custom_command(
        OUTPUT "${_swdec_release_bundle}/component-info.txt"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_swdec_release_bundle}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_swdec_release_bundle}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_swdec_release_build}/tilefinch-swdec.prx"
            "${_swdec_release_bundle}/tilefinch-swdec.prx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_swdec_meload_build}/swdec-meload.prx"
            "${_swdec_release_bundle}/swdec-meload.prx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_swdec_build}/component-info.txt"
            "${_swdec_release_bundle}/component-info.txt"
        DEPENDS
            "${_swdec_release_build}/tilefinch-swdec.prx"
            "${_swdec_build}/component-info.txt"
            "${_swdec_meload_build}/swdec-meload.prx"
        COMMENT "Staging the release software-decoder add-on for acceptance runs"
        VERBATIM)
    add_custom_target(tilefinch-swdec-release-bundle
        DEPENDS "${_swdec_release_bundle}/component-info.txt")
endif()

# This is intentionally an ordinary EBOOT rather than a PSPLink-only PRX.
# It exercises the same user-EBOOT -> user PRX -> kernel-helper boundary as
# the browser, without requiring PSPLink or starting the full application.
add_executable(psp-swdec-loader-probe
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psp_swdec_loader_probe.c")
target_include_directories(psp-swdec-loader-probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(psp-swdec-loader-probe PRIVATE
    pspdebug pspdisplay pspge)
set_target_properties(psp-swdec-loader-probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/swdec-probe")
create_pbp_file(TARGET psp-swdec-loader-probe
    TITLE "Tilefinch swdec loader probe")
add_dependencies(psp-swdec-loader-probe tilefinch-swdec-component)
add_custom_command(TARGET psp-swdec-loader-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_swdec_build}/tilefinch-swdec.prx"
        "$<TARGET_FILE_DIR:psp-swdec-loader-probe>/tilefinch-swdec.prx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_swdec_meload_build}/swdec-meload.prx"
        "$<TARGET_FILE_DIR:psp-swdec-loader-probe>/swdec-meload.prx"
    COMMENT "Staging swdec probe components beside its EBOOT")

set(TILEFINCH_SWDEC_COMPONENT_PRX
    "${_swdec_build}/tilefinch-swdec.prx" CACHE INTERNAL "")
set(TILEFINCH_SWDEC_MELOAD_PRX
    "${_swdec_meload_build}/swdec-meload.prx" CACHE INTERNAL "")
