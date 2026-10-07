add_executable(psp-browser-lab src/main.c)
target_link_libraries(psp-browser-lab PRIVATE tilefinch_core)
target_compile_definitions(psp-browser-lab PRIVATE
    TILEFINCH_PROFILE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/profiles"
    TILEFINCH_SANS_FONT="${PSP_BROWSER_SANS_FONT}"
    TILEFINCH_SERIF_FONT="${PSP_BROWSER_SERIF_FONT}"
    TILEFINCH_SANS_ITALIC_FONT="${PSP_BROWSER_SANS_ITALIC_FONT}"
    TILEFINCH_SANS_BOLD_FONT="${PSP_BROWSER_SANS_BOLD_FONT}"
    TILEFINCH_SERIF_BOLD_FONT="${PSP_BROWSER_SERIF_BOLD_FONT}"
    TILEFINCH_METRIC_SANS_FONT="${PSP_BROWSER_METRIC_SANS_FONT}"
    TILEFINCH_METRIC_SANS_BOLD_FONT="${PSP_BROWSER_METRIC_SANS_BOLD_FONT}")

add_executable(psp-browser-interactive-lab src/interactive_main.c)
target_link_libraries(psp-browser-interactive-lab PRIVATE tilefinch_core)
if(NOT PSP)
    add_executable(psp-browser-media-probe tools/media_mp4_probe.c)
    target_link_libraries(psp-browser-media-probe PRIVATE tilefinch_core)
    add_executable(tilefinch-youtube-resolver-probe
        tools/youtube_resolver_probe.c)
    target_link_libraries(tilefinch-youtube-resolver-probe
        PRIVATE tilefinch_core)
    add_executable(tilefinch-offline-library-fixture
        tools/offline_library_fixture.c)
    target_link_libraries(tilefinch-offline-library-fixture
        PRIVATE tilefinch_core)
    # Global camera motion between video frames for
    # scripts/analyze-game-video.py (recorded PPSSPP gameplay).
    add_executable(tilefinch-video-motion tools/video_motion.c)
    if(NOT WIN32)
        target_link_libraries(tilefinch-video-motion PRIVATE m)
    endif()
    # Short on-off-on blinks of small regions (the analyzer's blink check).
    add_executable(tilefinch-video-blink tools/video_blink.c)
endif()
if(NOT PSP AND PSP_BROWSER_BUILD_HOST_MEDIA_LAB)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TILEFINCH_FFMPEG QUIET IMPORTED_TARGET
            libavformat libavcodec libavutil libswscale libswresample)
        pkg_check_modules(TILEFINCH_SDL2 QUIET IMPORTED_TARGET sdl2)
    endif()
    if(TARGET PkgConfig::TILEFINCH_FFMPEG)
        target_sources(psp-browser-interactive-lab PRIVATE src/host_media.c)
        target_link_libraries(psp-browser-interactive-lab PRIVATE
            PkgConfig::TILEFINCH_FFMPEG)
        target_compile_definitions(psp-browser-interactive-lab PRIVATE
            TILEFINCH_HAVE_HOST_MEDIA=1)
        if(PSP_BROWSER_ENABLE_HOST_MEDIA_AUDIO
           AND TARGET PkgConfig::TILEFINCH_SDL2)
            target_link_libraries(psp-browser-interactive-lab PRIVATE
                PkgConfig::TILEFINCH_SDL2)
            target_compile_definitions(psp-browser-interactive-lab PRIVATE
                TILEFINCH_HAVE_SDL_AUDIO=1)
            message(STATUS "Host media audio: SDL2 enabled")
        elseif(PSP_BROWSER_ENABLE_HOST_MEDIA_AUDIO)
            message(STATUS
                "Host media audio: decode-only (SDL2 development library not found)")
        else()
            message(STATUS
                "Host media audio: decode-only (disabled for this configuration)")
        endif()
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            set_property(SOURCE src/host_media.c APPEND PROPERTY
                COMPILE_OPTIONS
                -Wall -Wextra -Wpedantic
                -Werror=implicit-function-declaration)
        endif()
        message(STATUS "Host video lab: FFmpeg enabled")
    else()
        message(STATUS
            "Host video lab: disabled (FFmpeg development libraries not found)")
    endif()
endif()

add_executable(psp-browser-failure-recovery src/failure_recovery_main.c)
target_link_libraries(psp-browser-failure-recovery PRIVATE tilefinch_core)

# Isolates the per-keypress cost of focus movement against scrolling, with
# and without the script runtime. Not a test: it reports numbers rather than
# asserting them, because the absolute values are host-specific.
add_executable(tilefinch-input-latency-bench tools/input_latency_bench.c)
target_link_libraries(tilefinch-input-latency-bench PRIVATE tilefinch_core)

# JavaScript engine micro-benchmark, host side of the PSP validation mode
# (boot.cfg validation_js_bench=N). Same kernels, same allocator path.
add_executable(tilefinch-js-bench tools/js_bench_main.c src/js_bench.c)
target_link_libraries(tilefinch-js-bench PRIVATE tilefinch_core)
target_include_directories(tilefinch-js-bench PRIVATE include)

