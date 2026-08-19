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

if(PSP)
    # These are host diagnostic frontends. Keep them individually available
    # for unusual toolchain experiments, but do not let a bare PSP aggregate
    # build try to link host time and 64-bit atomic facilities.
    set_target_properties(
        psp-browser-lab
        psp-browser-interactive-lab
        tilefinch-input-latency-bench
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
    src/psp_ui.c
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
        TILEFINCH_PSP_POWER_TEST_MENU=1)
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
        src/psp_time.c)
    target_include_directories(tilefinch-launcher PRIVATE
        include "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_options(tilefinch-launcher PRIVATE -flto)
    target_link_options(tilefinch-launcher PRIVATE -flto)
    target_link_libraries(tilefinch-launcher PRIVATE
        tilefinch_psp_display tilefinch_psp_systemctrl_imports
        ${TILEFINCH_PSP_CRYPTO_LIBRARIES}
        pspdisplay pspge pspctrl psprtc)
    set_target_properties(tilefinch-launcher PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher")
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
            src/psp_app/psp_app_input.c
            src/psp_app/psp_app_settings.c
            src/psp_app/psp_app_network.c
            src/psp_app/psp_app_page.c
            src/psp_app/psp_app_runtime.c
            src/psp_app/psp_app_surfaces.c
            src/psp_app/psp_app_exit_handoff.c
            src/psp_app/psp_app_glyph_component.c
            src/psp_app/psp_app_voice_component.c
            src/psp_clock_worker.c
            src/psp_log.c
            src/psp_media_buffering.c
            src/psp_media_open.c
            src/psp_media_present_session.c
            src/psp_media_seek.c
            src/psp_media_session.c
            src/psp_media_telemetry.c
            src/psp_offline_store.c
            src/screenshot_png.c
            src/psp_text_input.c
            src/psp_atomic_shims.c)
        if(TILEFINCH_PSP_VALIDATION_LOG AND NOT PSP_BROWSER_CURL_STUB)
            target_sources(psp-browser-script PRIVATE src/psp_update_e2e.c)
        endif()
        target_link_libraries(psp-browser-script PRIVATE tilefinch_core
            tilefinch_psp_ui tilefinch_psp_display
            tilefinch_psp_media_scale
            tilefinch_psp_media_present
            tilefinch_psp_app_support tilefinch_diagnostic_qr)
        # src/psp_app/ holds this EBOOT's private seams. They include
        # src/media_backend_psp_policy.h and src/psp_media_pixels.h, which
        # tilefinch_core keeps PRIVATE, so the executable needs src itself.
        target_include_directories(psp-browser-script PRIVATE src)
        target_compile_definitions(psp-browser-script PRIVATE
            TILEFINCH_UPDATE_REPOSITORY_OWNER="${TILEFINCH_UPDATE_REPOSITORY_OWNER}"
            TILEFINCH_UPDATE_REPOSITORY_NAME="${TILEFINCH_UPDATE_REPOSITORY_NAME}")
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
                src/psp_media_fixture.c
                src/psp_media_range_probe.c
                src/psp_raster_fixture.c
                "${_tilefinch_media_fixture_blob}")
            set_property(TARGET psp-browser-script APPEND PROPERTY
                LINK_DEPENDS
                "${TILEFINCH_MEDIA_FIXTURE_240}"
                "${TILEFINCH_MEDIA_FIXTURE_360}")
        endif()
        if(PSP_BROWSER_ENABLE_PSP_VOICE)
            target_sources(psp-browser-script PRIVATE
                src/psp_voice_input.c
                src/stt/stt_engine.c)
            target_include_directories(psp-browser-script PRIVATE src/stt)
            target_compile_definitions(
                psp-browser-script PRIVATE TILEFINCH_HAVE_PSP_VOICE=1)
            target_link_libraries(
                psp-browser-script PRIVATE
                tilefinch_voice_frontend tilefinch_pocketsphinx pspaudio)
        endif()
        if(NOT PSP_BROWSER_CURL_STUB)
            target_sources(psp-browser-script PRIVATE
                src/psp_network.c
                src/psp_time.c)
            target_compile_definitions(psp-browser-script PRIVATE
                TILEFINCH_PSP_LIVE_NETWORK=1)
            target_link_libraries(psp-browser-script PRIVATE
                ${TILEFINCH_PSP_TRANSPORT_LIBRARIES}
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
            set(TILEFINCH_PSP_TEXT_LIMIT 4460000)
        else()
            set(TILEFINCH_PSP_TEXT_LIMIT 4435000)
        endif()
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
                -DPSP_INTERACTIVE_LIMIT=$<IF:$<BOOL:${TILEFINCH_PSP_VALIDATION_LOG}>,20480,15360>
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
            add_custom_command(TARGET psp-browser-script POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
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
            add_dependencies(psp-browser-script-dev-prx psp-browser-script)
            # The same libraries in the same order as psp-browser-script
            # above, under the same conditions. Deliberately restated rather
            # than shared through a variable: the EBOOT's link line is the one
            # that ships, and this developer module must never be able to
            # perturb it.
            target_link_libraries(psp-browser-script-dev-prx PRIVATE
                tilefinch_core tilefinch_psp_ui tilefinch_psp_display
                tilefinch_psp_media_scale
                tilefinch_psp_media_present
                tilefinch_psp_app_support)
            if(PSP_BROWSER_ENABLE_PSP_VOICE)
                target_link_libraries(psp-browser-script-dev-prx PRIVATE
                    tilefinch_voice_frontend tilefinch_pocketsphinx pspaudio)
            endif()
            if(NOT PSP_BROWSER_CURL_STUB)
                target_link_libraries(psp-browser-script-dev-prx PRIVATE
                    ${TILEFINCH_PSP_TRANSPORT_LIBRARIES}
                    "-Wl,--whole-archive"
                    pspnet pspnet_inet pspnet_apctl pspnet_resolver pspwlan
                    psputility
                    "-Wl,--no-whole-archive")
            endif()
            target_link_libraries(psp-browser-script-dev-prx PRIVATE
                m pspdisplay pspge pspctrl pspgu pspdmac psppower
                pspaudiocodec pspaudio psputility)
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
                "-DASSET_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DOUTPUT=${TILEFINCH_PSP_INSTALL_TREE}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StagePspInstall.cmake"
            DEPENDS tilefinch-launcher psp-browser-script
            COMMENT
                "Staging launcher + slot-a + shared data PSP install tree"
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
            psp-browser-script-dev-prx)
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
