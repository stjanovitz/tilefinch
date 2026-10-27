/* PSP browser boot phases; see psp_app_boot.h. Each body is a verbatim
   move out of main (src/psp_script_main.c). */
#include "psp_app_internal.h"
#include "psp_app_boot.h"
#include "psp_app_validation.h"
#include "tilefinch/psp_time.h"
#include "tilefinch_compiler.h"

static TILEFINCH_COLD_PATH void psp_storage_paths_init(
    PspStoragePaths *storage, const TilefinchInstallPaths *install_paths)
{
    if (storage == NULL || install_paths == NULL) return;
    memset(storage, 0, sizeof(*storage));
    tilefinch_install_data_path(
        install_paths, "profile.cfg", storage->profile,
        sizeof(storage->profile));
    tilefinch_install_data_path(
        install_paths, "tab-hibernation.bin", storage->tab_hibernation,
        sizeof(storage->tab_hibernation));
    tilefinch_install_data_path(
        install_paths, "tabs-session.bin", storage->tab_session,
        sizeof(storage->tab_session));
    tilefinch_install_data_path(
        install_paths, "recovery.cfg", storage->recovery,
        sizeof(storage->recovery));
    tilefinch_install_data_path(
        install_paths, "http-cache.bin", storage->persistent_cache,
        sizeof(storage->persistent_cache));
    tilefinch_install_data_path(
        install_paths, "local-storage.bin", storage->local_storage,
        sizeof(storage->local_storage));
    tilefinch_install_data_path(
        install_paths, "tls-sessions.bin", storage->tls_sessions,
        sizeof(storage->tls_sessions));
    tilefinch_install_data_path(
        install_paths, "adblock.txt", storage->content_blocker,
        sizeof(storage->content_blocker));
    tilefinch_install_data_path(
        install_paths, "adblock-allow.txt", storage->content_allowlist,
        sizeof(storage->content_allowlist));
    tilefinch_install_data_path(
        install_paths, "offline", storage->offline_library,
        sizeof(storage->offline_library));
    tilefinch_install_data_path(
        install_paths, "site-storage", storage->site_storage,
        sizeof(storage->site_storage));
    tilefinch_install_data_path(
        install_paths, "script-cache", storage->script_cache,
        sizeof(storage->script_cache));
    tilefinch_install_data_path(install_paths, "youtube-login.bin",
        storage->youtube_login, sizeof(storage->youtube_login));
}

/* The optional persistent module bytecode tier (off unless boot.cfg names
   a directory). Kept out of main, whose size is an instruction-cache
   ratchet. */
__attribute__((noinline, cold))
static void psp_apply_module_cache_config(const PspBootConfig *config,
                                          BrowserConfig *engine_config)
{
    if (config->module_cache_dir[0] == '\0') return;
    snprintf(engine_config->script_cache_dir,
             sizeof(engine_config->script_cache_dir), "%s",
             config->module_cache_dir);
    engine_config->script_cache_write = config->module_cache_write == 1;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-script-cache: configured source=boot.cfg dir=%s "
           "write=%d\n", engine_config->script_cache_dir,
           engine_config->script_cache_write ? 1 : 0);
#endif
}

/* Env-driven engine knobs (newlib setenv works in-process). Kept out of
   main, whose size is an instruction-cache ratchet. */
__attribute__((noinline, cold))
static void psp_apply_engine_env_knobs(const PspBootConfig *config)
{
    char scratch[32];
    psp_validation_apply_env_knobs(config);
    if (config->window_kb > 0) {
        snprintf(scratch, sizeof(scratch), "%ld", config->window_kb);
        setenv("TILEFINCH_JS_BOOT_WINDOW_KB", scratch, 1);
    }
    if (config->gc_growth_pct > 0) {
        snprintf(scratch, sizeof(scratch), "%ld", config->gc_growth_pct);
        setenv("TILEFINCH_JS_GC_GROWTH_PCT", scratch, 1);
    }
    if (config->lazy_functions >= 0) {
        snprintf(scratch, sizeof(scratch), "%ld", config->lazy_functions);
        setenv("TILEFINCH_JS_LAZY_FUNCTIONS", scratch, 1);
    }
    /* Device profile: bound any single JS array's value buffer; a page
       once grouped a multi-megabyte binary string per character, which
       can never fit beside hydration on the 50MB heap. */
    snprintf(scratch, sizeof(scratch), "%u",
             (unsigned) BROWSER_PSP_APP_JS_ARRAY_CAP_KB);
    setenv("TILEFINCH_JS_ARRAY_CAP_KB", scratch, 1);
}

