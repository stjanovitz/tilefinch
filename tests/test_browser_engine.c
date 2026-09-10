#include "browser_engine_test_support.h"
#include "../src/tilefinch_test_faults.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/platform.h"
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/site_adapter.h"
#include "tilefinch/youtube_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    size_t selector_calls;
    size_t section_index;
    size_t writer_calls;
    size_t writer_section;
    ScriptRemoteNodeWriteKind writer_kind;
    char writer_key[96];
    char writer_name[64];
    char writer_value[64];
} BackingProbe;

static char *read_test_file(const char *path, size_t *length)
{
    if (length != NULL) *length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *data = malloc((size_t) end + 1u);
    if (data == NULL
        || fread(data, 1, (size_t) end, file) != (size_t) end) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    data[end] = '\0';
    if (length != NULL) *length = (size_t) end;
    return data;
}

static bool write_large_youtube_replay_configured(
    char directory[128], size_t *body_length, const char *url,
    bool include_identity)
{
    static const char identity[] =
        "<script>ytcfg.set({\"INNERTUBE_API_KEY\":\"fixture-key\","
        "\"INNERTUBE_CONTEXT_CLIENT_VERSION\":\"fixture-version\","
        "\"VISITOR_DATA\":\"fixture-visitor\"});</script>";
    static const char prefix[] =
        "<!doctype html><script>var ytInitialData = '{";
    static const char contents[] =
        "\"contents\":[{\"videoRenderer\":{\"title\":{\"runs\":[{\"text\":"
        "\"Ratchet Video\"}]},\"videoId\":\"abc_DEF-123\"}},"
        "{\"videoRenderer\":{\"title\":{\"runs\":[{\"text\":"
        "\"Second Ratchet Video\"}]},\"videoId\":\"def_GHI-456\"}},"
        "{\"videoRenderer\":{\"title\":{\"runs\":[{\"text\":"
        "\"Third Ratchet Video\"}]},\"videoId\":\"TFTEST00003\"}},"
        "{\"videoRenderer\":{\"title\":{\"runs\":[{\"text\":"
        "\"Fourth Ratchet Video\"}]},\"videoId\":\"TFTEST00004\"}},"
        "{\"padding\":\"";
    static const char suffix[] = "\"}]}'</script>";
    static const char watch_header[] =
        "\"videoDescriptionHeaderRenderer\":{\"views\":{\"runs\":[{\"text\":\"23 views\"}]}},";
    static const char watch_metadata[] =
        "<script>var ytInitialPlayerResponse={\"videoDetails\":{"
        "\"videoId\":\"TFTEST00001\",\"title\":\"Watch fixture\","
        "\"shortDescription\":\"Full description retained unchanged.\","
        "\"lengthSeconds\":\"19\"},\"microformat\":{"
        "\"playerMicroformatRenderer\":{\"publishDate\":\"2005-04-23\"}}};</script>";
    bool watch = strstr(url, "/watch?") != NULL;
    snprintf(directory, 128, "/tmp/tilefinch-youtube-large-XXXXXX");
    if (mkdtemp(directory) == NULL) return false;
    char path[256];
    snprintf(path, sizeof(path), "%s/0000.body", directory);
    FILE *body = fopen(path, "wb");
    size_t target = 768u * 1024u;
    size_t identity_length = include_identity ? sizeof(identity) - 1u : 0;
    size_t header_length = watch ? sizeof(watch_header) - 1u : 0;
    size_t metadata_length = watch ? sizeof(watch_metadata) - 1u : 0;
    bool ok = body != NULL
        && fwrite(identity, 1, identity_length, body) == identity_length
        && fwrite(prefix, 1, sizeof(prefix) - 1u, body)
               == sizeof(prefix) - 1u
        && fwrite(watch_header, 1, header_length, body) == header_length
        && fwrite(contents, 1, sizeof(contents) - 1u, body) == sizeof(contents) - 1u;
    char padding[4096];
    memset(padding, 'a', sizeof(padding));
    size_t padding_length =
        target - identity_length - (sizeof(prefix) - 1u) - (sizeof(contents) - 1u)
        - (sizeof(suffix) - 1u) - header_length - metadata_length;
    for (size_t written = 0; ok && written < padding_length;) {
        size_t amount = padding_length - written;
        if (amount > sizeof(padding)) amount = sizeof(padding);
        ok = fwrite(padding, 1, amount, body) == amount;
        written += amount;
    }
    ok = ok && fwrite(suffix, 1, sizeof(suffix) - 1u, body)
                   == sizeof(suffix) - 1u
        && fwrite(watch_metadata, 1, metadata_length, body) == metadata_length
        && fclose(body) == 0;
    if (!ok) return false;
    *body_length = target;

    snprintf(path, sizeof(path), "%s/0000.meta", directory);
    FILE *meta = fopen(path, "wb");
    ok = meta != NULL && fprintf(
        meta,
        "psp-http-trace=1\nmethod=GET\n"
        "url=%s\n"
        "success=1\nasync-delay-pumps=0\nexternal-cancel=0\n"
        "transport-timeout=0\nerror=\nstatus=200\nlength=%zu\n"
        "effective-url=%s\n"
        "content-type=text/html; charset=utf-8\netag=\nlast-modified=\n"
        "cf-mitigated=\naccept-ch=\ncritical-ch=\nserver=fixture-youtube\n"
        "cf-ray=\nset-cookie-count=0\n",
        url, target, url) > 0 && fclose(meta) == 0;
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    FILE *trace = ok ? fopen(path, "wb") : NULL;
    return trace != NULL && fprintf(
        trace, "psp-http-trace-clock=1\norigin-ms=1700000000000\n"
               "capture-complete=yes\nrecord-count=1\n") > 0
        && fclose(trace) == 0;
}

