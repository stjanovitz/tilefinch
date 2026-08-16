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

if(PSP)
    # Most engine sources intentionally contain several cohesive internal
    # functions in one translation unit. Let the PSP linker discard functions
    # and constants that are unreachable from the EBOOT without changing
    # source organization or runtime behavior. PSP ABI metadata is rooted by
    # the augmenting script because psp-fixup-imports discovers it after link.
    set(PSP_BROWSER_GC_KEEP_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PspGcKeep.ld")
    add_compile_options(-ffunction-sections -fdata-sections)
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
       "Enable stb_image GIF decoding (disable for the PSP stack profile)" ON)
option(PSP_BROWSER_ENABLE_WEB_FONTS
       "Enable bounded page-provided WOFF1 fonts through FreeType" ON)
option(PSP_BROWSER_ENABLE_PSP_VOICE
       "Build offline PocketSphinx push-to-talk input into the PSP EBOOT" ON)
option(PSP_BROWSER_SYSTEM_FREETYPE
       "Use a system FreeType (>= version floor) instead of the pinned vendored build" OFF)
option(PSP_BROWSER_USE_COMPILER_CACHE
       "Use ccache or sccache when one is available" ON)
option(PSP_BROWSER_USE_BELLARD_QUICKJS
       "Use pinned upstream QuickJS instead of QuickJS-NG" OFF)
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
include(cmake/PspOwnedTransport.cmake)
set(PSP_BROWSER_PSP_CA_BUNDLE
    "${CMAKE_CURRENT_SOURCE_DIR}/certs/roots.pem" CACHE FILEPATH
    "PEM trust bundle staged beside a real-network PSP browser EBOOT")
option(PSP_BROWSER_JS_EXECUTION_PROFILE
       "Collect bounded QuickJS opcode, function, and PC hot spots" OFF)
option(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE
       "Trace bounded null/undefined QuickJS property reads (lab diagnostics only)" OFF)
option(PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH
       "Inline trivial Bellard QuickJS getters that return one captured value" ON)
option(PSP_BROWSER_QUICKJS_FUNCTION_RECYCLE
       "Recycle bounded Bellard QuickJS function-object storage" ON)
option(PSP_BROWSER_QUICKJS_PORTABLE_REGION
       "Enable the bounded architecture-neutral QuickJS region tier" ON)
option(PSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH
       "Apply the experimental Bellard QuickJS VM patch" OFF)
option(PSP_BROWSER_QUICKJS_NATIVE_TRACE
       "Build the experimental GNU lightning QuickJS native trace tier" OFF)
set(PSP_BROWSER_LIGHTNING_ROOT "" CACHE PATH
    "GNU lightning installation prefix for the experimental native trace tier")