/* The one-time file_kb migration (psp_boot_config_migrate_file_kb) of a
   user-owned boot configuration file, before it is read. Shipping builds
   only: validation harnesses write their own boot.cfg for each run, and
   staged games keep their deliberate 512 KiB Game Profile ceiling. The
   slot's signed boot-defaults.cfg is never rewritten. */
static TILEFINCH_COLD_PATH void psp_migrate_boot_file_kb(const char *path)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    (void) path;
#else
    PspBootConfigMigration migration = psp_boot_config_migrate_file_kb(path);
    if (migration == PSP_BOOT_CONFIG_MIGRATION_DONE
        || migration == PSP_BOOT_CONFIG_MIGRATION_FAILED) {
        printf("tilefinch-config: file_kb-migration=%s file_kb=%ld\n",
               migration == PSP_BOOT_CONFIG_MIGRATION_DONE ? "done"
                                                           : "failed",
               psp_boot_config_default_file_kb());
    }
#endif
}

TILEFINCH_COLD_PATH PspStartupFailure psp_boot_load_config(
    PspProcessResources *process, const char *argv0)
{
    psp_log_set_phase(PSP_LOG_PHASE_CONFIG);
    psp_boot_config_defaults(&process->config);
    char path[768];
    bool config_loaded = false;
    if (process->install_paths.slotted) {
        if (tilefinch_install_program_path(
                &process->install_paths, "boot-defaults.cfg",
                path, sizeof(path))) {
            config_loaded = psp_boot_config_load(
                &process->config, path, psp_config_warning, NULL);
        }
        char overrides_path[TILEFINCH_INSTALL_PATH_LIMIT];
        bool have_overrides_path = tilefinch_install_data_path(
            &process->install_paths, "boot-overrides.cfg",
            overrides_path, sizeof(overrides_path));
        if (have_overrides_path)
            psp_migrate_boot_file_kb(overrides_path);
        bool overrides_loaded = have_overrides_path
            && psp_boot_config_load(
                &process->config, overrides_path, psp_config_warning, NULL);
        if (!overrides_loaded) {
            char legacy_path[TILEFINCH_INSTALL_PATH_LIMIT];
            int legacy_length = snprintf(
                legacy_path, sizeof(legacy_path), "%s/boot-live.cfg",
                process->install_paths.install_root);
            if (legacy_length > 0
                && (size_t) legacy_length < sizeof(legacy_path))
                psp_migrate_boot_file_kb(legacy_path);
            if (legacy_length > 0
                && (size_t) legacy_length < sizeof(legacy_path)
                && psp_boot_config_load(
                    &process->config, legacy_path, psp_config_warning, NULL)) {
                overrides_loaded = have_overrides_path
                    && psp_boot_config_write_overrides(
                        &process->config, overrides_path);
                printf("tilefinch-config: legacy-overrides=%s\n",
                       overrides_loaded ? "migrated" : "active-not-saved");
            }
        }
        config_loaded = config_loaded || overrides_loaded;
    } else {
        psp_sibling_path(path, sizeof(path), argv0, "boot.cfg");
        psp_migrate_boot_file_kb(path);
        config_loaded = psp_boot_config_load(
            &process->config, path, psp_config_warning, NULL);
    }
#ifndef TILEFINCH_PSP_VALIDATION_LOG
    /* A shipping EBOOT never executes a copied validation scenario. This is
       intentionally after both configuration layers: stale automation in
       either one is inert, while user-facing URL/network/update choices keep
       their ordinary precedence. */
    psp_boot_config_disable_automation(&process->config);
#endif
    if (!config_loaded)
        printf("tilefinch-psp: boot configuration missing, using defaults\n");
    /* Keep existing Memory Stick installs with `url=` compatible with the
       built-in start page, while an explicit boot.cfg URL remains an
       intentional direct-navigation override. */
    if (process->config.url[0] == '\0') {
        snprintf(process->config.url, sizeof(process->config.url), "%s",
                 TILEFINCH_HOMEPAGE_URL);
    }
    /*
     * A typo in the two hand-edited developer update keys must not brick a
     * Memory Stick install. They are optional and their only effect is to
     * offer the Developer channel, so an unusable value is treated as an
     * absent one: the channel stays hidden and the browser boots. Every
     * other key keeps the halt-on-invalid posture it had before this
     * channel existed.
     */
    unsigned dropped_developer_urls =
        psp_boot_config_drop_invalid_developer_urls(&process->config);
    if (dropped_developer_urls != 0u) {
        printf("tilefinch-config: ignoring unusable developer endpoint "
               "update-url=%d package-url=%d; the Developer update channel "
               "stays hidden\n",
               (dropped_developer_urls
                & PSP_BOOT_CONFIG_DROPPED_DEVELOPER_UPDATE_URL) != 0u,
               (dropped_developer_urls
                & PSP_BOOT_CONFIG_DROPPED_DEVELOPER_PACKAGE_URL) != 0u);
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Same posture, and the same reason: the exit handoff is a developer
       convenience whose only cost when it is wrong is that the console
       returns to the XMB, which is what happens with no key at all. The
       parser reads the key in every build; only a validation EBOOT can act
       on it, so only a validation EBOOT spends anything deciding. */
    if (psp_boot_config_drop_unusable_exit_to(&process->config)) {
        printf("tilefinch-config: ignoring unusable exit_to; this EBOOT "
               "will exit to the XMB\n");
    }
#endif
    const char *invalid_field = NULL;
    if (!psp_boot_config_validate(&process->config, &invalid_field)) {
        printf("tilefinch-psp-script: invalid boot configuration field=%s\n",
               invalid_field == NULL ? "unknown" : invalid_field);
        return psp_startup_failure(
            "Invalid boot configuration",
            "STARTUP FAILED: SETTINGS");
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_display_set_latch_probe_enabled(
        process->config.validation_latch_probe != 0);
    printf("tilefinch-display: latch-probe=%s\n",
           process->config.validation_latch_probe != 0
               ? "enabled" : "disabled");
#endif
    /* Publish the decoder-program knob before anything can open a video.
       The backend owns the parse so this boot path stays a single call. */
    media_psp_backend_set_wide_program(process->config.experimental_wide_video);
    media_psp_backend_set_au_dump(
        process->config.validation_media_au_dump != 0 ? 1 : 0);
    /* And how a refused access unit is recovered, published here for the same
       reason and in the same breath: boot decides it, and it has to be decided
       before anything can open a video. */
    psp_media_set_refusal_reset(
        process->config.validation_media_refusal_reset != 0);
    media_psp_backend_set_reset_mode(
        (int) process->config.validation_media_reset_mode);
    printf("tilefinch-psp-script: url=%s trace=%s ticks=%ld limit=%ldMB "
           "heap=%ldMB window=%ldKB profile=%s network-profile=%ld "
           "dump-frame=%ld validation-cancel-ms=%ld "
           "validation-preview-scroll=%ld validation-media-stability=%ld "
           "validation-media-fixture=%ld "
           "validation-raster-fixture=%ld "
           "validation-power-auto=%ld input-script=%s "
           /* Appended, never inserted: a soak log has to say which seek and
              which refusal recovery produced it without the boot file it was
              produced from, and every harvester reads this line by key. */
           "validation-media-seek-permille=%ld "
           "validation-media-lifecycle=%ld "
           "validation-media-refusal-reset=%ld "
           "validation-media-reset-mode=%ld app-heap=%ldMB\n",
           process->config.url, process->config.trace, process->config.ticks, process->config.limit_mb,
           process->config.heap_mb, process->config.window_kb, process->config.profile,
           process->config.network_profile, process->config.dump_frame,
           process->config.validation_cancel_after_ms,
           process->config.validation_preview_scroll,
           process->config.validation_media_stability_auto,
           process->config.validation_media_fixture_auto,
           process->config.validation_raster_fixture_auto,
           process->config.validation_power_test_auto,
           process->config.input_script[0] == '\0' ? "none" : process->config.input_script,
           process->config.validation_media_seek_permille,
           process->config.validation_media_lifecycle_auto,
           process->config.validation_media_refusal_reset,
           process->config.validation_media_reset_mode,
           process->config.app_heap_mb);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-psp-script: exit-to=%s\n",
           process->config.exit_to[0] == '\0' ? "none" : process->config.exit_to);
#endif
    psp_log_checkpoint("config-accepted");
    return psp_startup_failure(NULL, NULL);
}

TILEFINCH_COLD_PATH PspStartupFailure psp_boot_configure_engine(
    PspProcessResources *process, BrowserConfig **engine_config_out)
{
    if (strcmp(process->config.trace, "none") == 0) {
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        psp_log_set_phase(PSP_LOG_PHASE_ASSETS);
        char ca_bundle_path[768];
        if (!tilefinch_install_program_path(
                &process->install_paths, "roots.pem",
                ca_bundle_path, sizeof(ca_bundle_path))) {
            return psp_startup_failure(
                "Trust bundle path is unavailable",
                "STARTUP FAILED: INSTALL FILES");
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        size_t ca_bundle_bytes = 0;
        uint8_t ca_bundle_digest[TILEFINCH_SHA256_DIGEST_BYTES];
        if (!psp_probe_file(
                ca_bundle_path, 256u * KIB, &ca_bundle_bytes,
                ca_bundle_digest)) {
            /* This read exists only to enrich validation diagnostics. The
               transport independently loads the bundle and fails closed on
               the first HTTPS request, so a diagnostic allocation/I/O
               refusal must not brick local Home, Settings, or Library. */
            memset(ca_bundle_digest, 0, sizeof(ca_bundle_digest));
            printf("tilefinch-network: trust bundle diagnostic unavailable: %s\n",
                   ca_bundle_path);
        }
#endif
        if (!fetch_set_ca_bundle_path(ca_bundle_path)) {
            printf("tilefinch-network: trust bundle configuration failed\n");
            return psp_startup_failure(
                "Trust bundle configuration failed",
                "STARTUP FAILED: INSTALL FILES");
        }
        time_t tls_time = 0;
        PspTimeFields rtc = {0};
        PspTimeStatus rtc_status = psp_time_read_utc_fields(
            &rtc, &tls_time);
        if (rtc_status != PSP_TIME_OK) {
            /* Home, Settings, Library, and installed apps do not need TLS.
               Keep time() fail-closed at epoch zero and let an eventual
               certificate failure show the existing date/time guidance;
               an unreadable clock must not brick the entire browser-> */
            printf("tilefinch-network: RTC unavailable for TLS status=%s "
                   "action=continue-offline\n",
                   psp_time_status_name(rtc_status));
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        psp_report_tls_assets(
            ca_bundle_bytes, ca_bundle_digest,
            tls_time, &rtc, rtc_status);
#else
        /*
         * CURLOPT_CAINFO still verifies the bundle on the first HTTPS
         * request and fails closed if it is absent or invalid. Avoid reading,
         * allocating, and hashing the entire file before a local homepage
         * that does not need transport. Validation builds retain that
         * diagnostic so device reports still attest to the copied asset.
        */
        (void) rtc;
        (void) tls_time;
#endif
        psp_log_checkpoint("tls-assets-ready");
#else
        printf("tilefinch-network: live transport unavailable in replay "
               "build\n");
        return psp_startup_failure(
            "Live network transport is unavailable",
            "STARTUP FAILED: NETWORK BUILD");
#endif
    }

    psp_apply_engine_env_knobs(&process->config);

    printf("tilefinch-psp-script: env window=%s growth=%s\n",
           getenv("TILEFINCH_JS_BOOT_WINDOW_KB") == NULL
               ? "(null)" : getenv("TILEFINCH_JS_BOOT_WINDOW_KB"),
           getenv("TILEFINCH_JS_GC_GROWTH_PCT") == NULL
               ? "(null)" : getenv("TILEFINCH_JS_GC_GROWTH_PCT"));
    script_runtime_configure_deterministic_replay(
        strcmp(process->config.trace, "none") != 0, 7);

    psp_log_set_phase(PSP_LOG_PHASE_ENGINE);
    psp_log_checkpoint("engine-config-begin");
    BrowserDeviceProfile device_profile;
    browser_device_profile_psp3000(&device_profile);
    BrowserConfig *engine_config = calloc(1, sizeof(*engine_config));
    if (engine_config == NULL) {
        printf("tilefinch-psp-script: config allocation failed\n");
        return psp_startup_failure(
            "Browser configuration allocation failed",
            "STARTUP FAILED: LOW MEMORY");
    }
    browser_config_init(engine_config, &device_profile);
    /* The shared PSP app profile (also the lab's --psp-profile realistic);
       boot.cfg values, whose defaults are the same constants, follow. */
    (void) browser_config_apply_psp_app_defaults(engine_config);
    engine_config->memory_limit = (size_t) process->config.limit_mb * MIB;
    psp_apply_module_cache_config(&process->config, engine_config);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Same native audio slots as shipping unless boot.cfg explicitly selects
       the JS Web Audio reference for an A/B run. */
    if (process->config.validation_game_audio_reference == 1)
        engine_config->javascript.game_audio_internal_slots = false;
#endif
    engine_config->javascript.heap_limit = (size_t) process->config.heap_mb * MIB;
    /* Installed apps get at least the ordinary heap. */
    engine_config->javascript.installed_app_heap_limit =
        (size_t) (process->config.app_heap_mb > process->config.heap_mb
                      ? process->config.app_heap_mb
                      : process->config.heap_mb) * MIB;
    engine_config->javascript.runtime_timeout_ms =
        (unsigned long) process->config.script_timeout_ms;
    ScriptExecutionProfile execution_profile = SCRIPT_EXECUTION_PROFILE_LAB;
    if (strcmp(process->config.profile, "strict") == 0) {
        execution_profile = SCRIPT_EXECUTION_PROFILE_PSP_STRICT;
    } else if (strcmp(process->config.profile, "realistic") == 0) {
        execution_profile = SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC;
    }
    size_t configured_script_file_bytes =
        (size_t) process->config.file_kb * KIB;
    if (!browser_config_apply_script_limits(
            engine_config, execution_profile,
            (size_t) process->config.count,
            (size_t) process->config.total_mb * MIB,
            configured_script_file_bytes)) {
        printf("tilefinch-psp-script: execution policy failed\n");
        return psp_startup_failure(
            "JavaScript execution policy is unavailable",
            "STARTUP FAILED: BROWSER POLICY");
    }
    if (engine_config->javascript.maximum_file_bytes
            != configured_script_file_bytes) {
        printf("tilefinch-psp-script: script-file-limit clamped=%zu/%zuB\n",
               engine_config->javascript.maximum_file_bytes,
               configured_script_file_bytes);
    }

    char sans[300], serif[300], sans_italic[300], sans_bold[300];
    char serif_bold[300], metric_sans[300], metric_sans_bold[300];
    tilefinch_install_program_path(
        &process->install_paths, "fonts/DejaVuSans-Latin.ttf", sans, sizeof(sans));
    tilefinch_install_program_path(
        &process->install_paths, "fonts/DejaVuSerif-Latin.ttf", serif, sizeof(serif));
    tilefinch_install_program_path(
        &process->install_paths, "fonts/DejaVuSans-Oblique-Latin.ttf",
        sans_italic, sizeof(sans_italic));
    tilefinch_install_program_path(
        &process->install_paths, "fonts/DejaVuSans-Bold-Latin.ttf",
        sans_bold, sizeof(sans_bold));
    tilefinch_install_program_path(
        &process->install_paths, "fonts/DejaVuSerif-Bold-Latin.ttf",
        serif_bold, sizeof(serif_bold));
    tilefinch_install_program_path(
        &process->install_paths, "fonts/TilefinchSans-Regular.ttf",
        metric_sans, sizeof(metric_sans));
    tilefinch_install_program_path(
        &process->install_paths, "fonts/TilefinchSans-Bold.ttf",
        metric_sans_bold, sizeof(metric_sans_bold));
    if (!browser_config_set_font_paths(engine_config, sans, serif,
                                       sans_italic, sans_bold, serif_bold,
                                       metric_sans, metric_sans_bold,
                                       1536 * KIB)) {
        printf("tilefinch-psp-script: font configuration failed\n");
        return psp_startup_failure(
            "Font configuration failed",
            "STARTUP FAILED: INSTALL FILES");
    }
    /* Native HOME uses the embedded chrome fallback. Page fonts are loaded
       after its first frame, one bounded file at a time while association is
       otherwise waiting on firmware. */
    browser_config_set_staged_font_loading(engine_config, true);
    *engine_config_out = engine_config;
    return psp_startup_failure(NULL, NULL);
}

TILEFINCH_COLD_PATH PspStartupFailure psp_boot_open_profile_services(
    PspProcessResources *process, PspBrowserResources *browser,
    PspBootProfileState *state)
{
    /* Native HOME uses the regular page sans face for its card labels. Load
       that one face before the first HOME frame so the visible UI never
       switches from bitmap fallback to scalable glyphs. Metric sans and all
       optional faces remain staged behind interaction/network work. */
    if (!browser_engine_prepare_native_home_font(browser->engine)) {
        printf("tilefinch-fonts: native-home face unavailable; "
               "using stable fallback\n");
    }
    psp_presentation_init(&process->presentation);
    psp_presentation_bind_chrome_fonts(&process->presentation, browser->engine);
    /* The client snapshots carry bounded manifest tables and would enlarge
       main's already-large stack frame enough to pessimize every frame-local
       access on Allegrex. The browser has exactly one optional-component
       session, so give that singleton static storage instead. */
    static PspVoiceComponentSession voice_component;
    browser->voice_component_session = &voice_component;
    static PspGlyphComponentSession glyph_component;
    browser->glyph_component_session = &glyph_component;
    psp_storage_paths_init(&process->storage, &process->install_paths);
    /* Cross-boot TLS session resumption store
       (docs/engineering/PSP_TRANSPORT.md). The blobs are resumption
       secrets: stick-only, cleared by CLEAR HTTP CACHES below. The store is
       active only with the owned live-network transport; profile policy is
       applied below before the first network request. */
    (void) fetch_set_tls_session_store_path(process->storage.tls_sessions);
    browser->profile = browser_profile_create(browser->budget);
    if (browser->profile == NULL) {
        printf("tilefinch-profile: allocation failed\n");
        return psp_startup_failure(
            "Profile allocation failed",
            "STARTUP FAILED: LOW MEMORY");
    }
    if (browser_profile_load_without_content_blocker_sites(
            browser->profile, process->storage.profile))
        printf("tilefinch-profile: loaded\n");
    (void) psp_web_font_setting_boot(browser->profile, browser->engine);
    (void) psp_failure_report_configure(
        browser_profile_save_diagnostic_reports(browser->profile), false);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Automated device runs must never enter Sony's modal OSK: it outlives a
       stopped test process on some firmware and poisons the next PSPLink
       launch. This is an in-memory harness override; shipping profiles and
       the profile file on host0 remain unchanged. */
    if (process->config.input_script[0] != '\0') {
        browser_profile_set_text_entry_mode(
            browser->profile, BROWSER_TEXT_ENTRY_DANZEFF);
        printf("tilefinch-input-script: text-entry=danzeff\n");
    }
#endif
    bool tls_session_persistence =
        browser_profile_tls_session_persistence(browser->profile);
    (void) fetch_set_tls_session_persistence_enabled(
        tls_session_persistence);
    if (!psp_glyph_component_session_attach_selected(
            browser->glyph_component_session, browser->budget,
            &process->install_paths,
            browser_profile_glyph_language(browser->profile),
            psp_ui_language_glyphs(browser->profile),
            browser_profile_color_emoji(browser->profile))) {
        printf("tilefinch-glyph-component: selected packs unavailable\n");
    }
    unsigned locale = browser_profile_ui_language(browser->profile);
    if (psp_ui_language_font_available(browser))
        browser->ui_translation = tilefinch_ui_translation_load(browser->budget,
            &process->install_paths, locale);
    tilefinch_ui_translation_bind(browser->ui_translation);
    /* Do not enumerate optional packs during ordinary boot. Selected packs
       were verified above; the Options screen probes the rest on demand. */
    state->tls_session_persistence = tls_session_persistence;
    return psp_startup_failure(NULL, NULL);
}

TILEFINCH_COLD_PATH void psp_boot_apply_profile_policy(
    PspProcessResources *process, PspBrowserResources *browser,
    PspBootProfileState *state)
{
    bool javascript_enabled =
        browser_profile_javascript_enabled(browser->profile);
    bool site_data_allowed =
        browser_profile_site_data_allowed(browser->profile);
    browser_session_set_site_data_allowed(browser->session, site_data_allowed);
    psp_profile_store_init(&browser->profile_store, browser->profile, process->storage.profile);
    ContentBlockerMode content_blocker_mode =
        browser_profile_content_blocker_mode(browser->profile);
    if (!browser_engine_content_blocker_configure(
            browser->engine, content_blocker_mode, process->storage.content_blocker)) {
        content_blocker_mode = CONTENT_BLOCKER_BASIC;
        (void) browser_engine_content_blocker_configure(
            browser->engine, content_blocker_mode, NULL);
        printf(
            "tilefinch-content-blocker: custom list unavailable; "
            "using basic mode\n");
    }
    if (!psp_content_blocker_apply_allowed_sites(browser->engine, browser->profile)) {
        printf("tilefinch-content-blocker: allowlist unavailable\n");
    }
    ContentBlockerMetrics startup_blocker_metrics = {0};
    if (browser_engine_content_blocker_metrics(
            browser->engine, &startup_blocker_metrics)) {
        printf(
            "tilefinch-content-blocker: mode=%u rules=%zu allow-rules=%zu "
            "ignored=%zu allowed-sites=%zu retained=%zu\n",
            (unsigned) startup_blocker_metrics.mode,
            startup_blocker_metrics.rule_count,
            startup_blocker_metrics.allow_rule_count,
            startup_blocker_metrics.ignored_rule_count,
            startup_blocker_metrics.allowed_site_count,
            startup_blocker_metrics.retained_bytes);
    }
    unsigned persistent_cache_mb =
        browser_profile_persistent_cache_mb(browser->profile);
    unsigned live_cache_kib =
        browser_profile_live_cache_kib(browser->profile);
    process->persistent_site_data_available =
        strcmp(process->config.trace, "none") == 0;
    /* Before any localStorage snapshot loads, so a site kept on the
       Memory Stick is never overridden by it. */
    psp_app_site_storage_boot(process, browser);
    psp_app_script_cache_boot(process, browser);
    size_t live_cache_bytes = (size_t) live_cache_kib * KIB;
    bool live_cache_limit_set =
        browser_session_cache_set_maximum_bytes(browser->session, live_cache_bytes);
    if (!live_cache_limit_set) {
        printf("tilefinch-site-data: cache limit unavailable bytes=%zu\n",
               live_cache_bytes);
    }
    state->javascript_enabled = javascript_enabled;
    state->site_data_allowed = site_data_allowed;
    state->content_blocker_mode = content_blocker_mode;
    state->startup_requests_blocked = startup_blocker_metrics.requests_blocked;
    state->persistent_cache_mb = persistent_cache_mb;
    state->live_cache_kib = live_cache_kib;
    state->live_cache_limit_set = live_cache_limit_set;
}

TILEFINCH_COLD_PATH PspBootNavigation psp_boot_initial_navigation(
    PspProcessResources *process, PspBrowserResources *browser,
    PspEngineViews *engine_views, const char *startup_url,
    bool native_home_boot, bool restoring_last_page,
    const BrowserRecoveryCheckpoint *startup_recovery, const char *argv0)
{
    psp_log_set_phase(PSP_LOG_PHASE_NAVIGATION);
    psp_log_checkpoint("initial-navigation-begin");
    psp_validation_cancel_after_ms = (unsigned) PSP_VALIDATION_KNOB(
        process->config.validation_cancel_after_ms);
    psp_validation_preview_scroll = (unsigned) PSP_VALIDATION_KNOB(
        process->config.validation_preview_scroll);
    __sync_synchronize();
    uint64_t load_started_us = (uint64_t) sceKernelGetSystemTimeWide();
    bool initial_load_stopped = false;
    PspProfilePageKind initial_profile_page =
        psp_profile_page_kind(startup_url);
    bool loaded;
    if (native_home_boot) {
        /* Native HOME does not wait on a document. The empty committed page
           gives shell, tab, and history code a valid engine shape while HOME
           owns the first visible frame. Nothing here touches the network. */
        loaded = browser_engine_commit_empty_document(
            browser->engine, TILEFINCH_HOMEPAGE_URL, true)
            && browser_engine_refresh_shell(browser->engine);
        if (loaded) {
            psp_home_sync_ui(&process->presentation.ui, &process->presentation.home_surface, browser->profile, NULL, false);
            psp_ui_show_home(&process->presentation.ui);
            /* The first HOME frame is the settled, ordinary surface. Do not
               spend ten uninterruptible full-screen presents on an entrance
               animation immediately before the input loop starts. */
            psp_present(engine_views->frame, &process->presentation.ui);
            PSP_BOOT_TIMING_MARK("native-home-presented");
        }
    } else if (initial_profile_page != PSP_PROFILE_PAGE_NONE) {
        loaded = psp_profile_open_page(
            browser->engine, &process->presentation.ui, browser->profile,
            initial_profile_page);
    } else if (psp_offline_url(startup_url)) {
        PspOfflineRouteResult offline_result =
            psp_offline_store_handle_url(
                &browser->offline_store, browser->engine, browser->profile,
                startup_url,
                NULL, true);
        loaded = offline_result == PSP_OFFLINE_ROUTE_PAGE
            || offline_result == PSP_OFFLINE_ROUTE_STATE_CHANGED;
    } else {
        loaded = psp_run_initial_page_load(
            browser->engine, &process->presentation.ui, engine_views->frame,
            startup_url, PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS, argv0,
            PSP_VALIDATION_KNOB(process->config.dump_frame == 2),
            &initial_load_stopped);
    }
    psp_validation_cancel_after_ms = 0;
    psp_validation_preview_scroll = 0;
    __sync_synchronize();
    bool initial_download_offer = !loaded && psp_offline_store_take_download_offer(
        &browser->offline_store, browser->engine);
    if (initial_download_offer) {
        /* A binary startup URL has no incumbent page. Use the local library
           beneath its confirmation, not a failed HTML view of the file. */
        loaded = psp_offline_store_open_library(
            &browser->offline_store, browser->engine, true);
    }
    bool initial_error_page = false;
    uint64_t load_elapsed_ms =
        ((uint64_t) sceKernelGetSystemTimeWide() - load_started_us) / 1000u;
    printf("tilefinch-psp-script: load %s elapsed=%llums http=%ld "
           "server=\"%.32s\" mitigated=\"%.16s\" error=\"%.300s\" "
           "full-relayouts=%llu images=%zu/%zu stream=%zuB/%zu "
           "peak-buffer=%zu cancelled=%d truncated=%d system-free=%d "
           "system-largest=%d\n",
           loaded ? "ok" : "failed",
           (unsigned long long) load_elapsed_ms,
           engine_views->navigation->last_http_status, engine_views->navigation->last_server,
           engine_views->navigation->last_cf_mitigated,
           initial_load_stopped ? "stopped by user"
                                : engine_views->navigation->last_error,
           (unsigned long long) engine_views->navigation->performance.full_relayouts,
           engine_views->navigation->page.images.stats.loaded,
           engine_views->navigation->page.images.stats.discovered,
           engine_views->navigation->last_stream.bytes_received,
           engine_views->navigation->last_stream.chunks_received,
           engine_views->navigation->last_stream.peak_buffered_bytes,
           engine_views->navigation->last_stream.cancelled ? 1 : 0,
           engine_views->navigation->last_stream.truncated ? 1 : 0,
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    psp_log_checkpoint(
        loaded ? "initial-navigation-complete"
               : "initial-navigation-failed");
    if (!loaded && !initial_load_stopped) {
        (void) psp_write_navigation_failure_report(
            "initial-navigation", engine_views->navigation->last_error, startup_url,
            engine_views->navigation);
    }
    psp_ui_set_loading(&process->presentation.ui, false, loaded ? 1000 : 0);
    if (loaded) {
        /* The restored page is opened from behind HOME on a native boot, so
           its scroll is applied when it arrives, not to the blank document
           standing in for "nothing open yet". */
        if (restoring_last_page && !native_home_boot
            && startup_recovery->scroll_y > 0) {
            (void) browser_engine_scroll_by(
                browser->engine, startup_recovery->scroll_y);
            psp_ui_show_status(&process->presentation.ui, "LAST PAGE RESTORED", 180);
        }
        psp_profile_record_current(
            browser->profile, &browser->profile_store, browser->engine,
            (uint64_t) sceKernelGetSystemTimeWide());
        (void) psp_recovery_record_current(
            browser->profile, process->storage.recovery, browser->engine);
    } else {
        if (restoring_last_page) {
            (void) browser_recovery_clear(process->storage.recovery);
            printf("tilefinch-recovery: failed page cleared\n");
        }
        static const char failure_html[] =
            "<!doctype html><meta name=viewport "
            "content=\"width=device-width,initial-scale=1\">"
            "<title>Page unavailable</title>"
            "<style>*{box-sizing:border-box}body{font:16px sans-serif;"
            "margin:0;padding:22px;background:#f4f7f7;color:#18202a}"
            ".card{max-width:420px;padding:20px;background:#fff;border:1px "
            "solid #d5dfe2;border-radius:12px}h1{font-size:25px;margin:0 0 "
            "8px}p{line-height:1.4;color:#52636c}.actions{display:flex;gap:"
            "9px;margin-top:16px}.action{padding:10px 14px;border-radius:8px;"
            "background:#123040;color:#fff;text-decoration:none}.primary{"
            "background:#34d3b0;color:#08211d}</style><div class=card>"
            "<h1>Page unavailable</h1><p>Tilefinch could not open this page."
            " Check the connection, try again, or return home.</p>"
            "<div class=actions><a autofocus class=\"action primary\" "
            "href=\"tilefinch://retry\">Try again</a><a class=action "
            "href=\"tilefinch://home\">Go home</a></div></div>";
        static const char stopped_html[] =
            "<!doctype html><meta name=viewport "
            "content=\"width=device-width,initial-scale=1\">"
            "<title>Page load stopped</title>"
            "<style>*{box-sizing:border-box}body{font:16px sans-serif;"
            "margin:0;padding:22px;background:#f4f7f7;color:#18202a}"
            ".card{padding:20px;background:#fff;border:1px solid #d5dfe2;"
            "border-radius:12px}h1{font-size:25px;margin:0 0 8px}p{color:"
            "#52636c}.action{display:inline-block;margin:12px 7px 0 0;"
            "padding:10px 14px;border-radius:8px;background:#34d3b0;color:"
            "#08211d;text-decoration:none}</style><div class=card>"
            "<h1>Loading stopped</h1><p>The page stayed unchanged. You can "
            "retry when ready or return home.</p><a autofocus class=action "
            "href=\"tilefinch://retry\">Try again</a><a class=action "
            "href=\"tilefinch://home\">Go home</a></div>";
        const char *fallback_html =
            initial_load_stopped ? stopped_html : failure_html;
        size_t fallback_length = initial_load_stopped
            ? sizeof(stopped_html) - 1u : sizeof(failure_html) - 1u;
        initial_error_page = browser_engine_commit_html(
            browser->engine, startup_url,
            fallback_html, fallback_length, true)
            && browser_engine_refresh_shell(browser->engine);
        loaded = initial_error_page;
        if (initial_error_page) {
            (void) psp_engine_views_refresh(engine_views, browser->engine);
            psp_ui_show_status(
                &process->presentation.ui, initial_load_stopped
                    ? "PAGE LOAD STOPPED"
                    : "PAGE FAILED - START OPENS ADDRESS",
                300);
        }
    }
    return (PspBootNavigation) {
        .loaded = loaded,
        .download_offer = initial_download_offer,
        .error_page = initial_error_page,
        .load_started_us = load_started_us
    };
}
