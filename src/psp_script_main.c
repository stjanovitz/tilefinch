/* Tilefinch PSP browser EBOOT.
 *
 * Boot establishes scanout before optional storage and network work, then
 * hands control to the resident interactive loop. The same executable also
 * carries opt-in replay and hardware-validation modes; release builds compile
 * their high-volume logging out. boot.cfg parsing and defaults belong to
 * psp_boot_config.c, while the lifecycle and ownership model is documented in
 * docs/ARCHITECTURE.md.
 */

#include "psp_app/psp_app_internal.h"
#include "tilefinch/psp_threads.h"
#include "tilefinch/psp_voice_component_session.h"
#include "tilefinch/voice_component.h"
#include "tilefinch_compiler.h"
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && defined(TILEFINCH_PSP_LIVE_NETWORK)
#include "psp_update_e2e.h"
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
#include "tilefinch/psp_media_fixture.h"
#include "tilefinch/psp_media_range_probe.h"
#include "tilefinch/psp_raster_fixture.h"
/* After the tilefinch headers: the media policy header is written against
   fixed-width types the public headers have already established. */
#include "media_backend_psp_policy.h"
#endif

/* Shipping builds do not open or rotate the full validation log. Keep one
   bounded, overwrite-only failure snapshot instead: no boot I/O and no hot
   path writes, but a network/page failure on hardware remains actionable. */
static const TilefinchInstallPaths *psp_failure_report_paths;

typedef struct {
    bool transport_present;
    long transport_code;
    long tls_verify_result;
    bool transport_timed_out;
    bool tls12_compatibility_retry;
    char tls_version[16];
    bool network_present;
    int network_status;
    int network_failure_phase;
    int requested_profile;
    int selected_profile;
    int apctl_state;
    int wlan_switch_state;
    int wlan_power_state;
    uint32_t profile_query_success_mask;
    uint32_t profile_query_failure_mask;
    uint32_t initialization_adopted_mask;
    unsigned profile_security_type;
    bool profile_static_ip;
    bool profile_manual_dns;
    bool profile_uses_proxy;
    bool profile_fallback_used;
    size_t network_pump_calls;
    uint64_t network_elapsed_us;
    uint64_t network_maximum_pump_us;
    int network_maximum_pump_phase;
} PspFailureReportData;

static bool psp_write_failure_report_data(
    const char *stage, const char *detail, const char *url,
    long http_status, int native_result,
    const PspFailureReportData *diagnostics)
{
    PspFailureReportData absent = {0};
    if (diagnostics == NULL) diagnostics = &absent;
    if (psp_failure_report_paths == NULL) return false;
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    char temporary[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!tilefinch_install_data_path(
            psp_failure_report_paths, "tilefinch-last-error.txt",
            path, sizeof(path))
        || !tilefinch_install_data_path(
            psp_failure_report_paths, "tilefinch-last-error.tmp",
            temporary, sizeof(temporary))) return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    unsigned long long unix_time = (unsigned long long) time(NULL);
#else
    unsigned long long unix_time = 0;
#endif
    bool okay = fprintf(
        file,
        "tilefinch-device-error-v2\n"
        "version=%s\nrelease-sequence=%llu\n"
        "stage=%s\nhttp=%ld\nnative=0x%08x\n"
        "uptime-us=%llu\nunix-time=%llu\nfree=%d\nlargest=%d\n"
        "transport-present=%d\ncurl-code=%ld\ntimed-out=%d\n"
        "tls-verify=0x%08lx\ntls-version=%s\ntls12-retry=%d\n"
        "network-present=%d\nnetwork-status=%d\n"
        "network-failure-phase=%d\nprofile-requested=%d\n"
        "profile-selected=%d\nprofile-fallback=%d\napctl=%d\n"
        "wlan-switch=%d\nwlan-power=%d\nprofile-query-ok=0x%08x\n"
        "profile-query-failed=0x%08x\nprofile-adopted=0x%08x\n"
        "profile-security=%u\nprofile-static-ip=%d\n"
        "profile-manual-dns=%d\nprofile-proxy=%d\nnetwork-pumps=%zu\n"
        "network-elapsed-us=%llu\nnetwork-max-pump-us=%llu\n"
        "network-max-pump-phase=%d\nurl=%.2047s\ndetail=%.511s\n",
        TILEFINCH_VERSION_STRING,
        (unsigned long long) TILEFINCH_RELEASE_SEQUENCE,
        stage == NULL ? "unknown" : stage, http_status,
        (unsigned) native_result, (unsigned long long) now_us, unix_time,
        sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize(),
        diagnostics->transport_present ? 1 : 0,
        diagnostics->transport_code,
        diagnostics->transport_timed_out ? 1 : 0,
        (unsigned long) diagnostics->tls_verify_result,
        diagnostics->tls_version[0] == '\0'
            ? "absent" : diagnostics->tls_version,
        diagnostics->tls12_compatibility_retry ? 1 : 0,
        diagnostics->network_present ? 1 : 0,
        diagnostics->network_status,
        diagnostics->network_failure_phase,
        diagnostics->requested_profile,
        diagnostics->selected_profile,
        diagnostics->profile_fallback_used ? 1 : 0,
        diagnostics->apctl_state,
        diagnostics->wlan_switch_state,
        diagnostics->wlan_power_state,
        (unsigned) diagnostics->profile_query_success_mask,
        (unsigned) diagnostics->profile_query_failure_mask,
        (unsigned) diagnostics->initialization_adopted_mask,
        diagnostics->profile_security_type,
        diagnostics->profile_static_ip ? 1 : 0,
        diagnostics->profile_manual_dns ? 1 : 0,
        diagnostics->profile_uses_proxy ? 1 : 0,
        diagnostics->network_pump_calls,
        (unsigned long long) diagnostics->network_elapsed_us,
        (unsigned long long) diagnostics->network_maximum_pump_us,
        diagnostics->network_maximum_pump_phase,
        url == NULL ? "" : url, detail == NULL ? "" : detail) > 0;
    okay = fclose(file) == 0 && okay;
    if (!okay) {
        (void) remove(temporary);
        return false;
    }
    /* PSP FAT cannot rename over an existing destination. This artifact is
       advisory, so prefer the latest complete snapshot without paying for a
       boot-time rotation or device sync. */
    if (remove(path) != 0 && errno != ENOENT) {
        (void) remove(temporary);
        return false;
    }
    if (rename(temporary, path) != 0) {
        (void) remove(temporary);
        return false;
    }
    return true;
}

bool psp_write_failure_report(
    const char *stage, const char *detail, const char *url,
    long http_status, int native_result)
{
    return psp_write_failure_report_data(
        stage, detail, url, http_status, native_result, NULL);
}

bool psp_write_navigation_failure_report(
    const char *stage, const char *detail, const char *url,
    const NavigationSession *navigation)
{
    if (navigation == NULL) {
        return psp_write_failure_report(stage, detail, url, 0, 0);
    }
    PspFailureReportData diagnostics = {
        .transport_present = true,
        .transport_code = navigation->last_transport_code,
        .tls_verify_result = navigation->last_tls_verify_result,
        .transport_timed_out = navigation->last_transport_timed_out,
        .tls12_compatibility_retry =
            navigation->last_tls12_compatibility_retry
    };
    snprintf(diagnostics.tls_version, sizeof(diagnostics.tls_version),
             "%s", navigation->last_tls_version);
    return psp_write_failure_report_data(
        stage, detail, url, navigation->last_http_status, 0,
        &diagnostics);
}

#ifdef TILEFINCH_PSP_LIVE_NETWORK
bool psp_write_network_failure_report(const PspNetwork *network)
{
    if (network == NULL) {
        return psp_write_failure_report(
            "network-association", "network state unavailable", NULL,
            0, 0);
    }
    PspFailureReportData diagnostics = {
        .network_present = true,
        .network_status = network->status,
        .network_failure_phase = network->failure_phase,
        .requested_profile = network->requested_profile_index,
        .selected_profile = network->profile_index,
        .apctl_state = network->apctl_state,
        .wlan_switch_state = network->wlan_switch_state,
        .wlan_power_state = network->wlan_power_state,
        .profile_query_success_mask = network->profile_query_success_mask,
        .profile_query_failure_mask = network->profile_query_failure_mask,
        .initialization_adopted_mask =
            network->initialization_adopted_mask,
        .profile_security_type = network->profile_security_type,
        .profile_static_ip = network->profile_static_ip,
        .profile_manual_dns = network->profile_manual_dns,
        .profile_uses_proxy = network->profile_uses_proxy,
        .profile_fallback_used = network->profile_fallback_used,
        .network_pump_calls = network->pump_calls,
        .network_elapsed_us = network->elapsed_us,
        .network_maximum_pump_us = network->maximum_pump_us,
        .network_maximum_pump_phase = network->maximum_pump_phase
    };
    return psp_write_failure_report_data(
        "network-association",
        psp_network_status_name(network->failure_phase), NULL, 0,
        network->native_result, &diagnostics);
}
#endif

bool psp_offline_url(const char *url)
{
    static const char root[] = "https://tilefinch.local/offline";
    return url != NULL && strncmp(url, root, sizeof(root) - 1u) == 0
        && (url[sizeof(root) - 1u] == '\0'
            || url[sizeof(root) - 1u] == '/');
}

PSP_MODULE_INFO("Tilefinch", 0, 0, 1);
/* The complete device priority ladder and its measured invariants live in
   psp_threads.h; do not tune this thread independently. */
PSP_MAIN_THREAD_PRIORITY(TILEFINCH_PSP_THREAD_PRIORITY_BROWSER);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
/* QuickJS's C-stack guard assumes 2 MiB (js_runtime sets a 2048 KiB
   stack limit) and React hydration recurses deeply; the PSP default
   main-thread stack is only 256 KiB. */
PSP_MAIN_THREAD_STACK_SIZE_KB(2560);
/* A negative size tells PSPSDK to claim the largest available block; its
   magnitude is only a sentinel and does not reserve that many KiB. Keep the
   actual late-module reserve explicit: COMMON/INET plus AVCodec,
   Audiocodec AAC working memory, and mpeg_vsh preparation after newlib first
   initializes its heap. */
PSP_HEAP_SIZE_KB(-1);
PSP_HEAP_THRESHOLD_SIZE_KB(2048);

/* Stability telemetry excludes the first two seconds after priming, seeking,
   or buffering from its steady-state skew maximum. The all-phase maximum is
   retained beside it so discontinuities remain visible rather than hidden. */
#define PSP_MEDIA_STABILITY_STEADY_DELAY_US UINT64_C(2000000)

static uint64_t psp_media_platform_now_us(void *context)
{
    (void) context;
    return (uint64_t) sceKernelGetSystemTimeWide();
}

static bool psp_media_platform_cancel_requested(void *context)
{
    (void) context;
    return psp_navigation_cancel_requested();
}

static bool psp_text_input_system_cancel_requested(void *context)
{
    (void) context;
    return !psp_lifecycle_presentation_allowed(&psp_lifecycle);
}

static size_t psp_media_platform_free_memory(void *context)
{
    (void) context;
    return (size_t) sceKernelTotalFreeMemSize();
}

static size_t psp_media_platform_maximum_free_block(void *context)
{
    (void) context;
    return (size_t) sceKernelMaxFreeMemSize();
}

static void psp_media_platform_profile_changed(
    void *context, uint64_t now_us)
{
    psp_profile_store_mark_dirty(context, now_us);
}

bool psp_request_omnibox(
    PspTextInputService *text_input, const uint16_t *engine_frame,
    PspUiState *ui, const BrowserProfile *profile,
    const char *current_url, bool use_voice, bool start_empty,
    char *destination, size_t destination_capacity)
{
    if (text_input == NULL || ui == NULL || profile == NULL
        || destination == NULL || destination_capacity == 0) return false;
    char address[PSP_TEXT_INPUT_CAPACITY + 1] = {0};
    PspTextInputRequest request = {
        .description = "Search or address",
        .initial = use_voice || start_empty
            ? "" : (current_url == NULL ? "" : current_url),
        .keyboard_url_mode = false,
        .suggest_navigation = true
    };
    bool accepted = use_voice
        ? psp_text_input_request_voice(
              text_input, engine_frame, ui, &request,
              address, sizeof(address))
        : psp_text_input_request(
              text_input, engine_frame, ui, &request,
              address, sizeof(address));
    return accepted && browser_omnibox_resolve(
        address, browser_profile_search_engine(profile),
        destination, destination_capacity);
}

bool psp_internal_action_url(const char *url, const char *name)
{
    if (url == NULL || name == NULL) return false;
    char expected[64];
    int length = snprintf(
        expected, sizeof(expected), "tilefinch://%s", name);
    if (length <= 0 || (size_t) length >= sizeof(expected)) return false;
    size_t expected_length = (size_t) length;
    return strncmp(url, expected, expected_length) == 0
        && (url[expected_length] == '\0'
            || (url[expected_length] == '/'
                && url[expected_length + 1u] == '\0'));
}

/*
 * The one place this EBOOT stops.
 *
 * A device run driven from a host has no operator standing at the PSP, so an
 * exit that lands on the XMB strands the remote loop until someone power-cycles
 * the console by hand. `exit_to=` names the EBOOT to load instead. The clean
 * exit and the halt path had that handoff; the three validation qualification
 * modes did not, and a probe run therefore always ended on the XMB -- watched
 * happening on the device with the GE present probe.
 *
 * Funnelling every exit through one function is what stops the next validation
 * mode from reintroducing it; tests/test_psp_sdk_contracts.py holds the line.
 * Callers must have finished their log first: the load replaces the process, so
 * anything left unwritten is lost.
 */
static TILEFINCH_COLD_PATH void psp_exit_console(
    const char *exit_to)
{
    /*
     * Hand the panel back in the format the next program expects to find it.
     * A loaded EBOOT calls sceDisplaySetMode and then publishes 16-bit
     * buffers of its own, so it would recover on its own frame -- but the
     * window between the load and that first publish is the whole screen
     * showing 32-bit bytes read as 565, and this costs one vblank.
     */
    (void) psp_display_video_end(&psp_display);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (exit_to != NULL && exit_to[0] != '\0')
        (void) psp_exit_handoff(exit_to);
#else
    (void) exit_to;
#endif
    sceKernelExitGame();
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static TILEFINCH_COLD_PATH int psp_run_ge_present_qualification(
    BrowserEngine *engine, Budget *budget,
    PspClockWorker *clock_worker, bool clock_worker_started,
    const char *exit_to)
{
    /*
     * The one way the graphics-engine presenter can be exercised without
     * a decoder. It supplies its own pictures, so it reaches everything
     * in that path except the firmware that would normally produce them:
     * context setup, the framebuffer address, texture format and stride,
     * the UV mapping, the wide-frame split, the completion wait, and the
     * cache handshake either side of the draw.
     */
    psp_present_boot_surface(
        PSP_UI_STARTUP_HOMEPAGE, "TESTING VIDEO PRESENTATION", 860);
    void *probe_scratch = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1,
        PSP_MEDIA_PRESENT_PROBE_SCRATCH_BYTES);
    /*
     * The probe draws into the 32-bit video surface, because that is the
     * surface a Smooth present draws into and the passthrough it checks is a
     * property of both ends being 8888. Switching here also exercises the
     * mode change itself, which the emulator runs as faithfully as a game
     * does.
     */
    bool surface_entered = psp_display_video_begin(&psp_display);
    char probe_detail[192];
    snprintf(probe_detail, sizeof(probe_detail),
             surface_entered ? "scratch unavailable"
                             : "video surface unavailable");
    PspMediaPresentProbeCase probe_cases[PSP_MEDIA_PRESENT_PROBE_CASES] = {0};
    /* Staged into the display's own EDRAM texture, which is what playback
       samples: a probe that measured a main-memory read would be timing a
       pipeline nothing runs. */
    bool probe_passed = probe_scratch != NULL && surface_entered
        && psp_media_present_ge_probe(
               probe_scratch, PSP_MEDIA_PRESENT_PROBE_SCRATCH_BYTES,
               psp_display_video_texture(&psp_display),
               PSP_DISPLAY_VIDEO_TEXTURE_BYTES,
               psp_display_video_back_buffer(&psp_display),
               probe_cases, probe_detail, sizeof(probe_detail));
    bool surface_left = psp_display_video_end(&psp_display);
    /*
     * The per-case lines, logged here rather than in the presenter. That
     * translation unit's stdout is the PSP console, which nothing collects,
     * so its own printf never reached the validation log the truth cycle
     * reads -- and the emulator runner asks for exactly these lines.
     */
    for (unsigned at = 0; at < PSP_MEDIA_PRESENT_PROBE_CASES; at++) {
        const PspMediaPresentProbeCase *probe_case = &probe_cases[at];
        if (probe_case->source_width == 0) continue;
        /* sync is the real staged draw; output-sync is the same rectangle
           from a cache-resident texture, so texture-read = sync - output-sync.
           On device that split says whether the remaining GE cost is the
           texture read (chase it) or the 8888 output write (irreducible). */
        uint64_t texture_sync_us =
            probe_case->sync_us > probe_case->output_sync_us
                ? probe_case->sync_us - probe_case->output_sync_us : 0;
        printf("tilefinch-media-present-probe: case=%dx%d stride=%d quads=%u "
               "slot=%u "
               "output=%dx%d texture=%s flat=0x%06x halves=0x%06x/0x%06x "
               "submit=%lluus sync=%lluus output-sync=%lluus "
               "texture-sync=%lluus outcome=%s\n",
               probe_case->source_width, probe_case->source_height,
               probe_case->source_stride, probe_case->quads,
               probe_case->slot,
               probe_case->output_width, probe_case->output_height,
               probe_case->staged ? "edram" : "main",
               (unsigned) probe_case->flat, (unsigned) probe_case->left,
               (unsigned) probe_case->right,
               (unsigned long long) probe_case->submit_us,
               (unsigned long long) probe_case->sync_us,
               (unsigned long long) probe_case->output_sync_us,
               (unsigned long long) texture_sync_us,
               probe_case->passed ? "pass" : "fail");
    }
    uint32_t probe_drawn = 0;
    uint32_t probe_source = 0;
    psp_media_present_ge_passthrough(&probe_drawn, &probe_source);
    printf("tilefinch-media-present-probe: event=%s drawn=0x%08x "
           "source=0x%08x passthrough=%s surface=%d/%d detail=\"%s\"\n",
           probe_passed ? "pass" : "fail",
           (unsigned) probe_drawn, (unsigned) probe_source,
           probe_drawn != 0 && (probe_drawn & UINT32_C(0x00ffffff))
                   == (probe_source & UINT32_C(0x00ffffff))
               ? "yes" : "no",
           surface_entered ? 1 : 0, surface_left ? 1 : 0,
           probe_detail);
    /*
     * Where each surface byte landed, in both worlds. The engine column must
     * repeat its own probe -- that is what passthrough means -- and the scaler
     * column records the PSP-native 5650 mapping used by Sharp and chrome.
     */
    uint32_t probe_map_engine[PSP_MEDIA_PRESENT_CHANNEL_PROBES] = {0};
    uint16_t probe_map_scaler[PSP_MEDIA_PRESENT_CHANNEL_PROBES] = {0};
    psp_media_present_ge_channel_map(probe_map_engine, probe_map_scaler);
    printf("tilefinch-media-present-probe: channel-map "
           "byte0=0x%08x/0x%04x byte1=0x%08x/0x%04x byte2=0x%08x/0x%04x\n",
           (unsigned) probe_map_engine[0], (unsigned) probe_map_scaler[0],
           (unsigned) probe_map_engine[1], (unsigned) probe_map_scaler[1],
           (unsigned) probe_map_engine[2], (unsigned) probe_map_scaler[2]);
    budget_free(budget, probe_scratch);
    psp_report_budget_counters(budget, "ge-present-probe-exit");
    browser_engine_destroy(engine);
    if (clock_worker_started)
        (void) psp_clock_worker_shutdown(clock_worker);
    printf("tilefinch-validation: outcome=%s qualification=%s\n",
           probe_passed ? "clean-exit" : "qualification-failed",
           probe_passed ? "ge-present-pass" : "ge-present-fail");
    psp_log_checkpoint("ge-present-probe-complete");
    psp_log_finish(
        probe_passed ? "ge-present-probe-pass"
                     : "ge-present-probe-fail");
    psp_exit_console(exit_to);
    return probe_passed ? 0 : 1;
}

static TILEFINCH_COLD_PATH int psp_run_csc_order_qualification(
    BrowserEngine *engine, Budget *budget,
    PspClockWorker *clock_worker, bool clock_worker_started,
    const char *exit_to)
{
    /*
     * The experiment the present probe left owed to a device.
     *
     * Smooth and Sharp disagree about which surface byte holds which colour,
     * no PSP pixel format can reconcile them, and the only remaining free
     * variable is the byte order sceMpegBaseCscAvc writes -- hypothetically
     * chosen by two mode words nothing readable here documents. This decodes
     * one picture and re-converts it under each candidate, printing what the
     * firmware wrote beside what the panel is proven to want. It is its own
     * boot mode for a reason: no playback session may ever run this, because a
     * candidate that succeeds leaves an unpresentable surface behind.
     */
    psp_present_boot_surface(
        PSP_UI_STARTUP_HOMEPAGE, "TESTING VIDEO COLOUR ORDER", 860);
    PspMediaCscOrderReport report = {0};
    char probe_error[256] = {0};
    bool probe_passed = psp_media_fixture_csc_order_probe(
        budget, &report, probe_error, sizeof(probe_error));
    /* The same boundary the embedded fixture reports: an emulator has no
       raw-NAL decoder, so it never produces the picture this repeats, and that
       is untested rather than failed. */
    bool emulator_untested = !probe_passed
        && (report.last_native_error
                == MEDIA_BACKEND_RAW_NAL_BRIDGE_UNAVAILABLE
            || strstr(probe_error, "raw-NAL") != NULL
            || strstr(probe_error, "80010002") != NULL);
    printf(
        "tilefinch-media-csc-probe: event=%s error=\"%s\" candidates=%u "
        "usable=%u refused=%u quarantine=%d native=0x%08X\n",
        probe_passed ? "hardware-pass"
            : (emulator_untested ? "emulator-untested" : "hardware-fail"),
        probe_error, report.candidates_tried, report.candidates_usable,
        report.candidates_refused, report.quarantined ? 1 : 0,
        (unsigned) report.last_native_error);
    psp_report_budget_counters(budget, "csc-order-probe-exit");
    browser_engine_destroy(engine);
    if (clock_worker_started)
        (void) psp_clock_worker_shutdown(clock_worker);
    printf("tilefinch-validation: outcome=%s qualification=%s\n",
           probe_passed || emulator_untested
               ? "clean-exit" : "qualification-failed",
           probe_passed ? "csc-order-pass"
               : (emulator_untested ? "emulator-untested"
                                    : "csc-order-fail"));
    psp_log_checkpoint("csc-order-probe-complete");
    psp_log_finish(
        probe_passed ? "csc-order-probe-pass"
            : (emulator_untested ? "csc-order-probe-untested"
                                 : "csc-order-probe-fail"));
    psp_exit_console(exit_to);
    return probe_passed || emulator_untested ? 0 : 1;
}

static TILEFINCH_COLD_PATH int psp_run_media_range_qualification(
    BrowserEngine *engine, const PspBootConfig *config,
    PspClockWorker *clock_worker, bool clock_worker_started)
{
    /*
     * The transport half of a playback session with nothing behind it.
     *
     * A range or fragment-window regression can only be seen today inside the
     * first second of a real playback session, where the resolver, the Media
     * Engine pool, mpeg_vsh and a firmware decoder are all between the reader
     * and the log. This runs the same resolve, the same two bounded windows,
     * the same moov/sidx reads and the same sequential sample reads, far
     * enough to cross fragment boundaries, and stops there. It needs the
     * network and nothing else.
     *
     * Unlike the probes above it does not destroy the engine: it runs after
     * the media session, text input and offline store have been built on the
     * engine's budget, and tearing the budget out from under them would report
     * a clean teardown this mode has not earned. The budget counters below are
     * the honest half -- they say whether the probe returned its own bytes.
     */
    Budget *budget = browser_engine_budget(engine);
    BrowserSession *session = browser_engine_session(engine);
    PspMediaRangeProbeReport report = {0};
    char probe_error[256] = {0};
    bool passed = psp_media_range_probe_run(
        budget, session, config->url, &report,
        probe_error, sizeof(probe_error));
    printf(
        "tilefinch-media-range-probe: event=%s resolved=%d split=%d "
        "video-boundaries=%u audio-boundaries=%u error=\"%s\"\n",
        passed ? "pass" : "fail", report.resolved ? 1 : 0,
        report.split ? 1 : 0, report.video.fragment_boundaries,
        report.audio.fragment_boundaries, probe_error);
    psp_report_budget_counters(budget, "media-range-probe-exit");
    if (clock_worker_started)
        (void) psp_clock_worker_shutdown(clock_worker);
    printf("tilefinch-validation: outcome=%s qualification=%s\n",
           passed ? "clean-exit" : "qualification-failed",
           passed ? "media-range-pass" : "media-range-fail");
    psp_log_checkpoint("media-range-probe-complete");
    psp_log_finish(
        passed ? "media-range-probe-pass" : "media-range-probe-fail");
    psp_exit_console(config->exit_to);
    return passed ? 0 : 1;
}

static TILEFINCH_COLD_PATH int psp_run_raster_qualification(
    BrowserEngine *engine, Budget *budget, const char *argv0,
    PspGlyphComponentSession *glyph_component,
    PspClockWorker *clock_worker, bool clock_worker_started,
    const char *exit_to)
{
    psp_present_boot_surface(
        PSP_UI_STARTUP_HOMEPAGE, "TESTING PAGE RASTER", 860);
    PspRasterFixtureReport report = {0};
    char error[256] = {0};
    bool passed = psp_raster_fixture_run(
        engine, &report, error, sizeof(error));
    unsigned pack_reads = 0;
    bool regional_requested = false;
    bool emoji_requested = false;
    if (passed && glyph_component != NULL
        && glyph_component->provider != NULL) {
        uint32_t regional_key = 0, emoji_key = 0;
        unsigned width = 0;
        regional_requested =
            tilefinch_glyph_provider_has_codepoint(
                glyph_component->provider, 0x6f22u,
                &regional_key, &width)
            && tilefinch_glyph_provider_key_pending(
                glyph_component->provider, regional_key);
        emoji_requested =
            tilefinch_glyph_provider_has_codepoint(
                glyph_component->provider, 0x1f600u,
                &emoji_key, &width)
            && tilefinch_glyph_provider_key_pending(
                glyph_component->provider, emoji_key);
        /* Each pass can expose another bounded block. Keep the qualification
           deterministic and below the device write/read discipline: this is
           at most eight 16 KiB reads, and the release player does no such
           eager scan. */
        for (unsigned pass = 0; pass < 8u; pass++) {
            if (!psp_glyph_component_session_pump_runtime(
                    glyph_component, engine)) break;
            pack_reads++;
            passed = psp_raster_fixture_rerender(
                engine, &report, error, sizeof(error));
            if (!passed) break;
        }
        bool regional_attached =
            (glyph_component->attached_mask & 0x0fu) != 0;
        bool emoji_attached =
            (glyph_component->attached_mask
             & (1u << TILEFINCH_GLYPH_PACK_COLOR_EMOJI)) != 0;
        if ((regional_attached && !regional_requested)
            || (emoji_attached
                && (!emoji_requested
                    || report.fallback_authored_color_pixels == 0))) {
            passed = false;
            snprintf(error, sizeof(error),
                     "optional glyph raster not exercised regional=%u "
                     "emoji=%u color=%zu",
                     regional_requested ? 1u : 0u,
                     emoji_requested ? 1u : 0u,
                     report.fallback_authored_color_pixels);
        }
    }
    BrowserFrameView frame = {0};
    bool dumped = passed
        && browser_engine_frame_view(engine, &frame)
        && psp_dump_frame_strided_named(
            argv0, "frame-raster.ppm", frame.pixels,
            frame.pixel_count, (size_t) frame.stride);
    printf(
        "tilefinch-raster-fixture: event=%s hash=%016llx "
        "round=%zu/%zu stroke=%zu/%zu fallback=%zu color=%zu "
        "pack-reads=%u regional=%u emoji=%u italic=%zu/%d dump=%d "
        "error=\"%s\"\n",
        passed ? "psp-pass" : "psp-fail",
        (unsigned long long) report.frame_hash,
        report.rounded_interior_pixels, report.rounded_edge_pixels,
        report.stroke_interior_pixels, report.stroke_edge_pixels,
        report.fallback_ink_pixels,
        report.fallback_authored_color_pixels, pack_reads,
        regional_requested ? 1u : 0u, emoji_requested ? 1u : 0u,
        report.italic_continuous_pairs,
        report.italic_maximum_centroid_step, dumped ? 1 : 0, error);
    psp_report_budget_counters(budget, "raster-fixture-exit");
    psp_glyph_component_session_destroy(glyph_component);
    browser_engine_destroy(engine);
    if (clock_worker_started)
        (void) psp_clock_worker_shutdown(clock_worker);
    printf(
        "tilefinch-validation: outcome=%s qualification=%s\n",
        passed ? "clean-exit" : "qualification-failed",
        passed ? "psp-raster-pass" : "psp-raster-fail");
    psp_log_checkpoint("raster-fixture-complete");
    psp_log_finish(
        passed ? "raster-fixture-pass" : "raster-fixture-fail");
    psp_exit_console(exit_to);
    return passed ? 0 : 1;
}
#endif

