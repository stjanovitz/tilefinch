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
    find_package(Threads REQUIRED)
    if(NOT PSP)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME tilefinch-bootstrap-generated-check
            COMMAND tilefinch_bootstrap_bytecode_generator --check
                "${TILEFINCH_BOOTSTRAP_SOURCE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/js_bootstrap_bytecode.c"
                "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(tilefinch-bootstrap-generated-check PROPERTIES
            TIMEOUT 30)
        add_test(NAME tilefinch-bootstrap-global-owner-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bootstrap_global_owners.py
                ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tilefinch-bootstrap-global-owner-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;bootstrap;architecture"
            TIMEOUT 10)
        add_test(NAME tilefinch-psp-sdk-contract-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_psp_sdk_contracts.py
                ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tilefinch-psp-sdk-contract-tests PROPERTIES
            LABELS "tilefinch;unit;psp;architecture"
            TIMEOUT 10)
        add_test(NAME tilefinch-ca-bundle-tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ca_bundle.py
                ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tilefinch-ca-bundle-tests PROPERTIES
            LABELS "tilefinch;unit;security;tls"
            TIMEOUT 10)
    endif()
    add_executable(tilefinch-tests tests/test_tilefinch.c)
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

    add_executable(tilefinch-layout-tests tests/test_layout.c)
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

    add_executable(tilefinch-media-mp4-tests tests/test_media_mp4.c)
    target_link_libraries(tilefinch-media-mp4-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-media-mp4-tests PRIVATE
        TILEFINCH_TEST_MEDIA_FIXTURE_240="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/psp-media/baseline-320x240.mp4"
        TILEFINCH_TEST_MEDIA_FIXTURE_360="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/psp-media/main-640x360.mp4")
    add_test(NAME tilefinch-media-mp4-tests COMMAND tilefinch-media-mp4-tests)
    set_tests_properties(tilefinch-media-mp4-tests PROPERTIES
        LABELS "tilefinch;unit;media" TIMEOUT 30)
    add_executable(tilefinch-host-media-timing-tests
        tests/test_host_media_timing.c)
    add_test(NAME tilefinch-host-media-timing-tests
        COMMAND tilefinch-host-media-timing-tests)
    set_tests_properties(tilefinch-host-media-timing-tests PROPERTIES
        LABELS "tilefinch;unit;media" TIMEOUT 30)
    add_executable(tilefinch-media-promotion-tests
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
    add_executable(tilefinch-media-dom-tests tests/test_media_dom.c)
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

    add_executable(tilefinch-url-tests tests/test_url.c)
    target_link_libraries(tilefinch-url-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-url-tests COMMAND tilefinch-url-tests)
    set_tests_properties(tilefinch-url-tests PROPERTIES
        LABELS "tilefinch;unit;security"
        TIMEOUT 30)

    add_executable(tilefinch-session-security-tests
        tests/test_session_security.c)
    target_link_libraries(tilefinch-session-security-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-session-security-tests
        COMMAND tilefinch-session-security-tests)
    set_tests_properties(tilefinch-session-security-tests PROPERTIES
        LABELS "tilefinch;unit;security"
        TIMEOUT 30)

    add_executable(tilefinch-content-security-policy-tests
        tests/test_content_security_policy.c)
    target_link_libraries(tilefinch-content-security-policy-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-content-security-policy-tests
        COMMAND tilefinch-content-security-policy-tests)
    set_tests_properties(tilefinch-content-security-policy-tests PROPERTIES
        LABELS "tilefinch;unit;security;web-platform"
        TIMEOUT 30)

    add_executable(tilefinch-resource-integrity-tests
        tests/test_resource_integrity.c)
    target_link_libraries(tilefinch-resource-integrity-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-resource-integrity-tests
        COMMAND tilefinch-resource-integrity-tests)
    set_tests_properties(tilefinch-resource-integrity-tests PROPERTIES
        LABELS "tilefinch;unit;security;web-platform"
        TIMEOUT 30)

    add_executable(tilefinch-frame-sandbox-tests
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
    add_executable(tilefinch-bn-mul-allegrex-tests
        tests/test_bn_mul_allegrex.c)
    add_test(NAME tilefinch-bn-mul-allegrex-tests
        COMMAND tilefinch-bn-mul-allegrex-tests)
    set_tests_properties(tilefinch-bn-mul-allegrex-tests PROPERTIES
        LABELS "tilefinch;unit;security;psp;tls"
        TIMEOUT 60)

    add_executable(tilefinch-update-tests tests/test_update.c)
    target_link_libraries(tilefinch-update-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-update-tests COMMAND tilefinch-update-tests)
    set_tests_properties(tilefinch-update-tests PROPERTIES
        LABELS "tilefinch;unit;security;update"
        TIMEOUT 30)

    add_executable(tilefinch-voice-component-tests
        tests/test_voice_component.c)
    target_link_libraries(tilefinch-voice-component-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-voice-component-tests
        COMMAND tilefinch-voice-component-tests)
    set_tests_properties(tilefinch-voice-component-tests PROPERTIES
        LABELS "tilefinch;unit;psp;update;storage;security"
        TIMEOUT 30)
    add_executable(tilefinch-glyph-component-tests
        tests/test_glyph_component.c)
    target_link_libraries(tilefinch-glyph-component-tests
        PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-glyph-component-tests PRIVATE
        TILEFINCH_TEST_SANS_FONT="${PSP_BROWSER_SANS_FONT}")
    add_test(NAME tilefinch-glyph-component-tests
        COMMAND tilefinch-glyph-component-tests)
    set_tests_properties(tilefinch-glyph-component-tests PROPERTIES
        LABELS "tilefinch;unit;font;storage;security"
        TIMEOUT 30)
    # Signing-ceremony rehearsal against the real embedded root. Skips
    # (exit 77) unless the build embeds an update root and the rehearsal
    # artifact paths are supplied via TILEFINCH_PROOF_* environment
    # variables; see docs/RELEASE_PROCESS.md.
    add_executable(tilefinch-update-root-proof-tests
        tests/test_update_root_proof.c)
    target_link_libraries(tilefinch-update-root-proof-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-update-root-proof-tests
        COMMAND tilefinch-update-root-proof-tests)
    set_tests_properties(tilefinch-update-root-proof-tests PROPERTIES
        LABELS "tilefinch;security;update;release"
        SKIP_RETURN_CODE 77
        TIMEOUT 60)
    add_executable(tilefinch-install-path-tests tests/test_install_paths.c)
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

    add_executable(tilefinch-session-persistence-tests
        tests/test_session_persistence.c)
    target_link_libraries(tilefinch-session-persistence-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-session-persistence-tests
        COMMAND tilefinch-session-persistence-tests)
    set_tests_properties(tilefinch-session-persistence-tests PROPERTIES
        LABELS "tilefinch;unit;session"
        TIMEOUT 30)

    # Cross-boot TLS session store (docs/engineering/PSP_TRANSPORT.md).
    # Pure data plus file I/O; does
    # not link curl or Mbed TLS. Covers the round-trip, corruption -> miss,
    # LRU eviction, expiry/wrong-RTC pruning, over-cap skip, and the
    # store-version and crypto-pin mismatch gates.
    add_executable(tilefinch-tls-session-store-tests
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
    add_executable(tilefinch-fetch-preconnect-tests
        tests/test_fetch_preconnect.c)
    target_link_libraries(tilefinch-fetch-preconnect-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-fetch-preconnect-tests
        COMMAND tilefinch-fetch-preconnect-tests)
    set_tests_properties(tilefinch-fetch-preconnect-tests PROPERTIES
        LABELS "tilefinch;unit;fetch;tls"
        TIMEOUT 30)

    add_executable(tilefinch-fetch-stream-scheduler-tests
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

    add_executable(tilefinch-style-index-tests tests/test_style_index.c)
    target_link_libraries(tilefinch-style-index-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-style-index-tests COMMAND tilefinch-style-index-tests)
    set_tests_properties(tilefinch-style-index-tests PROPERTIES
        LABELS "tilefinch;unit;style;performance"
        TIMEOUT 30)

    add_executable(tilefinch-navigation-load-tests
        tests/test_navigation_load.c)
    target_link_libraries(tilefinch-navigation-load-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-navigation-load-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-navigation-load-tests
        COMMAND tilefinch-navigation-load-tests)
    set_tests_properties(tilefinch-navigation-load-tests PROPERTIES
        LABELS "tilefinch;unit;network;streaming;navigation"
        TIMEOUT 30)

    add_executable(tilefinch-browser-engine-tests
        tests/test_browser_engine.c)
    target_link_libraries(tilefinch-browser-engine-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-browser-engine-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-browser-engine-tests
        COMMAND tilefinch-browser-engine-tests)
    set_tests_properties(tilefinch-browser-engine-tests PROPERTIES
        LABELS "tilefinch;unit;architecture;lifecycle"
        ENVIRONMENT "TILEFINCH_TRACE_TASKS=1"
        TIMEOUT 30)

    add_executable(tilefinch-browser-profile-tests
        tests/test_browser_profile.c)
    target_link_libraries(tilefinch-browser-profile-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-browser-profile-tests
        COMMAND tilefinch-browser-profile-tests)
    set_tests_properties(tilefinch-browser-profile-tests PROPERTIES
        LABELS "tilefinch;unit;storage;psp"
        TIMEOUT 10)

    add_executable(tilefinch-page-find-tests tests/test_page_find.c)
    target_link_libraries(tilefinch-page-find-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-page-find-tests COMMAND tilefinch-page-find-tests)
    set_tests_properties(tilefinch-page-find-tests PROPERTIES
        LABELS "tilefinch;unit;navigation;layout;psp"
        TIMEOUT 10)

    add_executable(tilefinch-content-blocker-tests
        tests/test_content_blocker.c)
    target_link_libraries(tilefinch-content-blocker-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-content-blocker-tests
        COMMAND tilefinch-content-blocker-tests)
    set_tests_properties(tilefinch-content-blocker-tests PROPERTIES
        LABELS "tilefinch;unit;network;security;psp"
        TIMEOUT 10)

    add_executable(tilefinch-browser-tabs-tests
        tests/test_browser_tabs.c)
    target_link_libraries(tilefinch-browser-tabs-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-browser-tabs-tests
        COMMAND tilefinch-browser-tabs-tests)
    set_tests_properties(tilefinch-browser-tabs-tests PROPERTIES
        LABELS "tilefinch;unit;navigation;psp"
        TIMEOUT 10)

    add_executable(tilefinch-offline-library-tests
        tests/test_offline_library.c src/psp_offline_store.c)
    target_link_libraries(tilefinch-offline-library-tests PRIVATE tilefinch_core)
    target_compile_definitions(tilefinch-offline-library-tests PRIVATE
        TILEFINCH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME tilefinch-offline-library-tests
        COMMAND tilefinch-offline-library-tests)
    set_tests_properties(tilefinch-offline-library-tests PROPERTIES
        LABELS "tilefinch;unit;storage;psp"
        TIMEOUT 10)

    add_executable(tilefinch-screenshot-png-tests
        tests/test_screenshot_png.c src/screenshot_png.c)
    target_include_directories(tilefinch-screenshot-png-tests PRIVATE include)
    add_test(NAME tilefinch-screenshot-png-tests
        COMMAND tilefinch-screenshot-png-tests)
    set_tests_properties(tilefinch-screenshot-png-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui;storage"
        TIMEOUT 10)

    add_executable(tilefinch-danzeff-input-tests
        tests/test_danzeff_input.c src/danzeff_input.c)
    target_include_directories(tilefinch-danzeff-input-tests PRIVATE include)
    add_test(NAME tilefinch-danzeff-input-tests
        COMMAND tilefinch-danzeff-input-tests)
    set_tests_properties(tilefinch-danzeff-input-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui;input"
        TIMEOUT 10)

    add_executable(tilefinch-omnibox-tests tests/test_omnibox.c)
    target_link_libraries(tilefinch-omnibox-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-omnibox-tests COMMAND tilefinch-omnibox-tests)
    set_tests_properties(tilefinch-omnibox-tests PROPERTIES
        LABELS "tilefinch;unit;navigation;psp"
        TIMEOUT 10)

    add_executable(tilefinch-voice-frontend-tests
        tests/test_voice_frontend.c)
    target_link_libraries(
        tilefinch-voice-frontend-tests PRIVATE tilefinch_voice_frontend)
    add_test(NAME tilefinch-voice-frontend-tests
        COMMAND tilefinch-voice-frontend-tests)
    set_tests_properties(tilefinch-voice-frontend-tests PROPERTIES
        LABELS "tilefinch;unit;psp;audio"
        TIMEOUT 30)

    add_executable(tilefinch-psp-ui-tests tests/test_psp_ui.c)
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

    add_executable(tilefinch-psp-boot-config-tests
        tests/test_psp_boot_config.c)
    target_link_libraries(tilefinch-psp-boot-config-tests
        PRIVATE tilefinch_psp_app_support)
    add_test(NAME tilefinch-psp-boot-config-tests
        COMMAND tilefinch-psp-boot-config-tests)
    set_tests_properties(tilefinch-psp-boot-config-tests PROPERTIES
        LABELS "tilefinch;unit;psp;config"
        TIMEOUT 10)

    add_executable(tilefinch-psp-boot-order-tests
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
    add_executable(tilefinch-psp-input-script-tests
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

    add_executable(tilefinch-psp-profile-store-tests
        tests/test_psp_profile_store.c)
    target_link_libraries(tilefinch-psp-profile-store-tests
        PRIVATE tilefinch_psp_app_support)
    add_test(NAME tilefinch-psp-profile-store-tests
        COMMAND tilefinch-psp-profile-store-tests)
    set_tests_properties(tilefinch-psp-profile-store-tests PROPERTIES
        LABELS "tilefinch;unit;psp;profile"
        TIMEOUT 10)

    add_executable(tilefinch-psp-lifecycle-tests
        tests/test_psp_lifecycle.c)
    target_link_libraries(tilefinch-psp-lifecycle-tests
        PRIVATE tilefinch_psp_app_support)
    add_test(NAME tilefinch-psp-lifecycle-tests
        COMMAND tilefinch-psp-lifecycle-tests)
    set_tests_properties(tilefinch-psp-lifecycle-tests PROPERTIES
        LABELS "tilefinch;unit;psp;lifecycle"
        TIMEOUT 10)

    add_executable(tilefinch-psp-update-session-tests
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
        src/psp_power_policy.c)
    target_include_directories(tilefinch_psp_power_test_ui PUBLIC include)
    target_link_libraries(
        tilefinch_psp_power_test_ui PUBLIC tilefinch_core)
    target_compile_definitions(tilefinch_psp_power_test_ui PRIVATE
        TILEFINCH_PSP_POWER_TEST_MENU=1)
    add_executable(
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

    add_executable(tilefinch-psp-media-scale-tests
        tests/test_psp_media_scale.c)
    target_link_libraries(tilefinch-psp-media-scale-tests PRIVATE
        tilefinch_psp_media_scale)
    add_test(NAME tilefinch-psp-media-scale-tests
        COMMAND tilefinch-psp-media-scale-tests)
    set_tests_properties(tilefinch-psp-media-scale-tests PROPERTIES
        LABELS "tilefinch;unit;psp;media"
        TIMEOUT 30)

    add_executable(tilefinch-psp-media-present-tests
        tests/test_psp_media_present.c)
    target_link_libraries(tilefinch-psp-media-present-tests PRIVATE
        tilefinch_psp_media_present)
    add_test(NAME tilefinch-psp-media-present-tests
        COMMAND tilefinch-psp-media-present-tests)
    set_tests_properties(tilefinch-psp-media-present-tests PROPERTIES
        LABELS "tilefinch;unit;psp;media"
        TIMEOUT 30)

    add_executable(tilefinch-psp-display-tests tests/test_psp_display.c)
    target_link_libraries(tilefinch-psp-display-tests PRIVATE
        tilefinch_psp_display)
    add_test(NAME tilefinch-psp-display-tests
        COMMAND tilefinch-psp-display-tests)
    set_tests_properties(tilefinch-psp-display-tests PROPERTIES
        LABELS "tilefinch;unit;psp;ui"
        TIMEOUT 10)

    add_executable(tilefinch-dynamic-script-async-tests
        tests/test_dynamic_script_async.c)
    target_link_libraries(tilefinch-dynamic-script-async-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-dynamic-script-async-tests
        COMMAND tilefinch-dynamic-script-async-tests)
    set_tests_properties(tilefinch-dynamic-script-async-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;network;async"
        TIMEOUT 30)

    add_executable(tilefinch-js-responsiveness-tests
        tests/test_js_responsiveness.c)
    target_link_libraries(tilefinch-js-responsiveness-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-js-responsiveness-tests
        COMMAND tilefinch-js-responsiveness-tests)
    set_tests_properties(tilefinch-js-responsiveness-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;responsiveness"
        TIMEOUT 30)

    if(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE)
        add_test(NAME tilefinch-property-fault-trace-tests
            COMMAND psp-browser-interactive-lab
                --fixture
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/property_fault_trace.html
                --no-external-resources
                --ticks 1
                --no-loop-capture)
        set_tests_properties(tilefinch-property-fault-trace-tests PROPERTIES
            ENVIRONMENT "TILEFINCH_TRACE_JS_PROPERTY_FAULTS=2"
            PASS_REGULAR_EXPRESSION
                "quickjs-property-fault seq=1.*base=undefined.*property=\\\"<string:12:computed-key>\\\""
            LABELS "tilefinch;unit;javascript;diagnostic"
            TIMEOUT 10)
    endif()

    add_executable(tilefinch-script-lazy-tests tests/test_script_lazy.c)
    target_link_libraries(tilefinch-script-lazy-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-script-lazy-tests COMMAND tilefinch-script-lazy-tests)
    set_tests_properties(tilefinch-script-lazy-tests PROPERTIES
        LABELS "tilefinch;unit;javascript;memory"
        TIMEOUT 30)

    if(PSP_BROWSER_USE_BELLARD_QUICKJS)
        add_executable(tilefinch-quickjs-oom-tests
            tests/test_quickjs_oom.c)
        target_link_libraries(tilefinch-quickjs-oom-tests PRIVATE tilefinch_core)
        add_test(NAME tilefinch-quickjs-oom-tests
            COMMAND tilefinch-quickjs-oom-tests)
        set_tests_properties(tilefinch-quickjs-oom-tests PROPERTIES
            LABELS "tilefinch;unit;javascript;allocator;sanitizer"
            TIMEOUT 30)
    endif()

    add_executable(tilefinch-stylesheet-resource-tests
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

    add_executable(tilefinch-web-font-tests tests/test_web_fonts.c)
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

    add_executable(tilefinch-budget-concurrent-tests
        tests/test_budget_concurrent.c)
    target_link_libraries(tilefinch-budget-concurrent-tests
        PRIVATE tilefinch_core Threads::Threads)
    add_test(NAME tilefinch-budget-concurrent-tests
        COMMAND tilefinch-budget-concurrent-tests)
    set_tests_properties(tilefinch-budget-concurrent-tests PROPERTIES
        LABELS "tilefinch;unit;allocator;concurrency;sanitizer"
        TIMEOUT 30)

    add_executable(tilefinch-psp-media-ownership-tests
        tests/test_psp_media_ownership.c)
    target_include_directories(tilefinch-psp-media-ownership-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tilefinch-psp-media-ownership-tests
        PRIVATE Threads::Threads)
    add_test(NAME tilefinch-psp-media-ownership-tests
        COMMAND tilefinch-psp-media-ownership-tests)
    set_tests_properties(tilefinch-psp-media-ownership-tests PROPERTIES
        LABELS "tilefinch;unit;media;psp;concurrency;sanitizer"
        TIMEOUT 30)

    add_executable(tilefinch-psp-media-state-tests
        tests/test_psp_media_state.c)
    target_link_libraries(tilefinch-psp-media-state-tests PRIVATE tilefinch_core)
    add_test(NAME tilefinch-psp-media-state-tests
        COMMAND tilefinch-psp-media-state-tests)
    set_tests_properties(tilefinch-psp-media-state-tests PROPERTIES
        LABELS "tilefinch;unit;media;psp;state"
        TIMEOUT 30)

    add_executable(tilefinch-psp-network-supervisor-tests
        tests/test_psp_network_supervisor.c)
    target_link_libraries(tilefinch-psp-network-supervisor-tests
        PRIVATE tilefinch_core)
    add_test(NAME tilefinch-psp-network-supervisor-tests
        COMMAND tilefinch-psp-network-supervisor-tests)
    set_tests_properties(tilefinch-psp-network-supervisor-tests PROPERTIES
        LABELS "tilefinch;unit;network;psp;state"
        TIMEOUT 30)

    add_test(NAME tilefinch-memory-ledger-tests
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_memory_ledger.sh)
    set_tests_properties(tilefinch-memory-ledger-tests PROPERTIES
        LABELS "tilefinch;unit;allocator;acceptance"
        TIMEOUT 10)

    find_program(TILEFINCH_TEST_PYTHON3_EXECUTABLE python3)
    if(TILEFINCH_TEST_PYTHON3_EXECUTABLE)
        add_test(NAME tilefinch-candidate-acceptance-runner-tests
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_candidate_acceptance_runner.py)
        set_tests_properties(tilefinch-candidate-acceptance-runner-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling"
            TIMEOUT 10)
        add_test(NAME tilefinch-reference-frame-tools-tests
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reference_frame_tools.py)
        set_tests_properties(tilefinch-reference-frame-tools-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling"
            TIMEOUT 10)
        add_test(NAME tilefinch-text-metrics-tests
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_text_metrics.py
                $<TARGET_FILE:psp-browser-interactive-lab>)
        set_tests_properties(tilefinch-text-metrics-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;font;layout"
            TIMEOUT 20)
        if(PSP_BROWSER_USE_BELLARD_QUICKJS)
            add_test(NAME tilefinch-fast-array-growth-tests
                COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fast_array_growth.py
                    $<TARGET_FILE:psp-browser-interactive-lab>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fast-array-growth.html)
            set_tests_properties(tilefinch-fast-array-growth-tests PROPERTIES
                LABELS "tilefinch;unit;javascript;allocator;performance"
                TIMEOUT 30)
        endif()
        add_test(NAME tilefinch-visual-scenario-tests
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_visual_scenarios.py)
        set_tests_properties(tilefinch-visual-scenario-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling"
            TIMEOUT 10)
        add_test(NAME tilefinch-fidelity-scoreboard-tests
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fidelity_scoreboard.py)
        set_tests_properties(tilefinch-fidelity-scoreboard-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;fidelity"
            TIMEOUT 10)
        # The fidelity oracle is defined on the optimized lab; unoptimized
        # builds diverge in replay-driven image scheduling (see
        # docs/FIDELITY.md) and would ratchet against the wrong binary.
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_test(NAME tilefinch-fidelity-floor-tests
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run-fidelity-scoreboard.py
                --manifest ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/fidelity-scenarios.tsv
                --trace-root ${CMAKE_CURRENT_SOURCE_DIR}/fidelity/captures
                --reference-root ${CMAKE_CURRENT_SOURCE_DIR}/fidelity/references
                --work-dir ${CMAKE_CURRENT_SOURCE_DIR}/fidelity/floor-check
                --lab $<TARGET_FILE:psp-browser-lab>
                --check-floors ${CMAKE_CURRENT_SOURCE_DIR}/tests/fidelity-baselines.tsv)
        set_tests_properties(tilefinch-fidelity-floor-tests PROPERTIES
            LABELS "tilefinch;acceptance;tooling;fidelity"
            SKIP_RETURN_CODE 77
            TIMEOUT 300)
        endif()
        add_test(NAME tilefinch-counter-baseline-tests
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
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
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_trace_replay_server.py)
        set_tests_properties(tilefinch-trace-replay-server-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;network"
            TIMEOUT 10)
        add_test(NAME tilefinch-reference-capture-tests
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reference_capture.py)
        set_tests_properties(tilefinch-reference-capture-tests PROPERTIES
            LABELS "tilefinch;unit;acceptance;tooling;network"
            TIMEOUT 10)
        if(PSP_BROWSER_LIBCURL_TRANSPORT)
            add_test(NAME tilefinch-trace-acquisition-tests
                COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_trace_acquisition.py
                    $<TARGET_FILE:psp-browser-trace-acquire>
                    $<TARGET_FILE:psp-browser-trace-inventory>)
            set_tests_properties(tilefinch-trace-acquisition-tests PROPERTIES
                LABELS "tilefinch;unit;acceptance;tooling;security"
                # This adversarial test creates executable wrappers. On macOS,
                # the first-execution scanner can stall them indefinitely when
                # other process-heavy acceptance tests start concurrently.
                # Serial execution keeps the security assertions intact and
                # makes the release gate independent of that host scheduler.
                RUN_SERIAL TRUE
                TIMEOUT 150)
        endif()
    endif()

    # `scripts/dev.sh test` builds this complete set before invoking CTest.
    # Keep it aligned with every executable-backed test registered below so a
    # clean build tree cannot report tests as Not Run because their binary was
    # never requested.
    set(TILEFINCH_TEST_BINARY_TARGETS
        tilefinch-tests
        tilefinch-media-mp4-tests
        tilefinch-host-media-timing-tests
        tilefinch-media-promotion-tests
        tilefinch-media-dom-tests
        tilefinch-layout-tests
        tilefinch-url-tests
        tilefinch-session-security-tests
        tilefinch-session-persistence-tests
        tilefinch-tls-session-store-tests
        tilefinch-fetch-preconnect-tests
        tilefinch-fetch-stream-scheduler-tests
        tilefinch-style-index-tests
        tilefinch-navigation-load-tests
        tilefinch-browser-engine-tests
        tilefinch-browser-profile-tests
        tilefinch-browser-tabs-tests
        tilefinch-screenshot-png-tests
        tilefinch-danzeff-input-tests
        tilefinch-omnibox-tests
        tilefinch-voice-frontend-tests
        tilefinch-glyph-component-tests
        tilefinch-psp-ui-tests
        tilefinch-psp-boot-config-tests
        tilefinch-psp-boot-order-tests
        tilefinch-psp-input-script-tests
        tilefinch-psp-profile-store-tests
        tilefinch-psp-update-session-tests
        tilefinch-dynamic-script-async-tests
        tilefinch-js-responsiveness-tests
        tilefinch-script-lazy-tests
        tilefinch-stylesheet-resource-tests
        tilefinch-web-font-tests
        tilefinch-budget-concurrent-tests
        tilefinch-psp-media-ownership-tests
        tilefinch-psp-media-state-tests
        psp-browser-interactive-lab
    )
    if(PSP_BROWSER_USE_BELLARD_QUICKJS)
        list(APPEND TILEFINCH_TEST_BINARY_TARGETS tilefinch-quickjs-oom-tests)
    endif()
    if(PSP_BROWSER_LIBCURL_TRANSPORT)
        list(APPEND TILEFINCH_TEST_BINARY_TARGETS
            psp-browser-trace-acquire
            psp-browser-trace-inventory)
    endif()

    if(PSP_BROWSER_BUILD_HOSTILE_PARSER_HARNESS)
        add_executable(tilefinch-hostile-parser-harness
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
        find_program(TILEFINCH_PYTHON3_EXECUTABLE python3)
        if(TILEFINCH_PYTHON3_EXECUTABLE)
            add_executable(tilefinch-fetch-redirect-tests
                tests/test_fetch_redirect.c)
            list(APPEND TILEFINCH_TEST_BINARY_TARGETS
                tilefinch-fetch-redirect-tests)
            target_link_libraries(tilefinch-fetch-redirect-tests PRIVATE tilefinch_core)
            add_test(NAME tilefinch-fetch-redirect-tests
                COMMAND ${TILEFINCH_PYTHON3_EXECUTABLE}
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
            add_executable(tilefinch-media-http-range-tests
                tests/test_media_http_range.c)
            list(APPEND TILEFINCH_TEST_BINARY_TARGETS
                tilefinch-media-http-range-tests)
            target_link_libraries(tilefinch-media-http-range-tests
                PRIVATE tilefinch_core)
            add_test(NAME tilefinch-media-http-range-tests
                COMMAND ${TILEFINCH_PYTHON3_EXECUTABLE}
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

    add_test(NAME selected-web-platform-tests
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run-web-platform-correctness.sh
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/selected-web-platform)
    set_tests_properties(selected-web-platform-tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        TIMEOUT 30)

    if(TILEFINCH_TEST_PYTHON3_EXECUTABLE)
        add_test(NAME tilefinch-upstream-wpt-runner-tests
            COMMAND ${TILEFINCH_TEST_PYTHON3_EXECUTABLE}
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

    if(PSP_BROWSER_QUICKJS_NATIVE_TRACE)
        add_executable(native-trace-tests
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
