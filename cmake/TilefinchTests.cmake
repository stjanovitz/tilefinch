# The device-cost gate boots PPSSPP, so it stays out of the default lane
# until someone asks for it. It is registered either way (see
# tilefinch-device-cost-tests below) so `ctest -N` always names it.
option(TILEFINCH_DEVICE_COST_GATE
    "Enable the PPSSPP device-cost baseline test in CTest" OFF)
set(TILEFINCH_DEVICE_COST_BUILD_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/build-preset-psp-validation" CACHE PATH
    "PSP build directory (TILEFINCH_PSP_VALIDATION_LOG=ON) the device-cost gate boots")

if(PSP_BROWSER_BUILD_TESTS)
    enable_testing()
    # Register the dependency at the same site that creates each test binary.
    # Conditional suites therefore cannot drift out of the dev.sh build gate.
    function(tilefinch_add_test_binary target)
        add_executable(${target} ${ARGN})
        set_property(GLOBAL APPEND PROPERTY TILEFINCH_TEST_BINARIES ${target})
    endfunction()
    find_package(Threads REQUIRED)
    if(NOT PSP)
        tilefinch_add_test_binary(tilefinch-youtube-feed-probe-tests tests/test_youtube_feed_probe.c)
        target_link_libraries(tilefinch-youtube-feed-probe-tests PRIVATE tilefinch_core)
        add_test(NAME tilefinch-youtube-feed-probe-tests COMMAND tilefinch-youtube-feed-probe-tests)
        set_tests_properties(tilefinch-youtube-feed-probe-tests PROPERTIES
            LABELS "tilefinch;unit;provider" TIMEOUT 20)
    endif()
    tilefinch_add_test_binary(tilefinch-linked-video-preview-tests tests/test_linked_video_preview.c)
    target_link_libraries(tilefinch-linked-video-preview-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-linked-video-preview-tests COMMAND tilefinch-linked-video-preview-tests)
    set_tests_properties(tilefinch-linked-video-preview-tests PROPERTIES
        LABELS "tilefinch;unit;navigation;resources" TIMEOUT 30)
    tilefinch_add_test_binary(tilefinch-compact-controls-tests tests/test_compact_controls.c)
    target_link_libraries(tilefinch-compact-controls-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-compact-controls-tests COMMAND tilefinch-compact-controls-tests)
    set_tests_properties(tilefinch-compact-controls-tests PROPERTIES
        LABELS "tilefinch;unit;layout;controls" TIMEOUT 20)
    tilefinch_add_test_binary(tilefinch-media-queries-tests tests/test_media_queries.c)
    target_link_libraries(tilefinch-media-queries-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-media-queries-tests COMMAND tilefinch-media-queries-tests)
    set_tests_properties(tilefinch-media-queries-tests PROPERTIES
        LABELS "tilefinch;unit;script;style" TIMEOUT 20)
    tilefinch_add_test_binary(tilefinch-dom-intrinsics-tests tests/test_dom_intrinsics.c)
    target_link_libraries(tilefinch-dom-intrinsics-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-dom-intrinsics-tests COMMAND tilefinch-dom-intrinsics-tests)
    set_tests_properties(tilefinch-dom-intrinsics-tests PROPERTIES
        LABELS "tilefinch;unit;script;dom" TIMEOUT 20)
    tilefinch_add_test_binary(tilefinch-ui-language-tests tests/test_ui_language.c)
    target_link_libraries(tilefinch-ui-language-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-ui-language-tests PRIVATE
        TILEFINCH_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        TILEFINCH_UI_RESOURCE_ROOT="${TILEFINCH_UI_RESOURCE_ROOT}")
    if(NOT PSP)
        add_dependencies(tilefinch-ui-language-tests tilefinch-ui-resources)
    endif()
    add_test(NAME tilefinch-ui-language-tests COMMAND tilefinch-ui-language-tests)
    set_tests_properties(tilefinch-ui-language-tests PROPERTIES
        LABELS "tilefinch;unit;ui;storage" TIMEOUT 20)
    if(NOT PSP)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        tilefinch_add_test_binary(tilefinch-ui-language-layout-tests
            tests/test_ui_language_layout.c)
        target_link_libraries(tilefinch-ui-language-layout-tests PRIVATE tilefinch_psp_ui)
        target_compile_definitions(tilefinch-ui-language-layout-tests PRIVATE
            TILEFINCH_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
            TILEFINCH_UI_RESOURCE_ROOT="${TILEFINCH_UI_RESOURCE_ROOT}")
        add_dependencies(tilefinch-ui-language-layout-tests tilefinch-ui-resources)
        add_test(NAME tilefinch-ui-language-layout-tests COMMAND tilefinch-ui-language-layout-tests)
        set_tests_properties(tilefinch-ui-language-layout-tests PROPERTIES
            LABELS "tilefinch;unit;ui;visual" TIMEOUT 60)
        add_test(NAME tilefinch-ui-translations-generated-check
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_ui_translations.py
                --check --resource-dir "${TILEFINCH_UI_RESOURCE_ROOT}")
        set_tests_properties(tilefinch-ui-translations-generated-check PROPERTIES
            LABELS "tilefinch;unit;ui;tooling" TIMEOUT 10)
        add_test(NAME tilefinch-ui-translation-resources-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ui_translation_resources.py)
        set_tests_properties(tilefinch-ui-translation-resources-tests PROPERTIES
            LABELS "tilefinch;unit;ui;tooling" TIMEOUT 10)
        if(TARGET tilefinch-offline-library-fixture)
            add_test(NAME tilefinch-offline-app-fixture-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_offline_app_fixture.py
                    $<TARGET_FILE:tilefinch-offline-library-fixture>)
            set_tests_properties(tilefinch-offline-app-fixture-tests PROPERTIES
                LABELS "tilefinch;unit;offline;tooling" TIMEOUT 30)
        endif()
        add_test(NAME tilefinch-private-test-infrastructure-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_private_test_infrastructure.py)
        set_tests_properties(tilefinch-private-test-infrastructure-tests PROPERTIES
            LABELS "tilefinch;unit;tooling" TIMEOUT 30)
        if(TARGET tilefinch-video-motion)
            # Recorded-gameplay analyzer on synthetic ffmpeg clips (reports
            # Skipped, exit 77, without ffmpeg; no emulator).
            add_test(NAME tilefinch-game-video-analyzer-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_analyze_game_video.py
                    $<TARGET_FILE:tilefinch-video-motion>
                    $<TARGET_FILE:tilefinch-video-blink>)
            set_tests_properties(tilefinch-game-video-analyzer-tests PROPERTIES
                LABELS "tilefinch;unit;visual;tooling" TIMEOUT 120
                SKIP_RETURN_CODE 77)
        endif()
        add_test(NAME tilefinch-js-profile-report-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_js_profile_report.py)
        set_tests_properties(tilefinch-js-profile-report-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;tooling" TIMEOUT 10)
        add_test(NAME tilefinch-work-vector-report-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_work_vector_report.py)
        set_tests_properties(tilefinch-work-vector-report-tests PROPERTIES
            LABELS "tilefinch;unit;performance;tooling" TIMEOUT 10)
        add_test(NAME tilefinch-load-timeline-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_load_timeline.py)
        set_tests_properties(tilefinch-load-timeline-tests PROPERTIES
            LABELS "tilefinch;unit;performance;tooling" TIMEOUT 10)
        add_test(NAME tilefinch-buffered-reply-harness-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_buffered_reply_harness.py)
        set_tests_properties(tilefinch-buffered-reply-harness-tests PROPERTIES
            LABELS "tilefinch;unit;performance;tooling" TIMEOUT 10)
        add_test(NAME tilefinch-native-tier-sample-report-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_tier_sample_report.py)
        set_tests_properties(tilefinch-native-tier-sample-report-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;tooling" TIMEOUT 10)
        add_test(NAME tilefinch-native-tier-admission-report-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_tier_admission_report.py)
        set_tests_properties(tilefinch-native-tier-admission-report-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;tooling" TIMEOUT 10)
        if(TARGET psp-browser-interactive-lab)
            add_test(NAME tilefinch-interactive-until-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_interactive_until.py
                    $<TARGET_FILE:psp-browser-interactive-lab>)
            set_tests_properties(tilefinch-interactive-until-tests PROPERTIES
                LABELS "tilefinch;unit;performance;tooling" TIMEOUT 30)
            add_test(NAME tilefinch-js-profile-accounting-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_js_profile_accounting.py
                    $<TARGET_FILE:psp-browser-interactive-lab>)
            set_tests_properties(tilefinch-js-profile-accounting-tests PROPERTIES
                LABELS "tilefinch;unit;javascript;tooling" TIMEOUT 30)
            add_test(NAME tilefinch-work-vector-determinism-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_work_vector_determinism.py
                    $<TARGET_FILE:psp-browser-interactive-lab>)
            set_tests_properties(tilefinch-work-vector-determinism-tests PROPERTIES
                LABELS "tilefinch;unit;performance;tooling" TIMEOUT 120)
            add_test(NAME tilefinch-site-census-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_site_census.py
                    $<TARGET_FILE:psp-browser-interactive-lab>)
            set_tests_properties(tilefinch-site-census-tests PROPERTIES
                LABELS "tilefinch;unit;tooling" TIMEOUT 120)
            add_test(NAME tilefinch-declarative-refresh-lab-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_declarative_refresh_lab.py
                    $<TARGET_FILE:psp-browser-interactive-lab>
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/http-declarative-refresh)
            set_tests_properties(tilefinch-declarative-refresh-lab-tests PROPERTIES
                LABELS "tilefinch;unit;navigation;security;tooling" TIMEOUT 60)
        endif()
        add_test(NAME tilefinch-cursor-cadence-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cursor_cadence.py)
        set_tests_properties(tilefinch-cursor-cadence-tests PROPERTIES
            LABELS "tilefinch;unit;input;performance" TIMEOUT 10)
        add_test(NAME tilefinch-input-timeline-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_input_timeline.py)
        set_tests_properties(tilefinch-input-timeline-tests PROPERTIES
            LABELS "tilefinch;unit;input;performance" TIMEOUT 10)
        add_test(NAME tilefinch-bootstrap-generated-check
            COMMAND tilefinch_bootstrap_bytecode_generator --check
                "${TILEFINCH_BOOTSTRAP_SOURCE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap_bytecode.c"
                "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(tilefinch-bootstrap-generated-check PROPERTIES
            TIMEOUT 30)
        add_test(NAME tilefinch-bootstrap-verification-cache-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bootstrap_verification_cache.py)
        set_tests_properties(tilefinch-bootstrap-verification-cache-tests PROPERTIES
            LABELS "tilefinch;unit;bootstrap;tooling"
            TIMEOUT 30)
        add_test(NAME tilefinch-bootstrap-global-owner-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bootstrap_global_owners.py
                ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tilefinch-bootstrap-global-owner-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;bootstrap;architecture"
            TIMEOUT 10)
        # The computed-style registry generates membership, enumeration and
        # the getter's identifiers; this checks what the compiler cannot.
        add_test(NAME tilefinch-computed-style-registry-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_computed_style_registry.py
                ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tilefinch-computed-style-registry-tests
            PROPERTIES LABELS "tilefinch;unit;architecture" TIMEOUT 20)
        add_test(NAME tilefinch-diagnostic-switch-registry-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_diagnostic_switches.py
                ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tilefinch-diagnostic-switch-registry-tests
            PROPERTIES LABELS "tilefinch;unit;architecture" TIMEOUT 20)
        add_test(NAME tilefinch-public-tree-hygiene-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_public_tree_hygiene.py
                ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tilefinch-public-tree-hygiene-tests PROPERTIES
            LABELS "tilefinch;unit;release;tooling" TIMEOUT 10)
        # Source scans (about 10 s alone).
        add_test(NAME tilefinch-psp-sdk-contract-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_psp_sdk_contracts.py
                ${CMAKE_CURRENT_SOURCE_DIR} PspSdkContractTests)
        set_tests_properties(tilefinch-psp-sdk-contract-tests PROPERTIES
            LABELS "tilefinch;unit;psp;architecture"
            TIMEOUT 60)
        # The QuickJS variant case runs nine real CMake configures that copy
        # and patch the vendored engine (about 22 s alone, over 60 s at load
        # 30), so it is its own test and overlaps the scans.
        add_test(NAME tilefinch-psp-sdk-quickjs-variant-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_psp_sdk_contracts.py
                ${CMAKE_CURRENT_SOURCE_DIR} QuickJsVariantPreparationTests)
        set_tests_properties(tilefinch-psp-sdk-quickjs-variant-tests PROPERTIES
            LABELS "tilefinch;unit;psp;architecture"
            TIMEOUT 180)
        add_test(NAME tilefinch-psp-game-stage-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage_psp_game.py)
        set_tests_properties(tilefinch-psp-game-stage-tests PROPERTIES
            LABELS "tilefinch;unit;psp;game;tooling"
            TIMEOUT 10)
        add_test(NAME tilefinch-ca-bundle-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ca_bundle.py
                ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tilefinch-ca-bundle-tests PROPERTIES
            LABELS "tilefinch;unit;security;tls"
            TIMEOUT 10)
    endif()
    tilefinch_add_test_binary(tilefinch-tests tests/test_tilefinch.c)
    target_link_libraries(tilefinch-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        TILEFINCH_TEST_SANS_FONT="${PSP_BROWSER_SANS_FONT}"
        TILEFINCH_TEST_SERIF_FONT="${PSP_BROWSER_SERIF_FONT}"
        TILEFINCH_TEST_SANS_ITALIC_FONT="${PSP_BROWSER_SANS_ITALIC_FONT}"
        TILEFINCH_TEST_SANS_BOLD_FONT="${PSP_BROWSER_SANS_BOLD_FONT}"
        TILEFINCH_TEST_SERIF_BOLD_FONT="${PSP_BROWSER_SERIF_BOLD_FONT}"
        TILEFINCH_TEST_METRIC_SANS_FONT="${PSP_BROWSER_METRIC_SANS_FONT}"
        TILEFINCH_TEST_METRIC_SANS_BOLD_FONT="${PSP_BROWSER_METRIC_SANS_BOLD_FONT}")

    tilefinch_add_test_binary(tilefinch-gamepad-tests tests/test_gamepad.c)
    target_link_libraries(tilefinch-gamepad-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-gamepad-tests COMMAND tilefinch-gamepad-tests)
    set_tests_properties(tilefinch-gamepad-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;input" TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-multiplayer-tests tests/test_multiplayer.c)
    target_link_libraries(tilefinch-multiplayer-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-multiplayer-tests COMMAND tilefinch-multiplayer-tests)
    set_tests_properties(tilefinch-multiplayer-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;network" TIMEOUT 10)
    if(NOT PSP)
        find_program(TILEFINCH_TEST_NODE_EXECUTABLE NAMES node)
        # Treadline's Node tests. Without node they are still registered,
        # as tests that report Skipped (exit 77), so a host without node
        # shows the missing coverage instead of silently running fewer tests.
        function(tilefinch_add_node_test name script)
            cmake_parse_arguments(NODE_TEST "" "TIMEOUT;LABELS"
                "NODE_ARGS;ARGS" ${ARGN})
            if(TILEFINCH_TEST_NODE_EXECUTABLE)
                add_test(NAME ${name}
                    COMMAND ${TILEFINCH_TEST_NODE_EXECUTABLE} ${NODE_TEST_NODE_ARGS}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/${script}
                        ${NODE_TEST_ARGS})
            else()
                add_test(NAME ${name}
                    COMMAND ${Python3_EXECUTABLE} -c
                        "import sys; print('SKIP: node was not found when CMake configured this tree'); sys.exit(77)")
            endif()
            set_tests_properties(${name} PROPERTIES
                LABELS "${NODE_TEST_LABELS}" TIMEOUT ${NODE_TEST_TIMEOUT}
                SKIP_RETURN_CODE 77)
        endfunction()
        tilefinch_add_node_test(tilefinch-treadline-web-multiplayer-tests
            test_treadline_web_multiplayer.js
            LABELS "tilefinch;unit;javascript;network;game" TIMEOUT 10)
        # index.html ends the "Starting..." wait when a game script fails
        # to load (its element's error event).
        tilefinch_add_node_test(tilefinch-treadline-boot-failure-tests
            test_treadline_boot_failure.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 10)
        tilefinch_add_node_test(tilefinch-treadline-camera-motion-tests
            test_treadline_camera_motion.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 10)
        tilefinch_add_node_test(tilefinch-treadline-ai-queue-tests
            test_treadline_ai_queue.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 10)
        tilefinch_add_node_test(tilefinch-treadline-bank-windup-tests
            test_treadline_bank_windup.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 10)
        tilefinch_add_node_test(tilefinch-treadline-engine-voice-tests
            test_treadline_engine_voice.js
            LABELS "tilefinch;unit;javascript;game;audio" TIMEOUT 10)
        tilefinch_add_node_test(tilefinch-treadline-audio-param-tests
            test_treadline_audio_params.js
            LABELS "tilefinch;unit;javascript;game;audio" TIMEOUT 10)
        # Browser Web Audio automation rules (curve overlap), which the
        # bootstrap subset does not enforce.
        tilefinch_add_node_test(tilefinch-treadline-audio-automation-tests
            test_treadline_audio_automation.js
            LABELS "tilefinch;unit;javascript;game;audio" TIMEOUT 60)
        tilefinch_add_node_test(tilefinch-treadline-tiny-query-tests
            test_treadline_tiny_queries.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 10)
        tilefinch_add_node_test(tilefinch-treadline-ray-query-tests
            test_treadline_ray_queries.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 10)
        tilefinch_add_node_test(tilefinch-treadline-objective-instance-tests
            test_treadline_objective_instances.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 10)
        tilefinch_add_node_test(tilefinch-treadline-retained-effect-tests
            test_treadline_retained_effects.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 30)
        tilefinch_add_node_test(tilefinch-treadline-partial-route-tests
            test_treadline_partial_routes.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 30)
        tilefinch_add_node_test(tilefinch-treadline-optional-admission-tests
            test_treadline_optional_admission.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 30)
        tilefinch_add_node_test(tilefinch-treadline-tank-tail-tests
            test_treadline_tank_tail.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 30)
        tilefinch_add_node_test(tilefinch-treadline-hazard-query-tests
            test_treadline_hazard_queries.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 30)
        # Page-URL switches: seed= only in qualification runs, whole names.
        tilefinch_add_node_test(tilefinch-treadline-url-switch-tests
            test_treadline_url_switches.js
            LABELS "tilefinch;unit;javascript;game" TIMEOUT 10)
        # Adaptive music: state machine, scheduling bounds, effect
        # priority, settings and determinism on the shipped scripts.
        tilefinch_add_node_test(tilefinch-treadline-music-tests
            test_treadline_music.js
            NODE_ARGS --expose-gc --no-concurrent-recompilation
            LABELS "tilefinch;unit;javascript;game;audio" TIMEOUT 60)
        # The same recorded commands replayed through the real mixer.
        tilefinch_add_node_test(tilefinch-treadline-music-render-tests
            test_treadline_music_render.js
            ARGS $<TARGET_FILE:tilefinch-game-audio-render>
                ${CMAKE_CURRENT_BINARY_DIR}/treadline-music-render
            LABELS "tilefinch;unit;javascript;game;audio" TIMEOUT 60)
    endif()

    tilefinch_add_test_binary(tilefinch-xmb-redirect-policy-tests
        tests/test_xmb_redirect_policy.c
        src/xmb_redirect_policy.c)
    target_include_directories(tilefinch-xmb-redirect-policy-tests PRIVATE
        include)
    add_test(NAME tilefinch-xmb-redirect-policy-tests
        COMMAND tilefinch-xmb-redirect-policy-tests)
    set_tests_properties(tilefinch-xmb-redirect-policy-tests PROPERTIES
        LABELS "tilefinch;unit;psp;xmb" TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-layout-tests tests/test_layout.c)
    target_link_libraries(tilefinch-layout-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-layout-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        TILEFINCH_TEST_SANS_FONT="${PSP_BROWSER_SANS_FONT}"
        TILEFINCH_TEST_SERIF_FONT="${PSP_BROWSER_SERIF_FONT}"
        TILEFINCH_TEST_SANS_ITALIC_FONT="${PSP_BROWSER_SANS_ITALIC_FONT}"
        TILEFINCH_TEST_SANS_BOLD_FONT="${PSP_BROWSER_SANS_BOLD_FONT}"
        TILEFINCH_TEST_SERIF_BOLD_FONT="${PSP_BROWSER_SERIF_BOLD_FONT}"
        TILEFINCH_TEST_METRIC_SANS_FONT="${PSP_BROWSER_METRIC_SANS_FONT}"
        TILEFINCH_TEST_METRIC_SANS_BOLD_FONT="${PSP_BROWSER_METRIC_SANS_BOLD_FONT}")

    tilefinch_add_test_binary(tilefinch-text-bidi-tests tests/test_text_bidi.c)
    target_link_libraries(tilefinch-text-bidi-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-text-bidi-tests COMMAND tilefinch-text-bidi-tests)
    set_tests_properties(tilefinch-text-bidi-tests PROPERTIES
        LABELS "tilefinch;unit;layout;unicode" TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-paint-image-geometry-tests
        tests/test_paint_image_geometry.c)
    target_link_libraries(tilefinch-paint-image-geometry-tests PRIVATE
        tilefinch_core)
    add_test(NAME tilefinch-paint-image-geometry-tests
        COMMAND tilefinch-paint-image-geometry-tests)
    set_tests_properties(tilefinch-paint-image-geometry-tests PROPERTIES
        LABELS "tilefinch;unit;layout;image" TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-media-mp4-tests tests/test_media_mp4.c)
    target_link_libraries(tilefinch-media-mp4-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-media-mp4-tests PRIVATE
        TILEFINCH_TEST_MEDIA_FIXTURE_240="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/psp-media/baseline-320x240.mp4"
        TILEFINCH_TEST_MEDIA_FIXTURE_360="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/psp-media/main-640x360.mp4")
    add_test(NAME tilefinch-media-mp4-tests COMMAND tilefinch-media-mp4-tests)
    set_tests_properties(tilefinch-media-mp4-tests PROPERTIES
        LABELS "tilefinch;unit;media" TIMEOUT 30)
    tilefinch_add_test_binary(tilefinch-swdec-ts-tests
        tests/test_swdec_ts.c src/swdec/swdec_ts.c)
    target_include_directories(tilefinch-swdec-ts-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/swdec)
    add_test(NAME tilefinch-swdec-ts-tests COMMAND tilefinch-swdec-ts-tests)
    set_tests_properties(tilefinch-swdec-ts-tests PROPERTIES
        LABELS "tilefinch;unit;media;parser" TIMEOUT 30)
    tilefinch_add_test_binary(tilefinch-swdec-bounds-tests tests/test_swdec_bounds.c)
    target_include_directories(tilefinch-swdec-bounds-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/swdec)
    add_test(NAME tilefinch-swdec-bounds-tests
        COMMAND tilefinch-swdec-bounds-tests)
    set_tests_properties(tilefinch-swdec-bounds-tests PROPERTIES
        LABELS "tilefinch;unit;media;security" TIMEOUT 10)
    tilefinch_add_test_binary(tilefinch-media-hls-tests tests/test_media_hls.c)
    target_link_libraries(tilefinch-media-hls-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-media-hls-tests COMMAND tilefinch-media-hls-tests)
    set_tests_properties(tilefinch-media-hls-tests PROPERTIES
        LABELS "tilefinch;unit;media;network;parser" TIMEOUT 30)
    tilefinch_add_test_binary(tilefinch-psp-hls-gzip-tests
        tests/test_psp_hls_gzip.c src/psp_hls_gzip.c)
    target_include_directories(tilefinch-psp-hls-gzip-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tilefinch-psp-hls-gzip-tests PRIVATE
        tilefinch_core ZLIB::ZLIB)
    add_test(NAME tilefinch-psp-hls-gzip-tests
        COMMAND tilefinch-psp-hls-gzip-tests)
    set_tests_properties(tilefinch-psp-hls-gzip-tests PROPERTIES
        LABELS "tilefinch;unit;media;network;compression" TIMEOUT 30)
    tilefinch_add_test_binary(tilefinch-reader-mode-tests tests/test_reader_mode.c)
    target_link_libraries(tilefinch-reader-mode-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-reader-mode-tests COMMAND tilefinch-reader-mode-tests)
    set_tests_properties(tilefinch-reader-mode-tests PROPERTIES
        LABELS "tilefinch;unit;reader;performance" TIMEOUT 30)
    tilefinch_add_test_binary(tilefinch-host-media-timing-tests
        tests/test_host_media_timing.c)
    add_test(NAME tilefinch-host-media-timing-tests
        COMMAND tilefinch-host-media-timing-tests)
    set_tests_properties(tilefinch-host-media-timing-tests PROPERTIES
        LABELS "tilefinch;unit;media" TIMEOUT 30)
    tilefinch_add_test_binary(tilefinch-media-promotion-tests
        tests/test_media_promotion.c)
    target_include_directories(tilefinch-media-promotion-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tilefinch-media-promotion-tests PRIVATE
        tilefinch_core)
    add_test(NAME tilefinch-media-promotion-tests
        COMMAND tilefinch-media-promotion-tests)
    set_tests_properties(tilefinch-media-promotion-tests PROPERTIES
        LABELS "tilefinch;unit;media;psp;promotion;memory" TIMEOUT 10)
    # This advances virtual media time and finishes in milliseconds, but it is
    # deliberately absent from CTest and every default build/test lane. Run
    # only when sustained cadence policy is the subject of the milestone.
    add_custom_target(tilefinch-occasional-media-timing
        COMMAND $<TARGET_FILE:tilefinch-host-media-timing-tests>
            --one-hour-simulation
        DEPENDS tilefinch-host-media-timing-tests
        COMMENT "Running opt-in one-hour virtual media timing simulation"
        VERBATIM)
    tilefinch_add_test_binary(tilefinch-media-dom-tests tests/test_media_dom.c)
    target_link_libraries(tilefinch-media-dom-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-media-dom-tests COMMAND tilefinch-media-dom-tests)
    set_tests_properties(tilefinch-media-dom-tests PROPERTIES
        LABELS "tilefinch;unit;media;javascript" TIMEOUT 30)

    function(tilefinch_add_unit_suite test_name)
        if(ARGC GREATER 1)
            add_test(NAME ${test_name}
                COMMAND tilefinch-tests --filter ${ARGV1})
        else()
            add_test(NAME ${test_name} COMMAND tilefinch-tests)
        endif()
        set_tests_properties(${test_name} PROPERTIES
            LABELS "tilefinch;unit"
            TIMEOUT 120)
    endfunction()

    # Keep the aggregate executable/CLI for compatibility, but do not register
    # it in default CTest: the four suites below cover the same inventory and
    # would otherwise make every large test run twice.
    tilefinch_add_unit_suite(tilefinch-foundation-tests foundation)
    tilefinch_add_unit_suite(tilefinch-web-runtime-tests web-runtime)
    add_test(NAME tilefinch-layout-tests COMMAND tilefinch-layout-tests)
    set_tests_properties(tilefinch-layout-tests PROPERTIES
        LABELS "tilefinch;unit"
        TIMEOUT 120)
    tilefinch_add_unit_suite(tilefinch-section-tests sections)

    tilefinch_add_test_binary(tilefinch-url-tests tests/test_url.c)
    target_link_libraries(tilefinch-url-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-url-tests COMMAND tilefinch-url-tests)
    set_tests_properties(tilefinch-url-tests PROPERTIES
        LABELS "tilefinch;unit;security"
        TIMEOUT 30)

    # Per-site storage: RAM allowances and growth, the Memory Stick offer,
    # both stick tiers across a reboot, torn logs and compaction.
    tilefinch_add_test_binary(tilefinch-session-site-storage-tests
        tests/test_session_site_storage.c)
    target_link_libraries(tilefinch-session-site-storage-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-session-site-storage-tests
        COMMAND tilefinch-session-site-storage-tests)
    set_tests_properties(tilefinch-session-site-storage-tests PROPERTIES
        LABELS "tilefinch;unit;storage"
        TIMEOUT 30)
    tilefinch_add_test_binary(tilefinch-session-security-tests
        tests/test_session_security.c)
    target_link_libraries(tilefinch-session-security-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-session-security-tests
        COMMAND tilefinch-session-security-tests)
    set_tests_properties(tilefinch-session-security-tests PROPERTIES
        LABELS "tilefinch;unit;security"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-content-security-policy-tests
        tests/test_content_security_policy.c)
    target_link_libraries(tilefinch-content-security-policy-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-content-security-policy-tests
        COMMAND tilefinch-content-security-policy-tests)
    set_tests_properties(tilefinch-content-security-policy-tests PROPERTIES
        LABELS "tilefinch;unit;security;web-platform"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-csp-page-tests
        tests/test_csp_pages.c)
    target_link_libraries(tilefinch-csp-page-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-csp-page-tests
        COMMAND tilefinch-csp-page-tests)
    set_tests_properties(tilefinch-csp-page-tests PROPERTIES
        LABELS "tilefinch;unit;security;web-platform"
        TIMEOUT 60)

    tilefinch_add_test_binary(tilefinch-resource-integrity-tests
        tests/test_resource_integrity.c)
    target_link_libraries(tilefinch-resource-integrity-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-resource-integrity-tests
        COMMAND tilefinch-resource-integrity-tests)
    set_tests_properties(tilefinch-resource-integrity-tests PROPERTIES
        LABELS "tilefinch;unit;security;web-platform"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-frame-sandbox-tests
        tests/test_frame_sandbox.c)
    target_link_libraries(tilefinch-frame-sandbox-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-frame-sandbox-tests
        COMMAND tilefinch-frame-sandbox-tests)
    set_tests_properties(tilefinch-frame-sandbox-tests PROPERTIES
        LABELS "tilefinch;unit;security;web-platform"
        TIMEOUT 30)

    # Semantic proof for the Allegrex bignum core in
    # patches/mbedtls-3.6.6-psp-bnmul.patch. Pure arithmetic, no
    # dependencies: it models the maddu accumulator and compares it limb
    # for limb with both portable mbed TLS MULADDC cores. Runs on every
    # host build, including this one, which does not link that patch --
    # the point is to catch a wrong carry before a device build exists.
    tilefinch_add_test_binary(tilefinch-bn-mul-allegrex-tests
        tests/test_bn_mul_allegrex.c)
    add_test(NAME tilefinch-bn-mul-allegrex-tests
        COMMAND tilefinch-bn-mul-allegrex-tests)
    set_tests_properties(tilefinch-bn-mul-allegrex-tests PROPERTIES
        LABELS "tilefinch;unit;security;psp;tls"
        TIMEOUT 60)

    tilefinch_add_test_binary(tilefinch-update-tests tests/test_update.c)
    target_link_libraries(tilefinch-update-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-update-tests COMMAND tilefinch-update-tests)
    set_tests_properties(tilefinch-update-tests PROPERTIES
        LABELS "tilefinch;unit;security;update"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-voice-component-tests
        tests/test_voice_component.c)
    target_link_libraries(tilefinch-voice-component-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-voice-component-tests
        COMMAND tilefinch-voice-component-tests)
    set_tests_properties(tilefinch-voice-component-tests PROPERTIES
        LABELS "tilefinch;unit;psp;update;storage;security"
        TIMEOUT 30)
    # The session source is compiled in with its host-only identity seam so
    # unsigned fixture packs exercise the real install/re-attach path.
    tilefinch_add_test_binary(tilefinch-glyph-component-tests
        tests/test_glyph_component.c src/psp_glyph_component_session.c)
    target_link_libraries(tilefinch-glyph-component-tests
        PRIVATE tilefinch_core tilefinch_psp_ui)
    target_compile_definitions(tilefinch-glyph-component-tests PRIVATE
        TILEFINCH_GLYPH_SESSION_TEST_SEAM=1
        TILEFINCH_TEST_SANS_FONT="${PSP_BROWSER_SANS_FONT}"
        TILEFINCH_TEST_PSP_SANS_FONT="${PSP_BROWSER_PSP_SANS_FONT}")
    add_test(NAME tilefinch-glyph-component-tests
        COMMAND tilefinch-glyph-component-tests)
    set_tests_properties(tilefinch-glyph-component-tests PROPERTIES
        LABELS "tilefinch;unit;font;storage;security"
        TIMEOUT 30)
    # Signing-ceremony rehearsal against the real embedded root. Skips
    # (exit 77) unless the build embeds an update root and the rehearsal
    # artifact paths are supplied via TILEFINCH_PROOF_* environment
    # variables; see docs/RELEASE_PROCESS.md.
    tilefinch_add_test_binary(tilefinch-update-root-proof-tests
        tests/test_update_root_proof.c)
    target_link_libraries(tilefinch-update-root-proof-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-update-root-proof-tests
        COMMAND tilefinch-update-root-proof-tests)
    set_tests_properties(tilefinch-update-root-proof-tests PROPERTIES
        LABELS "tilefinch;security;update;release"
        SKIP_RETURN_CODE 77
        TIMEOUT 60)
    tilefinch_add_test_binary(tilefinch-install-path-tests tests/test_install_paths.c)
    target_link_libraries(tilefinch-install-path-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-install-path-tests
        COMMAND tilefinch-install-path-tests)
    set_tests_properties(tilefinch-install-path-tests PROPERTIES
        LABELS "tilefinch;unit;storage;psp;update"
        TIMEOUT 10)
    add_test(NAME tilefinch-update-tool-tests
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_tool.py")
    set_tests_properties(tilefinch-update-tool-tests PROPERTIES
        LABELS "tilefinch;unit;security;update;tooling"
        TIMEOUT 30)
    if(NOT PSP)
        add_test(NAME tilefinch-local-update-server-tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_local_update_server.py")
        set_tests_properties(tilefinch-local-update-server-tests PROPERTIES
            LABELS "tilefinch;unit;security;update;tooling;network"
            TIMEOUT 30)
    endif()

    tilefinch_add_test_binary(tilefinch-session-persistence-tests
        tests/test_session_persistence.c)
    target_link_libraries(tilefinch-session-persistence-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-session-persistence-tests
        COMMAND tilefinch-session-persistence-tests)
    set_tests_properties(tilefinch-session-persistence-tests PROPERTIES
        LABELS "tilefinch;unit;session"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-youtube-login-tests tests/test_youtube_login.c)
    target_link_libraries(tilefinch-youtube-login-tests PRIVATE tilefinch_core tilefinch_psp_ui)
    add_test(NAME tilefinch-youtube-login-tests COMMAND tilefinch-youtube-login-tests)
    set_tests_properties(tilefinch-youtube-login-tests PROPERTIES LABELS "tilefinch;unit;session;youtube" TIMEOUT 30)
    add_test(NAME tilefinch-youtube-login-journey COMMAND tilefinch-browser-engine-tests --youtube-login-journey)
    set_tests_properties(tilefinch-youtube-login-journey PROPERTIES LABELS "tilefinch;journey;youtube" TIMEOUT 60)

    # Cross-boot TLS session store (docs/engineering/PSP_TRANSPORT.md).
    # Pure data plus file I/O; does
    # not link curl or Mbed TLS. Covers the round-trip, corruption -> miss,
    # LRU eviction, expiry/wrong-RTC pruning, over-cap skip, and the
    # store-version and crypto-pin mismatch gates.
    tilefinch_add_test_binary(tilefinch-tls-session-store-tests
        tests/test_tls_session_store.c)
    target_link_libraries(tilefinch-tls-session-store-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-tls-session-store-tests
        COMMAND tilefinch-tls-session-store-tests)
    set_tests_properties(tilefinch-tls-session-store-tests PROPERTIES
        LABELS "tilefinch;unit;session;tls"
        TIMEOUT 30)

    # Speculative preconnect (docs/engineering/PSP_TRANSPORT.md): the
    # pure dwell/eligibility state machine -- one outstanding, ~300 ms dwell,
    # cancel-on-focus-change, gated on network-ready/not-quiescing -- plus the
    # transport API's callable/inert-safe accounting (started/reused/cancelled).
    tilefinch_add_test_binary(tilefinch-fetch-preconnect-tests
        tests/test_fetch_preconnect.c)
    target_link_libraries(tilefinch-fetch-preconnect-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-fetch-preconnect-tests
        COMMAND tilefinch-fetch-preconnect-tests)
    set_tests_properties(tilefinch-fetch-preconnect-tests PROPERTIES
        LABELS "tilefinch;unit;fetch;tls"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-fetch-stream-scheduler-tests
        tests/test_fetch_stream_scheduler.c)
    target_link_libraries(tilefinch-fetch-stream-scheduler-tests
        PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-fetch-stream-scheduler-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-fetch-stream-scheduler-tests
        COMMAND tilefinch-fetch-stream-scheduler-tests)
    set_tests_properties(tilefinch-fetch-stream-scheduler-tests PROPERTIES
        LABELS "tilefinch;unit;network;streaming"
        TIMEOUT 30)

    if(NOT PSP)
        tilefinch_add_test_binary(tilefinch-fetch-trace-disabled-tests
            tests/test_fetch_trace_disabled.c src/fetch.c)
        target_compile_definitions(tilefinch-fetch-trace-disabled-tests PRIVATE
            TILEFINCH_NO_FETCH_TRACE=1)
        target_link_libraries(tilefinch-fetch-trace-disabled-tests PRIVATE
            tilefinch_core)
        add_test(NAME tilefinch-fetch-trace-disabled-tests
            COMMAND tilefinch-fetch-trace-disabled-tests)
        set_tests_properties(tilefinch-fetch-trace-disabled-tests PROPERTIES
            LABELS "tilefinch;unit;network;size" TIMEOUT 10)
    endif()

    tilefinch_add_test_binary(tilefinch-style-index-tests tests/test_style_index.c)
    target_link_libraries(tilefinch-style-index-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-style-index-tests COMMAND tilefinch-style-index-tests)
    set_tests_properties(tilefinch-style-index-tests PROPERTIES
        LABELS "tilefinch;unit;style;performance"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-navigation-load-tests
        tests/test_navigation_load.c)
    target_link_libraries(tilefinch-navigation-load-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-navigation-load-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-navigation-load-tests
        COMMAND tilefinch-navigation-load-tests)
    set_tests_properties(tilefinch-navigation-load-tests PROPERTIES
        LABELS "tilefinch;unit;network;streaming;navigation"
        # This executable compares real background-transport deadlines with
        # parser watchdog time. A counting clock cannot model its independent
        # worker; parallel CPU-heavy fixtures inflate the wall-time ratchet.
        RUN_SERIAL TRUE TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-browser-engine-tests
        tests/test_browser_engine.c
        tests/test_browser_engine_journeys.c
        tests/test_browser_engine_interruptions.c
        tests/test_browser_engine_cleanups.c)
    target_link_libraries(tilefinch-browser-engine-tests PRIVATE tilefinch_core tilefinch_psp_ui)
    target_compile_definitions(tilefinch-browser-engine-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-browser-engine-tests
        COMMAND tilefinch-browser-engine-tests)
    set_tests_properties(tilefinch-browser-engine-tests PROPERTIES
        LABELS "tilefinch;unit;architecture;lifecycle"
        ENVIRONMENT "TILEFINCH_TRACE_TASKS=1"
        TIMEOUT 30)
    add_test(NAME tilefinch-search-video-journey-tests
        COMMAND tilefinch-browser-engine-tests --search-video-journey-only)
    set_tests_properties(tilefinch-search-video-journey-tests PROPERTIES
        LABELS "tilefinch;journey;navigation;media"
        TIMEOUT 30)
    # Every registered computed-style property, read from a fixture page that
    # authors most of them, against a checked-in dump: a changed resolved
    # value is a reviewable diff. Regenerate with --computed-style-dump.
    add_test(NAME tilefinch-computed-style-golden-tests
        COMMAND tilefinch-browser-engine-tests --computed-style-golden
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/computed-style-dump.html
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/computed-style-dump.golden.txt
            ${CMAKE_CURRENT_BINARY_DIR}/computed-style-dump.actual.txt)
    set_tests_properties(tilefinch-computed-style-golden-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;style"
        TIMEOUT 60)

    tilefinch_add_test_binary(tilefinch-declarative-refresh-tests
        tests/test_declarative_refresh.c)
    target_link_libraries(tilefinch-declarative-refresh-tests PRIVATE
        tilefinch_core)
    target_compile_definitions(tilefinch-declarative-refresh-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-declarative-refresh-tests
        COMMAND tilefinch-declarative-refresh-tests)
    set_tests_properties(tilefinch-declarative-refresh-tests PROPERTIES
        LABELS "tilefinch;unit;navigation;security;psp"
        TIMEOUT 60)

    tilefinch_add_test_binary(tilefinch-browser-profile-tests
        tests/test_browser_profile.c)
    target_link_libraries(tilefinch-browser-profile-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-browser-profile-tests
        COMMAND tilefinch-browser-profile-tests)
    set_tests_properties(tilefinch-browser-profile-tests PROPERTIES
        LABELS "tilefinch;unit;storage;psp"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-page-find-tests tests/test_page_find.c)
    target_link_libraries(tilefinch-page-find-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-page-find-tests COMMAND tilefinch-page-find-tests)
    set_tests_properties(tilefinch-page-find-tests PROPERTIES
        LABELS "tilefinch;unit;navigation;layout;psp"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-content-blocker-tests
        tests/test_content_blocker.c)
    target_link_libraries(tilefinch-content-blocker-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-content-blocker-tests
        COMMAND tilefinch-content-blocker-tests)
    set_tests_properties(tilefinch-content-blocker-tests PROPERTIES
        LABELS "tilefinch;unit;network;security;psp"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-browser-tabs-tests
        tests/test_browser_tabs.c)
    target_link_libraries(tilefinch-browser-tabs-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-browser-tabs-tests
        COMMAND tilefinch-browser-tabs-tests)
    set_tests_properties(tilefinch-browser-tabs-tests PROPERTIES
        LABELS "tilefinch;unit;navigation;psp"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-offline-library-tests
        tests/test_offline_library.c src/psp_offline_store.c)
    target_link_libraries(tilefinch-offline-library-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-offline-library-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-offline-library-tests
        COMMAND tilefinch-offline-library-tests)
    set_tests_properties(tilefinch-offline-library-tests PROPERTIES
        LABELS "tilefinch;unit;storage;psp"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-offline-app-limits-tests
        tests/test_offline_app_limits.c)
    target_link_libraries(tilefinch-offline-app-limits-tests
        PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-offline-app-limits-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-offline-app-limits-tests
        COMMAND tilefinch-offline-app-limits-tests)
    set_tests_properties(tilefinch-offline-app-limits-tests PROPERTIES
        LABELS "tilefinch;unit;storage;psp"
        TIMEOUT 120)

    tilefinch_add_test_binary(tilefinch-offline-app-launch-tests
        tests/test_offline_app_launch.c src/psp_offline_store.c)
    target_link_libraries(tilefinch-offline-app-launch-tests
        PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-offline-app-launch-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-offline-app-launch-tests
        COMMAND tilefinch-offline-app-launch-tests)
    set_tests_properties(tilefinch-offline-app-launch-tests PROPERTIES
        LABELS "tilefinch;unit;storage;javascript;game;psp"
        TIMEOUT 120)

    tilefinch_add_test_binary(tilefinch-canvas-overlay-paint-tests
        tests/test_canvas_overlay_paint.c)
    target_link_libraries(tilefinch-canvas-overlay-paint-tests
        PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-canvas-overlay-paint-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-canvas-overlay-paint-tests
        COMMAND tilefinch-canvas-overlay-paint-tests)
    set_tests_properties(tilefinch-canvas-overlay-paint-tests PROPERTIES
        LABELS "tilefinch;unit;render;javascript;psp"
        TIMEOUT 120)

    tilefinch_add_test_binary(tilefinch-screenshot-png-tests
        tests/test_screenshot_png.c src/screenshot_png.c)
    target_include_directories(tilefinch-screenshot-png-tests PRIVATE include)
    add_test(NAME tilefinch-screenshot-png-tests
        COMMAND tilefinch-screenshot-png-tests)
    set_tests_properties(tilefinch-screenshot-png-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui;storage"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-danzeff-input-tests
        tests/test_danzeff_input.c src/danzeff_input.c)
    target_include_directories(tilefinch-danzeff-input-tests PRIVATE include)
    add_test(NAME tilefinch-danzeff-input-tests
        COMMAND tilefinch-danzeff-input-tests)
    set_tests_properties(tilefinch-danzeff-input-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui;input"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-omnibox-tests tests/test_omnibox.c)
    target_link_libraries(tilefinch-omnibox-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-omnibox-tests COMMAND tilefinch-omnibox-tests)
    set_tests_properties(tilefinch-omnibox-tests PROPERTIES
        LABELS "tilefinch;unit;navigation;psp"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-stt-component-loader-tests
        tests/test_stt_component_loader.c src/stt/stt_component_loader.c)
    target_include_directories(tilefinch-stt-component-loader-tests PRIVATE
        tests/stt_psp_stubs src/stt)
    target_link_libraries(tilefinch-stt-component-loader-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-stt-component-loader-tests
        COMMAND tilefinch-stt-component-loader-tests)
    set_tests_properties(tilefinch-stt-component-loader-tests PROPERTIES
        LABELS "tilefinch;unit;voice" TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-game-audio-mix-tests
        tests/test_game_audio_mix.c)
    target_link_libraries(tilefinch-game-audio-mix-tests PRIVATE tilefinch_core)
    # Host replay/renderer for recorded game-audio command logs (music
    # previews and tilefinch-treadline-music-render-tests).
    tilefinch_add_test_binary(tilefinch-game-audio-render tests/game_audio_render.c)
    target_link_libraries(tilefinch-game-audio-render PRIVATE tilefinch_core)
    add_test(NAME tilefinch-game-audio-mix-tests COMMAND tilefinch-game-audio-mix-tests)
    set_tests_properties(tilefinch-game-audio-mix-tests PROPERTIES
        LABELS "tilefinch;unit;audio;performance" TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-validation-phase-tests
        tests/test_validation_phase.c)
    target_include_directories(tilefinch-validation-phase-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME tilefinch-validation-phase-tests COMMAND tilefinch-validation-phase-tests)
    set_tests_properties(tilefinch-validation-phase-tests PROPERTIES
        LABELS "tilefinch;unit;performance" TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-voice-frontend-tests
        tests/test_voice_frontend.c)
    target_link_libraries(
        tilefinch-voice-frontend-tests PRIVATE tilefinch_voice_frontend)
    add_test(NAME tilefinch-voice-frontend-tests
        COMMAND tilefinch-voice-frontend-tests)
    set_tests_properties(tilefinch-voice-frontend-tests PROPERTIES
        LABELS "tilefinch;unit;psp;audio"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-psp-basic-fallback-tests
        tests/test_psp_basic_fallback.c)
    target_link_libraries(tilefinch-psp-basic-fallback-tests
        PRIVATE tilefinch_psp_ui)
    add_test(NAME tilefinch-psp-basic-fallback-tests
        COMMAND tilefinch-psp-basic-fallback-tests)
    set_tests_properties(tilefinch-psp-basic-fallback-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-psp-reader-policy-tests
        tests/test_psp_reader_policy.c)
    target_link_libraries(tilefinch-psp-reader-policy-tests
        PRIVATE tilefinch_psp_ui)
    add_test(NAME tilefinch-psp-reader-policy-tests
        COMMAND tilefinch-psp-reader-policy-tests)
    set_tests_properties(tilefinch-psp-reader-policy-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-psp-ui-tests tests/test_psp_ui.c)
    target_link_libraries(tilefinch-psp-ui-tests PRIVATE tilefinch_psp_ui)
    # TILEFINCH_TEST_SANS_FONT is the full upstream DejaVuSans.ttf. The two
    # PSP_ paths are the faces actually staged beside the browser EBOOT, and
    # the chrome glyph-coverage test must use those: a covering test that read
    # the full face once certified U+25BA, which no device has ever carried.
    target_compile_definitions(tilefinch-psp-ui-tests PRIVATE
        TILEFINCH_TEST_SANS_FONT="${PSP_BROWSER_SANS_FONT}"
        TILEFINCH_TEST_PSP_SANS_FONT="${PSP_BROWSER_PSP_SANS_FONT}"
        TILEFINCH_TEST_PSP_SANS_BOLD_FONT="${PSP_BROWSER_SANS_BOLD_FONT}")
    add_test(NAME tilefinch-psp-ui-tests COMMAND tilefinch-psp-ui-tests)
    set_tests_properties(tilefinch-psp-ui-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-psp-media-presentation-tests
        tests/test_psp_media_presentation.c)
    target_link_libraries(tilefinch-psp-media-presentation-tests PRIVATE
        tilefinch_psp_ui)
    add_test(NAME tilefinch-psp-media-presentation-tests
        COMMAND tilefinch-psp-media-presentation-tests)
    set_tests_properties(tilefinch-psp-media-presentation-tests PROPERTIES
        LABELS "tilefinch;unit;psp;media;ui;state"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-diagnostic-qr-tests
        tests/test_diagnostic_qr.c)
    target_link_libraries(tilefinch-diagnostic-qr-tests PRIVATE
        tilefinch_diagnostic_qr)
    add_test(NAME tilefinch-diagnostic-qr-tests
        COMMAND tilefinch-diagnostic-qr-tests)
    set_tests_properties(tilefinch-diagnostic-qr-tests PROPERTIES
        LABELS "tilefinch;unit;psp;diagnostics"
        TIMEOUT 10)
    if(NOT PSP)
        add_test(NAME tilefinch-diagnostic-qr-decoder-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_diagnostic_qr_decoder.py")
        set_tests_properties(tilefinch-diagnostic-qr-decoder-tests PROPERTIES
            LABELS "tilefinch;unit;psp;diagnostics"
            TIMEOUT 10)
    endif()

    tilefinch_add_test_binary(tilefinch-bootstrap-footprint-tests
        tests/test_bootstrap_footprint.c)
    target_link_libraries(tilefinch-bootstrap-footprint-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-bootstrap-footprint-tests
        COMMAND tilefinch-bootstrap-footprint-tests)
    set_tests_properties(tilefinch-bootstrap-footprint-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;budget"
        TIMEOUT 60)

    tilefinch_add_test_binary(tilefinch-psp-boot-config-tests
        tests/test_psp_boot_config.c)
    target_link_libraries(tilefinch-psp-boot-config-tests
        PRIVATE tilefinch_psp_app_support)
    add_test(NAME tilefinch-psp-boot-config-tests
        COMMAND tilefinch-psp-boot-config-tests)
    set_tests_properties(tilefinch-psp-boot-config-tests PROPERTIES
        LABELS "tilefinch;unit;psp;config"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-psp-boot-order-tests
        tests/test_psp_boot_order.c)
    target_link_libraries(tilefinch-psp-boot-order-tests
        PRIVATE tilefinch_psp_app_support)
    add_test(NAME tilefinch-psp-boot-order-tests
        COMMAND tilefinch-psp-boot-order-tests)
    set_tests_properties(tilefinch-psp-boot-order-tests PROPERTIES
        LABELS "tilefinch;unit;psp;config"
        TIMEOUT 10)

    # Scripted-input harness. The parser and stepper are host-neutral on
    # purpose: this replays the checked-in scenario through the same object
    # the validation EBOOT runs, so a renumbered menu row fails here in a
    # second rather than in an emulator run. Receiver coverage is the device
    # golden's job (scripts/run-ppsspp-input-script.sh).
    tilefinch_add_test_binary(tilefinch-psp-input-script-tests
        tests/test_psp_input_script.c
        src/psp_input_script.c)
    target_link_libraries(tilefinch-psp-input-script-tests
        PRIVATE tilefinch_psp_ui)
    target_compile_definitions(tilefinch-psp-input-script-tests PRIVATE
        TILEFINCH_INPUT_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/input-scripts")
    add_test(NAME tilefinch-psp-input-script-tests
        COMMAND tilefinch-psp-input-script-tests)
    set_tests_properties(tilefinch-psp-input-script-tests PROPERTIES
        LABELS "tilefinch;unit;psp;input"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-psp-profile-store-tests
        tests/test_psp_profile_store.c)
    target_link_libraries(tilefinch-psp-profile-store-tests
        PRIVATE tilefinch_psp_app_support)
    add_test(NAME tilefinch-psp-profile-store-tests
        COMMAND tilefinch-psp-profile-store-tests)
    set_tests_properties(tilefinch-psp-profile-store-tests PROPERTIES
        LABELS "tilefinch;unit;psp;profile"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-psp-lifecycle-tests
        tests/test_psp_lifecycle.c)
    target_link_libraries(tilefinch-psp-lifecycle-tests
        PRIVATE tilefinch_psp_app_support)
    add_test(NAME tilefinch-psp-lifecycle-tests
        COMMAND tilefinch-psp-lifecycle-tests)
    set_tests_properties(tilefinch-psp-lifecycle-tests PROPERTIES
        LABELS "tilefinch;unit;psp;lifecycle"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-psp-update-session-tests
        tests/test_psp_update_session.c)
    target_link_libraries(tilefinch-psp-update-session-tests
        PRIVATE tilefinch_psp_app_support)
    add_test(NAME tilefinch-psp-update-session-tests
        COMMAND tilefinch-psp-update-session-tests)
    set_tests_properties(tilefinch-psp-update-session-tests PROPERTIES
        LABELS "tilefinch;unit;psp;update"
        TIMEOUT 10)

    add_library(tilefinch_psp_power_test_ui STATIC
        src/psp_ui.c
        src/psp_ui_theme.c
        src/psp_ui_menu.c
        src/psp_power_policy.c)
    target_include_directories(tilefinch_psp_power_test_ui PUBLIC include)
    target_link_libraries(
        tilefinch_psp_power_test_ui PUBLIC tilefinch_core)
    target_compile_definitions(tilefinch_psp_power_test_ui PRIVATE
        TILEFINCH_PSP_POWER_TEST_MENU=1)
    tilefinch_add_test_binary(
        tilefinch-psp-power-menu-tests tests/test_psp_power_menu.c)
    target_link_libraries(
        tilefinch-psp-power-menu-tests
        PRIVATE tilefinch_psp_power_test_ui)
    add_test(
        NAME tilefinch-psp-power-menu-tests
        COMMAND tilefinch-psp-power-menu-tests)
    set_tests_properties(
        tilefinch-psp-power-menu-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui;power"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-psp-media-scale-tests
        tests/test_psp_media_scale.c)
    target_link_libraries(tilefinch-psp-media-scale-tests PRIVATE
        tilefinch_psp_media_scale)
    add_test(NAME tilefinch-psp-media-scale-tests
        COMMAND tilefinch-psp-media-scale-tests)
    set_tests_properties(tilefinch-psp-media-scale-tests PROPERTIES
        LABELS "tilefinch;unit;psp;media"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-psp-media-present-tests
        tests/test_psp_media_present.c)
    target_link_libraries(tilefinch-psp-media-present-tests PRIVATE
        tilefinch_psp_media_present)
    add_test(NAME tilefinch-psp-media-present-tests
        COMMAND tilefinch-psp-media-present-tests)
    set_tests_properties(tilefinch-psp-media-present-tests PROPERTIES
        LABELS "tilefinch;unit;psp;media"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-present-chrome-stability-tests
        tests/test_present_chrome_stability.c)
    target_link_libraries(tilefinch-present-chrome-stability-tests PRIVATE
        tilefinch_psp_display
        tilefinch_psp_media_present
        tilefinch_psp_ui)
    add_test(NAME tilefinch-present-chrome-stability-tests
        COMMAND tilefinch-present-chrome-stability-tests)
    set_tests_properties(tilefinch-present-chrome-stability-tests PROPERTIES
        LABELS "tilefinch;unit;psp;media"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-psp-display-tests tests/test_psp_display.c)
    target_link_libraries(tilefinch-psp-display-tests PRIVATE
        tilefinch_psp_display)
    add_test(NAME tilefinch-psp-display-tests
        COMMAND tilefinch-psp-display-tests)
    set_tests_properties(tilefinch-psp-display-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui"
        TIMEOUT 10)

    tilefinch_add_test_binary(tilefinch-dynamic-script-async-tests
        tests/test_dynamic_script_async.c)
    target_link_libraries(tilefinch-dynamic-script-async-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-dynamic-script-async-tests
        COMMAND tilefinch-dynamic-script-async-tests)
    set_tests_properties(tilefinch-dynamic-script-async-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;network;async"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-image-decode-scaled-tests
        tests/test_image_decode_scaled.c)
    target_link_libraries(tilefinch-image-decode-scaled-tests PRIVATE
        tilefinch_core)
    target_include_directories(tilefinch-image-decode-scaled-tests PRIVATE
        "${stb_SOURCE_DIR}")
    add_test(NAME tilefinch-image-decode-scaled-tests
        COMMAND tilefinch-image-decode-scaled-tests)
    set_tests_properties(tilefinch-image-decode-scaled-tests PROPERTIES
        LABELS "tilefinch;unit;image;memory"
        TIMEOUT 60)

    tilefinch_add_test_binary(tilefinch-text-encoding-tests
        tests/test_text_encoding.c)
    target_link_libraries(tilefinch-text-encoding-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-text-encoding-tests
        COMMAND tilefinch-text-encoding-tests)
    set_tests_properties(tilefinch-text-encoding-tests PROPERTIES
        LABELS "tilefinch;unit" TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-js-responsiveness-tests
        tests/test_js_responsiveness.c)
    target_link_libraries(tilefinch-js-responsiveness-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-js-responsiveness-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME tilefinch-js-responsiveness-tests
        COMMAND tilefinch-js-responsiveness-tests)
    set_tests_properties(tilefinch-js-responsiveness-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;responsiveness"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-game-audio-slots-tests
        tests/test_game_audio_slots.c)
    target_link_libraries(tilefinch-game-audio-slots-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-game-audio-slots-tests
        COMMAND tilefinch-game-audio-slots-tests)
    set_tests_properties(tilefinch-game-audio-slots-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;audio"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-game-audio-slots-fault-tests
        tests/test_game_audio_slots_faults.c)
    target_link_libraries(tilefinch-game-audio-slots-fault-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-game-audio-slots-fault-tests
        COMMAND tilefinch-game-audio-slots-fault-tests)
    set_tests_properties(tilefinch-game-audio-slots-fault-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;audio"
        TIMEOUT 120)

    tilefinch_add_test_binary(tilefinch-game-audio-selection-tests
        tests/test_game_audio_selection.c)
    target_link_libraries(tilefinch-game-audio-selection-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-game-audio-selection-tests
        COMMAND tilefinch-game-audio-selection-tests)
    set_tests_properties(tilefinch-game-audio-selection-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;audio"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-web-conformance-tests
        tests/test_web_conformance.c)
    target_link_libraries(tilefinch-web-conformance-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-web-conformance-tests
        COMMAND tilefinch-web-conformance-tests)
    set_tests_properties(tilefinch-web-conformance-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;conformance"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-module-bytecode-tests
        tests/test_module_bytecode.c)
    target_link_libraries(tilefinch-module-bytecode-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-module-bytecode-tests
        COMMAND tilefinch-module-bytecode-tests)
    set_tests_properties(tilefinch-module-bytecode-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;memory"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-classic-bytecode-tests
        tests/test_classic_bytecode.c)
    target_link_libraries(tilefinch-classic-bytecode-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-classic-bytecode-tests
        COMMAND tilefinch-classic-bytecode-tests)
    set_tests_properties(tilefinch-classic-bytecode-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;memory"
        TIMEOUT 30)

    # The persistent compiled-script tier, in host directories under the
    # build tree only (the tier refuses device paths on host builds).
    tilefinch_add_test_binary(tilefinch-script-disk-cache-tests
        tests/test_script_disk_cache.c)
    target_link_libraries(tilefinch-script-disk-cache-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-script-disk-cache-tests
        COMMAND tilefinch-script-disk-cache-tests)
    set_tests_properties(tilefinch-script-disk-cache-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;storage"
        ENVIRONMENT "TILEFINCH_TEST_SCRATCH_DIR=${CMAKE_BINARY_DIR}/test-scratch"
        TIMEOUT 60)

    # Lazy webpack bundle records (RAM and the persistent tier, in host
    # directories under the build tree only).
    tilefinch_add_test_binary(tilefinch-lazy-bundle-record-tests
        tests/test_lazy_bundle_records.c)
    target_link_libraries(tilefinch-lazy-bundle-record-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-lazy-bundle-record-tests
        COMMAND tilefinch-lazy-bundle-record-tests)
    set_tests_properties(tilefinch-lazy-bundle-record-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;storage"
        ENVIRONMENT "TILEFINCH_TEST_SCRATCH_DIR=${CMAKE_BINARY_DIR}/test-scratch"
        TIMEOUT 60)

    tilefinch_add_test_binary(tilefinch-canvas-webgl-conformance-tests
        tests/test_canvas_webgl_conformance.c src/psp_offline_store.c)
    target_link_libraries(tilefinch-canvas-webgl-conformance-tests
        PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-canvas-webgl-conformance-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-canvas-webgl-conformance-tests
        COMMAND tilefinch-canvas-webgl-conformance-tests)
    set_tests_properties(tilefinch-canvas-webgl-conformance-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;canvas;webgl;game"
        TIMEOUT 90)
    add_test(NAME tilefinch-treadline-feature-budget-tests
        COMMAND tilefinch-canvas-webgl-conformance-tests --treadline-features)
    set_tests_properties(tilefinch-treadline-feature-budget-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;game;memory"
        TIMEOUT 90)
    # Aim guide: exact cast vs real shells across seeded arenas, cache,
    # concealment, settings, instance admission, tracers, determinism.
    add_test(NAME tilefinch-treadline-aim-guide-tests
        COMMAND tilefinch-canvas-webgl-conformance-tests --treadline-aim-guide)
    set_tests_properties(tilefinch-treadline-aim-guide-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;game"
        TIMEOUT 120)
    add_test(NAME tilefinch-treadline-host-proxy-tests
        COMMAND tilefinch-canvas-webgl-conformance-tests --treadline-host-profile)
    set_tests_properties(tilefinch-treadline-host-proxy-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;game;performance"
        TIMEOUT 30)
    # The loopback lab runner both Treadline gates below share, against a
    # stand-in lab (no build needed); skips (77) without loopback sockets.
    add_test(NAME tilefinch-treadline-lab-runner-tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_treadline_lab_runner.py)
    set_tests_properties(tilefinch-treadline-lab-runner-tests PROPERTIES
        LABELS "tilefinch;unit;game;tooling"
        SKIP_RETURN_CODE 77
        TIMEOUT 30)
    # Real 480x272 layout of every Treadline menu screen in the lab: no
    # clipped labels, no panel scrolling, focus visible. Serves over
    # loopback and skips (77) where loopback sockets are forbidden.
    add_test(NAME tilefinch-treadline-menu-layout-tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-treadline-menu-layout.py
            --lab $<TARGET_FILE:psp-browser-interactive-lab>)
    set_tests_properties(tilefinch-treadline-menu-layout-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;game;layout"
        SKIP_RETURN_CODE 77
        TIMEOUT 120)
    # Approved reference screenshots of Treadline's key scenes (title,
    # menus, Controls pages, Practice Range, pause, HUD in play, each
    # authored arena's opening view), pixel for pixel in the lab. A failure
    # writes the render and a diff image to treadline-references/ and names
    # the approve command (--update-references) for intended changes.
    add_test(NAME tilefinch-treadline-reference-tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-treadline-references.py
            --lab $<TARGET_FILE:psp-browser-interactive-lab>
            --out ${CMAKE_CURRENT_BINARY_DIR}/treadline-references)
    set_tests_properties(tilefinch-treadline-reference-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;game;visual"
        SKIP_RETURN_CODE 77
        TIMEOUT 180)
    # The milestone PPSSPP visual review (scripts/visual-review.py) needs the
    # emulator and a validation PSP build, so it is a build target, never a
    # test: cmake --build build-preset-release --target treadline-visual-review
    if(TARGET tilefinch-video-blink AND TARGET tilefinch-offline-library-fixture)
    set(TILEFINCH_VISUAL_REVIEW_PSP_BUILD
        "${CMAKE_CURRENT_SOURCE_DIR}/build-preset-psp-validation" CACHE PATH
        "Validation PSP build (EBOOT.PBP) recorded by the treadline-visual-review target")
    add_custom_target(treadline-visual-review
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/visual-review.py
            --build-dir ${TILEFINCH_VISUAL_REVIEW_PSP_BUILD}
            --tools ${CMAKE_CURRENT_BINARY_DIR}
            --lab $<TARGET_FILE:psp-browser-interactive-lab>
            --out ${CMAKE_CURRENT_BINARY_DIR}/treadline-visual-review
        DEPENDS tilefinch-offline-library-fixture tilefinch-video-motion
            tilefinch-video-blink psp-browser-interactive-lab
        USES_TERMINAL
        COMMENT "Recording Treadline's standard scenarios in PPSSPP (about 10 minutes)")
    endif()

    add_test(NAME tilefinch-usability-probe-read-only
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/usability-probe-inert.html
            --fetch-scripts --ticks 2 --probe-usability --no-loop-capture)
    set_tests_properties(tilefinch-usability-probe-read-only PROPERTIES
        PASS_REGULAR_EXPRESSION
            "usability-probe focus=ready.*activation=not-run.*handlers=0/0 network=0/0"
        FAIL_REGULAR_EXPRESSION "CLICKED_BY_PROBE;activations=[1-9]"
        LABELS "tilefinch;unit;acceptance;tooling;javascript"
        TIMEOUT 10)

    add_test(NAME tilefinch-blank-reader-recurring-recovery
        COMMAND psp-browser-interactive-lab
            --fixture
                ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reader-recovery-recurring.html
            --fetch-scripts --ticks 16 --tick-ms 20 --pace-real-time
            --no-loop-capture)
    set_tests_properties(tilefinch-blank-reader-recurring-recovery PROPERTIES
        PASS_REGULAR_EXPRESSION
            "(basic|reader)-recovery available=yes.*applied=yes activated=yes"
        LABELS "tilefinch;unit;acceptance;tooling;javascript;reader;lifecycle"
        TIMEOUT 10)

    add_test(NAME tilefinch-actionable-page-qualification
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run-actionable-page-qualification.sh
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/actionable-page-qualification)
    set_tests_properties(tilefinch-actionable-page-qualification PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        LABELS "tilefinch;unit;acceptance;navigation;javascript;usability"
        TIMEOUT 15)

    if(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE)
        add_test(NAME tilefinch-property-miss-trace-tests
            COMMAND psp-browser-interactive-lab
                --fixture
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/property_miss_trace.html
                --no-external-resources
                --ticks 1
                --no-loop-capture)
        set_tests_properties(tilefinch-property-miss-trace-tests PROPERTIES
            ENVIRONMENT "TILEFINCH_TRACE_JS_PROPERTY_FAULTS=16"
            PASS_REGULAR_EXPRESSION
                "quickjs-property-miss.*base=object.*property=\\\"optionalStandardFeature\\\""
            LABELS "tilefinch;unit;javascript;diagnostic"
            TIMEOUT 10)

        add_test(NAME tilefinch-property-absent-trace-tests
            COMMAND psp-browser-interactive-lab
                --fixture
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/property_miss_trace.html
                --no-external-resources
                --ticks 1
                --no-loop-capture)
        set_tests_properties(tilefinch-property-absent-trace-tests PROPERTIES
            ENVIRONMENT "TILEFINCH_TRACE_JS_PROPERTY_FAULTS=absent"
            PASS_REGULAR_EXPRESSION
                "quickjs-property-absent.*base=object.*property=\\\"<string:12:missingViaIn>\\\""
            LABELS "tilefinch;unit;javascript;diagnostic"
            TIMEOUT 10)

        add_test(NAME tilefinch-property-fault-trace-tests
            COMMAND psp-browser-interactive-lab
                --fixture
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/property_fault_trace.html
                --no-external-resources
                --ticks 1
                --no-loop-capture)
        set_tests_properties(tilefinch-property-fault-trace-tests PROPERTIES
            ENVIRONMENT "TILEFINCH_TRACE_JS_PROPERTY_FAULTS=16"
            PASS_REGULAR_EXPRESSION
                "quickjs-property-fault.*base=undefined.*property=\\\"<string:12:computed-key>\\\""
            LABELS "tilefinch;unit;javascript;diagnostic"
            TIMEOUT 10)

        add_test(NAME tilefinch-property-fault-only-trace-tests
            COMMAND psp-browser-interactive-lab
                --fixture
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/property_fault_trace.html
                --no-external-resources
                --ticks 1
                --no-loop-capture)
        set_tests_properties(tilefinch-property-fault-only-trace-tests PROPERTIES
            ENVIRONMENT "TILEFINCH_TRACE_JS_PROPERTY_FAULTS=fault"
            PASS_REGULAR_EXPRESSION
                "quickjs-property-fault.*base=undefined.*property=\\\"<string:12:computed-key>\\\""
            FAIL_REGULAR_EXPRESSION "quickjs-property-miss"
            LABELS "tilefinch;unit;javascript;diagnostic"
            TIMEOUT 10)

        add_test(NAME tilefinch-property-write-fault-trace-tests
            COMMAND psp-browser-interactive-lab
                --fixture
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/property_fault_trace.html
                --no-external-resources
                --ticks 1
                --no-loop-capture)
        set_tests_properties(tilefinch-property-write-fault-trace-tests PROPERTIES
            ENVIRONMENT "TILEFINCH_TRACE_JS_PROPERTY_FAULTS=16"
            PASS_REGULAR_EXPRESSION
                "quickjs-property-fault.*base=undefined.*property=\\\"<string:12:write-target>\\\""
            LABELS "tilefinch;unit;javascript;diagnostic"
            TIMEOUT 10)
    endif()

    tilefinch_add_test_binary(tilefinch-script-admission-tests
        tests/test_script_admission.c)
    target_link_libraries(tilefinch-script-admission-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-script-admission-tests
        COMMAND tilefinch-script-admission-tests)
    set_tests_properties(tilefinch-script-admission-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;memory"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-script-lazy-tests tests/test_script_lazy.c)
    target_link_libraries(tilefinch-script-lazy-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-script-lazy-tests COMMAND tilefinch-script-lazy-tests)
    set_tests_properties(tilefinch-script-lazy-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;memory"
        TIMEOUT 30)

    if(PSP_BROWSER_USE_BELLARD_QUICKJS)
        tilefinch_add_test_binary(tilefinch-quickjs-oom-tests
            tests/test_quickjs_oom.c)
        target_link_libraries(tilefinch-quickjs-oom-tests PRIVATE tilefinch_core)
        add_test(NAME tilefinch-quickjs-oom-tests
            COMMAND tilefinch-quickjs-oom-tests)
        set_tests_properties(tilefinch-quickjs-oom-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;allocator;sanitizer"
            TIMEOUT 30)

        tilefinch_add_test_binary(tilefinch-quickjs-property-creation-tests
            tests/test_quickjs_property_creation.c)
        target_link_libraries(tilefinch-quickjs-property-creation-tests
            PRIVATE tilefinch_core)
        add_test(NAME tilefinch-quickjs-property-creation-tests
            COMMAND tilefinch-quickjs-property-creation-tests)
        set_tests_properties(tilefinch-quickjs-property-creation-tests
            PROPERTIES
            LABELS "tilefinch;unit;javascript;memory;sanitizer"
            TIMEOUT 60)

        tilefinch_add_test_binary(tilefinch-quickjs-native-call-tests
            tests/test_quickjs_native_calls.c)
        target_link_libraries(tilefinch-quickjs-native-call-tests
            PRIVATE tilefinch_core)
        add_test(NAME tilefinch-quickjs-native-call-tests
            COMMAND tilefinch-quickjs-native-call-tests)
        set_tests_properties(tilefinch-quickjs-native-call-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;sanitizer"
            TIMEOUT 60)

        tilefinch_add_test_binary(tilefinch-quickjs-lazy-function-tests
            tests/test_quickjs_lazy_functions.c)
        target_link_libraries(tilefinch-quickjs-lazy-function-tests
            PRIVATE tilefinch_core)
        # One iteration per kernel: proves every kernel compiles and runs
        # on this engine build, so a PSP bench run cannot fail on a kernel
        # the host never exercised.
        add_test(NAME tilefinch-js-bench-smoke-tests
            COMMAND tilefinch-js-bench --scale 0 --repeat 1)
        set_tests_properties(tilefinch-js-bench-smoke-tests PROPERTIES
            LABELS "tilefinch;unit;javascript" TIMEOUT 60)
        add_test(NAME tilefinch-js-bench-selection-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_js_bench_selection.py
                $<TARGET_FILE:tilefinch-js-bench>)
        set_tests_properties(tilefinch-js-bench-selection-tests PROPERTIES
            LABELS "tilefinch;unit;javascript" TIMEOUT 30)
        add_test(NAME tilefinch-quickjs-lazy-function-tests
            COMMAND tilefinch-quickjs-lazy-function-tests)
        set_tests_properties(tilefinch-quickjs-lazy-function-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;memory;sanitizer"
            TIMEOUT 60)
    endif()

    tilefinch_add_test_binary(tilefinch-stylesheet-resource-tests
        tests/test_stylesheet_resources.c)
    target_link_libraries(tilefinch-stylesheet-resource-tests
        PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-stylesheet-resource-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-stylesheet-resource-tests
        COMMAND tilefinch-stylesheet-resource-tests)
    set_tests_properties(tilefinch-stylesheet-resource-tests PROPERTIES
        LABELS "tilefinch;unit;network;style"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-web-font-tests tests/test_web_fonts.c)
    target_link_libraries(tilefinch-web-font-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-web-font-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(PSP_BROWSER_FREETYPE_AVAILABLE)
        target_compile_definitions(tilefinch-web-font-tests PRIVATE
            TILEFINCH_TEST_HAVE_FREETYPE=1)
    endif()
    add_test(NAME tilefinch-web-font-tests COMMAND tilefinch-web-font-tests)
    set_tests_properties(tilefinch-web-font-tests PROPERTIES
        LABELS "tilefinch;unit;network;style;font;security"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-budget-concurrent-tests
        tests/test_budget_concurrent.c)
    target_link_libraries(tilefinch-budget-concurrent-tests
        PRIVATE tilefinch_core Threads::Threads)
    add_test(NAME tilefinch-budget-concurrent-tests
        COMMAND tilefinch-budget-concurrent-tests)
    set_tests_properties(tilefinch-budget-concurrent-tests PROPERTIES
        LABELS "tilefinch;unit;allocator;concurrency;sanitizer"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-fetch-background-ownership-tests
        tests/test_fetch_background_ownership.c)
    target_include_directories(tilefinch-fetch-background-ownership-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(tilefinch-fetch-background-ownership-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-fetch-background-ownership-tests
        COMMAND tilefinch-fetch-background-ownership-tests)
    set_tests_properties(tilefinch-fetch-background-ownership-tests PROPERTIES
        LABELS "tilefinch;unit;network;media;psp;concurrency;sanitizer"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-psp-media-ownership-tests
        tests/test_psp_media_ownership.c)
    target_include_directories(tilefinch-psp-media-ownership-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tilefinch-psp-media-ownership-tests
        PRIVATE Threads::Threads)
    add_test(NAME tilefinch-psp-media-ownership-tests
        COMMAND tilefinch-psp-media-ownership-tests)
    # Two threads hand 100,000 frames back and forth, waiting with
    # sched_yield(). The run is 0.2 s of CPU; its wall time is the number of
    # hand-offs times scheduling latency, which a loaded host stretches (on
    # macOS a yield depresses the caller's priority for a quantum). Observed:
    # 2-19 s for one instance at load averages of 80-175, 58-94 s for eight
    # at once, and a 30 s timeout in a -j8 suite run at load 170.
    set_tests_properties(tilefinch-psp-media-ownership-tests PROPERTIES
        LABELS "tilefinch;unit;media;psp;concurrency;sanitizer"
        TIMEOUT 300)

    # The same stress under ThreadSanitizer. The plain binary can only catch
    # a race that happens to corrupt a picture; this one reports any slot
    # field the two threads touch without the state word ordering them,
    # whether or not the bytes it read were wrong -- which is how the take's
    # epoch read of a writer-owned FREE slot was found. Host-only, and only
    # where the compiler links -fsanitize=thread and no other sanitizer is in
    # the global flags (TSan cannot share a binary with ASan, so the sanitize
    # preset keeps its ASan/UBSan copy above and skips this one). Under TSan
    # the 100,000 hand-offs take well under a second unloaded.
    if(NOT PSP AND NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
        include(CheckCSourceCompiles)
        set(_tilefinch_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
        set(_tilefinch_saved_required_link "${CMAKE_REQUIRED_LINK_OPTIONS}")
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=thread")
        set(CMAKE_REQUIRED_LINK_OPTIONS
            ${CMAKE_REQUIRED_LINK_OPTIONS} -fsanitize=thread)
        check_c_source_compiles(
            "#include <pthread.h>
             static void *run(void *p) { return p; }
             int main(void) { pthread_t t; pthread_create(&t, 0, run, 0);
                              return pthread_join(t, 0); }"
            TILEFINCH_HOST_HAS_TSAN)
        set(CMAKE_REQUIRED_FLAGS "${_tilefinch_saved_required_flags}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "${_tilefinch_saved_required_link}")
        unset(_tilefinch_saved_required_flags)
        unset(_tilefinch_saved_required_link)
    endif()
    if(TILEFINCH_HOST_HAS_TSAN AND NOT PSP
       AND NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
        tilefinch_add_test_binary(tilefinch-psp-media-ownership-tsan-tests
            tests/test_psp_media_ownership.c)
        target_include_directories(tilefinch-psp-media-ownership-tsan-tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(tilefinch-psp-media-ownership-tsan-tests
            PRIVATE -fsanitize=thread -g -fno-omit-frame-pointer)
        target_link_options(tilefinch-psp-media-ownership-tsan-tests
            PRIVATE -fsanitize=thread)
        target_link_libraries(tilefinch-psp-media-ownership-tsan-tests
            PRIVATE Threads::Threads)
        add_test(NAME tilefinch-psp-media-ownership-tsan-tests
            COMMAND tilefinch-psp-media-ownership-tsan-tests)
        # halt_on_error turns the first report into a failing exit rather
        # than a warning after "all checks passed".
        set_tests_properties(tilefinch-psp-media-ownership-tsan-tests
            PROPERTIES
            LABELS "tilefinch;unit;media;psp;concurrency;sanitizer;tsan"
            ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:exitcode=66"
            TIMEOUT 300)
    endif()

    tilefinch_add_test_binary(tilefinch-psp-media-state-tests
        tests/test_psp_media_state.c)
    target_link_libraries(tilefinch-psp-media-state-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-psp-media-state-tests
        COMMAND tilefinch-psp-media-state-tests)
    set_tests_properties(tilefinch-psp-media-state-tests PROPERTIES
        LABELS "tilefinch;unit;media;psp;state"
        TIMEOUT 30)

    # Rule 2 as a gate: the unlocked Budget ledger aborts host tests when a
    # thread other than its owner mutates it. This binary provokes the
    # violation deliberately, with the abort disabled.
    tilefinch_add_test_binary(tilefinch-budget-owner-tests
        tests/test_budget_owner.c)
    # The test creates threads itself, independent of how the core links.
    target_link_libraries(tilefinch-budget-owner-tests
        PRIVATE tilefinch_core Threads::Threads)
    add_test(NAME tilefinch-budget-owner-tests
        COMMAND tilefinch-budget-owner-tests)
    set_tests_properties(tilefinch-budget-owner-tests PROPERTIES
        LABELS "tilefinch;unit;budget"
        TIMEOUT 30)

    # The resident loop's optional pumps are admitted through one declared
    # policy table. The test proves table order, each admission rule, and the
    # exact set of pump pairs able to contend for one resource in a frame.
    tilefinch_add_test_binary(tilefinch-frame-pumps-tests
        tests/test_frame_pumps.c)
    target_link_libraries(tilefinch-frame-pumps-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-frame-pumps-tests
        COMMAND tilefinch-frame-pumps-tests)
    set_tests_properties(tilefinch-frame-pumps-tests PROPERTIES
        LABELS "tilefinch;unit;psp;state"
        TIMEOUT 60)

    # The work ledger behind tilefinch-loop-work: exclusive nested kinds,
    # the counted thread only, nothing while disabled.
    tilefinch_add_test_binary(tilefinch-work-ledger-tests
        tests/test_work_ledger.c)
    target_link_libraries(tilefinch-work-ledger-tests
        PRIVATE tilefinch_core Threads::Threads)
    add_test(NAME tilefinch-work-ledger-tests
        COMMAND tilefinch-work-ledger-tests)
    set_tests_properties(tilefinch-work-ledger-tests PROPERTIES
        LABELS "tilefinch;unit;state"
        TIMEOUT 10)

    # The script split behind tilefinch-script-split and the checkpoint
    # record's split fields: time inside script only, exclusive nested
    # kinds, poll attribution, the counted thread only.
    tilefinch_add_test_binary(tilefinch-script-split-tests
        tests/test_script_split.c)
    target_link_libraries(tilefinch-script-split-tests
        PRIVATE tilefinch_core Threads::Threads)
    add_test(NAME tilefinch-script-split-tests
        COMMAND tilefinch-script-split-tests)
    set_tests_properties(tilefinch-script-split-tests PROPERTIES
        LABELS "tilefinch;unit;state"
        TIMEOUT 10)
    if(PSP_BROWSER_USE_BELLARD_QUICKJS)
        tilefinch_add_test_binary(tilefinch-script-census-tests
            tests/test_script_census.c)
        target_link_libraries(tilefinch-script-census-tests
            PRIVATE tilefinch_core Threads::Threads)
        add_test(NAME tilefinch-script-census-tests
            COMMAND tilefinch-script-census-tests)
        set_tests_properties(tilefinch-script-census-tests PROPERTIES
            LABELS "tilefinch;unit;state;performance" TIMEOUT 10)
    endif()

    # The background update check's state machine is pure data shared with
    # the PSP frontend; its eligibility and sign-in pause rules live here.
    tilefinch_add_test_binary(tilefinch-psp-update-check-tests
        tests/test_psp_update_check.c)
    target_link_libraries(tilefinch-psp-update-check-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-psp-update-check-tests
        COMMAND tilefinch-psp-update-check-tests)
    set_tests_properties(tilefinch-psp-update-check-tests PROPERTIES
        LABELS "tilefinch;unit;psp;state"
        TIMEOUT 10)

    # When a streaming load preview is (re)built and how deep: the pure
    # state machine behind navigation's pre-EOF previews.
    tilefinch_add_test_binary(tilefinch-preview-policy-tests
        tests/test_preview_policy.c)
    target_link_libraries(tilefinch-preview-policy-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-preview-policy-tests
        COMMAND tilefinch-preview-policy-tests)
    set_tests_properties(tilefinch-preview-policy-tests PROPERTIES
        LABELS "tilefinch;unit;state"
        TIMEOUT 10)

    # Where a button press goes while the PSP input supervisor owns input
    # (loading page, page service, media); pure routing behind the
    # supervisor tick.
    tilefinch_add_test_binary(tilefinch-psp-input-route-tests
        tests/test_psp_input_route.c)
    target_link_libraries(tilefinch-psp-input-route-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-psp-input-route-tests
        COMMAND tilefinch-psp-input-route-tests)
    set_tests_properties(tilefinch-psp-input-route-tests PROPERTIES
        LABELS "tilefinch;unit;psp;state"
        TIMEOUT 10)

    # The media session's presentation gate
    # (psp_media_presentation_gate.h): startup preroll, seek floor and
    # audio hold.
    tilefinch_add_test_binary(tilefinch-psp-media-presentation-gate-tests
        tests/test_psp_media_presentation_gate.c)
    target_link_libraries(tilefinch-psp-media-presentation-gate-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-psp-media-presentation-gate-tests
        COMMAND tilefinch-psp-media-presentation-gate-tests)
    set_tests_properties(tilefinch-psp-media-presentation-gate-tests
        PROPERTIES LABELS "tilefinch;unit;psp;state" TIMEOUT 10)

    # The media session's scrub and pending-target machines
    # (psp_media_scrub.h): what the user asked of the timeline across
    # pipeline rebuilds.
    tilefinch_add_test_binary(tilefinch-psp-media-scrub-tests
        tests/test_psp_media_scrub.c)
    target_link_libraries(tilefinch-psp-media-scrub-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-psp-media-scrub-tests
        COMMAND tilefinch-psp-media-scrub-tests)
    set_tests_properties(tilefinch-psp-media-scrub-tests PROPERTIES
        LABELS "tilefinch;unit;psp;state"
        TIMEOUT 10)

    # The PSP DNS stub's pure half (src/psp_dns_stub.h): query encoding,
    # reply validation, compression and CNAME bounds, retransmit schedule.
    tilefinch_add_test_binary(tilefinch-psp-dns-stub-tests
        tests/test_psp_dns_stub.c)
    add_test(NAME tilefinch-psp-dns-stub-tests
        COMMAND tilefinch-psp-dns-stub-tests)
    set_tests_properties(tilefinch-psp-dns-stub-tests PROPERTIES
        LABELS "tilefinch;unit;psp;network"
        TIMEOUT 10)

    # The PSP TLS entropy source's pure half (src/psp_entropy_pool.h):
    # predictor-based credit, pool framing and ratchet, seed-file format.
    tilefinch_add_test_binary(tilefinch-psp-entropy-pool-tests
        tests/test_psp_entropy_pool.c
        src/psp_entropy_pool.c
        src/sha256.c)
    target_include_directories(tilefinch-psp-entropy-pool-tests
        PRIVATE include)
    add_test(NAME tilefinch-psp-entropy-pool-tests
        COMMAND tilefinch-psp-entropy-pool-tests)
    set_tests_properties(tilefinch-psp-entropy-pool-tests PROPERTIES
        LABELS "tilefinch;unit;psp;network;security"
        TIMEOUT 10)

    # Load-time reach and scroll responsiveness, reported by validation
    # builds; pure data shared with the PSP frontend.
    tilefinch_add_test_binary(tilefinch-psp-load-experience-tests
        tests/test_psp_load_experience.c)
    target_link_libraries(tilefinch-psp-load-experience-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-psp-load-experience-tests
        COMMAND tilefinch-psp-load-experience-tests)
    set_tests_properties(tilefinch-psp-load-experience-tests PROPERTIES
        LABELS "tilefinch;unit;psp;state"
        TIMEOUT 10)

    # The emulator scenario that exercises the restore pumps boots from a
    # checked-in Memory Stick seed. Loading it here with the shipping readers
    # turns a format change into a host failure; inside PPSSPP a stale seed
    # would only look like a restore with nothing to do.
    tilefinch_add_test_binary(tilefinch-site-data-fixture
        tests/tools/site_data_fixture.c)
    target_link_libraries(tilefinch-site-data-fixture
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-site-data-fixture-tests
        COMMAND tilefinch-site-data-fixture --check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ppsspp-site-data)
    set_tests_properties(tilefinch-site-data-fixture-tests PROPERTIES
        LABELS "tilefinch;unit;psp;state"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-psp-network-supervisor-tests
        tests/test_psp_network_supervisor.c)
    target_link_libraries(tilefinch-psp-network-supervisor-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-psp-network-supervisor-tests
        COMMAND tilefinch-psp-network-supervisor-tests)
    set_tests_properties(tilefinch-psp-network-supervisor-tests PROPERTIES
        LABELS "tilefinch;unit;network;psp;state"
        TIMEOUT 30)

    tilefinch_add_test_binary(tilefinch-captive-portal-tests
        tests/test_captive_portal.c)
    target_link_libraries(tilefinch-captive-portal-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-captive-portal-tests
        COMMAND tilefinch-captive-portal-tests)
    set_tests_properties(tilefinch-captive-portal-tests PROPERTIES
        LABELS "tilefinch;unit;network;security"
        TIMEOUT 30)

    add_test(NAME tilefinch-memory-ledger-tests
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_memory_ledger.sh)
    set_tests_properties(tilefinch-memory-ledger-tests PROPERTIES
        LABELS "tilefinch;unit;allocator;acceptance"
        TIMEOUT 10)

    # Python is required above for host test registration.
    if(NOT PSP)
        add_test(NAME tilefinch-quickjs-pgo-training-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_quickjs_pgo_training.py)
        set_tests_properties(tilefinch-quickjs-pgo-training-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;tooling;release" TIMEOUT 10)
        if(PSP_BROWSER_USE_BELLARD_QUICKJS)
            add_test(NAME tilefinch-quickjs-variant-control-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_quickjs_variant_control.py)
            set_tests_properties(tilefinch-quickjs-variant-control-tests PROPERTIES
                LABELS "tilefinch;unit;javascript;tooling" TIMEOUT 10)
        endif()
        add_test(NAME tilefinch-execution-census-report-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_execution_census_report.py)
        set_tests_properties(tilefinch-execution-census-report-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;performance;tooling"
            TIMEOUT 10)
        add_test(NAME tilefinch-candidate-acceptance-runner-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_candidate_acceptance_runner.py)
        set_tests_properties(tilefinch-candidate-acceptance-runner-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling"
            TIMEOUT 10)
        add_test(NAME tilefinch-reference-frame-tools-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reference_frame_tools.py)
        set_tests_properties(tilefinch-reference-frame-tools-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling"
            TIMEOUT 10)
        add_test(NAME tilefinch-text-metrics-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_text_metrics.py
                $<TARGET_FILE:psp-browser-interactive-lab>)
        set_tests_properties(tilefinch-text-metrics-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;font;layout"
            TIMEOUT 20)
        if(PSP_BROWSER_USE_BELLARD_QUICKJS)
            add_test(NAME tilefinch-fast-array-growth-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fast_array_growth.py
                    $<TARGET_FILE:psp-browser-interactive-lab>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fast-array-growth.html)
            set_tests_properties(tilefinch-fast-array-growth-tests PROPERTIES
                LABELS "tilefinch;unit;javascript;allocator;performance"
                TIMEOUT 30)
        endif()
        add_test(NAME tilefinch-visual-scenario-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_visual_scenarios.py)
        # About 2 s alone; 10 s timed out at load 30.
        set_tests_properties(tilefinch-visual-scenario-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling"
            TIMEOUT 60)
        add_test(NAME tilefinch-search-form-visual-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_search_form_visual.py
                $<TARGET_FILE:psp-browser-lab>
                ${CMAKE_CURRENT_BINARY_DIR}/search-form-visual)
        set_tests_properties(tilefinch-search-form-visual-tests PROPERTIES
            LABELS "tilefinch;acceptance;visual;layout"
            TIMEOUT 30)
        add_test(NAME tilefinch-fidelity-scoreboard-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fidelity_scoreboard.py)
        set_tests_properties(tilefinch-fidelity-scoreboard-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;fidelity"
            TIMEOUT 10)
        # The fidelity oracle is defined on the optimized lab; unoptimized
        # builds diverge in replay-driven image scheduling (see
        # docs/FIDELITY.md) and would ratchet against the wrong binary.
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_test(NAME tilefinch-fidelity-floor-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run-fidelity-scoreboard.py
                --manifest ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/fidelity-scenarios.tsv
                --trace-root ${CMAKE_CURRENT_SOURCE_DIR}/fidelity/captures
                --reference-root ${CMAKE_CURRENT_SOURCE_DIR}/fidelity/references
                --work-dir ${CMAKE_CURRENT_SOURCE_DIR}/fidelity/floor-check
                --lab $<TARGET_FILE:psp-browser-lab>
                --jobs 4
                --check-floors ${CMAKE_CURRENT_SOURCE_DIR}/tests/fidelity-baselines.tsv)
        set_tests_properties(tilefinch-fidelity-floor-tests PROPERTIES
            LABELS "tilefinch;acceptance;tooling;fidelity"
            PROCESSORS 4
            SKIP_RETURN_CODE 77
            TIMEOUT 300)
        endif()
        add_test(NAME tilefinch-counter-baseline-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_counter_baselines.py
                $<TARGET_FILE:psp-browser-lab>)
        set_tests_properties(tilefinch-counter-baseline-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;layout;style;performance"
            TIMEOUT 60)
        # The counter baseline above is the HOST lab, and it deliberately
        # excludes byte totals. This is the device half: what one hermetic
        # PSP boot is allowed to cost, measured under PPSSPP against
        # tests/psp-device-cost-baseline.tsv.
        #
        # It boots an emulator, so it is opt-in rather than part of the
        # default lane; it is still registered unconditionally so `ctest -N`
        # names it either way, and it skips (77) rather than failing when
        # PPSSPP or the validation EBOOT is absent.
        #
        #   cmake --preset psp -B build-preset-psp-validation \
        #       -DTILEFINCH_PSP_VALIDATION_LOG=ON
        #   cmake --build build-preset-psp-validation --target psp-browser-script
        #   cmake --preset release -DTILEFINCH_DEVICE_COST_GATE=ON
        #   ctest --test-dir build-preset-release -L device-cost \
        #       --output-on-failure
        add_test(NAME tilefinch-device-cost-tests
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-ppsspp-device-cost.sh
                --build-dir ${TILEFINCH_DEVICE_COST_BUILD_DIR}
                --scenario start-page
                --runs 2
                --skip-when-missing)
        set_tests_properties(tilefinch-device-cost-tests PROPERTIES
            LABELS "tilefinch;acceptance;psp;device-cost;performance"
            SKIP_RETURN_CODE 77
            TIMEOUT 1200)
        if(NOT TILEFINCH_DEVICE_COST_GATE)
            set_tests_properties(tilefinch-device-cost-tests PROPERTIES
                DISABLED TRUE)
        endif()
        add_test(NAME tilefinch-trace-replay-server-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_trace_replay_server.py)
        set_tests_properties(tilefinch-trace-replay-server-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;network"
            TIMEOUT 10)
        add_test(NAME tilefinch-reference-capture-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reference_capture.py)
        set_tests_properties(tilefinch-reference-capture-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;network"
            TIMEOUT 10)
        add_test(NAME tilefinch-reference-capture-browser-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reference_capture_browser.py)
        set_tests_properties(tilefinch-reference-capture-browser-tests PROPERTIES
            LABELS "tilefinch;acceptance;tooling;network;visual"
            TIMEOUT 120)
        add_test(NAME tilefinch-native-capture-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_capture.py
                --lab $<TARGET_FILE:psp-browser-interactive-lab>)
        set_tests_properties(tilefinch-native-capture-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;visual"
            TIMEOUT 30)
        if(PSP_BROWSER_LIBCURL_TRANSPORT)
            add_test(NAME tilefinch-trace-acquisition-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_trace_acquisition.py
                    $<TARGET_FILE:psp-browser-trace-acquire>
                    $<TARGET_FILE:psp-browser-trace-inventory>)
            set_tests_properties(tilefinch-trace-acquisition-tests PROPERTIES
                LABELS "tilefinch;unit;acceptance;tooling;security"
                # This adversarial test creates executable wrappers and is
                # kept serial so their first execution on macOS does not queue
                # behind other process-heavy acceptance tests. That was once
                # blamed for this test's intermittent 120 s failures; the cause
                # was different (the tool passed its own standard input to a
                # helper which reads to end-of-file, so the test hung whenever
                # it was launched with a standard input that stays open) and is
                # fixed in the tool and pinned by the test.
                RUN_SERIAL TRUE
                TIMEOUT 150)
        endif()
    endif()

    # Non-test executables used by Python/interactive acceptance drivers.
    set(TILEFINCH_TEST_BINARY_TARGETS psp-browser-interactive-lab)
    if(TARGET tilefinch-offline-library-fixture)
        list(APPEND TILEFINCH_TEST_BINARY_TARGETS tilefinch-offline-library-fixture)
    endif()
    if(PSP_BROWSER_LIBCURL_TRANSPORT)
        list(APPEND TILEFINCH_TEST_BINARY_TARGETS
            psp-browser-trace-acquire psp-browser-trace-inventory)
    endif()

    if(PSP_BROWSER_BUILD_HOSTILE_PARSER_HARNESS)
        tilefinch_add_test_binary(tilefinch-hostile-parser-harness
            tests/test_hostile_parsers.c)
        list(APPEND TILEFINCH_TEST_BINARY_TARGETS
            tilefinch-hostile-parser-harness)
        target_link_libraries(tilefinch-hostile-parser-harness
            PRIVATE tilefinch_core)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(tilefinch-hostile-parser-harness PRIVATE
                -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
        endif()
        add_test(NAME tilefinch-hostile-parser-tests
            COMMAND tilefinch-hostile-parser-harness
                --seed 0x535441474531 --iterations 64)
        set_tests_properties(tilefinch-hostile-parser-tests PROPERTIES
            LABELS "tilefinch;fuzz;sanitizer"
            TIMEOUT 120)
    endif()

    if(PSP_BROWSER_LIBCURL_TRANSPORT)
        # Python is required above for host test registration.
        if(NOT PSP)
            tilefinch_add_test_binary(tilefinch-fetch-redirect-tests
                tests/test_fetch_redirect.c)
            list(APPEND TILEFINCH_TEST_BINARY_TARGETS
                tilefinch-fetch-redirect-tests)
            target_link_libraries(tilefinch-fetch-redirect-tests PRIVATE tilefinch_core)
            add_test(NAME tilefinch-fetch-redirect-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_fetch_redirect_test.py
                    $<TARGET_FILE:tilefinch-fetch-redirect-tests>)
            set_tests_properties(tilefinch-fetch-redirect-tests PROPERTIES
                LABELS "tilefinch;network;security"
                SKIP_RETURN_CODE 77
                TIMEOUT 20)

            # The media range source is the only FetchScheduler consumer that
            # issues, polls and installs its own responses, and the unit tests
            # substitute a synchronous transport that skips all of it. That gap
            # let a window install read its length out of an already-destroyed
            # response and cost a device cycle. This drives the real path
            # against a googlevideo-shaped loopback server.
            tilefinch_add_test_binary(tilefinch-media-http-range-tests
                tests/test_media_http_range.c)
            list(APPEND TILEFINCH_TEST_BINARY_TARGETS
                tilefinch-media-http-range-tests)
            target_link_libraries(tilefinch-media-http-range-tests
                PRIVATE tilefinch_core)
            add_test(NAME tilefinch-media-http-range-tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_media_http_range_test.py
                    $<TARGET_FILE:tilefinch-media-http-range-tests>)
            set_tests_properties(tilefinch-media-http-range-tests PROPERTIES
                LABELS "tilefinch;network;media"
                SKIP_RETURN_CODE 77
                TIMEOUT 60)
        endif()
    endif()

    add_test(NAME interactive-loop-tests
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run-interactive-acceptance.sh
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/interactive-acceptance)
    set_tests_properties(interactive-loop-tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        TIMEOUT 30)

    add_test(NAME tilefinch-private-interactive-tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_private_interactive_lab.py
            $<TARGET_FILE:psp-browser-interactive-lab>)
    set_tests_properties(tilefinch-private-interactive-tests PROPERTIES
        LABELS "tilefinch;lab"
        TIMEOUT 60)

    add_test(NAME selected-web-platform-tests
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run-web-platform-correctness.sh
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/selected-web-platform)
    set_tests_properties(selected-web-platform-tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        TIMEOUT 30)

    if(NOT PSP)
        add_test(NAME tilefinch-upstream-wpt-runner-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_upstream_wpt_runner.py)
        set_tests_properties(tilefinch-upstream-wpt-runner-tests PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            LABELS "tilefinch;unit;wpt"
            TIMEOUT 10)
    endif()

    add_test(NAME pressure-profile-tests
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run-pressure-qualification.sh
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/pressure-qualification)
    set_tests_properties(pressure-profile-tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        TIMEOUT 30)

    # Lab scenarios: the lab binaries driven by a fixture (and usually a
    # command file), judged by their output. Their PASS/FAIL expression lists
    # are spelled out per test because regex lists do not survive being
    # forwarded through a CMake function; the shared label is applied once,
    # below, to every test registered between these two snapshots.
    get_property(_tilefinch_tests_before_lab DIRECTORY PROPERTY TESTS)

    add_test(NAME reader-profile-default-none
        COMMAND psp-browser-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/demo.html
            --no-render --limit-mb 24)
    set_tests_properties(reader-profile-default-none PROPERTIES
        PASS_REGULAR_EXPRESSION "reader profile=none"
        TIMEOUT 30)

    add_test(NAME reader-profile-explicit-opt-in
        COMMAND psp-browser-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/demo.html
            --reader-profile wikipedia --no-render --limit-mb 24)
    set_tests_properties(reader-profile-explicit-opt-in PROPERTIES
        PASS_REGULAR_EXPRESSION "reader profile=wikipedia"
        TIMEOUT 30)

    add_test(NAME experimental-compressed-section-opt-in
        COMMAND psp-browser-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/demo.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 1 --no-render)
    set_tests_properties(experimental-compressed-section-opt-in PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-section-store.*sections="
        TIMEOUT 30)

    add_test(NAME experimental-section-pager-interactive
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/demo.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 1
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-pager-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-pager-final.ppm)
    set_tests_properties(experimental-section-pager-interactive PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-pager status=active.*swaps=5"
        TIMEOUT 30)

    add_test(NAME experimental-section-anchor-interactive
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anchor.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anchor-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-anchor-final.ppm)
    set_tests_properties(experimental-section-anchor-interactive PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-anchor id=\"alpha\" section=0"
        TIMEOUT 30)

    add_test(NAME experimental-section-link-activation
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anchor.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anchor-link-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-link-final.ppm)
    set_tests_properties(experimental-section-link-activation PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-anchor id=\"alpha\" section=0"
        TIMEOUT 30)

    add_test(NAME experimental-section-reload
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anchor.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 1
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-reload-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-reload-final.ppm)
    set_tests_properties(experimental-section-reload PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-reload section=2/3.*scroll="
        TIMEOUT 30)

    add_test(NAME experimental-section-history
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anchor.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-history-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-history-final.ppm)
    set_tests_properties(experimental-section-history PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-history direction=forward index=1 section=2"
        TIMEOUT 30)

    add_test(NAME experimental-section-focus
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anchor.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-focus-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-focus-final.ppm)
    set_tests_properties(experimental-section-focus PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-focus restored id=\"jump-gamma\" section=0"
        TIMEOUT 30)

    add_test(NAME experimental-section-state
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anchor.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-state-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-state-final.ppm)
    set_tests_properties(experimental-section-state PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-control id=\"section-input\" value=\"AB\""
        TIMEOUT 30)

    # The lab's realistic profile is the PSP app's own configuration
    # (BROWSER_PSP_APP_* shared with src/psp_boot_config.c).
    add_test(NAME lab-psp-realistic-profile-matches-app
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/demo.html
            --psp-profile realistic
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/lab-psp-realistic-profile.ppm)
    set_tests_properties(lab-psp-realistic-profile-matches-app PROPERTIES
        PASS_REGULAR_EXPRESSION "runtime-profile=realistic limit-mb=32 adaptive=yes history=4 session-cache=655360 tiles=24 navigation=transactional\nruntime-policy scripts=256 source-bytes=16777216 file-bytes=4194304 heap-bytes=5242880 stylesheets=24 css-bytes=2162688 css-file-bytes=786432"
        TIMEOUT 30)

    # A top-level DataDome 403 (sanitized from a news-site capture) must
    # raise the bot-protection notice rather than a silent blank page.
    add_test(NAME bot-wall-notice-datadome
        COMMAND psp-browser-interactive-lab
            --url https://www.nytimes.com/
            --replay-http-response-keyed
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/news-compat/bot-wall-datadome
            --psp-profile realistic --fetch-scripts
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/news-compat/bot-wall-datadome/commands
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/bot-wall-datadome-final.ppm)
    set_tests_properties(bot-wall-notice-datadome PROPERTIES
        PASS_REGULAR_EXPRESSION "bot-wall detected=yes site=\"nytimes.com\" vendor=\"DataDome\" status=403 blank-refused-script=no notice=\"nytimes.com blocked Tilefinch[|]Bot protection: this site may not work\""
        TIMEOUT 30)

    add_test(NAME experimental-section-image-cache
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-image.html
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-image-cap
            --deterministic-replay-seed 42
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 1
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-resource-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-image-final.ppm)
    set_tests_properties(experimental-section-image-cache PROPERTIES
        PASS_REGULAR_EXPRESSION "session .*cache-hits=[1-9]"
        TIMEOUT 30)

    add_test(NAME experimental-section-external-layout
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-external-layout.html
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-external-layout
            --deterministic-replay-seed 42
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-external-layout-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-external-layout-final.ppm)
    set_tests_properties(experimental-section-external-layout PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-layout-reindex status=applied selectors=1 sections=3->2 selected=0"
        TIMEOUT 30)

    add_test(NAME experimental-section-cross-document-history
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anchor.html
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-stream
            --deterministic-replay-seed 42
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-cross-document-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-cross-document-final.ppm)
    set_tests_properties(experimental-section-cross-document-history PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-resume section=2/3"
        TIMEOUT 30)

    add_test(NAME experimental-section-url-resume
        COMMAND psp-browser-interactive-lab
            --url https://section-history.test/a
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-cross-document
            --deterministic-replay-seed 42
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 1
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-cross-document-url-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-url-resume-final.ppm)
    set_tests_properties(experimental-section-url-resume PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-resume section=1/2.*chunks=1"
        TIMEOUT 30)

    add_test(NAME experimental-section-listener-state
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-listener.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-listener-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-listener-final.ppm)
    set_tests_properties(experimental-section-listener-state PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-control id=\"listener-result\" value=\"AB\""
        TIMEOUT 30)

    add_test(NAME experimental-section-mutation-state
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-mutation.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-mutation-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-mutation-final.ppm)
    set_tests_properties(experimental-section-mutation-state PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"mutation-target\" text=\"Changed\""
        TIMEOUT 30)

    add_test(NAME experimental-section-lazy-scripts
        COMMAND psp-browser-interactive-lab
            --url https://section-scripts.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-lazy-scripts
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-lazy-script-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-lazy-script-final.ppm)
    set_tests_properties(experimental-section-lazy-scripts PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"order-result\" text=\"head>alpha>beta>module:visibility=yes:relations=yes\""
        FAIL_REGULAR_EXPRESSION "text=\"102\";text=\"121\";text=\"201\";text=\"211\""
        TIMEOUT 30)

    add_test(NAME experimental-section-document-script-order-late-start
        COMMAND psp-browser-interactive-lab
            --url https://section-scripts.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-lazy-scripts
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 1
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-lazy-script-late-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-script-late-final.ppm)
    set_tests_properties(experimental-section-document-script-order-late-start
        PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"order-result\" text=\"head>alpha>beta>module:visibility=yes:relations=yes\""
        FAIL_REGULAR_EXPRESSION "loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME full-document-script-order-reference
        COMMAND psp-browser-interactive-lab
            --url https://section-scripts.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-lazy-scripts
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-lazy-script-reference-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-script-reference.ppm)
    set_tests_properties(full-document-script-order-reference PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"order-result\" text=\"head>alpha>beta>module:visibility=yes:relations=yes\""
        FAIL_REGULAR_EXPRESSION "loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-parser-raw-visibility
        COMMAND psp-browser-interactive-lab
            --url https://section-parser-raw.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-parser-raw
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-parser-raw-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-parser-raw.ppm)
    set_tests_properties(experimental-section-parser-raw-visibility PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"raw-parser-result\" text=\"RAW-PARSER-PASS\""
        FAIL_REGULAR_EXPRESSION "RAW-PARSER-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-parser-raw-visibility-late-start
        COMMAND psp-browser-interactive-lab
            --url https://section-parser-raw.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-parser-raw
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 1
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-parser-raw-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-parser-raw-late.ppm)
    set_tests_properties(experimental-section-parser-raw-visibility-late-start
        PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"raw-parser-result\" text=\"RAW-PARSER-PASS\""
        FAIL_REGULAR_EXPRESSION "RAW-PARSER-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME full-document-parser-raw-visibility-reference
        COMMAND psp-browser-interactive-lab
            --url https://section-parser-raw.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-parser-raw
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-parser-raw-reference-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-parser-raw-reference.ppm)
    set_tests_properties(full-document-parser-raw-visibility-reference PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"raw-parser-result\" text=\"RAW-PARSER-PASS\""
        FAIL_REGULAR_EXPRESSION "RAW-PARSER-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-raw-relations
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-raw-relations.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0 --ticks 2
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-raw-relation-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-raw-relations.ppm)
    set_tests_properties(experimental-section-raw-relations PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"raw-result\" text=\"RAW-PASS\""
        FAIL_REGULAR_EXPRESSION "RAW-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-raw-relations-late-start
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-raw-relations.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 1 --ticks 2
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-raw-relation-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-raw-relations-late.ppm)
    set_tests_properties(experimental-section-raw-relations-late-start
        PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"raw-result\" text=\"RAW-PASS\""
        FAIL_REGULAR_EXPRESSION "RAW-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME full-document-raw-relations-reference
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-raw-relations.html
            --psp-profile strict --ticks 2
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-raw-relation-reference-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-raw-relations-reference.ppm)
    set_tests_properties(full-document-raw-relations-reference PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"raw-result\" text=\"RAW-PASS\""
        FAIL_REGULAR_EXPRESSION "RAW-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-remote-geometry
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-remote-geometry.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0 --ticks 2
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-remote-geometry-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-remote-geometry.ppm)
    set_tests_properties(experimental-section-remote-geometry PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"geometry-result\" text=\"GEOMETRY-PASS\""
        FAIL_REGULAR_EXPRESSION "GEOMETRY-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-remote-geometry-late-start
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-remote-geometry.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 1 --ticks 2
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-remote-geometry-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-remote-geometry-late.ppm)
    set_tests_properties(experimental-section-remote-geometry-late-start
        PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"geometry-result\" text=\"GEOMETRY-PASS\""
        FAIL_REGULAR_EXPRESSION "GEOMETRY-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME full-document-remote-geometry-reference
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-remote-geometry.html
            --psp-profile strict --ticks 2
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-remote-geometry-reference-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-remote-geometry-reference.ppm)
    set_tests_properties(full-document-remote-geometry-reference PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"geometry-result\" text=\"GEOMETRY-PASS\""
        FAIL_REGULAR_EXPRESSION "GEOMETRY-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-document-script-quota
        COMMAND psp-browser-interactive-lab
            --url https://section-scripts.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-lazy-scripts
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts --script-count 2
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-script-quota-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-script-quota-final.ppm)
    set_tests_properties(experimental-section-document-script-quota PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"beta-result\" text=\"original-b\""
        FAIL_REGULAR_EXPRESSION "experimental-node id=\"beta-result\" text=\"111\""
        TIMEOUT 30)

    add_test(NAME experimental-section-remote-id
        COMMAND psp-browser-interactive-lab
            --url https://section-remote.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-remote-id
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-remote-id-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-remote-id-final.ppm)
    set_tests_properties(experimental-section-remote-id PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"remote-result\" text=\"REMOTE-PASS\""
        FAIL_REGULAR_EXPRESSION "REMOTE-FAIL;loop command-failed"
        TIMEOUT 30)

    add_test(NAME experimental-section-query-order
        COMMAND psp-browser-interactive-lab
            --url https://section-query-order.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-query-order
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections
            --experimental-section 2
            --ticks 2
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-query-order-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-query-order-final.ppm)
    set_tests_properties(experimental-section-query-order PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"query-order-result\" text=\"ORDER-PASS\""
        FAIL_REGULAR_EXPRESSION "ORDER-FAIL;loop command-failed"
        TIMEOUT 30)

    add_test(NAME full-document-query-order-reference
        COMMAND psp-browser-interactive-lab
            --url https://section-query-order.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-query-order
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --ticks 2
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-query-order-reference-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-query-order-reference.ppm)
    set_tests_properties(full-document-query-order-reference PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"query-order-result\" text=\"ORDER-PASS\""
        FAIL_REGULAR_EXPRESSION "ORDER-FAIL;loop command-failed"
        TIMEOUT 30)

    add_test(NAME experimental-section-wide-query
        COMMAND psp-browser-interactive-lab
            --url https://section-wide-query.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-wide-query
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-wide-query-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-wide-query.ppm)
    set_tests_properties(experimental-section-wide-query PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"wide-query-result\" text=\"WIDE-QUERY-PASS\""
        FAIL_REGULAR_EXPRESSION "WIDE-QUERY-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-wide-query-late-start
        COMMAND psp-browser-interactive-lab
            --url https://section-wide-query.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-wide-query
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 1
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-wide-query-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-wide-query-late.ppm)
    set_tests_properties(experimental-section-wide-query-late-start PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"wide-query-result\" text=\"WIDE-QUERY-PASS\""
        FAIL_REGULAR_EXPRESSION "WIDE-QUERY-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME full-document-wide-query-reference
        COMMAND psp-browser-interactive-lab
            --url https://section-wide-query.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-wide-query
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-wide-query-reference-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-wide-query-reference.ppm)
    set_tests_properties(full-document-wide-query-reference PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"wide-query-result\" text=\"WIDE-QUERY-PASS\""
        FAIL_REGULAR_EXPRESSION "WIDE-QUERY-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-root-mutation
        COMMAND psp-browser-interactive-lab
            --url https://section-root-mutation.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-root-mutation
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-root-mutation-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-root-mutation.ppm)
    set_tests_properties(experimental-section-root-mutation PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"root-result\" text=\"ROOT-MUTATION-PASS\";experimental-walk start=0 distance=0 end=0"
        FAIL_REGULAR_EXPRESSION "ROOT-MUTATION-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME full-document-root-mutation-reference
        COMMAND psp-browser-interactive-lab
            --url https://section-root-mutation.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-root-mutation
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-root-mutation-reference-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-root-mutation-reference.ppm)
    set_tests_properties(full-document-root-mutation-reference PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"root-result\" text=\"ROOT-MUTATION-PASS\""
        FAIL_REGULAR_EXPRESSION "ROOT-MUTATION-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-root-attributes
        COMMAND psp-browser-interactive-lab
            --url https://section-root-attributes.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-root-attributes
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-root-attributes-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-root-attributes.ppm)
    set_tests_properties(experimental-section-root-attributes PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node-attribute id=\"root-html\" name=\"data-root-state\" value=\"html-kept\";experimental-node-attribute id=\"root-head\" name=\"data-root-state\" value=\"head-kept\";experimental-node-attribute id=\"root-body\" name=\"data-root-state\" value=\"body-kept\""
        FAIL_REGULAR_EXPRESSION "loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME full-document-root-attributes-reference
        COMMAND psp-browser-interactive-lab
            --url https://section-root-attributes.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-root-attributes
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-root-attributes-reference-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-root-attributes-reference.ppm)
    set_tests_properties(full-document-root-attributes-reference PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node-attribute id=\"root-html\" name=\"data-root-state\" value=\"html-kept\";experimental-node-attribute id=\"root-head\" name=\"data-root-state\" value=\"head-kept\";experimental-node-attribute id=\"root-body\" name=\"data-root-state\" value=\"body-kept\""
        FAIL_REGULAR_EXPRESSION "loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-root-text
        COMMAND psp-browser-interactive-lab
            --url https://section-root-text.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-root-text
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-root-text-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-root-text.ppm)
    set_tests_properties(experimental-section-root-text PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"root-text-result\" text=\"ROOT-TEXT-PASS\""
        FAIL_REGULAR_EXPRESSION "ROOT-TEXT-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME full-document-root-text-reference
        COMMAND psp-browser-interactive-lab
            --url https://section-root-text.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-root-text
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-root-text-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-root-text-reference.ppm)
    set_tests_properties(full-document-root-text-reference PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"root-text-result\" text=\"ROOT-TEXT-PASS\""
        FAIL_REGULAR_EXPRESSION "ROOT-TEXT-FAIL;loop command-failed;interactive failure"
        TIMEOUT 30)

    add_test(NAME experimental-section-retention-caps
        COMMAND psp-browser-interactive-lab
            --url https://section-retention-cap.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-retention-cap
            --deterministic-replay-seed 42
            --psp-profile realistic --fetch-scripts
            --script-heap-mb 12 --script-total-mb 16
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-retention-cap-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-retention-cap-final.ppm)
    set_tests_properties(experimental-section-retention-caps PROPERTIES
        PASS_REGULAR_EXPRESSION "javascript-section-retention wrapper-evictions=[1-9][0-9]* listener-drops=[1-9][0-9]* handler-drops=[1-9][0-9]* observer-drops=[1-9][0-9]* record-drops=[1-9][0-9]* dirty-drops=[1-9][0-9]* state-evictions=[1-9][0-9]* control-drops=[1-9][0-9]*"
        FAIL_REGULAR_EXPRESSION "loop command-failed;status=FAIL"
        TIMEOUT 30)

    add_test(NAME experimental-section-anonymous-state
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anonymous-state.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-anonymous-state-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-anonymous-state-final.ppm)
    set_tests_properties(experimental-section-anonymous-state PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-focus restored key=\"s:0:.*experimental-control id=\"anonymous-result\" value=\"HLOHLO\""
        FAIL_REGULAR_EXPRESSION "value=\"X;loop command-failed"
        TIMEOUT 30)

    add_test(NAME experimental-section-property-handler-state
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-onhandler.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-onhandler-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-onhandler-final.ppm)
    set_tests_properties(experimental-section-property-handler-state PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-control id=\"handler-result\" value=\"AB\""
        TIMEOUT 30)

    add_test(NAME experimental-section-observer-selection-state
        COMMAND psp-browser-interactive-lab
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-observer-selection.html
            --psp-profile strict --experimental-compressed-sections
            --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-observer-selection-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-observer-selection-final.ppm)
    set_tests_properties(experimental-section-observer-selection-state PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-control id=\"observer-result\" value=\"A:1-3-forwardO\""
        TIMEOUT 30)

    add_test(NAME experimental-section-url-reload
        COMMAND psp-browser-interactive-lab
            --url https://section-scripts.test/page
            --replay-http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http-section-lazy-scripts
            --deterministic-replay-seed 42
            --psp-profile strict --fetch-scripts
            --experimental-compressed-sections --experimental-section 0
            --commands ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/section-url-reload-commands.txt
            --no-loop-capture
            --output ${CMAKE_CURRENT_BINARY_DIR}/section-url-reload-final.ppm)
    set_tests_properties(experimental-section-url-reload PROPERTIES
        PASS_REGULAR_EXPRESSION "experimental-node id=\"beta-result\" text=\"110\""
        FAIL_REGULAR_EXPRESSION "interactive failure"
        TIMEOUT 30)

    get_property(_tilefinch_lab_scenarios DIRECTORY PROPERTY TESTS)
    list(REMOVE_ITEM _tilefinch_lab_scenarios ${_tilefinch_tests_before_lab})
    set_tests_properties(${_tilefinch_lab_scenarios} PROPERTIES
        LABELS "tilefinch;lab;scenario")

    if(PSP_BROWSER_QUICKJS_NATIVE_TRACE)
        tilefinch_add_test_binary(native-trace-tests
            tests/test_native_trace.c
        )
        list(APPEND TILEFINCH_TEST_BINARY_TARGETS native-trace-tests)
        target_include_directories(native-trace-tests PRIVATE
            include
            "${quickjs_SOURCE_DIR}")
        target_link_libraries(native-trace-tests PRIVATE qjs)
        set_target_properties(native-trace-tests PROPERTIES C_EXTENSIONS ON)
        add_test(NAME native-trace-tests COMMAND native-trace-tests)
    endif()

    # Load-tolerant timeouts for CPU-bound tests. A TIMEOUT bounds a hang,
    # not speed, but the suite shares the host with other builds and test
    # runs: at load averages of 100-175 on a 10-core host, tests in a -j8
    # run took up to 6.4x their CPU time (10x for sub-second ones), and the
    # tightest of them timed out (ray queries: 3.3 s of CPU, 10 s limit).
    # Each test below therefore gets at least 20 times its CPU time,
    # rounded up to 10 s. A lower TIMEOUT at the registration site is
    # raised; a higher one stands. CPU is user+sys seconds of the whole
    # process tree in the release build, in tenths rounded up, the larger of
    # two runs of each enabled test's command under /usr/bin/time, one test
    # at a time; first measured 2026-10-06 at 9acecc62, re-measured at
    # e6e5c93e after the out-of-memory sweep in tilefinch-quickjs-oom-tests
    # (3.9 -> 6.6 s) and other later work. Tests already at 20x are absent.
    # Re-measure when a test's work grows.
    set(_tilefinch_test_cpu_tenths
        tilefinch-reference-capture-tests 61
        tilefinch-treadline-ray-query-tests 35
        tilefinch-game-video-analyzer-tests 248
        tilefinch-psp-game-stage-tests 21
        tilefinch-treadline-feature-budget-tests 168
        tilefinch-treadline-tiny-query-tests 19
        tilefinch-psp-sdk-quickjs-variant-tests 229
        tilefinch-quickjs-oom-tests 66
        tilefinch-diagnostic-qr-tests 12
        tilefinch-treadline-partial-route-tests 33
        tilefinch-js-responsiveness-tests 40
        tilefinch-private-test-infrastructure-tests 29
        tilefinch-candidate-acceptance-runner-tests 8
        tilefinch-treadline-aim-guide-tests 109
        tilefinch-browser-engine-tests 30
        tilefinch-navigation-load-tests 30
        tilefinch-stylesheet-resource-tests 21
        tilefinch-reference-frame-tools-tests 9
        tilefinch-psp-sdk-contract-tests 38)
    while(_tilefinch_test_cpu_tenths)
        list(POP_FRONT _tilefinch_test_cpu_tenths _test _tenths)
        if(NOT TEST ${_test})
            continue()
        endif()
        # 20x CPU in seconds is 2 * tenths; round up to a multiple of 10 s.
        math(EXPR _floor "((2 * ${_tenths} + 9) / 10) * 10")
        get_test_property(${_test} TIMEOUT _timeout)
        if(NOT _timeout OR _timeout LESS _floor)
            set_tests_properties(${_test} PROPERTIES TIMEOUT ${_floor})
        endif()
    endwhile()

    include(${CMAKE_CURRENT_LIST_DIR}/TilefinchPrivateTests.cmake)
    get_property(registered_test_binaries GLOBAL PROPERTY TILEFINCH_TEST_BINARIES)
    list(APPEND TILEFINCH_TEST_BINARY_TARGETS ${registered_test_binaries})
    list(REMOVE_DUPLICATES TILEFINCH_TEST_BINARY_TARGETS)
    add_custom_target(tilefinch-test-binaries
        DEPENDS ${TILEFINCH_TEST_BINARY_TARGETS}
                psp-browser-lab
                psp-browser-interactive-lab
                psp-browser-failure-recovery)
    if(NOT PSP)
        add_dependencies(tilefinch-test-binaries
            tilefinch_bootstrap_bytecode_generator)
    endif()
endif()