/* The stability report runs once at the end of an explicitly requested
   device soak. Keep its formatting and teardown out of the interactive
   loop's already-large instruction footprint. */
static TILEFINCH_COLD_PATH void
psp_finish_media_stability(PspApp *app, uint64_t elapsed_us)
{
    PspMediaSession *media = &app->browser->media;
    const PspBootConfig *config = &app->process->config;
    MediaBackendStats final_stats = {0};
    bool final_stats_ready = psp_media_backend_stats_snapshot(
        media, &final_stats);
    size_t decoded = media->accumulated_decoded_video_frames
        + final_stats.decoded_video_frames;
    size_t pipeline_skips = media->accumulated_dropped_video_frames
        + final_stats.dropped_video_frames;
    size_t seek_preroll_skips =
        media->accumulated_discarded_seek_video_frames
        + final_stats.discarded_seek_video_frames;
    size_t display_claims = media->accumulated_video_claims
        + final_stats.video_claims;
    size_t displayed = media->accumulated_video_claims_displayed
        + final_stats.video_claims_displayed;
    size_t display_drops = media->accumulated_video_claims_dropped
        + final_stats.video_claims_dropped;
    size_t quiesce_drops = media->accumulated_video_claims_quiesced
        + final_stats.video_claims_quiesced;
    size_t audio_packets = media->accumulated_audio_packets
        + final_stats.submitted_audio_packets;
    unsigned native_errors = media->accumulated_native_errors
        + (final_stats.last_native_error != 0 ? 1u : 0u);
    size_t peak_external = final_stats.external_bytes
            > media->peak_external_bytes
        ? final_stats.external_bytes : media->peak_external_bytes;
    int ending_capacity = scePowerGetBatteryRemainCapacity();
    int ending_percent = scePowerGetBatteryLifePercent();
    int capacity_delta = ending_capacity >= 0
            && app->interactive->media_stability_start_capacity >= 0
        ? ending_capacity - app->interactive->media_stability_start_capacity
        : INT_MIN;
    int percent_delta = ending_percent >= 0
            && app->interactive->media_stability_start_percent >= 0
        ? ending_percent - app->interactive->media_stability_start_percent
        : INT_MIN;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_power_log_battery(
        "media-finish", PSP_POWER_TEST_FIXED_HIGH, elapsed_us / 1000u);
#endif
    printf(
        "tilefinch-media-stability: event=skew-peak steady=%lluus "
        "at=%lluus audio=%lluus video=%lluus no-frame-ms=%u\n",
        (unsigned long long) app->interactive->media_stability_skew.steady_maximum_us,
        (unsigned long long)
            app->interactive->media_stability_skew.steady_peak_elapsed_us,
        (unsigned long long) app->interactive->media_stability_skew.steady_peak_audio_us,
        (unsigned long long) app->interactive->media_stability_skew.steady_peak_video_us,
        app->interactive->media_stability_skew.steady_peak_no_frame_ms);
    printf(
        "tilefinch-media-stability: event=complete stats-ready=%d "
        "elapsed=%lluus requested=%up actual=%dx%d decoded=%zu "
        "pipeline-skips=%zu seek-preroll-skips=%zu "
        "display-claims=%zu displayed=%zu "
        "display-drops=%zu quiesce-drops=%zu audio-packets=%zu "
        "native-errors=%u last-native=%d "
        "seeks=%u loops=%u fallbacks=%u peak-external=%zu min-free=%u "
        "min-largest=%u max-av-skew=%lluus steady-max-av-skew=%lluus "
        "steady-skew-samples=%u capacity-start=%d "
        "capacity-end=%d capacity-delta=%d percent-start=%d percent-end=%d "
        "percent-delta=%d scale-frames=%zu scale-total=%lluus "
        "scale-max=%lluus ge-frames=%zu ge-submit-total=%lluus "
        "ge-sync-total=%lluus ge-sync-max=%lluus skipped=%zu\n",
        final_stats_ready ? 1 : 0, (unsigned long long) elapsed_us,
        (unsigned) browser_profile_youtube_quality(app->browser->profile),
        media->stream.width, media->stream.height, decoded, pipeline_skips,
        seek_preroll_skips,
        display_claims, displayed, display_drops, quiesce_drops,
        audio_packets, native_errors, final_stats.last_native_error,
        media->seek_completions, app->interactive->media_stability_loops,
        media->quality_fallbacks, peak_external,
        app->interactive->media_stability_min_free,
        app->interactive->media_stability_min_largest,
        (unsigned long long) app->interactive->media_stability_skew.maximum_us,
        (unsigned long long) app->interactive->media_stability_skew.steady_maximum_us,
        app->interactive->media_stability_skew.steady_samples,
        app->interactive->media_stability_start_capacity, ending_capacity,
        capacity_delta, app->interactive->media_stability_start_percent, ending_percent,
        percent_delta, media->present_scale_frames,
        (unsigned long long) media->present_scale_total_us,
        (unsigned long long) media->present_scale_max_us,
        media->present_ge_frames,
        (unsigned long long) media->present_ge_submit_total_us,
        (unsigned long long) media->present_ge_sync_total_us,
        (unsigned long long) media->present_ge_sync_max_us,
        media->present_skipped_frames);
    app->interactive->media_stability_active = false;
    app->process->presentation.ui.validation_media_test_phase = 0;
    media->ui.playing = false;
    if (media->playback != NULL)
        media_playback_set_playing(media->playback, false);
    char completed_status[64];
    snprintf(
        completed_status, sizeof(completed_status),
        "%ld SEC VIDEO TEST COMPLETE",
        config->validation_media_stability_seconds);
    psp_ui_show_status(&app->process->presentation.ui, completed_status, 600);
    if (app->interactive->media_stability_auto_exit)
        psp_exit_plan_request(
            &app->interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
}

static TILEFINCH_COLD_PATH void
psp_sample_media_stability(PspApp *app, uint64_t now_us)
{
    PspMediaSession *media = &app->browser->media;
    PspMediaStabilitySkew *skew = &app->interactive->media_stability_skew;
    bool steady_candidate =
        media->machine.state == PSP_MEDIA_SESSION_PLAYING
        && media->job_phase == PSP_MEDIA_JOB_NONE
        && !media->presentation_preroll_audio_held
        && !media->ui.ended;
    if (!steady_candidate) {
        skew->steady_since_us = 0;
    } else if (skew->steady_since_us == 0) {
        skew->steady_since_us = now_us;
    }
    if (app->interactive->media_stability_started_us == 0
        || now_us < app->interactive->media_stability_next_sample_us) return;

    unsigned free_bytes = sceKernelTotalFreeMemSize();
    unsigned largest_bytes = sceKernelMaxFreeMemSize();
    if (free_bytes < app->interactive->media_stability_min_free)
        app->interactive->media_stability_min_free = free_bytes;
    if (largest_bytes < app->interactive->media_stability_min_largest)
        app->interactive->media_stability_min_largest = largest_bytes;
    MediaBackendStats live = {0};
    uint64_t audio_us = 0;
    if (media_playback_backend_stats(media->playback, &live)
        && live.presented_video_us != 0
        && media_playback_audio_cursor_us(media->playback, &audio_us)) {
        /* Successful sceAudioOutputBlocking calls put blocks into the PSP's
           firmware queue; they do not mean those samples are audible yet.
           The session clock and teardown telemetry both use the elapsed,
           queue-capped audio cursor. Sample that same clock here so startup
           preroll and seek refill do not masquerade as hundreds of
           milliseconds of steady A/V skew. */
        uint64_t sample_us = audio_us > live.presented_video_us
            ? audio_us - live.presented_video_us
            : live.presented_video_us - audio_us;
        if (sample_us > skew->maximum_us) skew->maximum_us = sample_us;
        if (skew->steady_since_us != 0
            && now_us - skew->steady_since_us
                   >= PSP_MEDIA_STABILITY_STEADY_DELAY_US) {
            if (sample_us > skew->steady_maximum_us) {
                skew->steady_maximum_us = sample_us;
                skew->steady_peak_elapsed_us =
                    now_us - app->interactive->media_stability_started_us;
                skew->steady_peak_audio_us = audio_us;
                skew->steady_peak_video_us = live.presented_video_us;
                skew->steady_peak_no_frame_ms = media->no_frame_ms;
            }
            if (skew->steady_samples != UINT_MAX) skew->steady_samples++;
        }
    }
    app->interactive->media_stability_next_sample_us =
        now_us + UINT64_C(250000);
}

static __attribute__((noinline)) void psp_media_stability_schedule_seeks(
    PspApp *app, uint64_t elapsed_us, uint64_t duration_us)
{
    PspMediaSession *media = &app->browser->media;
    int permille = (int) app->process->config.validation_media_seek_permille;
    if (app->interactive->media_stability_started_us != 0 && permille != 0
            && !app->interactive->media_stability_forward_seek
            && elapsed_us >= UINT64_C(15000000)
            && media->playback != NULL
            && media->have_frame
            && media->job_phase == PSP_MEDIA_JOB_NONE) {
        uint64_t target_us = psp_media_stability_seek_target_us(
            duration_us, permille);
        if (psp_media_request_seek(media, target_us, false)) {
            psp_video_scanout_note_discontinuity();
            media->job_resume_playing = true;
            app->interactive->media_stability_forward_seek = true;
            app->interactive->media_stability_skew.steady_since_us = 0;
            printf(
                "tilefinch-media-stability: event=seek "
                "direction=forward target=%lluus\n",
                (unsigned long long) target_us);
        }
    }
    if (app->interactive->media_stability_started_us != 0 && permille != 0
            && !app->interactive->media_stability_backward_seek
            && elapsed_us >= UINT64_C(45000000)
            && media->playback != NULL
            && media->have_frame
            && media->job_phase == PSP_MEDIA_JOB_NONE) {
        uint64_t target_us = duration_us > UINT64_C(6000000)
            ? UINT64_C(3000000) : 0;
        if (psp_media_request_seek(media, target_us, false)) {
            psp_video_scanout_note_discontinuity();
            media->job_resume_playing = true;
            app->interactive->media_stability_backward_seek = true;
            app->interactive->media_stability_skew.steady_since_us = 0;
            printf(
                "tilefinch-media-stability: event=seek "
                "direction=backward target=%lluus\n",
                (unsigned long long) target_us);
        }
    }
}

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
}

static TILEFINCH_COLD_PATH bool psp_save_site_data_on_exit(
    BrowserSession *session, const BrowserProfile *profile,
    bool persistent_site_data_available,
    const TilefinchInstallPaths *install_paths,
    const PspStoragePaths *storage, PspUiState *ui,
    const uint16_t *engine_frame)
{
    bool saved = true;
    bool stick_full = false;
    unsigned cache_mb = browser_profile_persistent_cache_mb(profile);
    bool cache_enabled = persistent_site_data_available && cache_mb != 0;
    bool storage_enabled =
        persistent_site_data_available
        && browser_profile_site_data_allowed(profile)
        && browser_profile_persist_local_storage(profile);

    /* A save publishes through a rotated backup, so a full generation needs
       free space in addition to the current one. Refusing before the first
       write preserves the previous generation. */
    uint64_t required =
        (cache_enabled ? (uint64_t) cache_mb * MIB : 0u)
        + (storage_enabled
               ? (uint64_t) BROWSER_SESSION_PERSIST_MAX_FILE_BYTES
               : 0u);
    uint64_t free_bytes = 0;
    if (required != 0
        && tilefinch_update_query_free_space(
               install_paths->data_dir, &free_bytes)
        && free_bytes < required) {
        stick_full = true;
        saved = false;
        printf(
            "tilefinch-site-data: preflight=refused free=%llu "
            "required=%llu\n",
            (unsigned long long) free_bytes,
            (unsigned long long) required);
    }
    if (!stick_full && cache_enabled) {
        BrowserSessionPersistenceLimits limits =
            psp_site_data_limits(cache_mb);
        saved = psp_site_data_save(
            session, storage->persistent_cache,
            BROWSER_SESSION_PERSIST_CACHE, &limits);
    }
    if (!stick_full && storage_enabled) {
        BrowserSessionPersistenceLimits limits = psp_site_data_limits(0);
        saved = psp_site_data_save(
                    session, storage->local_storage,
                    BROWSER_SESSION_PERSIST_LOCAL_STORAGE, &limits)
            && saved;
    }
    if (!saved) {
        psp_ui_show_status(
            ui,
            stick_full
                ? "MEMORY STICK FULL - SITE DATA NOT SAVED"
                : "SITE DATA COULD NOT BE SAVED",
            300);
        psp_present(engine_frame, ui);
    }
    return saved;
}



typedef struct {
    unsigned update_check_attempts;
    unsigned update_check_ratelimited;
    unsigned update_check_completed;
    unsigned update_check_available;
    uint64_t power_high_ms;
    uint64_t power_idle_ms;
    uint64_t power_transition_ms;
} PspInteractiveResult;

/* The resident frame loop. It borrows physical owners and returns only
   cleanup telemetry; lifecycle authority remains in the media and network
   machines reached through PspApp. */
