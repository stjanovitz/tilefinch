# Vendored, pinned FreeType built from source for BOTH the host and the PSP.
#
# Rationale: page fonts are hostile network input; see THIRD_PARTY_NOTICES.md.
# Building one pinned FreeType from source with an identical minimal module set
# on every target makes host and device glyph rasters bit-comparable and is a
# stronger guarantee than trusting a system-version floor.  It also removes the
# hostile-input version-gate guesswork: there is exactly one FreeType, pinned by
# URL + SHA256, on every build.
#
# The module set (sfnt + truetype + smooth, with WOFF/WOFF2 support) is selected
# identically for both targets via the FT_CONFIG_MODULES_H
# override in cmake/freetype/ftmodule-minimal.h. WOFF1 uses the same zlib
# already required by the browser, rather than FreeType's private inflater.
# WOFF2 uses the pinned decode-only Brotli target and capped FreeType allocator.
#
# Every other optional external dependency is disabled EXPLICITLY so that
# neither target silently links a system library the other lacks. zlib is
# required, never opportunistic: both targets already use it in the core.

set(FT_DISABLE_ZLIB     OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_ZLIB     ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON  CACHE BOOL "" FORCE) # Never discover a system decoder.
include(cmake/brotli_vendored.cmake)
set(FT_ENABLE_ERROR_STRINGS OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL    ON  CACHE BOOL "" FORCE)
set(DISABLE_FORCE_DEBUG_POSTFIX ON CACHE BOOL "" FORCE)  # stable `freetype` target/lib name
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)

set(PSP_BROWSER_FREETYPE_MINIMAL_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/freetype/ftmodule-minimal.h")
set(PSP_BROWSER_FREETYPE_OPTIONS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/freetype/ftoption-tilefinch.h")

if(PSP_BROWSER_VENDOR_DIR AND EXISTS "${PSP_BROWSER_VENDOR_DIR}/freetype/CMakeLists.txt")
    # Offline mirror, matching how lexbor / quickjs-ng / stb / dejavu-fonts are
    # handled.  The committed FetchContent URL + hash below still makes a clean
    # checkout self-contained.
    add_subdirectory("${PSP_BROWSER_VENDOR_DIR}/freetype"
                     "${CMAKE_BINARY_DIR}/_deps/freetype-build" EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(
        freetype
        URL
            https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz
            https://sourceforge.net/projects/freetype/files/freetype2/2.14.3/freetype-2.14.3.tar.xz/download
        URL_HASH SHA256=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(freetype)
endif()

# Run on existing mirrors/build trees too; a changed patch must not depend
# on whether FetchContent happens to populate this configure.
if(PSP_BROWSER_VENDOR_DIR AND EXISTS "${PSP_BROWSER_VENDOR_DIR}/freetype/CMakeLists.txt")
    set(tilefinch_freetype_source "${PSP_BROWSER_VENDOR_DIR}/freetype")
else()
    set(tilefinch_freetype_source "${freetype_SOURCE_DIR}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DPATCH_SOURCE_DIR=${tilefinch_freetype_source}"
    "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/freetype-2.14.3-budgeted-woff2.patch"
    "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake"
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DPATCH_SOURCE_DIR=${tilefinch_freetype_source}"
    "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/freetype-2.14.3-compact-sfnt.patch"
    "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake"
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DPATCH_SOURCE_DIR=${tilefinch_freetype_source}"
    "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/freetype-2.14.3-partial-variation-cleanup.patch"
    "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake"
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DPATCH_SOURCE_DIR=${tilefinch_freetype_source}"
    "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/freetype-2.14.3-variation-retry.patch"
    "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake"
    COMMAND_ERROR_IS_FATAL ANY)

if(TARGET freetype)
    # ftinit.c includes FT_CONFIG_MODULES_H.  Overriding it here — identically
    # for host and PSP — narrows FT_Add_Default_Modules() to the three modules
    # the backend actually needs and lets the linker drop the unreferenced
    # driver/renderer objects from every final binary.
    target_compile_definitions(freetype PRIVATE
        "FT_CONFIG_MODULES_H=\"${PSP_BROWSER_FREETYPE_MINIMAL_MODULES}\""
        "FT_CONFIG_OPTIONS_H=\"${PSP_BROWSER_FREETYPE_OPTIONS}\"")
    target_link_libraries(freetype PUBLIC tilefinch_brotli_decode)
    if(PSP)
        # psp-gcc rejects -fPIC under -mabi=eabi (the same accommodation the
        # lexbor static port needs); a trailing -fno-pic wins if anything up
        # the chain requests position-independent code.
        target_compile_options(freetype PRIVATE -fno-pic)
    endif()
endif()
