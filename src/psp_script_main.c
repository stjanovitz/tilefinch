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
#include "psp_app/psp_app_boot.h"
#include "psp_app/psp_app_loop.h"
#include "psp_app/psp_app_validation.h"
#include "tilefinch/psp_fpu.h"
#include "tilefinch/psp_time.h"
#include "tilefinch/psp_threads.h"
#include "psp_thread_contract.h"
#include "tilefinch/psp_voice_component_session.h"
#include "tilefinch/update_history.h"
#include "tilefinch/voice_component.h"
#include "tilefinch/wasm_runtime.h"
#include "tilefinch/runtime_clock.h"
#include "tilefinch/game_audio.h"
#include "tilefinch/work_ledger.h"
#include "tilefinch/url.h"

#include <stdarg.h>
#ifdef TILEFINCH_HAVE_PSP_VOICE
#include "stt_component_loader.h"
#endif
#include "tilefinch_compiler.h"
#ifdef TILEFINCH_PSP_LIVE_NETWORK
#include "psp_multiplayer.h"
#include "tilefinch/psp_entropy.h"
#endif
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && defined(TILEFINCH_PSP_LIVE_NETWORK)
#include "psp_update_e2e.h"
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
#include <stdatomic.h>
#include "tilefinch/psp_raster_fixture.h"
/* After the tilefinch headers: the media policy header is written against
   fixed-width types the public headers have already established. */
#include "media_backend_psp_policy.h"
#endif

/* Shipping builds do not open or rotate the full validation log. Ordinary
   failure persistence is also opt-in; fatal startup failures bypass the
   setting once because the UI may never become usable. */
static const TilefinchInstallPaths *psp_failure_report_paths;
static bool psp_failure_report_persistence_enabled;
static char psp_wifi_report[4096];

const char *psp_wifi_diagnostic_report(void) { return psp_wifi_report; }
void psp_wifi_diagnostic_clear(void) { psp_wifi_report[0] = '\0'; }

typedef struct {
    bool transport_present;
    long transport_code;
    long tls_verify_result;
    bool tls_verify_result_available;
    bool tls_verification_failed;
    bool transport_timed_out;
    bool tls12_compatibility_retry;
    char tls_version[16];
    char tls_peer_issuer[TILEFINCH_TLS_PEER_ISSUER_LIMIT];
    bool worker_present;
    size_t worker_occupied_slots;
    size_t worker_queued_slots;
    size_t worker_running_slots;
    size_t worker_complete_slots;
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
    const PspFailureReportData *diagnostics, bool force_persistence)
{
    PspFailureReportData absent = {0};
    if (diagnostics == NULL) diagnostics = &absent;
    if (psp_failure_report_paths == NULL) return false;
    if (!force_persistence && !psp_failure_report_persistence_enabled)
        return false;
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
    time_t sampled_epoch = 0;
    PspTimeFields rtc = {0};
    PspTimeStatus rtc_status = psp_time_read_utc_fields(
        &rtc, &sampled_epoch);
    unsigned long long unix_time = (unsigned long long) sampled_epoch;
#else
    unsigned long long unix_time = 0;
    PspTimeFields rtc = {0};
    PspTimeStatus rtc_status = PSP_TIME_UNREADABLE;
#endif
    uint32_t ca_bundle_version = 0;
    size_t ca_bundle_bytes = 0;
    uint8_t ca_bundle_digest[TILEFINCH_CA_BUNDLE_SHA256_BYTES];
    bool ca_bundle_present = fetch_ca_bundle_identity(
        &ca_bundle_version, &ca_bundle_bytes, ca_bundle_digest);
    char ca_bundle_sha256[TILEFINCH_CA_BUNDLE_SHA256_BYTES * 2u + 1u];
    if (ca_bundle_present) {
        for (size_t at = 0; at < TILEFINCH_CA_BUNDLE_SHA256_BYTES; at++) {
            (void) snprintf(
                ca_bundle_sha256 + at * 2u, 3u, "%02x",
                (unsigned) ca_bundle_digest[at]);
        }
    } else {
        snprintf(ca_bundle_sha256, sizeof(ca_bundle_sha256), "absent");
    }
    bool okay = fprintf(
        file,
        "tilefinch-device-error-v3\n"
        "version=%s\nrelease-sequence=%llu\n"
        "stage=%s\nhttp=%ld\nnative=0x%08x\n"
        "uptime-us=%llu\nunix-time=%llu\nrtc-valid=%d\nrtc-status=%s\n"
        "rtc=%04u-%02u-%02uT%02u:%02u:%02uZ\nfree=%d\nlargest=%d\n"
        "transport-present=%d\ncurl-code=%ld\ntimed-out=%d\n"
        "tls-verify=0x%08lx\ntls-verify-available=%d\n"
        "tls-verification-failed=%d\ntls-version=%s\ntls12-retry=%d\n"
        "tls-peer-issuer=%s\nca-bundle-present=%d\n"
        "ca-bundle-version=%u\nca-bundle-bytes=%zu\n"
        "ca-bundle-sha256=%s\n"
        "worker-present=%d\nworker-slots=%zu\nworker-queued=%zu\n"
        "worker-running=%zu\nworker-complete=%zu\n"
        "network-present=%d\nnetwork-status=%d\n"
        "network-failure-phase=%d\nprofile-requested=%d\n"
        "profile-selected=%d\nprofile-fallback=%d\napctl=%d\n"
        "wlan-switch=%d\nwlan-power=%d\nprofile-query-ok=0x%08x\n"
        "profile-query-failed=0x%08x\nprofile-adopted=0x%08x\n"
        "profile-security=%u\nprofile-static-ip=%d\n"
        "profile-manual-dns=%d\nprofile-proxy=%d\nnetwork-pumps=%zu\n"
        "network-elapsed-us=%llu\nnetwork-max-pump-us=%llu\n"
        "network-max-pump-phase=%d\nurl=%.2047s\ndetail=%.1023s\n",
        TILEFINCH_VERSION_STRING,
        (unsigned long long) TILEFINCH_RELEASE_SEQUENCE,
        stage == NULL ? "unknown" : stage, http_status,
        (unsigned) native_result, (unsigned long long) now_us, unix_time,
        rtc_status == PSP_TIME_OK ? 1 : 0,
        psp_time_status_name(rtc_status),
        (unsigned) rtc.year, (unsigned) rtc.month,
        (unsigned) rtc.day, (unsigned) rtc.hour, (unsigned) rtc.minute,
        (unsigned) rtc.second,
        sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize(),
        diagnostics->transport_present ? 1 : 0,
        diagnostics->transport_code,
        diagnostics->transport_timed_out ? 1 : 0,
        (unsigned long) diagnostics->tls_verify_result,
        diagnostics->tls_verify_result_available ? 1 : 0,
        diagnostics->tls_verification_failed ? 1 : 0,
        diagnostics->tls_version[0] == '\0'
            ? "absent" : diagnostics->tls_version,
        diagnostics->tls12_compatibility_retry ? 1 : 0,
        diagnostics->tls_peer_issuer[0] == '\0'
            ? "absent" : diagnostics->tls_peer_issuer,
        ca_bundle_present ? 1 : 0,
        (unsigned) ca_bundle_version, ca_bundle_bytes, ca_bundle_sha256,
        diagnostics->worker_present ? 1 : 0,
        diagnostics->worker_occupied_slots,
        diagnostics->worker_queued_slots,
        diagnostics->worker_running_slots,
        diagnostics->worker_complete_slots,
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
        stage, detail, url, http_status, native_result, NULL, false);
}

bool psp_failure_report_configure(bool enabled, bool clear_existing)
{
    psp_failure_report_persistence_enabled = enabled;
    if (enabled || !clear_existing || psp_failure_report_paths == NULL)
        return true;
    static const char *const names[] = {
        "tilefinch-last-error.txt", "tilefinch-last-error.tmp"
    };
    bool okay = true;
    for (size_t at = 0; at < sizeof(names) / sizeof(names[0]); at++) {
        char path[TILEFINCH_INSTALL_PATH_LIMIT];
        if (!tilefinch_install_data_path(
                psp_failure_report_paths, names[at], path, sizeof(path))) {
            okay = false;
            continue;
        }
        if (remove(path) != 0 && errno != ENOENT) okay = false;
    }
    return okay;
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
        .tls_verify_result_available =
            navigation->last_tls_verify_result_available,
        .tls_verification_failed =
            navigation->last_tls_verification_failed,
        .transport_timed_out = navigation->last_transport_timed_out,
        .tls12_compatibility_retry =
            navigation->last_tls12_compatibility_retry
    };
    FetchBackgroundTransportMetrics worker = {0};
    if (fetch_background_transport_metrics(&worker)) {
        diagnostics.worker_present = true;
        diagnostics.worker_occupied_slots = worker.occupied_slots;
        diagnostics.worker_queued_slots = worker.queued_slots;
        diagnostics.worker_running_slots = worker.running_slots;
        diagnostics.worker_complete_slots = worker.complete_slots;
    }
    snprintf(diagnostics.tls_version, sizeof(diagnostics.tls_version),
             "%s", navigation->last_tls_version);
    snprintf(diagnostics.tls_peer_issuer,
             sizeof(diagnostics.tls_peer_issuer), "%s",
             navigation->last_tls_peer_issuer);
    return psp_write_failure_report_data(
        stage, detail, url, navigation->last_http_status, 0,
        &diagnostics, false);
}

#ifdef TILEFINCH_PSP_LIVE_NETWORK
bool psp_write_network_failure_report(const PspNetwork *network)
{
    /* Independent of file persistence: opt-in details remain in RAM and can
       be shared through Diagnostic QR even when Save error reports is off. */
    if (psp_network_diagnostics_enabled())
        (void) psp_network_diagnostics_format(network, psp_wifi_report,
                                             sizeof(psp_wifi_report));
    else psp_wifi_diagnostic_clear();
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
        network->native_result, &diagnostics, false);
}
#endif

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