set(PSP_BROWSER_VENDOR_DIR "" CACHE PATH "Optional directory containing lexbor/ and quickjs-ng/")
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
set(PSP_BROWSER_BELLARD_QUICKJS_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-vm-profile.patch")
set(PSP_BROWSER_BELLARD_QUICKJS_OOM_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-oom-backtrace.patch")
set(PSP_BROWSER_BELLARD_QUICKJS_INTERRUPT_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-compile-interrupt.patch")
set(PSP_BROWSER_BELLARD_QUICKJS_ARRAY_GROWTH_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-bounded-array-growth.patch")
set(PSP_BROWSER_BELLARD_QUICKJS_CAPTURE_GETTER_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-capture-getter-fastpath.patch")
set(PSP_BROWSER_BELLARD_QUICKJS_PROPERTY_FAULT_TRACE_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-property-fault-trace.patch")
set(PSP_BROWSER_BELLARD_QUICKJS_DYNAMIC_CODE_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/bellard-quickjs-04be246-dynamic-code-policy.patch")
set(PSP_BROWSER_LEXBOR_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/lexbor-v3.0.0-partial-document-destroy.patch")
set(PSP_BROWSER_NANOSVG_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/nanosvg-239e102-bounded-fixed-edges.patch")
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
    "${PSP_BROWSER_BELLARD_QUICKJS_PATCH}"
    "${PSP_BROWSER_BELLARD_QUICKJS_OOM_PATCH}"
    "${PSP_BROWSER_BELLARD_QUICKJS_INTERRUPT_PATCH}"
    "${PSP_BROWSER_BELLARD_QUICKJS_ARRAY_GROWTH_PATCH}"
    "${PSP_BROWSER_BELLARD_QUICKJS_CAPTURE_GETTER_PATCH}"
    "${PSP_BROWSER_BELLARD_QUICKJS_PROPERTY_FAULT_TRACE_PATCH}"
    "${PSP_BROWSER_BELLARD_QUICKJS_DYNAMIC_CODE_PATCH}"
    "${PSP_BROWSER_LEXBOR_PATCH}"
    "${PSP_BROWSER_NANOSVG_PATCH}"
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
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPATCH_SOURCE_DIR=${PSP_BROWSER_LEXBOR_SOURCE_DIR}
        -DPATCH_FILE=${PSP_BROWSER_LEXBOR_PATCH}
        -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
        -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
    RESULT_VARIABLE lexbor_patch_result)
if(NOT lexbor_patch_result EQUAL 0)
    message(FATAL_ERROR "Could not prepare the Lexbor source")
endif()
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
    FetchContent_Declare(
        quickjs
        URL https://github.com/bellard/quickjs/archive/04be246001599f5995fa2f2d8c91a0f198d3f34c.tar.gz
        URL_HASH SHA256=2a87ffcca6c870f764ce70a7736351bd7cff3dc1fb95a8fb059c260979f1e01a
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(quickjs)
    # These are exact accepted states for the pinned 04be246 source.  Checking
    # the complete files is stronger than treating a successful reverse patch
    # dry-run as proof that a patch was already applied: BSD patch cannot
    # reverse-check the OOM patch's adjacent minimal-context hunks reliably,
    # and a later VM patch legitimately changes the same files again.
    set(bellard_quickjs_oom_c_sha256
        "3a1c22544909d0f1f59124945de4980141b4bb27ee10f4b1d7fb1c09068ddb9b")
    set(bellard_quickjs_oom_h_sha256
        "2165f47772af9faee1798999a599fa9de850d1bf0259502dade1c25d4a588316")
    # The compile-interrupt patch stacks on the OOM fix and is applied in
    # every Bellard configuration; the resulting pair is the portable
    # baseline.  quickjs.h is untouched by both patches.
    set(bellard_quickjs_interrupt_c_sha256
        "77faf57435f09f283a5194af760c4f224d90c1247367e45079db947bedd03377")
    set(bellard_quickjs_bounded_c_sha256
        "dacbd511ede679e6c56627b68a011dfc051e8ac7c96d6c8dd5c6b3db5dfb0fea")
    set(bellard_quickjs_capture_getter_c_sha256
        "8bc71abd5ff38c4f94b4f8407e4c3575b91623f83c7d4e25adf39d99466e16bd")
    set(bellard_quickjs_property_fault_trace_c_sha256
        "0a37b1829b2aed93a3a81319120c79efd7aa4e70806795f8162159e51580cb05")
    set(bellard_quickjs_capture_getter_property_fault_trace_c_sha256
        "6727d84ee9add1133200d26d41161079a60b8ea58d85abeb90073fff541eb5c3")
    # The vm-profile patch is regenerated against the compile-interrupt
    # baseline; older vm-patched trees (813b15d4... / 1c341fc3...) are no
    # longer recognized and need a fresh binary directory.
    set(bellard_quickjs_vm_c_sha256
        "807d55648ec1f1048d3a266d9e41a53f07e4993abfcbe8308a3224f7505e09f8")
    set(bellard_quickjs_vm_h_sha256
        "0acf0b53accb016cd7bc5605b9e4f8eb3761a9d2117f3bdf064d84d20b13feeb")
    set(bellard_quickjs_bounded_vm_c_sha256
        "3b1127ea0f847deaec10db28edbc05319883e2329e41e2651b4f82e62ca11f45")
    set(bellard_quickjs_pristine_c_sha256
        "a68622cecb806f39bf24738c376a0a73032ea8913478cad702e0265c46f7999f")
    set(bellard_quickjs_pristine_h_sha256
        "2165f47772af9faee1798999a599fa9de850d1bf0259502dade1c25d4a588316")
    # FetchContent reuses its source tree. Remove our final, variant-agnostic
    # CSP hook before identifying the underlying pinned/optional patch state;
    # it is re-applied after that state has been fully validated below.
    file(STRINGS "${quickjs_SOURCE_DIR}/quickjs.h"
         bellard_dynamic_code_preexisting
         REGEX "JS_SetDynamicCodeEnabled")
    if(bellard_dynamic_code_preexisting)
        execute_process(
            COMMAND "${PATCH_EXECUTABLE}" --silent -R -p1
                -i "${PSP_BROWSER_BELLARD_QUICKJS_DYNAMIC_CODE_PATCH}"
            WORKING_DIRECTORY "${quickjs_SOURCE_DIR}"
            RESULT_VARIABLE bellard_dynamic_code_reverse_result)
        if(NOT bellard_dynamic_code_reverse_result EQUAL 0)
            message(FATAL_ERROR
                "Could not remove the Bellard QuickJS CSP hook before fingerprint validation")
        endif()
    endif()
    file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.c"
         bellard_quickjs_c_sha256)
    file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.h"
         bellard_quickjs_h_sha256)
    set(bellard_quickjs_oom_ready OFF)
    set(bellard_quickjs_interrupt_ready OFF)
    set(bellard_quickjs_vm_ready OFF)
    set(bellard_quickjs_bounded_ready OFF)
    set(bellard_quickjs_capture_getter_ready OFF)
    set(bellard_quickjs_property_fault_trace_ready OFF)
    set(bellard_quickjs_oom_preimage "")
    if(bellard_quickjs_c_sha256 STREQUAL
           bellard_quickjs_interrupt_c_sha256
       AND bellard_quickjs_h_sha256 STREQUAL
           bellard_quickjs_oom_h_sha256)
        set(bellard_quickjs_oom_ready ON)
        set(bellard_quickjs_interrupt_ready ON)
    elseif(bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_bounded_c_sha256
           AND bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_oom_h_sha256)
        set(bellard_quickjs_oom_ready ON)
        set(bellard_quickjs_interrupt_ready ON)
        set(bellard_quickjs_bounded_ready ON)
    elseif(PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH
           AND bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_capture_getter_c_sha256
           AND bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_oom_h_sha256)
        set(bellard_quickjs_oom_ready ON)
        set(bellard_quickjs_interrupt_ready ON)
        set(bellard_quickjs_bounded_ready ON)
        set(bellard_quickjs_capture_getter_ready ON)
    elseif(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE
           AND bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_property_fault_trace_c_sha256
           AND bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_oom_h_sha256)
        set(bellard_quickjs_oom_ready ON)
        set(bellard_quickjs_interrupt_ready ON)
        set(bellard_quickjs_bounded_ready ON)
        set(bellard_quickjs_property_fault_trace_ready ON)
    elseif(PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH
           AND PSP_BROWSER_JS_PROPERTY_FAULT_TRACE
           AND bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_capture_getter_property_fault_trace_c_sha256
           AND bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_oom_h_sha256)
        set(bellard_quickjs_oom_ready ON)
        set(bellard_quickjs_interrupt_ready ON)
        set(bellard_quickjs_bounded_ready ON)
        set(bellard_quickjs_capture_getter_ready ON)
        set(bellard_quickjs_property_fault_trace_ready ON)
    elseif(bellard_quickjs_c_sha256 STREQUAL
           bellard_quickjs_oom_c_sha256
       AND bellard_quickjs_h_sha256 STREQUAL
           bellard_quickjs_oom_h_sha256)
        set(bellard_quickjs_oom_ready ON)
    elseif(bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_vm_c_sha256
           AND bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_vm_h_sha256)
        set(bellard_quickjs_oom_ready ON)
        set(bellard_quickjs_interrupt_ready ON)
        set(bellard_quickjs_vm_ready ON)
        set(bellard_quickjs_capture_getter_ready ON)
    elseif(bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_bounded_vm_c_sha256
           AND bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_vm_h_sha256)
        set(bellard_quickjs_oom_ready ON)
        set(bellard_quickjs_interrupt_ready ON)
        set(bellard_quickjs_vm_ready ON)
        set(bellard_quickjs_bounded_ready ON)
        set(bellard_quickjs_capture_getter_ready ON)
    elseif(bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_pristine_c_sha256
           AND bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_pristine_h_sha256)
        set(bellard_quickjs_oom_preimage "pristine")
    else()
        message(FATAL_ERROR
            "Bellard QuickJS source has an unrecognized fingerprint "
            "(possibly a tree patched before the compile-interrupt "
            "baseline); configure in a fresh binary directory or remove "
            "that binary directory's _deps/quickjs-src")
    endif()
    # build_backtrace() receives the runtime's current exception as a borrowed
    # value. If adding its stack property exhausts the heap, QuickJS replaces
    # current_exception and can free that object while build_backtrace() is
    # still using it. QuickJS-NG roots the error for the same reason; carry the
    # minimal lifetime fix against our pinned Bellard revision.
    if(NOT bellard_quickjs_oom_ready)
        # One hunk intentionally ends immediately after its added line.  BSD
        # patch 2.0 rejects that valid zero-trailing-context hunk both forward
        # and in reverse.  Git's explicit unidiff-zero mode handles it without
        # guessing; the exact complete-file fingerprints below remain the
        # authority for admitting the result.
        find_program(PSP_BROWSER_GIT_EXECUTABLE git REQUIRED)
        file(REAL_PATH "${quickjs_SOURCE_DIR}"
            bellard_quickjs_work_dir)
        get_filename_component(bellard_quickjs_source_parent
            "${bellard_quickjs_work_dir}" DIRECTORY)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                "GIT_CEILING_DIRECTORIES=${bellard_quickjs_source_parent}"
                "${PSP_BROWSER_GIT_EXECUTABLE}" apply --check
                --no-index --unidiff-zero
                "${PSP_BROWSER_BELLARD_QUICKJS_OOM_PATCH}"
            WORKING_DIRECTORY "${bellard_quickjs_work_dir}"
            RESULT_VARIABLE bellard_quickjs_oom_check_result
            OUTPUT_VARIABLE bellard_quickjs_oom_check_output
            ERROR_VARIABLE bellard_quickjs_oom_check_error)
        if(NOT bellard_quickjs_oom_check_result EQUAL 0)
            message(FATAL_ERROR
                "Bellard QuickJS OOM patch does not apply to the pinned "
                "source: ${bellard_quickjs_oom_check_output}"
                "${bellard_quickjs_oom_check_error}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                "GIT_CEILING_DIRECTORIES=${bellard_quickjs_source_parent}"
                "${PSP_BROWSER_GIT_EXECUTABLE}" apply
                --no-index --unidiff-zero
                "${PSP_BROWSER_BELLARD_QUICKJS_OOM_PATCH}"
            WORKING_DIRECTORY "${bellard_quickjs_work_dir}"
            RESULT_VARIABLE bellard_quickjs_oom_patch_result
            OUTPUT_VARIABLE bellard_quickjs_oom_patch_output
            ERROR_VARIABLE bellard_quickjs_oom_patch_error)
        if(NOT bellard_quickjs_oom_patch_result EQUAL 0)
            message(FATAL_ERROR
                "Could not apply the Bellard QuickJS OOM backtrace fix: "
                "${bellard_quickjs_oom_patch_output}"
                "${bellard_quickjs_oom_patch_error}")
        endif()
        file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.c"
             bellard_quickjs_c_sha256)
        file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.h"
             bellard_quickjs_h_sha256)
        if(bellard_quickjs_oom_preimage STREQUAL "pristine"
           AND bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_oom_c_sha256
           AND bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_oom_h_sha256)
            set(bellard_quickjs_oom_ready ON)
        else()
            message(FATAL_ERROR
                "Bellard QuickJS OOM patch produced an unrecognized source "
                "fingerprint; refusing a partial or stale patch result")
        endif()
    endif()
    # The compile-interrupt patch is part of every Bellard configuration: it
    # lets the ordinary watchdog interrupt handler abort oversized compiles
    # at a bounded token cadence instead of running one uninterruptible
    # JS_Eval parse.  It stacks directly on the OOM baseline.
    if(NOT bellard_quickjs_interrupt_ready)
        find_program(PSP_BROWSER_GIT_EXECUTABLE git REQUIRED)
        file(REAL_PATH "${quickjs_SOURCE_DIR}"
            bellard_quickjs_work_dir)
        get_filename_component(bellard_quickjs_source_parent
            "${bellard_quickjs_work_dir}" DIRECTORY)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                "GIT_CEILING_DIRECTORIES=${bellard_quickjs_source_parent}"
                "${PSP_BROWSER_GIT_EXECUTABLE}" apply
                --no-index --unidiff-zero
                "${PSP_BROWSER_BELLARD_QUICKJS_INTERRUPT_PATCH}"
            WORKING_DIRECTORY "${bellard_quickjs_work_dir}"
            RESULT_VARIABLE bellard_quickjs_interrupt_result
            OUTPUT_VARIABLE bellard_quickjs_interrupt_output
            ERROR_VARIABLE bellard_quickjs_interrupt_error)
        if(NOT bellard_quickjs_interrupt_result EQUAL 0)
            message(FATAL_ERROR
                "Could not apply the Bellard QuickJS compile-interrupt "
                "patch: ${bellard_quickjs_interrupt_output}"
                "${bellard_quickjs_interrupt_error}")
        endif()
        file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.c"
             bellard_quickjs_c_sha256)
        file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.h"
             bellard_quickjs_h_sha256)
        if(NOT bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_interrupt_c_sha256
           OR NOT bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_oom_h_sha256)
            message(FATAL_ERROR
                "Bellard QuickJS compile-interrupt patch produced an "
                "unrecognized source fingerprint; refusing a partial or "
                "stale patch result")
        endif()
        set(bellard_quickjs_interrupt_ready ON)
    endif()
    if(NOT PSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH)
        # FetchContent deliberately reuses its populated source directory.
        # A binary tree that was configured once with the experimental VM
        # patch therefore stays patched even after the option is turned off.
        # Refuse that misleading configuration instead of silently labelling
        # experimental QuickJS as the pristine control.
        if((NOT bellard_quickjs_c_sha256 STREQUAL
                bellard_quickjs_interrupt_c_sha256
            AND NOT bellard_quickjs_c_sha256 STREQUAL
                bellard_quickjs_bounded_c_sha256
            AND NOT (PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH
                     AND bellard_quickjs_c_sha256 STREQUAL
                         bellard_quickjs_capture_getter_c_sha256)
            AND NOT (PSP_BROWSER_JS_PROPERTY_FAULT_TRACE
                     AND bellard_quickjs_c_sha256 STREQUAL
                         bellard_quickjs_property_fault_trace_c_sha256)
            AND NOT (PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH
                     AND PSP_BROWSER_JS_PROPERTY_FAULT_TRACE
                     AND bellard_quickjs_c_sha256 STREQUAL
                         bellard_quickjs_capture_getter_property_fault_trace_c_sha256))
           OR NOT bellard_quickjs_h_sha256 STREQUAL
               bellard_quickjs_oom_h_sha256)
            message(FATAL_ERROR
                "PSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH is OFF, but the "
                "Bellard QuickJS source does not match the pinned portable "
                "04be246 baseline (OOM + compile-interrupt) fingerprints: "
                "${quickjs_SOURCE_DIR}. Configure in a fresh "
                "binary directory (or remove only that binary directory's "
                "_deps/quickjs-src) to obtain a truthful baseline build.")
        endif()
    endif()
    if(PSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH)
        if(bellard_quickjs_bounded_ready AND NOT bellard_quickjs_vm_ready)
            message(FATAL_ERROR
                "The experimental Bellard VM patch must precede the bounded "
                "array-growth patch. Configure its first use in a fresh "
                "binary directory; normal portable builds reuse bounded "
                "QuickJS sources without this restriction.")
        endif()
        if(NOT bellard_quickjs_vm_ready)
            execute_process(
                COMMAND "${CMAKE_COMMAND}"
                    -DPATCH_SOURCE_DIR=${quickjs_SOURCE_DIR}
                    -DPATCH_FILE=${PSP_BROWSER_BELLARD_QUICKJS_PATCH}
                    -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
                    -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
                RESULT_VARIABLE bellard_quickjs_patch_result)
            if(NOT bellard_quickjs_patch_result EQUAL 0)
                message(FATAL_ERROR
                    "Could not prepare the fetched Bellard QuickJS source")
            endif()
            file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.c"
                 bellard_quickjs_c_sha256)
            file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.h"
                 bellard_quickjs_h_sha256)
            if(NOT bellard_quickjs_c_sha256 STREQUAL
                   bellard_quickjs_vm_c_sha256
               OR NOT bellard_quickjs_h_sha256 STREQUAL
                   bellard_quickjs_vm_h_sha256)
                message(FATAL_ERROR
                    "Bellard QuickJS VM patch produced an unrecognized "
                    "source fingerprint; refusing a partial or stale patch "
                    "result")
            endif()
            set(bellard_quickjs_vm_ready ON)
        endif()
    endif()
    # Dense arrays use 1.5x geometric growth upstream. Once the value buffer
    # is at least one MiB, cap spare-capacity growth at 512 KiB. This is an
    # architecture-neutral byte policy, so the same source naturally holds
    # fewer unused JSValue slots on a 32-bit PSP than on a 64-bit host.
    if(NOT bellard_quickjs_bounded_ready)
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                -DPATCH_SOURCE_DIR=${quickjs_SOURCE_DIR}
                -DPATCH_FILE=${PSP_BROWSER_BELLARD_QUICKJS_ARRAY_GROWTH_PATCH}
                -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
                -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
            RESULT_VARIABLE bellard_quickjs_bounded_patch_result)
        if(NOT bellard_quickjs_bounded_patch_result EQUAL 0)
            message(FATAL_ERROR
                "Could not apply the Bellard QuickJS bounded array-growth patch")
        endif()
        file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.c"
             bellard_quickjs_c_sha256)
        if(bellard_quickjs_vm_ready)
            set(bellard_quickjs_bounded_expected_sha256
                "${bellard_quickjs_bounded_vm_c_sha256}")
        else()
            set(bellard_quickjs_bounded_expected_sha256
                "${bellard_quickjs_bounded_c_sha256}")
        endif()
        if(NOT bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_bounded_expected_sha256)
            message(FATAL_ERROR
                "Bellard QuickJS bounded array-growth patch produced an "
                "unrecognized source fingerprint; refusing a partial or "
                "stale patch result")
        endif()
        set(bellard_quickjs_bounded_ready ON)
    endif()
    if(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE
       AND NOT bellard_quickjs_property_fault_trace_ready)
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                -DPATCH_SOURCE_DIR=${quickjs_SOURCE_DIR}
                -DPATCH_FILE=${PSP_BROWSER_BELLARD_QUICKJS_PROPERTY_FAULT_TRACE_PATCH}
                -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
                -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
            RESULT_VARIABLE bellard_quickjs_property_fault_trace_patch_result)
        if(NOT bellard_quickjs_property_fault_trace_patch_result EQUAL 0)
            message(FATAL_ERROR
                "Could not apply the Bellard QuickJS property-fault trace patch")
        endif()
        file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.c"
             bellard_quickjs_c_sha256)
        if(NOT bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_property_fault_trace_c_sha256)
            message(FATAL_ERROR
                "Bellard QuickJS property-fault trace patch produced an unrecognized source fingerprint")
        endif()
        set(bellard_quickjs_property_fault_trace_ready ON)
    endif()
    # A bytecode function consisting solely of get_var_ref0 + return is a
    # general closure getter. It cannot observe an interpreter frame, receiver,
    # argument handling, or allocation, so returning the captured value
    # directly preserves semantics while removing the dominant dispatch cost
    # in closure-heavy JavaScript. Keep it independently switchable so the lab
    # can retain a truthful upstream-interpreter control. The experimental VM
    # patch already contains the same shortcut.
    if(PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH
       AND NOT bellard_quickjs_vm_ready
       AND NOT bellard_quickjs_capture_getter_ready)
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                -DPATCH_SOURCE_DIR=${quickjs_SOURCE_DIR}
                -DPATCH_FILE=${PSP_BROWSER_BELLARD_QUICKJS_CAPTURE_GETTER_PATCH}
                -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
                -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
            RESULT_VARIABLE bellard_quickjs_capture_getter_patch_result)
        if(NOT bellard_quickjs_capture_getter_patch_result EQUAL 0)
            message(FATAL_ERROR
                "Could not apply the Bellard QuickJS capture-getter fast path")
        endif()
        file(SHA256 "${quickjs_SOURCE_DIR}/quickjs.c"
             bellard_quickjs_c_sha256)
        if(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE)
            set(bellard_quickjs_capture_getter_expected_sha256
                "${bellard_quickjs_capture_getter_property_fault_trace_c_sha256}")
        else()
            set(bellard_quickjs_capture_getter_expected_sha256
                "${bellard_quickjs_capture_getter_c_sha256}")
        endif()
        if(NOT bellard_quickjs_c_sha256 STREQUAL
               bellard_quickjs_capture_getter_expected_sha256)
            message(FATAL_ERROR
                "Bellard QuickJS capture-getter patch produced an "
                "unrecognized source fingerprint; refusing a partial or "
                "stale patch result")
        endif()
        set(bellard_quickjs_capture_getter_ready ON)
    endif()
    # CSP's unsafe-eval policy must live below page JavaScript. Apply one
    # small engine hook after all optional QuickJS patches; its semantic
    # markers make repeated configurations idempotent across valid variants.
    file(STRINGS "${quickjs_SOURCE_DIR}/quickjs.h"
         bellard_dynamic_code_marker
         REGEX "JS_SetDynamicCodeEnabled")
    if(NOT bellard_dynamic_code_marker)
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                -DPATCH_SOURCE_DIR=${quickjs_SOURCE_DIR}
                -DPATCH_FILE=${PSP_BROWSER_BELLARD_QUICKJS_DYNAMIC_CODE_PATCH}
                -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
                -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
            RESULT_VARIABLE bellard_dynamic_code_patch_result)
        if(NOT bellard_dynamic_code_patch_result EQUAL 0)
            message(FATAL_ERROR
                "Could not apply the Bellard QuickJS dynamic-code policy patch")
        endif()
    endif()
    file(STRINGS "${quickjs_SOURCE_DIR}/quickjs.c"
         bellard_dynamic_code_checks
         REGEX "dynamic code compilation disabled by Content Security Policy")
    list(LENGTH bellard_dynamic_code_checks bellard_dynamic_code_check_count)
    if(bellard_dynamic_code_check_count LESS 4)
        message(FATAL_ERROR
            "Bellard QuickJS dynamic-code policy hook is incomplete")
    endif()
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
    target_compile_options(qjs PRIVATE -funsigned-char -fwrapv)
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
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPATCH_SOURCE_DIR=${nanosvg_SOURCE_DIR}
        -DPATCH_FILE=${PSP_BROWSER_NANOSVG_PATCH}
        -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}
        -P ${PSP_BROWSER_APPLY_PATCH_SCRIPT}
    RESULT_VARIABLE nanosvg_patch_result)
if(NOT nanosvg_patch_result EQUAL 0)
    message(FATAL_ERROR "Could not prepare the NanoSVG source")
endif()

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
