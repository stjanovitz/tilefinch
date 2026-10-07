# libFuzzer targets for Tilefinch's untrusted-input parsers.
# Configure through tests/fuzz/build.sh (clang with libFuzzer required); the
# whole tree must be compiled with -fsanitize=fuzzer-no-link so the engine
# library carries coverage instrumentation.

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "TILEFINCH_BUILD_FUZZERS requires clang with libFuzzer")
endif()

set(TILEFINCH_FUZZ_TARGETS)
function(tilefinch_add_fuzzer name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE tilefinch_core)
    target_include_directories(${name} PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set(TILEFINCH_FUZZ_TARGETS ${TILEFINCH_FUZZ_TARGETS} ${name} PARENT_SCOPE)
endfunction()

tilefinch_add_fuzzer(fuzz_url fuzz_url.c)
tilefinch_add_fuzzer(fuzz_css fuzz_css.c)
tilefinch_add_fuzzer(fuzz_mp4 fuzz_mp4.c)
tilefinch_add_fuzzer(fuzz_hls fuzz_hls.c)
tilefinch_add_fuzzer(fuzz_ts fuzz_ts.c)
tilefinch_add_fuzzer(fuzz_image fuzz_image.c)
tilefinch_add_fuzzer(fuzz_http fuzz_http.c)
tilefinch_add_fuzzer(fuzz_hls_source fuzz_hls_source.c)
tilefinch_add_fuzzer(fuzz_font fuzz_font.c)
tilefinch_add_fuzzer(fuzz_bidi fuzz_bidi.c)
tilefinch_add_fuzzer(fuzz_html fuzz_html.c)
tilefinch_add_fuzzer(fuzz_misc fuzz_misc.c)

add_custom_target(tilefinch-fuzzers DEPENDS ${TILEFINCH_FUZZ_TARGETS})
