set(PLATFORM_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/wasm/psp")
add_definitions(-DBH_PLATFORM_PSP=1 -DWASM_HAVE_MREMAP=0)
include_directories(
    "${PLATFORM_SHARED_DIR}"
    "${SHARED_DIR}/platform/include")
set(PLATFORM_SHARED_SOURCE
    "${PLATFORM_SHARED_DIR}/psp_wamr_platform.c"
    "${SHARED_DIR}/platform/common/memory/mremap.c")