static TILEFINCH_HOT_BOUNDARY PspInteractiveResult psp_app_run_interactive(
    PspProcessResources *process, PspBrowserResources *browser,
    PspInteractiveState *interactive, PspEngineViews *engine_views,
    const char *argv0, bool loaded, const char *startup_url,
    size_t startup_blocked_requests, PspBootQueue *boot_queue,
    FetchPreconnectDwell *home_preconnect_dwell,
    PspPowerPolicy *power_policy,
    bool native_home_boot, bool restoring_last_page,
    bool initial_error_page
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    , PspNetwork *network, PspNetworkLifecycle *network_lifecycle
#endif
)
{
    PspInteractiveResult result = {0};
#ifndef TILEFINCH_PSP_VALIDATION_LOG
    (void) argv0;
#endif
    TilefinchUpdateState trial_health_state = {0};
    TilefinchUpdateSlot running_slot =
        psp_update_session_current_slot(&process->install_paths);
    bool have_update_state = process->install_paths.slotted
        && tilefinch_update_journal_load(
               process->install_paths.data_dir, &trial_health_state, NULL);
    bool running_developer_slot = process->install_paths.slotted
        && tilefinch_update_slot_is_developer(
               process->install_paths.program_dir);
    if (have_update_state && !running_developer_slot) {
        TilefinchUpdateState raised = trial_health_state;
        if (tilefinch_update_state_raise_installed_floor(
                &raised, running_slot,
                TILEFINCH_RELEASE_SEQUENCE)
            && tilefinch_update_journal_store(
                   process->install_paths.data_dir, &raised, NULL, NULL)) {
            trial_health_state = raised;
            printf(
                "tilefinch-update-floor: reseeded sequence=%llu "
                "slot=%d\n",
                (unsigned long long) raised.installed_sequence,
                (int) running_slot);
        }
    }
    bool trial_health_pending = have_update_state
        && trial_health_state.trial == TILEFINCH_UPDATE_TRIAL_STARTED
        && trial_health_state.pending_slot == running_slot;
    uint64_t trial_health_started_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t trial_health_next_attempt_us = trial_health_started_us;
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
&& defined(TILEFINCH_PSP_LIVE_NETWORK)
    /*
     * The isolated update qualification deliberately uses the ordinary
     * signed Stable client, installer, launcher trial, and health journal.
     * Its only special powers are choosing a loopback HTTPS origin and
     * advancing the otherwise user-driven primary action.  A candidate
     * boot never checks again: it waits for the real health confirmation
     * above, records one terminal line, and leaves through normal cleanup.
     */
    PspUpdateE2E update_e2e;
    psp_update_e2e_init(
        &update_e2e, process->config.validation_update_auto != 0,
        trial_health_pending);
#endif

    /*
     * Optional background update-metadata check. Latched once per boot
     * and deferred until the app is idle. Signed channels require an
     * embedded root; Developer instead requires its explicit local URL.
     * Trace replay and the Options toggle keep either mode inert.
     */
    unsigned update_check_attempts = 0;
    unsigned update_check_ratelimited = 0;
    unsigned update_check_completed = 0;
    unsigned update_check_available = 0;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    bool update_check_toasted = false;
    BrowserUpdateChannel boot_update_channel =
        browser_profile_update_channel(browser->profile);
    bool boot_update_trust_configured =
        tilefinch_update_root_is_configured()
        || (boot_update_channel == BROWSER_UPDATE_CHANNEL_DEVELOPER
            && process->config.developer_update_url[0] != '\0');
    bool update_check_pending =
        strcmp(process->config.trace, "none") == 0
        && process->config.validation_update_auto == 0
        && boot_update_trust_configured
        && browser_profile_update_check_enabled(browser->profile);
    bool update_check_running = false;
#endif

#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /*
     * The local homepage is already scanout-visible and interactive at
     * this point. Warm the connection one PSP state-machine phase per UI
     * loop so the first external navigation can reuse work already in
     * flight instead of paying the full association delay.
     */
    if (strcmp(process->config.trace, "none") == 0
        && loaded
        && !psp_network_lifecycle_started(network_lifecycle)
        && (native_home_boot
            || psp_profile_page_kind(startup_url)
                != PSP_PROFILE_PAGE_NONE
            || !site_adapter_navigation_requires_network(
                   "GET", startup_url))
        && psp_network_begin(
               network, (int) process->config.network_profile)) {
        psp_network_lifecycle_request(
            network_lifecycle, PSP_NETWORK_REQUEST_BOOT,
            true, (int) process->config.network_profile, network,
            PSP_NETWORK_SUPERVISOR_STARTING, "warmup");
        printf("tilefinch-network-warmup: status=started "
               "after-homepage=yes\n");
    }
#endif

    unsigned voice_pressure_ticks = 0;
    unsigned scroll_log_counter = 0;
    bool render_job_pending = false;
    uint64_t render_job_last_progress_us = 0;
    size_t navigation_pump_boundaries = 0;
    size_t blocker_navigation_baseline =
        startup_blocked_requests;
    bool blocker_navigation_active = false;
    interactive->recovery = (PspRecoveryTracker) {
        .entry = navigation_current(engine_views->navigation),
        .observed_generation = engine_views->navigation->generation,
        .last_change_us = (uint64_t) sceKernelGetSystemTimeWide()
    };
    interactive->recovery.observed_scroll =
        interactive->recovery.entry == NULL ? 0 : interactive->recovery.entry->scroll_y;
    /* Automated PPSSPP smoke runs use the normal report and cleanup
       paths without requiring controller injection to leave the app. */
    if (process->config.exit_after_report != 0)
        psp_exit_plan_request(
            &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
    long interactive_validation_ticks = 0;
    /* The media-open scope is the only cooperate scope this loop holds
       across frames, so the loop -- not a per-frame predicate -- has to
       remember that it owns one. */
    bool media_open_scope = false;
    interactive->media_stability_active =
        process->config.validation_media_stability_auto != 0;
    interactive->media_stability_auto_exit =
        interactive->media_stability_active;
    const uint64_t media_stability_target_us =
        (uint64_t) process->config.validation_media_stability_seconds
            * UINT64_C(1000000);
    bool media_stability_lifecycle_injected = false;
    uint64_t previous_ui_sample_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t next_color_mode_check_us =
        previous_ui_sample_us + UINT64_C(60000000);
    unsigned power_worker_completions = 0;
    unsigned power_worker_failures = 0;
    uint64_t power_high_ms = 0;
    uint64_t power_idle_ms = 0;
    uint64_t power_transition_ms = 0;
    size_t lifecycle_network_requests_cancelled = 0;
    unsigned reader_shortcut_hold_ms = 0;
    bool reader_shortcut_held = false;
    bool reader_shortcut_triggered = false;
    bool reader_shortcut_started_on_page = false;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    bool lifecycle_network_was_started = false;
    bool lifecycle_network_was_ready = false;
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    interactive->power_auto_boot_pending =
        process->config.validation_power_test_auto != 0;
#endif
    /* Borrow the canonical owners for this loop invocation. Dispatchers may
       mutate through these pointers but never acquire their lifetimes. */
    PspApp app = {
        .process = process,
        .browser = browser,
        .views = engine_views,
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        .network = network,
        .network_lifecycle = network_lifecycle,
        .update_check_pending = &update_check_pending,
        .update_check_running = &update_check_running,
#endif
        .interactive = interactive
    };
    psp_app_refresh_network_profile_label(
        &app, (int) process->config.network_profile);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool input_script_loaded = psp_input_script_begin(
        &process->install_paths, argv0, process->config.input_script);
    if (process->config.input_script[0] != '\0' && !input_script_loaded) {
        /* A named scenario is an automation contract, not an optional
           controller aid. Falling back to the real pad makes the PPSSPP
           runner wait for its full timeout and can accidentally exercise
           unrelated user input. Leave through normal cleanup instead;
           the harness outcome makes the run fail immediately. */
        printf("tilefinch-input-script: outcome=load-failed "
               "script=%s\n", process->config.input_script);
        psp_exit_plan_request(
            &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
    }
#endif
    while (!psp_exit_plan_requested(&interactive->exit)
           && !psp_home_exit_pending()) {
        if ((process->presentation.ui.screen == PSP_UI_SCREEN_OPTIONS
             || process->presentation.ui.screen
                    == PSP_UI_SCREEN_OPTION_ITEMS)
            && !process->presentation.ui.network_profile_label_valid) {
            psp_app_refresh_network_profile_label(
                &app, (int) process->config.network_profile);
        }
        psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
        psp_log_heartbeat();
        /*
         * The navigation boot owed the user. It starts from inside the
         * interactive loop, not in front of it: the surface is already
         * up and taking input, the network warm-up is already running a
         * phase per frame, and a page that never arrives can no longer
         * keep the whole browser closed.
         */
        const char *deferred_url = psp_boot_queue_take(boot_queue);
        if (deferred_url != NULL) {
            char deferred[NAVIGATION_URL_LIMIT];
            snprintf(deferred, sizeof(deferred), "%s", deferred_url);
            bool deferred_started = false;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            if (strcmp(process->config.trace, "none") != 0
                || psp_ensure_network_for_navigation(
                       network, network_lifecycle,
                       (int) process->config.network_profile, "GET", deferred,
                       true,
                       engine_views->frame, &process->presentation.ui))
#endif
            {
                deferred_started = psp_begin_page_load(
                    browser->engine, &process->presentation.ui, browser->profile, engine_views->frame, &process->text_input,
                    deferred, true, 4 * MIB, 30000);
            }
            if (deferred_started) {
                interactive->navigation_job_started_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
                psp_ui_show_status(
                    &process->presentation.ui, restoring_last_page ? "RESTORING LAST PAGE"
                                             : "OPENING MY HOMEPAGE",
                    180);
            } else {
                psp_ui_show_home(&process->presentation.ui);
                psp_ui_show_status(&process->presentation.ui, "LAST PAGE UNAVAILABLE", 180);
            }
            printf(
                "tilefinch-boot-order: deferred-navigation started=%d\n",
                deferred_started ? 1 : 0);
        }
        offline_download_manager_set_maximum_height(
            &browser->offline_store.download,
            (int) browser_profile_youtube_quality(browser->profile));
        /* A PSP cannot schedule its own wake from a user-mode suspend.
           This validation-only cycle therefore publishes both callbacks
           together: lifecycle_poll still orders QUIESCE before RECOVER,
           exercising the exact media teardown/reopen, display rearm and
           network revalidation paths without claiming to test firmware
           sleep itself. Do not wait for an idle codec slot; split-state
           teardown is part of the contract this probe exists to cover. */
        if (process->config.validation_media_lifecycle_auto != 0
            && interactive->media_stability_active
            && !media_stability_lifecycle_injected
            && interactive->media_stability_started_us != 0
            && (uint64_t) sceKernelGetSystemTimeWide()
                   - interactive->media_stability_started_us
                   >= UINT64_C(30000000)
            && psp_lifecycle_state(&psp_lifecycle)
                   == PSP_LIFECYCLE_RUNNING) {
            media_stability_lifecycle_injected = true;
            printf(
                "tilefinch-media-stability: "
                "event=lifecycle-injected kind=logical "
                "clock=%lluus job=%d\n",
                (unsigned long long) browser->media.clock_us,
                (int) browser->media.job_phase);
            psp_lifecycle_notify_suspend(&psp_lifecycle);
            psp_lifecycle_notify_resume(&psp_lifecycle);
        }
        bool lifecycle_recovered = false;
        for (unsigned lifecycle_step = 0;
             lifecycle_step < 2u; lifecycle_step++) {
            PspLifecycleAction lifecycle_action =
                psp_lifecycle_poll(&psp_lifecycle);
            if (lifecycle_action == PSP_LIFECYCLE_ACTION_NONE)
                break;
            if (lifecycle_action
                    == PSP_LIFECYCLE_ACTION_QUIESCE) {
                bool navigation_was_pending =
                    browser_engine_navigation_pending(browser->engine);
                bool render_was_pending = render_job_pending;
                bool media_was_active =
                    browser->media.ui.visible
                    || psp_media_open_work_pending(&browser->media)
                    || psp_media_decode_work_pending(&browser->media);
                bool screenshot_was_pending =
                    interactive->screenshot.writer.status
                        == SCREENSHOT_PNG_PENDING;
                if (navigation_was_pending) {
                    const char *pending_url =
                        browser_engine_pending_navigation_url(browser->engine);
                    interactive->lifecycle_retry_available = pending_url != NULL;
                    if (pending_url != NULL)
                        snprintf(
                            interactive->lifecycle_retry_url,
                            sizeof(interactive->lifecycle_retry_url), "%s",
                            pending_url);
                    browser_engine_cancel_navigation(
                        browser->engine, "system suspended");
                }
                if (render_job_pending) {
                    browser_engine_cancel_render_job(browser->engine);
                    render_job_pending = false;
                    render_job_last_progress_us = 0;
                }
                if (screenshot_was_pending) {
                    screenshot_png_cancel(&interactive->screenshot.writer);
                    budget_free(browser->budget, interactive->screenshot.pixels);
                    interactive->screenshot.pixels = NULL;
                }
                browser_engine_cancel_idle_work(browser->engine);
                if (psp_navigation_cooperate_active())
                    psp_navigation_cooperate_end("system-suspend");
                /* The media-open owner survives ordinary frame boundaries,
                   but this scope was just fenced explicitly. Do not let a
                   stale local owner later release a different cooperate
                   session installed after resume. */
                media_open_scope = false;
                uint32_t suspended_download_id = 0;
                if (offline_download_manager_active(
                        &browser->offline_store.download,
                        &suspended_download_id))
                    (void) offline_download_manager_pause(
                        &browser->offline_store.download,
                        suspended_download_id);
                psp_media_suspend(&browser->media);
                /* The optional voice model is safe to rebuild and is the
                   only text-input resource large enough to matter across
                   a suspend. The page graph and framebuffer stay live. */
                psp_text_input_before_navigation(&process->text_input);
                if (browser->tabs != NULL)
                    (void) browser_tabs_capture_active(
                        browser->tabs, engine_views->navigation);
                bool tab_session_saved =
                    !browser_profile_restore_last_page(browser->profile)
                    || browser->tabs == NULL
                    || browser_tabs_save_session(
                        browser->tabs, process->storage.tab_session,
                        process->storage.tab_hibernation);
                bool recovery_saved =
                    psp_recovery_record_current(
                        browser->profile, process->storage.recovery, engine_views->navigation);
                bool profile_saved =
                    psp_profile_store_flush(&browser->profile_store);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                lifecycle_network_was_started =
                    psp_network_lifecycle_started(network_lifecycle);
                lifecycle_network_was_ready =
                    psp_network_lifecycle_ready(network_lifecycle)
                    && network->status == PSP_NETWORK_READY;
                lifecycle_network_requests_cancelled = 0;
                if (lifecycle_network_was_started) {
                    psp_network_lifecycle_suspend(
                        network_lifecycle, network,
                        lifecycle_network_was_ready, "suspend");
                }
                if (lifecycle_network_was_started
                    && !lifecycle_network_was_ready) {
                    lifecycle_network_requests_cancelled =
                        browser_engine_cancel_network_work(
                            browser->engine, "network suspended");
                    psp_shutdown_network_logged(network);
                }
                /* Never hold a speculative connection across a suspend:
                   drop it (and its transport reference) as the network is
                   torn down. */
                fetch_preconnect_cancel("quiesce");
                bool tls_sessions_saved =
                    fetch_tls_session_store_flush();
#else
                bool tls_sessions_saved = true;
#endif
                interactive->navigation_job_started_us = 0;
                printf(
                    "tilefinch-lifecycle: event=quiesce epoch=%u "
                    "navigation=%d render=%d media=%d screenshot=%d "
                    "network-cancelled=%zu recovery=%d tabs=%d "
                    "profile=%d tls-sessions=%d "
                    "free=%d largest=%d\n",
                    psp_lifecycle.handled_suspend_epoch,
                    navigation_was_pending ? 1 : 0,
                    render_was_pending ? 1 : 0,
                    media_was_active ? 1 : 0,
                    screenshot_was_pending ? 1 : 0,
                    lifecycle_network_requests_cancelled,
                    recovery_saved ? 1 : 0,
                    tab_session_saved ? 1 : 0,
                    profile_saved ? 1 : 0,
                    tls_sessions_saved ? 1 : 0,
                    sceKernelTotalFreeMemSize(),
                    sceKernelMaxFreeMemSize());
                psp_lifecycle_complete_quiesce(&psp_lifecycle);
                continue;
            }

            int resume_clock_result =
                scePowerSetClockFrequency(333, 333, 166);
            if (process->clock_live) {
                uint64_t resume_clock_request_us = 0;
                (void) psp_clock_worker_request(
                    &process->clock_worker, false,
                    &resume_clock_request_us);
            }
            power_policy->quiet_ms = 0;
            power_policy->idle_clock = false;
            /* Nothing survived suspend, least of all a video session:
               psp_media_suspend destroyed the pipeline, so the surface
               the rearm should reassert is the page's. */
            (void) psp_display_video_end(&psp_display);
            bool display_rearmed =
                psp_display_rearm(&psp_display);
            /* Neither buffer's contents survived suspend in any way this
               code may assume, so no present may be skipped against
               what they used to hold. */
            psp_media_present_forget_buffers();
            /* The last accepted frame is scanout-safe. Enable presentation
               before the bounded network reconnect so the existing supervisor
               can keep the recovery UI responsive. */
            psp_lifecycle_complete_recovery(&psp_lifecycle);
            interactive->previous_buttons = 0;
            psp_ui_suspend_page_input(&process->presentation.ui);
            previous_ui_sample_us =
                (uint64_t) sceKernelGetSystemTimeWide();
            next_color_mode_check_us =
                previous_ui_sample_us + UINT64_C(60000000);
            bool resume_network_ready = true;
            int resume_network_native = 0;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            if (lifecycle_network_was_ready) {
                resume_network_ready =
                    psp_network_resume_ready(
                        network, &resume_network_native);
                psp_network_lifecycle_resume_result(
                    network_lifecycle, network, true,
                    resume_network_ready, "resume");
                resume_network_ready = resume_network_ready
                    && psp_network_lifecycle_ready(
                        network_lifecycle);
                if (!resume_network_ready) {
                    lifecycle_network_requests_cancelled +=
                        browser_engine_cancel_network_work(
                            browser->engine, "network changed while suspended");
                }
            } else if (lifecycle_network_was_started) {
                resume_network_ready = false;
                psp_network_lifecycle_resume_result(
                    network_lifecycle, network, false,
                    false, "resume-off");
            }
            if (lifecycle_network_was_started
                && !resume_network_ready) {
                psp_ui_show_status(
                    &process->presentation.ui, "RECONNECTING NETWORK", 600);
                resume_network_ready = psp_connect_network(
                    network, (int) process->config.network_profile,
                    engine_views->frame, &process->presentation.ui);
                if (!resume_network_ready
                    && psp_network_lifecycle_started(
                           network_lifecycle)) {
                    psp_shutdown_network_logged(network);
                }
            }
            lifecycle_network_was_started = false;
            lifecycle_network_was_ready = false;
#endif
            psp_media_resume(&browser->media);
            psp_ui_show_status(
                &process->presentation.ui,
                interactive->lifecycle_retry_available
                    ? "WELCOME BACK - SQUARE RETRIES PAGE"
                    : (resume_network_ready
                           ? "WELCOME BACK" : "RESUMED OFFLINE"),
                interactive->lifecycle_retry_available ? 360 : 180);
            printf(
                "tilefinch-lifecycle: event=recover epoch=%u "
                "display=%d display-error=0x%08x clock=0x%08x "
                "network=%d network-native=0x%08x "
                "network-cancelled=%zu media-reopen=%d "
                "free=%d largest=%d\n",
                psp_lifecycle.handled_resume_epoch,
                display_rearmed ? 1 : 0,
                (unsigned) psp_display.last_rearm_error,
                (unsigned) resume_clock_result,
                resume_network_ready ? 1 : 0,
                (unsigned) resume_network_native,
                lifecycle_network_requests_cancelled,
                psp_media_open_work_pending(&browser->media) ? 1 : 0,
                sceKernelTotalFreeMemSize(),
                sceKernelMaxFreeMemSize());
            psp_present(engine_views->frame, &process->presentation.ui);
            lifecycle_recovered = true;
        }
        if (psp_lifecycle_state(&psp_lifecycle)
                != PSP_LIFECYCLE_RUNNING) {
            (void) sceKernelDelayThread(10000);
            continue;
        }
        if (lifecycle_recovered) continue;
        /* The last thing the frame does before it stops is give the codec
           worker something to do while it is stopped. */
        (void) psp_media_feed_before_blocking(&browser->media);
        /*
         * The fullscreen presenter itself waits for vblank before it
         * latches a complete frame. Waiting another vblank here samples a
         * 24-fps stream only about 26 times a second: an early sample then
         * lands almost a whole frame late, producing the fast/slow cadence
         * visible on hardware and forcing the late-frame catch-up path.
         *
         * Poll only while a real playing picture owns the panel. The 2ms
         * sleep is still a blocking yield for the lower-priority codec
         * worker -- no busy spin -- and the eventual publish remains the
         * sole tear-free display clock. Pages, loading/error panels,
         * paused media and seek transactions keep the established vblank
         * throttle.
         */
        bool fullscreen_media_poll = browser->media.ui.visible
            && browser->media.ui.playing && browser->media.have_frame
            && !psp_media_open_work_pending(&browser->media)
            && browser->media.job_phase == PSP_MEDIA_JOB_NONE
            && !browser->media.pause_boundary_pending;
        /* A normal page frame waits here and again when the completed back
           buffer is published. Do not give UI motion the media shortcut:
           NEXTFRAME does not latch until vblank, so returning after a 2ms
           poll can rotate back to and overwrite a buffer the panel is still
           scanning. Fullscreen media owns a separately fenced presentation
           path; menus retain the established vblank discipline. */
        if (fullscreen_media_poll) {
            (void) sceKernelDelayThread(
                PSP_MEDIA_FULLSCREEN_POLL_YIELD_US);
        } else {
            sceDisplayWaitVblankStart();
        }
#ifdef TILEFINCH_HAVE_PSP_VOICE
        if ((++voice_pressure_ticks & 63u) == 0)
            psp_text_input_trim(&process->text_input);
#else
        (void) voice_pressure_ticks;
#endif
        SceCtrlData pad;
        if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) continue;
        PspAppFrameState frame;
        frame.ui_sample_us =
            (uint64_t) sceKernelGetSystemTimeWide();
        bool color_mode_visual_changed = false;
        if (frame.ui_sample_us >= next_color_mode_check_us) {
            bool was_dark = process->presentation.ui.page_dark;
            psp_refresh_page_color_mode(&process->presentation.ui);
            next_color_mode_check_us =
                frame.ui_sample_us + UINT64_C(60000000);
            if (process->presentation.ui.page_dark != was_dark) {
                (void) browser_engine_set_forced_dark(
                    browser->engine, process->presentation.ui.page_dark);
                color_mode_visual_changed = true;
            }
        }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        /* Refresh the cached wifi bar count on its own slow cadence;
           the status line forwards the cache without paying the read. */
        psp_sample_wifi_strength(
            psp_network_lifecycle_ready(network_lifecycle)
                ? network : NULL,
            frame.ui_sample_us);
#endif
        uint64_t ui_elapsed_us = frame.ui_sample_us - previous_ui_sample_us;
        previous_ui_sample_us = frame.ui_sample_us;
        unsigned ui_elapsed_ms =
            ui_elapsed_us >= UINT64_C(1000000)
                ? 1000u : (uint32_t) ui_elapsed_us / 1000u;
        if (ui_elapsed_ms == 0) ui_elapsed_ms = 1;
        /*
         * Video is presented against the wall clock, so the media session
         * gets the frame time that actually elapsed and never a nominal
         * one. The scripted-input harness pins ui_elapsed_ms to 16 ms so a
         * slow host cannot turn a tap into a long press; a device frame
         * during playback is about 32 ms, so feeding that 16 ms to the
         * media clock advanced it at half speed. Everything downstream of
         * the clock followed: the decode horizon admitted half the content
         * per second, the pipeline reported itself caught up while audio
         * ran at 23 of its 43 blocks a second, and the Media Engine sat 88%
         * idle. The scripted media truth cycle was measuring the harness.
         */
        unsigned media_elapsed_ms = ui_elapsed_ms;
        PspUiInput input = psp_ui_input(
            &pad, interactive->previous_buttons, ui_elapsed_ms);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        /*
         * The scripted source replaces the sampled pad outright, and only
         * steps forward on a frame the browser could actually have taken
         * the press on. That readiness gate is what makes one script walk
         * the identical sequence under PPSSPP and on a PSP-3000, where
         * the same navigation costs a different number of frames.
         */
        if (psp_input_script_running()) {
            /* The script is the sole controller source until a person
               touches the real pad. A physical press is an explicit handoff:
               stop every automatic driver and deliver that same press now.
               Merging sources would let a later scripted Cross immediately
               undo a manual Pause. */
            if (input.held != 0u) {
                psp_input_script_interrupt_by_user();
                psp_boot_config_disable_automation(&process->config);
                interactive->media_stability_active = false;
                interactive->media_stability_auto_exit = false;
                process->presentation.ui.validation_media_test_phase = 0;
                input.pressed = input.held;
            } else {
                uint32_t scripted_download_id = 0;
                bool scripted_background_idle =
                    interactive->screenshot.writer.status
                        != SCREENSHOT_PNG_PENDING
                    && !psp_update_session_active(&browser->update_session)
                    && !offline_download_manager_active(
                           &browser->offline_store.download,
                           &scripted_download_id);
                bool supervisor_owns_script =
                    psp_navigation_cooperate_supervised();
                bool scripted_ready =
                    scripted_background_idle
                    && !process->presentation.ui.loading
                    && !render_job_pending
                    && !browser->media.ui.visible
                    && !browser_engine_navigation_pending(browser->engine)
                    && !navigation_background_resources_pending(
                           engine_views->navigation);
                if (supervisor_owns_script) {
                    memset(&input, 0, sizeof(input));
                    input.analog_x = 128;
                    input.analog_y = 128;
                } else if (!psp_input_script_frame(
                               &input, scripted_ready)) {
                    psp_exit_plan_request(
                        &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
                    continue;
                }
            }
            /* The loop's own millisecond consumers -- the reader-mode
               hold, the power policy -- read this rather than the input,
               and a frame that took 800 ms under a loaded host would
               otherwise turn a scripted tap into a long press.
               The media presentation clock is deliberately NOT one of
               them: see media_elapsed_ms below. */
            ui_elapsed_ms = 16u;
        }
#endif
        interactive->previous_buttons = input.held;
        bool reader_shortcut = false;
        bool toolbar_held =
            (input.held & PSP_UI_BUTTON_TOOLBAR) != 0;
        if (toolbar_held && !reader_shortcut_held) {
            reader_shortcut_hold_ms = 0;
            reader_shortcut_triggered = false;
            reader_shortcut_started_on_page =
                process->presentation.ui.screen == PSP_UI_SCREEN_PAGE && !browser->media.ui.visible;
        }
        if (toolbar_held && reader_shortcut_started_on_page) {
            input.pressed &= ~PSP_UI_BUTTON_TOOLBAR;
            if (reader_shortcut_hold_ms < 1000u)
                reader_shortcut_hold_ms += ui_elapsed_ms;
            if (reader_shortcut_hold_ms >= 700u
                && !reader_shortcut_triggered) {
                reader_shortcut = true;
                reader_shortcut_triggered = true;
            }
        } else if (!toolbar_held && reader_shortcut_held) {
            if (reader_shortcut_started_on_page
                && !reader_shortcut_triggered)
                input.pressed |= PSP_UI_BUTTON_TOOLBAR;
            reader_shortcut_hold_ms = 0;
            reader_shortcut_triggered = false;
            reader_shortcut_started_on_page = false;
        }
        reader_shortcut_held = toolbar_held;
        bool power_auto_started_visual = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        bool power_auto_start_ready =
            interactive->power_auto_boot_pending
            && !process->presentation.ui.loading && !render_job_pending
            && !browser->media.ui.visible
            && !browser_engine_navigation_pending(browser->engine)
            && !navigation_background_resources_pending(engine_views->navigation)
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            && !psp_network_lifecycle_warming(network_lifecycle)
#endif
            ;
        if (power_auto_start_ready) {
            PspClockWorkerSnapshot initial_power = {0};
            if (process->clock_live)
                psp_clock_worker_snapshot(
                    &process->clock_worker, &initial_power);
            interactive->power_auto_boot_pending = false;
            bool power_auto_started = psp_power_auto_start(
                &interactive->power_auto, &interactive->power_test, frame.ui_sample_us,
                process->clock_live ? &initial_power : NULL,
                &process->presentation.ui, process->config.validation_power_test_auto != 0,
                process->config.validation_power_test_auto != 0
                    ? "boot-config" : "menu");
            if (!power_auto_started)
                process->presentation.ui.validation_power_test_phase = 0;
            power_auto_started_visual = true;
        }
        bool power_auto_abort_requested = false;
        if (interactive->power_auto_boot_pending || interactive->power_auto.active) {
            power_auto_abort_requested =
                (input.pressed & PSP_UI_BUTTON_CANCEL) != 0;
            if (power_auto_abort_requested
                && interactive->power_auto_boot_pending
                && !interactive->power_auto.active) {
                interactive->power_auto_boot_pending = false;
                process->presentation.ui.validation_power_test_phase = 0;
                psp_ui_show_status(
                    &process->presentation.ui, "POWER TEST STOPPED", 240);
                power_auto_started_visual = true;
                power_auto_abort_requested = false;
            }
            /* This benchmark isolates the idle-reading state. Ignore
               ordinary controls so every segment presents the same page
               and follows the same browser path; Circle remains an
               immediate escape hatch. */
            input.pressed = 0;
            input.held = 0;
            input.analog_x = 128;
            input.analog_y = 128;
        }
#endif
        bool power_interaction =
            input.pressed != 0 || input.held != 0
            || input.analog_x < 104 || input.analog_x > 152
            || input.analog_y < 104 || input.analog_y > 152;
        bool visible_active_work =
            process->presentation.ui.loading || render_job_pending || browser->media.ui.visible
            || browser_engine_navigation_pending(browser->engine)
            || navigation_background_resources_pending(engine_views->navigation)
            || process->presentation.ui.screen == PSP_UI_SCREEN_UPDATE;
        bool power_active_work = visible_active_work;
        bool adaptive_power_test_active = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        adaptive_power_test_active =
            interactive->power_test.phase == PSP_POWER_TEST_ADAPTIVE;
#endif
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        power_active_work =
            power_active_work
            || psp_network_lifecycle_warming(network_lifecycle);
#endif
        if (psp_power_policy_update(
                power_policy, ui_elapsed_ms,
                power_interaction,
                power_active_work || !adaptive_power_test_active)) {
            printf(
                "tilefinch-power-request: cpu=%u bus=%u request=%lluus "
                "count=%u failures=%u\n",
                power_policy->idle_clock
                    ? PSP_POWER_POLICY_IDLE_CPU_MHZ
                    : PSP_POWER_POLICY_HIGH_CPU_MHZ,
                power_policy->idle_clock
                    ? PSP_POWER_POLICY_IDLE_BUS_MHZ
                    : PSP_POWER_POLICY_HIGH_BUS_MHZ,
                (unsigned long long)
                    power_policy->last_transition_us,
                power_policy->transitions, power_policy->failures);
        }
        /*
         * Ambient motion is the first thing to go when the device is
         * doing something that matters: a stepped-down clock, a page
         * loading, media open, or an update in flight. The surface says
         * so by holding still rather than by claiming to be busy.
         */
        bool suppress_ambient_motion =
            power_policy->idle_clock || visible_active_work;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        /* The active-wave baseline spent about 2.0 s of compositor CPU
           over the 12 s startup scenario (roughly one sixth of the
           foreground budget) while association was still in flight.
           Hold the already-painted wave still until APCTL becomes
           terminal; input and the rest of HOME remain interactive-> */
        suppress_ambient_motion =
            suppress_ambient_motion
            || psp_network_lifecycle_warming(network_lifecycle);
#endif
        process->presentation.ui.motion_suppressed = suppress_ambient_motion ? 1u : 0u;
        PspClockWorkerSnapshot power_snapshot = {0};
        if (process->clock_live) {
            psp_clock_worker_snapshot(
                &process->clock_worker, &power_snapshot);
            if (power_snapshot.completions
                    != power_worker_completions
                || power_snapshot.failures
                    != power_worker_failures) {
                power_worker_completions =
                    power_snapshot.completions;
                power_worker_failures = power_snapshot.failures;
                printf(
                    "tilefinch-power-complete: desired=%s applied=%s "
                    "transition=%luus maximum=%luus completions=%u "
                    "failures=%u\n",
                    power_snapshot.desired_idle ? "idle" : "high",
                    power_snapshot.applied_idle ? "idle" : "high",
                    power_snapshot.last_transition_us,
                    power_snapshot.maximum_transition_us,
                    power_snapshot.completions,
                    power_snapshot.failures);
            }
        }
        if (process->clock_live && power_snapshot.transitioning)
            power_transition_ms += ui_elapsed_ms;
        else if (process->clock_live && power_snapshot.applied_idle)
            power_idle_ms += ui_elapsed_ms;
        else
            power_high_ms += ui_elapsed_ms;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        psp_power_test_tick(
            &interactive->power_test, frame.ui_sample_us, ui_elapsed_ms,
            process->clock_live ? &power_snapshot : NULL);
        bool power_auto_visual_changed =
            power_auto_started_visual;
        if (interactive->power_auto.active
            && (power_auto_abort_requested
                || frame.ui_sample_us - interactive->power_test.started_us
                       >= PSP_POWER_AUTO_SEGMENT_US)) {
            PspPowerTestResult segment_result =
                psp_power_test_finish(
                    &interactive->power_test, frame.ui_sample_us,
                    process->clock_live ? &power_snapshot : NULL,
                    power_auto_abort_requested
                        ? "user-abort" : "automatic-segment");
            psp_power_auto_accumulate(
                &interactive->power_auto, &segment_result);
            printf(
                "tilefinch-power-auto: event=segment-finish "
                "segment=%u/4 phase=%s\n",
                interactive->power_auto.segment + 1u,
                psp_power_test_phase_name(segment_result.phase));
            if (power_auto_abort_requested) {
                interactive->power_auto.active = false;
                process->presentation.ui.validation_power_test_phase = 0;
                psp_power_auto_log_summary(
                    &interactive->power_auto, frame.ui_sample_us, "user-abort");
                psp_ui_show_status(
                    &process->presentation.ui, "POWER TEST STOPPED", 240);
            } else if (++interactive->power_auto.segment
                       < PSP_POWER_AUTO_SEGMENTS) {
                psp_power_auto_begin_segment(
                    &interactive->power_auto, &interactive->power_test, frame.ui_sample_us,
                    process->clock_live ? &power_snapshot : NULL,
                    &process->presentation.ui);
            } else {
                interactive->power_auto.active = false;
                process->presentation.ui.validation_power_test_phase = 0;
                psp_power_auto_log_summary(
                    &interactive->power_auto, frame.ui_sample_us, "complete");
                psp_ui_show_status(
                    &process->presentation.ui,
                    "2 MIN POWER TEST COMPLETE - EXIT TO SAVE LOG",
                    900);
                if (process->config.validation_power_test_auto != 0) {
                    psp_log_checkpoint(
                        "automatic-power-test-complete");
                    psp_exit_plan_request(
                        &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
                }
            }
            power_auto_visual_changed = true;
        }
#else
        bool power_auto_visual_changed =
            power_auto_started_visual;
#endif
        if (trial_health_pending
            && frame.ui_sample_us >= trial_health_next_attempt_us
            && (input.pressed != 0
                || frame.ui_sample_us - trial_health_started_us
                       >= UINT64_C(10000000))) {
            TilefinchUpdateState healthy = trial_health_state;
            if (tilefinch_update_state_confirm_healthy(&healthy)
                && tilefinch_update_journal_store(
                       process->install_paths.data_dir, &healthy, NULL, NULL)) {
                trial_health_state = healthy;
                trial_health_pending = false;
                printf(
                    "tilefinch-update-health: confirmed input=%d "
                    "elapsed=%llums\n",
                    input.pressed != 0 ? 1 : 0,
                    (unsigned long long)
                        ((frame.ui_sample_us - trial_health_started_us)
                         / 1000u));
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
&& defined(TILEFINCH_PSP_LIVE_NETWORK)
                if (psp_update_e2e_confirmed(
                        &update_e2e, &healthy, running_slot)) {
                    psp_exit_plan_request(
                        &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
                }
#endif
            } else {
                trial_health_next_attempt_us =
                    frame.ui_sample_us + UINT64_C(1000000);
            }
        }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        /*
         * Fire the boot-latched background check only once the boot
         * really succeeded, nothing foreground is in flight, the user
         * has been idle for the house autohide interval, and an
         * already-joined network is READY. This path never initiates a
         * join; if the network never comes up, the check silently
         * stays pending for this boot.
         */
        if (update_check_pending
            && !update_check_running
            && loaded && !initial_error_page
            && !process->presentation.ui.loading && !render_job_pending
            && !browser->media.ui.visible
            && process->presentation.ui.screen == PSP_UI_SCREEN_PAGE
            && !browser_engine_navigation_pending(browser->engine)
            && !navigation_background_resources_pending(engine_views->navigation)
            && !psp_network_lifecycle_warming(network_lifecycle)
            && !power_interaction
            && process->presentation.ui.activity_frames == 0
            && psp_network_lifecycle_ready(network_lifecycle)
            && network->status == PSP_NETWORK_READY) {
            update_check_pending = false;
            time_t update_check_now = time(NULL);
            if (update_check_now <= 0) {
                printf("tilefinch-update-check: skipped=no-clock\n");
            } else if (!browser_profile_update_check_due(
                           browser->profile,
                           (uint64_t) update_check_now)) {
                update_check_ratelimited++;
                printf(
                    "tilefinch-update-check: skipped=ratelimited "
                    "last=%llu now=%llu\n",
                    (unsigned long long)
                        browser_profile_update_check_last_unix(
                            browser->profile),
                    (unsigned long long) update_check_now);
            } else {
                /* A completed hidden player is only a replay cache. An
                   update check has a larger, foreground-visible download
                   transaction and must not inherit that decoder's native
                   and contiguous-memory footprint. */
                (void) psp_media_reclaim_hidden_pipeline(&browser->media);
                if (!psp_update_session_initialized(&browser->update_session))
                    (void) psp_update_session_initialize(
                        &browser->update_session, browser->budget, &process->install_paths,
                        &(PspUpdateSessionOptions) {
                            .channel =
                                browser_profile_update_channel(browser->profile),
                            .developer_metadata_url =
                                process->config.developer_update_url,
                            .developer_package_url =
                                process->config.developer_package_url
                        },
                        &process->presentation.ui);
                if (psp_update_session_available(&browser->update_session)
                    && psp_update_session_begin_check(
                           &browser->update_session,
                           (uint64_t) update_check_now, true)) {
                    /* Refresh the snapshot so the shared pump below
                       adopts the new CHECKING phase this frame. */
                    psp_update_session_refresh_ui(
                        &browser->update_session, &process->presentation.ui);
                    update_check_running = true;
                    update_check_attempts++;
                    printf(
                        "tilefinch-update-check: started "
                        "now=%llu\n",
                        (unsigned long long) update_check_now);
                }
            }
        }
#endif
        PspUiIntent intent = {0};
        bool media_visual_changed = false;
        bool navigation_visual_changed =
            color_mode_visual_changed || power_auto_visual_changed;
        bool navigation_pending =
            browser_engine_navigation_pending(browser->engine);
        if (navigation_pending
            && (input.pressed & PSP_UI_BUTTON_CANCEL) != 0) {
            uint32_t cancel_operation =
                psp_log_operation_begin("navigation-cancel");
            browser_engine_cancel_navigation(
                browser->engine, "cancelled from PSP browser UI");
            psp_ui_set_loading(&process->presentation.ui, false, 0);
            psp_ui_show_status(&process->presentation.ui, "PAGE LOAD STOPPED", 180);
            psp_navigation_cooperate_end("interactive-cancel");
            interactive->navigation_job_started_us = 0;
            blocker_navigation_active = false;
            navigation_pending = false;
            if (interactive->tab_transition.pending) {
                (void) psp_tabs_finish(
                    browser->tabs, browser->engine, &interactive->tab_transition, false,
                    browser_profile_tab_hibernation_enabled(browser->profile),
                    process->storage.tab_hibernation);
                psp_tabs_sync_ui(&process->presentation.ui, browser->tabs, &process->presentation.tab_view);
            }
            navigation_visual_changed = true;
            input.pressed &= ~PSP_UI_BUTTON_CANCEL;
            psp_log_operation_end(
                cancel_operation, "navigation-cancel", "requested");
        }
        if (navigation_pending) {
            /* The supervisor acknowledges non-cancel button presses as
               busy rather than letting an unrelated action race the
               candidate navigation. The analog cursor remains chrome-
               local and responsive; its page event is suppressed below
               until the candidate reaches a terminal state. */
            psp_ui_suspend_page_input(&process->presentation.ui);
            input.pressed = 0;
            input.held = 0;
        }
        bool cursor_feedback_presented = false;
        uint8_t cursor_shape_before_dispatch = process->presentation.ui.cursor_shape;
        /* Consume the callback supervisor's completed mailbox at one loop
           boundary. A suspend or cancellation can end a media-owned scope
           outside the two ordinary open/decode exits below; central capture
           prevents that intent from surviving until an unrelated later
           cooperation scope. */
        psp_app_capture_supervisor_media_intent(interactive);
        if (browser->media.ui.visible && !navigation_pending) {
            media_visual_changed =
                psp_app_dispatch_deferred_media_intent(
                    &browser->media, interactive)
                || media_visual_changed;
            PspUiMediaIntent media_intent =
                psp_ui_media_update(&browser->media.ui, &input);
            uint32_t media_operation = 0;
            const char *media_action =
                psp_media_action_name(media_intent.action);
            if (media_intent.action != PSP_UI_MEDIA_ACTION_NONE) {
                psp_log_set_phase(PSP_LOG_PHASE_MEDIA);
                media_operation =
                    psp_log_operation_begin(media_action);
            }
            if (media_intent.visual_changed
                && !psp_navigation_cooperate_supervised()) {
                /* Publish the controls' pressed state before a close,
                   seek, or retry can enter a slower backend operation. */
                psp_present(engine_views->frame, &process->presentation.ui);
            }
            psp_media_execute_intent(&browser->media, media_intent);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            psp_input_script_observe_media(
                &media_intent, &browser->media.ui);
#endif
            if (media_operation != 0) {
                psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
                psp_log_operation_end(
                    media_operation, media_action, "returned");
            }
            media_visual_changed = media_intent.visual_changed;
        } else {
            intent = psp_ui_update(&process->presentation.ui, &input);
            if (reader_shortcut) {
                intent = (PspUiIntent) {
                    .action = PSP_UI_ACTION_TOGGLE_READER,
                    .visual_changed = true
                };
            }
            /*
             * Two HOME rows are not navigations at all: SEARCH opens the
             * omnibox and a CONTINUE row switches tabs. Resolve them into
             * the actions that already exist rather than duplicating
             * either flow inside the surface's own case.
             */
            if (intent.action == PSP_UI_ACTION_HOME_ACTIVATE) {
                size_t home_target = 0;
                PspHomeTargetKind home_kind = psp_home_target_kind(
                    &process->presentation.home_surface, intent.list_index, &home_target);
                if (home_kind == PSP_HOME_TARGET_SEARCH) {
                    intent.action = PSP_UI_ACTION_OPEN_ADDRESS;
                } else if (home_kind == PSP_HOME_TARGET_TAB) {
                    psp_ui_leave_native_surface(&process->presentation.ui);
                    intent.action = PSP_UI_ACTION_SWITCH_TAB;
                    intent.tab_index = (uint8_t) home_target;
                }
            }
            if (intent.pointer_phase == PSP_UI_POINTER_MOVE
                && !psp_navigation_cooperate_supervised()) {
                /* Cursor position is chrome state, so publish it before
                   hover dispatch, update work, navigation pumping, JS,
                   or a raster slice. The prior path waited behind all of
                   those because analog movement has no button edge. */
                psp_cursor_latency_sample(frame.ui_sample_us);
                psp_present(engine_views->frame, &process->presentation.ui);
                cursor_feedback_presented = true;
            }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            /*
             * Speculative preconnect (docs/engineering/
             * PSP_TRANSPORT.md). While a HOME tile settles under
             * focus, warm its host's TCP+TLS in the background so the
             * handshake is already paid when the user opens it. Bounds are
             * carried by the pure dwell state machine: only the user's own
             * tiles (bookmarks / the built-in cards), at most one
             * connection, started only after ~300 ms of focus, cancelled on
             * focus move, never before the network is READY or under the
             * deterministic harness. It is connect-only -- no request is
             * sent. The pump is non-blocking and rides this cooperative
             * checkpoint without a scheduler slot.
             */
            {
                bool home_ready =
                    process->presentation.ui.screen == PSP_UI_SCREEN_HOME
                    && strcmp(process->config.trace, "none") == 0
                    && psp_network_lifecycle_ready(
                        network_lifecycle)
                    && network->status == PSP_NETWORK_READY;
                const char *preconnect_url =
                    home_ready
                        ? psp_home_target_url(
                              &process->presentation.home_surface, process->presentation.ui.home_selection, browser->profile)
                        : NULL;
                uint32_t preconnect_key =
                    preconnect_url == NULL
                        ? 0u : fetch_preconnect_tile_key(preconnect_url);
                FetchPreconnectDwellAction preconnect_action =
                    fetch_preconnect_dwell_step(
                        home_preconnect_dwell, preconnect_key != 0u,
                        preconnect_key, ui_elapsed_ms,
                        PSP_HOME_PRECONNECT_DWELL_MS);
                if (preconnect_action == FETCH_PRECONNECT_DWELL_START) {
                    (void) fetch_preconnect(preconnect_url, browser->budget);
                } else if (preconnect_action
                           == FETCH_PRECONNECT_DWELL_CANCEL) {
                    fetch_preconnect_cancel("home-focus-change");
                }
                fetch_preconnect_pump();
            }
#endif
            if (process->presentation.ui.screen == PSP_UI_SCREEN_TABS
                && intent.visual_changed && browser->tabs != NULL) {
                (void) browser_tabs_capture_active(
                    browser->tabs, engine_views->navigation);
                psp_tabs_capture_thumbnail(
                    &process->presentation.tab_view, browser_tabs_active_index(browser->tabs),
                    engine_views->frame, PSP_SCREEN_WIDTH,
                    PSP_SCREEN_HEIGHT, PSP_SCREEN_WIDTH);
                psp_tabs_sync_ui(&process->presentation.ui, browser->tabs, &process->presentation.tab_view);
            }
        }
        bool voice_component_visual_changed =
            psp_voice_component_handle_frame(
                &app, &intent, frame.ui_sample_us);
        bool glyph_component_visual_changed =
            psp_glyph_component_handle_frame(
                &app, &intent, frame.ui_sample_us);
        bool update_visual_changed = false;
        if (process->presentation.ui.screen == PSP_UI_SCREEN_UPDATE
            && !psp_update_session_initialized(&browser->update_session)) {
            (void) psp_update_session_initialize(
                &browser->update_session, browser->budget, &process->install_paths,
                &(PspUpdateSessionOptions) {
                    .channel = browser_profile_update_channel(browser->profile),
                    .developer_metadata_url =
                        process->config.developer_update_url,
                    .developer_package_url =
                        process->config.developer_package_url
                },
                &process->presentation.ui);
            update_visual_changed = true;
        }
        if (intent.update_cancel_requested) {
            bool cancelled =
                psp_update_session_cancel(&browser->update_session);
            if (!cancelled)
                psp_ui_show_status(
                    &process->presentation.ui, "UPDATE IS FINISHING SAFELY", 180);
            update_visual_changed = true;
        }
        if (intent.update_primary_requested
            && psp_update_session_available(&browser->update_session)) {
            PspUpdatePrimaryResult primary =
                psp_update_session_primary(
                    &browser->update_session, &process->install_paths);
            if (primary == PSP_UPDATE_PRIMARY_RESTART_REQUIRED) {
                psp_exit_plan_request(
                    &interactive->exit, PSP_EXIT_UPDATE_RESTART);
            } else if (primary
                       == PSP_UPDATE_PRIMARY_CHECK_REQUIRED) {
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                psp_ui_set_update(
                    &process->presentation.ui, TILEFINCH_VERSION_STRING,
                    "CONNECTING...", "", -1, "", false, true);
                if (!psp_navigation_cooperate_supervised())
                    psp_present(engine_views->frame, &process->presentation.ui);
                PspUpdateSessionOptions selected_update = {
                    .channel = browser_profile_update_channel(browser->profile),
                    .developer_metadata_url =
                        process->config.developer_update_url,
                    .developer_package_url =
                        process->config.developer_package_url
                };
                char selected_update_url[768];
                bool have_selected_update_url =
                    psp_update_session_metadata_url(
                        &selected_update, selected_update_url,
                        sizeof(selected_update_url));
                bool network_ready =
                    strcmp(process->config.trace, "none") == 0
                    && have_selected_update_url
                    && psp_ensure_network_for_navigation(
                           network, network_lifecycle,
                           (int) process->config.network_profile,
                           "GET", selected_update_url, false,
                           engine_views->frame, &process->presentation.ui);
                psp_ui_set_loading(&process->presentation.ui, false, 0);
                time_t now = time(NULL);
                if (network_ready) {
                    (void) psp_update_session_begin_check(
                        &browser->update_session,
                        now > 0 ? (uint64_t) now : 0,
                        now > 0);
                }
#else
                psp_ui_set_update(
                    &process->presentation.ui, TILEFINCH_VERSION_STRING,
                    "LIVE NETWORKING IS NOT IN THIS BUILD",
                    "", -1, "", false, false);
#endif
            }
            update_visual_changed = true;
        }
        if (update_visual_changed
            && psp_update_session_initialized(&browser->update_session))
            psp_update_session_refresh_ui(&browser->update_session, &process->presentation.ui);
        if (psp_update_session_active(&browser->update_session)) {
            psp_update_session_pump(&browser->update_session, &process->presentation.ui);
            update_visual_changed =
                process->presentation.ui.screen == PSP_UI_SCREEN_UPDATE;
        } else if (
            psp_update_session_initialized(&browser->update_session)) {
            psp_update_session_refresh_ui(&browser->update_session, &process->presentation.ui);
        }
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
&& defined(TILEFINCH_PSP_LIVE_NETWORK)
        psp_update_e2e_pump(
            &update_e2e, process->config.trace, process->config.validation_update_url,
            network_lifecycle, network, &browser->update_session, browser->budget,
            &process->install_paths, &process->presentation.ui,
            &interactive->exit);
#endif
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        if (update_check_running
            && !psp_update_session_active(&browser->update_session)) {
            update_check_running = false;
            TilefinchUpdateClientPhase check_phase =
                browser->update_session.client_snapshot.phase;
            bool check_found_release =
                check_phase == TILEFINCH_UPDATE_CLIENT_AVAILABLE
                || check_phase == TILEFINCH_UPDATE_CLIENT_DOWNLOADED;
            if (check_found_release
                || check_phase
                       == TILEFINCH_UPDATE_CLIENT_UP_TO_DATE) {
                /* Only a completed check advances the twice-a-week
                   cadence; failed or cancelled attempts stay silent
                   and retry on a later boot. */
                update_check_completed++;
                time_t update_check_done = time(NULL);
                if (update_check_done > 0)
                    browser_profile_set_update_check_last_unix(
                        browser->profile, (uint64_t) update_check_done);
                if (check_found_release) {
                    update_check_available++;
                    browser_profile_set_update_check_available_sequence(
                        browser->profile,
                        browser_profile_update_channel(browser->profile)
                                == BROWSER_UPDATE_CHANNEL_DEVELOPER
                            ? TILEFINCH_UPDATE_DEVELOPER_SEQUENCE
                            : browser->update_session.client_snapshot
                                  .manifest.release_sequence);
                    process->presentation.ui.update_release_available = true;
                    if (!update_check_toasted) {
                        update_check_toasted = true;
                        psp_ui_show_status(
                            &process->presentation.ui, "UPDATE READY - SEE OPTIONS", 240);
                        navigation_visual_changed = true;
                    }
                } else {
                    browser_profile_set_update_check_available_sequence(
                        browser->profile, 0);
                    process->presentation.ui.update_release_available = false;
                }
                psp_profile_store_mark_dirty(
                    &browser->profile_store, frame.ui_sample_us);
                printf(
                    "tilefinch-update-check: completed "
                    "available=%d sequence=%llu\n",
                    check_found_release ? 1 : 0,
                    (unsigned long long)
                        browser->update_session.client_snapshot
                            .manifest.release_sequence);
            } else {
                printf(
                    "tilefinch-update-check: failed phase=%u "
                    "status=%u\n",
                    (unsigned) check_phase,
                    (unsigned)
                        browser->update_session.client_snapshot.status);
            }
        }
#endif
        frame.page_dirty = color_mode_visual_changed;
        navigation_visual_changed |= voice_component_visual_changed
            || glyph_component_visual_changed;
        frame.pointer_activation = false;
        if (!navigation_pending
            && intent.pointer_phase != PSP_UI_POINTER_NONE) {
            bool pointer_page_changed = false;
            if (!browser_engine_pointer_event(
                browser->engine,
                (ControllerPointerPhase) intent.pointer_phase,
                intent.pointer_x, intent.pointer_y,
                &frame.pointer_activation, &pointer_page_changed)) {
                frame.pointer_activation = false;
            }
            frame.page_dirty = pointer_page_changed;
            if (intent.pointer_phase == PSP_UI_POINTER_UP
                && frame.pointer_activation) {
                intent.action = PSP_UI_ACTION_ACTIVATE;
            }
        }
        if (intent.action == PSP_UI_ACTION_RELOAD
            && browser_profile_experimental_voice_input(browser->profile)) {
            ControllerTextInputInfo voice_info = {0};
            if (browser_engine_text_input_info(browser->engine, &voice_info)
                && voice_info.editable && voice_info.voice_allowed) {
                intent.action = PSP_UI_ACTION_VOICE_FOCUSED_TEXT;
            }
        }
        const char *discrete_action =
            psp_ui_action_name(intent.action);
        uint32_t discrete_operation = 0;
        if (intent.action != PSP_UI_ACTION_NONE) {
            discrete_operation =
                psp_log_operation_begin(discrete_action);
        }
        const char *action_ack =
            psp_ui_action_acknowledgement(intent.action);
        bool predispatch_presented = cursor_feedback_presented;
        bool predispatch_complete = cursor_feedback_presented;
        if (action_ack != NULL
            && !psp_navigation_cooperate_supervised()) {
            psp_ui_show_status(&process->presentation.ui, action_ack, 90);
            /* Input receipt and operation completion are separate visual
               events. Publish this one before dispatching the action. */
            psp_present(engine_views->frame, &process->presentation.ui);
            predispatch_presented = true;
            printf("tilefinch-input-ack: action=%s immediate=yes\n",
                   discrete_action);
        } else if (psp_ui_intent_has_predispatch_visual(&intent)
                   && input.pressed != 0
                   && !psp_navigation_cooperate_supervised()) {
            /* Menus/options and cursor movement already changed visible
               UI state. Page-directed focus and scrolling are excluded:
               their new pixels do not exist until after dispatch. */
            psp_present(engine_views->frame, &process->presentation.ui);
            predispatch_presented = true;
            predispatch_complete =
                psp_ui_intent_predispatch_is_complete(&intent);
        }

        /* Avoid command-dispatch fan-in on the overwhelmingly common idle
           frame; the receiver lives out of line in another translation unit. */
        if (intent.action != PSP_UI_ACTION_NONE)
            psp_app_dispatch_action(&app, &frame, &intent);
        if (discrete_operation != 0) {
            psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
            psp_log_operation_end(
                discrete_operation, discrete_action, "returned");
        }

        bool screenshot_visual_changed = false;
        if (interactive->screenshot.writer.status == SCREENSHOT_PNG_PENDING) {
            ScreenshotPngStatus screenshot_status =
                screenshot_png_pump(&interactive->screenshot.writer, 4);
            unsigned per_mille =
                screenshot_png_progress_per_mille(&interactive->screenshot.writer);
            unsigned tenth = per_mille / 100u;
            if (screenshot_status == SCREENSHOT_PNG_PENDING
                && tenth > interactive->screenshot.reported_tenth) {
                interactive->screenshot.reported_tenth = tenth;
                char screenshot_status_text[40];
                snprintf(
                    screenshot_status_text,
                    sizeof(screenshot_status_text),
                    "SAVING SCREENSHOT %u%%", tenth * 10u);
                psp_ui_show_status(
                    &process->presentation.ui, screenshot_status_text, 240);
                screenshot_visual_changed = true;
            } else if (screenshot_status
                           == SCREENSHOT_PNG_COMPLETE) {
                char saved_name[48];
                const char *saved_basename = strrchr(
                    interactive->screenshot.writer.final_path, '/');
                saved_basename = saved_basename == NULL
                    ? interactive->screenshot.writer.final_path
                    : saved_basename + 1;
                snprintf(
                    saved_name, sizeof(saved_name),
                    "SAVED %.40s", saved_basename);
                printf(
                    "tilefinch-screenshot: event=complete path=\"%s\"\n",
                    interactive->screenshot.writer.final_path);
                budget_free(browser->budget, interactive->screenshot.pixels);
                interactive->screenshot.pixels = NULL;
                screenshot_png_cancel(&interactive->screenshot.writer);
                psp_ui_show_status(
                    &process->presentation.ui, saved_name, 300);
                screenshot_visual_changed = true;
            } else if (screenshot_status == SCREENSHOT_PNG_FAILED) {
                printf(
                    "tilefinch-screenshot: event=failed error=\"%s\"\n",
                    screenshot_png_error(&interactive->screenshot.writer));
                /* Ask before the cancel below removes the partial
                   temporary and gives the space back. */
                bool screenshot_stick_full =
                    psp_screenshot_space_short(process->install_paths.data_dir);
                budget_free(browser->budget, interactive->screenshot.pixels);
                interactive->screenshot.pixels = NULL;
                screenshot_png_cancel(&interactive->screenshot.writer);
                psp_ui_show_status(
                    &process->presentation.ui,
                    screenshot_stick_full
                        ? "MEMORY STICK FULL - SCREENSHOT NOT SAVED"
                        : "SCREENSHOT SAVE FAILED",
                    240);
                screenshot_visual_changed = true;
            }
        }

        if (intent.setting.id != PSP_UI_SETTING_NONE)
            psp_app_apply_setting(&app, &frame, &intent);
        if (intent.setting.id == PSP_UI_SETTING_UPDATE_CHANNEL) {
            /* The transport endpoint is immutable for one client. The
               next Update-page entry recreates it for the selected
               channel; there is no cross-channel fallback. */
            psp_update_session_destroy(&browser->update_session);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            update_check_running = false;
            update_check_pending =
                browser_profile_update_check_enabled(browser->profile)
                && strcmp(process->config.trace, "none") == 0
                && (tilefinch_update_root_is_configured()
                    || (browser_profile_update_channel(browser->profile)
                            == BROWSER_UPDATE_CHANNEL_DEVELOPER
                        && process->config.developer_update_url[0] != '\0'));
#endif
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        /* After both receivers, so the line records a screen the
           dispatch has already moved to rather than the one it left. */
        psp_input_script_observe(&intent, &process->presentation.ui);
#endif
        if (intent.load_content_blocker_allowlist_requested) {
            BrowserProfileAllowlistImport imported = {0};
            size_t previous_allowed =
                browser_profile_content_blocker_allowed_site_count(
                    browser->profile);
            bool loaded =
                browser_profile_import_content_blocker_allowed_sites(
                    browser->profile, process->storage.content_allowlist, &imported)
                && psp_content_blocker_apply_allowed_sites(
                       browser->engine, browser->profile);
            if (!loaded)
                psp_content_blocker_restore_allowed_site_count(
                    browser->profile, previous_allowed);
            if (loaded && imported.added != 0)
                psp_profile_store_mark_dirty(
                    &browser->profile_store, frame.ui_sample_us);
            process->presentation.ui.content_blocker_site_allowed =
                process->presentation.ui.content_blocker_mode != CONTENT_BLOCKER_OFF
                && browser_profile_content_blocker_site_allowed(
                       browser->profile, process->presentation.ui.url);
            if (loaded) {
                const NavigationEntry *entry =
                    navigation_current(engine_views->navigation);
                (void) psp_set_presentation_css(
                    browser->engine, &process->presentation.ui, browser->profile, process->presentation.ui.reader_mode,
                    entry == NULL ? process->presentation.ui.url : entry->url,
                    process->presentation.ui.page_font_percent, true);
                (void) psp_engine_views_refresh(engine_views, browser->engine);
                frame.page_dirty = true;
            }
            char status[72];
            if (!loaded) {
                snprintf(status, sizeof(status),
                         "ALLOWLIST FILE NOT AVAILABLE");
            } else if (imported.resident_full) {
                snprintf(status, sizeof(status),
                         "ALLOWLIST FULL - %u ADDED",
                         (unsigned) imported.added);
            } else {
                snprintf(status, sizeof(status),
                         "ALLOWLIST LOADED - %u ADDED",
                         (unsigned) imported.added);
            }
            psp_ui_show_status(&process->presentation.ui, status, 240);
            printf(
                "tilefinch-content-blocker: import added=%zu "
                "duplicate=%zu ignored=%zu full=%s truncated=%s\n",
                imported.added, imported.duplicate, imported.ignored,
                imported.resident_full ? "yes" : "no",
                imported.truncated ? "yes" : "no");
        }
        if (intent.clear_cache_requested) {
            (void) browser_session_persistence_clear(
                browser->session, BROWSER_SESSION_PERSIST_CACHE);
            /* The TLS session store holds resumption secrets, so CLEAR
               HTTP CACHES erases it too (README privacy note). */
            (void) fetch_tls_session_store_clear();
            BrowserSessionPersistenceStatus clear_status =
                process->persistent_site_data_available
                    ? browser_session_persistence_remove(
                          process->storage.persistent_cache)
                    : BROWSER_SESSION_PERSISTENCE_OK;
            psp_ui_show_status(
                &process->presentation.ui,
                clear_status == BROWSER_SESSION_PERSISTENCE_OK
                    ? "HTTP CACHES CLEARED"
                    : "CACHE FILE COULD NOT BE CLEARED",
                240);
        }
        if (intent.clear_cookies_requested) {
            browser_session_cookie_clear(browser->session);
            psp_ui_show_status(&process->presentation.ui, "COOKIES CLEARED", 180);
        }
        if (intent.clear_local_storage_requested) {
            (void) browser_session_persistence_clear(
                browser->session, BROWSER_SESSION_PERSIST_LOCAL_STORAGE);
            BrowserSessionPersistenceStatus clear_status =
                process->persistent_site_data_available
                    ? browser_session_persistence_remove(
                          process->storage.local_storage)
                    : BROWSER_SESSION_PERSISTENCE_OK;
            psp_ui_show_status(
                &process->presentation.ui,
                clear_status == BROWSER_SESSION_PERSISTENCE_OK
                    ? "LOCAL STORAGE CLEARED"
                    : "LOCAL FILE COULD NOT BE CLEARED",
                240);
        }
        if (intent.clear_session_storage_requested) {
            browser_session_storage_clear_all(browser->session, false);
            psp_ui_show_status(
                &process->presentation.ui, "SESSION STORAGE CLEARED", 180);
        }

        if (intent.scroll_delta != 0) {
            bool log_scroll = (scroll_log_counter++ & 31u) == 0;
            uint32_t scroll_operation = log_scroll
                ? psp_log_operation_begin("analog-scroll") : 0;
            bool provisional = psp_request_provisional_scroll(
                browser->engine, &process->presentation.ui, intent.scroll_delta);
            if (!provisional) {
                frame.page_dirty = browser_engine_scroll_by(
                    browser->engine, intent.scroll_delta)
                    || frame.page_dirty;
            }
            if (scroll_operation != 0) {
                psp_log_operation_end(
                    scroll_operation, "analog-scroll",
                    provisional ? "provisional"
                                : (frame.page_dirty
                                       ? "changed" : "unchanged"));
            }
        }
        if (intent.scroll_settle) {
            frame.page_dirty = browser_engine_scroll_settle(browser->engine)
                || frame.page_dirty;
        }

        if (browser_engine_navigation_pending(browser->engine)) {
            /* A closed player retains its native decoder only for a fast
               replay on the same watch page. Release it before the first
               pump of any other candidate so the candidate receives the
               full contiguous-memory budget instead of failing before the
               old route is replaced at commit. */
            (void) psp_media_reclaim_hidden_pipeline_for_navigation(
                &browser->media, browser_engine_pending_navigation_url(browser->engine));
            if (!blocker_navigation_active) {
                ContentBlockerMetrics blocker_metrics = {0};
                blocker_navigation_baseline =
                    browser_engine_content_blocker_metrics(
                        browser->engine, &blocker_metrics)
                    ? blocker_metrics.requests_blocked : 0;
                blocker_navigation_active = true;
                process->presentation.ui.page_requests_blocked = 0;
            }
            psp_log_set_phase(PSP_LOG_PHASE_NAVIGATION);
            psp_log_heartbeat();
            if (!psp_navigation_cooperate_active()) {
                psp_navigation_cooperate_begin(
                    &process->presentation.ui, engine_views->frame, browser->engine);
            }
            BrowserNavigationJobQuota navigation_quota =
                psp_navigation_quota();
            BrowserNavigationJobStatus navigation_status =
                browser_engine_pump_navigation(
                    browser->engine, &navigation_quota);
            /* Same cooperation point the boot loop marks: this loop
               polls the pad every pass, so the gap between two pumps is
               not a blind window and must not be charged as one. */
            navigation_pump_boundaries++;
            if (!tilefinch_platform_cooperate(
                    "navigation-pump", navigation_pump_boundaries)
                && navigation_status
                       == BROWSER_NAVIGATION_JOB_PENDING) {
                browser_engine_cancel_navigation(
                    browser->engine, "cancelled from PSP browser UI");
                navigation_status = BROWSER_NAVIGATION_JOB_CANCELLED;
            }
            if (psp_navigation_cancel_requested()) {
                if (navigation_status
                        == BROWSER_NAVIGATION_JOB_PENDING) {
                    browser_engine_cancel_navigation(
                        browser->engine, "cancelled from PSP browser UI");
                }
                navigation_status =
                    BROWSER_NAVIGATION_JOB_CANCELLED;
            }
            BrowserNavigationJobMetrics navigation_metrics = {0};
            (void) browser_engine_navigation_job_metrics(
                browser->engine, &navigation_metrics);
            psp_ui_set_loading(
                &process->presentation.ui, true,
                (int) navigation_metrics.completion_per_mille);
            navigation_visual_changed = true;
            uint64_t now_us =
                (uint64_t) sceKernelGetSystemTimeWide();
            if (navigation_status == BROWSER_NAVIGATION_JOB_PENDING
                && interactive->navigation_job_started_us != 0
                && now_us - interactive->navigation_job_started_us
                       >= PSP_NAVIGATION_JOB_TIMEOUT_US) {
                browser_engine_cancel_navigation(
                    browser->engine, "PSP navigation watchdog expired");
                navigation_status =
                    BROWSER_NAVIGATION_JOB_CANCELLED;
            }
            if (navigation_status
                    != BROWSER_NAVIGATION_JOB_PENDING) {
                ContentBlockerMetrics blocker_metrics = {0};
                size_t page_blocked = 0;
                if (browser_engine_content_blocker_metrics(
                        browser->engine, &blocker_metrics)) {
                    page_blocked = blocker_metrics.requests_blocked
                            >= blocker_navigation_baseline
                        ? blocker_metrics.requests_blocked
                              - blocker_navigation_baseline
                        : blocker_metrics.requests_blocked;
                }
                blocker_navigation_active = false;
                psp_navigation_cooperate_end("interactive");
                (void) psp_engine_views_refresh(engine_views, browser->engine);
                const NavigationEntry *reader_entry =
                    navigation_current(engine_views->navigation);
                psp_reader_navigation_finish(
                    browser->engine, &process->presentation.ui,
                    browser->profile, &interactive->reader_navigation,
                    reader_entry == NULL ? NULL : reader_entry->url,
                    navigation_status == BROWSER_NAVIGATION_JOB_SUCCEEDED);
                (void) psp_engine_views_refresh(engine_views, browser->engine);
                psp_ui_set_loading(
                    &process->presentation.ui, false,
                    navigation_status
                            == BROWSER_NAVIGATION_JOB_SUCCEEDED
                        ? 1000 : 0);
                if (navigation_status
                        == BROWSER_NAVIGATION_JOB_SUCCEEDED) {
                    interactive->lifecycle_retry_available = false;
                    process->presentation.ui.page_requests_blocked =
                        page_blocked > UINT32_MAX
                        ? UINT32_MAX : (uint32_t) page_blocked;
                    browser_profile_record_content_blocked(
                        browser->profile, page_blocked);
                    uint64_t total_blocked =
                        browser_profile_content_blocker_total_blocked(
                            browser->profile);
                    process->presentation.ui.total_requests_blocked =
                        total_blocked > UINT32_MAX
                        ? UINT32_MAX : (uint32_t) total_blocked;
                    if (page_blocked != 0)
                        psp_profile_store_mark_dirty(
                            &browser->profile_store, frame.ui_sample_us);
                    const NavigationEntry *loaded_entry =
                        navigation_current(engine_views->navigation);
                    const char *loaded_url = loaded_entry == NULL
                        ? process->presentation.ui.url : loaded_entry->url;
                    if (!process->presentation.ui.reader_mode
                        && browser_profile_reader_site_always(
                               browser->profile, loaded_url)) {
                        unsigned reader_percent =
                            browser_profile_page_font_percent(browser->profile);
                        if (process->presentation.ui.remember_reader_site_scale)
                            (void) browser_profile_reader_site_font_percent(
                                browser->profile, loaded_url, &reader_percent);
                        if (psp_set_presentation_css(
                                browser->engine, &process->presentation.ui, browser->profile, true, loaded_url,
                                reader_percent, true)) {
                            process->presentation.ui.reader_mode = true;
                            process->presentation.ui.page_font_percent = reader_percent;
                            (void) psp_engine_views_refresh(
                                engine_views, browser->engine);
                            frame.page_dirty = true;
                        }
                    }
                    bool tab_restored = true;
                    if (interactive->tab_transition.pending) {
                        tab_restored = psp_tabs_finish(
                            browser->tabs, browser->engine, &interactive->tab_transition, true,
                            browser_profile_tab_hibernation_enabled(
                                browser->profile),
                            process->storage.tab_hibernation);
                        (void) psp_engine_views_refresh(
                            engine_views, browser->engine);
                    } else if (browser->tabs != NULL) {
                        tab_restored =
                            browser_tabs_capture_active(
                                browser->tabs, engine_views->navigation);
                    }
                    psp_tabs_sync_ui(&process->presentation.ui, browser->tabs, &process->presentation.tab_view);
                    frame.page_dirty = true;
                    psp_profile_record_current(
                        browser->profile, &browser->profile_store, engine_views->navigation,
                        frame.ui_sample_us);
                    if (psp_recovery_record_current(
                            browser->profile, process->storage.recovery, engine_views->navigation)) {
                        interactive->recovery.entry =
                            navigation_current(engine_views->navigation);
                        interactive->recovery.observed_scroll =
                            interactive->recovery.entry == NULL
                                ? 0 : interactive->recovery.entry->scroll_y;
                        interactive->recovery.observed_generation =
                            engine_views->navigation->generation;
                        interactive->recovery.last_change_us =
                            (uint64_t) sceKernelGetSystemTimeWide();
                        interactive->recovery.dirty = false;
                    }
                    char ready_status[PSP_UI_STATUS_CAPACITY];
                    if (page_blocked != 0) {
                        snprintf(
                            ready_status, sizeof(ready_status),
                            page_blocked > 999u
                                ? "PAGE READY - 999+ BLOCKED"
                                : "PAGE READY - %zu BLOCKED",
                            page_blocked);
                    } else {
                        snprintf(
                            ready_status, sizeof(ready_status), "%s",
                            tab_restored
                                ? "PAGE READY"
                                : "PAGE READY - TAB STATE LIMITED");
                    }
                    psp_ui_show_status(&process->presentation.ui, ready_status, 120);
                } else {
                    if (interactive->tab_transition.pending) {
                        (void) psp_tabs_finish(
                            browser->tabs, browser->engine, &interactive->tab_transition, false,
                            browser_profile_tab_hibernation_enabled(
                                browser->profile),
                            process->storage.tab_hibernation);
                        psp_tabs_sync_ui(&process->presentation.ui, browser->tabs, &process->presentation.tab_view);
                    }
                    if (navigation_status
                            != BROWSER_NAVIGATION_JOB_CANCELLED) {
                        (void) psp_write_navigation_failure_report(
                            "navigation",
                            browser_engine_last_error(browser->engine), process->presentation.ui.url,
                            engine_views->navigation);
                    }
                    psp_ui_show_status(
                        &process->presentation.ui,
                        navigation_status
                                == BROWSER_NAVIGATION_JOB_CANCELLED
                            ? "PAGE LOAD STOPPED"
                            : browser_engine_last_error(browser->engine),
                        300);
                    /* Keep the actionable failure in its own bounded
                       record. The attribution line below intentionally
                       carries many counters and may be truncated by the
                       fixed-size device logger before its tail. */
                    psp_report_job_failure(
                        "navigation", NULL, (int) navigation_status,
                        engine_views->navigation->last_http_status,
                        browser_engine_last_error(browser->engine));
                }
                printf("tilefinch-navigation-job: status=%d "
                       "http=%ld server=\"%.32s\" mitigated=\"%.16s\" "
                       "pumps=%zu body=%zuB yields=%zu "
                       "max-pump=%lluus parser-max=%lluus "
                       "preview=%zu/%zu first=%lluus "
                       "scrolls=%zu y=%d bytes=%zu "
                       "headers=%lluus first-body=%lluus "
                       "first-dom=%lluus source=%zu nodes=%zu "
                       "style-refresh=%lluus "
                       "style-builds=%zu continuation=%zu/%zu/%lluus "
                       "rules=%zu+%zu discovery=%lluus "
                       "context=%lluus append=%lluus "
                       "refresh-failures=%zu "
                       "parser=%lluus runtime=%lluus css=%lluus "
                       "script=%lluus/%lluus "
                       "finalize=%lluus transform=%lluus/%zu/%zu "
                       "irreducible=%lluus/%zu "
                       "elapsed=%lluus system-free=%d "
                       "system-largest=%d\n",
                       (int) navigation_status,
                       engine_views->navigation->last_http_status,
                       engine_views->navigation->last_server,
                       engine_views->navigation->last_cf_mitigated,
                       navigation_metrics.pump_calls,
                       navigation_metrics.load.body_bytes,
                       navigation_metrics.load.quota_yields,
                       (unsigned long long)
                           navigation_metrics.load.maximum_pump_us,
                       (unsigned long long)
                           navigation_metrics.load
                               .maximum_parser_pump_us,
                       navigation_metrics.provisional_paints,
                       navigation_metrics.provisional_frame_count,
                       (unsigned long long)
                           navigation_metrics
                               .provisional_first_present_us,
                       navigation_metrics.provisional_scrolls,
                       navigation_metrics.provisional_scroll_y,
                       navigation_metrics.provisional_bytes,
                       (unsigned long long)
                           engine_views->navigation->performance.response_headers_us,
                       (unsigned long long)
                           engine_views->navigation->performance.first_body_byte_us,
                       (unsigned long long)
                           engine_views->navigation->performance.first_dom_us,
                       engine_views->navigation->performance
                           .streaming_preview_source_bytes,
                       engine_views->navigation->performance
                           .streaming_preview_node_count,
                       (unsigned long long)
                           engine_views->navigation->performance
                               .streaming_preview_style_refresh_us,
                       engine_views->navigation->performance
                           .blocking_stylesheet_builds,
                       engine_views->navigation->performance
                           .blocking_stylesheet_continuations,
                       engine_views->navigation->performance
                           .blocking_stylesheet_continuation_fallbacks,
                       (unsigned long long)
                           engine_views->navigation->performance
                               .blocking_stylesheet_continuation_us,
                       engine_views->navigation->performance
                           .blocking_stylesheet_continuation_rules_before,
                       engine_views->navigation->performance
                           .blocking_stylesheet_continuation_rules,
                       (unsigned long long)
                           engine_views->navigation->performance
                               .blocking_stylesheet_continuation_discovery_us,
                       (unsigned long long)
                           engine_views->navigation->performance
                               .blocking_stylesheet_continuation_context_us,
                       (unsigned long long)
                           engine_views->navigation->performance
                               .blocking_stylesheet_continuation_append_us,
                       engine_views->navigation->performance
                           .streaming_preview_style_refresh_failures,
                       (unsigned long long)
                           engine_views->navigation->performance.parser_feed_us,
                       (unsigned long long)
                           engine_views->navigation->performance
                               .parser_runtime_startup_us,
                       (unsigned long long)
                           engine_views->navigation->performance
                               .parser_stylesheet_us,
                       (unsigned long long)
                           engine_views->navigation->performance
                               .parser_script_compile_us,
                       (unsigned long long)
                           engine_views->navigation->performance
                               .parser_script_execute_us,
                       (unsigned long long)
                           navigation_metrics.load.finalize_us,
                       (unsigned long long)
                           navigation_metrics
                               .maximum_transform_slice_us,
                       navigation_metrics.transform_slices,
                       navigation_metrics.transform_quota_overruns,
                       (unsigned long long)
                           navigation_metrics
                               .maximum_irreducible_unit_us,
                       navigation_metrics.irreducible_unit_overruns,
                       (unsigned long long)
                           navigation_metrics.elapsed_us,
                           sceKernelTotalFreeMemSize(),
                           sceKernelMaxFreeMemSize());
                psp_report_blocking_script_samples(
                    &engine_views->navigation->performance);
                psp_report_background_transport_metrics();
                if (navigation_status
                        != BROWSER_NAVIGATION_JOB_SUCCEEDED
                    && navigation_status
                        != BROWSER_NAVIGATION_JOB_CANCELLED) {
                    psp_log_checkpoint(
                        "interactive-navigation-failed");
                }
                interactive->navigation_job_started_us = 0;
            }
            psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
        }

        uint32_t offline_download_id = 0;
        bool offline_download_active = offline_download_manager_active(
            &browser->offline_store.download, &offline_download_id);
        if (offline_download_active && !browser->media.ui.visible) {
            /* Saving a video is mutually exclusive with playback on this
               memory budget. A closed decoder is a cache, not a reason to
               leave the queued download permanently unpumped. */
            (void) psp_media_reclaim_hidden_pipeline(&browser->media);
        }
        if (offline_download_active
            && !browser_engine_navigation_pending(browser->engine)
            && !psp_media_open_work_pending(&browser->media)
            && !psp_media_decode_work_pending(&browser->media)
            && browser->media.playback == NULL
            && !psp_navigation_cooperate_active()) {
            psp_ui_set_loading(&process->presentation.ui, true, -1);
            psp_ui_show_status(&process->presentation.ui, "SAVING VIDEO  O PAUSE", 120);
            psp_work_cooperate_begin(
                &process->presentation.ui, engine_views->frame, true, true, false,
                "PAUSING DOWNLOAD...", "offline-download", NULL, NULL);
            bool completed = psp_offline_store_pump(&browser->offline_store);
            bool cancelled = psp_navigation_cancel_requested();
            uint32_t observed_buttons =
                psp_ui_buttons(psp_navigation_observed_buttons());
            psp_navigation_cooperate_end("offline-download");
            interactive->previous_buttons = observed_buttons;
            if (cancelled)
                (void) offline_download_manager_pause(
                    &browser->offline_store.download, offline_download_id);
            uint32_t remaining_id = 0;
            bool remains_active = offline_download_manager_active(
                &browser->offline_store.download, &remaining_id);
            const OfflineLibraryItem *download_item =
                offline_library_find(
                    &browser->offline_store.library,
                    remains_active ? remaining_id : offline_download_id);
            int progress = -1;
            if (download_item != NULL) {
                uint64_t total = download_item->content_bytes
                    + download_item->audio_bytes;
                if (total != 0)
                    progress = (int) psp_ui_ratio_extent_u64(
                        download_item->downloaded_bytes, total, 1000u);
            }
            psp_ui_set_loading(&process->presentation.ui, remains_active, progress);
            if (cancelled || completed || !remains_active)
                psp_ui_show_status(
                    &process->presentation.ui, cancelled ? "VIDEO DOWNLOAD PAUSED"
                                   : psp_offline_store_status(
                                         &browser->offline_store),
                    240);
        }

        bool runtime_layout_changed = false;
        psp_log_heartbeat();
        bool runtime_ok = browser_engine_advance_runtime(
            browser->engine, (unsigned) process->config.tick_ms, 2,
            &runtime_layout_changed);
        (void) runtime_ok;
        frame.page_dirty = runtime_layout_changed || frame.page_dirty;
        if (frame.page_dirty) {
            if (!render_job_pending) {
                render_job_last_progress_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
            }
            render_job_pending = true;
            browser_engine_cancel_idle_work(browser->engine);
        } else if (!render_job_pending) {
            (void) browser_engine_run_idle_work(browser->engine);
        }
        const NavigationEntry *current_entry =
            navigation_current(engine_views->navigation);
        psp_media_prepare_route(
            &browser->media, current_entry == NULL ? NULL : current_entry->url,
            engine_views->navigation->generation);
        psp_log_set_phase(PSP_LOG_PHASE_MEDIA);
        psp_log_heartbeat();
        bool media_open_before = psp_media_open_work_pending(&browser->media);
        bool navigation_still_pending =
            browser_engine_navigation_pending(browser->engine);
        if (media_open_before && !navigation_still_pending
            && !psp_navigation_cooperate_active()) {
            psp_work_cooperate_begin_media_open(
                &process->presentation.ui, engine_views->frame, &browser->media.ui);
            media_open_scope = true;
        }
        bool media_decode_scope = false;
        if (!media_open_before && !navigation_still_pending
            && psp_media_decode_work_pending(&browser->media)
            && !psp_navigation_cooperate_active()) {
            psp_work_cooperate_begin(
                &process->presentation.ui, engine_views->frame, false, false, false,
                "STOPPING VIDEO...",
                "video-decoder-submit", NULL, &browser->media.ui);
            media_decode_scope = true;
        }
        /* Do not start a second blocking-origin operation while a page
           candidate owns the cancellation token and UI supervisor. */
        if (!media_open_before || !navigation_still_pending) {
            media_visual_changed =
                psp_media_advance(
                    &browser->media, media_elapsed_ms,
                    psp_navigation_cooperate_active()
                        ? psp_navigation_cancellation() : NULL)
                || media_visual_changed;
        } else if (psp_media_open_watchdog(&browser->media)) {
            /* The suppressed frames are still frames the open's deadline
               and the cancel law run on. Without this an open that nothing
               was pumping had neither. */
            media_visual_changed = true;
        }
        if (media_open_before && psp_navigation_cooperate_active())
            psp_work_cooperate_refresh_media(&browser->media.ui);
        bool stability_needs_play =
            interactive->media_stability_active
            && process->config.input_script[0] == '\0'
            && browser->media.playback != NULL && browser->media.have_frame
            && !browser->media.pause_boundary_pending && !browser->media.ui.playing
            && browser->media.job_phase == PSP_MEDIA_JOB_NONE
            && !browser->media.ui.ended;
        /* Autoplay for the validation measurement engages the instant the
           open settles -- playback live, no job in flight -- rather than
           waiting for a first frame. The resume-open leg this path takes
           decodes nothing on its own, so a have-frame gate here left
           playing=0 forever and the panel never rendered, which is useless
           to measure. PLAY_PAUSE clears pause-after-next-frame and starts
           the decoder against the connection media-open already warmed, so
           there is no idle gap and no handshake inside the measured window.
           The injected flag and the not-already-playing gate keep this to a
           single press even if a script also sends play. */
        bool validation_autoplay =
            process->config.validation_media_play != 0
            && !interactive->validation_media_play_injected
            && browser->media.playback != NULL
            && browser->media.job_phase == PSP_MEDIA_JOB_NONE
            && !browser->media.ui.playing
            && !browser->media.ui.ended;
        bool natural_autoplay_ready =
            ((process->config.validation_media_play != 0
              && !interactive->validation_media_play_injected)
             || (interactive->media_stability_active
                 && interactive->media_stability_started_us == 0))
            && browser->media.playback != NULL
            && browser->media.job_phase == PSP_MEDIA_JOB_NONE
            && browser->media.ui.playing
            && !browser->media.ui.ended;
        if (validation_autoplay || stability_needs_play
            || natural_autoplay_ready) {
            bool injected_play = validation_autoplay || stability_needs_play;
            if (injected_play) {
                PspUiMediaIntent play_intent = {
                    .action = PSP_UI_MEDIA_ACTION_PLAY_PAUSE
                };
                psp_media_execute_intent(&browser->media, play_intent);
            }
            if (process->config.validation_media_play != 0)
                interactive->validation_media_play_injected = true;
            if (interactive->media_stability_active
                && interactive->media_stability_started_us == 0) {
                interactive->media_stability_started_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
                interactive->media_stability_next_sample_us =
                    interactive->media_stability_started_us + UINT64_C(250000);
                interactive->media_stability_start_capacity =
                    scePowerGetBatteryRemainCapacity();
                interactive->media_stability_start_percent =
                    scePowerGetBatteryLifePercent();
                unsigned starting_free = sceKernelTotalFreeMemSize();
                unsigned starting_largest = sceKernelMaxFreeMemSize();
                interactive->media_stability_min_free = starting_free;
                interactive->media_stability_min_largest = starting_largest;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                psp_power_log_battery(
                    "media-start", PSP_POWER_TEST_FIXED_HIGH, 0);
#endif
                printf(
                    "tilefinch-media-stability: event=started "
                    "requested=%up actual=%dx%d itag=%d "
                    "system-free=%u system-largest=%u\n",
                    (unsigned) browser->media.requested_quality,
                    browser->media.stream.width, browser->media.stream.height,
                    browser->media.stream.itag,
                    starting_free, starting_largest);
            }
            media_visual_changed = true;
            printf("tilefinch-media-validation: play-source=%s "
                   "after-open=1\n",
                   injected_play ? "validation-input" : "route-autoplay");
        }
        if (process->config.validation_media_play != 0
            && !interactive->media_stability_active
            && interactive->validation_media_play_injected
            && !interactive->validation_media_play_confirmed
            && browser->media.clock_us >= UINT64_C(500000)) {
            MediaBackendStats backend_stats = {0};
            if (media_playback_backend_stats(
                    browser->media.playback, &backend_stats)
                && backend_stats.decoded_video_frames >= 2
                && backend_stats.submitted_audio_packets != 0) {
                interactive->validation_media_play_confirmed = true;
                printf("tilefinch-media-validation: playback-confirmed "
                       "clock=%lluus video-frames=%zu "
                       "audio-packets=%zu video-us=%lluus "
                       "audio-origin-us=%lluus audio-blocks=%lu "
                       "audio-rate=%u native-error=%d\n",
                       (unsigned long long) browser->media.clock_us,
                       backend_stats.decoded_video_frames,
                       backend_stats.submitted_audio_packets,
                       (unsigned long long)
                           backend_stats.presented_video_us,
                       (unsigned long long)
                           backend_stats.audio_origin_us,
                       backend_stats.audio_output_blocks,
                       backend_stats.audio_sample_rate,
                       backend_stats.last_native_error);
                psp_exit_plan_request(
                    &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
            }
        }
        if (interactive->media_stability_active) {
            if (browser->media.ui.failed
                && !psp_media_open_work_pending(&browser->media)
                && browser->media.job_phase == PSP_MEDIA_JOB_NONE) {
                printf(
                    "tilefinch-media-stability: event=failed "
                    "reason=\"%.160s\"\n",
                    browser->media.ui.status);
                interactive->media_stability_active = false;
                process->presentation.ui.validation_media_test_phase = 0;
                if (interactive->media_stability_auto_exit)
                    psp_exit_plan_request(
                        &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
            }
        }
        if (interactive->media_stability_active) {
            uint64_t stability_now_us =
                (uint64_t) sceKernelGetSystemTimeWide();
            uint64_t stability_elapsed_us =
                interactive->media_stability_started_us == 0 ? 0
                    : stability_now_us - interactive->media_stability_started_us;
            psp_sample_media_stability(&app, stability_now_us);
            uint64_t duration_us =
                psp_media_duration_us(&browser->media);
            psp_media_stability_schedule_seeks(
                &app, stability_elapsed_us, duration_us);
            if (browser->media.ui.ended && browser->media.playback != NULL
                && browser->media.job_phase == PSP_MEDIA_JOB_NONE
                && stability_elapsed_us
                       < media_stability_target_us
                && psp_media_request_seek(&browser->media, 0, false)) {
                browser->media.job_resume_playing = true;
                interactive->media_stability_skew.steady_since_us = 0;
                interactive->media_stability_loops++;
                printf(
                    "tilefinch-media-stability: event=loop count=%u\n",
                    interactive->media_stability_loops);
            }
            if (interactive->media_stability_started_us != 0
                && stability_elapsed_us
                       >= media_stability_target_us) {
                psp_finish_media_stability(
                    &app, stability_elapsed_us);
            }
        }
        if (media_decode_scope) {
            bool cancelled = psp_navigation_cancel_requested();
            uint32_t observed_buttons =
                psp_ui_buttons(psp_navigation_observed_buttons());
            if (cancelled) psp_media_close(&browser->media);
            if (cancelled) media_visual_changed = true;
            psp_navigation_cooperate_end("video-decoder-submit");
            interactive->previous_buttons = observed_buttons;
            if (cancelled)
                psp_ui_show_status(&process->presentation.ui, "VIDEO CLOSED", 180);
        }
        /*
         * End the scope on this loop's ownership, never on a re-derived
         * "an open is still in flight". The interactive half of the same
         * frame can retire the open job before this point is reached --
         * a CIRCLE press dispatches PSP_UI_MEDIA_ACTION_CLOSE, which
         * clears the open service and job phase. Keying the end on the
         * job then skipped it forever: the callback-thread supervisor
         * kept repainting a frozen "OPENING VIDEO"/"DECODING FIRST FRAME"
         * player over every later frame and answered every later press
         * with "VIDEO IS STILL STOPPING", so Back could never get the
         * user back to the page. A scope another site already closed
         * (system suspend, interactive navigation cancel) is released
         * here too rather than being ended twice.
         */
        if (media_open_scope) {
            bool scope_owned = psp_navigation_cooperate_active();
            bool cancelled =
                scope_owned && psp_navigation_cancel_requested();
            if (!scope_owned || cancelled || navigation_still_pending
                || !psp_media_open_work_pending(&browser->media)) {
                uint32_t observed_buttons =
                    psp_ui_buttons(psp_navigation_observed_buttons());
                if (cancelled) {
                    psp_media_close(&browser->media);
                    media_visual_changed = true;
                }
                if (scope_owned) {
                    psp_navigation_cooperate_end("media-open");
                    interactive->previous_buttons = observed_buttons;
                }
                media_open_scope = false;
                /* The indicator belongs to whoever still owns the
                   surface: a navigation that took it over re-asserts
                   its own progress every frame. */
                if (!navigation_still_pending)
                    psp_ui_set_loading(&process->presentation.ui, false, 0);
                if (cancelled) {
                    psp_ui_show_status(
                        &process->presentation.ui, "VIDEO OPEN STOPPED", 180);
                }
            }
        }
        psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);

        psp_sync_ui(&process->presentation.ui, browser->engine, browser->profile);
        if (process->presentation.ui.cursor_shape != cursor_shape_before_dispatch)
            navigation_visual_changed = true;
        psp_find_sync(browser->engine, &process->presentation.ui, &process->presentation.find_view);
        bool render_visual_changed = false;
        if (render_job_pending) {
            psp_log_set_phase(PSP_LOG_PHASE_RENDER);
            psp_log_heartbeat();
            bool render_scope = false;
            if (!psp_navigation_cooperate_active()) {
                psp_work_cooperate_begin(
                    &process->presentation.ui, engine_views->frame, false, true, false,
                    "STOPPING PAGE UPDATE...",
                    "render-raster-unit", NULL, NULL);
                render_scope = true;
            }
            const TileCache *render_stats =
                browser_engine_render_metrics_view(browser->engine);
            size_t units_before = render_stats == NULL ? 0
                : render_stats->frame_job_units;
            BrowserRenderJobStatus render_status =
                browser_engine_render_frame_bounded_cancelable(
                    browser->engine, PSP_RENDER_JOB_BUDGET_US,
                    PSP_RENDER_JOB_MAXIMUM_TILES,
                    render_scope
                        ? psp_navigation_cancellation() : NULL);
            uint64_t render_now_us =
                (uint64_t) sceKernelGetSystemTimeWide();
            if (render_stats != NULL
                && render_stats->frame_job_units > units_before) {
                render_job_last_progress_us = render_now_us;
            }
            bool render_cancelled =
                render_status == BROWSER_RENDER_JOB_CANCELLED;
            if (render_scope) {
                uint32_t observed_buttons =
                    psp_ui_buttons(psp_navigation_observed_buttons());
                psp_navigation_cooperate_end("render-raster-unit");
                interactive->previous_buttons = observed_buttons;
            }
            if (render_status == BROWSER_RENDER_JOB_FAILED) {
                psp_report_job_failure(
                    "render", "interactive-render-failed",
                    (int) render_status, 0,
                    browser_engine_last_error(browser->engine));
                psp_ui_show_status(
                    &process->presentation.ui, browser_engine_last_error(browser->engine), 300);
                render_job_pending = false;
                render_job_last_progress_us = 0;
                render_visual_changed = true;
            } else if (render_cancelled) {
                render_job_pending = false;
                render_job_last_progress_us = 0;
                /*
                 * A cancellation may have arrived while the final
                 * compositor was irreducibly in flight. Keep the last
                 * completed scanout authoritative instead of publishing
                 * that unrequested candidate frame.
                 */
                frame.page_dirty = false;
                render_visual_changed = false;
                psp_ui_show_status(
                    &process->presentation.ui, "PAGE UPDATE STOPPED", 180);
                psp_present_supervisor_ui(engine_views->frame, &process->presentation.ui);
            } else if (render_status == BROWSER_RENDER_JOB_COMPLETE) {
                render_job_pending = false;
                render_job_last_progress_us = 0;
                render_visual_changed = true;
                (void) psp_engine_views_refresh(engine_views, browser->engine);
            } else {
                if (render_job_last_progress_us != 0
                    && render_now_us - render_job_last_progress_us
                           >= PSP_RENDER_JOB_TIMEOUT_US) {
                    browser_engine_cancel_render_job(browser->engine);
                    render_job_pending = false;
                    render_job_last_progress_us = 0;
                    render_visual_changed = true;
                    psp_ui_show_status(
                        &process->presentation.ui, "PAGE PAINT STOPPED", 300);
                    printf("tilefinch-render-job: no-progress-timeout "
                           "slices=%zu units=%zu max-slice=%lluus "
                           "max-unit=%lluus\n",
                           render_stats == NULL ? 0
                               : render_stats->frame_job_slices,
                           render_stats == NULL ? 0
                               : render_stats->frame_job_units,
                           (unsigned long long) (render_stats == NULL ? 0
                               : render_stats->max_frame_job_slice_us),
                           (unsigned long long) (render_stats == NULL ? 0
                               : render_stats->max_frame_job_unit_us));
                }
            }
            psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
        }
        if (browser_profile_restore_last_page(browser->profile)) {
            interactive->recovery.entry = navigation_current(engine_views->navigation);
            uint64_t recovery_now_us =
                (uint64_t) sceKernelGetSystemTimeWide();
            bool external_page =
                interactive->recovery.entry != NULL
                && interactive->recovery.entry->url != NULL
                && strncmp(
                       interactive->recovery.entry->url,
                       "https://tilefinch.local/",
                       strlen("https://tilefinch.local/")) != 0;
            if (external_page
                && (engine_views->navigation->generation
                        != interactive->recovery.observed_generation
                    || interactive->recovery.entry->scroll_y
                        != interactive->recovery.observed_scroll)) {
                interactive->recovery.dirty = true;
                interactive->recovery.observed_generation =
                    engine_views->navigation->generation;
                interactive->recovery.observed_scroll =
                    interactive->recovery.entry->scroll_y;
                interactive->recovery.last_change_us = recovery_now_us;
            }
            if (interactive->recovery.dirty && external_page
                && recovery_now_us - interactive->recovery.last_change_us
                       >= UINT64_C(2000000)
                && (interactive->recovery.last_save_us == 0
                    || recovery_now_us - interactive->recovery.last_save_us
                           >= UINT64_C(30000000))
                && psp_recovery_record_current(
                       browser->profile, process->storage.recovery, engine_views->navigation)) {
                interactive->recovery.last_change_us = recovery_now_us;
                interactive->recovery.last_save_us = recovery_now_us;
                interactive->recovery.dirty = false;
            }
        }
        bool profile_flush_attempted = false;
        if (!psp_profile_store_flush_due(
                &browser->profile_store, frame.ui_sample_us, UINT64_C(750000),
                &profile_flush_attempted)
            && profile_flush_attempted) {
            printf("tilefinch-profile: deferred save failed\n");
        }
        if (frame.page_dirty || render_visual_changed
            || (intent.visual_changed
                && !(predispatch_presented && predispatch_complete))
            || media_visual_changed
            || (navigation_visual_changed
                && !cursor_feedback_presented)
            || update_visual_changed || screenshot_visual_changed) {
            if (!psp_navigation_cooperate_supervised()) {
                psp_log_set_phase(PSP_LOG_PHASE_RENDER);
                psp_present(engine_views->frame, &process->presentation.ui);
                psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
            }
        }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        /* The callback supervisor owns both scripted advancement and
           capture while cooperative work is active. Keeping the main
           thread out of the same three-slot capture array also makes the
           validation harness obey the product's single-owner rule. */
        if (!psp_navigation_cooperate_supervised()) {
            psp_input_script_capture_live_mark(
                psp_display_front_buffer(&psp_display),
                PSP_DISPLAY_BUFFER_PIXELS, PSP_DISPLAY_STRIDE);
        }
#endif
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        /* One supervisor service unit runs after input and presentation.
           Foreground connect drives the same ladder with the larger
           demand quota, so background warmup cannot add cold-nav delay. */
        bool network_was_warming =
            psp_network_lifecycle_warming(network_lifecycle);
        PspNetworkStatus previous_network_status = network->status;
        psp_log_set_phase(PSP_LOG_PHASE_NETWORK);
        psp_network_lifecycle_pump(
            network_lifecycle, network, frame.ui_sample_us);
        psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
        if (network_was_warming
            && network->status != previous_network_status) {
            printf("tilefinch-network-warmup-stage: "
                   "status=%s apctl=%d elapsed=%llums pump=%lluus\n",
                   psp_network_status_name(network->status),
                   network->apctl_state,
                   (unsigned long long) (network->elapsed_us / 1000u),
                   (unsigned long long) network->last_pump_us);
        }
        if (network_was_warming
            && !psp_network_lifecycle_warming(network_lifecycle)) {
            if (network->status == PSP_NETWORK_READY
                && psp_network_lifecycle_ready(network_lifecycle)) {
                    printf("tilefinch-network-warmup: status=ready "
                           "elapsed=%llums\n",
                           (unsigned long long)
                               (network->elapsed_us / 1000u));
                    psp_report_network_result(network);
                    /* The selected HOME tile has already remained stable
                       throughout association, far longer than the 300 ms
                       speculative dwell. Start its connect-only work now
                       instead of charging another post-association delay;
                       the shared worker keeps DNS/TCP/TLS off this loop. */
                    const char *url = psp_home_target_url(
                        &process->presentation.home_surface, process->presentation.ui.home_selection, browser->profile);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
                    printf("tilefinch-preconnect: event=selected "
                           "selection=%u url=%s\n",
                           (unsigned) process->presentation.ui.home_selection,
                           url == NULL ? "-" : url);
#endif
                    uint32_t key = url == NULL ? 0u
                        : fetch_preconnect_tile_key(url);
                    FetchPreconnectDwellAction action =
                        fetch_preconnect_dwell_step(
                            home_preconnect_dwell, key != 0u, key,
                            PSP_HOME_PRECONNECT_DWELL_MS,
                            PSP_HOME_PRECONNECT_DWELL_MS);
                    if (action == FETCH_PRECONNECT_DWELL_START)
                        (void) fetch_preconnect(url, browser->budget);
            } else {
                printf("tilefinch-network-warmup: status=%s "
                       "retry=on-navigation\n",
                       psp_network_status_name(network->status));
                psp_report_network_result(network);
            }
        }
#endif
        if (process->config.interactive_validation_ticks > 0
            && ++interactive_validation_ticks
                   >= process->config.interactive_validation_ticks) {
            if (process->config.validation_media_play != 0
                && !interactive->validation_media_play_confirmed) {
                MediaBackendStats backend_stats = {0};
                bool have_stats = media_playback_backend_stats(
                    browser->media.playback, &backend_stats);
                printf("tilefinch-media-validation: "
                       "playback-not-confirmed injected=%d "
                       "clock=%lluus have-stats=%d video-frames=%zu "
                       "audio-packets=%zu native-error=%d\n",
                       interactive->validation_media_play_injected ? 1 : 0,
                       (unsigned long long) browser->media.clock_us,
                       have_stats ? 1 : 0,
                       backend_stats.decoded_video_frames,
                       backend_stats.submitted_audio_packets,
                       backend_stats.last_native_error);
            }
            printf("tilefinch-validation: interactive-loops=%ld "
                   "background-pending=%d render-pending=%d\n",
                   interactive_validation_ticks,
                   navigation_background_resources_pending(engine_views->navigation)
                       ? 1 : 0,
                   render_job_pending ? 1 : 0);
            psp_log_checkpoint("interactive-validation-complete");
            psp_exit_plan_request(
                &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
        }
    }
    if (!psp_exit_plan_requested(&interactive->exit)
        && psp_home_exit_pending()) {
        psp_exit_plan_request(
            &interactive->exit, PSP_EXIT_HOME_CALLBACK);
    }
    /* HOME can become pending between loop iterations while a media-open
       operation deliberately retains the presentation supervisor. The
       loop condition then bypasses the per-frame release block. Fence the
       callback presenter before destroying media, frame, or engine state;
       psp_navigation_cooperate_end waits for an in-flight presentation. */
    if (psp_navigation_cooperate_active())
        psp_navigation_cooperate_end(
            media_open_scope ? "media-open-exit" : "interactive-exit");
    media_open_scope = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_input_script_summary();
    if (interactive->power_test.phase != PSP_POWER_TEST_OFF) {
        PspClockWorkerSnapshot current_power = {0};
        if (process->clock_live)
            psp_clock_worker_snapshot(
                &process->clock_worker, &current_power);
        uint64_t now_us =
            (uint64_t) sceKernelGetSystemTimeWide();
        PspPowerTestResult result = psp_power_test_finish(
            &interactive->power_test,
            now_us,
            process->clock_live ? &current_power : NULL,
            "browser-exit");
        if (interactive->power_auto.active) {
            psp_power_auto_accumulate(&interactive->power_auto, &result);
            interactive->power_auto.active = false;
            psp_power_auto_log_summary(
                &interactive->power_auto, now_us, "browser-exit");
        }
        process->presentation.ui.validation_power_test_phase = 0;
    }
#endif
    result.update_check_attempts = update_check_attempts;
    result.update_check_ratelimited = update_check_ratelimited;
    result.update_check_completed = update_check_completed;
    result.update_check_available = update_check_available;
    result.power_high_ms = power_high_ms;
    result.power_idle_ms = power_idle_ms;
    result.power_transition_ms = power_transition_ms;
    return result;
}


/* Controlled browser teardown. State machines decide when their resources are
   releasable; this coordinator records any retained physical obligation and
   never overrides a quarantine or live transport lease. */
static TILEFINCH_COLD_PATH PspShutdownReport psp_browser_close(
    PspProcessResources *process, PspBrowserResources *browser,
    PspInteractiveState *interactive, PspEngineViews *engine_views,
    const PspInteractiveResult *interactive_result
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    , PspNetwork *network, PspNetworkLifecycle *network_lifecycle
#endif
)
{
    PspShutdownReport shutdown_report = {0};
    if (process->clock_live) {
        uint64_t request_us = 0;
        bool requested = psp_clock_worker_request(
            &process->clock_worker, false, &request_us);
        bool applied = requested
            && psp_clock_worker_wait_applied(
                &process->clock_worker, false, 500);
        printf(
            "tilefinch-power-cleanup: high-request=%d "
            "request=%lluus applied=%d\n",
            requested ? 1 : 0,
            (unsigned long long) request_us, applied ? 1 : 0);
    }
    psp_log_set_phase(PSP_LOG_PHASE_CLEANUP);
    printf("tilefinch-update-check: attempts=%u ratelimited=%u "
           "completed=%u available=%u\n",
           interactive_result->update_check_attempts,
           interactive_result->update_check_ratelimited,
           interactive_result->update_check_completed,
           interactive_result->update_check_available);
    printf("tilefinch-validation: outcome=exit-requested "
           "free-mem=%d max-free=%d\n",
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    psp_log_checkpoint("cleanup-begin");

#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /* Release any parked speculative connection (and its transport
       reference) so the shared transport tears down cleanly at exit. */
    fetch_preconnect_cancel("shutdown");
#endif

    screenshot_png_cancel(&interactive->screenshot.writer);
    budget_free(browser->budget, interactive->screenshot.pixels);
    interactive->screenshot.pixels = NULL;

    uint32_t cleanup_operation =
        psp_log_operation_begin("safe-memory-report");
    psp_report_budget_counters(browser->budget, "controlled-exit");
    psp_log_operation_end(
        cleanup_operation, "safe-memory-report", "ok");
    psp_log_set_phase(PSP_LOG_PHASE_CLEANUP);

    cleanup_operation =
        psp_log_operation_begin("media-cleanup");
    psp_offline_store_destroy(&browser->offline_store);
    bool media_resume_changed =
        psp_media_record_resume(&browser->media, false);
    psp_active_media = NULL;
    psp_media_shutdown(&browser->media);
    if (media_psp_backend_quarantined())
        shutdown_report.retained |=
            PSP_SHUTDOWN_RETAIN_MEDIA_BACKEND;
    if (psp_media_present_ge_stage_dma_quarantined())
        shutdown_report.retained |= PSP_SHUTDOWN_RETAIN_MEDIA_DMA;
    if (browser->media.seek_preview_pixels != NULL) {
        budget_free(browser->media.budget, browser->media.seek_preview_pixels);
        browser->media.seek_preview_pixels = NULL;
    }
    psp_log_operation_end(
        cleanup_operation, "media-cleanup", "ok");

    cleanup_operation =
        psp_log_operation_begin("text-input-cleanup");
    psp_text_input_shutdown(&process->text_input);
    psp_log_operation_end(
        cleanup_operation, "text-input-cleanup", "ok");

    cleanup_operation =
        psp_log_operation_begin("site-data-save");
    bool site_data_saved = psp_save_site_data_on_exit(
        browser->session, browser->profile,
        process->persistent_site_data_available,
        &process->install_paths, &process->storage,
        &process->presentation.ui, engine_views->frame);
    psp_log_operation_end(
        cleanup_operation, "site-data-save",
        site_data_saved ? "ok" : "save-failed");

    cleanup_operation =
        psp_log_operation_begin("profile-cleanup");
    if (browser->tabs != NULL)
        (void) browser_tabs_capture_active(browser->tabs, engine_views->navigation);
    bool tab_session_saved =
        !browser_profile_restore_last_page(browser->profile)
        || browser->tabs == NULL
        || browser_tabs_save_session(
            browser->tabs, process->storage.tab_session,
            process->storage.tab_hibernation);
    (void) psp_recovery_record_current(
        browser->profile, process->storage.recovery, engine_views->navigation);
    if (media_resume_changed)
        psp_profile_store_mark_dirty(
            &browser->profile_store,
            (uint64_t) sceKernelGetSystemTimeWide());
    bool profile_saved = psp_profile_store_flush(&browser->profile_store);
    if (!profile_saved)
        printf("tilefinch-profile: cleanup save failed\n");
    browser_profile_destroy(browser->profile);
    browser->profile = NULL;
    psp_log_operation_end(
        cleanup_operation, "profile-cleanup",
        profile_saved && tab_session_saved
            ? "saved" : "save-failed");

    BrowserEngineMetrics exit_engine_metrics = {0};
    if (browser_engine_metrics(browser->engine, &exit_engine_metrics)) {
        printf("tilefinch-render-suppression: "
               "unchanged-runtime-frames=%zu rendered-frames=%zu\n",
               exit_engine_metrics.unchanged_runtime_frames_suppressed,
               exit_engine_metrics.rendered_frames);
    }
    psp_report_presentation_cadence("controlled-exit");
    cleanup_operation =
        psp_log_operation_begin("engine-cleanup");
    psp_update_session_destroy(&browser->update_session);
    psp_voice_component_session_destroy(
        browser->voice_component_session);
    psp_glyph_component_session_destroy(
        browser->glyph_component_session);
    (void) remove(process->storage.tab_hibernation);
    browser_tabs_destroy(browser->tabs);
    browser->tabs = NULL;
    psp_presentation_unbind_chrome_fonts(&process->presentation);
    browser_engine_destroy(browser->engine);
    browser->engine = NULL;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /* Engine teardown releases the last authoritative schedulers and
       exports their newest TLS sessions into the in-memory generation.
       Flush only after that state is complete. */
    if (!fetch_tls_session_store_flush())
        printf("tilefinch-tls-session: cleanup save failed\n");
#endif
    psp_log_operation_end(
        cleanup_operation, "engine-cleanup", "ok");
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /* The engine owns schedulers which can still hold transport request
       IDs. Terminate the shared worker only after those owners have been
       destroyed, and never unload sceNet under a worker that did not
       join. A process exit can safely retain that stack; terminating it
       underneath curl recreates the cross-thread firmware hazard the
       network supervisor is intended to exclude. */
    cleanup_operation =
        psp_log_operation_begin("transport-cleanup");
    bool background_transport_stopped =
        fetch_background_transport_shutdown(4000u);
    if (!background_transport_stopped) {
        shutdown_report.retained |=
            PSP_SHUTDOWN_RETAIN_TRANSPORT
            | PSP_SHUTDOWN_RETAIN_NETWORK_STACK;
    }
    psp_log_operation_end(
        cleanup_operation, "transport-cleanup",
        background_transport_stopped ? "ok" : "retained-stack");
    if (!background_transport_stopped) {
        printf("tilefinch-transport-worker: shutdown=timed-out "
               "action=retain-network-stack\n");
    }
    if (background_transport_stopped
        && psp_network_lifecycle_started(network_lifecycle)) {
        cleanup_operation =
            psp_log_operation_begin("network-cleanup");
        psp_network_lifecycle_request(
            network_lifecycle,
            PSP_NETWORK_REQUEST_SHUTDOWN_INHIBIT, true, 0,
            network, PSP_NETWORK_SUPERVISOR_STOPPING,
            "exit-off");
        bool network_stopped = psp_shutdown_network_logged(network);
        if (!network_stopped)
            shutdown_report.retained |=
                PSP_SHUTDOWN_RETAIN_NETWORK_STACK;
        psp_log_operation_end(
            cleanup_operation, "network-cleanup",
            network_stopped ? "ok" : "retained-stack");
    } else if (!background_transport_stopped
               && psp_network_lifecycle_started(network_lifecycle)) {
        printf("tilefinch-network-shutdown: skipped=transport-worker "
               "action=retain-stack\n");
    }
    psp_network_lifecycle_report(network_lifecycle);
#endif
    if (process->clock_live) {
        PspClockWorkerSnapshot power_snapshot;
        psp_clock_worker_snapshot(&process->clock_worker, &power_snapshot);
        bool stopped = psp_clock_worker_shutdown(&process->clock_worker);
        printf(
            "tilefinch-power-summary: completions=%u failures=%u "
            "maximum=%luus high=%llums idle=%llums "
            "transition=%llums stopped=%d\n",
            power_snapshot.completions, power_snapshot.failures,
            power_snapshot.maximum_transition_us,
            (unsigned long long) interactive_result->power_high_ms,
            (unsigned long long) interactive_result->power_idle_ms,
            (unsigned long long) interactive_result->power_transition_ms,
            stopped ? 1 : 0);
        process->clock_live = false;
    }
    psp_log_set_phase(PSP_LOG_PHASE_CLEANUP);
    printf(
        "tilefinch-shutdown: exit-cause=%u retained=0x%08x\n",
        (unsigned) interactive->exit.cause,
        (unsigned) shutdown_report.retained);
    printf("tilefinch-validation: outcome=clean-exit "
           "free-mem=%d max-free=%d\n",
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    psp_log_checkpoint("cleanup-complete");
    psp_log_finish("clean-exit");
    return shutdown_report;
}

int main(int argc, char *argv[])
{
    PspProcessResources process = {0};
    PspBrowserResources browser = {0};
    PspInteractiveState interactive = {
        .media_stability_min_free = UINT_MAX,
        .media_stability_min_largest = UINT_MAX,
        .media_stability_start_capacity = INT_MIN,
        .media_stability_start_percent = INT_MIN
    };
    const char *argv0 = argc > 0 ? argv[0] : NULL;
    if (!tilefinch_install_paths_derive(argv0, &process.install_paths))
        memset(&process.install_paths, 0, sizeof(process.install_paths));
    psp_failure_report_paths = &process.install_paths;
    if (process.install_paths.slotted) (void) mkdir(process.install_paths.data_dir, 0777);
    uint64_t main_entered_us = (uint64_t) sceKernelGetSystemTimeWide();
    atomic_init(&psp_home_exit_requested, false);
    psp_lifecycle_init(&psp_lifecycle);
    /*
     * Paint before touching the Memory Stick log, callbacks, or network
     * modules.  Log rotation alone took about 5.6 seconds on the physical
     * device, so presenting later left a misleading black screen even when
     * startup was healthy.
     */
    bool display_ready =
        psp_display_begin(&psp_display, psp_display_system_backend());
    /* Frame one of the entrance: the mark alone. Nothing else has been read
       from the Memory Stick yet. */
    /* Keep pre-input boot completely static. The old entrance rebuilt and
       composited animated wave bands between slow initialization stages,
       making startup look jerky while no input could yet be acknowledged. */
    psp_present_boot_entrance(0u, "STARTING TILEFINCH", false, NULL);
    uint64_t boot_surface_presented_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    int callback_result = -1;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    char log_identity[TILEFINCH_INSTALL_PATH_LIMIT];
    const char *log_argv0 = argv0;
    int log_identity_length = process.install_paths.slotted
        ? snprintf(
              log_identity, sizeof(log_identity), "%s/EBOOT.PBP",
              process.install_paths.data_dir) : -1;
    if (log_identity_length > 0
        && (size_t) log_identity_length < sizeof(log_identity))
        log_argv0 = log_identity;
    bool persistent_log_started = psp_log_start(log_argv0);
    if (!persistent_log_started) {
        fputs("tilefinch: persistent validation logging unavailable\n",
              stdout);
        fflush(stdout);
    }
    psp_present_boot_entrance(
        1u,
        persistent_log_started
            ? "DIAGNOSTIC LOG: TILEFINCH/DATA"
            : "DIAGNOSTIC LOG UNAVAILABLE",
        false, NULL);
#else
    bool persistent_log_started = false;
#endif
    psp_log_set_phase(PSP_LOG_PHASE_BOOT);
    int exception_handler_result = psp_log_install_exception_handler();
    bool watchdog_started = psp_log_start_watchdog(15000);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork network = {0};
    PspNetworkLifecycle network_lifecycle;
    psp_network_lifecycle_init(&network_lifecycle);
    psp_network_lifecycle_bind(&network_lifecycle);
#endif
    callback_result = psp_setup_callbacks();
    int clock_result = scePowerSetClockFrequency(333, 333, 166);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /*
     * Battery-aware clocking remains a validation experiment until matched,
     * counterbalanced device runs demonstrate a benefit.  The ordinary
     * release therefore keeps the established fixed 333 MHz behavior and
     * does not spend even the clock worker's 8 KiB stack.
     */
    process.clock_live =
        clock_result >= 0 && psp_clock_worker_start(&process.clock_worker);
#endif
    PspPowerPolicy power_policy;
    psp_power_policy_init(
        &power_policy,
        (PspPowerPolicyBackend) {
            .set_clock = process.clock_live
                ? psp_request_policy_clock : NULL,
            .context = &process.clock_worker
        });
    int sampling_cycle_result = sceCtrlSetSamplingCycle(0);
    int sampling_mode_result = sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    static const TilefinchPlatformServices services = {
        .context = &psp_navigation_cooperate,
        .wall_time_ns = psp_wall_time_ns,
        .monotonic_time_ns = psp_time_ns,
        .monotonic_time_us = psp_time_us,
        .cooperate = psp_platform_cooperate,
        .present_rgb565 = psp_platform_present,
        .log_message = psp_log_message,
    };
    tilefinch_platform_set_services(&services);
    /*
     * Reserve the Media Engine's memory before anything else can grow the
     * heap. The extra RAM bank a custom firmware unlocks starts at 0x0A000000
     * and only the main CPU can address it, so a media buffer allocated later
     * -- after a page, its images, and a JavaScript heap have moved the
     * cursor -- can land where the Media Engine is physically unable to read
     * it, and both firmware codecs then fail on perfectly valid input. This
     * must therefore stay above every other allocation in boot order. The
     * backend owns the sizing and the logging so this stays a single call.
     */
    media_psp_backend_reserve_pool();

    printf("tilefinch-validation: version=3 persistent=%d "
           "crash-journal=%d operation-input=redacted "
           "exception-handler=%s/0x%08x "
           "watchdog=%d callback=0x%08x clock=0x%08x controls=0x%08x/"
           "0x%08x\n",
           persistent_log_started ? 1 : 0,
           persistent_log_started ? 1 : 0,
           exception_handler_result >= 0 ? "installed" : "user-unavailable",
           (unsigned) exception_handler_result,
           watchdog_started ? 1 : 0, (unsigned) callback_result,
           (unsigned) clock_result, (unsigned) sampling_cycle_result,
           (unsigned) sampling_mode_result);
    printf("tilefinch-power: worker=%s idle-delay=%ums\n",
           process.clock_live ? "ready" : "unavailable",
           PSP_POWER_POLICY_IDLE_DELAY_MS);
    printf("tilefinch-psp-script: boot devkit=0x%08x "
           "cpu=%dMHz bus=%dMHz free-mem=%d max-free=%d "
           "main-stack=2560KB heap-threshold=2048KB\n",
           (unsigned) sceKernelDevkitVersion(),
           scePowerGetCpuClockFrequencyInt(),
           scePowerGetBusClockFrequencyInt(),
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_clock_validation_probe();
    psp_power_log_battery("boot", PSP_POWER_TEST_OFF, 0);
    uint64_t validation_free_space = 0;
    bool validation_free_space_ok =
        tilefinch_update_query_free_space(
            process.install_paths.program_dir, &validation_free_space);
    printf(
        "tilefinch-update-storage: boot-probe=%s available=%llu "
        "directory=%s\n",
        validation_free_space_ok ? "ok" : "failed",
        (unsigned long long) validation_free_space,
        process.install_paths.program_dir);
#endif
    /* Validation records the real newlib heap ceiling. It does not influence
       budget geometry, so ordinary boots avoid allocating and freeing up to
       64 MiB before their first page. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    {
        enum { PROBE_LIMIT = 64 };
        void *blocks[PROBE_LIMIT];
        int count = 0;
        while (count < PROBE_LIMIT) {
            blocks[count] = malloc(1024 * 1024);
            if (blocks[count] == NULL) break;
            count++;
        }
        for (int i = 0; i < count; i++) free(blocks[i]);
        printf("tilefinch-psp-script: heap-capacity=%dMB\n", count);
    }
#endif
    /* The boot surface was painted before the log existed, so report the
       scanout outcome here.  A browser nobody can see must not look like a
       healthy boot. */
    printf("tilefinch-display: ready=%d mode-error=0x%08x presents=%u "
           "main-to-first-present=%lluus rejected=%u "
           "first-error=0x%08x\n",
           display_ready ? 1 : 0, (unsigned) psp_display.mode_error,
           psp_display.presents,
           (unsigned long long) (
               boot_surface_presented_us >= main_entered_us
                   ? boot_surface_presented_us - main_entered_us : 0),
           psp_display.rejections,
           (unsigned) psp_display.first_error);
    psp_log_checkpoint("boot-ready");
    psp_present_boot_entrance(3u, "READING SETTINGS", false, NULL);

    psp_log_set_phase(PSP_LOG_PHASE_CONFIG);
    psp_boot_config_defaults(&process.config);
    char path[768];
    bool config_loaded = false;
    if (process.install_paths.slotted) {
        if (tilefinch_install_program_path(
                &process.install_paths, "boot-defaults.cfg",
                path, sizeof(path))) {
            config_loaded = psp_boot_config_load(
                &process.config, path, psp_config_warning, NULL);
        }
        char overrides_path[TILEFINCH_INSTALL_PATH_LIMIT];
        bool have_overrides_path = tilefinch_install_data_path(
            &process.install_paths, "boot-overrides.cfg",
            overrides_path, sizeof(overrides_path));
        bool overrides_loaded = have_overrides_path
            && psp_boot_config_load(
                &process.config, overrides_path, psp_config_warning, NULL);
        if (!overrides_loaded) {
            char legacy_path[TILEFINCH_INSTALL_PATH_LIMIT];
            int legacy_length = snprintf(
                legacy_path, sizeof(legacy_path), "%s/boot-live.cfg",
                process.install_paths.install_root);
            if (legacy_length > 0
                && (size_t) legacy_length < sizeof(legacy_path)
                && psp_boot_config_load(
                    &process.config, legacy_path, psp_config_warning, NULL)) {
                overrides_loaded = have_overrides_path
                    && psp_boot_config_write_overrides(
                        &process.config, overrides_path);
                printf("tilefinch-config: legacy-overrides=%s\n",
                       overrides_loaded ? "migrated" : "active-not-saved");
            }
        }
        config_loaded = config_loaded || overrides_loaded;
    } else {
        psp_sibling_path(path, sizeof(path), argv0, "boot.cfg");
        config_loaded = psp_boot_config_load(
            &process.config, path, psp_config_warning, NULL);
    }
#ifndef TILEFINCH_PSP_VALIDATION_LOG
    /* A shipping EBOOT never executes a copied validation scenario. This is
       intentionally after both configuration layers: stale automation in
       either one is inert, while user-facing URL/network/update choices keep
       their ordinary precedence. */
    psp_boot_config_disable_automation(&process.config);
#endif
    if (!config_loaded)
        printf("tilefinch-psp: boot configuration missing, using defaults\n");
    /* Keep existing Memory Stick installs with `url=` compatible with the
       built-in start page, while an explicit boot.cfg URL remains an
       intentional direct-navigation override. */
    if (process.config.url[0] == '\0') {
        snprintf(process.config.url, sizeof(process.config.url), "%s",
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
        psp_boot_config_drop_invalid_developer_urls(&process.config);
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
    if (psp_boot_config_drop_unusable_exit_to(&process.config)) {
        printf("tilefinch-config: ignoring unusable exit_to; this EBOOT "
               "will exit to the XMB\n");
    }
#endif
    const char *invalid_field = NULL;
    if (!psp_boot_config_validate(&process.config, &invalid_field)) {
        printf("tilefinch-psp-script: invalid boot configuration field=%s\n",
               invalid_field == NULL ? "unknown" : invalid_field);
        goto sleep_forever;
    }
    /* Publish the experimental decoder knob before anything can open a video.
       The backend owns the parse so this boot path stays a single call. */
    media_psp_backend_set_wide_program(process.config.experimental_wide_video);
    media_psp_backend_set_au_dump(
        process.config.validation_media_au_dump != 0 ? 1 : 0);
    /* And how a refused access unit is recovered, published here for the same
       reason and in the same breath: boot decides it, and it has to be decided
       before anything can open a video. */
    psp_media_set_refusal_reset(
        process.config.validation_media_refusal_reset != 0);
    media_psp_backend_set_reset_mode(
        (int) process.config.validation_media_reset_mode);
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
           "validation-media-reset-mode=%ld\n",
           process.config.url, process.config.trace, process.config.ticks, process.config.limit_mb,
           process.config.heap_mb, process.config.window_kb, process.config.profile,
           process.config.network_profile, process.config.dump_frame,
           process.config.validation_cancel_after_ms,
           process.config.validation_preview_scroll,
           process.config.validation_media_stability_auto,
           process.config.validation_media_fixture_auto,
           process.config.validation_raster_fixture_auto,
           process.config.validation_power_test_auto,
           process.config.input_script[0] == '\0' ? "none" : process.config.input_script,
           process.config.validation_media_seek_permille,
           process.config.validation_media_lifecycle_auto,
           process.config.validation_media_refusal_reset,
           process.config.validation_media_reset_mode);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-psp-script: exit-to=%s\n",
           process.config.exit_to[0] == '\0' ? "none" : process.config.exit_to);
#endif
    psp_log_checkpoint("config-accepted");
    /* The bypass is restricted to probes that require an initial document.
       Passive automation must observe the shipping entrance and native HOME
       or it cannot validate what users actually run. */
    bool boot_url_overridden =
        !psp_ui_native_home_url(process.config.url);
    bool boot_trace_replay = strcmp(process.config.trace, "none") != 0;
    bool boot_validation_mode =
        psp_boot_config_automation_requires_engine_first(&process.config);
    bool deterministic_boot = boot_url_overridden || boot_trace_replay
        || boot_validation_mode;
    if (deterministic_boot) {
        psp_present_boot_surface(
            PSP_UI_STARTUP_HOMEPAGE, "LOADING BROWSER", 420);
    } else {
        psp_present_boot_entrance(6u, "LOADING BROWSER", false, NULL);
    }
    if (strcmp(process.config.trace, "none") == 0) {
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        psp_log_set_phase(PSP_LOG_PHASE_ASSETS);
        char ca_bundle_path[768];
        if (!tilefinch_install_program_path(
                &process.install_paths, "roots.pem",
                ca_bundle_path, sizeof(ca_bundle_path))) goto sleep_forever;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        size_t ca_bundle_bytes = 0;
        uint8_t ca_bundle_digest[TILEFINCH_SHA256_DIGEST_BYTES];
        if (!psp_probe_file(
                ca_bundle_path, 256u * KIB, &ca_bundle_bytes,
                ca_bundle_digest)) {
            printf("tilefinch-network: trust bundle missing: %s\n",
                   ca_bundle_path);
            goto sleep_forever;
        }
#endif
        if (!fetch_set_ca_bundle_path(ca_bundle_path)) {
            printf("tilefinch-network: trust bundle configuration failed\n");
            goto sleep_forever;
        }
        time_t tls_time = time(NULL);
        if (tls_time <= 0) {
            printf("tilefinch-network: RTC unavailable for TLS\n");
            goto sleep_forever;
        }
        ScePspDateTime rtc = {0};
        int rtc_result = sceRtcGetCurrentClock(&rtc, 0);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        printf("tilefinch-network: trust-bytes=%zu trust-sha256="
               "%02x%02x%02x%02x%02x%02x%02x%02x "
               "verification=required utc-epoch=%ld "
               "rtc=%04u-%02u-%02uT%02u:%02u:%02uZ rtc-result=0x%08x "
               "curl=%s tls=%s http2=%s http2-enabled=%u\n",
               ca_bundle_bytes,
               ca_bundle_digest[0], ca_bundle_digest[1],
               ca_bundle_digest[2], ca_bundle_digest[3],
               ca_bundle_digest[4], ca_bundle_digest[5],
               ca_bundle_digest[6], ca_bundle_digest[7],
               (long) tls_time, rtc.year, rtc.month, rtc.day,
               rtc.hour, rtc.minute, rtc.second,
               (unsigned) rtc_result,
               fetch_transport_version(),
               fetch_transport_tls_version(),
               fetch_transport_http2_version(),
               fetch_transport_http2_available() ? 1u : 0u);
#else
        /*
         * CURLOPT_CAINFO still verifies the bundle on the first HTTPS
         * request and fails closed if it is absent or invalid. Avoid reading,
         * allocating, and hashing the entire file before a local homepage
         * that does not need transport. Validation builds retain that
         * diagnostic so device reports still attest to the copied asset.
         */
        (void) rtc;
        (void) rtc_result;
#endif
        psp_log_checkpoint("tls-assets-ready");
#else
        printf("tilefinch-network: live transport unavailable in replay "
               "build\n");
        goto sleep_forever;
#endif
    }

    /* Env-driven engine knobs (newlib setenv works in-process). */
    char scratch[32];
    if (process.config.window_kb > 0) {
        snprintf(scratch, sizeof(scratch), "%ld", process.config.window_kb);
        setenv("TILEFINCH_JS_BOOT_WINDOW_KB", scratch, 1);
    }
    if (process.config.gc_growth_pct > 0) {
        snprintf(scratch, sizeof(scratch), "%ld", process.config.gc_growth_pct);
        setenv("TILEFINCH_JS_GC_GROWTH_PCT", scratch, 1);
    }
    /* Device profile: bound any single JS array's value buffer; a page
       once grouped a multi-megabyte binary string per character, which
       can never fit beside hydration on the 50MB heap. */
    setenv("TILEFINCH_JS_ARRAY_CAP_KB", "4096", 1);

    printf("tilefinch-psp-script: env window=%s growth=%s\n",
           getenv("TILEFINCH_JS_BOOT_WINDOW_KB") == NULL
               ? "(null)" : getenv("TILEFINCH_JS_BOOT_WINDOW_KB"),
           getenv("TILEFINCH_JS_GC_GROWTH_PCT") == NULL
               ? "(null)" : getenv("TILEFINCH_JS_GC_GROWTH_PCT"));
    script_runtime_configure_deterministic_replay(
        strcmp(process.config.trace, "none") != 0, 7);

    psp_log_set_phase(PSP_LOG_PHASE_ENGINE);
    psp_log_checkpoint("engine-config-begin");
    BrowserDeviceProfile device_profile;
    browser_device_profile_psp3000(&device_profile);
    BrowserConfig *engine_config = calloc(1, sizeof(*engine_config));
    if (engine_config == NULL) {
        printf("tilefinch-psp-script: config allocation failed\n");
        goto sleep_forever;
    }
    browser_config_init(engine_config, &device_profile);
    engine_config->memory_limit = (size_t) process.config.limit_mb * MIB;
    engine_config->history_capacity = 4;
    engine_config->session_cache_limit = 512 * KIB;
    /*
     * The runaway-response guard, not a page-size policy. Four megabytes was
     * set for ordinary documents and a media watch page is not one: a device
     * cycle fetched 1.84 MiB of watch HTML successfully and tripped the cap on
     * another attempt in the same session, which cost a whole media open and
     * recovered only because the retry happened to come in under it. Eight
     * gives that document real headroom while still refusing a response no
     * page has any business being.
     */
    engine_config->maximum_document_bytes = 8 * MIB;
    engine_config->navigation_timeout_ms = 30000;
    engine_config->tile_capacity = 8;
    engine_config->javascript.enabled = true;
    engine_config->javascript.document_scripts_enabled = true;
    engine_config->javascript.heap_limit = (size_t) process.config.heap_mb * MIB;
    engine_config->javascript.runtime_timeout_ms =
        (unsigned long) process.config.script_timeout_ms;
    engine_config->javascript.maximum_scripts = (size_t) process.config.count;
    engine_config->javascript.maximum_total_bytes =
        (size_t) process.config.total_mb * MIB;
    engine_config->javascript.maximum_file_bytes =
        (size_t) process.config.file_kb * KIB;
    engine_config->javascript.network_timeout_ms = 15000;
    ScriptExecutionProfile execution_profile = SCRIPT_EXECUTION_PROFILE_LAB;
    if (strcmp(process.config.profile, "strict") == 0) {
        execution_profile = SCRIPT_EXECUTION_PROFILE_PSP_STRICT;
    } else if (strcmp(process.config.profile, "realistic") == 0) {
        execution_profile = SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC;
    }
    if (!script_execution_policy_for_profile(
            execution_profile,
            &engine_config->javascript.execution_policy)) {
        printf("tilefinch-psp-script: execution policy failed\n");
        goto sleep_forever;
    }
    size_t configured_script_file_bytes =
        engine_config->javascript.maximum_file_bytes;
    if (engine_config->javascript.maximum_file_bytes
            > engine_config->javascript.maximum_total_bytes) {
        engine_config->javascript.maximum_file_bytes =
            engine_config->javascript.maximum_total_bytes;
    }
    size_t maximum_compile_bytes =
        engine_config->javascript.execution_policy
            .maximum_host_compile_source_bytes;
    if (maximum_compile_bytes != 0
        && engine_config->javascript.maximum_file_bytes
               > maximum_compile_bytes) {
        engine_config->javascript.maximum_file_bytes =
            maximum_compile_bytes;
    }
    if (engine_config->javascript.maximum_file_bytes
            != configured_script_file_bytes) {
        printf("tilefinch-psp-script: script-file-limit clamped=%zu/%zuB\n",
               engine_config->javascript.maximum_file_bytes,
               configured_script_file_bytes);
    }
    engine_config->resources.enabled = true;
    /* Modern mobile shells split their critical component rules across many
       small sheets and occasionally one 700 KiB decoded module. Keep the
       limits hard, but retain a small tail allowance for late mobile-header
       and accessibility components. The existing parse-admission gate still
       uses actual page headroom, and strict host profiles retain their tighter
       ladder to exercise bounded degradation. */
    engine_config->resources.maximum_stylesheets = 24;
    engine_config->resources.maximum_stylesheet_bytes = 2112 * KIB;
    engine_config->resources.maximum_stylesheet_file_bytes = 768 * KIB;
    engine_config->resources.maximum_images = 24;
    engine_config->resources.maximum_image_bytes = 1536 * KIB;
    engine_config->resources.maximum_image_file_bytes = 384 * KIB;
    engine_config->resources.maximum_decoded_image_bytes = 3 * MIB;
    engine_config->resources.timeout_ms = 15000;

    char sans[300], serif[300], sans_italic[300], sans_bold[300];
    char serif_bold[300], metric_sans[300], metric_sans_bold[300];
    tilefinch_install_program_path(
        &process.install_paths, "fonts/DejaVuSans-Latin.ttf", sans, sizeof(sans));
    tilefinch_install_program_path(
        &process.install_paths, "fonts/DejaVuSerif-Latin.ttf", serif, sizeof(serif));
    tilefinch_install_program_path(
        &process.install_paths, "fonts/DejaVuSans-Oblique-Latin.ttf",
        sans_italic, sizeof(sans_italic));
    tilefinch_install_program_path(
        &process.install_paths, "fonts/DejaVuSans-Bold-Latin.ttf",
        sans_bold, sizeof(sans_bold));
    tilefinch_install_program_path(
        &process.install_paths, "fonts/DejaVuSerif-Bold-Latin.ttf",
        serif_bold, sizeof(serif_bold));
    tilefinch_install_program_path(
        &process.install_paths, "fonts/TilefinchSans-Regular.ttf",
        metric_sans, sizeof(metric_sans));
    tilefinch_install_program_path(
        &process.install_paths, "fonts/TilefinchSans-Bold.ttf",
        metric_sans_bold, sizeof(metric_sans_bold));
    if (!browser_config_set_font_paths(engine_config, sans, serif,
                                       sans_italic, sans_bold, serif_bold,
                                       metric_sans, metric_sans_bold,
                                       1536 * KIB)) {
        printf("tilefinch-psp-script: font configuration failed\n");
        goto sleep_forever;
    }

    char engine_error[256] = {0};
    browser.engine = browser_engine_create(
        engine_config, engine_error, sizeof(engine_error));
    free(engine_config);
    if (browser.engine == NULL) {
        printf("tilefinch-psp-script: engine setup failed: %s\n",
               engine_error);
        goto sleep_forever;
    }
    psp_log_checkpoint("engine-created");
    if (deterministic_boot) {
        psp_present_boot_surface(
            PSP_UI_STARTUP_HOMEPAGE, "OPENING HOME", 760);
    } else {
        psp_present_boot_entrance(9u, "OPENING HOME", false, NULL);
    }
    browser.budget = browser_engine_budget(browser.engine);
    browser.session = browser_engine_session(browser.engine);
    PspEngineViews engine_views = {0};
    (void) psp_engine_views_refresh(&engine_views, browser.engine);
    if (browser.budget == NULL || browser.session == NULL || engine_views.navigation == NULL) {
        printf("tilefinch-psp-script: engine accessors unavailable\n");
        goto sleep_forever;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (process.config.validation_media_fixture_auto != 0) {
        psp_present_boot_surface(
            PSP_UI_STARTUP_HOMEPAGE, "TESTING VIDEO HARDWARE", 860);
        PspMediaFixtureReport fixture_report = {0};
        char fixture_error[256] = {0};
        bool fixture_passed = psp_media_fixture_run(
            browser.budget, &fixture_report,
            fixture_error, sizeof(fixture_error));
        bool emulator_untested = !fixture_passed
            && (fixture_report.last_native_error
                    == MEDIA_BACKEND_RAW_NAL_BRIDGE_UNAVAILABLE
                || strstr(fixture_error, "raw-NAL") != NULL
                || strstr(fixture_error, "0x80010002") != NULL
                || strstr(fixture_error, "80010002") != NULL);
        printf(
            "tilefinch-media-fixture: event=%s error=\"%s\" "
            "clips=%u decoded=%u observed=%u native=0x%08X\n",
            fixture_passed ? "hardware-pass"
                : (emulator_untested ? "emulator-untested"
                                     : "hardware-fail"),
            fixture_error,
            fixture_report.clips_completed,
            fixture_report.frames_decoded,
            fixture_report.frames_observed,
            (unsigned) fixture_report.last_native_error);
        psp_report_budget_counters(browser.budget, "media-fixture-exit");
        browser_engine_destroy(browser.engine);
        browser.engine = NULL;
        if (process.clock_live) {
            (void) psp_clock_worker_shutdown(&process.clock_worker);
            process.clock_live = false;
        }
        printf(
            "tilefinch-validation: outcome=%s qualification=%s\n",
            fixture_passed || emulator_untested
                ? "clean-exit" : "qualification-failed",
            fixture_passed ? "hardware-pass"
                : (emulator_untested ? "emulator-untested"
                                     : "hardware-fail"));
        psp_log_checkpoint("media-fixture-complete");
        psp_log_finish(
            fixture_passed ? "media-fixture-pass"
                : (emulator_untested ? "media-fixture-untested"
                                     : "media-fixture-fail"));
        psp_exit_console(process.config.exit_to);
        return fixture_passed || emulator_untested ? 0 : 1;
    }
    if (process.config.validation_ge_present_probe != 0) {
        return psp_run_ge_present_qualification(
            browser.engine, browser.budget, &process.clock_worker, process.clock_live,
            process.config.exit_to);
    }
    if (process.config.validation_csc_order_probe != 0) {
        return psp_run_csc_order_qualification(
            browser.engine, browser.budget, &process.clock_worker, process.clock_live,
            process.config.exit_to);
    }
#endif
    psp_presentation_init(&process.presentation);
    psp_presentation_bind_chrome_fonts(&process.presentation, browser.engine);
    /* The client snapshots carry bounded manifest tables and would enlarge
       main's already-large stack frame enough to pessimize every frame-local
       access on Allegrex. The browser has exactly one optional-component
       session, so give that singleton static storage instead. */
    static PspVoiceComponentSession voice_component;
    browser.voice_component_session = &voice_component;
    static PspGlyphComponentSession glyph_component;
    browser.glyph_component_session = &glyph_component;
    psp_storage_paths_init(&process.storage, &process.install_paths);
    /* Cross-boot TLS session resumption store
       (docs/engineering/PSP_TRANSPORT.md). The blobs are resumption
       secrets: stick-only, cleared by CLEAR HTTP CACHES below. The store is
       active only with the owned live-network transport; profile policy is
       applied below before the first network request. */
    (void) fetch_set_tls_session_store_path(process.storage.tls_sessions);
    browser.profile = browser_profile_create(browser.budget);
    if (browser.profile == NULL) {
        printf("tilefinch-profile: allocation failed\n");
        goto sleep_forever;
    }
    if (browser_profile_load_without_content_blocker_sites(
            browser.profile, process.storage.profile))
        printf("tilefinch-profile: loaded\n");
    bool tls_session_persistence =
        browser_profile_tls_session_persistence(browser.profile);
    (void) fetch_set_tls_session_persistence_enabled(
        tls_session_persistence);
    if (!psp_glyph_component_session_attach_selected(
            browser.glyph_component_session, browser.budget,
            &process.install_paths,
            browser_profile_glyph_language(browser.profile),
            browser_profile_color_emoji(browser.profile))) {
        printf("tilefinch-glyph-component: selected packs unavailable\n");
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf(
        "tilefinch-glyph-component: language=%u color=%u "
        "installed=0x%02x attached=0x%02x\n",
        (unsigned) browser_profile_glyph_language(browser.profile),
        browser_profile_color_emoji(browser.profile) ? 1u : 0u,
        (unsigned) browser.glyph_component_session->installed_mask,
        (unsigned) browser.glyph_component_session->attached_mask);
    if (process.config.validation_raster_fixture_auto != 0) {
        return psp_run_raster_qualification(
            browser.engine, browser.budget, argv0,
            browser.glyph_component_session,
            &process.clock_worker, process.clock_live,
            process.config.exit_to);
    }
#endif
    /* Do not enumerate optional packs during ordinary boot. Selected packs
       were verified above; the Options screen probes the rest on demand. */
    bool javascript_enabled =
        browser_profile_javascript_enabled(browser.profile);
    bool site_data_allowed =
        browser_profile_site_data_allowed(browser.profile);
    if (!browser_engine_set_javascript_enabled(
            browser.engine, javascript_enabled)) {
        printf("tilefinch-profile: JavaScript policy unavailable\n");
    }
    browser_session_set_site_data_allowed(browser.session, site_data_allowed);
    psp_profile_store_init(&browser.profile_store, browser.profile, process.storage.profile);
    ContentBlockerMode content_blocker_mode =
        browser_profile_content_blocker_mode(browser.profile);
    if (!browser_engine_content_blocker_configure(
            browser.engine, content_blocker_mode, process.storage.content_blocker)) {
        content_blocker_mode = CONTENT_BLOCKER_BASIC;
        (void) browser_engine_content_blocker_configure(
            browser.engine, content_blocker_mode, NULL);
        printf(
            "tilefinch-content-blocker: custom list unavailable; "
            "using basic mode\n");
    }
    if (!psp_content_blocker_apply_allowed_sites(browser.engine, browser.profile)) {
        printf("tilefinch-content-blocker: allowlist unavailable\n");
    }
    ContentBlockerMetrics startup_blocker_metrics = {0};
    if (browser_engine_content_blocker_metrics(
            browser.engine, &startup_blocker_metrics)) {
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
        browser_profile_persistent_cache_mb(browser.profile);
    unsigned live_cache_kib =
        browser_profile_live_cache_kib(browser.profile);
    process.persistent_site_data_available =
        strcmp(process.config.trace, "none") == 0;
    size_t live_cache_bytes = (size_t) live_cache_kib * KIB;
    bool live_cache_limit_set =
        browser_session_cache_set_maximum_bytes(browser.session, live_cache_bytes);
    if (!live_cache_limit_set) {
        printf("tilefinch-site-data: cache limit unavailable bytes=%zu\n",
               live_cache_bytes);
    }
    char startup_url[BROWSER_PROFILE_URL_LIMIT];
    snprintf(startup_url, sizeof(startup_url), "%s", process.config.url);
    BrowserRecoveryCheckpoint startup_recovery = {0};
    bool restoring_last_page =
        browser_profile_restore_last_page(browser.profile)
        && browser_recovery_load(process.storage.recovery, &startup_recovery);
    if (restoring_last_page) {
        printf("tilefinch-recovery: loaded scroll=%d\n",
               startup_recovery.scroll_y);
    }
    /*
     * Which surface boot puts up first. The decision itself is a host-tested
     * pure function so the harness bypass is provable without a device: any
     * boot that names a URL, replays a trace, or arms a validation knob uses
     * direct initial-page loading rather than the native HOME path.
     */
    PspBootInputs boot_inputs = {
        .url_overridden = boot_url_overridden,
        .trace_replay = boot_trace_replay,
        .validation_mode = boot_validation_mode,
        .restore_last_page = restoring_last_page,
        .custom_homepage = browser_profile_custom_homepage_enabled(browser.profile)
    };
    PspBootPlan boot_plan = psp_boot_plan(&boot_inputs);
    PspBootQueue boot_queue;
    psp_boot_queue_init(&boot_queue);
    bool native_home_boot =
        boot_plan.surface == PSP_BOOT_SURFACE_NATIVE_HOME;
    if (restoring_last_page && !native_home_boot) {
        snprintf(
            startup_url, sizeof(startup_url), "%s",
            startup_recovery.url);
    } else if (!native_home_boot
               && browser_profile_custom_homepage_enabled(browser.profile)
               && psp_ui_native_home_url(startup_url)) {
        snprintf(
            startup_url, sizeof(startup_url), "%s",
            BROWSER_PROFILE_HOMEPAGE_URL);
    } else if (boot_plan.deferred_navigation) {
        /* Owed, not blocking: the surface is up first and this starts behind
           it the moment the browser is interactive. */
        (void) psp_boot_queue_request(
            &boot_queue,
            restoring_last_page ? startup_recovery.url
                                : BROWSER_PROFILE_HOMEPAGE_URL);
    }
    (void) browser_session_set_third_party_cookie_site_allowed(
        browser.session, startup_url,
        browser_profile_third_party_cookie_site_allowed(
            browser.profile, startup_url));
    printf("tilefinch-boot-order: surface=%s deferred=%s url-override=%d "
           "trace=%d validation=%d\n",
           native_home_boot ? "native-home" : "engine-first",
           boot_plan.deferred_navigation ? "yes" : "no",
           boot_inputs.url_overridden ? 1 : 0,
           boot_inputs.trace_replay ? 1 : 0,
           boot_inputs.validation_mode ? 1 : 0);
    psp_offline_store_init(
        &browser.offline_store, browser.budget, browser.session, process.storage.offline_library);
    offline_download_manager_set_cancel(
        &browser.offline_store.download,
        psp_media_platform_cancel_requested, &browser.offline_store);
    offline_download_manager_set_maximum_height(
        &browser.offline_store.download,
        (int) browser_profile_youtube_quality(browser.profile));
    const PspMediaSessionPlatform media_platform = {
        .context = &browser.offline_store,
        .profile_context = &browser.profile_store,
        .now_us = psp_media_platform_now_us,
        .cancel_requested = psp_media_platform_cancel_requested,
        .free_memory = psp_media_platform_free_memory,
        .maximum_free_block = psp_media_platform_maximum_free_block,
        .resolve_offline = psp_offline_store_resolve_media,
        .profile_changed = psp_media_platform_profile_changed
    };
    psp_media_init(
        &browser.media, browser.budget, browser.session, browser.profile, process.storage.profile,
        browser_engine_font_face(browser.engine, FONT_SANS), &media_platform);
    psp_active_media = &browser.media;
    psp_text_input_init(
        &process.text_input, browser.budget, "",
        psp_text_input_present, NULL);
    psp_text_input_set_profile(&process.text_input, browser.profile);
    psp_text_input_set_danzeff_enabled(
        &process.text_input,
        browser_profile_text_entry_mode(browser.profile)
            == BROWSER_TEXT_ENTRY_DANZEFF);
    psp_text_input_set_cancel_requested(
        &process.text_input, psp_text_input_system_cancel_requested, NULL);
    PspVoicePrepareContext voice_prepare = {
        .engine = browser.engine,
        .media = &browser.media,
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        .network = &network,
        .network_lifecycle = &network_lifecycle,
#endif
    };
    psp_text_input_set_voice_prepare(
        &process.text_input, psp_text_input_prepare_voice, &voice_prepare);
    psp_text_input_set_adaptive_voice_memory(
        &process.text_input,
        browser_profile_adaptive_voice_memory(browser.profile));
    bool experimental_voice_enabled =
        false;
    if (!psp_text_input_set_voice_enabled(
            &process.text_input, experimental_voice_enabled)) {
        experimental_voice_enabled = false;
        browser_profile_set_experimental_voice_input(browser.profile, false);
        psp_profile_store_mark_dirty(
            &browser.profile_store,
            (uint64_t) sceKernelGetSystemTimeWide());
    }
#ifdef TILEFINCH_HAVE_PSP_VOICE
    printf(
        "tilefinch-voice: experimental=%s model=\"%s\" "
        "tiers=extra-wide,small residency=%s lazy=yes "
        "eviction=pressure,navigation\n",
        experimental_voice_enabled ? "enabled" : "disabled",
        "lazy-component-probe",
        browser_profile_adaptive_voice_memory(browser.profile)
            ? "adaptive" : "full");
#endif

    if (strcmp(process.config.trace, "none") != 0) {
        char trace_path[768];
        psp_sibling_path(trace_path, sizeof(trace_path), argv0, process.config.trace);
        char trace_error[256] = {0};
        if (!fetch_trace_replay_begin(trace_path, trace_error,
                                      sizeof(trace_error))) {
            printf("tilefinch-psp-script: replay setup failed: %s\n",
                   trace_error);
            goto sleep_forever;
        }
        if (!fetch_trace_replay_seed_session(browser.session, trace_error,
                                             sizeof(trace_error))) {
            printf("tilefinch-psp-script: cookie seed failed: %s\n",
                   trace_error);
            goto sleep_forever;
        }
    } else {
        printf("tilefinch-psp-script: live transport enabled\n");
    }

#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /* Speculative-preconnect dwell state (docs/engineering/
       PSP_TRANSPORT.md). Persists across frames; a HOME tile must hold
       focus this long before its host is worth a background TCP+TLS connect. */
    FetchPreconnectDwell home_preconnect_dwell = {0};
#endif
    process.presentation.ui.browser_ui_scale = browser_profile_ui_scale(browser.profile);
    process.presentation.ui.page_font_percent =
        browser_profile_page_font_percent(browser.profile);
    process.presentation.ui.reader_font_serif =
        browser_profile_reader_font(browser.profile) == BROWSER_READER_FONT_SERIF;
    process.presentation.ui.remember_reader_site_scale =
        browser_profile_remember_reader_site_scale(browser.profile);
    process.presentation.ui.custom_homepage_enabled =
        browser_profile_custom_homepage_enabled(browser.profile);
    process.presentation.ui.history_enabled = browser_profile_history_enabled(browser.profile);
    process.presentation.ui.restore_last_page =
        browser_profile_restore_last_page(browser.profile);
    process.presentation.ui.tab_hibernation_enabled =
        browser_profile_tab_hibernation_enabled(browser.profile);
    process.presentation.ui.experimental_voice_input = experimental_voice_enabled;
    psp_ui_set_voice_component(
        &process.presentation.ui, PSP_UI_VOICE_COMPONENT_UNKNOWN, -1);
    process.presentation.ui.adaptive_voice_memory =
        browser_profile_adaptive_voice_memory(browser.profile);
    process.presentation.ui.analog_cursor_enabled =
        browser_profile_analog_cursor_enabled(browser.profile);
    process.presentation.ui.danzeff_text_input =
        browser_profile_text_entry_mode(browser.profile)
            == BROWSER_TEXT_ENTRY_DANZEFF;
    process.presentation.ui.persistent_cache_mb = persistent_cache_mb;
    process.presentation.ui.live_cache_kib = live_cache_kib;
    process.presentation.ui.persist_local_storage =
        browser_profile_persist_local_storage(browser.profile);
    process.presentation.ui.tls_session_persistence =
        tls_session_persistence;
    process.presentation.ui.network_profile =
        (uint8_t) process.config.network_profile;
    process.presentation.ui.javascript_enabled = javascript_enabled ? 1u : 0u;
    process.presentation.ui.site_data_allowed = site_data_allowed ? 1u : 0u;
    process.presentation.ui.mixed_content_site_allowed =
        browser_session_mixed_content_site_allowed(browser.session, startup_url);
    process.presentation.ui.third_party_cookie_site_allowed =
        browser_profile_third_party_cookie_site_allowed(browser.profile, startup_url);
    process.presentation.ui.search_engine = browser_profile_search_engine(browser.profile);
    process.presentation.ui.color_mode = browser_profile_color_mode(browser.profile);
    process.presentation.ui.chrome_theme = (unsigned) browser_profile_chrome_theme(browser.profile);
    process.presentation.ui.glyph_language =
        (unsigned) browser_profile_glyph_language(browser.profile);
    process.presentation.ui.color_emoji =
        browser_profile_color_emoji(browser.profile);
    psp_ui_set_glyph_component(
        &process.presentation.ui,
        browser.glyph_component_session->installed_mask, 0,
        PSP_UI_GLYPH_COMPONENT_UNKNOWN, -1);
    process.presentation.ui.youtube_240p =
        browser_profile_youtube_quality(browser.profile)
            == BROWSER_YOUTUBE_QUALITY_240P;
    process.presentation.ui.youtube_compact_results =
        browser_profile_youtube_compact_results(browser.profile);
    process.presentation.ui.video_scaling_sharp =
        browser_profile_video_scaling(browser.profile)
            == BROWSER_VIDEO_SCALING_SHARP;
    process.presentation.ui.video_startup_buffering =
        browser_profile_video_startup_buffering(browser.profile);
    process.presentation.ui.resume_offline_downloads =
        browser_profile_resume_offline_downloads(browser.profile);
    process.presentation.ui.content_blocker_mode = (uint8_t) content_blocker_mode;
    process.presentation.ui.content_blocker_cosmetic_hiding =
        browser_profile_content_blocker_cosmetic_hiding(browser.profile);
    process.presentation.ui.cookie_banner_hidden =
        browser_profile_cookie_banner_hidden(browser.profile, startup_url);
    process.presentation.ui.content_blocker_site_allowed =
        content_blocker_mode != CONTENT_BLOCKER_OFF
        && browser_profile_content_blocker_site_allowed(
               browser.profile, startup_url);
    process.presentation.ui.update_check_enabled =
        browser_profile_update_check_enabled(browser.profile);
    process.presentation.ui.update_channel = (uint8_t)
        browser_profile_update_channel(browser.profile);
    process.presentation.ui.developer_update_available =
        process.config.developer_update_url[0] != '\0';
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* The Experimental screen's decoder picker opens on the spelling this
       boot actually used, not on the shipping default. Keep the table walk
       outside main's growth-tripwire symbol. */
    psp_app_seed_video_decoder_choice(&process.presentation.ui, &process.config);
#endif
    /* Retain the serialized preference but do not spend CPU or presentation
       bandwidth on ambient motion. A future GPU-owned version
       can migrate the same preference without changing this boot path. */
    process.presentation.ui.wave_enabled = 0u;
    /* A release recorded by an earlier boot's background check stays marked
       until this build's own sequence catches up to it. */
    process.presentation.ui.update_release_available =
        browser_profile_update_check_available_sequence(browser.profile)
            > TILEFINCH_RELEASE_SEQUENCE;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    process.presentation.ui.validation_media_test_phase =
        process.config.validation_media_stability_auto != 0 ? 1u : 0u;
#endif
    psp_refresh_page_color_mode(&process.presentation.ui);
    (void) browser_engine_set_forced_dark(browser.engine, process.presentation.ui.page_dark);
    (void) browser_engine_set_youtube_compact_results(
        browser.engine, process.presentation.ui.youtube_compact_results);
    (void) psp_set_presentation_css(
        browser.engine, &process.presentation.ui, browser.profile, false, startup_url,
        process.presentation.ui.page_font_percent, false);
    psp_ui_set_page(&process.presentation.ui, "OPENING PAGE", startup_url,
                    strncmp(startup_url, "https://", 8) == 0);
    psp_ui_set_loading(&process.presentation.ui, true, -1);
    (void) browser_engine_fill_frame(browser.engine, PSP_STARTUP_BACKGROUND);
    (void) psp_engine_views_refresh(&engine_views, browser.engine);
    if (engine_views.frame != NULL && !native_home_boot) {
        psp_present(engine_views.frame, &process.presentation.ui);
    }

    /* The first stable frame is already visible. Activate saved site
       exceptions only now so profile growth never delays visible chrome. */
    BrowserProfileAllowlistImport startup_allowlist = {0};
    if (browser_profile_import_content_blocker_allowed_sites(
            browser.profile, process.storage.profile, &startup_allowlist)) {
        if (!psp_content_blocker_apply_allowed_sites(browser.engine, browser.profile)) {
            /* Keep the imported profile entries even if this session cannot
               allocate the engine-side exception table. A later unrelated
               profile save must never erase durable user state. */
            printf(
                "tilefinch-content-blocker: deferred allowlist retained; "
                "session apply refused\n");
        } else {
            process.presentation.ui.content_blocker_site_allowed =
                content_blocker_mode != CONTENT_BLOCKER_OFF
                && browser_profile_content_blocker_site_allowed(
                       browser.profile, startup_url);
            (void) psp_set_presentation_css(
                browser.engine, &process.presentation.ui, browser.profile, false, startup_url,
                process.presentation.ui.page_font_percent, false);
            printf(
                "tilefinch-content-blocker: deferred-sites=%zu "
                "duplicate=%zu ignored=%zu full=%s\n",
                startup_allowlist.added, startup_allowlist.duplicate,
                startup_allowlist.ignored,
                startup_allowlist.resident_full ? "yes" : "no");
        }
    }

    /*
     * Persistent site data is opt-in and Memory Stick latency is unbounded
     * enough to be visible. Never put that read ahead of the first frame:
     * users should see stable chrome even while a larger cache is restored.
     */
    if (process.persistent_site_data_available && live_cache_limit_set
        && persistent_cache_mb != 0) {
        BrowserSessionPersistenceLimits limits =
            psp_site_data_limits(persistent_cache_mb);
        bool loaded = psp_site_data_load(
            browser.session, process.storage.persistent_cache,
            BROWSER_SESSION_PERSIST_CACHE, &limits);
        printf("tilefinch-site-data: disk-cache=%uMB load=%s\n",
               persistent_cache_mb, loaded ? "ok" : "empty");
    }
    if (process.persistent_site_data_available
        && site_data_allowed
        && browser_profile_persist_local_storage(browser.profile)) {
        BrowserSessionPersistenceLimits limits =
            psp_site_data_limits(0);
        bool loaded = psp_site_data_load(
            browser.session, process.storage.local_storage,
            BROWSER_SESSION_PERSIST_LOCAL_STORAGE, &limits);
        printf("tilefinch-site-data: local-storage load=%s\n",
               loaded ? "ok" : "empty");
    }

#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (strcmp(process.config.trace, "none") == 0
        && !native_home_boot
        && psp_profile_page_kind(startup_url) == PSP_PROFILE_PAGE_NONE
        && site_adapter_navigation_requires_network(
               "GET", startup_url)) {
        psp_log_set_phase(PSP_LOG_PHASE_NETWORK);
        psp_log_checkpoint("network-begin");
        bool network_ready = psp_connect_network(
            &network, (int) process.config.network_profile, engine_views.frame, &process.presentation.ui);
        if (!network_ready) {
            printf("tilefinch-psp-script: page load skipped without "
                   "network\n");
            if (psp_network_lifecycle_started(&network_lifecycle)) {
                psp_shutdown_network_logged(&network);
            }
            goto sleep_forever;
        }
        psp_log_checkpoint("network-ready");
    } else if (strcmp(process.config.trace, "none") == 0) {
        printf("tilefinch-network: deferred for native HOME\n");
    }
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* The one validation mode that has to be dispatched here rather than
       beside the others: it is the only one that needs the network, and this
       is the first point at which the network is up. */
    if (process.config.validation_media_range_probe != 0) {
        return psp_run_media_range_qualification(
            browser.engine, &process.config, &process.clock_worker, process.clock_live);
    }
#endif

    psp_log_set_phase(PSP_LOG_PHASE_NAVIGATION);
    psp_log_checkpoint("initial-navigation-begin");
    psp_validation_cancel_after_ms =
        (unsigned) process.config.validation_cancel_after_ms;
    psp_validation_preview_scroll =
        (unsigned) process.config.validation_preview_scroll;
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
        static const char blank_html[] =
            "<!doctype html><meta name=viewport "
            "content=\"width=device-width,initial-scale=1\">"
            "<title>Tilefinch</title>"
            "<style>html,body{margin:0;background:#100e0d}</style>";
        loaded = browser_engine_commit_html(
            browser.engine, TILEFINCH_HOMEPAGE_URL,
            blank_html, sizeof(blank_html) - 1u, true)
            && browser_engine_refresh_shell(browser.engine);
        if (loaded) {
            psp_home_sync_ui(&process.presentation.ui, &process.presentation.home_surface, browser.profile, NULL, false);
            psp_ui_show_home(&process.presentation.ui);
            /* The first HOME frame is the settled, ordinary surface. Do not
               spend ten uninterruptible full-screen presents on an entrance
               animation immediately before the input loop starts. */
            psp_present(engine_views.frame, &process.presentation.ui);
        }
    } else {
        loaded = initial_profile_page != PSP_PROFILE_PAGE_NONE
            ? psp_profile_open_page(
                  browser.engine, &process.presentation.ui, browser.profile, initial_profile_page)
            : psp_run_initial_page_load(
                  browser.engine, &process.presentation.ui, engine_views.frame, startup_url, 4 * MIB, 30000,
                  argv0, process.config.dump_frame == 2, &initial_load_stopped);
    }
    psp_validation_cancel_after_ms = 0;
    psp_validation_preview_scroll = 0;
    __sync_synchronize();
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
           engine_views.navigation->last_http_status, engine_views.navigation->last_server,
           engine_views.navigation->last_cf_mitigated,
           initial_load_stopped ? "stopped by user"
                                : engine_views.navigation->last_error,
           (unsigned long long) engine_views.navigation->performance.full_relayouts,
           engine_views.navigation->page.images.stats.loaded,
           engine_views.navigation->page.images.stats.discovered,
           engine_views.navigation->last_stream.bytes_received,
           engine_views.navigation->last_stream.chunks_received,
           engine_views.navigation->last_stream.peak_buffered_bytes,
           engine_views.navigation->last_stream.cancelled ? 1 : 0,
           engine_views.navigation->last_stream.truncated ? 1 : 0,
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    psp_log_checkpoint(
        loaded ? "initial-navigation-complete"
               : "initial-navigation-failed");
    if (!loaded && !initial_load_stopped) {
        (void) psp_write_navigation_failure_report(
            "initial-navigation", engine_views.navigation->last_error, startup_url,
            engine_views.navigation);
    }
    psp_ui_set_loading(&process.presentation.ui, false, loaded ? 1000 : 0);
    if (loaded) {
        /* The restored page is opened from behind HOME on a native boot, so
           its scroll is applied when it arrives, not to the blank document
           standing in for "nothing open yet". */
        if (restoring_last_page && !native_home_boot
            && startup_recovery.scroll_y > 0) {
            (void) browser_engine_scroll_by(
                browser.engine, startup_recovery.scroll_y);
            psp_ui_show_status(&process.presentation.ui, "LAST PAGE RESTORED", 180);
        }
        psp_profile_record_current(
            browser.profile, &browser.profile_store, engine_views.navigation,
            (uint64_t) sceKernelGetSystemTimeWide());
        (void) psp_recovery_record_current(
            browser.profile, process.storage.recovery, engine_views.navigation);
    } else {
        if (restoring_last_page) {
            (void) browser_recovery_clear(process.storage.recovery);
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
            browser.engine, startup_url,
            fallback_html, fallback_length, true)
            && browser_engine_refresh_shell(browser.engine);
        loaded = initial_error_page;
        if (initial_error_page) {
            (void) psp_engine_views_refresh(&engine_views, browser.engine);
            psp_ui_show_status(
                &process.presentation.ui, initial_load_stopped
                    ? "PAGE LOAD STOPPED"
                    : "PAGE FAILED - START OPENS ADDRESS",
                300);
        }
    }
    if (loaded && browser_engine_render_frame(browser.engine, NULL)) {
        psp_sync_ui(&process.presentation.ui, browser.engine, browser.profile);
        psp_present(engine_views.frame, &process.presentation.ui);
    }
    if (!loaded) goto report;

    long advance_failures = 0;
    for (long i = 0; i < process.config.ticks; i++) {
        psp_log_heartbeat();
        if (!browser_engine_advance_runtime(
                browser.engine, (unsigned) process.config.tick_ms, 8, NULL)) {
            /* The host lab keeps pumping through recoverable script
               errors (an uncaught microtask still leaves timers, fetch
               deliveries, and rendering live); mirror that instead of
               abandoning the boot at the first one. */
            if (advance_failures++ == 0) {
                printf("tilefinch-psp-script: advance failed at tick %ld"
                       " (continuing)\n", i);
            }
            if (advance_failures > 256) {
                printf("tilefinch-psp-script: advance failing persistently"
                       " at tick %ld\n", i);
                break;
            }
            continue;
        }
        if ((i & 127) == 127) {
            printf("tilefinch-psp-script: tick=%ld elapsed=%llums "
                   "budget=%zu peak=%zu relayouts=%llu/%llu images=%zu\n",
                   i + 1,
                   (unsigned long long)
                       (((uint64_t) sceKernelGetSystemTimeWide()
                         - load_started_us) / 1000u),
                   browser.budget->current, browser.budget->peak,
                   (unsigned long long) engine_views.navigation->performance.full_relayouts,
                   (unsigned long long) engine_views.navigation->performance.fast_relayouts,
                   engine_views.navigation->page.images.stats.loaded);
            psp_ui_set_loading(
                &process.presentation.ui, true,
                process.config.ticks <= 0 ? -1 : (int) ((i + 1) * 1000 / process.config.ticks));
            if (engine_views.frame != NULL) psp_present(engine_views.frame, &process.presentation.ui);
        }
    }
    psp_ui_set_loading(&process.presentation.ui, false, 1000);

report:
    psp_log_set_phase(PSP_LOG_PHASE_REPORT);
    printf("tilefinch-psp-script: total-elapsed=%llums height=%d links=%zu "
           "budget=%zu peak=%zu\n",
           (unsigned long long) (((uint64_t) sceKernelGetSystemTimeWide()
                                  - load_started_us) / 1000u),
           engine_views.navigation->page.layout.height,
           engine_views.navigation->page.layout.link_count,
           browser.budget->current, browser.budget->peak);
    /* The four diagnostic counters are uint64_t, not size_t. Printing them
       with %zu read four bytes of an eight-byte vararg slot, and the o32 ABI's
       8-byte alignment for the first of them left a padding hole where
       declarations= read uninitialized stack -- 0xDEADBEEF on device -- and
       every later specifier on the line consumed the wrong slot. Match the
       host report in src/main.c, which has always used %llu. */
    printf("tilefinch-psp-script: css-rules=%zu important=%zu layers=%zu "
           "vars=%zu declarations=%llu supported=%llu rejected=%llu "
           "deferred=%llu\n",
           engine_views.navigation->page.stylesheet.count,
           engine_views.navigation->page.stylesheet.important_rule_count,
           engine_views.navigation->page.stylesheet.layer_count,
           engine_views.navigation->page.stylesheet.variable_count,
           (unsigned long long)
               engine_views.navigation->page.stylesheet.diagnostic_declarations,
           (unsigned long long)
               engine_views.navigation->page.stylesheet.diagnostic_supported_declarations,
           (unsigned long long)
               engine_views.navigation->page.stylesheet.diagnostic_rejected_declarations,
           (unsigned long long)
               engine_views.navigation->page.stylesheet.diagnostic_deferred_declarations);
    printf("tilefinch-psp-script: css-cache-hits=%zu retained-hits=%zu "
           "retries=%zu transient=%zu pressure-serializations=%zu\n",
           engine_views.navigation->page.external_stylesheets.cache_hits,
           engine_views.navigation->page.external_stylesheets.retained_body_hits,
           engine_views.navigation->page.external_stylesheets.transient_retries,
           engine_views.navigation->page.external_stylesheets.transient_failures,
           engine_views.navigation->page.external_stylesheets.pressure_serializations);
    printf("tilefinch-psp-script: css-fragments=%zu/%zu/%zu "
           "fragment-rules=%zu fragment-bytes=%zu ir=%zu/%zu/%zu "
           "ir-operations=%zu ir-bytes=%zu\n",
           engine_views.navigation->page.external_stylesheets
             .compiled_fragment_hits,
           engine_views.navigation->page.external_stylesheets
             .compiled_fragment_misses,
           engine_views.navigation->page.external_stylesheets
             .compiled_fragment_stores,
           engine_views.navigation->page.external_stylesheets
             .compiled_fragment_rules_reused,
           engine_views.navigation->page.external_stylesheets
             .compiled_fragment_bytes,
           engine_views.navigation->page.external_stylesheets.parsed_ir_hits,
           engine_views.navigation->page.external_stylesheets.parsed_ir_misses,
           engine_views.navigation->page.external_stylesheets.parsed_ir_stores,
           engine_views.navigation->page.external_stylesheets
             .parsed_ir_operations_reused,
           engine_views.navigation->page.external_stylesheets.parsed_ir_bytes);
    printf("tilefinch-psp-script: stylesheets=%zu/%zu css-bytes=%zu "
           "css-terminal-failures=%zu images=%zu/%zu\n",
           engine_views.navigation->page.external_stylesheets.loaded,
           engine_views.navigation->page.external_stylesheets.discovered,
           engine_views.navigation->page.external_stylesheets.bytes,
           engine_views.navigation->page.external_stylesheets.terminal_failures,
           engine_views.navigation->page.images.stats.loaded,
           engine_views.navigation->page.images.stats.discovered);
    printf("tilefinch-psp-script: preloads=%zu/%zu/%zu cache=%zu "
           "failed=%zu deferred=%zu headroom-skips=%zu\n",
           engine_views.navigation->preloads_discovered,
           engine_views.navigation->preloads_launched,
           engine_views.navigation->preloads_completed,
           engine_views.navigation->preloads_cache_hits,
           engine_views.navigation->preloads_failed,
           engine_views.navigation->preloads_deferred,
           engine_views.navigation->preloads_headroom_skipped);
    printf("tilefinch-psp-script: scripts discovered=%zu attempted=%zu "
           "loaded=%zu failed=%zu modules=%zu module-map-hits=%zu "
           "bytes=%zu\n",
           engine_views.navigation->script_discovered, engine_views.navigation->script_attempted,
           engine_views.navigation->script_loaded, engine_views.navigation->script_failed,
           engine_views.navigation->script_modules,
           engine_views.navigation->script_module_map_hits,
           engine_views.navigation->script_bytes);
    printf("tilefinch-psp-script: js-error=\"%s\" rejections=%zu "
           "last=\"%.600s\"\n",
           engine_views.navigation->page.script_result.error,
           engine_views.navigation->page.script_result.promise_rejections,
           engine_views.navigation->page.script_result.last_promise_rejection);
    psp_report_budget_counters(browser.budget, "pre-interaction");
    const TileCache *render_stats = browser_engine_render_metrics_view(browser.engine);
    if (render_stats != NULL) {
        printf("tilefinch-render-job: scheduled=%zu completed=%zu "
               "cancelled=%zu slices=%zu units=%zu budget-exhausted=%zu "
               "overruns=%zu max-slice=%lluus max-unit=%lluus\n",
               render_stats->frame_jobs_scheduled,
               render_stats->frame_jobs_completed,
               render_stats->frame_jobs_cancelled,
               render_stats->frame_job_slices,
               render_stats->frame_job_units,
               render_stats->frame_job_budget_exhaustions,
               render_stats->frame_job_slice_overruns,
               (unsigned long long) render_stats->max_frame_job_slice_us,
               (unsigned long long) render_stats->max_frame_job_unit_us);
    }
    printf("tilefinch-js-retention: handles=%zu/%zu stable=%zu "
           "listeners=%zu handlers=%zu observers=%zu timers=%zu "
           "network=%zu wrapper-evictions=%zu observer-drops=%zu "
           "control-drops=%zu\n",
           engine_views.navigation->page.script_result.dom_handle_slots_live,
           engine_views.navigation->page.script_result.dom_handle_slots_peak,
           engine_views.navigation->page.script_result.root_stable_wrappers,
           engine_views.navigation->page.script_result.root_stable_listener_targets,
           engine_views.navigation->page.script_result.root_stable_handler_targets,
           engine_views.navigation->page.script_result.root_mutation_observers,
           engine_views.navigation->page.script_result.root_timers,
           engine_views.navigation->page.script_result.root_pending_network,
           engine_views.navigation->page.script_result.retention_wrapper_evictions,
           engine_views.navigation->page.script_result.retention_observer_drops,
           engine_views.navigation->page.script_result.retention_control_drops);
    BrowserEngineMetrics engine_metrics = {0};
    if (browser_engine_metrics(browser.engine, &engine_metrics)) {
        printf("tilefinch-engine-memory: control=%zu framebuffer=%zu "
               "reserve=%zu limit=%zu current=%zu peak=%zu "
               "external=%zu external-peak=%zu active=%zu largest=%zu "
               "failures=%zu system-free=%d system-largest=%d\n",
               engine_metrics.control_bytes,
               engine_metrics.framebuffer_bytes,
               engine_metrics.non_page_memory_reserve,
               engine_metrics.budget_limit,
               engine_metrics.budget_current,
               engine_metrics.budget_peak,
               engine_metrics.budget_external_reserved,
               engine_metrics.budget_external_reserved_peak,
               engine_metrics.budget_active_allocations,
               engine_metrics.budget_largest_allocation,
               engine_metrics.allocation_failures,
               sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    }
    psp_log_checkpoint("compact-report-complete");

    if (loaded) {
        if (!browser_engine_refresh_shell(browser.engine)
            || !browser_engine_render_frame(browser.engine, NULL)) {
            printf("tilefinch-psp-script: frame render failed: %s\n",
                   browser_engine_last_error(browser.engine));
            goto sleep_forever;
        }
        (void) psp_engine_views_refresh(&engine_views, browser.engine);
        browser.tabs = browser_tabs_create(browser.budget, engine_views.navigation);
        if (browser.tabs == NULL) {
            printf("tilefinch-tabs: bounded session allocation unavailable\n");
        } else if (browser_profile_restore_last_page(browser.profile)
                   && browser_tabs_restore_session(
                       browser.tabs, process.storage.tab_session)) {
            const BrowserTabSnapshot *restored_tab =
                browser_tabs_active(browser.tabs);
            const BrowserTabHistoryEntry *restored_entry =
                restored_tab == NULL || restored_tab->history_count == 0
                    || restored_tab->history_index
                           >= restored_tab->history_count
                ? NULL
                : &restored_tab->history[restored_tab->history_index];
            const char *current_url = engine_views.navigation->page.document_url;
            if (restored_entry == NULL || current_url == NULL
                || strcmp(restored_entry->url, current_url) != 0) {
                browser_tabs_destroy(browser.tabs);
                browser.tabs = browser_tabs_create(browser.budget, engine_views.navigation);
                printf("tilefinch-tabs: saved session did not match page\n");
            } else {
                NavigationHistoryRecord records[BROWSER_TAB_HISTORY_LIMIT];
                size_t record_count = 0, record_current = 0;
                bool history_restored = browser_tab_history_records(
                        restored_tab, records, &record_count,
                        &record_current)
                    && browser_engine_replace_history(
                        browser.engine, records, record_count, record_current)
                    && browser_engine_restore_view(
                        browser.engine, restored_entry->scroll_y,
                        restored_entry->focus_kind,
                        restored_entry->focus_index);
                printf(
                    "tilefinch-tabs: session restore tabs=%zu result=%s\n",
                    browser_tabs_count(browser.tabs),
                    history_restored ? "ok" : "facts-only");
            }
        }
        (void) psp_engine_views_refresh(&engine_views, browser.engine);
        psp_sync_ui(&process.presentation.ui, browser.engine, browser.profile);
        psp_tabs_sync_ui(&process.presentation.ui, browser.tabs, &process.presentation.tab_view);
        if (native_home_boot) {
            /* Everything HOME reads exists now, including the tab list its
               CONTINUE rows come from. The surface owns its own hint line,
               so it does not also get a readiness toast. */
            psp_boot_queue_set_ready(&boot_queue, true);
            psp_home_sync_ui(&process.presentation.ui, &process.presentation.home_surface, browser.profile, browser.tabs, true);
            psp_ui_show_home(&process.presentation.ui);
        } else {
            psp_ui_show_status(
                &process.presentation.ui,
                initial_error_page
                    ? "PAGE FAILED - START OPENS ADDRESS"
                    : "READY - SELECT OPENS MENU",
                initial_error_page ? 300 : 240);
        }
        const NavigationEntry *media_entry = navigation_current(engine_views.navigation);
        psp_media_prepare_route(
            &browser.media, media_entry == NULL ? NULL : media_entry->url,
            engine_views.navigation->generation);
        if (browser_profile_resume_offline_downloads(browser.profile)
            && offline_library_load(&browser.offline_store.library)) {
            uint32_t resumed_id = 0;
            for (size_t index = 0;
                 index < browser.offline_store.library.count; index++) {
                OfflineLibraryItem *item =
                    &browser.offline_store.library.items[index];
                if (item->type == OFFLINE_ITEM_YOUTUBE
                    && (item->state == OFFLINE_ITEM_PAUSED
                        || item->state == OFFLINE_ITEM_QUEUED)
                    && offline_download_manager_start(
                        &browser.offline_store.download, item->id)) {
                    resumed_id = item->id;
                    break;
                }
            }
            if (resumed_id != 0) {
                printf(
                    "tilefinch-offline: event=auto-resume id=%u\n",
                    (unsigned) resumed_id);
                psp_ui_show_status(
                    &process.presentation.ui, "RESUMING SAVED VIDEO", 240);
            }
        }

        printf("tilefinch-psp-script: frame-hash=%016llx ui-state=%zuB\n",
               (unsigned long long)
                   psp_frame_hash(engine_views.frame, engine_views.frame_pixels),
               psp_ui_state_bytes());
        /* Presenting is not the same as being seen.  Publish the scanout
           outcome so a display the panel never accepted cannot be reported as
           a healthy run. */
        printf("tilefinch-display: presents=%u rejected=%u "
               "first-error=0x%08x rearms=%u rearm-failures=%u "
               "last-rearm-error=0x%08x\n",
               psp_display.presents, psp_display.rejections,
               (unsigned) psp_display.first_error,
               psp_display.rearms, psp_display.rearm_failures,
               (unsigned) psp_display.last_rearm_error);
        psp_report_presentation_cadence("pre-interaction");
        /* Publish before optional Memory Stick diagnostics so the user never
           waits for a frame dump. Capture the accepted front buffer rather
           than the bare engine raster: native HOME and all browser chrome
           are composed only in VRAM, and a page-only dump cannot validate
           what the PSP actually displayed. */
        psp_present(engine_views.frame, &process.presentation.ui);
        if (process.config.dump_frame != 0) {
            uint32_t dump_operation =
                psp_log_operation_begin("frame-dump");
            const uint16_t *presented =
                psp_display_front_buffer(&psp_display);
            bool dumped = psp_dump_frame_strided_named(
                argv0, "frame-device.ppm",
                presented, PSP_DISPLAY_BUFFER_PIXELS,
                PSP_DISPLAY_STRIDE);
            printf("tilefinch-psp-script: frame-dumped=%d\n",
                   dumped ? 1 : 0);
            psp_log_operation_end(
                dump_operation, "frame-dump",
                dumped ? "ok" : "failed");
        }
        printf("tilefinch-psp-script: DONE\n");
        psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
        psp_log_checkpoint("interactive-ready");
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        psp_ui_show_status(
            &process.presentation.ui,
            persistent_log_started
                ? "LOGGING ON - TILEFINCH/DATA"
                : "DIAGNOSTIC LOG UNAVAILABLE",
            360);
        psp_present(engine_views.frame, &process.presentation.ui);
#endif

        PspInteractiveResult interactive_result =
            psp_app_run_interactive(
                &process, &browser, &interactive, &engine_views,
                argv0, loaded, startup_url,
                startup_blocker_metrics.requests_blocked,
                &boot_queue, &home_preconnect_dwell,
                &power_policy,
                native_home_boot, restoring_last_page,
                initial_error_page
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                , &network, &network_lifecycle
#endif
            );
        PspShutdownReport shutdown_report = psp_browser_close(
            &process, &browser, &interactive, &engine_views,
            &interactive_result
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            , &network, &network_lifecycle
#endif
        );
        (void) shutdown_report;
        bool restart_launcher =
            psp_exit_plan_restarts_launcher(&interactive.exit);
        if (restart_launcher && process.install_paths.slotted) {
            char launcher_path[TILEFINCH_INSTALL_PATH_LIMIT];
            int length = snprintf(
                launcher_path, sizeof(launcher_path), "%s/EBOOT.PBP",
                process.install_paths.install_root);
            if (length > 0
                && (size_t) length < sizeof(launcher_path)) {
                (void) psp_load_exec_eboot(launcher_path, NULL, NULL);
            }
        }
        /* After psp_log_finish(), which is what put this run's log and crash
           record on the card: the load replaces the process, so anything
           left unwritten here would be lost. A restart request owns the
           handoff above and is not overridden. */
        psp_exit_console(
            restart_launcher ? NULL : process.config.exit_to);
        return 0;
    }

sleep_forever:
    psp_log_set_phase(PSP_LOG_PHASE_HALTED);
    psp_present_boot_surface(
        PSP_UI_STARTUP_SPLASH, "STARTUP FAILED - SEE LOG", 0);
    printf("tilefinch-psp-script: halted\n");
    printf("tilefinch-validation: outcome=halted "
           "free-mem=%d max-free=%d\n",
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (psp_network_lifecycle_started(&network_lifecycle)) {
        uint32_t cleanup_operation =
            psp_log_operation_begin("halted-network-cleanup");
        psp_network_lifecycle_request(
            &network_lifecycle,
            PSP_NETWORK_REQUEST_SHUTDOWN_INHIBIT, true, 0,
            &network, PSP_NETWORK_SUPERVISOR_STOPPING,
            "halt-off");
        psp_shutdown_network_logged(&network);
        psp_log_operation_end(
            cleanup_operation, "halted-network-cleanup", "ok");
    }
    psp_network_lifecycle_report(&network_lifecycle);
#endif
    psp_log_finish("halted");
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* A boot that failed is exactly when a remote loop most needs the
       console back: sleeping here strands it until someone power-cycles the
       device by hand. Every halt site is downstream of the configuration
       load, so exit_to is decided by the time any of them is reached, and
       the log is already on the card. */
    if (process.config.exit_to[0] != '\0') (void) psp_exit_handoff(process.config.exit_to);
#endif
    sceKernelSleepThread();
    return 1;
}
