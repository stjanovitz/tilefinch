set(TILEFINCH_CORE_SOURCES
    src/browser_engine.c
    src/browser_profile.c
    src/browser_tabs.c
    src/budget.c
    src/content_blocker.c
    src/content_security_policy.c
    src/controller.c
    src/media_discovery.c
    src/danzeff_input.c
    src/data_url.c
    src/diagnostics.c
    src/document.c
    src/document_backing.c
    ${PSP_BROWSER_TRANSPORT_SOURCE}
    src/fetch_fault.c
    src/frame_sandbox.c
    src/font.c
    src/glyph_component.c
    src/glyph_component_store.c
    src/image.c
    src/install_paths.c
    src/generated/js_bootstrap.c
    src/generated/js_bootstrap_bytecode.c
    src/js_dom_bindings.c
    src/js_fetch_cors.c
    src/js_lazy_webpack.c
    src/js_module_loader.c
    src/js_remote_bindings.c
    src/js_runtime.c
    src/layout.c
    src/media_backend.c
    src/media_hls.c
    src/swdec/swdec_ts.c
    src/media_source.c
    src/media_http.c
    src/layout_block.c
    src/layout_block_decoration.c
    src/layout_block_flexrow.c
    src/layout_block_geometry.c
    src/layout_block_grid.c
    src/layout_block_margins.c
    src/layout_controls.c
    src/layout_float.c
    src/layout_flex.c
    src/layout_inline.c
    src/layout_paint.c
    src/layout_scroll.c
    src/layout_table.c
    src/navigation.c
    src/media_file.c
    src/media_h264_psp_compat.c
    src/media_mp4.c
    src/omnibox.c
    src/offline_download.c
    src/offline_library.c
    src/page_find.c
    src/platform.c
    src/psp_media_state.c
    src/psp_network_supervisor.c
    src/public_suffix.c
    src/render.c
    src/reader_mode.c
    src/request_context.c
    src/resource_integrity.c
    src/resources.c
    src/section_pager.c
    src/section_router.c
    src/section_store.c
    src/sha256.c
    src/script_lazy.c
    src/script_loader.c
    src/session.c
    src/session_persistence.c
    src/site_adapter.c
    src/style.c
    src/style_match.c
    src/style_math.c
    src/style_properties.c
    src/style_queries.c
    src/style_resolve.c
    src/style_selector_program.c
    src/style_sheet.c
    src/style_values.c
    src/swdec_component_store.c
    src/tls_session_store.c
    src/url.c
    src/update_manifest.c
    src/update_root_embedded.c
    src/update_client.c
    src/update_history.c
    src/update_journal.c
    src/update_installer.c
    src/update_package.c
    src/update_slot.c
    src/update_state.c
    src/update_storage.c
    src/voice_component.c
    src/viewport.c
    src/youtube_lite.c
    src/youtube_resolver.c
)

set(TILEFINCH_PSP_FRONTEND_SOURCES
    src/main.c
    src/interactive_main.c
    src/failure_recovery_main.c
    src/psp_text_input.c
    src/psp_ui.c
    src/psp_ui_menu.c
    src/psp_voice_input.c
    src/psp_network.c
    src/psp_time.c
    src/media_backend_psp.c
    src/media_backend_psp_imports.S
)
set(TILEFINCH_PORTABILITY_SOURCES
    ${TILEFINCH_CORE_SOURCES}
    ${TILEFINCH_PSP_FRONTEND_SOURCES}
)
list(REMOVE_DUPLICATES TILEFINCH_PORTABILITY_SOURCES)
string(REPLACE ";" "\n" TILEFINCH_PORTABILITY_SOURCE_MANIFEST
       "${TILEFINCH_PORTABILITY_SOURCES}")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/tilefinch-portability-sources.txt"
    CONTENT "${TILEFINCH_PORTABILITY_SOURCE_MANIFEST}\n")

