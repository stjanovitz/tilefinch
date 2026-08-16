# Vendored, pinned FreeType built from source for BOTH the host and the PSP.
#
# Rationale: page fonts are hostile network input; see THIRD_PARTY_NOTICES.md.
# Building one pinned FreeType from source with an identical minimal module set
# on every target makes host and device glyph rasters bit-comparable and is a
# stronger guarantee than trusting a system-version floor.  It also removes the
# hostile-input version-gate guesswork: there is exactly one FreeType, pinned by
# URL + SHA256, on every build.
#
# The module set (sfnt + truetype + smooth, plus FreeType's internal gzip for
# WOFF1) is selected identically for both targets via the FT_CONFIG_MODULES_H
# override in cmake/freetype/ftmodule-minimal.h.
#
# Every optional external dependency is disabled EXPLICITLY so that neither
# target silently links a system library the other lacks (a determinism and
# portability hazard called out in the plan's risk list).

set(FT_DISABLE_ZLIB     ON  CACHE BOOL "" FORCE)  # internal gzip only, never system zlib
set(FT_DISABLE_BZIP2    ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON  CACHE BOOL "" FORCE)  # WOFF2 is rejected by the backend
set(FT_ENABLE_ERROR_STRINGS OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL    ON  CACHE BOOL "" FORCE)
set(DISABLE_FORCE_DEBUG_POSTFIX ON CACHE BOOL "" FORCE)  # stable `freetype` target/lib name
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)

set(PSP_BROWSER_FREETYPE_MINIMAL_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/freetype/ftmodule-minimal.h")

if(PSP_BROWSER_VENDOR_DIR AND EXISTS "${PSP_BROWSER_VENDOR_DIR}/freetype/CMakeLists.txt")
    # Offline mirror, matching how lexbor / quickjs-ng / stb / dejavu-fonts are
    # handled.  The committed FetchContent URL + hash below still makes a clean
    # checkout self-contained.
    add_subdirectory("${PSP_BROWSER_VENDOR_DIR}/freetype"
                     "${CMAKE_BINARY_DIR}/_deps/freetype-build" EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(
        freetype
        URL https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz
        URL_HASH SHA256=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(freetype)
endif()

if(TARGET freetype)
    # ftinit.c includes FT_CONFIG_MODULES_H.  Overriding it here — identically
    # for host and PSP — narrows FT_Add_Default_Modules() to the three modules
    # the backend actually needs and lets the linker drop the unreferenced
    # driver/renderer objects from every final binary.
    target_compile_definitions(freetype PRIVATE
        "FT_CONFIG_MODULES_H=\"${PSP_BROWSER_FREETYPE_MINIMAL_MODULES}\"")
    if(PSP)
        # psp-gcc rejects -fPIC under -mabi=eabi (the same accommodation the
        # lexbor static port needs); a trailing -fno-pic wins if anything up
        # the chain requests position-independent code.
        target_compile_options(freetype PRIVATE -fno-pic)
    endif()
endif()