static bool write_large_youtube_replay(char directory[128], size_t *body_length)
{
    return write_large_youtube_replay_configured(directory, body_length,
        "https://m.youtube.com/results?search_query=ratchet", false);
}

static void remove_large_youtube_replay(const char *directory)
{
    char path[256];
    static const char *const files[] = {
        "0000.body", "0000.meta", "trace.meta"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        (void) unlink(path);
    }
    (void) rmdir(directory);
}

static bool write_history_form_replay(
    char directory[128], const char *html, size_t html_length,
    const char *effective_url)
{
    snprintf(directory, 128, "/tmp/tilefinch-history-form-XXXXXX");
    if (mkdtemp(directory) == NULL) return false;
    char path[256];
    snprintf(path, sizeof(path), "%s/0000.body", directory);
    FILE *body = fopen(path, "wb");
    bool ok = body != NULL
        && fwrite(html, 1, html_length, body) == html_length
        && fclose(body) == 0;
    snprintf(path, sizeof(path), "%s/0000.meta", directory);
    FILE *meta = ok ? fopen(path, "wb") : NULL;
    ok = meta != NULL && fprintf(
        meta,
        "psp-http-trace=1\nmethod=GET\n"
        "url=https://history-form.test/search\n"
        "success=1\nasync-delay-pumps=0\nexternal-cancel=0\n"
        "transport-timeout=0\nerror=\nstatus=200\nlength=%zu\n"
        "effective-url=%s\n"
        "content-type=text/html; charset=utf-8\netag=\nlast-modified=\n"
        "cf-mitigated=\naccept-ch=\ncritical-ch=\nserver=fixture\n"
        "cf-ray=\nset-cookie-count=0\n",
        html_length, effective_url) > 0 && fclose(meta) == 0;
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    FILE *trace = ok ? fopen(path, "wb") : NULL;
    return trace != NULL && fprintf(
        trace, "psp-http-trace-clock=1\norigin-ms=1700000000000\n"
               "capture-complete=yes\nrecord-count=1\n") > 0
        && fclose(trace) == 0;
}

typedef struct {
    size_t events;
    uint64_t last_sequence;
    TilefinchDiagnosticCode last_code;
    char last_name[TILEFINCH_DIAGNOSTIC_NAME_LIMIT];
    bool valid;
} DiagnosticProbe;

static void capture_diagnostic(void *opaque,
                               const TilefinchDiagnosticEvent *event)
{
    DiagnosticProbe *probe = opaque;
    if (probe == NULL || event == NULL) return;
    if (event->sequence <= probe->last_sequence) probe->valid = false;
    probe->events++;
    probe->last_sequence = event->sequence;
    probe->last_code = event->code;
    snprintf(probe->last_name, sizeof(probe->last_name), "%s", event->name);
}

