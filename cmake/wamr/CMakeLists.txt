cmake_minimum_required(VERSION 3.16)

if(NOT TILEFINCH_WAMR_SOURCE_DIR)
    message(FATAL_ERROR "TILEFINCH_WAMR_SOURCE_DIR is required")
endif()

string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" WAMR_BUILD_PLATFORM)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(WAMR_BUILD_TARGET "AARCH64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WAMR_BUILD_TARGET "X86_64")
else()
    set(WAMR_BUILD_TARGET "X86_32")
endif()

set(WAMR_ROOT_DIR "${TILEFINCH_WAMR_SOURCE_DIR}")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIBC_EMCC 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_THREAD_MGR 0)
# WAMR's single-module optimization collapses a memory whose bytecode lacks a
# memory.grow instruction into one large immutable page.  The WebAssembly JS
# API may still grow that exported memory through Memory.prototype.grow(), so
# retain the declared 64 KiB page geometry even when the module itself never
# executes memory.grow.
set(WAMR_BUILD_MULTI_MODULE 1)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_GC 0)
set(WAMR_BUILD_DEBUG_INTERP 0)
set(WAMR_BUILD_DUMP_CALL_STACK 0)
set(WAMR_BUILD_MEMORY_PROFILING 0)
set(WAMR_BUILD_PERF_PROFILING 0)
set(WAMR_BUILD_INSTRUCTION_METERING 1)
set(WAMR_BUILD_SIMD 1)
set(WAMR_BUILD_BULK_MEMORY 1)

# Keep WAMR's directory-wide compile definitions and include paths confined to
# this subdirectory; none of them are part of Tilefinch's public ABI.
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
add_library(tilefinch_wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE}
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/wasm/wasm_introspection.c")
set_target_properties(tilefinch_wamr PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
target_include_directories(tilefinch_wamr SYSTEM PUBLIC
    "${WAMR_ROOT_DIR}/core/iwasm/include")
target_include_directories(tilefinch_wamr PRIVATE
    "${WAMR_ROOT_DIR}/core/iwasm/interpreter")
target_compile_options(tilefinch_wamr PRIVATE
    -Wno-unused-function -Wno-unused-variable -Wno-sign-compare)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    # The fast interpreter deliberately performs unaligned integer accesses on
    # targets whose hardware admits them. Alignment UBSan rejects those C
    # lvalues before it can test Tilefinch's BufferSource/import boundary. The
    # pinned pointer-load patch still removes native-pointer alignment hazards;
    # suppress only this upstream interpreter diagnostic.
    target_compile_options(tilefinch_wamr PRIVATE -fno-sanitize=alignment)
endif()
