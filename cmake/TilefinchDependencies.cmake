include(FetchContent)
find_program(PATCH_EXECUTABLE patch REQUIRED)

# A build tree can outlive dependency sources in another, pruned build tree.
# FetchContent and several dependencies cache both source overrides and paths
# derived from them. Drop only missing, project-local _deps paths; normal
# cache values and deliberate external overrides remain untouched.
function(tilefinch_forget_missing_dependency_cache_paths)
    get_cmake_property(cache_names CACHE_VARIABLES)
    foreach(cache_name IN LISTS cache_names)
        set(cache_value "${${cache_name}}")
        if(IS_ABSOLUTE "${cache_value}")
            string(FIND "${cache_value}"
                "${CMAKE_CURRENT_SOURCE_DIR}/" project_prefix)
            string(REGEX MATCH "^.*/_deps/[^/]+-src"
                dependency_source_root "${cache_value}")
            if(project_prefix EQUAL 0
               AND NOT "${dependency_source_root}" STREQUAL ""
               AND NOT EXISTS "${dependency_source_root}")
                message(WARNING
                    "Ignoring stale ${cache_name}=${cache_value}")
                unset(${cache_name} CACHE)
            endif()
        endif()
    endforeach()
endfunction()

tilefinch_forget_missing_dependency_cache_paths()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Host tools and tests share one engine image.  Building dependencies as PIC
# lets that image be a shared library on ELF platforms as well as macOS, so an
# engine-only edit does not force every small test executable to relink.  PSP
# remains entirely static and pays neither the code-size nor runtime cost.
if(NOT PSP)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(PSP)
    # Most engine sources intentionally contain several cohesive internal
    # functions in one translation unit. Let the PSP linker discard functions
    # and constants that are unreachable from the EBOOT without changing
    # source organization or runtime behavior. PSP ABI metadata is rooted by
    # the augmenting script because psp-fixup-imports discovers it after link.
    set(PSP_BROWSER_GC_KEEP_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PspGcKeep.ld")
    # Keep compiler-expanded source locations deterministic and free of the
    # builder's home-directory layout. This covers Tilefinch plus dependencies
    # added through this CMake tree; the separately configured owned transport
    # repeats the same mapping in PspOwnedTransport.cmake.
    set(TILEFINCH_PSP_PREFIX_MAP_FLAGS
        "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=/tilefinch/source"
        "-fdebug-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=/tilefinch/source"
        "-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=/tilefinch/source"
        "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=/tilefinch/build"
        "-fdebug-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=/tilefinch/build"
        "-fmacro-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=/tilefinch/build")
    if(DEFINED ENV{PSPDEV} AND NOT "$ENV{PSPDEV}" STREQUAL "")
        list(APPEND TILEFINCH_PSP_PREFIX_MAP_FLAGS
            "-ffile-prefix-map=$ENV{PSPDEV}=/pspdev"
            "-fdebug-prefix-map=$ENV{PSPDEV}=/pspdev"
            "-fmacro-prefix-map=$ENV{PSPDEV}=/pspdev")
    endif()
    add_compile_options(
        -ffunction-sections -fdata-sections
        ${TILEFINCH_PSP_PREFIX_MAP_FLAGS})
    add_link_options(
        "LINKER:--gc-sections"
        "LINKER:-T,${PSP_BROWSER_GC_KEEP_SCRIPT}")
endif()

option(PSP_BROWSER_BUILD_TESTS "Build the Tilefinch test suite" ON)
option(PSP_BROWSER_BUILD_HOSTILE_PARSER_HARNESS
       "Build the deterministic sanitizer-oriented parser harness" OFF)
option(PSP_BROWSER_BUILD_JSC_SPIKE
       "Build the macOS-only JavaScriptCore diagnostic runner" ON)
option(PSP_BROWSER_BUILD_HOST_MEDIA_LAB
       "Build the FFmpeg-backed host-only video playback lab" OFF)
option(PSP_BROWSER_ENABLE_HOST_MEDIA_AUDIO
       "Enable SDL audio output in the host media lab" ON)
option(PSP_BROWSER_DISABLE_TRACE
    "Compile out TILEFINCH_TRACE_* diagnostics (PSP profile)" OFF)
if(PSP)
    set(TILEFINCH_BOOTSTRAP_SOURCE_FALLBACK_DEFAULT OFF)
else()
    set(TILEFINCH_BOOTSTRAP_SOURCE_FALLBACK_DEFAULT ON)
endif()
option(PSP_BROWSER_EMBED_BOOTSTRAP_SOURCE_FALLBACK
    "Embed authored JavaScript bootstrap source for bytecode restore fallback"
    ${TILEFINCH_BOOTSTRAP_SOURCE_FALLBACK_DEFAULT})
option(PSP_BROWSER_ENABLE_LTO
    "Build the engine and frontends with link-time optimization" OFF)
option(PSP_BROWSER_ENABLE_GIF
       "Enable bounded first-frame GIF decoding" ON)
option(PSP_BROWSER_ENABLE_WEB_FONTS
       "Enable bounded page-provided WOFF1 fonts through FreeType" ON)
option(PSP_BROWSER_ENABLE_HOST_WEBASSEMBLY
       "Enable the bounded WAMR-backed WebAssembly interpreter in host labs" ON)
option(PSP_BROWSER_ENABLE_PSP_VOICE
       "Build offline PocketSphinx push-to-talk input into the PSP EBOOT" ON)
option(PSP_BROWSER_SYSTEM_FREETYPE
       "Use a system FreeType (>= version floor) instead of the pinned vendored build" OFF)
option(PSP_BROWSER_USE_COMPILER_CACHE
       "Use ccache or sccache when one is available" ON)
option(PSP_BROWSER_USE_BELLARD_QUICKJS
       "Use pinned upstream QuickJS instead of QuickJS-NG" OFF)
option(TILEFINCH_PROFILE_LAYOUT_FLOW
    "Enable intrusive exclusive layout-flow timers in PSP validation builds" OFF)
# The unlocked Budget ledger has exactly one owning thread. Host test builds
# assert that rule on every ledger mutation. The PSP is where the worker
# threads actually exist, so a PSP build may opt in for a PPSSPP or device
# qualification run; it stays off by default there because a thread-identity
# syscall per allocation distorts the timing validation builds measure.
if(PSP_BROWSER_BUILD_TESTS AND NOT PSP)
    set(tilefinch_owner_checks_default ON)
else()
    set(tilefinch_owner_checks_default OFF)
endif()
option(TILEFINCH_OWNER_CHECKS
       "Report (and on the host abort) when a Budget is mutated by a thread that does not own it"
       ${tilefinch_owner_checks_default})
option(TILEFINCH_PSP_VALIDATION_LOG
       "Enable PSP stdout validation, Memory Stick logs/crash journal, and the logging watchdog (slow; intended only for diagnostic builds)" OFF)
option(TILEFINCH_PSP_MEDIA_PICTURE_TRACE
       "Emit per-picture PSP media traces in validation builds (large; aggregate telemetry remains available when disabled)" OFF)
option(TILEFINCH_PSP_COMPILER_HARDENING
       "Build PSP C sources with stack-protector-strong and newlib fortification (candidate until device-qualified)" OFF)
if(TILEFINCH_PSP_COMPILER_HARDENING)
    if(NOT PSP)
        message(FATAL_ERROR
            "TILEFINCH_PSP_COMPILER_HARDENING is meaningful only for PSP builds")
    endif()

    include(CheckCCompilerFlag)
    check_c_compiler_flag(-fstack-protector-strong
        TILEFINCH_PSP_HAS_STACK_PROTECTOR_STRONG)
    if(NOT TILEFINCH_PSP_HAS_STACK_PROTECTOR_STRONG)
        message(FATAL_ERROR
            "The selected PSP compiler does not support -fstack-protector-strong")
    endif()

    include(CheckCSourceCompiles)
    set(_tilefinch_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS
        "${CMAKE_REQUIRED_FLAGS} -O2 -D_FORTIFY_SOURCE=2")
    check_c_source_compiles(
        "#include <string.h>
         #if !defined(__SSP_FORTIFY_LEVEL) || __SSP_FORTIFY_LEVEL < 2
         #error newlib fortification level 2 is unavailable
         #endif
         int main(void) { char out[8]; return (int) sizeof(strcpy(out, \"ok\")); }"
        TILEFINCH_PSP_HAS_FORTIFY_LEVEL_2)
    set(CMAKE_REQUIRED_FLAGS "${_tilefinch_saved_required_flags}")
    unset(_tilefinch_saved_required_flags)
    if(NOT TILEFINCH_PSP_HAS_FORTIFY_LEVEL_2)
        message(FATAL_ERROR
            "The selected PSP libc/compiler cannot provide _FORTIFY_SOURCE=2")
    endif()

    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:-fstack-protector-strong>")
    add_compile_definitions(_FORTIFY_SOURCE=2)
    message(STATUS
        "PSP compiler-hardening candidate: stack-protector-strong + fortify level 2")
endif()
if(PSP)
    set(TILEFINCH_STRIP_RELEASE_EBOOT_DEFAULT ON)
else()
    set(TILEFINCH_STRIP_RELEASE_EBOOT_DEFAULT OFF)
endif()
option(TILEFINCH_STRIP_RELEASE_EBOOT
       "Strip the packaged PSP EBOOT while retaining an unstripped ELF sidecar"
       ${TILEFINCH_STRIP_RELEASE_EBOOT_DEFAULT})
option(PSP_BROWSER_CURL_STUB
       "On PSP, satisfy fetch.c's curl references with a no-op stub instead of linking the SDK libcurl stack (hermetic replay only)" OFF)
if(PSP AND NOT TILEFINCH_PSP_VALIDATION_LOG AND NOT PSP_BROWSER_CURL_STUB)
    set(tilefinch_fetch_trace_default OFF)
else()
    set(tilefinch_fetch_trace_default ON)
endif()
option(PSP_BROWSER_ENABLE_FETCH_TRACE
    "Include HTTP capture/replay (host labs, validation and hermetic replay builds)"
    ${tilefinch_fetch_trace_default})
include(cmake/PspOwnedTransport.cmake)
set(PSP_BROWSER_PSP_CA_BUNDLE
    "${CMAKE_CURRENT_SOURCE_DIR}/certs/roots.pem" CACHE FILEPATH
    "PEM trust bundle staged beside a real-network PSP browser EBOOT")
option(PSP_BROWSER_JS_EXECUTION_PROFILE
       "Collect bounded QuickJS opcode, function, and PC hot spots" OFF)
option(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE
       "Trace bounded null/undefined QuickJS property reads (lab diagnostics only)" OFF)
option(PSP_BROWSER_JS_CALL_COUNTS
       "Count calls per QuickJS function for the JS profiler's calls table (dev builds)" OFF)
option(PSP_BROWSER_JS_OP_COUNTS
       "Count QuickJS calls, dispatched opcodes, and float64 values for the tilefinch-work record (count builds: never time them)" OFF)
option(PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH
       "Inline trivial Bellard QuickJS getters that return one captured value" ON)
option(PSP_BROWSER_QUICKJS_FUNCTION_RECYCLE
       "Recycle bounded Bellard QuickJS function-object storage" ON)
option(PSP_BROWSER_QUICKJS_PORTABLE_REGION
       "Enable the bounded architecture-neutral QuickJS region tier" ON)
option(PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY
       "Pack dense arrays of one-character Latin-1 strings" ON)
option(PSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH
       "Apply the experimental Bellard QuickJS VM patch" OFF)
option(PSP_BROWSER_QUICKJS_NATIVE_TRACE
       "Build the experimental GNU lightning QuickJS native trace tier" OFF)
set(PSP_BROWSER_LIGHTNING_ROOT "" CACHE PATH
    "GNU lightning installation prefix for the experimental native trace tier")
set(PSP_BROWSER_VENDOR_DIR "" CACHE PATH "Optional directory containing lexbor/ and quickjs-ng/")
set(PSP_BROWSER_WAMR_SOURCE_DIR "" CACHE PATH
    "Optional prepared WAMR 2.4.5 source tree for bounded WebAssembly")
set(PSP_BROWSER_PGO_GENERATE "" CACHE STRING
    "Clang raw-profile filename pattern used to train the allocator benchmark")
set(PSP_BROWSER_PGO_USE "" CACHE FILEPATH
    "Clang indexed profile used to optimize the allocator benchmark")
set(PSP_BROWSER_CURL_IMPERSONATE_LIBRARY "" CACHE FILEPATH
    "Optional lab-only libcurl-impersonate library")
set(PSP_BROWSER_CURL_IMPERSONATE_TARGET "safari184_ios" CACHE STRING
    "curl-impersonate browser transport profile")
set(PSP_BROWSER_TRANSPORT_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fetch.c" CACHE FILEPATH
    "Transport backend implementing the bounded tilefinch/fetch.h contract")
set(PSP_BROWSER_QUICKJS_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/quickjs-ng-v0.15.0-closure-shape-cache.patch")
set(PSP_BROWSER_LEXBOR_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/lexbor-v3.0.0-partial-document-destroy.patch")
set(PSP_BROWSER_LEXBOR_FOSTER_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/lexbor-v3.0.0-foster-parent-insertion-events.patch")
set(PSP_BROWSER_LEXBOR_TOKENIZER_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/lexbor-v3.0.0-compact-tokenizer-storage.patch")
set(PSP_BROWSER_LEXBOR_MRAW_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/lexbor-v3.0.0-mraw-shrink-offset.patch")
set(PSP_BROWSER_STB_JPEG_SCALE_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/stb-31c1ad37-jpeg-scaled-idct.patch")
set(PSP_BROWSER_NANOSVG_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/nanosvg-239e102-bounded-fixed-edges.patch")
set(PSP_BROWSER_NANOSVG_GEOMETRY_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/nanosvg-239e102-hostile-geometry.patch")
set(PSP_BROWSER_NANOSVG_DIVISIONS_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/nanosvg-239e102-bounded-curve-divisions.patch")
set(PSP_BROWSER_POCKETSPHINX_SOURCE_DIR "" CACHE PATH
    "Optional prepared PocketSphinx 5.1.1 source tree for PSP voice input")
option(PSP_BROWSER_PACKED_VOICE_LEXICON
    "Pack immutable PSP voice dictionary storage into bounded arenas" ON)
option(PSP_BROWSER_COMPACT_FIXED_VOICE
    "Use exact streamed/precompiled structures for fixed PSP voice models" ON)
set(TILEFINCH_VOICE_SENDUMP_ROWS 384)
set(TILEFINCH_VOICE_SENDUMP_ROW_BYTES 5126)
set(PSP_BROWSER_APPLY_PATCH_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PSP_BROWSER_QUICKJS_PATCH}"
    "${PSP_BROWSER_LEXBOR_PATCH}"
    "${PSP_BROWSER_LEXBOR_FOSTER_PATCH}"
    "${PSP_BROWSER_LEXBOR_TOKENIZER_PATCH}"
    "${PSP_BROWSER_LEXBOR_MRAW_PATCH}"
    "${PSP_BROWSER_STB_JPEG_SCALE_PATCH}"
    "${PSP_BROWSER_NANOSVG_PATCH}"
    "${PSP_BROWSER_NANOSVG_GEOMETRY_PATCH}"
    "${PSP_BROWSER_NANOSVG_DIVISIONS_PATCH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/pocketsphinx/pocketsphinx-5.1.1-psp-int32.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/pocketsphinx/pocketsphinx-5.1.1-psp-no-mmap.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/pocketsphinx/pocketsphinx-5.1.1-psp-timer.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/pocketsphinx/pocketsphinx-5.1.1-packed-lexicon.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/pocketsphinx/pocketsphinx-5.1.1-fixed-recognizer.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/pocketsphinx/pocketsphinx-5.1.1-stream-sendump.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/pocketsphinx/pocketsphinx-5.1.1-three-state-hmm.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/pocketsphinx/pocketsphinx-5.1.1-compact-search.patch"
    "${PSP_BROWSER_APPLY_PATCH_SCRIPT}")

include(cmake/PspVoice.cmake)
tilefinch_configure_psp_voice()

set(PSP_BROWSER_LIBCURL_TRANSPORT OFF)
if(PSP_BROWSER_TRANSPORT_SOURCE STREQUAL
   "${CMAKE_CURRENT_SOURCE_DIR}/src/fetch.c")
    set(PSP_BROWSER_LIBCURL_TRANSPORT ON)
    if(NOT PSP)
        find_package(CURL 7.85 REQUIRED)
    endif()
endif()

set(PSP_BROWSER_FREETYPE_AVAILABLE OFF)
set(PSP_BROWSER_WEB_FONT_FREETYPE_MINIMUM_VERSION "2.14.3")
if(PSP_BROWSER_ENABLE_WEB_FONTS)
    if(PSP_BROWSER_SYSTEM_FREETYPE)
        # Explicit opt-in to a system FreeType.  Page fonts are hostile network
        # input, so we still gate on the minimum version: do not silently expose
        # them to an older system FreeType merely because it can render the
        # format.  The *_FOUND result is what proves the requested minimum was
        # satisfied; target existence alone must not bypass that gate.
        find_package(Freetype ${PSP_BROWSER_WEB_FONT_FREETYPE_MINIMUM_VERSION}
                     QUIET)
        if(Freetype_FOUND OR FREETYPE_FOUND)
            set(PSP_BROWSER_FREETYPE_AVAILABLE ON)
            message(STATUS "Bounded webfonts: system FreeType enabled")
        else()
            message(STATUS
                "Bounded webfonts: disabled (system FreeType >= ${PSP_BROWSER_WEB_FONT_FREETYPE_MINIMUM_VERSION} was not found)")
        endif()
    else()
        # Default: build the pinned vendored FreeType from source for BOTH the
        # host and the PSP with an identical minimal module set.  A pinned
        # vendored build is a stronger guarantee than a system-version floor —
        # it makes host and device glyph rasters bit-comparable — so no version
        # gate is needed here; the pin is the floor.
        include(cmake/freetype_vendored.cmake)
        if(TARGET freetype)
            set(PSP_BROWSER_FREETYPE_AVAILABLE ON)
            message(STATUS
                "Bounded webfonts: vendored FreeType ${PSP_BROWSER_WEB_FONT_FREETYPE_MINIMUM_VERSION} enabled")
        else()
            message(FATAL_ERROR
                "Bounded webfonts requested but the vendored FreeType target was not created")
        endif()
    endif()
endif()

if(PSP_BROWSER_USE_COMPILER_CACHE AND NOT CMAKE_C_COMPILER_LAUNCHER)
    find_program(PSP_BROWSER_COMPILER_CACHE NAMES ccache sccache NO_CACHE)
    if(PSP_BROWSER_COMPILER_CACHE)
        set(CMAKE_C_COMPILER_LAUNCHER "${PSP_BROWSER_COMPILER_CACHE}")
        message(STATUS "Compiler cache: ${PSP_BROWSER_COMPILER_CACHE}")
    endif()
endif()

if(PSP_BROWSER_QUICKJS_NATIVE_TRACE)
    if(NOT PSP_BROWSER_USE_BELLARD_QUICKJS)
        message(FATAL_ERROR
            "The native trace experiment currently requires Bellard QuickJS")
    endif()
    find_path(PSP_BROWSER_LIGHTNING_INCLUDE_DIR lightning.h
        HINTS "${PSP_BROWSER_LIGHTNING_ROOT}/include" REQUIRED)
    find_library(PSP_BROWSER_LIGHTNING_LIBRARY lightning
        HINTS "${PSP_BROWSER_LIGHTNING_ROOT}/lib" REQUIRED)
endif()

if(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE
   AND PSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH)
    message(FATAL_ERROR
        "The property-fault diagnostic currently targets the portable Bellard baseline, not the experimental VM patch")
endif()

set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEXBOR_INSTALL_HEADERS OFF CACHE BOOL "" FORCE)

set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)

if(PSP_BROWSER_VENDOR_DIR AND EXISTS "${PSP_BROWSER_VENDOR_DIR}/lexbor/CMakeLists.txt")
    set(PSP_BROWSER_LEXBOR_SOURCE_DIR "${PSP_BROWSER_VENDOR_DIR}/lexbor")
    set(PSP_BROWSER_LEXBOR_NEEDS_ADD_SUBDIRECTORY ON)
else()
    FetchContent_Declare(
        lexbor
        URL https://github.com/lexbor/lexbor/archive/refs/tags/v3.0.0.tar.gz
        URL_HASH SHA256=eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(lexbor)
    set(PSP_BROWSER_LEXBOR_SOURCE_DIR "${lexbor_SOURCE_DIR}")
    set(PSP_BROWSER_LEXBOR_NEEDS_ADD_SUBDIRECTORY OFF)
endif()
foreach(lexbor_patch IN ITEMS
        "${PSP_BROWSER_LEXBOR_PATCH}" "${PSP_BROWSER_LEXBOR_FOSTER_PATCH}"
        "${PSP_BROWSER_LEXBOR_TOKENIZER_PATCH}"
        "${PSP_BROWSER_LEXBOR_MRAW_PATCH}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -DPATCH_SOURCE_DIR=${PSP_BROWSER_LEXBOR_SOURCE_DIR}
            -DPATCH_FILE=${lexbor_patch}
            -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
            -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
        RESULT_VARIABLE lexbor_patch_result)
    if(NOT lexbor_patch_result EQUAL 0)
        message(FATAL_ERROR "Could not prepare the Lexbor source")
    endif()
endforeach()
if(PSP_BROWSER_LEXBOR_NEEDS_ADD_SUBDIRECTORY)
    add_subdirectory("${PSP_BROWSER_LEXBOR_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/_deps/lexbor-build"
                     EXCLUDE_FROM_ALL)
endif()

if(PSP AND TARGET lexbor_static)
    # The lexbor posix port config hard-codes -fPIC, which psp-gcc rejects
    # for -mabi=eabi; a later -fno-pic wins on the command line.
    target_compile_options(lexbor_static PRIVATE -fno-pic)
endif()

if(PSP_BROWSER_USE_BELLARD_QUICKJS)
    # The engine is vendored: third_party/quickjs is upstream Bellard QuickJS
    # at commit 04be246 with this repository's patch stack already applied
    # (see third_party/quickjs/README.md for the list). Nothing is fetched
    # and nothing is patched for an ordinary build. One gate remains: the
    # vendored quickjs.c/quickjs.h must match the pinned fingerprints below,
    # so an accidental edit, a stale copy, or a half-merged change fails
    # configure loudly instead of shipping. An intentional engine change
    # updates the two pins in the same commit.
    set(tilefinch_quickjs_vendor_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs")
    set(tilefinch_quickjs_vendor_c_sha256
        "6570a5234cbc4c4aef83e3f1580cf52c1502aa1cda62ab255107631469c03ba8")
    set(tilefinch_quickjs_vendor_h_sha256
        "3b405af189281669e35d9177547ac36c89464bb6120523d53ea64170965a367d")
    file(SHA256 "${tilefinch_quickjs_vendor_dir}/quickjs.c" tilefinch_quickjs_c_sha256)
    file(SHA256 "${tilefinch_quickjs_vendor_dir}/quickjs.h" tilefinch_quickjs_h_sha256)
    if(NOT tilefinch_quickjs_c_sha256 STREQUAL tilefinch_quickjs_vendor_c_sha256
       OR NOT tilefinch_quickjs_h_sha256 STREQUAL tilefinch_quickjs_vendor_h_sha256)
        message(FATAL_ERROR
            "third_party/quickjs does not match its pinned fingerprint "
            "(quickjs.c ${tilefinch_quickjs_c_sha256}, quickjs.h "
            "${tilefinch_quickjs_h_sha256}). If this engine change is "
            "intentional, update tilefinch_quickjs_vendor_c_sha256 and "
            "tilefinch_quickjs_vendor_h_sha256 in cmake/TilefinchDependencies.cmake "
            "in the same commit; otherwise restore the vendored files.")
    endif()
    # The other engine sources whose change can alter compiled bytecode:
    # regexp literals are compiled at parse time and lre_compile's output is
    # serialized into every cached script (classic, module, Memory Stick
    # tier); the atom and opcode tables and the number parser shape every
    # record too. They are pinned as one fingerprint and folded into
    # TILEFINCH_QUICKJS_ENGINE_ID below, which keys the persistent tier.
    set(tilefinch_quickjs_bytecode_sources
        quickjs-atom.h quickjs-opcode.h libregexp.c libregexp.h
        libregexp-opcode.h libunicode.c libunicode.h libunicode-table.h
        dtoa.c dtoa.h)
    set(tilefinch_quickjs_vendor_bytecode_sources_sha256
        "be01d4dcecb931935ae07678aa36d947eaa66f5a7a35efe5c776dfabf7d38998")
    set(tilefinch_quickjs_bytecode_source_hashes "")
    foreach(source IN LISTS tilefinch_quickjs_bytecode_sources)
        file(SHA256 "${tilefinch_quickjs_vendor_dir}/${source}"
             tilefinch_quickjs_source_sha256)
        string(APPEND tilefinch_quickjs_bytecode_source_hashes
               "${source} ${tilefinch_quickjs_source_sha256}\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${tilefinch_quickjs_vendor_dir}/${source}")
    endforeach()
    string(SHA256 tilefinch_quickjs_bytecode_sources_sha256
           "${tilefinch_quickjs_bytecode_source_hashes}")
    if(NOT tilefinch_quickjs_bytecode_sources_sha256 STREQUAL
           tilefinch_quickjs_vendor_bytecode_sources_sha256)
        message(FATAL_ERROR
            "third_party/quickjs's regexp, unicode, atom, opcode and dtoa "
            "sources do not match their pinned fingerprint "
            "(${tilefinch_quickjs_bytecode_sources_sha256}). If this engine "
            "change is intentional, update "
            "tilefinch_quickjs_vendor_bytecode_sources_sha256 in "
            "cmake/TilefinchDependencies.cmake in the same commit; otherwise "
            "restore the vendored files.")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${tilefinch_quickjs_vendor_dir}/quickjs.c"
        "${tilefinch_quickjs_vendor_dir}/quickjs.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-host-get-code-for-eval.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-near-limit-array-growth.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-array-length-shrink.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-compact-byte-array.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-compact-array-repack.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-native-string-gc.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-property-fault-trace.patch")
    if(PSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH)
        message(FATAL_ERROR
            "The experimental Bellard VM patch is no longer applied at "
            "configure time. Its patch files remain under patches/ as "
            "history; build it from a branch that applies them to "
            "third_party/quickjs, or from a commit before the engine was "
            "vendored.")
    endif()
    set(quickjs_SOURCE_DIR "${tilefinch_quickjs_vendor_dir}")
    # Lab variants (a capture-getter control, compact character/byte-array
    # control, the property-fault trace) are the only remaining patch users.
    # They never touch the vendored tree: the two engine files are copied
    # into the binary directory, the default stack above the shared bounded
    # baseline is reversed and re-applied with the variant's choices, and the
    # result must match a pinned fingerprint computed for that combination.
    if(NOT PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH
       OR NOT PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY
       OR PSP_BROWSER_JS_PROPERTY_FAULT_TRACE)
        set(tilefinch_quickjs_variant_dir "${CMAKE_CURRENT_BINARY_DIR}/quickjs-variant")
        set(tilefinch_quickjs_patches "${CMAKE_CURRENT_SOURCE_DIR}/patches")
        set(tilefinch_quickjs_variant_key
            "${PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH}-${PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY}-${PSP_BROWSER_JS_PROPERTY_FAULT_TRACE}")
        # capture-getter, compact-char-array, property-fault-trace -> quickjs.c
        set(tilefinch_quickjs_variant_ON-OFF-OFF
            "503177c74e8c6e4683f009c30cc925bea26b58030ac2cf4c2c80aea4cca4ba77")
        set(tilefinch_quickjs_variant_ON-ON-ON
            "8d0aca7fe92288981aa7427827e7823b3d34d0ce0e18b2cf0311bd0cf3b1be18")
        set(tilefinch_quickjs_variant_ON-OFF-ON
            "494c98afa394cb099414aea38414bd5284468c91fc82d694af75c3a457e193d9")
        set(tilefinch_quickjs_variant_OFF-ON-OFF
            "bca2b483166080e7e5297f3d206020790104a0f80f49e4344ae84b05b5b3943c")
        set(tilefinch_quickjs_variant_OFF-ON-ON
            "d02bc841e1eb2d9861ec4cc04551889c9e24eadd22ea96399a68ffa4ef907d00")
        set(tilefinch_quickjs_variant_OFF-OFF-OFF
            "94d67b256b6ffa6c58d4a78689d2030720d95baf193f4db27bf94c4420583494")
        set(tilefinch_quickjs_variant_OFF-OFF-ON
            "79c0393feddf90d78d4d8bd8a83eff6e0f1bcf45739fd9544a54a8bed6452d41")
        if(NOT DEFINED tilefinch_quickjs_variant_${tilefinch_quickjs_variant_key})
            message(FATAL_ERROR
                "No pinned QuickJS variant for capture-getter/compact/property-fault "
                "= ${tilefinch_quickjs_variant_key}")
        endif()
        set(tilefinch_quickjs_variant_expected
            "${tilefinch_quickjs_variant_${tilefinch_quickjs_variant_key}}")
        set(tilefinch_quickjs_variant_ready OFF)
        if(EXISTS "${tilefinch_quickjs_variant_dir}/quickjs.c"
           AND EXISTS "${tilefinch_quickjs_variant_dir}/quickjs.h")
            file(SHA256 "${tilefinch_quickjs_variant_dir}/quickjs.c"
                 tilefinch_quickjs_variant_present)
            file(SHA256 "${tilefinch_quickjs_variant_dir}/quickjs.h"
                 tilefinch_quickjs_variant_header)
            if(tilefinch_quickjs_variant_present STREQUAL tilefinch_quickjs_variant_expected
               AND tilefinch_quickjs_variant_header STREQUAL tilefinch_quickjs_vendor_h_sha256)
                set(tilefinch_quickjs_variant_ready ON)
            endif()
        endif()
        if(NOT tilefinch_quickjs_variant_ready)
            file(REMOVE_RECURSE "${tilefinch_quickjs_variant_dir}")
            file(MAKE_DIRECTORY "${tilefinch_quickjs_variant_dir}")
            foreach(source quickjs.c quickjs.h)
                file(COPY_FILE "${tilefinch_quickjs_vendor_dir}/${source}"
                     "${tilefinch_quickjs_variant_dir}/${source}")
            endforeach()
            # Reverse the default stack above the shared bounded baseline.
            foreach(layer
                    float-array-indexed-store
                    native-string-gc
                    compact-array-repack
                    compact-byte-array
                    array-length-shrink
                    near-limit-array-growth
                    host-get-code-for-eval
                    repeat-rope-eval-scan
                    compact-char-array-cow compact-char-array
                    single-char-string-buffer repeat-rope-eval latin1-string
                    scope-oom retired-async-state realm-retirement
                    dynamic-code-policy capture-getter-fastpath)
                execute_process(COMMAND "${PATCH_EXECUTABLE}" --force --silent -R -p1
                    -i "${tilefinch_quickjs_patches}/bellard-quickjs-04be246-${layer}.patch"
                    WORKING_DIRECTORY "${tilefinch_quickjs_variant_dir}"
                    COMMAND_ERROR_IS_FATAL ANY)
            endforeach()
            file(SHA256 "${tilefinch_quickjs_variant_dir}/quickjs.c"
                 tilefinch_quickjs_variant_baseline)
            if(NOT tilefinch_quickjs_variant_baseline STREQUAL
                   "23965fca3eb0c0f8e977ce23d11bac3180d35d9e841bc01b20ab05193e698e9b")
                message(FATAL_ERROR
                    "Reversing the QuickJS default stack reached "
                    "${tilefinch_quickjs_variant_baseline}, not the pinned "
                    "bounded baseline")
            endif()
            set(tilefinch_quickjs_forward)
            if(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE)
                list(APPEND tilefinch_quickjs_forward property-fault-trace)
            endif()
            if(PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH)
                list(APPEND tilefinch_quickjs_forward capture-getter-fastpath)
            endif()
            list(APPEND tilefinch_quickjs_forward
                dynamic-code-policy realm-retirement retired-async-state scope-oom
                latin1-string repeat-rope-eval single-char-string-buffer)
            if(PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY)
                list(APPEND tilefinch_quickjs_forward
                    compact-char-array compact-char-array-cow)
            endif()
            list(APPEND tilefinch_quickjs_forward repeat-rope-eval-scan
                host-get-code-for-eval near-limit-array-growth
                array-length-shrink)
            if(PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY)
                list(APPEND tilefinch_quickjs_forward
                    compact-byte-array compact-array-repack)
            endif()
            list(APPEND tilefinch_quickjs_forward native-string-gc
                float-array-indexed-store)
            foreach(layer IN LISTS tilefinch_quickjs_forward)
                execute_process(COMMAND "${PATCH_EXECUTABLE}" --force --forward --silent -p1
                    -i "${tilefinch_quickjs_patches}/bellard-quickjs-04be246-${layer}.patch"
                    WORKING_DIRECTORY "${tilefinch_quickjs_variant_dir}"
                    COMMAND_ERROR_IS_FATAL ANY)
            endforeach()
            file(SHA256 "${tilefinch_quickjs_variant_dir}/quickjs.c"
                 tilefinch_quickjs_variant_result)
            if(NOT tilefinch_quickjs_variant_result STREQUAL tilefinch_quickjs_variant_expected)
                message(FATAL_ERROR
                    "QuickJS variant ${tilefinch_quickjs_variant_key} produced "
                    "${tilefinch_quickjs_variant_result}, not the pinned "
                    "${tilefinch_quickjs_variant_expected}")
            endif()
        endif()
        foreach(source quickjs-atom.h quickjs-opcode.h cutils.c cutils.h dtoa.c dtoa.h
                libregexp.c libregexp.h libregexp-opcode.h libunicode.c libunicode.h
                libunicode-table.h list.h)
            # Compilers see the binary-directory copy, so a vendor edit must
            # first trigger configure to refresh it before dependency checks.
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${tilefinch_quickjs_vendor_dir}/${source}")
            file(COPY_FILE "${tilefinch_quickjs_vendor_dir}/${source}"
                 "${tilefinch_quickjs_variant_dir}/${source}" ONLY_IF_DIFFERENT)
        endforeach()
        set(quickjs_SOURCE_DIR "${tilefinch_quickjs_variant_dir}")
    endif()
    set(TILEFINCH_QUICKJS_COMPILE_SOURCE_DIR "${quickjs_SOURCE_DIR}"
        CACHE INTERNAL "Actual Bellard engine source selected for this build" FORCE)
    # Persistent bytecode is valid only for the engine that wrote it: key it
    # by the exact interpreter source compiled here and the pinned sources
    # whose output it serializes (regexp programs, atoms, opcodes).
    file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.c" tilefinch_quickjs_engine_sha256)
    string(SHA256 tilefinch_quickjs_engine_sha256
           "${tilefinch_quickjs_engine_sha256} ${tilefinch_quickjs_bytecode_sources_sha256}")
    string(SUBSTRING "${tilefinch_quickjs_engine_sha256}" 0 16
        tilefinch_quickjs_engine_id)
    set(TILEFINCH_QUICKJS_ENGINE_ID "${tilefinch_quickjs_engine_id}"
        CACHE INTERNAL "Fingerprint of the compiled QuickJS source" FORCE)
    add_library(qjs STATIC
        "${quickjs_SOURCE_DIR}/quickjs.c"
        "${quickjs_SOURCE_DIR}/dtoa.c"
        "${quickjs_SOURCE_DIR}/libregexp.c"
        "${quickjs_SOURCE_DIR}/libunicode.c"
        "${quickjs_SOURCE_DIR}/cutils.c")
    target_include_directories(qjs SYSTEM PUBLIC "${quickjs_SOURCE_DIR}")
    target_compile_definitions(qjs PRIVATE
        _GNU_SOURCE CONFIG_VERSION="2026-06-04")
    target_compile_definitions(qjs PUBLIC
        TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY=1)
    option(PSP_BROWSER_EXECUTION_CENSUS
        "Compile validation-only interpreter census hooks (disable for a clean timing control)" ON)
    if(NOT PSP OR (TILEFINCH_PSP_VALIDATION_LOG AND PSP_BROWSER_EXECUTION_CENSUS))
        target_compile_definitions(qjs PUBLIC CONFIG_TILEFINCH_EXECUTION_CENSUS=1)
        option(PSP_BROWSER_JS_REFCOUNT_CENSUS "Count-only value retain/release diagnostic; timings are not comparable" OFF)
        if(PSP_BROWSER_JS_REFCOUNT_CENSUS)
            target_compile_definitions(qjs PUBLIC CONFIG_TILEFINCH_REFCOUNT_CENSUS=1)
        endif()
    endif()
    if(NOT PSP)
        # Deterministic host proof that repeated-string eval skips shared rope
        # nodes while preserving its logical interrupt accounting; and
        # JS_CompileLazyFunctions, which the lazy-function tests use to
        # compile every deferred body of a script.
        target_compile_definitions(qjs PUBLIC
            CONFIG_TILEFINCH_EVAL_ROPE_TEST=1
            CONFIG_TILEFINCH_LAZY_TOOLS=1)
    endif()
    target_compile_options(qjs PRIVATE -funsigned-char -fwrapv)
    if(PSP)
        # The PSP presets are MinSizeRel (-Os); the JavaScript engine alone
        # is built at -O2 (it follows the build type's flags, so it wins).
        # Device, chatgpt-ask offline replay: first usable input -3.2 s,
        # first answer -6.5 s, module compile 8.29 -> 7.43 s; the device
        # kernel bench runs most kernels at 0.6-0.9x the -Os time. Cost
        # ~270 KB of .text (user-approved; the ratchets below include it).
        target_compile_options(qjs PRIVATE -O2)
    endif()
    set_target_properties(qjs PROPERTIES C_EXTENSIONS ON)
    if(PSP)
        # newlib PSP accommodations, confined to the vendored VM:
        # malloc_usable_size is declared in <malloc.h> (not <stdlib.h>);
        # struct tm has no gmtoff member, so the single tm_gmtoff read
        # becomes a constant-zero expression (UTC, deterministic); and
        # newlib's int32_t is `long int`, so GCC 15's pointer-type hard
        # error relaxes to the warning it was before (identical 32-bit
        # representation on this ABI).
        target_compile_options(qjs PRIVATE
            -include malloc.h
            "-Dtm_gmtoff=tm_isdst * 0"
            -Wno-error=incompatible-pointer-types)
    endif()
    if(PSP_BROWSER_JS_EXECUTION_PROFILE)
        target_compile_definitions(qjs PUBLIC CONFIG_EXECUTION_PROFILE=1)
    endif()
    if(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE)
        target_compile_definitions(qjs PUBLIC CONFIG_PROPERTY_FAULT_TRACE=1)
    endif()
    if(PSP_BROWSER_JS_CALL_COUNTS)
        target_compile_definitions(qjs PRIVATE CONFIG_TILEFINCH_CALL_COUNTS=1)
    endif()
    if(PSP_BROWSER_JS_OP_COUNTS)
        target_compile_definitions(qjs PRIVATE CONFIG_TILEFINCH_OP_COUNTS=1)
    endif()
    if(PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY)
        target_compile_definitions(qjs PRIVATE
            CONFIG_TILEFINCH_COMPACT_CHAR_ARRAY=1)
    endif()
    if(NOT PSP_BROWSER_QUICKJS_FUNCTION_RECYCLE)
        target_compile_definitions(qjs PRIVATE
            CONFIG_DISABLE_FUNCTION_STORAGE_RECYCLE=1)
    endif()
    if(NOT PSP_BROWSER_QUICKJS_PORTABLE_REGION)
        target_compile_definitions(qjs PRIVATE
            CONFIG_DISABLE_PORTABLE_REGION=1)
    endif()
    if(PSP_BROWSER_QUICKJS_NATIVE_TRACE)
        target_sources(qjs PRIVATE src/native_trace.c)
        target_include_directories(qjs PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${PSP_BROWSER_LIGHTNING_INCLUDE_DIR}")
        target_compile_definitions(qjs PRIVATE CONFIG_NATIVE_TRACE=1)
        target_link_libraries(qjs PUBLIC "${PSP_BROWSER_LIGHTNING_LIBRARY}")
    endif()
    # Profile-guided optimisation of the engine objects on the PSP
    # (PERFORMANCE_LEDGER.md, "Profile-guided engine build"). Both are off by
    # default and neither touches any other object.
    #
    # GENERATE instruments the engine with arc and value counters (libgcov).
    # A PSP program never runs exit(), so the browser writes the counters
    # itself just before its clean-exit record, to pgo/ beside the EBOOT (a
    # relative path, resolved against the EBOOT's directory). Train under
    # PPSSPP only: scripts/train-quickjs-pgo.sh. The instrumented image is
    # far over the .text ratchet (PSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE).
    #
    # USE names a directory of the resulting .gcda files. GCC finds a
    # profile by its object's path, mangled ('/' -> '#') relative to the
    # build tree (-fprofile-prefix-path), so a profile trained in one build
    # directory applies in another, but only to the exact engine build it
    # was trained on (the fingerprint below). Functions the training never
    # ran stay at the ordinary -O2 (-fprofile-partial-training) rather than
    # being optimized for size, since other sites run engine paths
    # chatgpt.com does not.
    option(PSP_BROWSER_QUICKJS_PGO_GENERATE
        "Instrument the PSP QuickJS objects for profile collection (training builds)" OFF)
    set(PSP_BROWSER_QUICKJS_PGO_USE "" CACHE PATH
        "Directory of QuickJS .gcda profiles to optimize the PSP engine with")
    if(PSP_BROWSER_QUICKJS_PGO_GENERATE OR PSP_BROWSER_QUICKJS_PGO_USE)
        if(NOT PSP)
            message(FATAL_ERROR
                "PSP_BROWSER_QUICKJS_PGO_* apply to the PSP cross-build only")
        endif()
        if(PSP_BROWSER_QUICKJS_PGO_GENERATE AND PSP_BROWSER_QUICKJS_PGO_USE)
            message(FATAL_ERROR "PSP_BROWSER_QUICKJS_PGO_GENERATE and "
                "PSP_BROWSER_QUICKJS_PGO_USE are mutually exclusive")
        endif()
        # What a profile is valid for: the compiler, every engine source and
        # header, and the engine's flags and definitions as configured so
        # far (build-tree paths in the prefix maps excepted). Experiment
        # flags (PSP_BROWSER_QUICKJS_OPT_FLAGS) are not in it; a change that
        # alters control flow still fails the compile (-Wcoverage-mismatch
        # stays an error).
        set(tilefinch_quickjs_pgo_inputs
            "compiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
        file(GLOB tilefinch_quickjs_pgo_files
            "${quickjs_SOURCE_DIR}/*.c" "${quickjs_SOURCE_DIR}/*.h")
        list(SORT tilefinch_quickjs_pgo_files)
        foreach(tilefinch_quickjs_pgo_file IN LISTS tilefinch_quickjs_pgo_files)
            file(SHA256 "${tilefinch_quickjs_pgo_file}" tilefinch_quickjs_pgo_hash)
            get_filename_component(tilefinch_quickjs_pgo_name
                "${tilefinch_quickjs_pgo_file}" NAME)
            string(APPEND tilefinch_quickjs_pgo_inputs
                "\n${tilefinch_quickjs_pgo_name}=${tilefinch_quickjs_pgo_hash}")
        endforeach()
        string(TOUPPER "${CMAKE_BUILD_TYPE}" tilefinch_quickjs_pgo_config)
        get_directory_property(tilefinch_quickjs_pgo_dir_options COMPILE_OPTIONS)
        get_directory_property(tilefinch_quickjs_pgo_dir_defs COMPILE_DEFINITIONS)
        # The PSP toolchain's directory properties can be appended twice
        # (seen between otherwise identical trees); a repeated include
        # directory or definition does not change the compile.
        list(REMOVE_DUPLICATES tilefinch_quickjs_pgo_dir_options)
        list(REMOVE_DUPLICATES tilefinch_quickjs_pgo_dir_defs)
        foreach(tilefinch_quickjs_pgo_property
                COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES
                C_STANDARD C_EXTENSIONS SOURCES)
            get_target_property(tilefinch_quickjs_pgo_value qjs
                ${tilefinch_quickjs_pgo_property})
            if(tilefinch_quickjs_pgo_property STREQUAL "INCLUDE_DIRECTORIES"
                    OR tilefinch_quickjs_pgo_property STREQUAL "COMPILE_DEFINITIONS")
                list(REMOVE_DUPLICATES tilefinch_quickjs_pgo_value)
            endif()
            string(APPEND tilefinch_quickjs_pgo_inputs
                "\n${tilefinch_quickjs_pgo_property}=${tilefinch_quickjs_pgo_value}")
        endforeach()
        string(APPEND tilefinch_quickjs_pgo_inputs
            "\nflags=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${tilefinch_quickjs_pgo_config}}"
            "\ndirectory-options=${tilefinch_quickjs_pgo_dir_options}"
            "\ndirectory-definitions=${tilefinch_quickjs_pgo_dir_defs}")
        # The prefix maps and absolute paths (SOURCES, INCLUDE_DIRECTORIES)
        # name the checkout and build tree, not the engine.
        string(REGEX REPLACE "-f(file|debug|macro)-prefix-map=[^;\n]*;?" ""
            tilefinch_quickjs_pgo_inputs "${tilefinch_quickjs_pgo_inputs}")
        # The build tree may sit inside the checkout: map it first.
        string(REPLACE "${CMAKE_BINARY_DIR}" "<build>"
            tilefinch_quickjs_pgo_inputs "${tilefinch_quickjs_pgo_inputs}")
        string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}" "<source>"
            tilefinch_quickjs_pgo_inputs "${tilefinch_quickjs_pgo_inputs}")
        if(DEFINED ENV{PSPDEV} AND NOT "$ENV{PSPDEV}" STREQUAL "")
            string(REPLACE "$ENV{PSPDEV}" "<pspdev>"
                tilefinch_quickjs_pgo_inputs "${tilefinch_quickjs_pgo_inputs}")
        endif()
        string(SHA256 tilefinch_quickjs_pgo_fingerprint
            "${tilefinch_quickjs_pgo_inputs}")
        string(SUBSTRING "${tilefinch_quickjs_pgo_fingerprint}" 0 24
            tilefinch_quickjs_pgo_fingerprint)
        set(TILEFINCH_QUICKJS_PGO_FINGERPRINT "${tilefinch_quickjs_pgo_fingerprint}"
            CACHE INTERNAL "Engine build a QuickJS PGO profile is valid for" FORCE)
        file(WRITE "${CMAKE_BINARY_DIR}/quickjs-pgo-fingerprint.txt"
            "${tilefinch_quickjs_pgo_inputs}\n")
    endif()
    if(PSP_BROWSER_QUICKJS_PGO_GENERATE)
        message(WARNING "QuickJS is instrumented for profile collection: "
            "training build, not a shippable image")
        target_compile_options(qjs PRIVATE
            -fprofile-generate=pgo
            -fprofile-update=single
            "-fprofile-prefix-path=${CMAKE_BINARY_DIR}")
        target_link_libraries(qjs PUBLIC gcov)
    elseif(PSP_BROWSER_QUICKJS_PGO_USE)
        file(GLOB tilefinch_quickjs_pgo_profiles
            "${PSP_BROWSER_QUICKJS_PGO_USE}/*.gcda")
        if(NOT tilefinch_quickjs_pgo_profiles)
            message(FATAL_ERROR "PSP_BROWSER_QUICKJS_PGO_USE="
                "${PSP_BROWSER_QUICKJS_PGO_USE} holds no .gcda profiles")
        endif()
        # scripts/train-quickjs-pgo.sh records the fingerprint it trained.
        # GCC looks a function's profile up by an id that includes its
        # source line, so one line added near the top of quickjs.c loses the
        # profile of nearly every function below it (measured: 1,230 of
        # them), and that build's quickjs.c object was 28% larger than with
        # no profile at all. A profile is valid for exactly one engine build.
        if(NOT EXISTS "${PSP_BROWSER_QUICKJS_PGO_USE}/PROVENANCE")
            message(FATAL_ERROR "${PSP_BROWSER_QUICKJS_PGO_USE} has no "
                "PROVENANCE: cannot tell which engine build the profile was "
                "trained on (scripts/train-quickjs-pgo.sh writes it)")
        endif()
        file(STRINGS "${PSP_BROWSER_QUICKJS_PGO_USE}/PROVENANCE"
            tilefinch_quickjs_pgo_trained REGEX "^fingerprint=")
        string(REPLACE "fingerprint=" "" tilefinch_quickjs_pgo_trained
            "${tilefinch_quickjs_pgo_trained}")
        if(NOT tilefinch_quickjs_pgo_trained STREQUAL
           TILEFINCH_QUICKJS_PGO_FINGERPRINT)
            message(FATAL_ERROR "The QuickJS profile in "
                "${PSP_BROWSER_QUICKJS_PGO_USE} was trained on engine build "
                "'${tilefinch_quickjs_pgo_trained}'; this one is "
                "${TILEFINCH_QUICKJS_PGO_FINGERPRINT} (inputs in "
                "${CMAKE_BINARY_DIR}/quickjs-pgo-fingerprint.txt). Retrain "
                "with scripts/train-quickjs-pgo.sh: a stale profile is worse "
                "than none.")
        endif()
        # -fno-reorder-functions: no .text.hot/.text.unlikely function
        # grouping. The EBOOT's default link script would gather those
        # sections but the PSPLink PRX's linkfile.prx does not, so the two
        # would get different layouts; and whole-function ordering was
        # measured on its own and rejected (PERFORMANCE_LEDGER.md,
        # "Profile-ordered code layout"). Blocks inside each function are
        # still laid out by the profile.
        target_compile_options(qjs PRIVATE
            "-fprofile-use=${PSP_BROWSER_QUICKJS_PGO_USE}"
            "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
            -fprofile-partial-training
            -fno-reorder-functions
            -Werror=coverage-mismatch
            -Werror=missing-profile)
        # A new profile must recompile the engine, not only a new source.
        get_target_property(tilefinch_quickjs_pgo_sources qjs SOURCES)
        set_property(SOURCE ${tilefinch_quickjs_pgo_sources}
            APPEND PROPERTY OBJECT_DEPENDS ${tilefinch_quickjs_pgo_profiles})
    endif()
    # Engine-only compiler flags for optimisation experiments, applied after
    # the defaults and the PGO options above, so they can override either;
    # empty for every preset.
    set(PSP_BROWSER_QUICKJS_OPT_FLAGS "" CACHE STRING
        "Extra compile options for the vendored QuickJS objects only")
    if(PSP_BROWSER_QUICKJS_OPT_FLAGS)
        separate_arguments(tilefinch_quickjs_opt_flags
            NATIVE_COMMAND "${PSP_BROWSER_QUICKJS_OPT_FLAGS}")
        target_compile_options(qjs PRIVATE ${tilefinch_quickjs_opt_flags})
    endif()
elseif(PSP_BROWSER_VENDOR_DIR AND EXISTS "${PSP_BROWSER_VENDOR_DIR}/quickjs-ng/CMakeLists.txt")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -DPATCH_SOURCE_DIR=${PSP_BROWSER_VENDOR_DIR}/quickjs-ng
            -DPATCH_FILE=${PSP_BROWSER_QUICKJS_PATCH}
            -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
            -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
        RESULT_VARIABLE quickjs_patch_result)
    if(NOT quickjs_patch_result EQUAL 0)
        message(FATAL_ERROR "Could not prepare the vendored QuickJS-NG source")
    endif()
    add_subdirectory("${PSP_BROWSER_VENDOR_DIR}/quickjs-ng" "${CMAKE_BINARY_DIR}/_deps/quickjs-build" EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(
        quickjs_ng
        URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.15.0.tar.gz
        URL_HASH SHA256=d65f951fa9d347a912a53ec2c151bd0ac79bf73d445788e67670ca1b894c67c4
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(quickjs_ng)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -DPATCH_SOURCE_DIR=${quickjs_ng_SOURCE_DIR}
            -DPATCH_FILE=${PSP_BROWSER_QUICKJS_PATCH}
            -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
            -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
        RESULT_VARIABLE quickjs_patch_result)
    if(NOT quickjs_patch_result EQUAL 0)
        message(FATAL_ERROR "Could not prepare the fetched QuickJS-NG source")
    endif()
endif()

# WebAssembly is linked directly only in host labs. The PSP compiles the same
# pinned interpreter into a user PRX which is loaded on first WebAssembly use,
# keeping both its code and initialization off the boot path.
if((NOT PSP AND PSP_BROWSER_ENABLE_HOST_WEBASSEMBLY) OR PSP)
    if(PSP_BROWSER_WAMR_SOURCE_DIR)
        if(NOT EXISTS
           "${PSP_BROWSER_WAMR_SOURCE_DIR}/build-scripts/runtime_lib.cmake")
            message(FATAL_ERROR
                "PSP_BROWSER_WAMR_SOURCE_DIR is not a WAMR source tree: ${PSP_BROWSER_WAMR_SOURCE_DIR}")
        endif()
        set(TILEFINCH_WAMR_SOURCE_DIR
            "${PSP_BROWSER_WAMR_SOURCE_DIR}")
    else()
        FetchContent_Declare(
            tilefinch_wamr_source
            URL https://github.com/bytecodealliance/wasm-micro-runtime/archive/refs/tags/WAMR-2.4.5.tar.gz
            URL_HASH SHA256=1ab09d51099f276ca4a1d6629f6b589aab2bd0caa01445e05031a4bed22c199b
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            # MakeAvailable still handles pinned population and source-dir
            # overrides, but this deliberately nonexistent subdirectory keeps
            # WAMR's host-tool project out of PSP cross-builds.
            SOURCE_SUBDIR tilefinch-source-only)
        # WAMR's top-level project builds tools/AOT support and requires host
        # threading even during a PSP cross-build. Tilefinch consumes only
        # runtime_lib.cmake through the tightly configured subdirectory below,
        # so populate the pinned source without adding its project.
        FetchContent_MakeAvailable(tilefinch_wamr_source)
        set(TILEFINCH_WAMR_SOURCE_DIR
            "${tilefinch_wamr_source_SOURCE_DIR}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -DPATCH_SOURCE_DIR=${TILEFINCH_WAMR_SOURCE_DIR}
            -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/wamr-2.4.5-unaligned-pointer-store.patch
            -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
            -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
        RESULT_VARIABLE wamr_patch_result)
    if(NOT wamr_patch_result EQUAL 0)
        message(FATAL_ERROR "Could not prepare the bounded WAMR source")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -DPATCH_SOURCE_DIR=${TILEFINCH_WAMR_SOURCE_DIR}
            -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/wamr-2.4.5-browser-instantiation.patch
            -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
            -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
        RESULT_VARIABLE wamr_browser_patch_result)
    if(NOT wamr_browser_patch_result EQUAL 0)
        message(FATAL_ERROR
            "Could not apply the WAMR browser-instantiation contract")
    endif()
    if(PSP)
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                -DPATCH_SOURCE_DIR=${TILEFINCH_WAMR_SOURCE_DIR}
                -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/wamr-2.4.5-psp-allegrex.patch
                -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
                -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
            RESULT_VARIABLE wamr_psp_patch_result)
        if(NOT wamr_psp_patch_result EQUAL 0)
            message(FATAL_ERROR "Could not prepare WAMR for Allegrex")
        endif()
    endif()
    if(PSP)
        add_subdirectory(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wamr-psp"
            "${CMAKE_CURRENT_BINARY_DIR}/wamr-psp"
            EXCLUDE_FROM_ALL)
    else()
        add_subdirectory(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wamr"
            "${CMAKE_CURRENT_BINARY_DIR}/wamr-runtime"
            EXCLUDE_FROM_ALL)
    endif()
endif()

if(PSP_BROWSER_PGO_GENERATE AND PSP_BROWSER_PGO_USE)
    message(FATAL_ERROR
        "PSP_BROWSER_PGO_GENERATE and PSP_BROWSER_PGO_USE are mutually exclusive")
endif()
if((PSP_BROWSER_PGO_GENERATE OR PSP_BROWSER_PGO_USE) AND
   NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The allocator-benchmark PGO workflow requires Clang")
endif()

if(PSP_BROWSER_VENDOR_DIR AND EXISTS "${PSP_BROWSER_VENDOR_DIR}/stb/stb_truetype.h")
    set(stb_SOURCE_DIR "${PSP_BROWSER_VENDOR_DIR}/stb")
else()
    FetchContent_Declare(
        stb
        URL https://github.com/nothings/stb/archive/31c1ad37456438565541f4919958214b6e762fb4.tar.gz
        URL_HASH SHA256=e4e3bba9c572a4a4148373a914d88ea0f0d11de8cc2c66739926e7eca0223319
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(stb)
endif()
# src/image_decode.c decodes a JPEG that is shown smaller than its source by
# storing each IDCT block box-averaged by 2, 4 or 8, so the full-resolution
# component planes never exist (stbi__jpeg.tilefinch_scale_shift; zero, the
# default for every other stb caller, is the unchanged upstream path).
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPATCH_SOURCE_DIR=${stb_SOURCE_DIR}
        -DPATCH_FILE=${PSP_BROWSER_STB_JPEG_SCALE_PATCH}
        -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
        -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
    RESULT_VARIABLE stb_patch_result)
if(NOT stb_patch_result EQUAL 0)
    message(FATAL_ERROR "Could not prepare the stb_image source")
endif()

if(PSP_BROWSER_VENDOR_DIR AND EXISTS "${PSP_BROWSER_VENDOR_DIR}/dejavu-fonts/ttf/DejaVuSans.ttf")
    set(dejavu_fonts_SOURCE_DIR "${PSP_BROWSER_VENDOR_DIR}/dejavu-fonts")
else()
    FetchContent_Declare(
        dejavu_fonts
        URL https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip
        URL_HASH SHA256=7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(dejavu_fonts)
endif()

set(PSP_BROWSER_SANS_FONT "${dejavu_fonts_SOURCE_DIR}/ttf/DejaVuSans.ttf"
    CACHE FILEPATH "TrueType sans-serif face used by the desktop lab")
set(PSP_BROWSER_SERIF_FONT "${dejavu_fonts_SOURCE_DIR}/ttf/DejaVuSerif.ttf"
    CACHE FILEPATH "TrueType serif face used by the desktop lab")
set(PSP_BROWSER_SANS_ITALIC_FONT
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/DejaVuSans-Oblique-Latin.ttf"
    CACHE FILEPATH "Bounded TrueType italic sans-serif face used by the desktop lab")
set(PSP_BROWSER_SANS_BOLD_FONT
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/DejaVuSans-Bold-Latin.ttf"
    CACHE FILEPATH "Bounded TrueType bold sans-serif face used by the desktop lab")
set(PSP_BROWSER_SERIF_BOLD_FONT
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/DejaVuSerif-Bold-Latin.ttf"
    CACHE FILEPATH "Bounded TrueType bold serif face used by the desktop lab")
set(PSP_BROWSER_PSP_SANS_FONT
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/DejaVuSans-Latin.ttf")
set(PSP_BROWSER_PSP_SERIF_FONT
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/DejaVuSerif-Latin.ttf")
set(PSP_BROWSER_METRIC_SANS_FONT
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/TilefinchSans-Regular.ttf"
    CACHE FILEPATH "Bounded Arial/Helvetica-metric TrueType fallback face")
set(PSP_BROWSER_METRIC_SANS_BOLD_FONT
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/TilefinchSans-Bold.ttf"
    CACHE FILEPATH "Bounded bold Arial/Helvetica-metric TrueType fallback face")

if(PSP_BROWSER_VENDOR_DIR AND EXISTS "${PSP_BROWSER_VENDOR_DIR}/nanosvg/src/nanosvg.h")
    set(nanosvg_SOURCE_DIR "${PSP_BROWSER_VENDOR_DIR}/nanosvg")
else()
    FetchContent_Declare(
        nanosvg
        URL https://github.com/memononen/nanosvg/archive/239e102ec2c691f2902e20ace2ed36ee4a35cfe6.tar.gz
        URL_HASH SHA256=2bc68bdb518d7800252042e5cad50a0ab321596f0cbf49ef2a752926329063d2
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nanosvg)
endif()
foreach(nanosvg_patch IN ITEMS
        "${PSP_BROWSER_NANOSVG_PATCH}" "${PSP_BROWSER_NANOSVG_GEOMETRY_PATCH}"
        "${PSP_BROWSER_NANOSVG_DIVISIONS_PATCH}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -DPATCH_SOURCE_DIR=${nanosvg_SOURCE_DIR}
            -DPATCH_FILE=${nanosvg_patch}
            -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
            -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
        RESULT_VARIABLE nanosvg_patch_result)
    if(NOT nanosvg_patch_result EQUAL 0)
        message(FATAL_ERROR "Could not prepare the NanoSVG source")
    endif()
endforeach()

# Decode-only WebP support.  Sites increasingly sign the requested WebP
# transform into their CDN URLs, so content negotiation or URL rewriting
# cannot recover a JPEG/PNG sibling.  Keep encoders, tools, animation helpers,
# threading, and SIMD out of the PSP-sized browser binary.
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_USE_THREAD OFF CACHE BOOL "" FORCE)
set(WEBP_ENABLE_SIMD OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libwebp
    URL https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz
    URL_HASH SHA256=93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(libwebp)
# Upstream always declares its encoder libraries even when every encoder tool
# is disabled. Tilefinch links only webpdecoder; keep the unrelated encoder,
# demux, and sharp-YUV targets out of the ordinary `all` build so adding WebP
# does not turn each clean host build into an encoder build.
# src/image_decode.c decodes to MODE_RGBA only. WEBP_REDUCE_CSP keeps the
# RGBA/BGRA (and premultiplied) fancy upsamplers and drops the RGB, BGR,
# ARGB, RGBA-4444 and RGB-565 copies (about 4 KB of PSP .text); the RGBA
# path compiles identically, so decoded pixels do not change.
target_compile_definitions(webpdspdecode PRIVATE WEBP_REDUCE_CSP)
# Every libwebp allocation goes through WebPSafeMalloc/WebPSafeCalloc/
# WebPSafeFree in utils.c. Route them to src/image_decode.c, which charges
# the decode's Budget, so a WebP decode is accounted exactly (and refused when
# it does not fit) instead of pre-reserving a per-source-pixel estimate.
set_source_files_properties("${libwebp_SOURCE_DIR}/src/utils/utils.c"
    TARGET_DIRECTORY webputilsdecode
    PROPERTIES COMPILE_DEFINITIONS
        "malloc=tilefinch_webp_malloc;calloc=tilefinch_webp_calloc;free=tilefinch_webp_free")
foreach(_tilefinch_unused_webp_target
        sharpyuv webpencode webpdsp webputils webp webpdemux)
    if(TARGET ${_tilefinch_unused_webp_target})
        set_target_properties(${_tilefinch_unused_webp_target}
            PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()
if(PSP)
    # libwebp enables -fPIC for ordinary Unix builds. Allegrex's EABI rejects
    # PIC, so let the trailing option override the upstream default exactly as
    # the existing Lexbor and FreeType PSP accommodations do.
    target_compile_options(webpdecode PRIVATE -fno-pic)
    target_compile_options(webpdspdecode PRIVATE -fno-pic)
    target_compile_options(webputilsdecode PRIVATE -fno-pic)
endif()