add_library(tilefinch_core ${TILEFINCH_CORE_SOURCES})
target_include_directories(tilefinch_core PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(PSP)
    enable_language(ASM)
    target_sources(tilefinch_core PRIVATE
        src/media_backend_psp.c
        src/media_backend_psp_swdec.c)
    add_library(tilefinch_psp_media_imports STATIC
        src/media_backend_psp_imports.S)
    # Project-authored SystemCtrlForUser stubs replace the GPL-3.0
    # psp-cfw-sdk archive; see THIRD_PARTY_NOTICES.md.
    add_library(tilefinch_psp_systemctrl_imports STATIC
        src/systemctrl_user_imports.S)
    target_link_libraries(tilefinch_core PUBLIC
        tilefinch_psp_media_imports tilefinch_psp_systemctrl_imports
        pspaudiocodec pspaudio psputility)
endif()

if(PSP)
    find_program(TILEFINCH_PSP_OBJDUMP psp-objdump
        HINTS "$ENV{PSPDEV}/bin"
        REQUIRED)
    add_custom_target(check_tilefinch_bootstrap_generated
        COMMAND ${CMAKE_COMMAND}
            -DTILEFINCH_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DTILEFINCH_BOOTSTRAP_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/generated.sha256
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyBootstrapManifest.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/generated.sha256"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap_bytecode.c"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking PSP bootstrap source/artifact manifest")
    add_dependencies(tilefinch_core check_tilefinch_bootstrap_generated)
else()
    set(TILEFINCH_BOOTSTRAP_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap")
    file(GLOB TILEFINCH_BOOTSTRAP_INPUTS CONFIGURE_DEPENDS
        "${TILEFINCH_BOOTSTRAP_SOURCE_DIR}/*.js")
    list(APPEND TILEFINCH_BOOTSTRAP_INPUTS
        "${TILEFINCH_BOOTSTRAP_SOURCE_DIR}/sources.def")
    add_executable(tilefinch_bootstrap_bytecode_generator
        tools/bootstrap_bytecode_generator.c)
    target_include_directories(tilefinch_bootstrap_bytecode_generator PRIVATE
        include src)
    target_link_libraries(tilefinch_bootstrap_bytecode_generator PRIVATE
        qjs)
    add_custom_target(regenerate_tilefinch_bootstrap
        COMMAND tilefinch_bootstrap_bytecode_generator
            "${TILEFINCH_BOOTSTRAP_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap_bytecode.c"
        COMMAND ${CMAKE_COMMAND}
            -DTILEFINCH_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DTILEFINCH_BOOTSTRAP_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/generated.sha256
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteBootstrapManifest.cmake"
        DEPENDS tilefinch_bootstrap_bytecode_generator ${TILEFINCH_BOOTSTRAP_INPUTS}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Regenerating embedded browser bootstrap sources and bytecode")
    add_custom_target(check_tilefinch_bootstrap_generated
        COMMAND tilefinch_bootstrap_bytecode_generator --check
            "${TILEFINCH_BOOTSTRAP_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap_bytecode.c"
            "${CMAKE_CURRENT_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND}
            -DTILEFINCH_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DTILEFINCH_BOOTSTRAP_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/generated.sha256
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyBootstrapManifest.cmake"
        DEPENDS tilefinch_bootstrap_bytecode_generator
            ${TILEFINCH_BOOTSTRAP_INPUTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/generated.sha256"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking embedded browser bootstrap generated files")
    add_dependencies(tilefinch_core check_tilefinch_bootstrap_generated)
endif()

target_include_directories(tilefinch_core PUBLIC include PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${stb_SOURCE_DIR}" "${nanosvg_SOURCE_DIR}/src"
    "${libwebp_SOURCE_DIR}/src")
target_link_libraries(tilefinch_core PUBLIC webpdecoder)
if(NOT PSP_BROWSER_ENABLE_GIF)
    # Consumers need the same advertised image capability set as tilefinch_core;
    # tests and frontends must not assume GIF appears in Accept when its
    # decoder was compiled out.
    target_compile_definitions(tilefinch_core PUBLIC TILEFINCH_DISABLE_GIF=1)
endif()
if(PSP_BROWSER_DISABLE_TRACE)
    target_compile_definitions(tilefinch_core PRIVATE TILEFINCH_NO_TRACE=1)
endif()
if(PSP AND TILEFINCH_PSP_VALIDATION_LOG)
    # Device-library diagnostics must use the same explicit opt-in as the
    # frontend log sink. Ordinary release builds compile both the formatter
    # and its argument evaluation away.
    target_compile_definitions(
        tilefinch_core PRIVATE TILEFINCH_PSP_VALIDATION_LOG=1)
endif()
if(PSP AND TILEFINCH_PSP_VALIDATION_LOG
       AND TILEFINCH_PSP_MEDIA_PICTURE_TRACE)
    target_compile_definitions(
        tilefinch_core PRIVATE TILEFINCH_PSP_MEDIA_PICTURE_TRACE=1)
endif()
if(NOT PSP_BROWSER_EMBED_BOOTSTRAP_SOURCE_FALLBACK)
    target_compile_definitions(
        tilefinch_core PRIVATE TILEFINCH_BOOTSTRAP_BYTECODE_ONLY=1)
endif()
if(PSP_BROWSER_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT tilefinch_ipo_supported OUTPUT tilefinch_ipo_error)
    if(tilefinch_ipo_supported)
        set_property(TARGET tilefinch_core PROPERTY
                     INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "LTO requested but unsupported: ${tilefinch_ipo_error}")
    endif()
endif()
if(PSP_BROWSER_CURL_IMPERSONATE_LIBRARY AND NOT PSP_BROWSER_LIBCURL_TRANSPORT)
    message(FATAL_ERROR
        "curl impersonation requires the bundled libcurl transport backend")
elseif(PSP_BROWSER_CURL_IMPERSONATE_LIBRARY)
    target_link_libraries(tilefinch_core PUBLIC lexbor_static qjs
        "${PSP_BROWSER_CURL_IMPERSONATE_LIBRARY}")
    target_compile_definitions(tilefinch_core PRIVATE
        TILEFINCH_CURL_IMPERSONATE=1
        TILEFINCH_CURL_IMPERSONATE_TARGET="${PSP_BROWSER_CURL_IMPERSONATE_TARGET}")
elseif(PSP_BROWSER_LIBCURL_TRANSPORT)
    if(PSP AND PSP_BROWSER_CURL_STUB)
        # Hermetic replay never issues a transfer, so a no-op stub TU can
        # satisfy fetch.c's curl references and drop the SDK's static
        # libcurl + mbedtls + zlib (~1MB of text the 64MB device keeps).
        target_sources(tilefinch_core PRIVATE src/fetch_curl_stub_psp.c)
        target_link_libraries(tilefinch_core PUBLIC lexbor_static qjs)
    elseif(PSP)
        if(NOT EXISTS "${PSP_BROWSER_PSP_CA_BUNDLE}")
            message(FATAL_ERROR
                "Real PSP networking requires PSP_BROWSER_PSP_CA_BUNDLE: ${PSP_BROWSER_PSP_CA_BUNDLE}")
        endif()
        target_link_libraries(tilefinch_core PUBLIC lexbor_static qjs
            ${TILEFINCH_PSP_TRANSPORT_LIBRARIES})
    else()
        target_link_libraries(tilefinch_core PUBLIC lexbor_static qjs
            CURL::libcurl)
    endif()
else()
    target_link_libraries(tilefinch_core PUBLIC lexbor_static qjs)
endif()
if(PSP)
    target_sources(tilefinch_core PRIVATE src/update_crypto_mbedtls.c)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_sources(tilefinch_core PRIVATE src/update_crypto_openssl.c)
    target_link_libraries(tilefinch_core PUBLIC OpenSSL::Crypto)
endif()
if(PSP_BROWSER_USE_BELLARD_QUICKJS)
    target_compile_definitions(tilefinch_core PUBLIC PSP_BROWSER_BELLARD_QUICKJS=1)
endif()
if(PSP_BROWSER_FREETYPE_AVAILABLE)
    target_compile_definitions(tilefinch_core PRIVATE TILEFINCH_HAVE_FREETYPE=1)
    if(TARGET Freetype::Freetype)
        target_link_libraries(tilefinch_core PUBLIC Freetype::Freetype)
    elseif(TARGET freetype)
        target_link_libraries(tilefinch_core PUBLIC freetype)
    else()
        target_include_directories(tilefinch_core PRIVATE ${FREETYPE_INCLUDE_DIRS})
        target_link_libraries(tilefinch_core PUBLIC ${FREETYPE_LIBRARIES})
    endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(tilefinch_core PRIVATE -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
    set_property(SOURCE src/generated/js_bootstrap.c APPEND PROPERTY
        COMPILE_OPTIONS -Wno-overlength-strings)
endif()