static bool psp_media_platform_write_failure_report(
    void *context, const char *stage, const char *detail,
    const char *url, long http_status, int native_result)
{
    (void) context;
    return psp_write_failure_report(
        stage, detail, url, http_status, native_result);
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
PSP_VALIDATION_SHARED TILEFINCH_COLD_PATH void psp_exit_console(
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

#ifdef TILEFINCH_PSP_LIVE_NETWORK
#ifdef TILEFINCH_PSP_VALIDATION_LOG
static void psp_entropy_report_to_log(const char *line)
{
    psp_log_printf("%s\n", line);
}
#endif

/* The TLS entropy source (docs/engineering/PSP_TRANSPORT.md, "Entropy").
   Configured before any thread exists that could reach TLS or DNS; the seed
   file is only read and rewritten when the pool first seeds, so boot does no
   extra Memory Stick I/O. Shipping builds refuse output until timing jitter
   has been credited. Validation builds run under PPSSPP, whose emulated
   clock may earn no credit, so they release after one bounded attempt and log
   which way it went; a device run must show mode=credited. The path lives
   only in the entropy module, keeping main's frame unchanged. */
static TILEFINCH_COLD_PATH void psp_entropy_setup(
    const TilefinchInstallPaths *install_paths)
{
    char seed_path[TILEFINCH_INSTALL_PATH_LIMIT];
    bool have_path = tilefinch_install_data_path(
        install_paths, "entropy-seed.bin", seed_path, sizeof(seed_path));
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_entropy_configure(have_path ? seed_path : NULL,
                          PSP_ENTROPY_PERMIT_UNCREDITED,
                          psp_entropy_report_to_log);
#else
    psp_entropy_configure(have_path ? seed_path : NULL,
                          PSP_ENTROPY_REQUIRE_CREDIT, NULL);
#endif
}
#endif

static TILEFINCH_COLD_PATH bool psp_save_site_data_on_exit(
    BrowserSession *session, const BrowserProfile *profile,
    bool persistent_site_data_available,
    bool preserve_persisted_cache,
    const TilefinchInstallPaths *install_paths,
    const PspStoragePaths *storage, PspUiState *ui,
    const uint16_t *engine_frame)
{
    bool saved = true;
    bool stick_full = false;
    unsigned cache_mb = browser_profile_persistent_cache_mb(profile);
    bool cache_enabled = persistent_site_data_available && cache_mb != 0
        && !preserve_persisted_cache;
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
    } else if (preserve_persisted_cache) {
        printf("tilefinch-site-data: cache-save=skipped "
               "reason=restore-unread\n");
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

static void psp_site_data_restore_init(
    PspSiteDataRestore *restore, bool cache_enabled,
    unsigned cache_megabytes, bool storage_enabled)
{
    if (restore == NULL) return;
    memset(restore, 0, sizeof(*restore));
    restore->cache_enabled = cache_enabled;
    restore->cache_limits = psp_site_data_limits(cache_megabytes);
    restore->storage_limits = psp_site_data_limits(0);
    restore->phase = storage_enabled
        ? PSP_SITE_DATA_RESTORE_LOCAL_STORAGE
        : (cache_enabled ? PSP_SITE_DATA_RESTORE_CACHE
                         : PSP_SITE_DATA_RESTORE_DONE);
}

static const char *psp_site_data_restore_name(
    PspSiteDataRestorePhase phase)
{
    return phase == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE
        ? "local-storage"
        : (phase == PSP_SITE_DATA_RESTORE_CACHE ? "disk-cache" : "none");
}

static TILEFINCH_OUT_OF_LINE bool psp_site_data_restore_pump(
    PspSiteDataRestore *restore, BrowserSession *session,
    const PspStoragePaths *storage, size_t maximum_bytes)
{
    if (restore == NULL || session == NULL || storage == NULL
        || restore->phase == PSP_SITE_DATA_RESTORE_DISABLED
        || restore->phase == PSP_SITE_DATA_RESTORE_DONE) return false;
    const char *path = restore->phase
            == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE
        ? storage->local_storage : storage->persistent_cache;
    BrowserSessionPersistenceMask mask = restore->phase
            == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE
        ? BROWSER_SESSION_PERSIST_LOCAL_STORAGE
        : BROWSER_SESSION_PERSIST_CACHE;
    const BrowserSessionPersistenceLimits *limits = restore->phase
            == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE
        ? &restore->storage_limits : &restore->cache_limits;
    if (restore->load == NULL) {
        restore->load = browser_session_persistence_load_begin(
            session, path, mask, limits);
        if (restore->load == NULL) {
            printf("tilefinch-site-data: deferred-%s load=unavailable\n",
                   psp_site_data_restore_name(restore->phase));
            restore->phase = restore->phase
                    == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE
                && restore->cache_enabled
                ? PSP_SITE_DATA_RESTORE_CACHE
                : PSP_SITE_DATA_RESTORE_DONE;
            return true;
        }
    }
    BrowserSessionPersistenceStatus status =
        BROWSER_SESSION_PERSISTENCE_OK;
    BrowserSessionPersistenceLoadProgress progress =
        browser_session_persistence_load_pump(
            restore->load, maximum_bytes, &status);
    restore->pumps++;
    restore->bytes_budgeted += maximum_bytes;
    if (progress == BROWSER_SESSION_PERSISTENCE_LOAD_PENDING)
        return true;
    PspSiteDataRestorePhase completed = restore->phase;
    browser_session_persistence_load_destroy(restore->load);
    restore->load = NULL;
    printf(
        "tilefinch-site-data: deferred-%s load=%s pumps=%zu "
        "budget=%zuB\n",
        psp_site_data_restore_name(completed),
        status == BROWSER_SESSION_PERSISTENCE_OK
            ? "ok" : browser_session_persistence_status_name(status),
        restore->pumps, restore->bytes_budgeted);
    restore->pumps = 0;
    restore->bytes_budgeted = 0;
    restore->phase = completed == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE
            && restore->cache_enabled
        ? PSP_SITE_DATA_RESTORE_CACHE : PSP_SITE_DATA_RESTORE_DONE;
    return true;
}

static TILEFINCH_OUT_OF_LINE bool psp_site_data_restore_gate_navigation(
    PspSiteDataRestore *restore, BrowserSession *session,
    const PspStoragePaths *storage)
{
    if (restore == NULL) return false;
    if (restore->phase == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE) {
        (void) psp_site_data_restore_pump(
            restore, session, storage, 16u * KIB);
        if (restore->phase == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE)
            return true;
    }
    /* A destination has started. Restored cache bodies are merely an
       optimization, and committing them now could replace responses the new
       page just admitted. Keep localStorage's semantic gate above, but drop
       unfinished cache staging and let this navigation populate a new cache. */
    if (restore->phase == PSP_SITE_DATA_RESTORE_CACHE) {
        browser_session_persistence_load_destroy(restore->load);
        restore->load = NULL;
        restore->cache_restore_cancelled_unread = true;
        restore->phase = PSP_SITE_DATA_RESTORE_DONE;
        printf("tilefinch-site-data: deferred-disk-cache load=cancelled "
               "reason=navigation\n");
    }
    return false;
}

static void psp_site_data_restore_destroy(PspSiteDataRestore *restore)
{
    if (restore == NULL) return;
    browser_session_persistence_load_destroy(restore->load);
    restore->load = NULL;
    restore->phase = PSP_SITE_DATA_RESTORE_DONE;
}

static void psp_site_data_restore_cancel_mask(
    PspSiteDataRestore *restore, BrowserSessionPersistenceMask mask)
{
    if (restore == NULL) return;
    if ((mask & BROWSER_SESSION_PERSIST_CACHE) != 0) {
        restore->cache_enabled = false;
        /* An explicit clear intentionally retires the unread generation; a
           newly populated live cache may be saved on exit. */
        restore->cache_restore_cancelled_unread = false;
        if (restore->phase == PSP_SITE_DATA_RESTORE_CACHE) {
            browser_session_persistence_load_destroy(restore->load);
            restore->load = NULL;
            restore->phase = PSP_SITE_DATA_RESTORE_DONE;
        }
    }
    if ((mask & BROWSER_SESSION_PERSIST_LOCAL_STORAGE) != 0) {
        if (restore->phase == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE) {
            browser_session_persistence_load_destroy(restore->load);
            restore->load = NULL;
            restore->phase = restore->cache_enabled
                ? PSP_SITE_DATA_RESTORE_CACHE
                : PSP_SITE_DATA_RESTORE_DONE;
        }
    }
}

static void psp_site_data_restore_pause(PspSiteDataRestore *restore)
{
    if (restore == NULL) return;
    browser_session_persistence_load_destroy(restore->load);
    restore->load = NULL;
}

static TILEFINCH_OUT_OF_LINE bool psp_site_data_restore_idle_pump(
    PspApp *app, const PspUiInput *input, bool page_dirty,
    bool render_job_pending)
{
    if (app == NULL || input == NULL) return false;
    if (app->interactive->site_data_restore.phase
            == PSP_SITE_DATA_RESTORE_DISABLED
        || app->interactive->site_data_restore.phase
            == PSP_SITE_DATA_RESTORE_DONE) return false;
    /* Which frame facts and foreground writers defer restoration is declared
       once, in the frame-pump policy table. Cache bodies are the expendable
       half and additionally stay out of the association ladder. */
    FramePumpId pump = app->interactive->site_data_restore.phase
            == PSP_SITE_DATA_RESTORE_CACHE
        ? FRAME_PUMP_SITE_RESTORE_CACHE : FRAME_PUMP_SITE_RESTORE_STORAGE;
    uint32_t local_facts =
        (input->held != 0 || input->pressed != 0
             ? FRAME_FACT_INPUT_BUTTONS : 0u)
        | (page_dirty ? FRAME_FACT_PAGE_DIRTY : 0u)
        | (render_job_pending ? FRAME_FACT_RENDER_JOB : 0u);
    if (!psp_app_frame_pump_admit(app, pump, local_facts)) return false;
    if (!psp_site_data_restore_pump(
            &app->interactive->site_data_restore, app->browser->session,
            &app->process->storage, 16u * KIB)) return false;
    psp_app_frame_pump_ran(app, pump);
    return true;
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

static TILEFINCH_OUT_OF_LINE bool psp_navigation_suggests_wifi_sign_in(
    PspInteractiveState *interactive, BrowserNavigationJobStatus status,
    const NavigationSession *navigation, const char *error,
    TilefinchTlsGuidance tls_guidance)
{
    if (interactive == NULL || navigation == NULL) return false;
    if (tls_guidance == TILEFINCH_TLS_GUIDANCE_REDIRECTED) return true;
    bool transport_failure = navigation->last_transport_code != 0
        || navigation->last_transport_timed_out
        || (error != NULL
            && (strstr(error, "background hop") != NULL
                || strstr(error, "background transport") != NULL));
    if (status == BROWSER_NAVIGATION_JOB_CANCELLED
        || navigation->last_http_status != 0 || !transport_failure
        || tls_guidance != TILEFINCH_TLS_GUIDANCE_NONE) return false;
    if (interactive->captive_portal_failure_count < 2u)
        interactive->captive_portal_failure_count++;
    return interactive->captive_portal_failure_count >= 2u;
}

static TILEFINCH_OUT_OF_LINE void psp_navigation_present_failure(
    PspProcessResources *process, PspBrowserResources *browser,
    PspEngineViews *views, PspInteractiveState *interactive,
    BrowserNavigationJobStatus status)
{
    if (process == NULL || browser == NULL || views == NULL
        || views->navigation == NULL || interactive == NULL) return;
    if (psp_offline_store_take_download_offer(&browser->offline_store, browser->engine)) {
        psp_ui_show_file_download(&process->presentation.ui,
            psp_offline_store_file_preview(&browser->offline_store));
        return;
    }
    /* A hard/cancelled navigation failure still overlays its incumbent; only
       post-commit blank recovery may route Return through history. */
    interactive->recovery_offer =
        psp_recovery_offer_without_return(interactive->recovery_offer);
    if (interactive->basic_reload_pending) {
        /* The script-free Basic reload ended without a commit. */
        interactive->basic_reload_pending = false;
        (void) browser_engine_set_javascript_enabled(
            browser->engine, browser_profile_javascript_allowed_for_url(
                                 browser->profile,
                                 interactive->basic_reload_url));
    }
    if (status != BROWSER_NAVIGATION_JOB_CANCELLED) {
        (void) psp_write_navigation_failure_report(
            "navigation", browser_engine_last_error(browser->engine),
            process->presentation.ui.url, views->navigation);
    }
    char visible_error[PSP_UI_STATUS_CAPACITY];
    TilefinchTlsGuidance tls_guidance = TILEFINCH_TLS_GUIDANCE_NONE;
    const char *visible_detail = "PAGE LOAD STOPPED";
    if (status != BROWSER_NAVIGATION_JOB_CANCELLED) {
        BrowserViewSnapshot failed_view;
        (void) browser_engine_view_snapshot(browser->engine, &failed_view);
        visible_detail = psp_user_visible_error(
            browser_engine_last_error(browser->engine),
            failed_view.tls_verify_result,
            failed_view.tls_verify_result_available,
            failed_view.tls_verification_failed,
            &tls_guidance,
            visible_error, sizeof(visible_error));
    }
    bool wifi_sign_in = psp_navigation_suggests_wifi_sign_in(
            interactive, status, views->navigation,
            browser_engine_last_error(browser->engine),
            tls_guidance);
    if (wifi_sign_in) {
        psp_ui_show_wifi_sign_in_status(
            &process->presentation.ui,
            "THIS WI-FI MAY REQUIRE SIGN-IN", 600);
    } else if (tls_guidance == TILEFINCH_TLS_GUIDANCE_NONE) {
        psp_ui_show_status(
            &process->presentation.ui, visible_detail, 300);
    } else {
        psp_ui_show_tls_status(
            &process->presentation.ui, visible_detail, tls_guidance, 300);
    }
    if (status != BROWSER_NAVIGATION_JOB_CANCELLED
        && process->presentation.ui.url[0] != '\0') {
        snprintf(interactive->lifecycle_retry_url,
                 sizeof(interactive->lifecycle_retry_url), "%s",
                 process->presentation.ui.url);
        interactive->recovery_offer = PSP_RECOVERY_OFFER_OVER_INCUMBENT;
        uint8_t actions = psp_basic_fallback_failure_actions(
            browser_profile_basic_fallback_mode(browser->profile), false,
            false,
            psp_basic_fallback_navigation_reloadable(
                browser_engine_last_http_status(browser->engine),
                psp_ui_internal_url(interactive->lifecycle_retry_url)));
        if (wifi_sign_in) actions |= PSP_UI_FAILURE_WIFI;
        if (browser_profile_javascript_allowed_for_url(
                browser->profile, interactive->lifecycle_retry_url))
            actions |= PSP_UI_FAILURE_DISABLE_JAVASCRIPT;
        if (youtube_watch_url_supported(
                interactive->lifecycle_retry_url)) {
            if (!browser_profile_youtube_audio_only(browser->profile))
                actions |= PSP_UI_FAILURE_AUDIO_ONLY;
            if (browser_profile_youtube_quality(browser->profile)
                    == BROWSER_YOUTUBE_QUALITY_360P)
                actions |= PSP_UI_FAILURE_LOWER_QUALITY;
        }
        psp_ui_show_failure_recovery(
            &process->presentation.ui, visible_detail, actions);
    }
    psp_report_job_failure(
        "navigation", NULL, (int) status,
        browser_engine_last_http_status(browser->engine),
        browser_engine_last_error(browser->engine));
}

static TILEFINCH_OUT_OF_LINE void psp_suspend_pending_navigation(
    PspBrowserResources *browser, PspInteractiveState *interactive)
{
    if (browser == NULL || interactive == NULL) return;
    const char *pending_url =
        browser_engine_pending_navigation_url(browser->engine);
    interactive->recovery_offer = pending_url != NULL
        ? PSP_RECOVERY_OFFER_OVER_INCUMBENT : PSP_RECOVERY_OFFER_NONE;
    if (pending_url != NULL) {
        snprintf(interactive->lifecycle_retry_url,
                 sizeof(interactive->lifecycle_retry_url), "%s",
                 pending_url);
    }
    browser_engine_cancel_navigation(browser->engine, "system suspended");
}

/* The update endpoint the profile's channel selects: the signed channel
   metadata, or Developer's explicit local URLs. */
static PspUpdateSessionOptions psp_profile_update_options(
    const PspProcessResources *process, const PspBrowserResources *browser)
{
    return (PspUpdateSessionOptions) {
        .channel = browser_profile_update_channel(browser->profile),
        .developer_metadata_url = process->config.developer_update_url,
        .developer_package_url = process->config.developer_package_url
    };
}

/* Release discovery is an explicit, rare Settings action. Keep its network
   preparation and session replacement out of the resident frame-loop symbol;
   the loop retains only the event gate. */
static TILEFINCH_COLD_PATH bool psp_handle_update_version_intent(
    PspProcessResources *process, PspBrowserResources *browser,
    PspEngineViews *engine_views, const PspUiIntent *intent
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    , PspNetwork *network, PspNetworkLifecycle *network_lifecycle
#endif
)
{
    if (process == NULL || browser == NULL || engine_views == NULL
        || intent == NULL) return false;
    if (browser_session_captive_portal_active(browser->session)) {
        psp_ui_show_status(
            &process->presentation.ui,
            "UNAVAILABLE DURING WI-FI SIGN-IN", 150);
        return true;
    }
    if (intent->update_versions_requested) {
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        char history_url[256];
        bool have_history_url = tilefinch_update_history_url(
            TILEFINCH_UPDATE_REPOSITORY_OWNER,
            TILEFINCH_UPDATE_REPOSITORY_NAME,
            history_url, sizeof(history_url));
        bool network_ready = have_history_url
            && psp_ensure_network_for_navigation(
                   network, network_lifecycle,
                   (int) process->config.network_profile,
                   "GET", history_url, false,
                   engine_views->frame, &process->presentation.ui);
        psp_ui_set_loading(&process->presentation.ui, false, 0);
        bool started = network_ready
            && psp_update_session_begin_history(
                   &browser->update_session,
                   &process->presentation.ui);
        if (!started) {
            TilefinchUpdateHistorySnapshot failed = {
                .phase = TILEFINCH_UPDATE_HISTORY_ERROR
            };
            psp_ui_set_update_history(&process->presentation.ui, &failed);
        }
#else
        TilefinchUpdateHistorySnapshot failed = {
            .phase = TILEFINCH_UPDATE_HISTORY_ERROR
        };
        psp_ui_set_update_history(&process->presentation.ui, &failed);
#endif
    }
    if (intent->update_versions_closed) {
        psp_update_session_cancel_history(
            &browser->update_session, &process->presentation.ui);
    }
    if (intent->update_version_selected) {
        char release_tag[16];
        if (psp_ui_update_history_tag(
                &process->presentation.ui, intent->list_index,
                release_tag, sizeof(release_tag))) {
            psp_update_session_destroy(&browser->update_session);
            (void) psp_update_session_initialize(
                &browser->update_session, browser->budget,
                &process->install_paths,
                &(PspUpdateSessionOptions) {
                    .channel = BROWSER_UPDATE_CHANNEL_STABLE,
                    .release_tag = release_tag,
                    .allow_downgrade = true
                }, &process->presentation.ui);
        } else {
            psp_ui_show_status(
                &process->presentation.ui,
                "OLDER VERSION IS NOT AVAILABLE", 180);
        }
    }
    return true;
}

static TILEFINCH_OUT_OF_LINE void psp_present_committed_recovery(
    PspApp *app, const char *loaded_url, const char *detail,
    uint8_t actions, bool heavy);

static TILEFINCH_OUT_OF_LINE void psp_present_blank_reader_unavailable(
    PspApp *app, const char *loaded_url,
    const ReaderDocumentAnalysis *analysis)
{
    psp_present_committed_recovery(
        app, loaded_url, "PAGE SCRIPTS STOPPED BEFORE CONTENT APPEARED", 0u,
        false);
    printf("tilefinch-reader-recovery: available=no kind=%s confidence=%s\n",
           reader_page_kind_name(
               analysis == NULL ? READER_PAGE_RAW : analysis->kind),
           analysis != NULL && analysis->high_confidence
               ? "high" : "bounded");
}

/* The recovery sheet over a committed page: Return restores the entry the
   page came from. A heavy page's sheet is titled for a page too heavy for
   the PSP (psp_ui_show_heavy_recovery). */
static TILEFINCH_OUT_OF_LINE void psp_present_committed_recovery(
    PspApp *app, const char *loaded_url, const char *detail,
    uint8_t actions, bool heavy)
{
    if (app == NULL || app->interactive == NULL || loaded_url == NULL) return;
    BrowserEngine *engine = app->browser->engine;
    BrowserProfile *profile = app->browser->profile;
    PspUiState *ui = &app->process->presentation.ui;
    if (browser_profile_javascript_allowed_for_url(profile, loaded_url))
        actions |= PSP_UI_FAILURE_DISABLE_JAVASCRIPT;
    snprintf(app->interactive->lifecycle_retry_url,
             sizeof(app->interactive->lifecycle_retry_url), "%s",
             loaded_url);
    /* The page committed, so Return must restore the entry it came from;
       with no history predecessor that is native Home. */
    switch (browser_engine_last_navigation_return_target(engine)) {
    case BROWSER_NAVIGATION_RETURN_BACK:
        app->interactive->recovery_offer = PSP_RECOVERY_OFFER_RETURN_BACK;
        break;
    case BROWSER_NAVIGATION_RETURN_FORWARD:
        app->interactive->recovery_offer = PSP_RECOVERY_OFFER_RETURN_FORWARD;
        break;
    case BROWSER_NAVIGATION_RETURN_NONE:
    default:
        app->interactive->recovery_offer = PSP_RECOVERY_OFFER_RETURN_HOME;
        break;
    }
    app->interactive->blank_reader_recovery_pending = false;
    if (heavy) psp_ui_show_heavy_recovery(ui, detail, actions);
    else psp_ui_show_failure_recovery_actions(ui, detail, actions);
}

void psp_present_heavy_recovery(PspApp *app, const char *loaded_url,
                                const char *detail, uint8_t actions)
{
    psp_present_committed_recovery(app, loaded_url, detail, actions, true);
}

/* The Basic reload's commit, compared as request keys (normalized, without
   the fragment). A redirect or a different navigation is not presented.
   Out of line so its two key buffers leave the stack before the caller
   goes on into Basic view preparation. */
static TILEFINCH_OUT_OF_LINE bool psp_basic_reload_targets(const char *loaded, const char *wanted)
{
    char loaded_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    char wanted_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (loaded == NULL || wanted == NULL) return false;
    if (tilefinch_url_request_key(loaded, loaded_key, sizeof(loaded_key))
        && tilefinch_url_request_key(wanted, wanted_key, sizeof(wanted_key)))
        return strcmp(loaded_key, wanted_key) == 0;
    return strcmp(loaded, wanted) == 0;
}

static TILEFINCH_OUT_OF_LINE void psp_apply_reader_after_navigation_dirty(
    PspApp *app, bool *page_dirty)
{
    BrowserEngine *engine = app->browser->engine;
    BrowserProfile *profile = app->browser->profile;
    PspUiState *ui = &app->process->presentation.ui;
    /* A successful commit supersedes any recovery provenance from its
       incumbent. Blank post-commit recovery below records the new target. */
    app->interactive->recovery_offer =
        psp_recovery_offer_without_return(app->interactive->recovery_offer);
    char loaded_entry_buffer[NAVIGATION_URL_LIMIT];
    const char *loaded_entry_url = psp_app_current_url(
        app->browser->engine, loaded_entry_buffer, sizeof(loaded_entry_buffer));
    const char *loaded_url = loaded_entry_url == NULL
        ? ui->url : loaded_entry_url;
    if (app->interactive->basic_reload_pending) {
        /* The one script-free load is over: later loads follow the site's
           policy again. Only the requested address is presented in Basic
           (a different commit means the user navigated elsewhere). */
        app->interactive->basic_reload_pending = false;
        (void) browser_engine_set_javascript_enabled(
            engine, browser_profile_javascript_allowed_for_url(
                        profile, loaded_url));
        if (psp_basic_reload_targets(
                loaded_url, app->interactive->basic_reload_url)
            && !ui->reader_mode && !ui->basic_mode) {
            app->interactive->blank_reader_recovery_pending = false;
            ReaderDocumentAnalysis reload_analysis = {0};
            bool shown = browser_engine_prepare_basic_view(
                    engine, &reload_analysis)
                && psp_app_set_basic_view(app, loaded_url, true, page_dirty);
            psp_ui_show_status(
                ui, shown ? "BASIC VIEW ON" : "BASIC VIEW UNAVAILABLE", 180);
            printf("tilefinch-basic-recovery: reload=%s nodes=%u "
                   "truncated=%d\n", shown ? "yes" : "no",
                   (unsigned) reload_analysis.extracted_nodes,
                   (int) reload_analysis.extraction_truncated);
            return;
        }
    }
    bool site_requested = browser_profile_reader_site_always(
        profile, loaded_url);
    bool explicit_request = ui->reader_mode || site_requested;
    bool automatic = browser_profile_reader_auto_mode(profile);
    ReaderDocumentAnalysis analysis = {0};

    BrowserDeclaredMediaCard media_card = explicit_request
        ? BROWSER_DECLARED_MEDIA_CARD_NONE
        : browser_engine_prepare_declared_media_card(engine);
    if (media_card == BROWSER_DECLARED_MEDIA_CARD_DEFERRED) {
        app->interactive->blank_reader_recovery_pending = true;
        app->interactive->blank_reader_recovery_generation =
            browser_engine_page_generation(engine);
        return;
    }
    if (media_card == BROWSER_DECLARED_MEDIA_CARD_INSTALLED) {
        app->interactive->blank_reader_recovery_pending = false;
        (void) psp_engine_views_refresh(app->views, engine);
        if (page_dirty != NULL) *page_dirty = true;
        /* The in-page control is the primary, least-destructive recovery.
           Do not immediately hide it behind automatic Basic/Reader or spend
           the one extracted-presentation slot. The user can still request
           either view explicitly. */
        return;
    }

    /* Basic is the action-preserving recovery path. Try it before Reader so
       a degraded search/login shell cannot consume the single extracted-root
       slot with a text-only tree that drops its safe GET controls. Manual and
       per-site Reader requests remain explicit and therefore win; the Basic
       view fallback setting decides between switching, asking and leaving
       the page alone (psp_basic_fallback_*). */
    BrowserBasicFallbackMode fallback =
        browser_profile_basic_fallback_mode(profile);
    BrowserBasicViewRecovery basic_recovery =
        psp_basic_fallback_consulted(fallback, explicit_request)
            ? browser_engine_prepare_basic_view_recovery(engine, &analysis)
            : BROWSER_BASIC_VIEW_RECOVERY_NONE;
    switch (psp_basic_fallback_decide(fallback, basic_recovery)) {
    case PSP_BASIC_FALLBACK_DEFER:
        app->interactive->blank_reader_recovery_pending = true;
        app->interactive->blank_reader_recovery_generation =
            browser_engine_page_generation(engine);
        return;
    case PSP_BASIC_FALLBACK_SWITCH:
        app->interactive->blank_reader_recovery_pending = false;
        if (!psp_app_set_basic_view(app, loaded_url, true, page_dirty)) {
            psp_present_blank_reader_unavailable(
                app, loaded_url, &analysis);
            return;
        }
        printf("tilefinch-basic-recovery: available=yes forms=%u "
               "anchors=%u truncated=%d\n",
               (unsigned) analysis.retained_forms,
               (unsigned) analysis.mapped_anchors,
               (int) analysis.extraction_truncated);
        return;
    case PSP_BASIC_FALLBACK_ASK:
        app->interactive->blank_reader_recovery_pending = false;
        /* Once per page: a declined offer is not repeated. */
        if (app->interactive->basic_prompt_generation
                != browser_engine_page_generation(engine)) {
            app->interactive->basic_prompt_generation =
                browser_engine_page_generation(engine);
            psp_present_committed_recovery(
                app, loaded_url,
                "PAGE SCRIPTS STOPPED. BASIC VIEW CAN SHOW ITS CONTENT",
                psp_basic_fallback_failure_actions(
                    fallback, true, ui->basic_mode, false),
                false);
            printf("tilefinch-basic-recovery: available=yes prompt=ask\n");
        }
        return;
    case PSP_BASIC_FALLBACK_UNAVAILABLE:
        printf("tilefinch-basic-recovery: available=no prepared=%d "
               "visited=%u nodes=%u bytes=%u forms=%u anchors=%u "
               "truncated=%d bounded=%d\n",
               (int) analysis.prepared, (unsigned) analysis.visited_nodes,
               (unsigned) analysis.extracted_nodes,
               (unsigned) analysis.extracted_bytes,
               (unsigned) analysis.retained_forms,
               (unsigned) analysis.mapped_anchors,
               (int) analysis.extraction_truncated,
               (int) analysis.bounded_out);
        break;
    case PSP_BASIC_FALLBACK_KEEP_PAGE:
    default:
        break;
    }

    BrowserBlankReaderRecovery recovery =
        browser_engine_prepare_blank_reader_recovery(engine, &analysis);
    bool degraded_recovery_deferred =
        recovery == BROWSER_BLANK_READER_RECOVERY_DEFERRED;
    if (degraded_recovery_deferred) {
        app->interactive->blank_reader_recovery_pending = true;
        app->interactive->blank_reader_recovery_generation =
            browser_engine_page_generation(engine);
    } else {
        app->interactive->blank_reader_recovery_pending = false;
    }
    bool degraded_recovery =
        recovery == BROWSER_BLANK_READER_RECOVERY_AVAILABLE;
    bool degraded_recovery_unavailable =
        recovery == BROWSER_BLANK_READER_RECOVERY_UNAVAILABLE;
    bool requested = degraded_recovery || explicit_request;
    bool prepared = degraded_recovery;
    if (!degraded_recovery && explicit_request) {
        prepared = browser_engine_prepare_reader(engine, &analysis);
    } else if (!degraded_recovery && automatic
               && !degraded_recovery_deferred) {
        ReaderDocumentAnalysis candidate = {0};
        bool analyzed = browser_engine_analyze_reader(engine, &candidate);
        analysis = candidate;
        requested = analyzed && psp_reader_automatic_admits(&candidate);
        prepared = requested
            && browser_engine_prepare_reader(engine, &analysis);
    }
    if (!requested || !prepared) {
        /* A Reader sheet may have been staged before a candidate navigation
           so its first layout could be simplified. If the destination has no
           extracted semantic tree, remove that sheet transactionally before
           the page is presented rather than restyling a RAW/error/login shell. */
        if (explicit_request && ui->reader_mode) {
            unsigned percent = browser_profile_page_font_percent(profile);
            if (psp_set_presentation_css(
                    engine, ui, profile, false, loaded_url,
                    percent, true)) {
                ui->reader_mode = false;
                ui->basic_mode = false;
                ui->page_font_percent = percent;
                (void) psp_engine_views_refresh(app->views, engine);
                if (page_dirty != NULL) *page_dirty = true;
            }
        }
        if (degraded_recovery_unavailable) {
            psp_present_blank_reader_unavailable(
                app, loaded_url, &analysis);
        }
        return;
    }
    unsigned percent = browser_profile_page_font_percent(profile);
    if (ui->remember_reader_site_scale)
        (void) browser_profile_reader_site_font_percent(
            profile, loaded_url, &percent);
    bool presentation_applied = psp_set_presentation_css(
        engine, ui, profile, true, loaded_url, percent, true);
    if (!presentation_applied
        || !browser_engine_activate_reader_view(engine)) {
        if (presentation_applied) {
            (void) psp_set_presentation_css(
                engine, ui, profile, false, loaded_url,
                browser_profile_page_font_percent(profile), true);
        }
        ui->reader_mode = false;
        ui->basic_mode = false;
        ui->page_font_percent =
            browser_profile_page_font_percent(profile);
        browser_engine_set_reader_candidate_mode(engine, false);
        (void) psp_engine_views_refresh(app->views, engine);
        if (page_dirty != NULL) *page_dirty = true;
        if (degraded_recovery) {
            psp_present_blank_reader_unavailable(
                app, loaded_url, &analysis);
        } else {
            psp_ui_show_status(ui, "READER MODE UNAVAILABLE", 180);
        }
        return;
    }
    if (degraded_recovery) {
        printf("tilefinch-reader-recovery: available=yes kind=%s "
               "confidence=%s\n",
               reader_page_kind_name(analysis.kind),
               analysis.high_confidence ? "high" : "bounded");
    }
    /* Only the high-confidence check engages Reader by itself here; it
       says so, and the next navigation leaves it (psp_reader_policy.h). */
    psp_reader_mark_engaged(
        ui, !degraded_recovery && !explicit_request);
    ui->page_font_percent = percent;
    (void) psp_engine_views_refresh(app->views, engine);
    if (page_dirty != NULL) *page_dirty = true;
}

static TILEFINCH_OUT_OF_LINE void psp_apply_reader_after_navigation(
    PspApp *app, PspAppFrameState *frame)
{
    psp_apply_reader_after_navigation_dirty(
        app, frame == NULL ? NULL : &frame->page_dirty);
}

static TILEFINCH_OUT_OF_LINE void psp_retry_deferred_blank_reader(
    PspApp *app, bool *layout_changed, bool runtime_ok)
{
    if (app == NULL || app->interactive == NULL
        || !app->interactive->blank_reader_recovery_pending) return;
    (void) psp_engine_views_refresh(app->views, app->browser->engine);
    if (browser_engine_page_generation(app->browser->engine)
            != app->interactive->blank_reader_recovery_generation) {
        app->interactive->blank_reader_recovery_pending = false;
        return;
    }
    /* A failed turn (an exhausted realm, a refused frame step) is itself the
       evidence the recovery predicate looks for; only a turn that failed
       because a navigation is pending waits for the next one. */
    if (!runtime_ok && browser_engine_navigation_pending(app->browser->engine))
        return;
    /* The first settled runtime turn gets priority over any automatic
       blank-page fallback. Retry only within the same committed generation;
       psp_apply_reader_after_navigation clears the flag once author content
       appears or Reader admission settles. */
    psp_apply_reader_after_navigation_dirty(app, layout_changed);
    (void) psp_engine_views_refresh(app->views, app->browser->engine);
}

/* Focus and scroll repaint before optional image work. Once that bounded
   render has completed, spend one image-only continuation unit after
   presentation. Keeping the policy out of the resident loop preserves its
   instruction-cache ratchet; this helper is intentionally not cold because it
   is sampled once per browser frame. */
static TILEFINCH_OUT_OF_LINE bool psp_deferred_image_after_present(
    PspApp *app, bool render_job_pending, bool input_active)
{
    NavigationSession *navigation = app == NULL || app->browser == NULL
        ? NULL : browser_engine_navigation(app->browser->engine);
    /* Readiness is local: there must be a committed page with a shell on
       screen. Whether this frame has room for the unit is the declared
       frame-pump policy's decision. */
    if (app == NULL || app->browser == NULL || app->process == NULL
        || navigation == NULL || !navigation->page.loaded
        || browser_engine_render_shell(app->browser->engine) == NULL
        || psp_ui_screen_is_native_surface(
               app->process->presentation.ui.screen)
        || !psp_app_frame_pump_admit(
               app, FRAME_PUMP_DEFERRED_IMAGE,
               (input_active
                    ? FRAME_FACT_INPUT_BUTTONS | FRAME_FACT_INPUT_INTENT : 0u)
               | (render_job_pending ? FRAME_FACT_RENDER_JOB : 0u)))
        return false;
    psp_app_frame_pump_ran(app, FRAME_PUMP_DEFERRED_IMAGE);
    bool visual_changed = false;
    if (app->views != NULL) {
        psp_runtime_cooperate_begin(
            &app->process->presentation.ui, app->views->frame,
            &app->interactive->toolbar_input);
        /* Publishing a batch relayouts the page; presses are served while
           its screens are rebuilt (not while the images load). */
        psp_runtime_cooperate_serve_page_scroll(app->browser->engine);
    }
    (void) browser_engine_run_deferred_image_work(
        app->browser->engine, &visual_changed);
    uint32_t observed_buttons = 0;
    if (psp_runtime_cooperate_end(&observed_buttons))
        app->interactive->previous_buttons = psp_ui_buttons(observed_buttons);
    return visual_changed;
}

/* BrowserEngine cancels its tile job when a candidate navigation starts,
   while the PSP loop owns a separate scheduling bit. Reconcile the two at
   one boundary so a stale bit cannot try to rasterize after the incumbent
   shell has been retired. Native surfaces also have no visible page raster;
   suppressing hidden work there keeps HOME input and omnibox startup cheap.
   Kept out of line because the resident frame function is I-cache ratcheted. */
static TILEFINCH_OUT_OF_LINE bool psp_schedule_page_render_work(
    PspApp *app, PspAppFrameState *frame, bool page_input_active,
    bool *render_job_pending, uint64_t *render_job_last_progress_us)
{
    if (app == NULL || app->browser == NULL || app->process == NULL
        || frame == NULL || render_job_pending == NULL
        || render_job_last_progress_us == NULL) return false;
    BrowserEngine *engine = app->browser->engine;
    NavigationSession *navigation = browser_engine_navigation(engine);
    bool available = engine != NULL
        && !browser_engine_navigation_pending(engine)
        && navigation != NULL && navigation->page.loaded
        && browser_engine_render_shell(engine) != NULL
        && !psp_ui_screen_is_native_surface(
               app->process->presentation.ui.screen);
    if (!available) {
        if (*render_job_pending && engine != NULL)
            browser_engine_cancel_render_job(engine);
        *render_job_pending = false;
        *render_job_last_progress_us = 0;
        frame->page_dirty = false;
        return false;
    }
    if (psp_ui_page_work_paused(&app->process->presentation.ui)) return false;
    if (frame->page_dirty || frame->focus_partial_presented) {
        if (!*render_job_pending) {
            *render_job_last_progress_us =
                (uint64_t) sceKernelGetSystemTimeWide();
        }
        *render_job_pending = true;
        browser_engine_cancel_idle_work(engine);
        return false;
    }
    if (!psp_app_frame_pump_admit(
            app, FRAME_PUMP_PAGE_IDLE,
            (page_input_active
                 ? FRAME_FACT_INPUT_BUTTONS | FRAME_FACT_INPUT_INTENT : 0u)
            | (*render_job_pending ? FRAME_FACT_RENDER_JOB : 0u))) {
        return false;
    }
    psp_app_frame_pump_ran(app, FRAME_PUMP_PAGE_IDLE);
    bool idle_visual_changed = false;
    /* Font/image completion may rebuild a large article. Reuse the lazy,
       owner-thread-only safe-point supervisor used for runtime mutations:
       small slices stay cheap, while long layout keeps input acknowledgement
       and cancellation alive without a concurrent scanout writer. */
    if (app->views != NULL) {
        psp_runtime_cooperate_begin(
            &app->process->presentation.ui, app->views->frame,
            &app->interactive->toolbar_input);
        psp_runtime_cooperate_serve_page_scroll(engine);
    }
    (void) browser_engine_run_idle_work(engine, &idle_visual_changed);
    uint32_t idle_observed_buttons = 0;
    if (psp_runtime_cooperate_end(&idle_observed_buttons))
        app->interactive->previous_buttons = psp_ui_buttons(idle_observed_buttons);
    if (idle_visual_changed) {
        psp_presentation_bind_chrome_fonts(
            &app->process->presentation, engine);
        frame->page_dirty = true;
        *render_job_pending = true;
        *render_job_last_progress_us =
            (uint64_t) sceKernelGetSystemTimeWide();
    }
    return true;
}

/* A navigation commit and a local-surface transition can retire the page
   shell after the earlier scheduling decision. Recheck at the point of use;
   shell loss is an expected cancellation boundary, not a render failure to
   expose to the user. */
static TILEFINCH_OUT_OF_LINE void psp_reconcile_page_render_before_raster(
    PspApp *app, PspAppFrameState *frame, bool *render_job_pending,
    uint64_t *render_job_last_progress_us)
{
    if (app == NULL || app->browser == NULL || app->process == NULL
        || frame == NULL || render_job_pending == NULL
        || render_job_last_progress_us == NULL
        || !*render_job_pending) return;
    BrowserEngine *engine = app->browser->engine;
    NavigationSession *navigation = browser_engine_navigation(engine);
    if (engine != NULL && !browser_engine_navigation_pending(engine)
        && navigation != NULL && navigation->page.loaded
        && browser_engine_render_shell(engine) != NULL
        && !psp_ui_screen_is_native_surface(
               app->process->presentation.ui.screen)) {
        return;
    }
    if (engine != NULL) browser_engine_cancel_render_job(engine);
    *render_job_pending = false;
    *render_job_last_progress_us = 0;
    frame->page_dirty = false;
}


/* The loading page's input in a supervised load: the supervisor forwards
   button edges; on the page it scrolls and cancels itself, so a held button
   must not also repeat as focus movement here. */
static TILEFINCH_OUT_OF_LINE void psp_loading_page_take_input(
    PspUiInput *input, const PspUiState *ui)
{
    bool page_screen = ui->screen == PSP_UI_SCREEN_PAGE;
    input->pressed = psp_navigation_cooperate_owner_frame(page_screen);
    if (page_screen)
        input->held &= ~(PSP_UI_BUTTON_UP | PSP_UI_BUTTON_DOWN
                         | PSP_UI_BUTTON_PAGE_UP | PSP_UI_BUTTON_PAGE_DOWN
                         | PSP_UI_BUTTON_CANCEL);
}

/* The loading page under this loop's cursor and menus: at once for input,
   otherwise at the supervisor's pace. */
static TILEFINCH_OUT_OF_LINE void psp_loading_page_present(
    const PspUiState *ui, uint64_t *last_present_us, uint64_t now_us,
    bool urgent)
{
    if (!urgent && now_us - *last_present_us
                       < PSP_NAVIGATION_PRESENT_INTERVAL_US) return;
    if (psp_navigation_cooperate_owner_present(ui))
        *last_present_us = now_us;
}

/* X on a control of the loading page: the commit kept its focus, so act on
   it now. */
static TILEFINCH_OUT_OF_LINE void psp_run_deferred_activation(
    PspApp *app, PspAppFrameState *frame)
{
    if (!browser_engine_take_deferred_activation(app->browser->engine))
        return;
    PspUiIntent deferred = {0};
    deferred.action = PSP_UI_ACTION_ACTIVATE;
    frame->preview_pointer = false;
    psp_app_dispatch_action(app, frame, &deferred);
}

static TILEFINCH_OUT_OF_LINE bool psp_dispatch_page_pointer(
    PspApp *app, const PspUiIntent *intent, bool *activate, bool *changed)
{
    /* Hover can execute author listeners and relayout just like a timer.
       Keep native input alive on the retained frame throughout that work. */
    bool supervise = !psp_navigation_cooperate_active();
    if (supervise)
        psp_runtime_cooperate_begin(&app->process->presentation.ui,
                                    app->views->frame, &app->interactive->toolbar_input);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t started = sceKernelGetSystemTimeWide();
#endif
    bool okay = browser_engine_pointer_event(
        app->browser->engine, (ControllerPointerPhase) intent->pointer_phase,
        intent->pointer_x, intent->pointer_y, activate, changed);
    uint32_t observed_buttons = 0;
    if (supervise && psp_runtime_cooperate_end(&observed_buttons))
        app->interactive->previous_buttons = psp_ui_buttons(observed_buttons);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t elapsed = sceKernelGetSystemTimeWide() - started;
    if (elapsed > UINT64_C(33000))
        printf("tilefinch-pointer-dispatch: phase=%u elapsed=%lluus changed=%d\n",
               (unsigned) intent->pointer_phase, (unsigned long long) elapsed,
               *changed ? 1 : 0);
#endif
    return okay;
}

/* Set when a frame's runtime advance really turned the page's event loop
   (not deferred behind a render, a native menu or a handoff); the next
   frame's wait consumes it, so a frame that never reaches its runtime step
   cannot skip vblank twice on stale state. */
static bool psp_page_runtime_turned;
static TilefinchRuntimeClock psp_page_runtime_clock;

static TILEFINCH_OUT_OF_LINE bool psp_advance_page_runtime(
    PspApp *app, bool *layout_changed)
{
    if (layout_changed != NULL) *layout_changed = false;
    if (app == NULL || app->browser == NULL || app->interactive == NULL
        || app->process == NULL) return false;
    /* A pressure handoff deliberately splits the two QuickJS graph
       collections across frames. Do not run author tasks between those
       passes: they can rebuild the cycles just collected and spend CPU while
       the native player is waiting for decoder admission. */
    bool page_visible = app->process->presentation.ui.base_screen
                            == PSP_UI_SCREEN_PAGE
        && !app->browser->media.ui.visible;
    (void) browser_engine_set_page_visibility(
        app->browser->engine, page_visible);
    /* Native menus own the input turn. Keep page state retained, but do not
       start another author task/reflow between successive menu presses. */
    if (psp_ui_page_work_paused(&app->process->presentation.ui)) return true;
    if (app->interactive->provider_handoff_present_pending) {
        psp_vprobe_runtime_deferred(PSP_VPROBE_DEFER_HANDOFF);
        return true;
    }
    if (browser_engine_optional_memory_reclaim_pending(
            &app->interactive->provider_handoff_reclaim)) {
        psp_vprobe_runtime_deferred(PSP_VPROBE_DEFER_RECLAIM);
        return true;
    }
    /* A second animation callback would invalidate the tiles that are still
       publishing the preceding canvas frame. Finish that latest-wins frame
       first; scheduler time advances by one ordinary tick when callbacks
       resume, so this neither builds a rAF backlog nor accelerates game time. */
    if (browser_engine_render_frame_pending(app->browser->engine)) {
        psp_vprobe_runtime_deferred(PSP_VPROBE_DEFER_RENDER);
        return true;
    }
    PspVprobeRuntime probe;
    psp_vprobe_runtime_begin(&probe, app->browser->engine);
    /* The engine framebuffer remains the last completed page during runtime
       mutation/layout. Only the owner thread may present it at cooperative
       checkpoints: arbitrary author tasks can also submit synchronous GE
       work, so the callback-thread presenter must remain excluded. */
    if (page_visible && app->views != NULL)
        psp_runtime_cooperate_begin(
            &app->process->presentation.ui, app->views->frame,
            &app->interactive->toolbar_input);
    psp_page_runtime_turned = true;
    /* The page's timers follow the wall clock, at most one tick per
       advance (include/tilefinch/runtime_clock.h): an early turn must not
       run timers or animation frames ahead of real time. */
    unsigned clock_ms = tilefinch_runtime_clock_step(
        &psp_page_runtime_clock, (uint64_t) sceKernelGetSystemTimeWide(),
        (unsigned) app->process->config.tick_ms);
    bool advanced = browser_engine_advance_runtime(
        app->browser->engine, clock_ms, 2, layout_changed);
    uint32_t runtime_observed_buttons = 0;
    if (psp_runtime_cooperate_end(&runtime_observed_buttons))
        app->interactive->previous_buttons = psp_ui_buttons(runtime_observed_buttons);
    psp_vprobe_runtime_end(&probe, app->browser->engine, layout_changed);
    return advanced;
}

/* A location change requested by page script (a redirect, a reload, a
   script-driven form) becomes an ordinary navigation job: loading chrome,
   progress, the stall watchdog and Circle to cancel. Loading it inside the
   runtime advance froze the UI for the whole load; chatgpt.com's startup
   recovery reload held one advance for 52 s on hardware. */
static TILEFINCH_OUT_OF_LINE void psp_begin_script_navigation(PspApp *app)
{
    char url[NAVIGATION_URL_LIMIT];
    bool record_history = true;
    BrowserEngine *engine = app->browser->engine;
    /* A form page script submits after its own async work (chatgpt.com
       posts the prompt once its verification finishes) is a navigation
       too; retain its script provenance through the normal action/retry path. */
    ControllerAction action = {0};
    if (browser_engine_take_script_form_submission(engine, &action)) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        printf("tilefinch-script-form-submission: method=%.8s url=\"%.200s\" "
               "body=%zu\n", action.method, action.url, action.body_length);
#endif
        psp_ui_set_loading(&app->process->presentation.ui, true, -1);
        psp_ui_show_status(&app->process->presentation.ui,
                           "LOADING - CIRCLE CANCELS", 120);
        psp_navigation_cooperate_begin(
            &app->process->presentation.ui,
            app->views == NULL ? NULL : app->views->frame, engine);
        psp_text_input_before_navigation(&app->process->text_input);
        if (psp_retry_navigation_action_after_reclaim(
                engine, &action, PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS)) {
            app->interactive->navigation_job_started_us =
                (uint64_t) sceKernelGetSystemTimeWide();
        } else {
            psp_navigation_cooperate_end("script-form-start-failed");
            psp_ui_set_loading(&app->process->presentation.ui, false, 0);
            psp_ui_show_status(&app->process->presentation.ui,
                               browser_engine_last_error(engine), 300);
        }
        return;
    }
    if (!browser_engine_take_script_navigation(
            engine, url, sizeof(url), &record_history)) {
        /* A <meta http-equiv=refresh> or Refresh header that came due is
           the same kind of job; it replaces the current history entry. */
        NavigationRefreshTake refresh = browser_engine_take_refresh_navigation(
            engine, url, sizeof(url));
        if (refresh == NAVIGATION_REFRESH_TAKE_STOPPED) {
            psp_ui_show_status(&app->process->presentation.ui,
                               "AUTO-REFRESH STOPPED", 240);
            return;
        }
        /* A same-URL auto-reload of a page the reader has been using is
           offered, not forced: Square is the page's reload control. */
        if (refresh == NAVIGATION_REFRESH_TAKE_OFFERED) {
            psp_ui_show_status(&app->process->presentation.ui,
                               PSP_UI_STATUS_RELOAD_OFFER, 600);
            return;
        }
        if (refresh != NAVIGATION_REFRESH_TAKE_NAVIGATE) return;
        record_history = false;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    printf("tilefinch-script-navigation: history=%d url=\"%.200s\"\n",
           record_history ? 1 : 0, url);
#endif
    if (psp_begin_page_load(
            engine, &app->process->presentation.ui, app->browser->profile,
            app->views == NULL ? NULL : app->views->frame,
            &app->process->text_input, url, record_history,
            PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS))
        app->interactive->navigation_job_started_us =
            (uint64_t) sceKernelGetSystemTimeWide();
}

static TILEFINCH_OUT_OF_LINE void
psp_advance_page_runtime_with_reader_recovery(
    PspApp *app, bool *layout_changed)
{
    bool runtime_ok = psp_advance_page_runtime(app, layout_changed);
    if (app != NULL && app->browser != NULL && app->interactive != NULL
        && app->process != NULL && app->views != NULL
        && browser_engine_page_loaded(app->browser->engine)) {
        /* Pages are examined for the fallback at commit; one whose realm
           fails or is retired later (memory, a refused frame step) is
           examined once more for this navigation. */
        const BrowserEngine *engine = app->browser->engine;
        uint64_t generation = browser_engine_page_generation(engine);
        const PspUiState *ui = &app->process->presentation.ui;
        if (psp_basic_fallback_recheck(
                browser_profile_basic_fallback_mode(app->browser->profile),
                ui->basic_mode || ui->reader_mode,
                app->interactive->blank_reader_recovery_pending, runtime_ok,
                /* Current-page memory refusal or retirement, not ordinary
                   widget errors or JavaScript disabled for the site. */
                browser_engine_page_script_memory_failed(engine),
                app->interactive->basic_failure_examined_generation
                    == generation)) {
            app->interactive->basic_failure_examined_generation =
                generation;
            app->interactive->blank_reader_recovery_pending = true;
            app->interactive->blank_reader_recovery_generation =
                generation;
        }
    }
    if (app != NULL && app->browser != NULL && app->interactive != NULL
        && app->process != NULL) {
        psp_begin_script_navigation(app);
        if (psp_open_keyboard_for_late_script_focus(app)
            && layout_changed != NULL)
            *layout_changed = true;
    }
    if (app != NULL && app->interactive != NULL
        && app->interactive->blank_reader_recovery_pending) {
        psp_retry_deferred_blank_reader(app, layout_changed, runtime_ok);
    }
}

static TILEFINCH_OUT_OF_LINE void psp_log_parser_checkpoint_refusals(
    const NavigationPerformance *performance)
{
    if (performance == NULL
        || performance->parser_checkpoint_soft_refusals == 0u) return;
    printf("tilefinch-parser-checkpoint-soft: total=%zu metadata=%zu "
           "fingerprint=%zu stylesheet=%zu mutation=%zu cssom=%zu "
           "script=%zu stage-us=%llu stage-work=%zu stage-skipped=%zu "
           "breakers=%zu reasons=%zu/%zu/%zu\n",
           performance->parser_checkpoint_soft_refusals,
           performance->parser_checkpoint_metadata_refusals,
           performance->parser_checkpoint_fingerprint_refusals,
           performance->parser_checkpoint_stylesheet_refusals,
           performance->parser_checkpoint_mutation_refusals,
           performance->parser_checkpoint_cssom_refusals,
           performance->parser_checkpoint_script_refusals,
           (unsigned long long) performance->parser_script_stage_us,
           performance->parser_script_stage_work,
           performance->parser_script_stage_skipped,
           performance->parser_script_stage_breakers,
           performance->parser_script_stage_failure_breakers,
           performance->parser_script_stage_time_breakers,
           performance->parser_script_stage_work_breakers);
}

/* Ordinary page changes retain the four-tile/2 ms input-latency discipline.
   A canvas animation has already spent its JavaScript turn producing a whole
   coherent frame, so finish as many visible tiles as fit in one 12 ms slice.
   This keeps the compositor slice below one 60 Hz interval while preventing
   rAF from repeatedly cancelling partial frames. */
static TILEFINCH_OUT_OF_LINE BrowserRenderJobStatus psp_render_page_job(
    BrowserEngine *engine, const TilefinchCancellation *cancellation)
{
    bool canvas = browser_engine_canvas_frame_pending(engine);
    return browser_engine_render_frame_bounded_cancelable(
        engine, canvas ? UINT64_C(12000) : PSP_RENDER_JOB_BUDGET_US,
        canvas ? 48u : PSP_RENDER_JOB_MAXIMUM_TILES, cancellation);
}

/* A page/cursor frame publishes at the next vblank, so another
   full wait at the top of the following loop would cap continuous canvas
   animation and unfinished scroll raster at 30 Hz. Start those follow-up
   slices immediately: their
   eventual publication wait is the cooperative yield and preserves scanout
   synchronization. A frame with page work already waiting (see
   psp_loop_page_work_waiting) yields briefly instead of waiting for a
   vblank that would only delay it. Otherwise pages retain the established
   vblank throttle. */
static TILEFINCH_OUT_OF_LINE void psp_wait_before_interactive_input(
    bool fullscreen_media_poll, bool fast_page_followup,
    bool page_work_waiting, uint32_t canvas_wait_us)
{
    /* A successful video publication already waited for the actual scanout
       latch. Spend that pacing credit once, not another poll sleep before
       preparing its successor. No new publication (including a refused one)
       keeps the ordinary yield, so an early/empty decoder cannot busy-spin.
       Observe page/paused turns too: they must not leave stale video credit. */
    static unsigned previous_presents;
    bool video_followup = fullscreen_media_poll
        && psp_display_video_active(&psp_display)
        && psp_display.presents != previous_presents;
    previous_presents = psp_display.presents;
    unsigned ledger = work_ledger_enter(WORK_LEDGER_SLEEP);
    if (fullscreen_media_poll) {
        if (!video_followup)
            (void) sceKernelDelayThread(PSP_MEDIA_FULLSCREEN_POLL_YIELD_US);
    } else if (page_work_waiting) {
        (void) sceKernelDelayThread(PSP_PAGE_TASK_YIELD_US);
    } else if (canvas_wait_us != 0) {
        (void) sceKernelDelayThread(canvas_wait_us);
    } else if (!fast_page_followup) {
        sceDisplayWaitVblankStart();
    }
    work_ledger_leave(ledger);
}


static TILEFINCH_OUT_OF_LINE bool psp_update_page_fullscreen(
    PspApp *app, PspUiInput *input);

/* The page gets controls only after an explicit sustained chord. Input is
   sampled once here, before author callbacks, and published as one fixed
   Gamepad object. The firmware HOME callback is not part of SceCtrlData and
   therefore remains an unconditional exit path even while capture is active. */
static TILEFINCH_OUT_OF_LINE bool psp_update_page_gamepad(
    PspApp *app, PspUiInput *input,
    unsigned elapsed_ms, uint64_t sampled_us)
{
    if (app == NULL || app->browser == NULL || app->interactive == NULL
        || app->process == NULL || app->views == NULL || input == NULL)
        return false;
    PspUiState *ui = &app->process->presentation.ui;
    bool visual_changed = psp_update_page_fullscreen(app, input);
    TilefinchGamepadCapture *capture = &app->interactive->gamepad_capture;
    const uint32_t chord_buttons =
        PSP_UI_BUTTON_ADDRESS | PSP_UI_BUTTON_MENU;
    bool chord = (input->held & chord_buttons) == chord_buttons;
    bool page_requested = visual_changed && ui->page_fullscreen
        && !capture->active;
    if (!capture->active && !capture->chord_held && !chord
        && !page_requested)
        return visual_changed;
    bool page_available = ui->screen == PSP_UI_SCREEN_PAGE
        && !app->browser->media.ui.visible
        && !browser_engine_navigation_pending(app->browser->engine)
        && !(visual_changed && !ui->page_fullscreen)
        && browser_engine_page_gamepad_available(app->browser->engine);
    uint64_t generation = browser_engine_page_generation(app->browser->engine);
    TilefinchGamepadCaptureEvent event = page_requested
        ? tilefinch_gamepad_capture_request(
              capture, page_available, generation)
        : tilefinch_gamepad_capture_step(
              capture, page_available, generation, chord, elapsed_ms);
    visual_changed |= event != TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE;
    if (event == TILEFINCH_GAMEPAD_CAPTURE_ENTERED) {
        /* The exit chord is always announced on a document's first entry
           and on manual chord entry; a page may only quiet its repeated
           claims (requestPageControls {notice: "once"}). Five seconds of
           real time, whatever the page's presentation cadence. */
        bool notice = browser_engine_take_page_controls_notice(
            app->browser->engine);
        if (notice || !page_requested)
            psp_ui_show_status_ms(
                ui, "PAGE CONTROLS ON\nSTART+SELECT EXITS", 5000u);
    } else if (event == TILEFINCH_GAMEPAD_CAPTURE_EXITED) {
        psp_ui_show_status(ui, "PAGE CONTROLS OFF", 120);
    } else if (event == TILEFINCH_GAMEPAD_CAPTURE_UNAVAILABLE) {
        psp_ui_show_status(ui, "PAGE CONTROLS NEED AN ACTIVE JS PAGE", 180);
    }
    ui->page_gamepad_capture = capture->active;

    uint32_t game_buttons = 0;
    int16_t axis_x = 0, axis_y = 0;
    if (capture->suppress_input_until_release && input->held == 0u)
        capture->suppress_input_until_release = false;
    if (capture->active && !capture->suppress_input_until_release) {
        /* The capture chord is browser authority, never game input. Mask it
           so entering the mode cannot also press Start/Select in the game. */
        game_buttons = psp_gamepad_standard_ui_buttons(
            chord ? input->held & ~chord_buttons : input->held);
        game_buttons = tilefinch_gamepad_apply_face_mapping(
            game_buttons,
            ui->gamepad_circle_primary
                ? TILEFINCH_GAMEPAD_FACE_O_PRIMARY
                : TILEFINCH_GAMEPAD_FACE_X_PRIMARY);
        axis_x = tilefinch_gamepad_axis_from_u8(input->analog_x);
        axis_y = tilefinch_gamepad_axis_from_u8(input->analog_y);
    }
    bool gamepad_published = tilefinch_gamepad_state_update(
        &app->interactive->gamepad_state, capture->active,
        game_buttons, axis_x, axis_y, sampled_us / UINT64_C(1000));
    if (gamepad_published) {
        (void) browser_engine_set_gamepad_state(
            app->browser->engine, &app->interactive->gamepad_state);
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_input_script_observe_gamepad(
        capture->active, game_buttons, axis_x, axis_y, gamepad_published);
#endif

    if (tilefinch_gamepad_capture_consumes_browser_input(capture)) {
        input->pressed = 0;
        input->held = 0;
        input->analog_x = 128;
        input->analog_y = 128;
    }
    return visual_changed;
}

/* Fullscreen is uncommon per frame and belongs outside the ratcheted
   interactive-loop body. This synchronizes the standards state with native
   chrome. Triangle remains a browser-owned escape for ordinary fullscreen,
   but Page controls owns every game button; its sole escape is the sustained
   Start+Select chord handled by psp_update_page_gamepad(). */
static TILEFINCH_OUT_OF_LINE bool psp_update_page_fullscreen(
    PspApp *app, PspUiInput *input)
{
    if (app == NULL || app->browser == NULL || app->process == NULL
        || input == NULL) return false;
    PspUiState *ui = &app->process->presentation.ui;
    bool active = browser_engine_page_fullscreen_active(
        app->browser->engine);
    bool changed = ui->page_fullscreen != active;
    if (changed) {
        ui->page_fullscreen = active;
        ui->chrome_visible = !active;
    }
    if (ui->page_fullscreen
        && (app->interactive == NULL
            || !app->interactive->gamepad_capture.active)
        && (input->pressed & PSP_UI_BUTTON_TOOLBAR) != 0) {
        (void) browser_engine_exit_page_fullscreen(app->browser->engine);
        ui->page_fullscreen = false;
        ui->chrome_visible = true;
        input->pressed &= ~PSP_UI_BUTTON_TOOLBAR;
        input->held &= ~PSP_UI_BUTTON_TOOLBAR;
        changed = true;
    }
    return changed;
}

static TILEFINCH_OUT_OF_LINE void psp_suspend_page_runtime(
    PspBrowserResources *browser, PspProcessResources *process,
    PspInteractiveState *interactive)
{
    if (browser == NULL || process == NULL || interactive == NULL) return;
    browser_engine_cancel_idle_work(browser->engine);
    (void) browser_engine_set_page_visibility(browser->engine, false);
    browser_engine_suspend_page_presentations(browser->engine);
    process->presentation.ui.page_fullscreen = false;
    process->presentation.ui.chrome_visible = true;
    psp_site_data_restore_pause(&interactive->site_data_restore);
}

/* Site-information mutations are deliberate menu operations, never resident
   frame work. Keep their storage/profile reconciliation out of the Allegrex
   loop's instruction-cache footprint. */
static TILEFINCH_OUT_OF_LINE void psp_apply_storage_and_site_intent(
    PspProcessResources *process, PspBrowserResources *browser,
    PspInteractiveState *interactive, PspEngineViews *engine_views,
    const PspUiIntent *intent, uint64_t sampled_us, bool *page_dirty)
{
    PspApp login_app = {.process = process, .browser = browser, .interactive = interactive};
    if (process == NULL || browser == NULL || interactive == NULL
        || engine_views == NULL || intent == NULL || page_dirty == NULL)
        return;
    if (intent->clear_cache_requested) {
        psp_site_data_restore_cancel_mask(
            &interactive->site_data_restore,
            BROWSER_SESSION_PERSIST_CACHE);
        (void) browser_session_persistence_clear(
            browser->session, BROWSER_SESSION_PERSIST_CACHE);
        /* That emptied the compiled-script tier as well. */
        process->presentation.ui.compiled_scripts_kib = 0;
        /* Resumption blobs are credentials, not an independent hidden
           cache. The user's cache-clear operation erases both stores. */
        (void) fetch_tls_session_store_clear();
        BrowserSessionPersistenceStatus status =
            process->persistent_site_data_available
                ? browser_session_persistence_remove(
                      process->storage.persistent_cache)
                : BROWSER_SESSION_PERSISTENCE_OK;
        psp_ui_show_status(
            &process->presentation.ui,
            status == BROWSER_SESSION_PERSISTENCE_OK
                ? "HTTP CACHES CLEARED"
                : "CACHE FILE COULD NOT BE REMOVED",
            240);
        return;
    }
    if (intent->clear_compiled_scripts_requested) {
        bool cleared = browser_session_script_disk_clear(browser->session);
        uint64_t bytes = 0;
        (void) browser_session_script_disk_usage(browser->session, &bytes,
                                                 NULL);
        process->presentation.ui.compiled_scripts_kib =
            (unsigned) ((bytes + 1023u) / 1024u);
        psp_ui_show_status(
            &process->presentation.ui,
            cleared ? "COMPILED SCRIPTS CLEARED"
                    : "SOME FILES COULD NOT BE CLEARED", 180);
        return;
    }
    if (intent->clear_cookies_requested) {
        browser_session_cookie_clear(browser->session);
        bool removed = psp_app_youtube_login_clear(&login_app);
        psp_ui_show_status(
            &process->presentation.ui, removed ? "COOKIES CLEARED"
                : "COOKIES CLEARED - SAVED LOGIN NOT REMOVED", 240);
        return;
    }
    if (intent->clear_local_storage_requested) {
        psp_site_data_restore_cancel_mask(
            &interactive->site_data_restore,
            BROWSER_SESSION_PERSIST_LOCAL_STORAGE);
        bool cleared = browser_session_persistence_clear(
            browser->session, BROWSER_SESSION_PERSIST_LOCAL_STORAGE)
            == BROWSER_SESSION_PERSISTENCE_OK;
        BrowserSessionPersistenceStatus status =
            process->persistent_site_data_available
                ? browser_session_persistence_remove(
                      process->storage.local_storage)
                : BROWSER_SESSION_PERSISTENCE_OK;
        psp_ui_show_status(
            &process->presentation.ui,
            !cleared ? "SOME DATA COULD NOT BE CLEARED"
                : status == BROWSER_SESSION_PERSISTENCE_OK
                    ? "LOCAL STORAGE CLEARED"
                    : "LOCAL FILE COULD NOT BE REMOVED",
            240);
        return;
    }
    if (intent->clear_session_storage_requested) {
        bool cleared =
            browser_session_storage_clear_all(browser->session, false);
        cleared = browser_session_opfs_clear_all(browser->session)
            && cleared;
        psp_ui_show_status(
            &process->presentation.ui,
            cleared ? "SESSION STORAGE CLEARED"
                    : "SOME DATA COULD NOT BE CLEARED", 180);
        return;
    }
    char site_url[NAVIGATION_URL_LIMIT];
    snprintf(site_url, sizeof(site_url), "%s", process->presentation.ui.url);
    if (intent->clear_site_data_requested) {
        bool cleared = browser_session_clear_site_data(
            browser->session, site_url);
        char site[CONTENT_BLOCKER_HOST_LIMIT];
        /* The registrable site: every YouTube host is "youtube.com". */
        if (cleared && content_blocker_site_from_url(site_url, site)
            && strcmp(site, "youtube.com") == 0)
            cleared = psp_app_youtube_login_clear(&login_app);
        if (cleared) {
            /* Persisted snapshots are rewritten from the now-cleared
               in-memory authority at ordinary shutdown; no extra stick
               write is introduced by opening Site information. */
            psp_sync_ui(
                &process->presentation.ui, browser->engine,
                browser->profile);
        }
        psp_ui_show_status(
            &process->presentation.ui,
            cleared ? "SITE DATA CLEARED"
                    : "SITE DATA COULD NOT BE CLEARED",
            240);
        return;
    }
    bool reset = browser_profile_reset_site_permissions(
            browser->profile, site_url)
        && browser_session_set_mixed_content_site_allowed(
            browser->session, site_url, false)
        && browser_session_set_third_party_cookie_site_allowed(
            browser->session, site_url, false)
        && psp_content_blocker_apply_allowed_sites(
            browser->engine, browser->profile);
    if (reset) {
        (void) browser_engine_set_javascript_enabled(
            browser->engine,
            browser_profile_javascript_enabled(browser->profile));
        psp_profile_store_mark_dirty(&browser->profile_store, sampled_us);
        char entry_buffer[NAVIGATION_URL_LIMIT];
        const char *entry_url = psp_app_current_url(
            browser->engine, entry_buffer, sizeof(entry_buffer));
        (void) psp_set_presentation_css(
            browser->engine, &process->presentation.ui,
            browser->profile,
            process->presentation.ui.reader_mode
                || process->presentation.ui.basic_mode,
            entry_url == NULL ? site_url : entry_url,
            process->presentation.ui.page_font_percent, true);
        (void) psp_engine_views_refresh(engine_views, browser->engine);
        *page_dirty = true;
        psp_sync_ui(
            &process->presentation.ui, browser->engine,
            browser->profile);
    }
    psp_ui_show_status(
        &process->presentation.ui,
        reset ? "SITE PERMISSIONS RESET"
              : "SITE PERMISSIONS COULD NOT RESET",
        240);
}


/* The Update screen's primary button: install-and-restart, or begin the
   network check for the selected release. Rare and network-bound, so it
   stays out of the resident frame loop. */
static TILEFINCH_COLD_PATH void psp_handle_update_primary(
    PspProcessResources *process, PspBrowserResources *browser,
    PspEngineViews *engine_views, PspInteractiveState *interactive
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    , PspNetwork *network, PspNetworkLifecycle *network_lifecycle
#endif
)
{
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
        PspUpdateSessionOptions selected_update =
            psp_profile_update_options(process, browser);
        char selected_update_url[768];
        bool have_selected_update_url =
            browser->update_session.allow_downgrade
            ? psp_update_session_selected_metadata_url(
                  &browser->update_session, selected_update_url,
                  sizeof(selected_update_url))
            : psp_update_session_metadata_url(
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
}

#ifdef TILEFINCH_PSP_LIVE_NETWORK
/* A background update check has just finished. A completed check advances
   the weekly cadence and records whether a release is waiting (toasting it
   once per boot); a failed one only logs. Returns whether the chrome
   changed. */
static TILEFINCH_COLD_PATH bool psp_finish_update_check(
    PspProcessResources *process, PspBrowserResources *browser,
    uint64_t now_us, PspUpdateCheck *check)
{
    bool visual_changed = false;
    TilefinchUpdateClientPhase check_phase =
        browser->update_session.client_snapshot.phase;
    bool check_found_release =
        check_phase == TILEFINCH_UPDATE_CLIENT_AVAILABLE
        || check_phase == TILEFINCH_UPDATE_CLIENT_DOWNLOADED;
    if (check_found_release
        || check_phase
               == TILEFINCH_UPDATE_CLIENT_UP_TO_DATE) {
        /* Only a completed check advances the weekly
           cadence; failed or cancelled attempts stay silent
           and retry on a later boot. */
        check->completed++;
        time_t update_check_done = time(NULL);
        if (update_check_done > 0)
            browser_profile_set_update_check_last_unix(
                browser->profile, (uint64_t) update_check_done);
        if (check_found_release) {
            check->available++;
            browser_profile_set_update_check_available_sequence(
                browser->profile,
                browser_profile_update_channel(browser->profile)
                        == BROWSER_UPDATE_CHANNEL_DEVELOPER
                    ? TILEFINCH_UPDATE_DEVELOPER_SEQUENCE
                    : browser->update_session.client_snapshot
                          .manifest.release_sequence);
            process->presentation.ui.update_release_available = true;
            if (!check->toasted) {
                check->toasted = true;
                psp_ui_show_status(
                    &process->presentation.ui, "UPDATE READY - SEE OPTIONS", 240);
                visual_changed = true;
            }
        } else {
            browser_profile_set_update_check_available_sequence(
                browser->profile, 0);
            process->presentation.ui.update_release_available = false;
        }
        psp_profile_store_mark_dirty(
            &browser->profile_store, now_us);
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
    return visual_changed;
}
#endif

/* Advances the pending screenshot PNG writer one slice when the frame-pump
   policy admits it, reporting progress in tenths and the saved name or
   failure at the end. Returns whether the status line changed. */
static TILEFINCH_COLD_PATH bool psp_pump_screenshot(PspApp *app)
{
    if (!psp_app_frame_pump_run(app, FRAME_PUMP_SCREENSHOT)) return false;
    PspProcessResources *process = app->process;
    PspBrowserResources *browser = app->browser;
    PspInteractiveState *interactive = app->interactive;
    bool visual_changed = false;
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
        visual_changed = true;
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
        visual_changed = true;
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
        visual_changed = true;
    }
    return visual_changed;
}

/* Imports the content-blocker allowlist file from the storage card into
   the profile and reapplies it. Returns whether the page must repaint
   (only when the import took effect). */
static TILEFINCH_COLD_PATH bool psp_load_content_blocker_allowlist(
    PspProcessResources *process, PspBrowserResources *browser,
    PspEngineViews *engine_views, uint64_t now_us)
{
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
            &browser->profile_store, now_us);
    process->presentation.ui.content_blocker_site_allowed =
        process->presentation.ui.content_blocker_mode != CONTENT_BLOCKER_OFF
        && browser_profile_content_blocker_site_allowed(
               browser->profile, process->presentation.ui.url);
    if (loaded) {
        char entry_buffer[NAVIGATION_URL_LIMIT];
        const char *entry_url = psp_app_current_url(
            browser->engine, entry_buffer, sizeof(entry_buffer));
        (void) psp_set_presentation_css(
            browser->engine, &process->presentation.ui,
            browser->profile,
            process->presentation.ui.reader_mode
                || process->presentation.ui.basic_mode,
            entry_url == NULL ? process->presentation.ui.url : entry_url,
            process->presentation.ui.page_font_percent, true);
        (void) psp_engine_views_refresh(engine_views, browser->engine);
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
    return loaded;
}

/* psp_loop_frame runs one frame as a fixed sequence of steps. Steps that a
   page or media frame runs every time are hot boundaries, measured together
   by the build's frame-set ratchets (cmake/CheckPspHotSymbolSizes.cmake);
   the rest run only when their check in psp_loop_frame says so. */
static TILEFINCH_HOT_BOUNDARY void psp_loop_frame(PspLoop *loop);

/* Every page or media frame, in frame order. */
static TILEFINCH_HOT_BOUNDARY bool psp_loop_media_input(
    PspLoop *loop, PspUiInput *input);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
static TILEFINCH_HOT_BOUNDARY void psp_loop_home_preconnect(
    PspLoop *loop, unsigned ui_elapsed_ms);
#endif
static TILEFINCH_HOT_BOUNDARY bool psp_loop_update_frame(
    PspLoop *loop, uint64_t now_us, PspUiIntent *intent,
    bool *navigation_visual_changed);
static TILEFINCH_HOT_BOUNDARY void psp_loop_pump_navigation(
    PspLoop *loop, PspAppFrameState *frame);
static TILEFINCH_HOT_BOUNDARY void psp_loop_media_frame(
    PspLoop *loop, PspAppFrameState *frame, unsigned media_elapsed_ms,
    bool *media_visual_changed);
static TILEFINCH_HOT_BOUNDARY unsigned psp_loop_render_job(
    PspLoop *loop, PspAppFrameState *frame);
static TILEFINCH_HOT_BOUNDARY void psp_loop_track_recovery(PspLoop *loop);
static TILEFINCH_OUT_OF_LINE void psp_loop_recovery_moved(
    PspLoop *loop, uint64_t generation, int entry_scroll, bool changed,
    uint64_t recovery_now_us);
/* A dirty record still for two seconds, and 30 since the last save. */
static inline bool psp_recovery_save_due(const PspRecoveryTracker *recovery,
                                         uint64_t now_us)
{
    return recovery->dirty
        && now_us - recovery->last_change_us >= UINT64_C(2000000)
        && (recovery->last_save_us == 0
            || now_us - recovery->last_save_us >= UINT64_C(30000000));
}

/* Only when their check says so, in frame order. */
static TILEFINCH_OUT_OF_LINE void psp_loop_start_boot_navigation(
    PspLoop *loop, const char *deferred_url);
static TILEFINCH_OUT_OF_LINE bool psp_loop_serve_lifecycle(
    PspLoop *loop, PspLifecycleAction lifecycle_action);
static TILEFINCH_OUT_OF_LINE void psp_loop_confirm_trial_health(
    PspLoop *loop, uint64_t now_us, uint32_t pressed);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
static TILEFINCH_OUT_OF_LINE void psp_loop_fire_update_check(
    PspLoop *loop, bool power_interaction);
#endif
static TILEFINCH_OUT_OF_LINE void psp_loop_cancel_navigation(
    PspLoop *loop, PspAppFrameState *frame);
static TILEFINCH_OUT_OF_LINE void psp_loop_capture_tab_thumbnail(PspLoop *loop);
static TILEFINCH_OUT_OF_LINE void psp_loop_finish_navigation(
    PspLoop *loop, PspAppFrameState *frame,
    BrowserNavigationJobStatus navigation_status,
    const BrowserNavigationJobMetrics *navigation_metrics);
static TILEFINCH_OUT_OF_LINE void psp_loop_pump_offline_download(
    PspLoop *loop, uint32_t offline_download_id);
static TILEFINCH_OUT_OF_LINE void psp_loop_apply_media_request(
    PspLoop *loop, const ScriptMediaRequest *dom_media_request);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
static TILEFINCH_OUT_OF_LINE void psp_loop_report_network_warmup(
    PspLoop *loop, bool network_was_warming,
    PspNetworkStatus previous_network_status);
#endif

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
    bool initial_error_page, bool offline_startup
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    , PspNetwork *network, PspNetworkLifecycle *network_lifecycle
#endif
)
{
    PspInteractiveResult result = {0};
#ifndef TILEFINCH_PSP_VALIDATION_LOG
    (void) argv0;
#endif
    PspLoop loop_state = {
        .boot_queue = boot_queue,
        .home_preconnect_dwell = home_preconnect_dwell,
        .power_policy = power_policy,
        .loaded = loaded,
        .restoring_last_page = restoring_last_page,
        .initial_error_page = initial_error_page
    };
    PspLoop *loop = &loop_state;
    loop->running_slot =
        psp_update_session_current_slot(&process->install_paths);
    bool have_update_state = process->install_paths.slotted
        && tilefinch_update_journal_load(
               process->install_paths.data_dir, &loop->trial_health_state, NULL);
    bool running_developer_slot = process->install_paths.slotted
        && tilefinch_update_slot_is_developer(
               process->install_paths.program_dir);
    if (have_update_state && !running_developer_slot) {
        TilefinchUpdateState raised = loop->trial_health_state;
        if (tilefinch_update_state_raise_installed_floor(
                &raised, loop->running_slot,
                TILEFINCH_RELEASE_SEQUENCE)
            && tilefinch_update_journal_store(
                   process->install_paths.data_dir, &raised, NULL, NULL)) {
            loop->trial_health_state = raised;
            printf(
                "tilefinch-update-floor: reseeded sequence=%llu "
                "slot=%d\n",
                (unsigned long long) raised.installed_sequence,
                (int) loop->running_slot);
        }
    }
    loop->trial_health_pending = have_update_state
        && loop->trial_health_state.trial == TILEFINCH_UPDATE_TRIAL_STARTED
        && loop->trial_health_state.pending_slot == loop->running_slot;
    loop->trial_health_started_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    loop->trial_health_next_attempt_us = loop->trial_health_started_us;
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
    psp_update_e2e_init(
        &loop->update_e2e, process->config.validation_update_auto != 0,
        loop->trial_health_pending);
#endif

    /*
     * Optional background update-metadata check. Latched once per boot
     * and deferred until the app is idle. Signed channels require an
     * embedded root; Developer instead requires its explicit local URL.
     * Trace replay and the Options toggle keep either mode inert.
     */
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    bool background_network_allowed = !offline_startup
        && strcmp(process->config.trace, "none") == 0;
    psp_update_check_init(
        &loop->update_check,
        background_network_allowed
            && process->config.validation_update_auto == 0,
        psp_app_update_check_configured(browser->profile, &process->config));
#endif

#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /*
     * The local homepage is already scanout-visible and interactive at
     * this point. Warm the connection one PSP state-machine phase per UI
     * loop so the first external navigation can reuse work already in
     * flight instead of paying the full association delay.
     */
    if (background_network_allowed
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
        interactive->transport_prepare_pending = true;
    }
#endif

    loop->blocker_navigation_baseline =
        startup_blocked_requests;
    interactive->recovery = (PspRecoveryTracker) {
        .observed_generation = browser_engine_page_generation(browser->engine),
        .last_change_us = (uint64_t) sceKernelGetSystemTimeWide()
    };
    (void) browser_engine_current_entry(
        browser->engine, NULL, 0u, &interactive->recovery.observed_scroll);
    /* Automated PPSSPP smoke runs use the normal report and cleanup
       paths without requiring controller injection to leave the app. */
    if (process->config.exit_after_report != 0)
        psp_exit_plan_request(
            &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
    interactive->media_stability_active = PSP_VALIDATION_KNOB(
        process->config.validation_media_stability_auto != 0);
    interactive->media_stability_auto_exit =
        interactive->media_stability_active;
    loop->media_stability_target_us =
        (uint64_t) process->config.validation_media_stability_seconds
            * UINT64_C(1000000);
    loop->previous_ui_sample_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    /* HOME has just been latched. Sample its first input without another
       vblank wait; the flag is consumed once below, and later frames retain
       the ordinary presentation/cadence rules. */
    loop->fast_page_followup = native_home_boot;
    loop->next_color_mode_check_us =
        loop->previous_ui_sample_us + UINT64_C(60000000);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    interactive->power_auto_boot_pending =
        process->config.validation_power_test_auto != 0;
#endif
    /* Borrow the canonical owners for this loop invocation. Dispatchers may
       mutate through these pointers but never acquire their lifetimes. */
    loop->app = (PspApp) {
        .process = process,
        .browser = browser,
        .views = engine_views,
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        .network = network,
        .network_lifecycle = network_lifecycle,
        .update_check = &loop->update_check,
#endif
        .interactive = interactive
    };
    /* The saved profile number is already in UI state. Its name is only
       visible in Options; the normal lazy Options refresh below owns that
       firmware lookup, never Home's first input sample. */
    psp_validation_interactive_begin(process, browser, interactive, argv0);
    while (!psp_exit_plan_requested(&interactive->exit)
           && !psp_home_exit_pending()) {
        psp_loop_timing_begin();
        psp_loop_frame(loop);
        psp_loop_timing_end();
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
            loop->media_open_scope ? "media-open-exit" : "interactive-exit");
    psp_validation_interactive_end(process, interactive);
    result.update_check_attempts = loop->update_check.attempts;
    result.update_check_ratelimited = loop->update_check.ratelimited;
    result.update_check_completed = loop->update_check.completed;
    result.update_check_available = loop->update_check.available;
    result.power_high_ms = loop->power_high_ms;
    result.power_idle_ms = loop->power_idle_ms;
    result.power_transition_ms = loop->power_transition_ms;
    return result;
}

/* Whether this frame should start without waiting for vblank: the page's
   event loop has a task it could run now (a due timeout or message, a
   complete response, a microtask checkpoint left pending) and the last
   frame's advance really ran it, or the page's raster job is unfinished (it
   also holds page tasks back, see psp_advance_page_runtime). A due frame
   callback does not count: requestAnimationFrame belongs to a rendering
   opportunity. Publication still waits for vblank, so scanout stays
   synchronized; only the idle wait between two slices of work goes.
   Opt-in (boot key page_task_yield=1): the shorter waits starve
   lower-priority I/O threads on the device
   (.private-investigations/PERFORMANCE_LEDGER.md,
   2026-09-29), so ordinary frames keep the vblank wait. */
static TILEFINCH_OUT_OF_LINE bool psp_loop_page_work_waiting(PspLoop *loop)
{
    bool turned = psp_page_runtime_turned;
    psp_page_runtime_turned = false;
    PspApp *const app = &loop->app;
    if (app->process->config.page_task_yield != 1) return false;
    if (psp_ui_page_work_paused(&app->process->presentation.ui)
        || app->browser->media.ui.visible
        || browser_engine_navigation_pending(app->browser->engine))
        return false;
    if (loop->render_job_pending) return true;
    return turned && browser_engine_page_task_runnable(app->browser->engine);
}

/* A just-published canvas may return before the virtual rAF clock's next
   16ms deadline. Consuming fast_followup on an empty advance then adds a
   whole vblank on the following turn. Sleep only the projected remainder,
   retaining the presenter's existing fence and three-buffer ownership. */
static TILEFINCH_OUT_OF_LINE uint32_t psp_canvas_followup_wait(PspLoop *loop)
{
    if (psp_validation_canvas_followup_disabled()) return 0;
    PspApp *app = &loop->app;
    if (!loop->fast_page_followup || !loop->canvas_page_followup
        || app->browser->media.ui.visible
        || psp_ui_page_work_paused(&app->process->presentation.ui)
        || browser_engine_navigation_pending(app->browser->engine)) return 0;
    unsigned delay_ms;
    uint32_t wait_us = 0;
    if (browser_engine_frame_clock_delay_ms(app->browser->engine, &delay_ms))
        (void) tilefinch_runtime_clock_frame_wait(&psp_page_runtime_clock,
            (uint64_t)sceKernelGetSystemTimeWide(),
            (unsigned)app->process->config.tick_ms, delay_ms, &wait_us);
    return wait_us;
}

/* One frame of the interactive loop: input, dispatch, navigation, page
   work, render and present, then the post-present pumps. The order of
   those steps is load-bearing (frame pumps admit work by position).
   Returning early ends only this frame. */
static TILEFINCH_HOT_BOUNDARY void psp_loop_frame(PspLoop *loop)
{
    PspApp *const app = &loop->app;
    PspProcessResources *const process = app->process;
    PspBrowserResources *const browser = app->browser;
    PspInteractiveState *const interactive = app->interactive;
    PspEngineViews *const engine_views = app->views;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork *const network = app->network;
    PspNetworkLifecycle *const network_lifecycle = app->network_lifecycle;
#endif
    PspPowerPolicy *const power_policy = loop->power_policy;
    /* Presentation identifies the active tab's engine frame by this. */
    psp_present_canvas_engine_bind(browser->engine);
    /* Boot can hand off before its initial candidate becomes the active
       session. Measurement marks must follow the refreshed frontend view,
       not keep that boot-time session (which can have no page runtime). */
    psp_vprobe_frame_begin(engine_views->navigation,
        process->config.validation_game_audio_reference == 0);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (psp_input_script_report_pending()) {
        psp_exit_plan_request(
            &interactive->exit, PSP_EXIT_VALIDATION_COMPLETE);
        return;
    }
#endif
    if ((process->presentation.ui.screen == PSP_UI_SCREEN_OPTIONS
         || process->presentation.ui.screen
                == PSP_UI_SCREEN_OPTION_ITEMS)
        && !process->presentation.ui.network_profile_label_valid) {
        psp_app_refresh_network_profile_label(
            app, (int) process->config.network_profile);
    }
    psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
    psp_log_heartbeat();
    psp_log_set_stage("frame");
    /*
     * The navigation boot owed the user. It starts from inside the
     * interactive loop, not in front of it: the surface is already
     * up and taking input, the network warm-up is already running a
     * phase per frame, and a page that never arrives can no longer
     * keep the whole browser closed.
     */
    const char *deferred_url = psp_boot_queue_take(loop->boot_queue);
    if (deferred_url != NULL)
        psp_loop_start_boot_navigation(loop, deferred_url);
    offline_download_manager_set_maximum_height(
        &browser->offline_store.download,
        (int) browser_profile_youtube_quality(browser->profile));
    if (PSP_VALIDATION_KNOB(
            process->config.validation_media_lifecycle_auto != 0))
        psp_loop_inject_media_lifecycle(loop);
    PspLifecycleAction lifecycle_action =
        psp_lifecycle_poll(&psp_lifecycle);
    bool lifecycle_recovered =
        lifecycle_action != PSP_LIFECYCLE_ACTION_NONE
        && psp_loop_serve_lifecycle(loop, lifecycle_action);
    if (psp_lifecycle_state(&psp_lifecycle)
            != PSP_LIFECYCLE_RUNNING) {
        (void) sceKernelDelayThread(10000);
        return;
    }
    if (lifecycle_recovered) return;
    /* The last thing the frame does before it stops is give the codec
       worker something to do while it is stopped. */
    (void) psp_media_feed_before_blocking(&browser->media, false);
    /*
     * The fullscreen presenter itself waits for vblank before it
     * latches a complete frame. Waiting another vblank here samples a
     * 24-fps stream only about 26 times a second: an early sample then
     * lands almost a whole frame late, producing the fast/slow cadence
     * visible on hardware and forcing the late-frame catch-up path.
     *
     * Poll only while a real playing picture owns the panel. Empty turns
     * still yield for 2ms; a completed video latch supplies pacing for one
     * immediate follow-up. Audio and codec workers retain their higher
     * priorities, and publication remains the sole tear-free display clock.
     * Pages, loading/error panels,
     * paused media and seek transactions keep the established vblank
     * throttle.
     */
    bool fullscreen_media_poll = browser->media.ui.visible
        && browser->media.ui.playing && browser->media.have_frame
        && !psp_media_open_work_pending(&browser->media)
        && browser->media.job_phase == PSP_MEDIA_JOB_NONE
        && !browser->media.pause_boundary_pending;
    /* A normal page frame waits here and again when the completed back
       buffer is published. Continuous cursor motion is the exception:
       its previous frame used one of three page buffers, leaving one
       neither scanned nor queued for NEXTFRAME. A short yield samples
       input again without the otherwise mandatory extra vblank. Video
       remains double buffered and uses its separately fenced path. */
    bool queued_page_input = psp_navigation_cooperate_page_input_pending();
    loop->page_task_yield = !queued_page_input && !fullscreen_media_poll
        && !loop->fast_page_followup && psp_loop_page_work_waiting(loop);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_loop_timing_note_wait(
        loop->page_task_yield, loop->fast_page_followup);
#endif
    psp_wait_before_interactive_input(
        fullscreen_media_poll, loop->fast_page_followup || queued_page_input,
        loop->page_task_yield,
        queued_page_input ? 0 : psp_canvas_followup_wait(loop));
    bool canvas_live = loop->canvas_page_followup;
    loop->fast_page_followup = false;
    loop->canvas_page_followup = false;
#ifdef TILEFINCH_HAVE_PSP_VOICE
    if ((++loop->voice_pressure_ticks & 63u) == 0)
        psp_text_input_trim(&process->text_input);
#else
    (void) loop->voice_pressure_ticks;
#endif
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) return;
    PspAppFrameState frame;
    frame.ui_sample_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    frame.replayed_press_us = 0;
    frame.canvas_live = canvas_live || loop->focus_runtime_deferred;
    frame.runtime_deferred = loop->focus_runtime_deferred;
    frame.defer_runtime = false;
    frame_pumps_begin(&interactive->frame_pumps);
    psp_boot_input_probe(frame.ui_sample_us);
    bool color_mode_visual_changed = false;
    if (frame.ui_sample_us >= loop->next_color_mode_check_us) {
        bool was_dark = process->presentation.ui.page_dark;
        psp_refresh_page_color_mode(&process->presentation.ui);
        loop->next_color_mode_check_us =
            frame.ui_sample_us + UINT64_C(60000000);
        color_mode_visual_changed = psp_color_mode_refresh_apply(
            browser->engine, &process->presentation.ui, was_dark);
    }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /* Refresh the cached wifi bar count on its own slow cadence;
       the status line forwards the cache without paying the read. */
    psp_sample_wifi_strength(
        psp_network_lifecycle_ready(network_lifecycle)
            ? network : NULL,
        frame.ui_sample_us);
#endif
    uint64_t ui_elapsed_us = frame.ui_sample_us - loop->previous_ui_sample_us;
    loop->previous_ui_sample_us = frame.ui_sample_us;
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
    PSP_LOOP_PHASE(LOOP_INPUT);
    PspUiInput input = psp_ui_input(
        &pad, interactive->previous_buttons, ui_elapsed_ms);
    /* During a supervised load the callback supervisor is the one
       receiver of button edges (scrolling and Circle act at engine
       checkpoints) and forwards the rest here; this loop keeps the
       stick and held buttons. */
    bool loading_page_input = psp_navigation_cooperate_supervised()
        && browser_engine_navigation_pending(browser->engine);
    if (!loading_page_input)
        input.pressed |= psp_controller_take_latched_pressed();
    uint32_t supervisor_page_pressed = 0;
    uint64_t supervisor_page_pressed_us = 0;
    bool supervisor_page_input =
        psp_navigation_cooperate_take_page_input(
            &supervisor_page_pressed, &supervisor_page_pressed_us);
    if (supervisor_page_input)
        frame.replayed_press_us = supervisor_page_pressed_us;
    /*
     * The scripted source replaces the sampled pad outright, and only
     * steps forward on a frame the browser could actually have taken
     * the press on (psp_validation_scripted_frame).
     */
    if (!psp_validation_scripted_frame(
            loop, &input, supervisor_page_input, &frame, &ui_elapsed_ms))
        return;
    if (supervisor_page_input) {
        input.pressed |= supervisor_page_pressed;
        input.held |= supervisor_page_pressed;
    }
    if (loading_page_input)
        psp_loading_page_take_input(&input, &process->presentation.ui);
    interactive->previous_buttons = input.held;
    bool gamepad_visual_changed = psp_update_page_gamepad(
        app, &input, ui_elapsed_ms, frame.ui_sample_us);
    uint64_t toolbar_sample_ms = frame.ui_sample_us / 1000u;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (psp_input_script_running())
        toolbar_sample_ms = interactive->toolbar_input.sample_ms + ui_elapsed_ms;
#endif
    bool reader_shortcut = psp_ui_filter_toolbar_input(
        &interactive->toolbar_input, &input,
        process->presentation.ui.screen == PSP_UI_SCREEN_PAGE
            && !browser->media.ui.visible, toolbar_sample_ms);
    reader_shortcut |= interactive->toolbar_input.reader_pending;
    interactive->toolbar_input.reader_pending = false;
    bool power_auto_started_visual = false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool power_auto_abort_requested = false;
    if (interactive->power_auto_boot_pending
        || interactive->power_auto.active)
        power_auto_started_visual = psp_loop_power_test_input(
            loop, &input, frame.ui_sample_us,
            &power_auto_abort_requested);
#endif
    /* A press is the user acting: an unattended run of page refreshes
       starts over (NAVIGATION_REFRESH_CHAIN_LIMIT). */
    if (input.pressed != 0) browser_engine_note_user_input(browser->engine);
    bool power_interaction =
        input.pressed != 0 || input.held != 0
        || input.analog_x < 104 || input.analog_x > 152
        || input.analog_y < 104 || input.analog_y > 152;
    bool visible_active_work =
        process->presentation.ui.loading || loop->render_job_pending || browser->media.ui.visible
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
    PspClockWorkerSnapshot power_snapshot = {0};
    if (process->clock_live) {
        psp_clock_worker_snapshot(
            &process->clock_worker, &power_snapshot);
        if (power_snapshot.completions
                != loop->power_worker_completions
            || power_snapshot.failures
                != loop->power_worker_failures) {
            loop->power_worker_completions =
                power_snapshot.completions;
            loop->power_worker_failures = power_snapshot.failures;
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
        loop->power_transition_ms += ui_elapsed_ms;
    else if (process->clock_live && power_snapshot.applied_idle)
        loop->power_idle_ms += ui_elapsed_ms;
    else
        loop->power_high_ms += ui_elapsed_ms;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool power_auto_visual_changed = psp_loop_power_test_tick(
            loop, frame.ui_sample_us, ui_elapsed_ms, &power_snapshot,
            power_auto_abort_requested)
        || power_auto_started_visual;
#else
    bool power_auto_visual_changed =
        power_auto_started_visual;
#endif
    if (loop->trial_health_pending)
        psp_loop_confirm_trial_health(
            loop, frame.ui_sample_us, input.pressed);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /*
     * Fire the boot-latched background check only once the boot
     * really succeeded, nothing foreground is in flight, a page or
     * the native home screen is showing (not a menu), the user
     * has been idle for the house autohide interval, and an
     * already-joined network is READY. This path never initiates a
     * join; if the network never comes up, the check silently
     * stays pending for this boot.
     */
    if (psp_update_check_armed(&loop->update_check))
        psp_loop_fire_update_check(loop, power_interaction);
#endif
    PspUiIntent intent = {0};
    bool media_visual_changed = false;
    bool navigation_visual_changed =
        color_mode_visual_changed || power_auto_visual_changed;
    bool navigation_pending =
        browser_engine_navigation_pending(browser->engine);
    if (navigation_pending
        && process->presentation.ui.screen == PSP_UI_SCREEN_PAGE
        && (input.pressed & PSP_UI_BUTTON_CANCEL) != 0) {
        psp_loop_cancel_navigation(loop, &frame);
        navigation_pending = false;
        navigation_visual_changed = true;
        input.pressed &= ~PSP_UI_BUTTON_CANCEL;
    }
    if (navigation_pending && !loading_page_input) {
        /* Without a callback supervisor to receive button edges, the
           candidate navigation's own loop polls Circle; nothing else
           may race it. The analog cursor remains chrome-local and
           responsive; its page event is suppressed below until the
           candidate reaches a terminal state. (A supervised load
           forwards presses for the loading page instead.) */
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
        media_visual_changed = psp_loop_media_input(loop, &input);
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
               or a raster slice. The third page scanout buffer also lets
               the next loop sample the nub without another full-vblank
               safety wait. */
            psp_cursor_latency_sample(frame.ui_sample_us);
            cursor_feedback_presented = psp_present_cursor_feedback(
                engine_views->frame, &process->presentation.ui);
            loop->fast_page_followup = cursor_feedback_presented;
        } else {
            psp_cursor_latency_idle();
        }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        /*
         * Speculative preconnect (docs/engineering/
         * PSP_TRANSPORT.md). While a HOME tile settles under
         * focus, warm one reusable HTTP connection in the background so
         * later navigation can avoid TCP and TLS setup.
         * Bounds are
         * carried by the pure dwell state machine: only the user's own
         * built-in provider tile, at most one
         * connection, started only after ~300 ms of focus, cancelled on
         * focus move, never before the network is READY or under the
         * deterministic harness. It sends one bodyless HEAD request. The
         * pump is non-blocking and rides this cooperative
         * checkpoint without a scheduler slot.
         */
        psp_loop_home_preconnect(loop, ui_elapsed_ms);
#endif
        if (process->presentation.ui.screen == PSP_UI_SCREEN_TABS
            && intent.visual_changed && browser->tabs != NULL)
            psp_loop_capture_tab_thumbnail(loop);
    }
    bool voice_component_visual_changed =
        psp_app_background_handle_frame(
            app, &frame, &intent, frame.ui_sample_us);
    bool glyph_component_visual_changed =
        psp_glyph_component_handle_frame(
            app, &intent, frame.ui_sample_us);
    glyph_component_visual_changed |= psp_ui_language_handle_frame(
        app, &intent, frame.ui_sample_us);
    bool update_visual_changed = psp_loop_update_frame(
        loop, frame.ui_sample_us, &intent, &navigation_visual_changed);
    navigation_visual_changed |= psp_app_site_storage_poll(app);
    navigation_visual_changed |= psp_app_heavy_poll(app);
    navigation_visual_changed |= psp_app_youtube_login_poll(app);
    frame.page_dirty = color_mode_visual_changed
        || gamepad_visual_changed;
    navigation_visual_changed |= voice_component_visual_changed
        || glyph_component_visual_changed;
    frame.pointer_activation = false;
    frame.preview_pointer = false;
    if (navigation_pending
        && intent.pointer_phase == PSP_UI_POINTER_UP) {
        /* A click on the loading page acts on its preview. */
        frame.preview_pointer = true;
        frame.preview_pointer_x = intent.pointer_x;
        frame.preview_pointer_y = intent.pointer_y;
        intent.action = PSP_UI_ACTION_ACTIVATE;
    } else if (!navigation_pending
        && intent.pointer_phase != PSP_UI_POINTER_NONE) {
        bool pointer_page_changed = false;
        if (!psp_dispatch_page_pointer(
            app, &intent,
            &frame.pointer_activation, &pointer_page_changed)) {
            frame.pointer_activation = false;
        }
        frame.page_dirty = pointer_page_changed || frame.page_dirty;
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
    PSP_LOOP_PHASE(LOOP_ACTION);
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
        /* Input receipt and operation completion are separate visual
           events. Publish this one before dispatching the action. */
        predispatch_presented = psp_present_action_ack(
            engine_views->frame, &process->presentation.ui,
            intent.action, action_ack, frame.ui_sample_us)
            || predispatch_presented;
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
        psp_app_dispatch_action(app, &frame, &intent);
    /* A script-handled click returns no native control action. Its receipt
       still ends when dispatch returns; any newer operation status survives. */
    if (psp_ui_finish_page_activation(&process->presentation.ui,
            intent.action == PSP_UI_ACTION_ACTIVATE ? action_ack : NULL))
        intent.visual_changed = true;
    if (discrete_operation != 0) {
        psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
        psp_log_operation_end(
            discrete_operation, discrete_action, "returned");
    }

    bool screenshot_visual_changed = false;
    if (interactive->screenshot.writer.status == SCREENSHOT_PNG_PENDING)
        screenshot_visual_changed = psp_pump_screenshot(app);

    psp_app_handle_theme_catalog(app, &frame, &intent);
    if (intent.setting.id != PSP_UI_SETTING_NONE)
        psp_app_apply_setting(app, &frame, &intent);
    if (intent.setting.id == PSP_UI_SETTING_UPDATE_CHANNEL) {
        /* The transport endpoint is immutable for one client. The
           next Update-page entry recreates it for the selected
           channel; there is no cross-channel fallback. */
        psp_update_session_destroy(&browser->update_session);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        psp_update_check_reset(
            &loop->update_check, psp_app_update_check_configured(
                               browser->profile, &process->config));
#endif
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* After both receivers, so the line records a screen the
       dispatch has already moved to rather than the one it left. */
    PSP_LOOP_PHASE(LOOP_PROBE);
    psp_input_script_observe(&intent, &process->presentation.ui);
    psp_input_script_observe_page(engine_views, browser->engine);
#endif
    PSP_LOOP_PHASE(LOOP_NAVIGATION);
    if (intent.load_content_blocker_allowlist_requested) {
        if (psp_load_content_blocker_allowlist(
                process, browser, engine_views, frame.ui_sample_us))
            frame.page_dirty = true;
    }
    if (intent.clear_cache_requested
        || intent.clear_compiled_scripts_requested
        || intent.clear_cookies_requested
        || intent.clear_local_storage_requested
        || intent.clear_session_storage_requested
        || intent.clear_site_data_requested
        || intent.reset_site_permissions_requested)
        psp_apply_storage_and_site_intent(
            process, browser, interactive, engine_views, &intent,
            frame.ui_sample_us, &frame.page_dirty);

    if (intent.scroll_delta != 0) {
        bool log_scroll = (loop->scroll_log_counter++ & 31u) == 0;
        uint32_t scroll_operation = log_scroll
            ? psp_log_operation_begin("analog-scroll") : 0;
        bool provisional = psp_request_provisional_scroll(
            browser->engine, &process->presentation.ui,
            intent.scroll_delta, false);
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
        psp_loop_pump_navigation(loop, &frame);
        navigation_visual_changed = true;
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
    if (offline_download_active)
        psp_loop_pump_offline_download(loop, offline_download_id);

    bool runtime_layout_changed = false;
    PSP_LOOP_PHASE(LOOP_RUNTIME);
    psp_log_heartbeat();
    /* A focus move over a live canvas was composed and presented by its
       action: advancing the animation now would compose the screen a
       second time this iteration. The callbacks run next iteration (at
       most every other one is skipped); page time advances by the usual
       tick, so the game neither speeds up nor builds a backlog. */
    /* A completed scope may have queued the next focus/activation. Do not
       put a fresh task or raster slice in front of an already sampled press. */
    frame.defer_runtime |= psp_navigation_cooperate_page_input_pending();
    loop->focus_runtime_deferred = frame.defer_runtime;
    if (!frame.defer_runtime)
        psp_advance_page_runtime_with_reader_recovery(
            app, &runtime_layout_changed);
    /* Paint-only canvas commits can have no layout delta while still
       leaving a complete backing-surface generation ready to publish.
       Treat that authoritative engine state as render work now; waiting
       for a later generic mutation bit carries the frame across another
       vblank and turns an isolated long callback into a visible hitch. */
    frame.page_dirty = runtime_layout_changed || frame.page_dirty
        || browser_engine_canvas_frame_pending(browser->engine);
    PSP_LOOP_PHASE(LOOP_IDLE);
    ScriptMediaRequest dom_media_request;
    if (browser_engine_consume_media_request(
            browser->engine, &dom_media_request))
        psp_loop_apply_media_request(loop, &dom_media_request);
    (void) psp_site_data_restore_idle_pump(
        app, &input, frame.page_dirty, loop->render_job_pending);
    bool page_input_active = input.pressed != 0 || input.held != 0
        || psp_navigation_cooperate_page_input_pending()
        || intent.action != PSP_UI_ACTION_NONE
        || intent.pointer_phase != PSP_UI_POINTER_NONE
        || intent.scroll_delta != 0;
    /* Observe focus before page-idle admission. This retires an old
       result's resolver in the same frame and lets the newly focused
       thumbnail consume the otherwise blocked idle slice below. */
    psp_app_youtube_preresolve_tick(
        app, &frame, &intent, loop->render_job_pending,
        offline_download_active, page_input_active);
    /* Pre-resolution was pumped above, so it always gets the first
       browser-thread slice and the media transport lane. The helper may
       continue one bounded page-idle slice afterward; it also reconciles
       the frontend render bit when navigation retires the page shell. */
    (void) psp_schedule_page_render_work(
        app, &frame, page_input_active,
        &loop->render_job_pending, &loop->render_job_last_progress_us);
    psp_loop_media_frame(loop, &frame, media_elapsed_ms, &media_visual_changed);
    psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);

    psp_sync_ui(&process->presentation.ui, browser->engine, browser->profile);
    if (process->presentation.ui.cursor_shape != cursor_shape_before_dispatch)
        navigation_visual_changed = true;
    psp_find_sync(browser->engine, &process->presentation.ui, &process->presentation.find_view);
    PSP_LOOP_PHASE(LOOP_RASTER);
    unsigned render_visual_state = 0;
    if (frame.focus_partial_presented) {
        loop->render_placeholder_active = true;
        loop->render_progress_present_us = sceKernelGetSystemTimeWide();
    }
    psp_reconcile_page_render_before_raster(
        app, &frame, &loop->render_job_pending,
        &loop->render_job_last_progress_us);
    /* Time spent serving a native menu is not a stalled raster job. */
    if (loop->render_job_pending
        && (psp_ui_page_work_paused(&process->presentation.ui)
            || psp_navigation_cooperate_page_input_pending()))
        loop->render_job_last_progress_us =
            (uint64_t) sceKernelGetSystemTimeWide();
    if (loop->render_job_pending
        && !psp_navigation_cooperate_page_input_pending()
        && !psp_ui_page_work_paused(&process->presentation.ui))
        render_visual_state = psp_loop_render_job(loop, &frame);
    if (browser_profile_restore_last_page(browser->profile))
        psp_loop_track_recovery(loop);
    bool profile_flush_attempted = false;
    if (!psp_profile_store_flush_due(
            &browser->profile_store, frame.ui_sample_us, UINT64_C(750000),
            &profile_flush_attempted)
        && profile_flush_attempted) {
        printf("tilefinch-profile: deferred save failed\n");
    }
    /* Fullscreen requested by this frame's dispatch or runtime is adopted
       at the next input sample; its first frame must not carry chrome. */
    navigation_visual_changed |= psp_ui_retract_chrome_for_page_fullscreen(
        &process->presentation.ui,
        browser_engine_page_fullscreen_active(browser->engine));
    /* A page-render cancellation may consume page_dirty, but cannot
       consume the native player's first-presentation obligation. Keep
       retrying it independently until scanout publication succeeds. */
    PSP_LOOP_PHASE(LOOP_PRESENT);
    if (frame.page_dirty || render_visual_state != 0
        || interactive->provider_handoff_present_pending
        || (intent.visual_changed
            && !(predispatch_presented && predispatch_complete))
        || media_visual_changed
        || psp_media_menu_feedback_pending()
        || (navigation_visual_changed
            && !cursor_feedback_presented)
        || update_visual_changed || screenshot_visual_changed) {
        if (psp_navigation_cooperate_supervised()
            && browser_engine_navigation_pending(browser->engine)) {
            psp_loading_page_present(
                &process->presentation.ui, &loop->loading_present_us,
                frame.ui_sample_us,
                intent.visual_changed || input.pressed != 0
                    || frame.page_dirty);
        } else if (!psp_navigation_cooperate_supervised()) {
            psp_log_set_phase(PSP_LOG_PHASE_RENDER);
            PspVprobePresent present_probe;
            psp_vprobe_present_begin(&present_probe);
            bool published = psp_present(
                engine_views->frame, &process->presentation.ui);
            if (published)
                loop->render_progress_present_us = sceKernelGetSystemTimeWide();
            psp_vprobe_present_end(
                &present_probe, render_visual_state, published,
                browser->engine);
            /* A partial scroll already waited for safe scanout publication.
               Do not wait a second vblank before its next bounded slice.
               The next turn still samples input before raster, and each
               publication retains the normal three-buffer fence. A failed
               publication must not authorize a wait-free continuation. */
            loop->fast_page_followup |= published
                && (render_visual_state == 2u
                    || (render_visual_state == 1u && loop->render_job_pending));
            loop->canvas_page_followup = published && render_visual_state == 2u;
            psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
            /* Direct Play commits the native player before the optional
               page-memory trim. Resolver work was held above for this
               presentation, so the trim cannot race decoder admission. */
            psp_app_pump_provider_handoff_reclaim(
                app, frame.ui_sample_us, published);
        }
    }
    /* The current input response is already visible. A newly decoded
       image schedules one coalesced render for the next frame. */
    PSP_LOOP_PHASE(LOOP_IMAGES);
    if (psp_deferred_image_after_present(
            app, loop->render_job_pending, page_input_active)) {
        loop->render_job_pending = true;
        loop->render_job_last_progress_us =
            (uint64_t) sceKernelGetSystemTimeWide();
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
    PSP_LOOP_PHASE(LOOP_TAIL);
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
    psp_multiplayer_pump(app);
    psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
    if (network_was_warming)
        psp_loop_report_network_warmup(
            loop, network_was_warming, previous_network_status);
    psp_app_prepare_transport(app,
        (page_input_active ? FRAME_FACT_INPUT_BUTTONS : 0u)
        | (frame.page_dirty ? FRAME_FACT_PAGE_DIRTY : 0u)
        | (loop->render_job_pending ? FRAME_FACT_RENDER_JOB : 0u), false);
    /* Spend association wait frames on one font file at a time. On the
       transition-to-Ready frame, start the selected-site preconnect first;
       any remaining font work yields while that handshake is active. */
    bool baseline_fonts_were_ready =
        browser_engine_baseline_fonts_ready(browser->engine);
    if (!baseline_fonts_were_ready
        && psp_app_frame_pump_run(app, FRAME_PUMP_BASELINE_FONTS)) {
        (void) browser_engine_pump_baseline_fonts(browser->engine);
        if (browser_engine_baseline_fonts_ready(browser->engine)) {
            psp_presentation_bind_chrome_fonts(
                &process->presentation, browser->engine);
        }
    }
#endif
    if (PSP_VALIDATION_KNOB(process->config.interactive_validation_ticks > 0))
        psp_loop_count_validation_tick(loop);
}

static TILEFINCH_OUT_OF_LINE void psp_loop_start_boot_navigation(
    PspLoop *loop, const char *deferred_url)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork *const network = loop->app.network;
    PspNetworkLifecycle *const network_lifecycle = loop->app.network_lifecycle;
#endif
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
            deferred, true, PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS);
    }
    if (deferred_started) {
        interactive->navigation_job_started_us =
            (uint64_t) sceKernelGetSystemTimeWide();
        psp_ui_show_status(
            &process->presentation.ui, loop->restoring_last_page ? "RESTORING LAST PAGE"
                                     : "OPENING MY HOMEPAGE...",
            180);
    } else {
        psp_ui_show_home(&process->presentation.ui);
        psp_ui_show_status(&process->presentation.ui, "LAST PAGE UNAVAILABLE", 180);
    }
    printf(
        "tilefinch-boot-order: deferred-navigation started=%d\n",
        deferred_started ? 1 : 0);
}

/* Serves a system suspend or resume the frame polled (lifecycle_action)
   and at most one more after it. True when a resume was completed: the
   frame then starts over. */
static TILEFINCH_OUT_OF_LINE bool psp_loop_serve_lifecycle(
    PspLoop *loop, PspLifecycleAction lifecycle_action)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork *const network = loop->app.network;
    PspNetworkLifecycle *const network_lifecycle = loop->app.network_lifecycle;
#endif
    PspPowerPolicy *const power_policy = loop->power_policy;
    bool lifecycle_recovered = false;
    for (unsigned lifecycle_step = 0;
         lifecycle_step < 2u; lifecycle_step++) {
        if (lifecycle_step != 0)
            lifecycle_action = psp_lifecycle_poll(&psp_lifecycle);
        if (lifecycle_action == PSP_LIFECYCLE_ACTION_NONE)
            break;
        if (lifecycle_action
                == PSP_LIFECYCLE_ACTION_QUIESCE) {
            if (interactive->gamepad_capture.active) {
                interactive->gamepad_capture.active = false;
                interactive->gamepad_capture.chord_latched = false;
                interactive->gamepad_capture.chord_held = false;
                interactive->gamepad_capture.chord_hold_ms = 0;
                process->presentation.ui.page_gamepad_capture = false;
                if (tilefinch_gamepad_state_update(
                        &interactive->gamepad_state, false, 0, 0, 0,
                        (uint64_t) sceKernelGetSystemTimeWide()
                            / UINT64_C(1000))) {
                    (void) browser_engine_set_gamepad_state(
                        browser->engine, &interactive->gamepad_state);
                }
            }
            bool navigation_was_pending =
                browser_engine_navigation_pending(browser->engine);
            bool render_was_pending = loop->render_job_pending;
            bool media_was_active =
                browser->media.ui.visible
                || psp_media_open_work_pending(&browser->media)
                || psp_media_decode_work_pending(&browser->media);
            bool screenshot_was_pending =
                interactive->screenshot.writer.status
                    == SCREENSHOT_PNG_PENDING;
            if (navigation_was_pending)
                psp_suspend_pending_navigation(browser, interactive);
            psp_youtube_preresolve_reset(
                &browser->youtube_preresolve, "system suspended");
            if (loop->render_job_pending) {
                browser_engine_cancel_render_job(browser->engine);
                loop->render_job_pending = false;
                loop->render_job_last_progress_us = 0;
            }
            if (screenshot_was_pending) {
                screenshot_png_cancel(&interactive->screenshot.writer);
                budget_free(browser->budget, interactive->screenshot.pixels);
                interactive->screenshot.pixels = NULL;
            }
            psp_suspend_page_runtime(browser, process, interactive);
            if (psp_navigation_cooperate_active())
                psp_navigation_cooperate_end("system-suspend");
            /* The media-open owner survives ordinary frame boundaries,
               but this scope was just fenced explicitly. Do not let a
               stale local owner later release a different cooperate
               session installed after resume. */
            loop->media_open_scope = false;
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
                    browser->profile, process->storage.recovery, browser->engine);
            bool profile_saved =
                psp_profile_store_flush(&browser->profile_store);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            loop->lifecycle_network_was_started =
                psp_network_lifecycle_started(network_lifecycle);
            loop->lifecycle_network_was_ready =
                psp_network_lifecycle_ready(network_lifecycle)
                && network->status == PSP_NETWORK_READY;
            loop->lifecycle_network_requests_cancelled = 0;
            if (loop->lifecycle_network_was_started) {
                psp_network_lifecycle_suspend(
                    network_lifecycle, network,
                    loop->lifecycle_network_was_ready, "suspend");
            }
            if (loop->lifecycle_network_was_started
                && !loop->lifecycle_network_was_ready) {
                loop->lifecycle_network_requests_cancelled =
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
                loop->lifecycle_network_requests_cancelled,
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
        /* A deferred canvas frame points into EDRAM that did not survive
           either: drop it so the next present repaints from scratch. */
        image_canvas_native_serial_advance();
        /* The last accepted frame is scanout-safe. Enable presentation
           before the bounded network reconnect so the existing supervisor
           can keep the recovery UI responsive. */
        psp_lifecycle_complete_recovery(&psp_lifecycle);
        interactive->previous_buttons = 0;
        psp_ui_suspend_page_input(&process->presentation.ui);
        loop->previous_ui_sample_us =
            (uint64_t) sceKernelGetSystemTimeWide();
        loop->next_color_mode_check_us =
            loop->previous_ui_sample_us + UINT64_C(60000000);
        bool resume_network_ready = true;
        int resume_network_native = 0;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        if (loop->lifecycle_network_was_ready) {
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
                loop->lifecycle_network_requests_cancelled +=
                    browser_engine_cancel_network_work(
                        browser->engine, "network changed while suspended");
            }
        } else if (loop->lifecycle_network_was_started) {
            resume_network_ready = false;
            psp_network_lifecycle_resume_result(
                network_lifecycle, network, false,
                false, "resume-off");
        }
        if (loop->lifecycle_network_was_started
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
        loop->lifecycle_network_was_started = false;
        loop->lifecycle_network_was_ready = false;
#endif
        psp_media_resume(&browser->media);
        psp_ui_show_status(
            &process->presentation.ui,
            interactive->recovery_offer != PSP_RECOVERY_OFFER_NONE
                ? "WELCOME BACK - SQUARE RETRIES PAGE"
                : (resume_network_ready
                       ? "WELCOME BACK" : "RESUMED OFFLINE"),
            interactive->recovery_offer != PSP_RECOVERY_OFFER_NONE
                ? 360 : 180);
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
            loop->lifecycle_network_requests_cancelled,
            psp_media_open_work_pending(&browser->media) ? 1 : 0,
            sceKernelTotalFreeMemSize(),
            sceKernelMaxFreeMemSize());
        psp_present(engine_views->frame, &process->presentation.ui);
        lifecycle_recovered = true;
    }
    return lifecycle_recovered;
}



/* A freshly installed slot runs on trial until the first input, or ten
   seconds, proves it usable; this records that in the update journal. */
static TILEFINCH_OUT_OF_LINE void psp_loop_confirm_trial_health(
    PspLoop *loop, uint64_t now_us, uint32_t pressed)
{
    PspProcessResources *const process = loop->app.process;
    if (loop->trial_health_pending
        && now_us >= loop->trial_health_next_attempt_us
        && (pressed != 0
            || now_us - loop->trial_health_started_us
                   >= UINT64_C(10000000))) {
        TilefinchUpdateState healthy = loop->trial_health_state;
        if (tilefinch_update_state_confirm_healthy(&healthy)
            && tilefinch_update_journal_store(
                   process->install_paths.data_dir, &healthy, NULL, NULL)) {
            loop->trial_health_state = healthy;
            loop->trial_health_pending = false;
            printf(
                "tilefinch-update-health: confirmed input=%d "
                "elapsed=%llums\n",
                pressed != 0 ? 1 : 0,
                (unsigned long long)
                    ((now_us - loop->trial_health_started_us)
                     / 1000u));
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
&& defined(TILEFINCH_PSP_LIVE_NETWORK)
            if (psp_update_e2e_confirmed(
                    &loop->update_e2e, &healthy, loop->running_slot)) {
                psp_exit_plan_request(
                    &loop->app.interactive->exit,
                    PSP_EXIT_VALIDATION_COMPLETE);
            }
#endif
        } else {
            loop->trial_health_next_attempt_us =
                now_us + UINT64_C(1000000);
        }
    }
}

#ifdef TILEFINCH_PSP_LIVE_NETWORK
static TILEFINCH_OUT_OF_LINE void psp_loop_fire_update_check(
    PspLoop *loop, bool power_interaction)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspEngineViews *const engine_views = loop->app.views;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork *const network = loop->app.network;
    PspNetworkLifecycle *const network_lifecycle = loop->app.network_lifecycle;
#endif
    if (psp_update_check_armed(&loop->update_check)
        && loop->loaded && !loop->initial_error_page
        && !process->presentation.ui.loading && !loop->render_job_pending
        && !browser->media.ui.visible
        && (process->presentation.ui.screen == PSP_UI_SCREEN_PAGE
            || process->presentation.ui.screen == PSP_UI_SCREEN_HOME)
        && !browser_engine_navigation_pending(browser->engine)
        && !navigation_background_resources_pending(engine_views->navigation)
        && !psp_network_lifecycle_warming(network_lifecycle)
        && !power_interaction
        && process->presentation.ui.activity_frames == 0
        && psp_network_lifecycle_ready(network_lifecycle)
        && network->status == PSP_NETWORK_READY) {
        PspUpdateCheckOutcome update_check_outcome =
            PSP_UPDATE_CHECK_NOT_STARTED;
        time_t update_check_now = time(NULL);
        if (update_check_now <= 0) {
            printf("tilefinch-update-check: skipped=no-clock\n");
        } else if (!browser_profile_update_check_due(
                       browser->profile,
                       (uint64_t) update_check_now)) {
            update_check_outcome = PSP_UPDATE_CHECK_RATE_LIMITED;
            printf(
                "tilefinch-update-check: skipped=ratelimited "
                "last=%llu now=%llu\n",
                (unsigned long long)
                    browser_profile_update_check_last_unix(
                        browser->profile),
                (unsigned long long) update_check_now);
        } else if (!process->install_paths.slotted) {
            /* Only an A/B slot install can stage and apply a
               package; the update session refuses any other layout
               (a plain EBOOT or the dev PRX loop). */
            printf("tilefinch-update-check: skipped=unslotted\n");
        } else {
            /* A completed hidden player is only a replay cache. An
               update check has a larger, foreground-visible download
               transaction and must not inherit that decoder's native
               and contiguous-memory footprint. */
            (void) psp_media_reclaim_hidden_pipeline(&browser->media);
            if (!psp_update_session_initialized(&browser->update_session)) {
                PspUpdateSessionOptions options =
                    psp_profile_update_options(process, browser);
                (void) psp_update_session_initialize(
                    &browser->update_session, browser->budget,
                    &process->install_paths, &options,
                    &process->presentation.ui);
            }
            if (psp_update_session_available(&browser->update_session)
                && psp_update_session_begin_check(
                       &browser->update_session,
                       (uint64_t) update_check_now, true)) {
                /* Refresh the snapshot so the shared pump below
                   adopts the new CHECKING phase this frame. */
                psp_update_session_refresh_ui(
                    &browser->update_session, &process->presentation.ui);
                update_check_outcome = PSP_UPDATE_CHECK_STARTED;
                printf(
                    "tilefinch-update-check: started "
                    "now=%llu\n",
                    (unsigned long long) update_check_now);
            } else {
                printf("tilefinch-update-check: skipped=unavailable\n");
            }
        }
        psp_update_check_fired(&loop->update_check, update_check_outcome);
    }
}
#endif

/* Circle on a loading page: stop the candidate and keep the page. */
static TILEFINCH_OUT_OF_LINE void psp_loop_cancel_navigation(
    PspLoop *loop, PspAppFrameState *frame)
{
    PspApp *const app = &loop->app;
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    uint32_t cancel_operation =
        psp_log_operation_begin("navigation-cancel");
    browser_engine_cancel_navigation(
        browser->engine, "cancelled from PSP browser UI");
    psp_ui_set_loading(&process->presentation.ui, false, 0);
    psp_ui_show_status(&process->presentation.ui, "PAGE LOAD STOPPED", 180);
    psp_navigation_cooperate_end("interactive-cancel");
    interactive->navigation_job_started_us = 0;
    loop->blocker_navigation_active = false;
    if (interactive->tab_transition.pending) {
        (void) psp_tabs_finish(
            browser->tabs, browser->engine, &interactive->tab_transition, false,
            browser_profile_tab_hibernation_enabled(browser->profile),
            process->storage.tab_hibernation);
        psp_tabs_sync_ui(&process->presentation.ui, browser->tabs, &process->presentation.tab_view);
    }
    psp_captive_portal_navigation_settled(
        app, frame, false, true);
    psp_log_operation_end(
        cancel_operation, "navigation-cancel", "requested");
}

/* The Update page's intents, the update session's pump, and the end
   of a background update check. True when the Update page changed. */
static TILEFINCH_HOT_BOUNDARY bool psp_loop_update_frame(
    PspLoop *loop, uint64_t now_us, PspUiIntent *intent,
    bool *navigation_visual_changed)
{
    PspApp *const app = &loop->app;
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork *const network = loop->app.network;
    PspNetworkLifecycle *const network_lifecycle = loop->app.network_lifecycle;
#endif
    bool update_visual_changed = false;
    if (process->presentation.ui.screen == PSP_UI_SCREEN_UPDATE
        && !psp_update_session_initialized(&browser->update_session)) {
        PspUpdateSessionOptions options =
            psp_profile_update_options(process, browser);
        (void) psp_update_session_initialize(
            &browser->update_session, browser->budget,
            &process->install_paths, &options,
            &process->presentation.ui);
        update_visual_changed = true;
    }
    if (intent->update_versions_requested
        || intent->update_versions_closed
        || intent->update_version_selected)
        update_visual_changed = psp_handle_update_version_intent(
            process, browser, engine_views, intent
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            , network, network_lifecycle
#endif
        );
    if (intent->update_cancel_requested) {
        bool cancelled =
            psp_update_session_cancel(&browser->update_session);
        if (!cancelled)
            psp_ui_show_status(
                &process->presentation.ui, "UPDATE IS FINISHING SAFELY", 180);
        update_visual_changed = true;
    }
    if (intent->update_primary_requested
        && psp_update_session_available(&browser->update_session)) {
        psp_handle_update_primary(
            process, browser, engine_views, interactive
#ifdef TILEFINCH_PSP_LIVE_NETWORK
            , network, network_lifecycle
#endif
        );
        update_visual_changed = true;
    }
    if (update_visual_changed
        && process->presentation.ui.screen
               != PSP_UI_SCREEN_UPDATE_VERSIONS
        && psp_update_session_initialized(&browser->update_session))
        psp_update_session_refresh_ui(&browser->update_session, &process->presentation.ui);
    if (psp_update_session_active(&browser->update_session)) {
        if (psp_app_frame_pump_run(app, FRAME_PUMP_UPDATE_SESSION))
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
        &loop->update_e2e, process->config.trace, process->config.validation_update_url,
        network_lifecycle, network, &browser->update_session, browser->budget,
        &process->install_paths, &process->presentation.ui,
        &interactive->exit);
#endif
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (psp_update_check_running(&loop->update_check)
        && !psp_update_session_active(&browser->update_session)
        && psp_update_check_finished(&loop->update_check)) {
        *navigation_visual_changed |= psp_finish_update_check(
            process, browser, now_us, &loop->update_check);
    }
#endif
    return update_visual_changed;
}

static TILEFINCH_OUT_OF_LINE void psp_loop_capture_tab_thumbnail(PspLoop *loop)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspEngineViews *const engine_views = loop->app.views;
    /* The thumbnail reads the RAM frame directly. */
    (void) browser_engine_canvas_materialize(browser->engine);
    (void) browser_tabs_capture_active(
        browser->tabs, engine_views->navigation);
    psp_tabs_capture_thumbnail(
        &process->presentation.tab_view, browser_tabs_active_index(browser->tabs),
        engine_views->frame, PSP_SCREEN_WIDTH,
        PSP_SCREEN_HEIGHT, PSP_SCREEN_WIDTH);
    psp_tabs_sync_ui(&process->presentation.ui, browser->tabs, &process->presentation.tab_view);
}

#ifdef TILEFINCH_PSP_LIVE_NETWORK
static TILEFINCH_HOT_BOUNDARY void psp_loop_home_preconnect(
    PspLoop *loop, unsigned ui_elapsed_ms)
{
    PspApp *const app = &loop->app;
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork *const network = loop->app.network;
    PspNetworkLifecycle *const network_lifecycle = loop->app.network_lifecycle;
#endif
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
    if (preconnect_url != NULL
        && !site_adapter_handles_navigation(
               "GET", preconnect_url)) {
        preconnect_url = NULL;
    }
    uint32_t preconnect_key =
        preconnect_url == NULL
            ? 0u : fetch_preconnect_tile_key(preconnect_url);
    FetchPreconnectDwellAction preconnect_action =
        fetch_preconnect_dwell_step(
            loop->home_preconnect_dwell, preconnect_key != 0u,
            preconnect_key, ui_elapsed_ms,
            PSP_HOME_PRECONNECT_DWELL_MS);
    if (preconnect_action == FETCH_PRECONNECT_DWELL_START) {
        (void) fetch_preconnect(preconnect_url, browser->budget);
    } else if (preconnect_action
               == FETCH_PRECONNECT_DWELL_CANCEL) {
        fetch_preconnect_cancel("home-focus-change");
    }
    if (psp_app_frame_pump_run(app, FRAME_PUMP_PRECONNECT))
        fetch_preconnect_pump();
}
#endif

/* The candidate navigation finished (committed, failed or was
   cancelled): settle the chrome, tabs and records, and report it. */
static TILEFINCH_OUT_OF_LINE void psp_loop_finish_navigation(
    PspLoop *loop, PspAppFrameState *frame,
    BrowserNavigationJobStatus navigation_status,
    const BrowserNavigationJobMetrics *navigation_metrics)
{
    PspApp *const app = &loop->app;
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
    bool isolated_portal_navigation =
        browser->session != NULL
        && browser_session_captive_portal_active(
               browser->session);
    ContentBlockerMetrics blocker_metrics = {0};
    size_t page_blocked = 0;
    if (browser_engine_content_blocker_metrics(
            browser->engine, &blocker_metrics)) {
        page_blocked = blocker_metrics.requests_blocked
                >= loop->blocker_navigation_baseline
            ? blocker_metrics.requests_blocked
                  - loop->blocker_navigation_baseline
            : blocker_metrics.requests_blocked;
    }
    loop->blocker_navigation_active = false;
    psp_navigation_cooperate_end("interactive");
    (void) psp_engine_views_refresh(engine_views, browser->engine);
    char reader_entry_buffer[NAVIGATION_URL_LIMIT];
    const char *reader_entry_url = psp_app_current_url(
        browser->engine, reader_entry_buffer, sizeof(reader_entry_buffer));
    psp_reader_navigation_finish(
        browser->engine, &process->presentation.ui,
        browser->profile, &interactive->reader_navigation,
        reader_entry_url,
        navigation_status == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    (void) psp_engine_views_refresh(engine_views, browser->engine);
    psp_ui_set_loading(
        &process->presentation.ui, false,
        navigation_status
                == BROWSER_NAVIGATION_JOB_SUCCEEDED
            ? 1000 : 0);
    if (navigation_status
            == BROWSER_NAVIGATION_JOB_SUCCEEDED) {
        interactive->captive_portal_failure_count = 0;
        interactive->recovery_offer = PSP_RECOVERY_OFFER_NONE;
        NavigationBotWall bot_wall;
        if (navigation_bot_wall(engine_views->navigation, &bot_wall))
            (void) psp_ui_show_site_blocked_status(
                &process->presentation.ui, bot_wall.site, 480);
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
        psp_apply_reader_after_navigation(app, frame);
        psp_run_deferred_activation(app, frame);
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
        frame->page_dirty = true;
        if (!isolated_portal_navigation) {
            psp_profile_record_current(
                browser->profile, &browser->profile_store,
                browser->engine, frame->ui_sample_us);
            if (psp_recovery_record_current(
                browser->profile, process->storage.recovery,
                browser->engine)) {
                (void) browser_engine_current_entry(
                    browser->engine, NULL, 0u,
                    &interactive->recovery.observed_scroll);
                interactive->recovery.observed_generation =
                    browser_engine_page_generation(browser->engine);
                interactive->recovery.last_change_us =
                    (uint64_t) sceKernelGetSystemTimeWide();
                interactive->recovery.dirty = false;
            }
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
        psp_navigation_present_failure(
            process, browser, engine_views, interactive,
            navigation_status);
    }
    psp_captive_portal_navigation_settled(
        app, frame,
        navigation_status
            == BROWSER_NAVIGATION_JOB_SUCCEEDED,
        navigation_status
            == BROWSER_NAVIGATION_JOB_CANCELLED);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_load_experience_report(
        "follow", browser->engine, navigation_metrics);
#endif
    printf("tilefinch-navigation-job: status=%d "
           "http=%ld server=\"%.32s\" mitigated=\"%.16s\" "
           "pumps=%zu body=%zuB yields=%zu "
           "max-pump=%lluus parser-max=%lluus "
           "preview=%zu first=%lluus "
           "scrolls=%zu y=%d bytes=%zu "
           "headers=%lluus first-body=%lluus "
           "first-dom=%lluus source=%zu nodes=%zu "
           "style-refresh=%lluus "
           "style-builds=%zu continuation=%zu/%zu/%lluus "
           "viewport-rebuilds=%zu "
           "rules=%zu+%zu discovery=%lluus "
           "context=%lluus append=%lluus "
           "refresh-failures=%zu "
           "parser=%lluus runtime=%lluus css=%lluus observers=%lluus "
           "script=%lluus/%lluus "
           "finalize=%lluus transform=%lluus/%zu/%zu "
           "irreducible=%lluus/%zu "
           "elapsed=%lluus system-free=%d "
           "system-largest=%d\n",
           (int) navigation_status,
           engine_views->navigation->last_http_status,
           engine_views->navigation->last_server,
           engine_views->navigation->last_cf_mitigated,
           navigation_metrics->pump_calls,
           navigation_metrics->load.body_bytes,
           navigation_metrics->load.quota_yields,
           (unsigned long long)
               navigation_metrics->load.maximum_pump_us,
           (unsigned long long)
               navigation_metrics->load
                   .maximum_parser_pump_us,
           navigation_metrics->provisional_paints,
           (unsigned long long)
               navigation_metrics
                   ->provisional_first_present_us,
           navigation_metrics->provisional_scrolls,
           navigation_metrics->provisional_scroll_y,
           navigation_metrics->provisional_bytes,
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
               .blocking_stylesheet_viewport_rebuilds,
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
                   .parser_mutation_checkpoint_us,
           (unsigned long long)
               engine_views->navigation->performance
                   .parser_script_compile_us,
           (unsigned long long)
               engine_views->navigation->performance
                   .parser_script_execute_us,
           (unsigned long long)
               navigation_metrics->load.finalize_us,
           (unsigned long long)
               navigation_metrics
                   ->maximum_transform_slice_us,
           navigation_metrics->transform_slices,
           navigation_metrics->transform_quota_overruns,
           (unsigned long long)
               navigation_metrics
                   ->maximum_irreducible_unit_us,
           navigation_metrics->irreducible_unit_overruns,
           (unsigned long long)
               navigation_metrics->elapsed_us,
               sceKernelTotalFreeMemSize(),
               sceKernelMaxFreeMemSize());
    psp_log_parser_checkpoint_refusals(
        &engine_views->navigation->performance);
    if (navigation_metrics->adapter_commit_us != 0) {
        printf(
            "tilefinch-adapter-commit: total=%lluus "
            "parse=%lluus style=%lluus resource=%lluus "
            "layout=%lluus runtime=%lluus\n",
            (unsigned long long)
                navigation_metrics->adapter_commit_us,
            (unsigned long long)
                navigation_metrics->adapter_parse_us,
            (unsigned long long)
                navigation_metrics->adapter_style_us,
            (unsigned long long)
                navigation_metrics->adapter_resource_us,
            (unsigned long long)
                navigation_metrics->adapter_layout_us,
            (unsigned long long)
                navigation_metrics->adapter_runtime_us);
    }
    if (navigation_metrics->adapter_transport_samples != 0
        || navigation_metrics->adapter_document_cache_hits != 0
        || navigation_metrics->adapter_document_cache_stores != 0) {
        printf(
            "tilefinch-adapter-transport: samples=%zu "
            "reused=%zu cache-hit=%zu cache-store=%zu "
            "wall=%lluus transport=%lluus "
            "dns=%lluus tcp=%lluus tls=%lluus server=%lluus "
            "body=%lluus overhead=%lluus\n",
            navigation_metrics->adapter_transport_samples,
            navigation_metrics->adapter_reused_connections,
            navigation_metrics->adapter_document_cache_hits,
            navigation_metrics->adapter_document_cache_stores,
            (unsigned long long)
                navigation_metrics->adapter_request_wall_us,
            (unsigned long long)
                navigation_metrics->adapter_transport_total_us,
            (unsigned long long)
                navigation_metrics->adapter_dns_us,
            (unsigned long long)
                navigation_metrics->adapter_tcp_us,
            (unsigned long long)
                navigation_metrics->adapter_tls_us,
            (unsigned long long)
                navigation_metrics->adapter_server_us,
            (unsigned long long)
                navigation_metrics->adapter_body_transfer_us,
            (unsigned long long)
                navigation_metrics->adapter_admission_collect_us);
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_report_blocking_script_samples(
        &engine_views->navigation->performance);
    psp_report_background_transport_metrics();
#endif
    if (navigation_status
            != BROWSER_NAVIGATION_JOB_SUCCEEDED
        && navigation_status
            != BROWSER_NAVIGATION_JOB_CANCELLED) {
        psp_log_checkpoint(
            "interactive-navigation-failed");
    }
    interactive->navigation_job_started_us = 0;
}

/* One admitted slice of the shared file/video download lane. */
static TILEFINCH_OUT_OF_LINE void psp_loop_pump_offline_download(
    PspLoop *loop, uint32_t offline_download_id)
{
    PspApp *const app = &loop->app;
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
    if (!psp_app_frame_pump_run(app, FRAME_PUMP_OFFLINE_DOWNLOAD)) return;
    psp_ui_set_loading(&process->presentation.ui, true, -1);
    psp_ui_keep_status(&process->presentation.ui, "SAVING DOWNLOAD  O PAUSE", 120);
    psp_work_cooperate_begin(
        &process->presentation.ui, engine_views->frame, true, true, false,
        "PAUSING DOWNLOAD...", "offline-download", NULL, NULL);
    bool completed = psp_offline_store_pump(&browser->offline_store);
    bool cancelled = psp_navigation_cancel_requested();
    psp_navigation_cooperate_end_adopting_buttons(
        "offline-download", &interactive->previous_buttons);
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
    if (process->presentation.ui.screen
            == PSP_UI_SCREEN_COLLECTIONS
        && process->presentation.ui.collections_section
            == PSP_UI_COLLECTION_DOWNLOADS) {
        psp_collections_sync_ui(
            &process->presentation.ui,
            &process->presentation.collections_surface,
            browser->profile,
            &browser->offline_store,
            PSP_UI_COLLECTION_DOWNLOADS);
    }
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
            &process->presentation.ui, cancelled ? "DOWNLOAD PAUSED"
                           : psp_offline_store_status(
                                 &browser->offline_store),
            240);
}

/* The recovery record follows the page and its scroll position, saved
   once they have been still for two seconds and at most every 30. Each
   frame reads only the generation and scroll; the URL is copied and
   classified (psp_loop_recovery_moved) only when one of them moved or a
   save is due. */
static TILEFINCH_HOT_BOUNDARY void psp_loop_track_recovery(PspLoop *loop)
{
    PspRecoveryTracker *const recovery = &loop->app.interactive->recovery;
    int entry_scroll = 0;
    if (!browser_engine_current_entry(
            loop->app.browser->engine, NULL, 0u, &entry_scroll)) return;
    uint64_t generation =
        browser_engine_page_generation(loop->app.browser->engine);
    uint64_t recovery_now_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    bool changed = generation != recovery->observed_generation
        || entry_scroll != recovery->observed_scroll;
    if (!changed && !psp_recovery_save_due(recovery, recovery_now_us))
        return;
    psp_loop_recovery_moved(loop, generation, entry_scroll, changed,
                            recovery_now_us);
}

/* The page moved, or a save is due: copy and classify the URL. An
   internal page is never recorded, so its generation and scroll are
   simply observed, and any dirty state dropped: the next external page
   is a new load (the page generation only grows), which marks the record
   dirty again with a fresh timer, as before. */
static TILEFINCH_OUT_OF_LINE void psp_loop_recovery_moved(
    PspLoop *loop, uint64_t generation, int entry_scroll, bool changed,
    uint64_t recovery_now_us)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspRecoveryTracker *const recovery = &loop->app.interactive->recovery;
    char entry_url[NAVIGATION_URL_LIMIT];
    if (!browser_engine_current_entry(
            browser->engine, entry_url, sizeof(entry_url), NULL)) return;
    if (psp_ui_internal_url(entry_url)) {
        recovery->observed_generation = generation;
        recovery->observed_scroll = entry_scroll;
        recovery->dirty = false;
        return;
    }
    if (changed) {
        recovery->dirty = true;
        recovery->observed_generation = generation;
        recovery->observed_scroll = entry_scroll;
        recovery->last_change_us = recovery_now_us;
    }
    if (psp_recovery_save_due(recovery, recovery_now_us)
        && psp_recovery_record_current(
               browser->profile, process->storage.recovery, browser->engine)) {
        recovery->last_change_us = recovery_now_us;
        recovery->last_save_us = recovery_now_us;
        recovery->dirty = false;
    }
}

#ifdef TILEFINCH_PSP_LIVE_NETWORK
static TILEFINCH_OUT_OF_LINE void psp_loop_report_network_warmup(
    PspLoop *loop, bool network_was_warming,
    PspNetworkStatus previous_network_status)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    PspNetwork *const network = loop->app.network;
    PspNetworkLifecycle *const network_lifecycle = loop->app.network_lifecycle;
#endif
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
                   speculative dwell. Start its connection warmup now
                   instead of charging another post-association delay;
                   the shared worker keeps DNS/TCP/TLS off this loop. */
                const char *url = psp_home_target_url(
                    &process->presentation.home_surface,
                    process->presentation.ui.home_selection,
                    browser->profile);
                if (url != NULL
                    && !site_adapter_handles_navigation("GET", url)) {
                    url = NULL;
                }
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
                        loop->home_preconnect_dwell, key != 0u, key,
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
}
#endif

/* The media session's share of every frame, player open or not: route
   reconciliation, the open and decoder-submit cooperate scopes, the
   session advance (its surface housekeeping runs even when hidden),
   and the page <video>'s reported state. */
static TILEFINCH_HOT_BOUNDARY void psp_loop_media_frame(
    PspLoop *loop, PspAppFrameState *frame, unsigned media_elapsed_ms,
    bool *media_visual_changed)
{
    PspApp *const app = &loop->app;
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
    char current_url[NAVIGATION_URL_LIMIT];
    bool has_current_entry = browser_engine_current_entry(
        browser->engine, current_url, sizeof(current_url), NULL);
    /* Watch documents no longer autoplay: the thumbnail is the sole
       authored native-player target. The validation-only autoplay knob
       must therefore perform that activation explicitly instead of
       waiting for document route reconciliation to open media. Shipping
       configuration strips this knob before the interactive loop. */
    if (process->config.validation_media_play != 0
        && has_current_entry
        && browser->media.source[0] == '\0') {
        (void) psp_media_open_provider_route(
            &browser->media, current_url,
            browser_engine_page_generation(browser->engine));
    }
    bool media_open_was_pending =
        psp_media_open_work_pending(&browser->media);
    psp_media_prepare_route(
        &browser->media, has_current_entry ? current_url : NULL,
        browser_engine_page_generation(browser->engine));
    if (!media_open_was_pending
        && psp_media_open_work_pending(&browser->media)) {
        /* A selected video replaces the page as the visible surface.
           Retire optional page-owned requests before its resolver asks
           for a bounded worker descriptor; the committed DOM and pixels
           remain available if media open fails. */
        (void) browser_engine_cancel_network_work(
            browser->engine, "video selected");
    }
    psp_log_set_phase(PSP_LOG_PHASE_MEDIA);
    psp_log_heartbeat();
    bool media_open_before = psp_media_open_work_pending(&browser->media);
    bool navigation_still_pending =
        browser_engine_navigation_pending(browser->engine);
    if (media_open_before && !navigation_still_pending
        && !interactive->provider_handoff_present_pending
        && !browser_engine_optional_memory_reclaim_pending(
               &interactive->provider_handoff_reclaim)
        && !psp_navigation_cooperate_active()) {
        psp_work_cooperate_begin_media_open(
            &process->presentation.ui, engine_views->frame, &browser->media.ui);
        loop->media_open_scope = true;
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
    psp_app_pump_provider_handoff_reclaim(
        app, frame->ui_sample_us, false);
    if ((!media_open_before || !navigation_still_pending)
        && !interactive->provider_handoff_present_pending
        && !browser_engine_optional_memory_reclaim_pending(
               &interactive->provider_handoff_reclaim)) {
        *media_visual_changed =
            psp_media_advance(
                &browser->media, media_elapsed_ms,
                psp_navigation_cooperate_active()
                    ? psp_navigation_cancellation() : NULL)
            || *media_visual_changed;
    } else if (psp_media_open_watchdog(&browser->media)) {
        /* The suppressed frames are still frames the open's deadline
           and the cancel law run on. Without this an open that nothing
           was pumping had neither. */
        *media_visual_changed = true;
    }
    if (browser->media.page_source
        && browser->media.page_media_node_handle > 0) {
        uint64_t now_us = frame->ui_sample_us;
        if (browser->media.page_media_report_us == 0
            || now_us - browser->media.page_media_report_us
                   >= UINT64_C(250000)) {
            ScriptMediaState state = browser->media.ui.failed
                ? SCRIPT_MEDIA_STATE_ERROR
                : browser->media.ui.ended
                    ? SCRIPT_MEDIA_STATE_ENDED
                    : browser->media.playback == NULL
                        ? SCRIPT_MEDIA_STATE_LOADING
                        : browser->media.ui.playing
                            ? SCRIPT_MEDIA_STATE_PLAYING
                            : SCRIPT_MEDIA_STATE_PAUSED;
            (void) browser_engine_update_media_state(
                browser->engine, browser->media.page_media_node_handle,
                state, (double) browser->media.clock_us / 1000000.0,
                (double) psp_media_duration_us(&browser->media)
                    / 1000000.0);
            browser->media.page_media_report_us = now_us;
        }
    }
    if (media_open_before && psp_navigation_cooperate_active())
        psp_app_refresh_supervisor_media(interactive, &browser->media.ui);
    /* media_stability_active is only ever set from the stability knob or
       the validation build's MEDIA TEST action. */
    if (PSP_VALIDATION_KNOB(process->config.validation_media_play != 0
                            || interactive->media_stability_active))
        psp_loop_media_validation(loop, media_visual_changed);
    if (media_decode_scope) {
        bool cancelled = psp_navigation_cancel_requested();
        uint32_t observed_buttons =
            psp_ui_buttons(psp_navigation_observed_buttons());
        if (cancelled) psp_media_close(&browser->media);
        if (cancelled) *media_visual_changed = true;
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
    if (loop->media_open_scope) {
        bool scope_owned = psp_navigation_cooperate_active();
        bool cancelled =
            scope_owned && psp_navigation_cancel_requested();
        if (!scope_owned || cancelled || navigation_still_pending
            || !psp_media_open_work_pending(&browser->media)) {
            uint32_t observed_buttons =
                psp_ui_buttons(psp_navigation_observed_buttons());
            if (cancelled) {
                psp_media_close(&browser->media);
                *media_visual_changed = true;
            }
            if (scope_owned) {
                psp_navigation_cooperate_end("media-open");
                interactive->previous_buttons = observed_buttons;
            }
            loop->media_open_scope = false;
            /* The indicator belongs to whoever still owns the
               surface: a navigation that took it over re-asserts
               its own progress every frame. */
            if (!navigation_still_pending)
                psp_ui_set_loading(&process->presentation.ui, false, 0);
            /* The open's "OPENING VIDEO  O CANCEL" toast is done too. Left
               up, it froze while the player owned input (its frames count
               only in psp_ui_update) and then showed on the page for about
               ten seconds after the player closed. */
            psp_ui_dismiss_status(
                &process->presentation.ui, PSP_MEDIA_OPEN_STATUS);
            if (cancelled) {
                psp_ui_show_status(
                    &process->presentation.ui, "VIDEO OPEN STOPPED", 180);
            }
        }
    }
}

static TILEFINCH_OUT_OF_LINE void psp_loop_apply_media_request(
    PspLoop *loop, const ScriptMediaRequest *dom_media_request)
{
    PspBrowserResources *const browser = loop->app.browser;
    char dom_url[NAVIGATION_URL_LIMIT];
    bool has_dom_entry = browser_engine_current_entry(
        browser->engine, dom_url, sizeof(dom_url), NULL);
    uint64_t dom_generation = browser_engine_page_generation(browser->engine);
    bool same_page_media = browser->media.page_source
        && browser->media.page_audio
               == dom_media_request->audio_only
        && browser->media.page_media_node_handle
               == dom_media_request->node_handle
        && strcmp(browser->media.source,
                  dom_media_request->source) == 0;
    if (!same_page_media
        && (dom_media_request->command == SCRIPT_MEDIA_COMMAND_PLAY
            || dom_media_request->command
                   == SCRIPT_MEDIA_COMMAND_LOAD)) {
        MediaDiscoveryKind kind = media_discovery_reference_kind(
            dom_media_request->source,
            strlen(dom_media_request->source));
        same_page_media = has_dom_entry
            && (dom_media_request->audio_only
                ? kind != MEDIA_DISCOVERY_HLS
                  && psp_media_open_page_audio(
                      &browser->media, dom_media_request->source,
                      dom_url,
                      dom_generation,
                      dom_media_request->node_handle,
                      dom_media_request->mode,
                      dom_media_request->credentials,
                      dom_media_request->command
                          == SCRIPT_MEDIA_COMMAND_PLAY,
                      dom_media_request->command
                          == SCRIPT_MEDIA_COMMAND_LOAD)
                : kind == MEDIA_DISCOVERY_HLS
                ? psp_media_open_page_hls(
                    &browser->media, dom_media_request->source,
                    dom_url,
                    dom_generation,
                    dom_media_request->node_handle,
                    dom_media_request->mode,
                    dom_media_request->credentials,
                    dom_media_request->command
                        == SCRIPT_MEDIA_COMMAND_PLAY,
                    dom_media_request->command
                        == SCRIPT_MEDIA_COMMAND_LOAD)
                : psp_media_open_page_source(
                    &browser->media, dom_media_request->source,
                    dom_url,
                    dom_generation,
                    dom_media_request->node_handle,
                    dom_media_request->mode,
                    dom_media_request->credentials,
                    dom_media_request->command
                        == SCRIPT_MEDIA_COMMAND_PLAY,
                    dom_media_request->command
                        == SCRIPT_MEDIA_COMMAND_LOAD));
    }
    if (same_page_media) {
        if (dom_media_request->command == SCRIPT_MEDIA_COMMAND_SEEK) {
            double seconds = dom_media_request->value;
            if (seconds >= 0.0
                && seconds <= (double) UINT64_MAX / 1000000.0) {
                (void) psp_media_request_seek(
                    &browser->media,
                    (uint64_t) (seconds * 1000000.0), false);
            }
        } else if ((dom_media_request->command
                        == SCRIPT_MEDIA_COMMAND_PLAY
                    && !browser->media.ui.playing)
                   || (dom_media_request->command
                           == SCRIPT_MEDIA_COMMAND_PAUSE
                       && browser->media.ui.playing)) {
            psp_media_execute_intent(
                &browser->media, (PspUiMediaIntent) {
                    .action = PSP_UI_MEDIA_ACTION_PLAY_PAUSE
                });
        }
    } else {
        (void) browser_engine_update_media_state(
            browser->engine, dom_media_request->node_handle,
            SCRIPT_MEDIA_STATE_ERROR, 0.0, 0.0);
    }
}

/* This frame's input to the visible player. True when the player's
   chrome changed. */
static TILEFINCH_HOT_BOUNDARY bool psp_loop_media_input(
    PspLoop *loop, PspUiInput *input)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
    bool media_visual_changed = false;
    psp_cursor_latency_idle();
    media_visual_changed =
        psp_app_dispatch_deferred_media_intent(
            &browser->media, interactive)
        || media_visual_changed;
    uint32_t menu_before = psp_ui_media_menu_feedback_key(&browser->media.ui);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool menu_was_open = browser->media.ui.presentation != NULL
        && browser->media.ui.presentation->track_menu_open;
    unsigned menu_old_tab = browser->media.ui.presentation == NULL
        ? 0u : browser->media.ui.presentation->track_menu_tab;
    uint32_t menu_pressed_us = input->pressed == 0u ? 0u
        : (uint32_t) psp_input_script_request_time(sceKernelGetSystemTimeWide());
#endif
    PspUiMediaIntent media_intent =
        psp_ui_media_update(&browser->media.ui, input);
    /* A paused chooser already has its final UI state. Publish it before
       unrelated work. During playback, combine it with the next video frame:
       a separate old-picture swap consumes a vblank and hitches playback. The
       coverage key excludes closing/shrinking, rotation, seek and loading;
       those need fresh pixels or their existing semantic action handling. */
    bool menu_feedback = media_intent.visual_changed
        && !browser->media.ui.playing
        && media_intent.action == PSP_UI_MEDIA_ACTION_NONE
        && psp_ui_media_menu_feedback_reuse(menu_before,
            psp_ui_media_menu_feedback_key(&browser->media.ui));
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_media_menu_feedback_input(&browser->media.ui,
        menu_was_open, menu_old_tab, menu_pressed_us);
#endif
    uint32_t media_operation = 0;
    const char *media_action =
        psp_media_action_name(media_intent.action);
    if (media_intent.action != PSP_UI_MEDIA_ACTION_NONE) {
        psp_log_set_phase(PSP_LOG_PHASE_MEDIA);
        media_operation =
            psp_log_operation_begin(media_action);
    }
    bool menu_presented = false;
    if ((menu_feedback || psp_ui_media_intent_has_predispatch_visual(&media_intent))
        && !psp_navigation_cooperate_supervised()) {
        /* Publish UI state which already changed before a close,
           seek, or retry can enter a slower backend operation.
           Play/Pause is deliberately excluded: its visible state is
           committed by execute_intent below, so this would publish a
           stale legend and then its replacement one vblank later. */
        menu_presented = psp_present(engine_views->frame, &process->presentation.ui);
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
    /* A refused publication must retry. Successful chooser feedback needs
       no duplicate UI-only presentation; newly decoded frames still publish
       through the ordinary media advance below. */
    media_visual_changed |= media_intent.visual_changed
        && !(menu_feedback && menu_presented);
    return media_visual_changed;
}

/* One bounded pump of the pending navigation under the input
   supervisor, with its watchdog; a finished one is settled here. */
static TILEFINCH_HOT_BOUNDARY void psp_loop_pump_navigation(
    PspLoop *loop, PspAppFrameState *frame)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
    bool site_data_waiting =
        psp_site_data_restore_gate_navigation(
            &interactive->site_data_restore, browser->session,
            &process->storage);
    /* A closed player retains its native decoder only for a fast
       replay on the same watch page. Release it before the first
       pump of any other candidate so the candidate receives the
       full contiguous-memory budget instead of failing before the
       old route is replaced at commit. */
    (void) psp_media_reclaim_hidden_pipeline_for_navigation(
        &browser->media, browser_engine_pending_navigation_url(browser->engine));
    if (!loop->blocker_navigation_active) {
        ContentBlockerMetrics blocker_metrics = {0};
        loop->blocker_navigation_baseline =
            browser_engine_content_blocker_metrics(
                browser->engine, &blocker_metrics)
            ? blocker_metrics.requests_blocked : 0;
        loop->blocker_navigation_active = true;
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
        site_data_waiting
            ? BROWSER_NAVIGATION_JOB_PENDING
            : browser_engine_pump_navigation(
                  browser->engine, &navigation_quota);
    psp_show_page_script_clock(&process->presentation.ui, browser->engine);
    /* Same cooperation point the boot loop marks: this loop
       polls the pad every pass, so the gap between two pumps is
       not a blind window and must not be charged as one. */
    if (!site_data_waiting) loop->navigation_pump_boundaries++;
    if (!site_data_waiting && !tilefinch_platform_cooperate(
            "navigation-pump", loop->navigation_pump_boundaries)
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
    uint64_t now_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    if (site_data_waiting) {
        /* Deferred localStorage is part of boot recovery, not network
           progress. Start the one-minute navigation watchdog only once
           the candidate is actually allowed to pump. */
        interactive->navigation_job_started_us = now_us;
    }
    if (navigation_status == BROWSER_NAVIGATION_JOB_PENDING
        && interactive->navigation_job_started_us != 0
        && interactive->navigation_watchdog.started_us
               != interactive->navigation_job_started_us) {
        psp_navigation_watchdog_start(
            &interactive->navigation_watchdog,
            interactive->navigation_job_started_us,
            navigation_metrics.load.progress_units);
    }
    if (navigation_status == BROWSER_NAVIGATION_JOB_PENDING
        && interactive->navigation_job_started_us != 0
        && psp_navigation_watchdog_expired(
               &interactive->navigation_watchdog, now_us,
               navigation_metrics.load.progress_units)) {
        browser_engine_cancel_navigation(
            browser->engine, "PSP navigation watchdog expired");
        navigation_status =
            BROWSER_NAVIGATION_JOB_CANCELLED;
    }
    if (navigation_status
            != BROWSER_NAVIGATION_JOB_PENDING)
        psp_loop_finish_navigation(
            loop, frame, navigation_status, &navigation_metrics);
    psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);
}

/* One raster slice of the pending page render. Returns what it left to
   publish: 0 nothing, 1 a page frame, 2 a canvas frame (whose next
   input is sampled without another vblank wait). */
static TILEFINCH_HOT_BOUNDARY unsigned psp_loop_render_job(
    PspLoop *loop, PspAppFrameState *frame)
{
    PspProcessResources *const process = loop->app.process;
    PspBrowserResources *const browser = loop->app.browser;
    PspInteractiveState *const interactive = loop->app.interactive;
    PspEngineViews *const engine_views = loop->app.views;
    unsigned render_visual_state = 0;
    psp_log_set_phase(PSP_LOG_PHASE_RENDER);
    psp_log_heartbeat();
    bool canvas_render =
        browser_engine_canvas_frame_pending(browser->engine);
    bool render_scope = false;
    bool owner_render_scope = false;
    if (!psp_navigation_cooperate_active()) {
        if (!canvas_render && engine_views->frame != NULL
            && !process->presentation.ui.page_gamepad_capture) {
            psp_runtime_cooperate_begin(&process->presentation.ui,
                engine_views->frame, &interactive->toolbar_input);
            owner_render_scope = true;
        } else {
            psp_work_cooperate_begin(
                &process->presentation.ui, engine_views->frame, false, true, false,
                "STOPPING PAGE UPDATE...",
                "render-raster-unit", NULL, NULL);
        }
        render_scope = true;
    }
    const TileCache *render_stats =
        browser_engine_render_metrics_view(browser->engine);
    PspVprobeRender render_probe;
    psp_vprobe_render_begin(&render_probe, canvas_render, render_stats);
    size_t units_before = render_stats == NULL ? 0
        : render_stats->frame_job_units;
    BrowserRenderJobStatus render_status = psp_render_page_job(
        browser->engine,
        render_scope ? psp_navigation_cancellation() : NULL);
    uint64_t render_now_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    psp_vprobe_render_end(
        &render_probe, canvas_render, render_stats, render_now_us);
    if (render_stats != NULL
        && render_stats->frame_job_units > units_before) {
        loop->render_job_last_progress_us = render_now_us;
    }
    bool render_cancelled =
        render_status == BROWSER_RENDER_JOB_CANCELLED;
    if (render_scope) {
        if (owner_render_scope) {
            uint32_t observed_buttons = 0;
            if (psp_runtime_cooperate_end(&observed_buttons))
                interactive->previous_buttons = psp_ui_buttons(observed_buttons);
        } else {
            psp_navigation_cooperate_end_adopting_buttons(
                "render-raster-unit", &interactive->previous_buttons);
        }
    }
    if (render_status == BROWSER_RENDER_JOB_FAILED) {
        psp_report_job_failure(
            "render", "interactive-render-failed",
            (int) render_status, 0,
            browser_engine_last_error(browser->engine));
        psp_ui_show_status(
            &process->presentation.ui, browser_engine_last_error(browser->engine), 300);
        loop->render_job_pending = false;
        loop->render_job_last_progress_us = 0;
        render_visual_state = 1;
    } else if (render_cancelled) {
        loop->render_job_pending = false;
        loop->render_job_last_progress_us = 0;
        /*
         * A cancellation may have arrived while the final
         * compositor was irreducibly in flight. Keep the last
         * completed scanout authoritative instead of publishing
         * that unrequested candidate frame.
         */
        frame->page_dirty = false;
        render_visual_state = 0;
        psp_ui_show_status(
            &process->presentation.ui, "PAGE UPDATE STOPPED", 180);
        psp_present_supervisor_ui(engine_views->frame, &process->presentation.ui);
    } else if (render_status == BROWSER_RENDER_JOB_COMPLETE) {
        loop->render_job_pending = false;
        loop->render_job_last_progress_us = 0;
        loop->render_placeholder_active = false;
        render_visual_state = canvas_render ? 2u : 1u;
        (void) psp_engine_views_refresh(engine_views, browser->engine);
    } else {
        /* Scrolling never waits for raster: publish the new
           position at once from resident tiles with a checkerboard
           for the rest, then refill as slices land. Only a scroll
           earns placeholders; a job pending for in-place damage
           (an image arriving) keeps showing the old pixels. */
        bool scrolled = render_stats != NULL
            && render_stats->frame_work.pending
            && (!render_stats->presented_scroll_valid
                || render_stats->frame_work.scroll_y
                       != render_stats->presented_scroll_y);
        bool progressed = loop->render_placeholder_active
            && render_stats != NULL
            && render_stats->frame_job_units > units_before;
        size_t placeholders = 0;
        if (!canvas_render
            && frame_paint_progress_due(render_now_us,
                    loop->render_progress_present_us, scrolled, progressed)
            && browser_engine_render_placeholder_frame(
                   browser->engine, &placeholders)) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            printf("tilefinch-placeholder-present: placeholders=%zu "
                   "scrolled=%d\n", placeholders, scrolled ? 1 : 0);
#endif
            loop->render_placeholder_active = placeholders != 0;
            render_visual_state = 1;
            (void) psp_engine_views_refresh(
                engine_views, browser->engine);
        } else if (!canvas_render && progressed) {
            /* Keep the 2 ms slice and sample input again, rather than
               paying a vblank for every completed tile. Scanout ownership
               and fences are unchanged when a frame is published. */
            loop->fast_page_followup = true;
        }
        if (loop->render_job_last_progress_us != 0
            && render_now_us - loop->render_job_last_progress_us
                   >= PSP_RENDER_JOB_TIMEOUT_US) {
            browser_engine_cancel_render_job(browser->engine);
            loop->render_job_pending = false;
            loop->render_job_last_progress_us = 0;
            render_visual_state = 1;
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
    return render_visual_state;
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
    psp_report_frame_pumps(interactive);
    printf("tilefinch-validation: outcome=exit-requested "
           "free-mem=%d max-free=%d\n",
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    psp_log_checkpoint("cleanup-begin");

#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /* Release any parked speculative connection (and its transport
       reference) so the shared transport tears down cleanly at exit. */
    fetch_preconnect_cancel("shutdown");
#endif

    /* A portal tab and its temporary cookie/storage tables are never part of
       session persistence. Restore the ordinary context before any save. */
    PspApp portal_cleanup_app = {
        .process = process,
        .browser = browser,
        .views = engine_views,
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        .network = network,
        .network_lifecycle = network_lifecycle,
#endif
        .interactive = interactive
    };
    psp_captive_portal_destroy(&portal_cleanup_app);

    screenshot_png_cancel(&interactive->screenshot.writer);
    budget_free(browser->budget, interactive->screenshot.pixels);
    interactive->screenshot.pixels = NULL;
    psp_ui_set_diagnostic_qr(&process->presentation.ui, NULL);
    tilefinch_diagnostic_qr_destroy(process->diagnostic_qr);
    process->diagnostic_qr = NULL;

    uint32_t cleanup_operation =
        psp_log_operation_begin("safe-memory-report");
    psp_report_budget_counters(browser->budget, "controlled-exit");
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Whether a managed challenge frame received its larger script heap
       this session, and what the top-level response last said. */
    BrowserValidationCounters challenge_counters;
    if (engine_views != NULL && engine_views->navigation != NULL
        && browser_engine_validation_counters(
               browser->engine, &challenge_counters)) {
        printf("tilefinch-challenge: cf-mitigated=%s heap-raises=%zu\n",
               challenge_counters.last_cf_mitigated[0] == '\0'
                   ? "none" : challenge_counters.last_cf_mitigated,
               challenge_counters.managed_challenge_heap_raises);
    }
#endif
    psp_log_operation_end(
        cleanup_operation, "safe-memory-report", "ok");
    psp_log_set_phase(PSP_LOG_PHASE_CLEANUP);

    cleanup_operation =
        psp_log_operation_begin("media-cleanup");
    psp_youtube_preresolve_reset(
        &browser->youtube_preresolve, "browser shutdown");
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
    /* An immediate exit from HOME must not replace an unread snapshot with
       an empty live cache. Finish any remaining sequential restore now;
       cleanup is already non-interactive, while each parser call remains
       bounded and keeps the watchdog heartbeat alive. */
    size_t restore_cleanup_pumps = 0;
    while (interactive->site_data_restore.phase
               != PSP_SITE_DATA_RESTORE_DISABLED
           && interactive->site_data_restore.phase
               != PSP_SITE_DATA_RESTORE_DONE
           && restore_cleanup_pumps++ < 192u) {
        psp_log_heartbeat();
        (void) psp_site_data_restore_pump(
            &interactive->site_data_restore, browser->session,
            &process->storage, 64u * KIB);
    }
    bool preserve_persisted_cache =
        interactive->site_data_restore.cache_restore_cancelled_unread
        || (interactive->site_data_restore.cache_enabled
            && interactive->site_data_restore.phase
                   != PSP_SITE_DATA_RESTORE_DISABLED
            && interactive->site_data_restore.phase
                   != PSP_SITE_DATA_RESTORE_DONE);
    psp_site_data_restore_destroy(&interactive->site_data_restore);
    bool site_data_saved = psp_save_site_data_on_exit(
        browser->session, browser->profile,
        process->persistent_site_data_available,
        preserve_persisted_cache,
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
        browser->profile, process->storage.recovery, browser->engine);
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

    psp_report_session_measurements(browser, engine_views);
    psp_report_presentation_cadence("controlled-exit");
    cleanup_operation =
        psp_log_operation_begin("engine-cleanup");
    psp_update_session_destroy(&browser->update_session);
    psp_ui_theme_catalog_bind(NULL);
    psp_ui_language_destroy(browser);
    psp_ui_theme_catalog_destroy(browser->theme_catalog);
    browser->theme_catalog = NULL;
    psp_voice_component_session_destroy(
        browser->voice_component_session);
    psp_glyph_component_session_destroy(
        browser->glyph_component_session);
    (void) remove(process->storage.tab_hibernation);
    browser_tabs_destroy(browser->tabs);
    browser->tabs = NULL;
    psp_presentation_unbind_chrome_fonts(&process->presentation);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_focus_feedback_begin(NULL, 0, 0);
#endif
    browser_engine_destroy(browser->engine);
    browser->engine = NULL;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    psp_multiplayer_shutdown(&portal_cleanup_app);
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
    /* The worker held the last libcurl reference, so its shutdown is what
       exported the shared session cache into the store. The engine-cleanup
       flush above ran before that export and saved nothing new: TLS
       sessions were never persisted across launches. Save them now. */
    if (background_transport_stopped && !fetch_tls_session_store_flush())
        printf("tilefinch-tls-session: exit save failed\n");
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
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* A capture publishes its trace.meta only when it ends. */
    fetch_trace_end();
#endif
#ifdef TILEFINCH_PSP_PGO_GENERATE
    /* Engine PGO training build: a PSP program never runs exit(), so write
       libgcov's counters (pgo/ beside the EBOOT) here, before the record
       the PPSSPP runner stops the emulator on. */
    {
        extern void __gcov_dump(void);
        /* This target's libgcov cannot create directories (no
           TARGET_POSIX_IO) and reports failures only on stderr. */
        int made = mkdir("pgo", 0777);
        FILE *gcov_errors = freopen("pgo/libgcov-stderr.txt", "w", stderr);
        __gcov_dump();
        if (gcov_errors != NULL) (void) fflush(stderr);
        printf("tilefinch-pgo: counters written to pgo/ mkdir=%d\n", made);
    }
#endif
    printf("tilefinch-validation: outcome=clean-exit "
           "free-mem=%d max-free=%d\n",
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    psp_log_checkpoint("cleanup-complete");
    psp_log_finish("clean-exit");
    return shutdown_report;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
#include "psp_boot_timing.h"
#include "psp_boot_module.h"
#endif

static TILEFINCH_COLD_PATH void psp_report_startup_failure(
    const char *screen, const char *detail, const char *url)
{
    (void) psp_write_failure_report_data(
        "startup", detail, url, 0, 0, NULL, true);
    psp_present_boot_surface(PSP_UI_STARTUP_SPLASH, screen, 0);
    printf("tilefinch-startup-failure: screen=%s detail=%s\n",
           screen, detail);
    printf("tilefinch-psp-script: halted\n");
    printf("tilefinch-validation: outcome=halted "
           "free-mem=%d max-free=%d\n",
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Validation only: record this live run's HTTP exchanges beside the EBOOT
   (boot.cfg trace_capture). Kept out of main, whose size is ratcheted. */
static TILEFINCH_OUT_OF_LINE void psp_begin_trace_capture(
    const char *argv0, const char *name)
{
    char capture_path[768];
    psp_sibling_path(capture_path, sizeof(capture_path), argv0, name);
    char capture_error[256] = {0};
    bool capturing = fetch_trace_capture_begin(
        capture_path, capture_error, sizeof(capture_error));
    printf("tilefinch-psp-script: trace capture %s %s\n",
           capturing ? "recording to" : "failed:",
           capturing ? name : capture_error);
}

#endif

/* crypto.getRandomValues/randomUUID draw from the same credited pool as
   TLS. Without this service the PSP had no secure random source at all, and
   chatgpt.com's composer died on randomUUID() before wiring up Send. */
static bool psp_platform_secure_random(void *context, void *data,
                                       size_t length)
{
    (void) context;
    return length == 0 || psp_entropy_fill(data, length);
}

/* Boot-only download recovery must not expand main when player-owned
   metadata changes the browser owner's layout. */
static TILEFINCH_COLD_PATH void psp_boot_resume_download(
    BrowserProfile *profile, PspOfflineStore *store, PspUiState *ui)
{
    if (!browser_profile_resume_offline_downloads(profile)
        || !offline_library_load(&store->library)) return;
    uint32_t resumed_id = 0;
    for (size_t index = 0; index < store->library.count; index++) {
        OfflineLibraryItem *item = &store->library.items[index];
        if (item->type == OFFLINE_ITEM_YOUTUBE
            && (item->state == OFFLINE_ITEM_PAUSED
                || item->state == OFFLINE_ITEM_QUEUED)
            && offline_download_manager_start(&store->download, item->id)) {
            resumed_id = item->id;
            break;
        }
    }
    if (resumed_id != 0) {
        printf("tilefinch-offline: event=auto-resume id=%u\n", (unsigned) resumed_id);
        psp_ui_show_status(ui, "RESUMING SAVED VIDEO", 240);
    }
}

int main(int argc, char *argv[])
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t validation_main_entered_us =
        (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t validation_entry_tick = 0;
    (void) psp_boot_timing_clock(&validation_entry_tick);
#endif
    psp_fpu_mask_exceptions();
    PspProcessResources process = {0};
    PspBrowserResources browser = {0};
    PspInteractiveState interactive = {
        .media_stability_min_free = UINT_MAX,
        .media_stability_min_largest = UINT_MAX,
        .media_stability_start_capacity = INT_MIN,
        .media_stability_start_percent = INT_MIN
    };
    const char *argv0 = argc > 0 ? argv[0] : NULL;
    const char *startup_failure_detail = "Unknown startup failure";
    const char *startup_failure_screen = "STARTUP FAILED";
    if (!tilefinch_install_paths_derive(argv0, &process.install_paths))
        memset(&process.install_paths, 0, sizeof(process.install_paths));
    psp_failure_report_paths = &process.install_paths;
    if (process.install_paths.slotted) (void) mkdir(process.install_paths.data_dir, 0777);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    psp_entropy_setup(&process.install_paths);
#endif
    uint64_t main_entered_us = (uint64_t) sceKernelGetSystemTimeWide();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_boot_timing_begin(validation_main_entered_us);
#endif
    atomic_init(&psp_home_exit_requested, false);
    psp_lifecycle_init(&psp_lifecycle);
    /*
     * Paint before touching the Memory Stick log, callbacks, or network
     * modules.  Log rotation alone took about 5.6 seconds on the physical
     * device, so presenting later left a misleading black screen even when
     * startup was healthy.
     */
    bool display_ready = psp_display_begin(
        &psp_display,
        psp_validation_display_wrap(psp_display_system_backend()));
    /* Frame one of the entrance: the mark alone. Nothing else has been read
       from the Memory Stick yet. */
    /* Keep pre-input boot completely static. The old entrance rebuilt and
       composited animated wave bands between slow initialization stages,
       making startup look jerky while no input could yet be acknowledged. */
    psp_present_boot_entrance(0u, "STARTING TILEFINCH", false, NULL);
    uint64_t boot_surface_presented_us =
        (uint64_t) sceKernelGetSystemTimeWide();
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t validation_surface_tick = 0;
    (void) psp_boot_timing_clock(&validation_surface_tick);
#endif
    PSP_BOOT_TIMING_MARK("first-scanout");
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
    psp_display_set_validation_logger(psp_log_printf);
    if (psp_boot_module_report != NULL) psp_boot_module_report();
    psp_boot_report_handoff(
        argc, argv, validation_entry_tick, validation_surface_tick);
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
        .preferred_language = psp_preferred_language,
        .preferred_date_format = psp_preferred_date_format,
        .secure_random = psp_platform_secure_random,
        .cooperate_at = psp_platform_cooperate,
        .present_rgb565 = psp_platform_present,
        .retire_frame = psp_platform_retire_frame,
        .log_message = psp_log_message,
    };
    tilefinch_platform_set_services(&services);
    PSP_BOOT_TIMING_MARK("platform-ready");
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
    PSP_BOOT_TIMING_MARK("media-pool-ready");
#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && defined(TILEFINCH_PSP_LIVE_NETWORK)
    psp_preload_validation_ca_bundle(&process.install_paths);
#endif

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
    /* This marks native services, not a JavaScript realm. Native HOME never
       initializes author-script execution. */
    PSP_BOOT_TIMING_MARK("runtime-ready");
#ifdef TILEFINCH_PSP_VALIDATION_LOG
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
    /* Report the lower bound already established by the early reservation
       admission probe. Do not probe the heap to exhaustion again here: after
       the aligned Media Engine pool has split the arena, a second failure-
       driven walk can strand newlib inside its growth path on hardware. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_report_heap_capacity_lower_bound();
#endif
    /* Cheap diagnostic reporting only. Synthetic clock work is opt-in after
       config parsing; ordinary validation boots must not benchmark clocks. */
    PSP_BOOT_TIMING_MARK("validation-probes-complete");
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
    /* Keep the already-visible first frame until native HOME is ready.
       Intermediate splash repaints each impose a scanout wait but provide
       neither a new usable surface nor an input acknowledgement. */

    /* Configuration layers, then the media knobs it decides: before anything
       can open a video. */
    PspStartupFailure boot_step = psp_boot_load_config(&process, argv0);
    if (boot_step.detail != NULL) goto boot_step_failed;
    PSP_BOOT_TIMING_MARK("config-accepted");
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (process.config.validation_power_test_auto != 0) {
        psp_clock_validation_probe();
        PSP_BOOT_TIMING_MARK("power-probe-complete");
    }
#endif
    /* The bypass is restricted to probes that require an initial document.
       Passive automation must observe the shipping entrance and native HOME
       or it cannot validate what users actually run. */
    bool boot_url_overridden =
        !psp_ui_native_home_url(process.config.url);
    bool boot_trace_replay =
        PSP_VALIDATION_KNOB(strcmp(process.config.trace, "none") != 0);
    bool boot_validation_mode =
        psp_boot_config_automation_requires_engine_first(&process.config);
    bool deterministic_boot = boot_url_overridden || boot_trace_replay
        || boot_validation_mode;
    if (deterministic_boot) {
        psp_present_boot_surface(
            PSP_UI_STARTUP_HOMEPAGE, "LOADING BROWSER", 420);
    }
    /* Trust assets and env knobs precede the engine that reads them. */
    BrowserConfig *engine_config = NULL;
    boot_step = psp_boot_configure_engine(&process, &engine_config);
    if (boot_step.detail != NULL) goto boot_step_failed;
    PSP_BOOT_TIMING_MARK("engine-configured");

    char engine_error[256] = {0};
    browser.engine = browser_engine_create(
        engine_config, engine_error, sizeof(engine_error));
    free(engine_config);
    if (browser.engine == NULL) {
        printf("tilefinch-psp-script: engine setup failed: %s\n",
               engine_error);
        startup_failure_detail = engine_error[0] == '\0'
            ? "Browser engine creation failed" : engine_error;
        startup_failure_screen = "STARTUP FAILED: BROWSER MEMORY";
        goto sleep_forever;
    }
    /* Script navigations run as cooperative jobs (see
       psp_begin_script_navigation), not inside a runtime advance. */
    browser_engine_set_defer_script_navigation(browser.engine, true);
    psp_log_engine_creation(browser.engine);
    psp_log_checkpoint("engine-created");
    PSP_BOOT_TIMING_MARK("engine-created");
    if (deterministic_boot) {
        psp_present_boot_surface(
            PSP_UI_STARTUP_HOMEPAGE, "OPENING HOME", 760);
    }
    browser.budget = browser_engine_budget(browser.engine);
    browser.session = browser_engine_session(browser.engine);
    PspEngineViews engine_views = {0};
    (void) psp_engine_views_refresh(&engine_views, browser.engine);
    if (browser.budget == NULL || browser.session == NULL || engine_views.navigation == NULL) {
        printf("tilefinch-psp-script: engine accessors unavailable\n");
        startup_failure_detail = "Browser engine accessors are unavailable";
        startup_failure_screen = "STARTUP FAILED: BROWSER STATE";
        goto sleep_forever;
    }
    js_wasm_component_configure(process.install_paths.program_dir);
#ifdef TILEFINCH_HAVE_PSP_VOICE
    psp_voice_component_configure(process.install_paths.program_dir);
#endif
    int validation_exit = 0;
    if (psp_validation_engine_qualifications(
            &browser, &process, argv0, &validation_exit))
        return validation_exit;
    /* Native HOME's one page face loads before its first frame; the profile
       sets TLS-session policy before the first network request. */
    PspBootProfileState boot_profile = {0};
    boot_step = psp_boot_open_profile_services(
        &process, &browser, &boot_profile);
    if (boot_step.detail != NULL) goto boot_step_failed;
    if (psp_validation_raster_qualification(
            &browser, &process, argv0, &validation_exit))
        return validation_exit;
    /* Site storage and the script cache open before any localStorage
       snapshot loads. */
    psp_boot_apply_profile_policy(&process, &browser, &boot_profile);
    const bool tls_session_persistence = boot_profile.tls_session_persistence;
    const bool javascript_enabled = boot_profile.javascript_enabled;
    const bool site_data_allowed = boot_profile.site_data_allowed;
    const ContentBlockerMode content_blocker_mode =
        boot_profile.content_blocker_mode;
    const unsigned persistent_cache_mb = boot_profile.persistent_cache_mb;
    const unsigned live_cache_kib = boot_profile.live_cache_kib;
    const bool live_cache_limit_set = boot_profile.live_cache_limit_set;
    PSP_BOOT_TIMING_MARK("profile-and-home-data-ready");
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
    /* Native HOME owns its presentation and executes no author code. Keep
       the placeholder document realm-free; every real navigation re-applies
       the configured global/per-site policy in psp_begin_page_load() before
       it is admitted. Direct initial-page boots apply that same effective
       policy after their final restored/configured URL has been selected. */
    bool initial_javascript_allowed = native_home_boot
        ? false
        : browser_profile_javascript_allowed_for_url(
              browser.profile, startup_url);
    if (!browser_engine_set_javascript_enabled(
            browser.engine, initial_javascript_allowed)) {
        printf("tilefinch-profile: JavaScript policy unavailable\n");
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
    browser.offline_store.show_progress = psp_work_cooperate_show_status;
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
        .profile_changed = psp_media_platform_profile_changed,
        .write_failure_report = psp_media_platform_write_failure_report,
        .install_paths = &process.install_paths
    };
    psp_media_init(
        &browser.media, browser.budget, browser.session, browser.profile, process.storage.profile,
        browser_engine_font_face(browser.engine, FONT_SANS), &media_platform);
    psp_active_media = &browser.media;
    psp_text_input_init(
        &process.text_input, browser.budget, "",
        psp_text_input_present, NULL);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    process.text_input.validation_poll = psp_input_script_text_frame;
    process.text_input.validation_finished = psp_input_script_report_pending;
#endif
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

    if (PSP_VALIDATION_KNOB(strcmp(process.config.trace, "none") != 0)) {
        char trace_path[768];
        /* A device path (e.g. a read-only copy staged on ms0:) is used as
           given; a bare name is a sibling of the executable. Replay only
           reads the trace. */
        if (strstr(process.config.trace, ":/") != NULL) {
            snprintf(trace_path, sizeof(trace_path), "%s",
                     process.config.trace);
        } else {
            psp_sibling_path(trace_path, sizeof(trace_path), argv0,
                             process.config.trace);
        }
        char trace_error[256] = {0};
        bool replay_begun = process.config.trace_keyed != 0
            ? fetch_trace_replay_begin_response_keyed(
                  trace_path, trace_error, sizeof(trace_error))
            : fetch_trace_replay_begin(
                  trace_path, trace_error, sizeof(trace_error));
        if (!replay_begun) {
            printf("tilefinch-psp-script: replay setup failed: %s\n",
                   trace_error);
            startup_failure_detail = trace_error[0] == '\0'
                ? "Trace replay setup failed" : trace_error;
            startup_failure_screen = "STARTUP FAILED: TEST TRACE";
            goto sleep_forever;
        }
        if (!fetch_trace_replay_seed_session(browser.session, trace_error,
                                             sizeof(trace_error))) {
            printf("tilefinch-psp-script: cookie seed failed: %s\n",
                   trace_error);
            startup_failure_detail = trace_error[0] == '\0'
                ? "Trace cookie seed failed" : trace_error;
            startup_failure_screen = "STARTUP FAILED: TEST TRACE";
            goto sleep_forever;
        }
    } else {
        printf("tilefinch-psp-script: live transport enabled\n");
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (process.config.trace_capture[0] != '\0')
        psp_begin_trace_capture(argv0, process.config.trace_capture);
#endif

#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /* Speculative-preconnect dwell state (docs/engineering/
       PSP_TRANSPORT.md). Persists across frames; a HOME tile must hold
       focus this long before its host is worth a background TCP+TLS connect. */
    FetchPreconnectDwell home_preconnect_dwell = {0};
#endif
    /* Every chrome setting the profile alone determines; the ones that also
       depend on the start page, boot.cfg or this boot's state follow. */
    psp_app_ui_sync_from_profile(&process.presentation.ui, browser.profile);
    psp_app_heavy_boot(browser.engine, browser.profile, &interactive,
                       &process.presentation.ui);
    process.presentation.ui.experimental_voice_input = experimental_voice_enabled;
    psp_ui_set_voice_component(
        &process.presentation.ui, PSP_UI_VOICE_COMPONENT_UNKNOWN, -1);
    process.presentation.ui.persistent_cache_mb = persistent_cache_mb;
    process.presentation.ui.live_cache_kib = live_cache_kib;
    process.presentation.ui.tls_session_persistence =
        tls_session_persistence;
    psp_network_diagnostics_enable(process.presentation.ui.wifi_diagnostics);
    process.presentation.ui.network_profile =
        (uint8_t) process.config.network_profile;
    process.presentation.ui.javascript_enabled = javascript_enabled ? 1u : 0u;
    process.presentation.ui.site_data_allowed = site_data_allowed ? 1u : 0u;
    process.presentation.ui.mixed_content_site_allowed =
        browser_session_mixed_content_site_allowed(browser.session, startup_url);
    process.presentation.ui.third_party_cookie_site_allowed =
        browser_profile_third_party_cookie_site_allowed(browser.profile, startup_url);
    BrowserChromeTheme chrome_theme =
        browser_profile_chrome_theme(browser.profile);
    char chrome_theme_error[64];
    if (!psp_app_apply_chrome_theme(
            &process, chrome_theme,
            browser_profile_chrome_theme_file(browser.profile),
            chrome_theme_error, sizeof(chrome_theme_error))) {
        /* A requested custom file is optional external state. Its absence
           must not block HOME or make ordinary startup depend on storage. */
        (void) psp_app_apply_chrome_theme(
            &process, BROWSER_CHROME_THEME_FINCH, NULL, NULL, 0u);
    }
    psp_ui_set_glyph_component(
        &process.presentation.ui,
        browser.glyph_component_session->installed_mask, 0,
        PSP_UI_GLYPH_COMPONENT_UNKNOWN, -1);
    process.presentation.ui.content_blocker_mode = (uint8_t) content_blocker_mode;
    process.presentation.ui.cookie_banner_hidden =
        browser_profile_cookie_banner_hidden(browser.profile, startup_url);
    process.presentation.ui.content_blocker_site_allowed =
        content_blocker_mode != CONTENT_BLOCKER_OFF
        && browser_profile_content_blocker_site_allowed(
               browser.profile, startup_url);
    process.presentation.ui.developer_update_available =
        process.config.developer_update_url[0] != '\0';
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* The Experimental screen's decoder picker opens on the spelling this
       boot actually used, not on the shipping default. Keep the table walk
       outside main's growth-tripwire symbol. */
    psp_app_seed_video_decoder_choice(&process.presentation.ui, &process.config);
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    process.presentation.ui.validation_media_test_phase =
        process.config.validation_media_stability_auto != 0 ? 1u : 0u;
#endif
    psp_refresh_page_color_mode(&process.presentation.ui);
    (void) psp_engine_settings_boot(
        browser.profile, browser.engine, &process.presentation.ui);
    psp_app_youtube_login_boot(&process, &browser);
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

    /* Direct page boots need their storage before author code can observe it.
       Native HOME instead starts a transactional, byte-bounded restore from
       the interactive loop: bookmarks and boot settings are profile data and
       have already populated HOME independently of these two files. */
    psp_site_data_restore_init(
        &interactive.site_data_restore,
        native_home_boot && process.persistent_site_data_available
            && live_cache_limit_set && persistent_cache_mb != 0,
        persistent_cache_mb,
        native_home_boot && process.persistent_site_data_available
            && site_data_allowed
            && browser_profile_persist_local_storage(browser.profile));
    if (!native_home_boot
        && process.persistent_site_data_available && live_cache_limit_set
        && persistent_cache_mb != 0) {
        BrowserSessionPersistenceLimits limits =
            psp_site_data_limits(persistent_cache_mb);
        bool loaded = psp_site_data_load(
            browser.session, process.storage.persistent_cache,
            BROWSER_SESSION_PERSIST_CACHE, &limits);
        printf("tilefinch-site-data: disk-cache=%uMB load=%s\n",
               persistent_cache_mb, loaded ? "ok" : "empty");
    }
    if (!native_home_boot && process.persistent_site_data_available
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
        && !psp_offline_url(startup_url)
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
            startup_failure_detail = "Initial network association failed";
            startup_failure_screen = "STARTUP FAILED: WI-FI";
            goto sleep_forever;
        }
        psp_log_checkpoint("network-ready");
    } else if (strcmp(process.config.trace, "none") == 0) {
        printf("tilefinch-network: deferred for local startup\n");
    }
#endif
    if (psp_validation_network_qualification(
            &browser, &process, &validation_exit))
        return validation_exit;

    /* Native HOME presents without touching the network; a direct boot loads
       its page here and falls back to the retry/home error page. */
    PspBootNavigation initial_navigation = psp_boot_initial_navigation(
        &process, &browser, &engine_views, startup_url, native_home_boot,
        restoring_last_page, &startup_recovery, argv0);
    bool loaded = initial_navigation.loaded;
    const bool initial_download_offer = initial_navigation.download_offer;
    const bool initial_error_page = initial_navigation.error_page;
    const uint64_t load_started_us = initial_navigation.load_started_us;
    if (loaded && browser_engine_render_frame(browser.engine, NULL)) {
        psp_sync_ui(&process.presentation.ui, browser.engine, browser.profile);
        if (initial_download_offer)
            psp_ui_show_file_download(&process.presentation.ui,
                psp_offline_store_file_preview(&browser.offline_store));
        psp_present(engine_views.frame, &process.presentation.ui);
    }
    if (!loaded) goto report;

    long advance_failures = 0;
    for (long i = 0; i < PSP_VALIDATION_KNOB(process.config.ticks); i++) {
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
           "glyph-scripts=0x%04x budget=%zu peak=%zu\n",
           (unsigned long long) (((uint64_t) sceKernelGetSystemTimeWide()
                                  - load_started_us) / 1000u),
           engine_views.navigation->page.layout.height,
           engine_views.navigation->page.layout.link_count,
           (unsigned) browser_engine_glyph_script_mask(browser.engine),
           browser.budget->current, browser.budget->peak);
    printf("tilefinch-psp-script: css-rules=%zu important=%zu layers=%zu "
           "vars=%zu\n",
           engine_views.navigation->page.stylesheet.count,
           engine_views.navigation->page.stylesheet.important_rule_count,
           engine_views.navigation->page.stylesheet.layer_count,
           engine_views.navigation->page.stylesheet.variable_count);
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
           "css-terminal-failures=%zu css-truncated=%zu/%zu/%zu "
           "images=%zu/%zu\n",
           engine_views.navigation->page.external_stylesheets.loaded,
           engine_views.navigation->page.external_stylesheets.discovered,
           engine_views.navigation->page.external_stylesheets.bytes,
           engine_views.navigation->page.external_stylesheets.terminal_failures,
           engine_views.navigation->page.external_stylesheets.truncated,
           engine_views.navigation->page.external_stylesheets
             .truncated_applied_bytes,
           engine_views.navigation->page.external_stylesheets
             .truncated_received_bytes,
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
    {
        /* The views alias the engine's live session through const
           pointers; refresh the result through the mutable owner. */
        NavigationSession *owned = browser_engine_navigation(browser.engine);
        script_runtime_refresh_result(
            owned->page.runtime, &owned->page.script_result);
    }
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
    printf("tilefinch-js-retention: handles=%zu/%zu observers=%zu "
           "timers=%zu network=%zu observer-drops=%zu\n",
           engine_views.navigation->page.script_result.dom_handle_slots_live,
           engine_views.navigation->page.script_result.dom_handle_slots_peak,
           engine_views.navigation->page.script_result.root_mutation_observers,
           engine_views.navigation->page.script_result.root_timers,
           engine_views.navigation->page.script_result.root_pending_network,
           engine_views.navigation->page.script_result.retention_observer_drops);
    printf("tilefinch-js-handles: high-water=%zu capacity=%zu growths=%zu "
           "growth-refusals=%zu exhaustions=%zu\n",
           engine_views.navigation->page.script_result
             .dom_handle_slots_high_water,
           engine_views.navigation->page.script_result
             .dom_handle_slot_capacity,
           engine_views.navigation->page.script_result.dom_handle_growths,
           engine_views.navigation->page.script_result
             .dom_handle_growth_refusals,
           engine_views.navigation->page.script_result
             .dom_handle_exhaustions);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
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
#endif
    psp_log_checkpoint("compact-report-complete");

    if (loaded) {
        if (!browser_engine_refresh_shell(browser.engine)
            || !browser_engine_render_frame(browser.engine, NULL)) {
            printf("tilefinch-psp-script: frame render failed: %s\n",
                   browser_engine_last_error(browser.engine));
            startup_failure_detail = browser_engine_last_error(browser.engine);
            if (startup_failure_detail == NULL
                || startup_failure_detail[0] == '\0')
                startup_failure_detail = "Initial frame render failed";
            startup_failure_screen = "STARTUP FAILED: RENDERER";
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
        psp_boot_resume_download(browser.profile, &browser.offline_store,
                                  &process.presentation.ui);

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
        /* Home is already visible. Its next ordinary frame publishes tab
           changes together with input instead of waiting another vblank
           before the very first sample. Explicit captures still publish. */
        if (!native_home_boot || PSP_VALIDATION_KNOB(process.config.dump_frame != 0))
            psp_present(engine_views.frame, &process.presentation.ui);
        if (PSP_VALIDATION_KNOB(process.config.dump_frame != 0)) {
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
        PSP_BOOT_TIMING_MARK("interactive-ready");
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        psp_ui_show_status(
            &process.presentation.ui,
            persistent_log_started
                ? "LOGGING ON - TILEFINCH/DATA"
                : "DIAGNOSTIC LOG UNAVAILABLE",
            360);
        psp_present(engine_views.frame, &process.presentation.ui);
#endif

        /* Discard launcher/boot transitions before the interactive loop
           becomes authoritative. Later frames consume the PSP latch so a
           quick tap cannot begin and end between two 30 Hz page samples. */
        (void) psp_controller_take_latched_pressed();

        psp_webgl_measurement_bind(engine_views.navigation);
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        /* Backend ownership is process-scoped, so install it outside the
           frame-loop hot boundary. The worker itself remains lazy until an
           installed game opens a multiplayer channel. */
        (void) psp_multiplayer_bind(
            &process, &browser, &network, &network_lifecycle);
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        psp_boot_input_probe(0);
#endif
        PspInteractiveResult interactive_result =
            psp_app_run_interactive(
                &process, &browser, &interactive, &engine_views,
                argv0, loaded, startup_url,
                boot_profile.startup_requests_blocked,
                &boot_queue, &home_preconnect_dwell,
                &power_policy,
                native_home_boot, restoring_last_page,
                initial_error_page, psp_offline_url(startup_url)
#ifdef TILEFINCH_PSP_LIVE_NETWORK
                , &network, &network_lifecycle
#endif
            );
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (process.config.dump_frame != 0) psp_dump_final_frame(argv0);
#endif
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
    goto sleep_forever;

boot_step_failed:
    startup_failure_detail = boot_step.detail;
    startup_failure_screen = boot_step.screen;
sleep_forever:
    psp_log_set_phase(PSP_LOG_PHASE_HALTED);
    psp_report_startup_failure(
        startup_failure_screen, startup_failure_detail,
        process.config.url[0] == '\0' ? TILEFINCH_HOMEPAGE_URL
                                      : process.config.url);
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