uint64_t frame_checksum(const uint16_t *pixels, size_t pixel_count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < pixel_count; i++) {
        hash ^= pixels[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

typedef struct {
    size_t calls;
    uint64_t last_checksum;
} ProvisionalPresentProbe;

static bool capture_provisional_present(
    void *context, const uint16_t *pixels, size_t width,
    size_t height, size_t stride_pixels)
{
    ProvisionalPresentProbe *probe = context;
    if (probe == NULL || pixels == NULL || width == 0 || height == 0
        || stride_pixels != width || width > SIZE_MAX / height) {
        return false;
    }
    probe->calls++;
    probe->last_checksum = frame_checksum(pixels, width * height);
    return true;
}

static bool backing_selector_lookup(
    void *opaque, const char *selector, size_t length,
    size_t *section_index, char tag_name[32], char identifier[129],
    char stable_key[96])
{
    BackingProbe *probe = opaque;
    probe->selector_calls++;
    bool local_probe = length == 6 && memcmp(selector, "#probe", 6) == 0;
    bool deferred_probe = (length == 9
        && memcmp(selector, "#deferred", 9) == 0)
        || (length == 5 && memcmp(selector, "aside", 5) == 0);
    if (!local_probe && !deferred_probe) return false;
    *section_index = probe->section_index;
    snprintf(tag_name, 32, "div");
    snprintf(identifier, 129, "%s",
             deferred_probe ? "deferred" : "probe");
    snprintf(stable_key, 96, "id:%s",
             deferred_probe ? "deferred" : "probe");
    return true;
}

static bool backing_node_write(
    void *opaque, const char *stable_key, size_t stable_key_length,
    size_t section_index, ScriptRemoteNodeWriteKind kind,
    const char *name, size_t name_length,
    const char *value, size_t value_length)
{
    BackingProbe *probe = opaque;
    if (probe == NULL || stable_key == NULL
        || stable_key_length >= sizeof(probe->writer_key)
        || name_length >= sizeof(probe->writer_name)
        || value_length >= sizeof(probe->writer_value)) return false;
    probe->writer_calls++;
    probe->writer_section = section_index;
    probe->writer_kind = kind;
    memcpy(probe->writer_key, stable_key, stable_key_length);
    probe->writer_key[stable_key_length] = '\0';
    if (name != NULL) memcpy(probe->writer_name, name, name_length);
    probe->writer_name[name_length] = '\0';
    if (value != NULL) memcpy(probe->writer_value, value, value_length);
    probe->writer_value[value_length] = '\0';
    return true;
}

#include "suites/browser_engine_lifecycle.inc"
#include "suites/browser_engine_navigation.inc"
#include "suites/browser_engine_adapters.inc"
#include "suites/browser_engine_teardown.inc"

int main(int argc, char **argv)
{
    if ((argc == 4 || argc == 5)
        && (strcmp(argv[1], "--native-navigation-replay") == 0
            || strcmp(argv[1], "--native-navigation-strict-replay") == 0)) {
        char *end = NULL;
        unsigned long runs = argc == 5 ? strtoul(argv[4], &end, 10) : 1;
        if (runs < 1 || runs > 30 || (argc == 5 && (end == argv[4] || *end))) return 1;
        ProfileReplayMode mode = strcmp(argv[1], "--native-navigation-strict-replay") == 0
            ? PROFILE_REPLAY_NATIVE_STRICT : PROFILE_REPLAY_NATIVE;
        for (unsigned long run = 0; run < runs; run++)
            CHECK(profile_staged_font_replay(argv[2], argv[3], 0, mode, NULL, 0) == 0);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--font-staging-replay") == 0)
        return profile_staged_font_replay(argv[2], argv[3], 0, false, NULL, false);
    if (argc == 4 && strcmp(argv[1], "--navigation-staging-replay") == 0)
        return profile_staged_font_replay(argv[2], argv[3], 0, true, NULL, false);
    if (argc == 4 && strcmp(argv[1], "--navigation-settle-replay") == 0)
        return profile_staged_font_replay(argv[2], argv[3], 0, true, NULL, true);
    if (argc == 5 && strcmp(argv[1], "--navigation-settle-replay") == 0) {
        char *end = NULL;
        unsigned long ticks = strtoul(argv[4], &end, 10);
        if (end == argv[4] || *end != '\0' || ticks < 2 || ticks > 4096)
            return 1;
        script_runtime_configure_deterministic_replay(true, 7);
        int status = profile_staged_font_replay(
            argv[2], argv[3], 0, true, NULL, (unsigned) ticks);
        script_runtime_configure_deterministic_replay(false, 7);
        return status;
    }
    if (argc == 4 && strcmp(argv[1], "--search-journey-replay") == 0)
        return profile_staged_font_replay(argv[2], argv[3], 0, true, "psp", false);
    if (argc == 5 && (strcmp(argv[1], "--font-interrupt-replay") == 0
            || strcmp(argv[1], "--navigation-interrupt-replay") == 0)) {
        char *end = NULL;
        unsigned long delay = strtoul(argv[4], &end, 10);
        if (end == argv[4] || *end != '\0' || delay == 0 || delay > 1000000u)
            return 1;
        return profile_staged_font_replay(argv[2], argv[3], delay,
            strcmp(argv[1], "--navigation-interrupt-replay") == 0, NULL, false);
    }
    if (argc == 2 && strcmp(argv[1], "--font-staging-only") == 0)
        return test_staged_optional_fonts_relayout_before_repaint();
    if (argc == 2 && strcmp(argv[1], "--native-text-sync-only") == 0)
        return test_native_text_sync_does_not_relayout_twice();
    if (argc == 2 && strcmp(argv[1], "--computed-style-layout-only") == 0)
        return test_computed_paint_style_does_not_force_layout();
    if (argc == 2 && strcmp(argv[1], "--deferred-startup-only") == 0)
        return test_deferred_startup_journey();
    if (argc == 2 && strcmp(argv[1], "--interaction-journey-only") == 0)
        return test_loading_interaction_journey();
    if (argc == 2 && strcmp(argv[1], "--script-free-journey-only") == 0)
        return test_script_free_browsing_journey();
    if (argc == 2 && strcmp(argv[1], "--background-interruption-only") == 0)
        return test_background_interruption_journey();
    if (argc == 2 && strcmp(argv[1], "--provider-navigation-only") == 0) {
        return test_cooperative_site_adapter_navigation();
    }
    if (argc == 2 && strcmp(argv[1], "--pointer-search-only") == 0)
        return profile_staged_font_replay(TILEFINCH_TEST_SOURCE_DIR
            "/tests/fixtures/http-pointer-search", "https://search-journey.test/",
            0, true, "psp", false);
    CHECK(test_engine_lifecycle() == 0);
    CHECK(profile_staged_font_replay(TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-pointer-search", "https://search-journey.test/",
        0, true, "psp", false) == 0);
    CHECK(test_loading_interaction_journey() == 0);
    CHECK(test_script_free_browsing_journey() == 0);
    CHECK(test_background_interruption_journey() == 0);
    CHECK(test_deferred_startup_journey() == 0);
    CHECK(test_deferred_image_publication_survives_rebuild() == 0);
    CHECK(test_deferred_images_skip_oversized_pages() == 0);
    CHECK(test_same_document_and_script_free_defaults() == 0);
    CHECK(test_same_document_event_mutations_settle_immediately() == 0);
    CHECK(test_contenteditable_relayout_refusal_retires_shell() == 0);
    CHECK(test_author_relayout_refusal_retires_shell() == 0);
    CHECK(test_history_restores_bounded_form_state() == 0);
    CHECK(test_post_mutation_document_refresh_refusal_retires_shell() == 0);
    CHECK(test_page_video_activation() == 0);
    CHECK(test_page_video_data_candidate_activation() == 0);
    CHECK(test_page_audio_activation() == 0);
    CHECK(test_structured_audio_preview_activation() == 0);
    CHECK(test_nomodule_capability_suppression() == 0);
    CHECK(test_staged_optional_fonts_relayout_before_repaint() == 0);
    CHECK(test_native_text_sync_does_not_relayout_twice() == 0);
    CHECK(test_computed_paint_style_does_not_force_layout() == 0);
    CHECK(test_deferred_image_relayout_requests_repaint() == 0);
    CHECK(test_committed_document_glyph_script_hints() == 0);
    CHECK(test_responsive_navigation_convergence() == 0);
    CHECK(test_animated_canvas_follows_transactional_page_move() == 0);
    CHECK(test_scrollable_provisional_navigation() == 0);
    CHECK(test_youtube_localized_watch_metadata() == 0);
    CHECK(test_youtube_cooperative_build_convergence() == 0);
    CHECK(test_youtube_missing_initial_data_terminates() == 0);
    CHECK(test_google_search_compatibility_adapter() == 0);
    CHECK(test_reader_presentation_adapter() == 0);
    CHECK(test_blank_reader_frontend_recovery() == 0);
    CHECK(test_declared_media_card_recovery() == 0);
    CHECK(test_basic_view_admission_and_presentation() == 0);
    CHECK(test_basic_view_native_root_provenance() == 0);
    CHECK(test_basic_view_handle_capacity_is_transactional() == 0);
    CHECK(test_basic_view_bypasses_author_delegated_actions() == 0);
    /* Native extracted-view provenance and form-safety boundaries. */
    CHECK(test_basic_view_form_safety_edges() == 0);
    CHECK(test_basic_view_multiple_select_refuses_transactionally() == 0);
    CHECK(test_reader_activation_freezes_exact_native_root() == 0);
    CHECK(test_reader_activation_retires_frame_without_main_runtime() == 0);
    CHECK(test_reader_manual_image_aliases() == 0);
    CHECK(test_reader_alias_relocation_rebinds_declined_auto_layout() == 0);
    CHECK(test_reader_deferred_images_alias_before_relayout() == 0);
    CHECK(test_cooperative_site_adapter_navigation() == 0);
    CHECK(test_headless_repeated_teardown() == 0);
    CHECK(test_single_active_engine() == 0);
    CHECK(test_late_candidate_failure_transaction() == 0);
    CHECK(test_allocator_owner_conflict_does_not_poison_engine() == 0);
    CHECK(test_incumbent_teardown_cannot_run_author_javascript() == 0);

    BrowserConfig policy_config;
    browser_config_init(&policy_config, NULL);
    policy_config.memory_limit = 32u * MIB;
    policy_config.javascript.enabled = true;
    policy_config.javascript.document_scripts_enabled = true;
    char policy_error[128] = {0};
    BrowserEngine *policy_engine = browser_engine_create(
        &policy_config, policy_error, sizeof(policy_error));
    CHECK(policy_engine != NULL
          && browser_engine_set_javascript_enabled(policy_engine, false)
          && !browser_engine_config(policy_engine)->javascript.enabled
          && !browser_engine_config(policy_engine)
                  ->javascript.document_scripts_enabled
          && browser_engine_set_javascript_enabled(policy_engine, true)
          && browser_engine_config(policy_engine)->javascript.enabled
          && browser_engine_config(policy_engine)
                  ->javascript.document_scripts_enabled);
    browser_engine_destroy(policy_engine);

    BrowserConfig invalid;
    browser_config_init(&invalid, NULL);
    invalid.history_capacity = 0;
    char error[128] = {0};
    CHECK(!browser_config_validate(&invalid, error, sizeof(error))
          && error[0] != '\0'
          && browser_engine_create(&invalid, error, sizeof(error)) == NULL);
    browser_config_init(&invalid, NULL);
    memset(invalid.device.name, 'x', sizeof(invalid.device.name));
    CHECK(!browser_config_validate(&invalid, error, sizeof(error)));
    browser_config_init(&invalid, NULL);
    invalid.memory_limit = SIZE_MAX;
    invalid.non_page_memory_reserve = 1;
    CHECK(!browser_config_validate(&invalid, error, sizeof(error)));
    browser_config_init(&invalid, NULL);
    invalid.non_page_memory_reserve =
        BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE - 1u;
    CHECK(!browser_config_validate(&invalid, error, sizeof(error)));
    browser_config_init(&invalid, NULL);
    invalid.declared_css_width = 480;
    CHECK(!browser_config_validate(&invalid, error, sizeof(error)));
    browser_config_init(&invalid, NULL);
    invalid.declared_css_width = 480;
    invalid.declared_css_height = 4097;
    CHECK(!browser_config_validate(&invalid, error, sizeof(error)));
    browser_config_init(&invalid, NULL);
    invalid.fonts.enabled = true;
    invalid.fonts.maximum_total_bytes = 1024;
    CHECK(!browser_config_validate(&invalid, error, sizeof(error)));
    browser_config_init(&invalid, NULL);
    CHECK(!browser_config_set_font_paths(
              &invalid, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 1024));
    CHECK(strcmp(tilefinch_diagnostic_severity_name(
                     (TilefinchDiagnosticSeverity) -1), "unknown") == 0
          && strcmp(tilefinch_diagnostic_subsystem_name(
                        (TilefinchDiagnosticSubsystem) -1), "unknown") == 0
          && strcmp(tilefinch_diagnostic_code_name(
                        (TilefinchDiagnosticCode) -1), "unknown") == 0);
    CHECK(tilefinch_error_is_certificate_verification_failure(
              "YouTube page fetch failed: mbedTLS: The certificate is not "
              "correctly signed by the trusted CA")
          && tilefinch_error_is_certificate_verification_failure(
              "SSL peer certificate verification failed")
          && tilefinch_error_is_certificate_verification_failure(
              "SSL certificate problem: unable to get local issuer "
              "certificate")
          && tilefinch_error_is_certificate_verification_failure(
              "TLS certificate is not correctly signed")
          && tilefinch_error_is_certificate_verification_failure(
              "X.509 certificate is not yet valid")
          && !tilefinch_error_is_certificate_verification_failure(
              "page mentions a certificate")
          && !tilefinch_error_is_certificate_verification_failure(
              "request deadline expired"));
    PspTimeFields rtc = {
        .year = 1970, .month = 1, .day = 1
    };
    time_t rtc_epoch = (time_t) -1;
    CHECK(psp_time_classify_utc(NULL, &rtc_epoch) == PSP_TIME_UNREADABLE
          && rtc_epoch == 0);
    rtc.year = 1969;
    CHECK(psp_time_classify_utc(&rtc, &rtc_epoch)
              == PSP_TIME_OUT_OF_TIME_T_RANGE
          && rtc_epoch == 0);
    rtc.year = 1970;
    CHECK(psp_time_classify_utc(&rtc, &rtc_epoch) == PSP_TIME_OK
          && rtc_epoch == 0);
    rtc = (PspTimeFields) {
        .year = 2037, .month = 12, .day = 31,
        .hour = 23, .minute = 59, .second = 59
    };
    CHECK(psp_time_classify_utc(&rtc, &rtc_epoch) == PSP_TIME_OK
          && (uint64_t) rtc_epoch == UINT64_C(2145916799));
    rtc.year = 2038;
    CHECK(psp_time_classify_utc(&rtc, &rtc_epoch)
              == PSP_TIME_OUT_OF_TIME_T_RANGE);
    rtc.year = 2040;
    CHECK(psp_time_classify_utc(&rtc, &rtc_epoch)
              == PSP_TIME_OUT_OF_TIME_T_RANGE);
    rtc = (PspTimeFields) {.year = 2028, .month = 2, .day = 30};
    CHECK(psp_time_classify_utc(&rtc, &rtc_epoch)
              == PSP_TIME_INVALID_FIELDS);
    rtc = (PspTimeFields) {
        .year = 2028, .month = 2, .day = 29, .microsecond = 1000000
    };
    CHECK(psp_time_classify_utc(&rtc, &rtc_epoch)
              == PSP_TIME_INVALID_FIELDS
          && strcmp(psp_time_status_name(PSP_TIME_UNREADABLE),
                    "unreadable") == 0);

    CHECK(tilefinch_tls_verification_guidance(
              0, false, PSP_TIME_UNREADABLE)
              == TILEFINCH_TLS_GUIDANCE_TIME
          && tilefinch_tls_verification_guidance(
                 0, false, PSP_TIME_OK)
              == TILEFINCH_TLS_GUIDANCE_DETAILS
          && tilefinch_tls_verification_guidance(
                 0, true, PSP_TIME_OK)
              == TILEFINCH_TLS_GUIDANCE_DETAILS
          && tilefinch_tls_verification_guidance(
                 TILEFINCH_TLS_VERIFY_EXPIRED, true, PSP_TIME_OK)
              == TILEFINCH_TLS_GUIDANCE_TIME
          && tilefinch_tls_verification_guidance(
                 TILEFINCH_TLS_VERIFY_FUTURE, true, PSP_TIME_OK)
              == TILEFINCH_TLS_GUIDANCE_TIME
          && tilefinch_tls_verification_guidance(
                 TILEFINCH_TLS_VERIFY_HOSTNAME
                     | TILEFINCH_TLS_VERIFY_NOT_TRUSTED,
                 true, PSP_TIME_OK)
              == TILEFINCH_TLS_GUIDANCE_REDIRECTED
          && tilefinch_tls_verification_guidance(
                 TILEFINCH_TLS_VERIFY_NOT_TRUSTED, true, PSP_TIME_OK)
              == TILEFINCH_TLS_GUIDANCE_UNTRUSTED
          && tilefinch_tls_verification_guidance(
                 TILEFINCH_TLS_VERIFY_BAD_KEY, true, PSP_TIME_OK)
              == TILEFINCH_TLS_GUIDANCE_UNSUPPORTED);
    puts("tilefinch-browser-engine-tests: all checks passed");
    return 0;
}
