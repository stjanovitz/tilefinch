cmake_minimum_required(VERSION 3.16)

set(WAMR_ROOT_DIR "${TILEFINCH_WAMR_SOURCE_DIR}")
set(WAMR_BUILD_PLATFORM "psp")
set(WAMR_BUILD_TARGET "MIPS")
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
set(WAMR_BUILD_MULTI_MODULE 1)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_GC 0)
# Cloudflare's standards probe uses fixed-width WebAssembly SIMD.  WAMR's
# fast interpreter routes this through SIMDe, which provides a bounded scalar
# fallback on Allegrex rather than assuming native vector instructions.
set(WAMR_BUILD_SIMD 1)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_INSTRUCTION_METERING 1)
set(SHARED_PLATFORM_CONFIG "${CMAKE_CURRENT_LIST_DIR}/psp_platform.cmake")

include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")

add_prx_module(tilefinch-wasm-component
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/wasm/psp/wasm_component_psp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/wasm/wasm_introspection.c"
    ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(tilefinch-wasm-component PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${WAMR_ROOT_DIR}/core/iwasm/include"
    "${WAMR_ROOT_DIR}/core/iwasm/interpreter"
    "${WAMR_ROOT_DIR}/core/shared/platform/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/wasm/psp")
target_compile_options(tilefinch-wasm-component PRIVATE
    -Os -G0 -fno-pic -ffunction-sections -fdata-sections
    -Wno-unused-function -Wno-unused-variable -Wno-sign-compare
    "SHELL:-isystem ${PSPDEV}/psp/sdk/include")
target_link_options(tilefinch-wasm-component PRIVATE -Wl,--gc-sections)
target_link_libraries(tilefinch-wasm-component PRIVATE
    m pspuser pspsdk)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31")
    set_property(TARGET tilefinch-wasm-component PROPERTY
        LINK_LIBRARIES_STRATEGY REORDER_MINIMALLY)
endif()
set_target_properties(tilefinch-wasm-component PROPERTIES
    OUTPUT_NAME tilefinch-wasm
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wasm-component")

set(TILEFINCH_WASM_COMPONENT_PRX
    "${CMAKE_BINARY_DIR}/wasm-component/tilefinch-wasm.prx"
    CACHE INTERNAL "")
