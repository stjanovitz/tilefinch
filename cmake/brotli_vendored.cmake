# Decode-only, identical pinned sources on host and PSP. Do not find a host
# library: it would bypass the web-font allocation and equivalence contract.
FetchContent_Declare(tilefinch_brotli
    URL https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz
    URL_HASH SHA256=816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec
    SOURCE_SUBDIR c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(tilefinch_brotli)
file(GLOB brotli_common CONFIGURE_DEPENDS "${tilefinch_brotli_SOURCE_DIR}/c/common/*.c")
file(GLOB brotli_decode CONFIGURE_DEPENDS "${tilefinch_brotli_SOURCE_DIR}/c/dec/*.c")
add_library(tilefinch_brotli_decode STATIC ${brotli_common} ${brotli_decode})
target_include_directories(tilefinch_brotli_decode PUBLIC
    "${tilefinch_brotli_SOURCE_DIR}/c/include")
set_target_properties(tilefinch_brotli_decode PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(PSP)
    target_compile_options(tilefinch_brotli_decode PRIVATE -fno-pic)
endif()
if(NOT PSP)
    # Only generated unit-test fixtures use the encoder; never link it into
    # tilefinch_core or a device executable.
    file(GLOB brotli_encode CONFIGURE_DEPENDS "${tilefinch_brotli_SOURCE_DIR}/c/enc/*.c")
    add_library(tilefinch_brotli_test_encode STATIC EXCLUDE_FROM_ALL ${brotli_encode})
    target_include_directories(tilefinch_brotli_test_encode PUBLIC
        "${tilefinch_brotli_SOURCE_DIR}/c/include")
    target_link_libraries(tilefinch_brotli_test_encode PUBLIC tilefinch_brotli_decode)
endif()