if(PSP)
    # These are host diagnostic frontends. Keep them individually available
    # for unusual toolchain experiments, but do not let a bare PSP aggregate
    # build try to link host time and 64-bit atomic facilities.
    set_target_properties(
        psp-browser-lab
        psp-browser-interactive-lab
        tilefinch-input-latency-bench
        tilefinch-js-bench
        PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# User-triggered diagnostic export. It is deliberately separate from the
# engine library: normal browsing never needs the QR encoder or zlib entry
# points, while the PSP browser and the focused host tests do.
add_library(tilefinch_diagnostic_qr STATIC
    src/diagnostic_qr.c
    third_party/qrcodegen/qrcodegen.c)
target_include_directories(tilefinch_diagnostic_qr
    PUBLIC include
    PRIVATE third_party/qrcodegen)
if(PSP)
    target_link_libraries(tilefinch_diagnostic_qr PUBLIC z)
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(tilefinch_diagnostic_qr PUBLIC ZLIB::ZLIB)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(tilefinch_diagnostic_qr PRIVATE
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
endif()

add_library(tilefinch_psp_ui STATIC
    src/psp_basic_fallback.c
    src/psp_reader_policy.c
    src/psp_ui.c
    src/psp_ui_action.c
    src/psp_ui_theme.c
    src/psp_ui_menu.c
    src/psp_ui_media_8888.c
    src/psp_power_policy.c)
target_include_directories(tilefinch_psp_ui PUBLIC include)
target_link_libraries(tilefinch_psp_ui PUBLIC tilefinch_core)

add_library(tilefinch_psp_app_support STATIC
    src/psp_boot_config.c
    src/psp_boot_order.c
    src/psp_lifecycle.c
    src/psp_profile_store.c
    src/psp_glyph_component_session.c
    src/psp_voice_component_session.c
    src/psp_update_session.c)
target_include_directories(tilefinch_psp_app_support PUBLIC include)
target_link_libraries(tilefinch_psp_app_support PUBLIC
    tilefinch_core tilefinch_psp_ui)
target_compile_definitions(tilefinch_psp_app_support PRIVATE
    TILEFINCH_UPDATE_REPOSITORY_OWNER="${TILEFINCH_UPDATE_REPOSITORY_OWNER}"
    TILEFINCH_UPDATE_REPOSITORY_NAME="${TILEFINCH_UPDATE_REPOSITORY_NAME}"
    TILEFINCH_VOICE_COMPONENT_REPOSITORY_OWNER="${TILEFINCH_VOICE_COMPONENT_REPOSITORY_OWNER}"
    TILEFINCH_VOICE_COMPONENT_REPOSITORY_NAME="${TILEFINCH_VOICE_COMPONENT_REPOSITORY_NAME}"
    TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER="${TILEFINCH_GLYPH_COMPONENT_REPOSITORY_OWNER}"
    TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME="${TILEFINCH_GLYPH_COMPONENT_REPOSITORY_NAME}")
target_link_libraries(psp-browser-interactive-lab PRIVATE tilefinch_psp_ui)
if(PSP AND TILEFINCH_PSP_VALIDATION_LOG)
    target_compile_definitions(tilefinch_psp_ui PRIVATE
        TILEFINCH_PSP_POWER_TEST_MENU=1
        TILEFINCH_PSP_UI_TIMING=1)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(tilefinch_psp_ui PRIVATE
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
endif()

# Scanout front end shared by every PSP executable. Both EBOOTs and the host
# tests link the same object so the fixture and the browser cannot drift apart
# on display mode, buffer rotation, sync flag, or result checking again.
add_library(tilefinch_psp_display STATIC src/psp_display.c)
target_include_directories(tilefinch_psp_display PUBLIC include)
if(PSP AND TILEFINCH_PSP_VALIDATION_LOG)
    # The display library is shared with the launcher, so it cannot depend on
    # the browser logger directly. Compile the bounded hash probe here; the
    # browser installs its logger callback only after the persistent sink is
    # ready, while other consumers leave it inert.
    target_compile_definitions(tilefinch_psp_display PRIVATE
        TILEFINCH_PSP_LATCH_PROBE=1 TILEFINCH_PSP_VALIDATION_LOG=1)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(tilefinch_psp_display PRIVATE
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
endif()

# Video presentation scaler. Shared with the host tests so the tables and the
# converted output can be proven byte for byte off-device, and built at -O2
# even in a MinSizeRel image: this is the single hottest main-CPU loop in a
# media session (10.1 ms per presented frame before this file existed), and
# -Os leaves the loop unrotated for the sake of a few hundred bytes.
add_library(tilefinch_psp_media_scale STATIC src/psp_media_scale.c)
target_include_directories(tilefinch_psp_media_scale PUBLIC include)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(tilefinch_psp_media_scale PRIVATE
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration -O2)
endif()

# Where a decoded frame lands and, on a PSP, the graphics engine that draws it
# there. The geometry is pure and host-tested; the GU translation unit
# compiles to a stub that answers "no graphics engine" off-device, so the host
# tests link the same call the device takes.
add_library(tilefinch_psp_media_present STATIC
    src/psp_media_present.c
    src/psp_media_present_ge.c)
target_include_directories(tilefinch_psp_media_present PUBLIC include)
# Only the validation-only probe needs it: it draws the same synthetic frame
# through both presenters and requires them to agree pixel for pixel.
target_link_libraries(tilefinch_psp_media_present PUBLIC
    tilefinch_psp_media_scale)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(tilefinch_psp_media_present PRIVATE
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
endif()

add_library(tilefinch_voice_frontend STATIC
    src/voice_model_policy.c
    src/voice_job_lifecycle.c
    src/stt/audio_gate.c
    src/stt/resampler.c)
target_include_directories(tilefinch_voice_frontend PUBLIC include src/stt)
target_compile_definitions(tilefinch_voice_frontend PRIVATE
    TILEFINCH_VOICE_SENDUMP_ROWS=${TILEFINCH_VOICE_SENDUMP_ROWS}
    TILEFINCH_VOICE_SENDUMP_ROW_BYTES=${TILEFINCH_VOICE_SENDUMP_ROW_BYTES})
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(tilefinch_voice_frontend PRIVATE
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
endif()

if(NOT PSP)
    add_executable(psp-browser-ui-preview src/psp_ui_preview.c)
    target_link_libraries(psp-browser-ui-preview PRIVATE
        tilefinch_psp_ui tilefinch_diagnostic_qr)
endif()

if(PSP)
    # Optional ARK-4 VSH plugin. It observes the firmware HTML-viewer module
    # start, then hands off to the stable root launcher. It is deliberately a
    # separate kernel PRX: ordinary Tilefinch builds never load privileged
    # code, and the in-app A/B updater remains unaware of /SEPLUGINS.
    add_prx_module(tilefinch-xmb-redirect
        src/psp_xmb_redirect.c
        src/xmb_redirect_policy.c)
    target_include_directories(tilefinch-xmb-redirect PRIVATE include)
    target_compile_options(tilefinch-xmb-redirect PRIVATE
        -Os -G0 -fno-pic
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration
        "SHELL:-isystem ${PSPDEV}/psp/sdk/include")
    target_link_options(tilefinch-xmb-redirect PRIVATE
        -nostdlib
        "${PSPDEV}/psp/sdk/lib/prxexports.o")
    target_link_libraries(tilefinch-xmb-redirect PRIVATE
        pspsystemctrl_kernel
        pspdebug pspdisplay_driver pspctrl_driver
        pspmodinfo pspsdk pspkernel)
    set_target_properties(tilefinch-xmb-redirect PROPERTIES
        OUTPUT_NAME tilefinch_xmb
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/xmb-redirect")
    set_property(TARGET tilefinch-xmb-redirect APPEND PROPERTY LINK_DEPENDS
        "${PSPDEV}/psp/sdk/lib/prxexports.o")

    # Stable, deliberately small A/B launcher. It owns no network or browser
    # engine code: on trial boots it re-verifies signed Stable/Beta slots from
    # the embedded root, or every digest in an explicitly marked unsigned
    # Developer slot, then LoadExecs the selected browser.
    add_executable(tilefinch-launcher
        src/update_launcher_psp.c
        src/install_paths.c
        src/sha256.c
        src/update_manifest.c
        src/update_package.c
        src/update_state.c
        src/update_journal.c
        src/update_slot.c
        src/update_root_embedded.c
        src/update_crypto_mbedtls.c
        src/psp_time.c
        src/psp_time_policy.c)
    target_include_directories(tilefinch-launcher PRIVATE
        include "${CMAKE_CURRENT_BINARY_DIR}/generated")
    if(TILEFINCH_PSP_VALIDATION_LOG)
        # Launcher phase stamps go only to stdout/PSPLink. They deliberately
        # do not start the browser's Memory Stick validation logger.
        target_compile_definitions(tilefinch-launcher PRIVATE
            TILEFINCH_PSP_LAUNCHER_TIMING=1)
    endif()
    target_compile_options(tilefinch-launcher PRIVATE -flto)
    target_link_options(tilefinch-launcher PRIVATE -flto)
    target_link_libraries(tilefinch-launcher PRIVATE
        tilefinch_psp_display tilefinch_psp_systemctrl_imports
        ${TILEFINCH_PSP_CRYPTO_LIBRARIES}
        tilefinch_psp_entropy
        pspdisplay pspge pspctrl psprtc)
    set_target_properties(tilefinch-launcher PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher")
    if(TILEFINCH_STRIP_RELEASE_EBOOT)
        # create_pbp_file packages the target in place. Strip the launcher's
        # DWARF just as we do the browser, retaining a local symbol sidecar for
        # crash analysis and size inspection. This removes no loadable bytes.
        add_custom_command(TARGET tilefinch-launcher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:tilefinch-launcher>
                $<TARGET_FILE:tilefinch-launcher>.unstripped
            COMMAND "${PSPDEV}/bin/psp-strip"
                $<TARGET_FILE:tilefinch-launcher>
            COMMENT
                "Stripping packaged PSP launcher (keeping .unstripped symbols)")
    endif()
    create_pbp_file(
        TARGET tilefinch-launcher TITLE "Tilefinch"
        ICON_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/tilefinch-icon0-144x82.png")
    # pack-pbp runs as a POST_BUILD step, so nothing reruns it when only the
    # icon changes: a re-themed ICON0 would sit in the tree while every
    # packaged EBOOT still carried the old one. Make the link depend on it.
    set_property(TARGET tilefinch-launcher APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/tilefinch-icon0-144x82.png")
    add_custom_command(TARGET tilefinch-launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DPSP_OBJDUMP=${TILEFINCH_PSP_OBJDUMP}
            -DPSP_ELF=$<TARGET_FILE:tilefinch-launcher>
            -DPSP_TEXT_LIMIT=262144
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPspTextSize.cmake"
        COMMENT "Checking the stable launcher 256 KiB .text ratchet")

    # Qualification EBOOT (docs/engineering/DEVICE_QUALIFICATION.md): renders the embedded fixture
    # through the standard pipeline and prints the deterministic counters
    # for the host cross-check.
    set(PSP_FIXTURE_HEADER
        "${CMAKE_CURRENT_BINARY_DIR}/generated/psp_fixture_html.h")
    add_custom_command(
        OUTPUT "${PSP_FIXTURE_HEADER}"
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/float-article.html
            -DOUTPUT=${PSP_FIXTURE_HEADER}
            -DNAME=psp_fixture_html
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
        DEPENDS fixtures/float-article.html cmake/bin2c.cmake
        COMMENT "Embedding float-article fixture")
    add_executable(psp-browser-fixture src/psp_main.c src/psp_atomic_shims.c
        "${PSP_FIXTURE_HEADER}")
    target_include_directories(psp-browser-fixture PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(psp-browser-fixture PRIVATE tilefinch_core tilefinch_psp_ui
        tilefinch_psp_display m
        pspdisplay pspge pspctrl)
    # create_pbp_file writes EBOOT.PBP beside the target, and every script and
    # doc means the browser when it says build-preset-psp/EBOOT.PBP.  Give the
    # fixture its own directory so building it cannot silently replace the
    # browser EBOOT that validation then runs.
    set_target_properties(psp-browser-fixture PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture")
    create_pbp_file(TARGET psp-browser-fixture TITLE "Tilefinch Fixture")
    add_custom_command(TARGET psp-browser-fixture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:psp-browser-fixture>/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PSP_BROWSER_SANS_FONT}"
            "${PSP_BROWSER_SERIF_FONT}"
            "${PSP_BROWSER_SANS_ITALIC_FONT}"
            "${PSP_BROWSER_SANS_BOLD_FONT}"
            "${PSP_BROWSER_SERIF_BOLD_FONT}"
            "${PSP_BROWSER_METRIC_SANS_FONT}"
            "${PSP_BROWSER_METRIC_SANS_BOLD_FONT}"
            $<TARGET_FILE_DIR:psp-browser-fixture>/fonts
        COMMENT "Staging fonts beside the EBOOT")

    # Crypto selftest EBOOT: the device-side gate for the Allegrex bignum
    # core (docs/engineering/PSP_TRANSPORT.md). A sibling
    # of the qualification fixture -- same "print counters to stdout, let
    # PPSSPP's log capture them" harness shape, its own output directory
    # so it can never replace the browser or fixture EBOOT, and no engine
    # dependency at all. Driven by scripts/run-ppsspp-crypto-selftest.sh.
    if(TILEFINCH_PSP_TRANSPORT_IS_OWNED)
        add_executable(psp-crypto-selftest
            src/psp_crypto_selftest_main.c)
        target_include_directories(psp-crypto-selftest PRIVATE include)
        target_link_libraries(psp-crypto-selftest PRIVATE
            ${TILEFINCH_PSP_CRYPTO_LIBRARIES}
            pspdebug tilefinch_psp_display pspdisplay pspge)
        if(TILEFINCH_PSP_ALLEGREX_BIGNUM_ASM)
            target_compile_definitions(psp-crypto-selftest PRIVATE
                TILEFINCH_ALLEGREX_MULADDC=1)
        endif()
        # Everest changes the mbedtls_ecdh_context layout and gates its
        # x25519.h / Hacl declarations, so the EBOOT must see the same macro
        # the mbedTLS library was built with. Matches the -D passed in
        # cmake/PspOwnedTransport.cmake.
        if(TILEFINCH_PSP_EVEREST_X25519)
            target_compile_definitions(psp-crypto-selftest PRIVATE
                MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED=1)
        endif()
        set_target_properties(psp-crypto-selftest PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY
                "${CMAKE_CURRENT_BINARY_DIR}/crypto-selftest")
        create_pbp_file(TARGET psp-crypto-selftest
            TITLE "Tilefinch Crypto Selftest")
    endif()

    if(PSP_BROWSER_LIBCURL_TRANSPORT)
        # Full browser EBOOT: navigation, QuickJS, hermetic replay,
        # controller interaction, and PSP-native chrome.
        add_executable(psp-browser-script
            src/psp_script_main.c
            src/psp_app/psp_app_actions.c
            src/psp_app/psp_app_captive_portal.c
            src/psp_app/psp_app_input.c
            src/psp_app/psp_app_settings.c
            src/psp_app/psp_app_storage.c
            src/psp_app/psp_app_heavy.c
            src/psp_app/psp_app_network.c
            src/psp_app/psp_app_page.c
            src/psp_app/psp_app_runtime.c
            src/psp_canvas_ge.c
            src/psp_app/psp_app_surfaces.c
            src/psp_app/psp_app_youtube.c
            src/psp_app/psp_app_exit_handoff.c
            src/psp_app/psp_app_frame_pumps.c
            src/psp_app/psp_app_glyph_component.c
            src/psp_app/psp_app_voice_component.c
            src/psp_clock_worker.c
            src/psp_log.c
            src/psp_media_buffering.c
            src/psp_media_open.c
            src/psp_media_hls.c
            src/psp_hls_gzip.c
            src/psp_media_present_session.c
            src/psp_media_seek.c
            src/psp_media_session.c
            src/psp_media_telemetry.c
            src/psp_swdec_component.c
            src/psp_offline_store.c
            src/screenshot_png.c
            src/psp_text_input.c
            src/psp_atomic_shims.c)
        if(TILEFINCH_PSP_VALIDATION_LOG AND NOT PSP_BROWSER_CURL_STUB)
            target_sources(psp-browser-script PRIVATE src/psp_update_e2e.c)
            target_sources(psp-browser-script PRIVATE src/psp_transport_probe.c)
            set(_transport_probe_link_options)
            foreach(_symbol curl_multi_perform curl_multi_poll curlx_nonblock gethostbyname connect select recv send
                            close sceKernelWaitSema sceKernelDelayThread sceNetInetSelect
                            mbedtls_ssl_set_session mbedtls_ssl_get_session
                            mbedtls_ssl_read mbedtls_ssl_handshake)
                list(APPEND _transport_probe_link_options "LINKER:--wrap=${_symbol}")
            endforeach()
            target_link_options(psp-browser-script PRIVATE ${_transport_probe_link_options})
        endif()
        if(PSP_BROWSER_QUICKJS_PGO_GENERATE)
            # The clean exit writes the engine's profile counters
            # (TilefinchDependencies.cmake, PSP_BROWSER_QUICKJS_PGO_GENERATE).
            target_compile_definitions(psp-browser-script PRIVATE
                TILEFINCH_PSP_PGO_GENERATE=1)
        endif()
        target_link_libraries(psp-browser-script PRIVATE tilefinch_core
            tilefinch_psp_ui tilefinch_psp_display
            tilefinch_psp_media_scale
            tilefinch_psp_media_present
            tilefinch_psp_app_support tilefinch_diagnostic_qr)
        # newlib's integer-only printf cores are pulled in only by newlib
        # itself (assert -> fiprintf, strftime -> sniprintf), while the
        # browser already links the full _vfprintf_r and _svfprintf_r for its
        # own printf/snprintf calls. The full cores print every integer-only
        # format identically, so resolve the integer-only entry points to them
        # instead of linking a second copy of each (11.5 KB of .text). Only the
        # browser does this: in an image without its own printf, the alias
        # would pull the larger core in.
        target_link_options(psp-browser-script PRIVATE
            "LINKER:--defsym=_vfiprintf_r=_vfprintf_r"
            "LINKER:--defsym=_svfiprintf_r=_svfprintf_r")
        # Code-layout experiments (PERFORMANCE_LEDGER.md, "Profile-ordered
        # code layout"). Every function already has its own section
        # (-ffunction-sections), so a
        # GNU ld --section-ordering-file places a profiled hot set first and
        # contiguous in .text, inside as few 8 KiB I-cache ways as it needs.
        # Link-only: the objects, and so the instructions, are unchanged.
        # Empty (the default) leaves the link exactly as before.
        set(PSP_BROWSER_PSP_SECTION_ORDERING_FILE "" CACHE FILEPATH
            "GNU ld section ordering file for the PSP browser links (experiment)")
        option(PSP_BROWSER_PSP_LINK_MAP
            "Write a linker map beside the PSP browser ELF and dev PRX" OFF)
        set(TILEFINCH_PSP_LAYOUT_LINK_OPTIONS)
        if(PSP_BROWSER_PSP_SECTION_ORDERING_FILE)
            list(APPEND TILEFINCH_PSP_LAYOUT_LINK_OPTIONS
                "LINKER:--section-ordering-file,${PSP_BROWSER_PSP_SECTION_ORDERING_FILE}")
            set_property(TARGET psp-browser-script APPEND PROPERTY
                LINK_DEPENDS "${PSP_BROWSER_PSP_SECTION_ORDERING_FILE}")
        endif()
        if(TILEFINCH_PSP_LAYOUT_LINK_OPTIONS)
            target_link_options(psp-browser-script PRIVATE
                ${TILEFINCH_PSP_LAYOUT_LINK_OPTIONS})
        endif()
        if(PSP_BROWSER_PSP_LINK_MAP)
            target_link_options(psp-browser-script PRIVATE
                "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/psp-browser-script.map")
        endif()
        if(TARGET tilefinch-wasm-component)
            # This is a runtime asset, not a link input.  A POST_BUILD copy
            # alone goes stale when only the component changes and the EBOOT
            # is already current, so make staging an always-evaluated target
            # in the browser's dependency chain.
            add_custom_target(tilefinch-wasm-component-stage
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${TILEFINCH_WASM_COMPONENT_PRX}"
                    "$<TARGET_FILE_DIR:psp-browser-script>/tilefinch-wasm.prx"
                DEPENDS tilefinch-wasm-component
                COMMENT "Staging the lazy WebAssembly component")
            add_dependencies(
                psp-browser-script tilefinch-wasm-component-stage)
        endif()
        # src/psp_app/ holds this EBOOT's private seams. They include
        # src/media_backend_psp_policy.h and src/psp_media_pixels.h, which
        # tilefinch_core keeps PRIVATE, so the executable needs src itself.
        target_include_directories(psp-browser-script PRIVATE src)
        target_compile_definitions(psp-browser-script PRIVATE
            TILEFINCH_UPDATE_REPOSITORY_OWNER="${TILEFINCH_UPDATE_REPOSITORY_OWNER}"
            TILEFINCH_UPDATE_REPOSITORY_NAME="${TILEFINCH_UPDATE_REPOSITORY_NAME}")
        if(TILEFINCH_PSP_ENABLE_SWDEC_COMPONENT)
            add_dependencies(psp-browser-script tilefinch-swdec-bundle)
        endif()
        # Older build trees staged the optional PRXs beside EBOOT.PBP. Remove
        # those stale outputs on every browser link: current builds emit only
        # the separate components/swdec add-on bundle.
        add_custom_command(TARGET psp-browser-script POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f
                $<TARGET_FILE_DIR:psp-browser-script>/tilefinch-swdec.prx
                $<TARGET_FILE_DIR:psp-browser-script>/swdec-meload.prx
            COMMENT "Removing legacy slot-local decoder components")
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(psp-browser-script PRIVATE
                -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
            # pspdev.cmake hands the SDK headers to every target with a
            # plain -I, and -Wpedantic flags their out-of-range enumerators.
            # Re-listing the directory with -isystem makes GCC treat it as a
            # system directory (the duplicate -I is then ignored), keeping
            # these warnings pointed at this target's own sources.
            if(PSP)
                target_compile_options(psp-browser-script PRIVATE
                    "SHELL:-isystem ${PSPDEV}/psp/sdk/include")
            endif()
        endif()
        # Persistent Memory Stick logging costs a rotation and a device
        # synchronize before first paint (~5.6s measured on a PSP-3000) plus
        # another synchronize per checkpoint. Validation builds want it; a
        # homebrew user booting the browser does not.
        if(TILEFINCH_PSP_VALIDATION_LOG)
            set(TILEFINCH_MEDIA_FIXTURE_240
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/psp-media/baseline-320x240.mp4")
            set(TILEFINCH_MEDIA_FIXTURE_360
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/psp-media/main-640x360.mp4")
            set(_tilefinch_media_fixture_blob
                "${CMAKE_CURRENT_BINARY_DIR}/generated/psp_media_fixture_blob.S")
            configure_file(
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PspMediaFixtureBlob.S.in"
                "${_tilefinch_media_fixture_blob}" @ONLY)
            target_compile_definitions(psp-browser-script PRIVATE
                TILEFINCH_PSP_VALIDATION_LOG=1)
            if(TILEFINCH_PSP_MEDIA_PICTURE_TRACE)
                target_compile_definitions(psp-browser-script PRIVATE
                    TILEFINCH_PSP_MEDIA_PICTURE_TRACE=1)
            endif()
            # The scripted-input harness. Added here rather than to a shared
            # library so a shipping EBOOT links neither the parser nor the
            # name tables it prints; main()'s only calls into it are inside
            # the same TILEFINCH_PSP_VALIDATION_LOG guard.
            target_sources(psp-browser-script PRIVATE
                src/psp_input_script.c
                src/psp_app/psp_app_input_script.c
                src/psp_webgl_ge_probe.c
                src/js_bench.c
                src/psp_media_fixture.c
                src/psp_media_range_probe.c
                src/psp_raster_fixture.c
                "${_tilefinch_media_fixture_blob}")
            target_link_libraries(psp-browser-script PRIVATE pspgum)
            set_property(TARGET psp-browser-script APPEND PROPERTY
                LINK_DEPENDS
                "${TILEFINCH_MEDIA_FIXTURE_240}"
                "${TILEFINCH_MEDIA_FIXTURE_360}")
        endif()
        if(PSP_BROWSER_ENABLE_PSP_VOICE)
            target_sources(psp-browser-script PRIVATE
                src/psp_voice_input.c
                src/stt/stt_component_loader.c)
            target_include_directories(psp-browser-script PRIVATE src/stt)
            target_compile_definitions(
                psp-browser-script PRIVATE TILEFINCH_HAVE_PSP_VOICE=1)
            target_link_libraries(
                psp-browser-script PRIVATE
                tilefinch_voice_frontend pspaudio)
            add_custom_target(tilefinch-voice-component-stage
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${TILEFINCH_VOICE_COMPONENT_PRX}"
                    "$<TARGET_FILE_DIR:psp-browser-script>/tilefinch-voice.prx"
                DEPENDS tilefinch-voice-component
                COMMENT "Staging the lazy speech component")
            add_dependencies(psp-browser-script tilefinch-voice-component-stage)
            add_executable(psp-voice-component-probe EXCLUDE_FROM_ALL
                src/stt/stt_component_probe.c src/stt/stt_component_loader.c)
            target_include_directories(psp-voice-component-probe PRIVATE src/stt)
            target_link_libraries(psp-voice-component-probe PRIVATE
                tilefinch_core pspuser pspsdk m)
            set_target_properties(psp-voice-component-probe PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/voice-probe")
            create_pbp_file(TARGET psp-voice-component-probe
                TITLE "Tilefinch Voice Component Probe")
            add_dependencies(psp-voice-component-probe tilefinch-voice-component)
        endif()
        if(NOT PSP_BROWSER_CURL_STUB)
            target_sources(psp-browser-script PRIVATE
                src/psp_network.c
                src/psp_multiplayer.c
                src/psp_time.c)
            target_compile_definitions(psp-browser-script PRIVATE
                TILEFINCH_PSP_LIVE_NETWORK=1)
            # The DNS stub draws query IDs from the entropy pool in every
            # transport mode; the owned transport also lists it after
            # libmbedcrypto for the TLS hook.
            target_link_libraries(psp-browser-script PRIVATE
                ${TILEFINCH_PSP_TRANSPORT_LIBRARIES}
                tilefinch_psp_entropy
                "-Wl,--whole-archive"
                pspnet pspnet_inet pspnet_apctl pspnet_resolver pspwlan
                psputility
                "-Wl,--no-whole-archive")
        endif()
        # psp-fixup-imports expects SDK import archives after application and
        # third-party static libraries; those libraries may themselves pull
        # additional kernel/user imports. Keep this final ordering explicit.
        target_link_libraries(psp-browser-script PRIVATE
            m pspdisplay pspge pspctrl pspgu pspdmac psppower
            pspaudiocodec pspaudio psputility)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31")
            set_property(TARGET psp-browser-script PROPERTY
                LINK_LIBRARIES_STRATEGY REORDER_MINIMALLY)
        endif()
        if(TILEFINCH_STRIP_RELEASE_EBOOT)
            # The SDK strips only CMAKE_BUILD_TYPE=Release, while this device
            # preset intentionally uses MinSizeRel. Preserve symbols in a
            # sidecar, then package the smaller ELF to reduce pre-main()
            # Memory Stick I/O.
            add_custom_command(TARGET psp-browser-script POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                    $<TARGET_FILE:psp-browser-script>
                    $<TARGET_FILE:psp-browser-script>.unstripped
                COMMAND "${PSPDEV}/bin/psp-strip"
                    $<TARGET_FILE:psp-browser-script>
                COMMENT
                    "Stripping packaged PSP browser (keeping .unstripped symbols)")
        endif()
        create_pbp_file(
            TARGET psp-browser-script
            TITLE "Tilefinch"
            ICON_PATH
                "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/tilefinch-icon0-144x82.png")
        set_property(TARGET psp-browser-script APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/boot-live.cfg"
            # The trust bundle is staged by a POST_BUILD command below. A
            # certificate-only change must therefore retrigger that command
            # instead of leaving a stale roots.pem beside a current EBOOT.
            "${PSP_BROWSER_PSP_CA_BUNDLE}"
            # pack-pbp is a POST_BUILD step, so the packaged EBOOT keeps its
            # old ICON0 unless the link itself depends on the asset.
            "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/tilefinch-icon0-144x82.png")
        if(TILEFINCH_PSP_VALIDATION_LOG)
            # Native game-audio envelopes keep attack/release progress on the
            # mixer thread while JavaScript stalls. Keep validation probes
            # out of the shipping ratchet while allowing measured device-only
            # instrumentation to grow without code-golfing production paths.
            # Raised by 200,000 bytes (user-approved) for the measurement
            # tiers (work vector, offline replay): validation was within
            # 1.3 KB of the old 4,500,000-byte limit.
            # Raised by 280,000 bytes (user-approved) for the -O2 JavaScript
            # engine (TilefinchDependencies.cmake): 4,548,332 -> 4,824,900.
            # Native AudioParam scheduling measured 5,001,716 bytes with all
            # validation probes retained (previous image: 4,974,340). Its
            # device component tests reduce scheduling CPU by 59-73%; whole
            # gameplay qualification remains opt-in. Admit a user-approved
            # 24 KiB validation-only increase; shipping keeps its own limit.
            # Installed-app heap, deferred script source and recompile
            # work plus the larger game menus measured 5,029,216 bytes in
            # validation. The user approved a validation-only ceiling of
            # 5,500,000 bytes (2026-10-04); shipping keeps its own limit.
            set(TILEFINCH_PSP_TEXT_LIMIT 5500000)
        else()
            # Security-boundary retirement and private lazy Worker compiler
            # installation are native fail-closed paths.  Their measured
            # device cost is admitted explicitly rather than code-golfing
            # ownership checks at the browser/runtime boundary.
            # The bounded WebAssembly JavaScript adapter adds 14,148 bytes of
            # resident bridge code.  The 230+ KiB interpreter remains in the
            # lazy tilefinch-wasm.prx and therefore does not affect boot.
            # Security hardening for active-document principals and bounded
            # WebAssembly/QuickJS execution measured 4,470,576 bytes. Keep
            # 9,424 bytes of the user-approved 10 KiB growth allowance.
            # Raised by 280,000 bytes (user-approved) for the -O2 JavaScript
            # engine: first usable input -3.2 s and first answer -6.5 s on
            # the device (chatgpt-ask); this build 4,647,460 bytes.
            set(TILEFINCH_PSP_TEXT_LIMIT 4760000)
        endif()
        # Local size experiments only (an engine built at -O2 under PPSSPP,
        # for instance): a build directory that sets this is over the
        # ratchet by construction and must never be shipped or baselined.
        set(PSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE "" CACHE STRING
            "Replace the measured PSP .text ratchet in an experiment build")
        if(PSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE)
            message(WARNING "PSP .text ratchet overridden to "
                "${PSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE} bytes: experiment "
                "build, not a shippable image")
            set(TILEFINCH_PSP_TEXT_LIMIT ${PSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE})
        endif()
        # Public struct layouts must not depend on defines private to one
        # library (TILEFINCH_NO_TRACE is private to tilefinch_core; the PSP
        # support libraries never see TILEFINCH_PSP_VALIDATION_LOG). Compile
        # src/abi_layout_probe.c with each side's exact settings (never
        # linked) and fail the browser build if any struct size or field
        # offset differs from the core's.
        set(_abi_probe_objects)
        foreach(_abi_side IN ITEMS tilefinch_core tilefinch_psp_ui
                tilefinch_psp_app_support psp-browser-script)
            string(MAKE_C_IDENTIFIER "${_abi_side}" _abi_name)
            set(_abi_probe tilefinch_abi_probe_${_abi_name})
            add_library(${_abi_probe} OBJECT src/abi_layout_probe.c)
            target_compile_definitions(${_abi_probe} PRIVATE
                $<TARGET_PROPERTY:${_abi_side},COMPILE_DEFINITIONS>)
            target_include_directories(${_abi_probe} PRIVATE
                $<TARGET_PROPERTY:${_abi_side},INCLUDE_DIRECTORIES>)
            target_compile_options(${_abi_probe} PRIVATE
                $<TARGET_PROPERTY:${_abi_side},COMPILE_OPTIONS>)
            # The core's dependencies install headers the probe includes.
            add_dependencies(${_abi_probe} tilefinch_core)
            if(_abi_side STREQUAL "tilefinch_core")
                set(_abi_reference "$<TARGET_OBJECTS:${_abi_probe}>")
            else()
                list(APPEND _abi_probe_objects
                    "$<TARGET_OBJECTS:${_abi_probe}>")
            endif()
            list(APPEND _abi_probe_targets ${_abi_probe})
        endforeach()
        list(JOIN _abi_probe_objects "|" _abi_consumers)
        add_custom_target(tilefinch-abi-layout-check
            COMMAND ${CMAKE_COMMAND}
                -DPSP_NM=${CMAKE_NM}
                "-DABI_REFERENCE=${_abi_reference}"
                "-DABI_CONSUMERS=${_abi_consumers}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckAbiLayout.cmake"
            DEPENDS ${_abi_probe_targets}
            COMMENT "Checking public struct layouts across PSP libraries"
            VERBATIM)
        add_dependencies(psp-browser-script tilefinch-abi-layout-check)
        add_custom_command(TARGET psp-browser-script POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DPSP_OBJDUMP=${TILEFINCH_PSP_OBJDUMP}
                -DPSP_ELF=$<TARGET_FILE:psp-browser-script>
                -DPSP_TEXT_LIMIT=${TILEFINCH_PSP_TEXT_LIMIT}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPspTextSize.cmake"
            COMMENT "Checking measured PSP browser .text ratchet")
        add_custom_command(TARGET psp-browser-script POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DPSP_NM=${CMAKE_NM}
                -DPSP_ELF=$<TARGET_FILE:psp-browser-script>.unstripped
                # `main` is now cold boot orchestration and a thin call into
                # the separately-ratcheted resident loop. Its limit is a
                # growth tripwire, not an I-cache claim. Validation retains
                # extra room for its boot qualifications and log setup.
                -DPSP_MAIN_LIMIT=$<IF:$<BOOL:${TILEFINCH_PSP_VALIDATION_LOG}>,17408,10752>
                -DPSP_VALIDATION_LOG=$<BOOL:${TILEFINCH_PSP_VALIDATION_LOG}>
                -DPSP_NO_SCRIPT_SAMPLER=$<AND:$<BOOL:${PSP_BROWSER_DISABLE_TRACE}>,$<NOT:$<BOOL:${TILEFINCH_PSP_VALIDATION_LOG}>>>
                -DPSP_NO_FETCH_TRACE=$<NOT:$<BOOL:${PSP_BROWSER_ENABLE_FETCH_TRACE}>>
                -DPSP_SHARED_FONT_ZLIB=$<AND:$<BOOL:${PSP_BROWSER_FREETYPE_AVAILABLE}>,$<NOT:$<BOOL:${PSP_BROWSER_SYSTEM_FREETYPE}>>>
                -DPSP_LOOP_FRAME_LIMIT=$<IF:$<BOOL:${TILEFINCH_PSP_VALIDATION_LOG}>,6144,5120>
                -DPSP_FRAME_SET_LIMIT=$<IF:$<BOOL:${TILEFINCH_PSP_VALIDATION_LOG}>,19456,14336>
                -DTILEFINCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPspHotSymbolSizes.cmake"
            COMMENT "Checking PSP hot-function instruction-cache ratchets")
        add_custom_command(TARGET psp-browser-script POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:psp-browser-script>/fonts
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PSP_BROWSER_PSP_SANS_FONT}"
                "${PSP_BROWSER_PSP_SERIF_FONT}"
                "${PSP_BROWSER_SANS_ITALIC_FONT}"
                "${PSP_BROWSER_SANS_BOLD_FONT}"
                "${PSP_BROWSER_SERIF_BOLD_FONT}"
                "${PSP_BROWSER_METRIC_SANS_FONT}"
                "${PSP_BROWSER_METRIC_SANS_BOLD_FONT}"
                $<TARGET_FILE_DIR:psp-browser-script>/fonts
            COMMENT "Staging fonts beside the browser EBOOT")
        if(NOT PSP_BROWSER_CURL_STUB)
            add_custom_command(TARGET psp-browser-script POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PSP_BROWSER_PSP_CA_BUNDLE}"
                    $<TARGET_FILE_DIR:psp-browser-script>/roots.pem
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/boot-live.cfg"
                    $<TARGET_FILE_DIR:psp-browser-script>/boot-defaults.cfg
                COMMENT "Staging the PSP TLS trust bundle")
        endif()
        if(PSP_BROWSER_ENABLE_PSP_VOICE)
            if(PSP_BROWSER_COMPACT_FIXED_VOICE)
                find_program(TILEFINCH_HOST_PYTHON NAMES python3 REQUIRED)
                set(_tilefinch_voice_map_dir
                    "${CMAKE_CURRENT_BINARY_DIR}/voice-model-maps")
                set(_tilefinch_voice_extra_map
                    "${_tilefinch_voice_map_dir}/extra-wide/search.dict.tilefinch")
                set(_tilefinch_voice_small_map
                    "${_tilefinch_voice_map_dir}/search/search.dict.tilefinch")
                set(_tilefinch_voice_map_verifier
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyVoiceModelMap.cmake")
                add_custom_command(
                    OUTPUT
                        "${_tilefinch_voice_extra_map}"
                        "${_tilefinch_voice_small_map}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${_tilefinch_voice_map_dir}/extra-wide"
                        "${_tilefinch_voice_map_dir}/search"
                    COMMAND "${TILEFINCH_HOST_PYTHON}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_voice_model.py"
                        --mdef
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/en-us/mdef"
                        --dict
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/extra-wide/search.dict"
                        --filler
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/en-us/noisedict"
                        --sendump
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/en-us/sendump"
                        --expected-sendump-rows
                        "${TILEFINCH_VOICE_SENDUMP_ROWS}"
                        --expected-sendump-row-bytes
                        "${TILEFINCH_VOICE_SENDUMP_ROW_BYTES}"
                        --output "${_tilefinch_voice_extra_map}"
                    COMMAND "${TILEFINCH_HOST_PYTHON}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_voice_model.py"
                        --mdef
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/en-us/mdef"
                        --dict
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/search/search.dict"
                        --filler
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/en-us/noisedict"
                        --sendump
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/en-us/sendump"
                        --expected-sendump-rows
                        "${TILEFINCH_VOICE_SENDUMP_ROWS}"
                        --expected-sendump-row-bytes
                        "${TILEFINCH_VOICE_SENDUMP_ROW_BYTES}"
                        --output "${_tilefinch_voice_small_map}"
                    COMMAND "${CMAKE_COMMAND}"
                        "-DMAP_FILE=${_tilefinch_voice_extra_map}"
                        "-DEXPECTED_SIZE=332116"
                        "-DEXPECTED_SHA256=ed323cda185173c0304a828b5b6efe68c7be7271a3bb051470392a97f1aa353f"
                        -P "${_tilefinch_voice_map_verifier}"
                    COMMAND "${CMAKE_COMMAND}"
                        "-DMAP_FILE=${_tilefinch_voice_small_map}"
                        "-DEXPECTED_SIZE=213186"
                        "-DEXPECTED_SHA256=067cadf4156554599865e754e67d15f6879526e07644c85c9ce02a14cef26970"
                        -P "${_tilefinch_voice_map_verifier}"
                    DEPENDS
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_voice_model.py"
                        "${_tilefinch_voice_map_verifier}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/en-us/mdef"
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/en-us/noisedict"
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/en-us/sendump"
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/extra-wide/search.dict"
                        "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model/search/search.dict"
                    VERBATIM
                    COMMENT "Compiling exact fixed voice-model lookup maps")
                add_custom_target(tilefinch_voice_model_maps
                    DEPENDS
                        "${_tilefinch_voice_extra_map}"
                        "${_tilefinch_voice_small_map}")
                set(_tilefinch_voice_component_tree
                    "${CMAKE_CURRENT_BINARY_DIR}/voice-component/voice-en-us")
                set(_tilefinch_voice_component_package
                    "${CMAKE_CURRENT_BINARY_DIR}/voice-component/tilefinch-voice-en-us-v1.tfvp")
                add_custom_target(tilefinch-voice-component-package
                    COMMAND "${TILEFINCH_HOST_PYTHON}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage_voice_component.py"
                        --source "${CMAKE_CURRENT_SOURCE_DIR}"
                        --extra-map "${_tilefinch_voice_extra_map}"
                        --small-map "${_tilefinch_voice_small_map}"
                        --output "${_tilefinch_voice_component_tree}"
                    COMMAND "${TILEFINCH_HOST_PYTHON}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/tilefinch_update_tool.py"
                        pack --component
                        --directory "${_tilefinch_voice_component_tree}"
                        --output "${_tilefinch_voice_component_package}"
                    DEPENDS tilefinch_voice_model_maps
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage_voice_component.py"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/tilefinch_update_tool.py"
                    COMMENT
                        "Staging the separately signed optional voice package"
                    VERBATIM)
                add_dependencies(
                    psp-browser-script tilefinch_voice_model_maps)
            endif()
            # The model is ~8 MiB; recopying it on every relink cost more
            # than the link. copy_directory_if_different needs CMake 3.26.
            if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
                set(_tilefinch_voice_copy copy_directory_if_different)
            else()
                set(_tilefinch_voice_copy copy_directory)
            endif()
            add_custom_command(TARGET psp-browser-script POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E ${_tilefinch_voice_copy}
                    "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/voice-model"
                    $<TARGET_FILE_DIR:psp-browser-script>/voice-model
                COMMENT "Staging offline voice model beside the browser EBOOT")
            if(PSP_BROWSER_COMPACT_FIXED_VOICE)
                add_custom_command(TARGET psp-browser-script POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_tilefinch_voice_extra_map}"
                        $<TARGET_FILE_DIR:psp-browser-script>/voice-model/extra-wide/search.dict.tilefinch
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_tilefinch_voice_small_map}"
                        $<TARGET_FILE_DIR:psp-browser-script>/voice-model/search/search.dict.tilefinch
                    COMMENT "Staging compact fixed voice-model maps")
            endif()
        endif()

        # Relocatable PRX of the same browser, for PSPLink's `ld host0:/...`
        # (docs/engineering/INPUT_SCRIPT_HARNESS.md, "PSPLink live loop").
        #
        # PSPLink refuses a static PSP ELF with 0x80020148 (unsupported type),
        # so the USB developer loop needs a relocatable module. This is a
        # second LINK of the objects psp-browser-script already compiled --
        # $<TARGET_OBJECTS:> reuses them verbatim, so the PRX cannot drift
        # from the EBOOT and costs no second compile. Nothing about the EBOOT
        # target changes: no ratchet, no PBP, no install-tree entry, and the
        # PRX remains a developer artifact in every PSP build. In particular,
        # producing it from release objects lets a no-telemetry hardware soak
        # use host0: and perform zero Memory Stick writes. It is never
        # packaged or copied into the install tree.
        add_executable(psp-browser-script-dev-prx
                $<TARGET_OBJECTS:psp-browser-script>)
        if(TILEFINCH_PSP_VALIDATION_LOG AND NOT PSP_BROWSER_CURL_STUB)
            target_link_options(psp-browser-script-dev-prx PRIVATE ${_transport_probe_link_options})
        endif()
        # The EBOOT's newlib printf aliases (see psp-browser-script above),
        # so a host0: hardware run executes the same printf cores it does.
        target_link_options(psp-browser-script-dev-prx PRIVATE
            "LINKER:--defsym=_vfiprintf_r=_vfprintf_r"
            "LINKER:--defsym=_svfiprintf_r=_svfprintf_r")
            # A target whose only sources are prebuilt objects has no language
            # to infer a linker from. The .elf name keeps this intermediate
            # from ever colliding with the EBOOT's own ELF, which every script
            # and doc names by its bare target name.
            set_target_properties(psp-browser-script-dev-prx PROPERTIES
                LINKER_LANGUAGE C
                OUTPUT_NAME psp-browser-script-dev.elf)
            # Ordering, not just object freshness: the browser's ratchets are
            # POST_BUILD steps, so the developer module is not produced by a
            # build whose shipping ELF failed one.
            # Runtime assets are inputs to a host0: PRX run, not incidental
            # leftovers from whichever EBOOT happened to link most recently.
            # A POST_BUILD copy on psp-browser-script alone does not repair a
            # manually-cleaned roots.pem when the ELF itself is up to date,
            # which made otherwise identical PSPLink starts fail at random.
            add_custom_target(psp-browser-script-dev-assets
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_CURRENT_BINARY_DIR}/fonts"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PSP_BROWSER_PSP_SANS_FONT}"
                    "${PSP_BROWSER_PSP_SERIF_FONT}"
                    "${PSP_BROWSER_SANS_ITALIC_FONT}"
                    "${PSP_BROWSER_SANS_BOLD_FONT}"
                    "${PSP_BROWSER_SERIF_BOLD_FONT}"
                    "${PSP_BROWSER_METRIC_SANS_FONT}"
                    "${PSP_BROWSER_METRIC_SANS_BOLD_FONT}"
                    "${CMAKE_CURRENT_BINARY_DIR}/fonts"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/boot-live.cfg"
                    "${CMAKE_CURRENT_BINARY_DIR}/boot-defaults.cfg"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PSP_BROWSER_PSP_CA_BUNDLE}"
                    "${CMAKE_CURRENT_BINARY_DIR}/roots.pem"
                DEPENDS
                    "${PSP_BROWSER_PSP_SANS_FONT}"
                    "${PSP_BROWSER_PSP_SERIF_FONT}"
                    "${PSP_BROWSER_SANS_ITALIC_FONT}"
                    "${PSP_BROWSER_SANS_BOLD_FONT}"
                    "${PSP_BROWSER_SERIF_BOLD_FONT}"
                    "${PSP_BROWSER_METRIC_SANS_FONT}"
                    "${PSP_BROWSER_METRIC_SANS_BOLD_FONT}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/psp-assets/boot-live.cfg"
                    "${PSP_BROWSER_PSP_CA_BUNDLE}"
                COMMENT "Staging required host0 browser assets")
            add_dependencies(psp-browser-script-dev-prx
                psp-browser-script psp-browser-script-dev-assets)
            # The same libraries in the same order as psp-browser-script
            # above, under the same conditions. Deliberately restated rather
            # than shared through a variable: the EBOOT's link line is the one
            # that ships, and this developer module must never be able to
            # perturb it.
            target_link_libraries(psp-browser-script-dev-prx PRIVATE
                tilefinch_core tilefinch_psp_ui tilefinch_psp_display
                tilefinch_psp_media_scale
                tilefinch_psp_media_present
                tilefinch_psp_app_support tilefinch_diagnostic_qr)
            if(PSP_BROWSER_ENABLE_PSP_VOICE)
                target_link_libraries(psp-browser-script-dev-prx PRIVATE
                    tilefinch_voice_frontend pspaudio)
            endif()
            if(NOT PSP_BROWSER_CURL_STUB)
                target_link_libraries(psp-browser-script-dev-prx PRIVATE
                    ${TILEFINCH_PSP_TRANSPORT_LIBRARIES}
                    tilefinch_psp_entropy
                    "-Wl,--whole-archive"
                    pspnet pspnet_inet pspnet_apctl pspnet_resolver pspwlan
                    psputility
                    "-Wl,--no-whole-archive")
            endif()
            target_link_libraries(psp-browser-script-dev-prx PRIVATE
                m pspdisplay pspge pspctrl pspgu pspdmac psppower
                pspaudiocodec pspaudio psputility)
            if(TILEFINCH_PSP_VALIDATION_LOG)
                # Validation objects include the native WebGL GE probe. The
                # relocatable PSPLink link reuses those exact objects, so it
                # must carry the same GUM dependency as the validation EBOOT.
                target_link_libraries(psp-browser-script-dev-prx PRIVATE
                    pspgum)
            endif()
            if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31")
                set_property(TARGET psp-browser-script-dev-prx PROPERTY
                    LINK_LIBRARIES_STRATEGY REORDER_MINIMALLY)
            endif()
            # The PSPSDK relocatable-module recipe (lib/build.mak, BUILD_PRX):
            # prxspecs swaps crt0 for crt0_prx, -q keeps the relocations
            # psp-prxgen turns into the PRX relocation table, linkfile.prx is
            # the module link script, and prxexports.o supplies the default
            # module_start/module_stop export table. cmake/PspGcKeep.ld is an
            # INSERT script, so it still augments linkfile.prx and the ABI
            # metadata sections survive --gc-sections here too.
            target_link_options(psp-browser-script-dev-prx PRIVATE
                "-specs=${PSPDEV}/psp/sdk/lib/prxspecs"
                "LINKER:-q"
                "LINKER:-T,${PSPDEV}/psp/sdk/lib/linkfile.prx"
                "${PSPDEV}/psp/sdk/lib/prxexports.o")
            set_property(TARGET psp-browser-script-dev-prx APPEND PROPERTY
                LINK_DEPENDS
                "${PSPDEV}/psp/sdk/lib/linkfile.prx"
                "${PSPDEV}/psp/sdk/lib/prxexports.o")
            if(TILEFINCH_PSP_LAYOUT_LINK_OPTIONS)
                target_link_options(psp-browser-script-dev-prx PRIVATE
                    ${TILEFINCH_PSP_LAYOUT_LINK_OPTIONS})
                set_property(TARGET psp-browser-script-dev-prx APPEND PROPERTY
                    LINK_DEPENDS "${PSP_BROWSER_PSP_SECTION_ORDERING_FILE}")
            endif()
            if(PSP_BROWSER_PSP_LINK_MAP)
                target_link_options(psp-browser-script-dev-prx PRIVATE
                    "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/psp-browser-script-dev.map")
            endif()
            add_custom_command(TARGET psp-browser-script-dev-prx POST_BUILD
                COMMAND "${PSPDEV}/bin/psp-fixup-imports"
                    $<TARGET_FILE:psp-browser-script-dev-prx>
                COMMAND "${PSPDEV}/bin/psp-prxgen"
                    $<TARGET_FILE:psp-browser-script-dev-prx>
                    "${CMAKE_CURRENT_BINARY_DIR}/psp-browser-script-dev.prx"
                COMMENT "Generating psp-browser-script-dev.prx for PSPLink")

        # Exact first-install tree. The stable launcher is the only root
        # EBOOT; browser assets belong to slot-a, while mutable state is kept
        # in data. Recreate the staging tree so a removed asset cannot linger
        # into a release package.
        set(TILEFINCH_PSP_INSTALL_TREE
            "${CMAKE_CURRENT_BINARY_DIR}/tilefinch-install/Tilefinch")
        add_custom_target(tilefinch-psp-install-tree
            COMMAND ${CMAKE_COMMAND}
                "-DLAUNCHER_EBOOT=${CMAKE_CURRENT_BINARY_DIR}/launcher/EBOOT.PBP"
                "-DBROWSER_EBOOT=${CMAKE_CURRENT_BINARY_DIR}/EBOOT.PBP"
                "-DWASM_COMPONENT_PRX=${TILEFINCH_WASM_COMPONENT_PRX}"
                "-DVOICE_COMPONENT_PRX=${TILEFINCH_VOICE_COMPONENT_PRX}"
                "-DXMB_REDIRECT_PRX=${CMAKE_CURRENT_BINARY_DIR}/xmb-redirect/tilefinch_xmb.prx"
                "-DASSET_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DOUTPUT=${TILEFINCH_PSP_INSTALL_TREE}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StagePspInstall.cmake"
            DEPENDS tilefinch-launcher psp-browser-script
                tilefinch-wasm-component tilefinch-xmb-redirect
            COMMENT
                "Staging launcher + slot-a + optional XMB redirect"
            VERBATIM)
    endif()

    # Editing the augmenting linker script must relink every PSP executable;
    # merely carrying its path in the command line is not a build dependency.
    foreach(psp_target IN ITEMS
            psp-browser-lab
            psp-browser-interactive-lab
            psp-browser-failure-recovery
            psp-browser-fixture
            psp-crypto-selftest
            psp-browser-script
            psp-browser-script-dev-prx
            tilefinch-xmb-redirect)
        if(TARGET ${psp_target})
            set_property(TARGET ${psp_target} APPEND PROPERTY LINK_DEPENDS
                "${PSP_BROWSER_GC_KEEP_SCRIPT}")
        endif()
    endforeach()
endif()
target_compile_definitions(psp-browser-interactive-lab PRIVATE
    TILEFINCH_SANS_FONT="${PSP_BROWSER_SANS_FONT}"
    TILEFINCH_SERIF_FONT="${PSP_BROWSER_SERIF_FONT}"
    TILEFINCH_SANS_ITALIC_FONT="${PSP_BROWSER_SANS_ITALIC_FONT}"
    TILEFINCH_SANS_BOLD_FONT="${PSP_BROWSER_SANS_BOLD_FONT}"
    TILEFINCH_SERIF_BOLD_FONT="${PSP_BROWSER_SERIF_BOLD_FONT}"
    TILEFINCH_METRIC_SANS_FONT="${PSP_BROWSER_METRIC_SANS_FONT}"
    TILEFINCH_METRIC_SANS_BOLD_FONT="${PSP_BROWSER_METRIC_SANS_BOLD_FONT}")

# This recorder intentionally exists only with the host libcurl transport.
# PSP builds use their platform transport and never ship acquisition tooling.
if(PSP_BROWSER_LIBCURL_TRANSPORT AND NOT PSP)
    add_executable(psp-browser-trace-acquire src/trace_acquire_main.c)
    target_link_libraries(psp-browser-trace-acquire PRIVATE tilefinch_core)
    add_executable(psp-browser-trace-inventory src/trace_inventory_main.c)
    target_link_libraries(psp-browser-trace-inventory PRIVATE tilefinch_core)
endif()

# Explicit hardware helper-ABI control. This includes the exact selected VM
# source once, before qjs's supporting archive objects. No browser target links
# the fixture, and no CTest entry starts a cross-build or device run.
if(PSP AND PSP_BROWSER_USE_BELLARD_QUICKJS)
    add_executable(psp-native-tier-helper-probe EXCLUDE_FROM_ALL
        benchmarks/native-tier-helper-probe.c src/psp_atomic_shims.c)
    target_include_directories(psp-native-tier-helper-probe PRIVATE include)
    target_compile_definitions(psp-native-tier-helper-probe PRIVATE
        PSP_BROWSER_BELLARD_QUICKJS=1
        TILEFINCH_HELPER_QUICKJS_SOURCE="${quickjs_SOURCE_DIR}/quickjs.c"
        "$<TARGET_PROPERTY:qjs,COMPILE_DEFINITIONS>")
    target_compile_options(psp-native-tier-helper-probe PRIVATE
        "$<TARGET_PROPERTY:qjs,COMPILE_OPTIONS>")
    target_link_libraries(psp-native-tier-helper-probe PRIVATE
        tilefinch_core m psppower)
    set_target_properties(psp-native-tier-helper-probe PROPERTIES
        C_EXTENSIONS ON OUTPUT_NAME native-helper-probe.elf)
    target_link_options(psp-native-tier-helper-probe PRIVATE
        "-specs=${PSPDEV}/psp/sdk/lib/prxspecs"
        "LINKER:-q"
        "LINKER:-T,${PSPDEV}/psp/sdk/lib/linkfile.prx"
        "${PSPDEV}/psp/sdk/lib/prxexports.o")
    set_property(TARGET psp-native-tier-helper-probe APPEND PROPERTY LINK_DEPENDS
        "${PSPDEV}/psp/sdk/lib/linkfile.prx"
        "${PSPDEV}/psp/sdk/lib/prxexports.o"
        "${quickjs_SOURCE_DIR}/quickjs.c")
    add_custom_command(TARGET psp-native-tier-helper-probe POST_BUILD
        COMMAND "${PSPDEV}/bin/psp-fixup-imports"
            $<TARGET_FILE:psp-native-tier-helper-probe>
        COMMAND "${PSPDEV}/bin/psp-prxgen"
            $<TARGET_FILE:psp-native-tier-helper-probe>
            "${CMAKE_CURRENT_BINARY_DIR}/native-helper-probe.prx"
        COMMENT "Generating the isolated host0 helper ABI probe")

    # Actual machine-code lowering remains an explicit host0-only experiment.
    # The generated interpreter copy must never enter an ordinary browser link.
    set(native_region_engine "${CMAKE_CURRENT_BINARY_DIR}/native-tier-region-quickjs.c")
    add_custom_command(OUTPUT "${native_region_engine}"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${quickjs_SOURCE_DIR}/quickjs.c"
            "-DOUTPUT=${native_region_engine}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/native-tier-region-engine.cmake"
        DEPENDS "${quickjs_SOURCE_DIR}/quickjs.c"
            benchmarks/native-tier-region-engine.cmake VERBATIM)
    set_source_files_properties("${native_region_engine}" PROPERTIES HEADER_FILE_ONLY TRUE)
    add_executable(psp-native-tier-region-probe EXCLUDE_FROM_ALL
        benchmarks/native-tier-region-probe.c benchmarks/native-tier-code-pool.c
        src/psp_atomic_shims.c "${native_region_engine}")
    target_include_directories(psp-native-tier-region-probe PRIVATE include
        "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
        "${quickjs_SOURCE_DIR}")
    target_compile_definitions(psp-native-tier-region-probe PRIVATE
        PSP_BROWSER_BELLARD_QUICKJS=1 TF_REGION_FRAME_LEASE=1
        TF_REGION_TABLE_DISPATCH=1 TF_REGION_SEMANTIC_CONTINUATIONS=1
        TF_REGION_COMPACT_PLAN=1 "$<TARGET_PROPERTY:qjs,COMPILE_DEFINITIONS>")
    target_compile_options(psp-native-tier-region-probe PRIVATE
        "$<TARGET_PROPERTY:qjs,COMPILE_OPTIONS>")
    target_link_libraries(psp-native-tier-region-probe PRIVATE tilefinch_core m psppower)
    set_target_properties(psp-native-tier-region-probe PROPERTIES
        C_EXTENSIONS ON OUTPUT_NAME native-region-probe.elf)
    target_link_options(psp-native-tier-region-probe PRIVATE
        "-specs=${PSPDEV}/psp/sdk/lib/prxspecs" "LINKER:-q"
        "LINKER:-T,${PSPDEV}/psp/sdk/lib/linkfile.prx"
        "${PSPDEV}/psp/sdk/lib/prxexports.o")
    set_property(TARGET psp-native-tier-region-probe APPEND PROPERTY LINK_DEPENDS
        "${PSPDEV}/psp/sdk/lib/linkfile.prx" "${PSPDEV}/psp/sdk/lib/prxexports.o")
    add_custom_command(TARGET psp-native-tier-region-probe POST_BUILD
        COMMAND "${PSPDEV}/bin/psp-fixup-imports" $<TARGET_FILE:psp-native-tier-region-probe>
        COMMAND "${PSPDEV}/bin/psp-prxgen" $<TARGET_FILE:psp-native-tier-region-probe>
            "${CMAKE_CURRENT_BINARY_DIR}/native-region-probe.prx"
        COMMENT "Generating the isolated host0 Allegrex region probe")
endif()

# A hand-lowered, real-JSValue ABI experiment, not a browser execution tier.
# It includes QuickJS internals in its own translation unit. The qjs archive
# supplies only supporting objects; neither tilefinch_core nor a PSP EBOOT
# contains this fixture. Explicitly build this target when investigating it.
if(NOT PSP AND UNIX AND PSP_BROWSER_USE_BELLARD_QUICKJS
   AND quickjs_SOURCE_DIR STREQUAL tilefinch_quickjs_vendor_dir)
    function(tilefinch_add_native_tier_probe name)
        add_executable(${name} EXCLUDE_FROM_ALL ${ARGN} src/budget.c)
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_compile_definitions(${name} PRIVATE PSP_BROWSER_BELLARD_QUICKJS=1
            "$<TARGET_PROPERTY:qjs,COMPILE_DEFINITIONS>")
        target_compile_options(${name} PRIVATE -funsigned-char -fwrapv)
        set_target_properties(${name} PROPERTIES C_EXTENSIONS ON)
        target_link_libraries(${name} PRIVATE qjs lexbor_static m)
        if(TILEFINCH_OWNER_CHECKS)
            target_compile_definitions(${name} PRIVATE TILEFINCH_OWNER_CHECKS=1)
            target_link_libraries(${name} PRIVATE Threads::Threads)
        endif()
    endfunction()
    tilefinch_add_native_tier_probe(tilefinch-native-tier-helper-probe
        benchmarks/native-tier-helper-probe.c)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|x86_64|AMD64)$")
        tilefinch_add_native_tier_probe(tilefinch-native-tier-code-pool-probe
            benchmarks/native-tier-code-pool-probe.c benchmarks/native-tier-code-pool.c)
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        option(TILEFINCH_NATIVE_TIER_JOB_CENSUS
            "Compile per-opcode census hooks into the isolated native-tier lab"
            OFF)
        set(native_region_engine "${CMAKE_CURRENT_BINARY_DIR}/native-tier-region-quickjs.c")
        add_custom_command(OUTPUT "${native_region_engine}"
            COMMAND "${CMAKE_COMMAND}"
                "-DINPUT=${quickjs_SOURCE_DIR}/quickjs.c"
                "-DOUTPUT=${native_region_engine}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/native-tier-region-engine.cmake"
            DEPENDS "${quickjs_SOURCE_DIR}/quickjs.c"
                benchmarks/native-tier-region-engine.cmake VERBATIM)
        tilefinch_add_native_tier_probe(tilefinch-native-tier-region-probe
            benchmarks/native-tier-region-probe.c benchmarks/native-tier-code-pool.c
            "${native_region_engine}")
        set_source_files_properties("${native_region_engine}" PROPERTIES HEADER_FILE_ONLY TRUE)
        target_include_directories(tilefinch-native-tier-region-probe PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
            "${quickjs_SOURCE_DIR}")
        tilefinch_add_native_tier_probe(tilefinch-native-tier-region-reference
            benchmarks/native-tier-region-probe.c benchmarks/native-tier-code-pool.c
            "${native_region_engine}")
        target_compile_definitions(tilefinch-native-tier-region-reference PRIVATE
            TF_REGION_INTERPRETER_ONLY=1)
        target_include_directories(tilefinch-native-tier-region-reference PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
            "${quickjs_SOURCE_DIR}")
        tilefinch_add_native_tier_probe(tilefinch-native-tier-region-frame-lease
            benchmarks/native-tier-region-probe.c benchmarks/native-tier-code-pool.c
            "${native_region_engine}")
        target_compile_definitions(tilefinch-native-tier-region-frame-lease PRIVATE
            TF_REGION_FRAME_LEASE=1)
        target_include_directories(tilefinch-native-tier-region-frame-lease PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
            "${quickjs_SOURCE_DIR}")
        tilefinch_add_native_tier_probe(tilefinch-native-tier-region-table-dispatch
            benchmarks/native-tier-region-probe.c benchmarks/native-tier-code-pool.c
            "${native_region_engine}")
        target_compile_definitions(tilefinch-native-tier-region-table-dispatch PRIVATE
            TF_REGION_FRAME_LEASE=1 TF_REGION_TABLE_DISPATCH=1)
        target_include_directories(tilefinch-native-tier-region-table-dispatch PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
            "${quickjs_SOURCE_DIR}")
        tilefinch_add_native_tier_probe(tilefinch-native-tier-region-continuations
            benchmarks/native-tier-region-probe.c benchmarks/native-tier-code-pool.c
            "${native_region_engine}")
        target_compile_definitions(tilefinch-native-tier-region-continuations PRIVATE
            TF_REGION_FRAME_LEASE=1 TF_REGION_TABLE_DISPATCH=1
            TF_REGION_SEMANTIC_CONTINUATIONS=1)
        target_include_directories(tilefinch-native-tier-region-continuations PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
            "${quickjs_SOURCE_DIR}")
        # Keep the fixed-layout target above as the timing/ownership control.
        tilefinch_add_native_tier_probe(tilefinch-native-tier-region-compact-plan
            benchmarks/native-tier-region-probe.c benchmarks/native-tier-code-pool.c
            "${native_region_engine}")
        target_compile_definitions(tilefinch-native-tier-region-compact-plan PRIVATE
            TF_REGION_FRAME_LEASE=1 TF_REGION_TABLE_DISPATCH=1
            TF_REGION_SEMANTIC_CONTINUATIONS=1 TF_REGION_COMPACT_PLAN=1)
        target_include_directories(tilefinch-native-tier-region-compact-plan PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
            "${quickjs_SOURCE_DIR}")
        tilefinch_add_native_tier_probe(tilefinch-native-tier-region-sized-plan
            benchmarks/native-tier-region-probe.c benchmarks/native-tier-code-pool.c
            "${native_region_engine}")
        target_compile_definitions(tilefinch-native-tier-region-sized-plan PRIVATE
            TF_REGION_FRAME_LEASE=1 TF_REGION_TABLE_DISPATCH=1
            TF_REGION_SEMANTIC_CONTINUATIONS=1 TF_REGION_COMPACT_PLAN=1
            TF_REGION_COMPACT_PLAN_U32=1)
        target_include_directories(tilefinch-native-tier-region-sized-plan PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
            "${quickjs_SOURCE_DIR}")
        # Whole-page A/B only. This executable links the isolated interpreter
        # copy before qjs; the normal lab and every shipping target stay intact.
        foreach(lane IN ITEMS native reference)
            set(lab_target "psp-browser-native-tier-${lane}-lab")
            add_executable(${lab_target} EXCLUDE_FROM_ALL
                src/interactive_main.c benchmarks/native-tier-region-probe.c
                benchmarks/native-tier-code-pool.c "${native_region_engine}")
            target_link_libraries(${lab_target} PRIVATE tilefinch_psp_ui)
            target_include_directories(${lab_target} PRIVATE
                "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
                "${quickjs_SOURCE_DIR}")
            target_compile_definitions(${lab_target} PRIVATE
                PSP_BROWSER_BELLARD_QUICKJS=1
                TF_REGION_FRAME_LEASE=1 TF_REGION_TABLE_DISPATCH=1
                TF_REGION_SEMANTIC_CONTINUATIONS=1 TF_REGION_COMPACT_PLAN=1
                TF_REGION_LIVE_ENGINE=1 CONFIG_TILEFINCH_CALL_COUNTS=1
                "$<TARGET_PROPERTY:qjs,COMPILE_DEFINITIONS>")
            if(TILEFINCH_NATIVE_TIER_JOB_CENSUS)
                target_compile_definitions(${lab_target} PRIVATE
                    TF_REGION_JOB_CENSUS=1)
            endif()
            if(lane STREQUAL reference)
                target_compile_definitions(${lab_target} PRIVATE TF_REGION_LIVE_DISABLED=1)
            endif()
            get_target_property(lab_fonts psp-browser-interactive-lab COMPILE_DEFINITIONS)
            target_compile_definitions(${lab_target} PRIVATE ${lab_fonts})
            # Keep optimized machine code while allowing independent PC samples
            # to resolve interpreter cases to source lines in the lab only.
            target_compile_options(${lab_target} PRIVATE -funsigned-char -fwrapv
                -g1)
            set_target_properties(${lab_target} PROPERTIES C_EXTENSIONS ON)
        endforeach()
    endif()
endif()

if(NOT PSP_BROWSER_USE_BELLARD_QUICKJS)
    add_executable(psp-browser-quickjs-allocator-bench
        src/quickjs_allocator_bench.c)
    target_link_libraries(psp-browser-quickjs-allocator-bench PRIVATE tilefinch_core)
    if(PSP_BROWSER_PGO_GENERATE)
        target_compile_options(psp-browser-quickjs-allocator-bench PRIVATE
            "-fprofile-instr-generate=${PSP_BROWSER_PGO_GENERATE}")
        target_link_options(psp-browser-quickjs-allocator-bench PRIVATE
            "-fprofile-instr-generate=${PSP_BROWSER_PGO_GENERATE}")
    elseif(PSP_BROWSER_PGO_USE)
        target_compile_options(psp-browser-quickjs-allocator-bench PRIVATE
            "-fprofile-instr-use=${PSP_BROWSER_PGO_USE}")
    endif()
endif()

if(APPLE AND PSP_BROWSER_BUILD_JSC_SPIKE)
    enable_language(OBJC)
    add_executable(psp-browser-jsc-spike src/jsc_spike.m)
    target_link_libraries(psp-browser-jsc-spike PRIVATE
        "-framework Foundation" "-framework JavaScriptCore")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(psp-browser-jsc-spike PRIVATE -Wall -Wextra)
    endif()
endif()
