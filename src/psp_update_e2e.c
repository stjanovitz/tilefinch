#include "psp_app/psp_app_internal.h"

#include "psp_update_e2e.h"

#if defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && defined(TILEFINCH_PSP_LIVE_NETWORK)

#define UPDATE_ROUTE_CONFIG "update-route.cfg"
#define UPDATE_ROUTE_CONFIG_LIMIT 1024u

void psp_update_e2e_init(
    PspUpdateE2E *driver, bool requested, bool trial_health_pending)
{
    if (driver == NULL) return;
    memset(driver, 0, sizeof(*driver));
    driver->requested = requested;
    driver->candidate_boot = requested && trial_health_pending;
}

bool psp_update_e2e_confirmed(
    PspUpdateE2E *driver, const TilefinchUpdateState *healthy,
    TilefinchUpdateSlot running_slot)
{
    if (driver == NULL || healthy == NULL || !driver->candidate_boot
        || driver->terminal || healthy->active_slot != running_slot
        || healthy->installed_sequence <= TILEFINCH_RELEASE_SEQUENCE) {
        return false;
    }
    driver->terminal = true;
    printf(
        "tilefinch-update-e2e: outcome=complete slot=%u sequence=%llu\n",
        (unsigned) running_slot,
        (unsigned long long) healthy->installed_sequence);
    psp_log_checkpoint("update-e2e-complete");
    return true;
}

static void psp_update_e2e_fail(
    PspUpdateE2E *driver, PspExitPlan *exit_plan,
    const char *stage, const PspUpdateSession *session)
{
    driver->terminal = true;
    printf(
        "tilefinch-update-e2e: outcome=failed stage=%s "
        "client-phase=%u status=%u install-phase=%u message=%.48s\n",
        stage,
        session == NULL ? 0u
            : (unsigned) session->client_snapshot.phase,
        session == NULL ? 0u
            : (unsigned) session->client_snapshot.status,
        session == NULL ? 0u
            : (unsigned) session->install_snapshot.phase,
        session == NULL ? "unavailable" : session->client_snapshot.message);
    psp_log_checkpoint("update-e2e-failed");
    psp_exit_plan_request(exit_plan, PSP_EXIT_VALIDATION_COMPLETE);
}

static bool psp_update_e2e_directory(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static void psp_update_e2e_log_journal(const char *when, const char *data_dir)
{
    TilefinchUpdateState state;
    if (!tilefinch_update_journal_load(data_dir, &state, NULL)) {
        printf("tilefinch-update-route: journal %s none\n", when);
        return;
    }
    printf("tilefinch-update-route: journal %s generation=%llu active=%d "
           "pending=%d previous=%d trial=%d installed=%llu candidate=%llu\n",
           when, (unsigned long long) state.generation,
           (int) state.active_slot, (int) state.pending_slot,
           (int) state.previous_slot, (int) state.trial,
           (unsigned long long) state.installed_sequence,
           (unsigned long long) state.candidate_sequence);
}

/* What the launcher and the session left in the test install, read only:
   one line per entry of the install root, both slots, data/ and
   data/update/. The journal says which slot is which; this shows the
   startup copies, the decline file and a staged or leftover package. */
static void psp_update_e2e_log_install(const TilefinchInstallPaths *paths)
{
    static const char *const directories[] = {
        "", "slot-a", "slot-b", "data", "data/update"
    };
    for (size_t index = 0;
         index < sizeof(directories) / sizeof(directories[0]); index++) {
        char directory[TILEFINCH_INSTALL_PATH_LIMIT];
        int written = directories[index][0] == '\0'
            ? snprintf(directory, sizeof(directory), "%s",
                       paths->install_root)
            : snprintf(directory, sizeof(directory), "%s/%s",
                       paths->install_root, directories[index]);
        if (written <= 0 || (size_t) written >= sizeof(directory)) continue;
        DIR *listing = opendir(directory);
        if (listing == NULL) {
            printf("tilefinch-update-route: dir %s absent\n",
                   directories[index][0] == '\0' ? "." : directories[index]);
            continue;
        }
        unsigned entries = 0;
        struct dirent *entry;
        while ((entry = readdir(listing)) != NULL && entries < 64u) {
            if (strcmp(entry->d_name, ".") == 0
                || strcmp(entry->d_name, "..") == 0) continue;
            char path[TILEFINCH_INSTALL_PATH_LIMIT];
            struct stat info;
            written = snprintf(path, sizeof(path), "%s/%s", directory,
                               entry->d_name);
            bool known = written > 0 && (size_t) written < sizeof(path)
                && stat(path, &info) == 0;
            printf("tilefinch-update-route: file %s%s%s %s size=%lld\n",
                   directories[index], directories[index][0] == '\0'
                       ? "" : "/", entry->d_name,
                   known && S_ISDIR(info.st_mode) ? "dir" : "file",
                   known ? (long long) info.st_size : -1ll);
            entries++;
        }
        (void) closedir(listing);
    }
}

/*
 * The recorded route (psp_update_route.h). Without update-route.cfg beside
 * the trace this is the loopback qualification it always was. With one, a
 * keyed trace replaces the network and the session alone gets the named
 * Memory Stick install. Refusing writes nothing: the install's slot and
 * data directory must already exist (one ordinary XMB start of the test
 * install creates data/).
 */
static bool psp_update_e2e_route_load(
    PspUpdateE2E *driver, const char *trace,
    const TilefinchInstallPaths *install_paths)
{
    if (strcmp(trace, "none") == 0) return true;
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    char relative[TILEFINCH_INSTALL_PATH_LIMIT];
    int written = strstr(trace, ":/") != NULL
        ? snprintf(path, sizeof(path), "%s/%s", trace, UPDATE_ROUTE_CONFIG)
        : snprintf(relative, sizeof(relative), "%s/%s", trace,
                   UPDATE_ROUTE_CONFIG);
    bool named = written > 0 && (size_t) written < sizeof(path)
        && (strstr(trace, ":/") != NULL
            || tilefinch_install_program_path(
                   install_paths, relative, path, sizeof(path)));
    FILE *file = named ? fopen(path, "rb") : NULL;
    if (file == NULL) return true;
    char text[UPDATE_ROUTE_CONFIG_LIMIT + 1u];
    size_t length = fread(text, 1, sizeof(text), file);
    (void) fclose(file);
    char why[64] = "too long";
    if (length > UPDATE_ROUTE_CONFIG_LIMIT
        || !psp_update_route_parse(text, length, &driver->route,
                                   why, sizeof(why))) {
        printf("tilefinch-update-route: refused config reason=\"%s\"\n", why);
        return false;
    }
    char eboot[TILEFINCH_INSTALL_PATH_LIMIT];
    struct stat info;
    bool slot_present = tilefinch_install_program_path(
            &driver->route.paths, "EBOOT.PBP", eboot, sizeof(eboot))
        && stat(eboot, &info) == 0 && S_ISREG(info.st_mode);
    bool data_present = psp_update_e2e_directory(driver->route.paths.data_dir);
    if (!slot_present || !data_present) {
        printf("tilefinch-update-route: refused missing slot-eboot=%d "
               "data=%d root=%s (nothing written; start the test install "
               "from the XMB once first)\n",
               slot_present ? 1 : 0, data_present ? 1 : 0,
               driver->route.paths.install_root);
        return false;
    }
    driver->route_armed = true;
    printf("tilefinch-update-route: armed root=%s running=%s data=%s "
           "release-sequence=%llu journal-only=%d\n",
           driver->route.paths.install_root, driver->route.paths.slot_name,
           driver->route.paths.data_dir,
           (unsigned long long) TILEFINCH_RELEASE_SEQUENCE,
           driver->route.journal_only ? 1 : 0);
    psp_update_e2e_log_journal("before", driver->route.paths.data_dir);
    psp_update_e2e_log_install(&driver->route.paths);
    return true;
}

/* One line per client or installer phase change, so a device log shows how
   far a recorded update got and when. */
static void psp_update_e2e_stage(
    PspUpdateE2E *driver, const PspUpdateSession *session)
{
    int client = (int) session->client_snapshot.phase;
    int install = session->installer == NULL
        ? -1 : (int) session->install_snapshot.phase;
    if (client == driver->client_phase && install == driver->install_phase)
        return;
    driver->client_phase = client;
    driver->install_phase = install;
    printf("tilefinch-update-route: stage client=%d install=%d "
           "bytes=%llu/%llu status=%s ms=%llu\n", client, install,
           (unsigned long long) session->client_snapshot.bytes_received,
           (unsigned long long) session->client_snapshot.bytes_total,
           tilefinch_update_status_name(session->client_snapshot.status),
           (unsigned long long)
               (tilefinch_platform_monotonic_time_us() / 1000u));
}

void psp_update_e2e_pump(
    PspUpdateE2E *driver, const char *trace,
    const char *metadata_url, const PspNetworkLifecycle *network_lifecycle,
    const PspNetwork *network, PspUpdateSession *update_session,
    Budget *budget, const TilefinchInstallPaths *install_paths,
    PspUiState *ui, PspExitPlan *exit_plan)
{
    if (driver == NULL || !driver->requested || driver->candidate_boot
        || driver->terminal || trace == NULL || metadata_url == NULL
        || network_lifecycle == NULL || network == NULL
        || update_session == NULL || budget == NULL || install_paths == NULL
        || ui == NULL || exit_plan == NULL) {
        return;
    }
    if (!driver->route_loaded) {
        driver->route_loaded = true;
        driver->client_phase = -1;
        driver->install_phase = -1;
        if (!psp_update_e2e_route_load(driver, trace, install_paths)) {
            psp_update_e2e_fail(driver, exit_plan, "route", NULL);
            return;
        }
        if (driver->route_armed && driver->route.journal_only) {
            /* Before the session exists: no directory, sweep or download
               touches the stick. */
            driver->terminal = true;
            printf("tilefinch-update-e2e: outcome=journal-only\n");
            psp_log_checkpoint("update-e2e-journal-only");
            psp_exit_plan_request(exit_plan, PSP_EXIT_VALIDATION_COMPLETE);
            return;
        }
    }
    /* The session acts as the route's slot; every other file this process
       owns stays where its own argv[0] put it. */
    const TilefinchInstallPaths *session_paths =
        driver->route_armed ? &driver->route.paths : install_paths;
    bool source_ready = driver->route_armed
        ? fetch_trace_replay_active()
        : strcmp(trace, "none") == 0
            && psp_network_lifecycle_ready(network_lifecycle)
            && network->status == PSP_NETWORK_READY;
    if (!driver->started && source_ready) {
        /* HOME may have spent the shared transport's one PSP operation slot
           on a speculative homepage connection in this same frame.  The
           foreground update owns the slot now; retiring speculation first is
           the same precedence rule ordinary navigation uses. */
        fetch_preconnect_cancel("update-e2e");
        bool initialized = psp_update_session_initialized(update_session)
            || psp_update_session_initialize(
                   update_session, budget, session_paths,
                   &(PspUpdateSessionOptions) {
                       .channel = BROWSER_UPDATE_CHANNEL_STABLE,
                       .signed_metadata_url_override = metadata_url
                   },
                   ui);
        time_t update_now = time(NULL);
        if (!initialized || update_now <= 0
            || !psp_update_session_begin_check(
                   update_session, (uint64_t) update_now, true)) {
            psp_update_session_refresh_ui(update_session, ui);
            psp_update_e2e_fail(
                driver, exit_plan, "begin", update_session);
            return;
        }
        driver->started = true;
        printf(
            "tilefinch-update-e2e: outcome=started url=%s\n",
            metadata_url);
    }
    if (!driver->started || driver->terminal) return;

    if (driver->route_armed) psp_update_e2e_stage(driver, update_session);
    TilefinchUpdateClientPhase phase =
        update_session->client_snapshot.phase;
    bool install_failed = update_session->installer != NULL
        && update_session->install_snapshot.phase
               >= TILEFINCH_UPDATE_INSTALL_ERROR;
    if (phase == TILEFINCH_UPDATE_CLIENT_ERROR
        || phase == TILEFINCH_UPDATE_CLIENT_UP_TO_DATE || install_failed) {
        if (driver->route_armed)
            psp_update_e2e_log_journal("after", driver->route.paths.data_dir);
        psp_update_e2e_fail(
            driver, exit_plan,
            install_failed ? "install" : "client", update_session);
        return;
    }
    PspUpdatePrimaryResult primary = psp_update_session_primary(
        update_session, session_paths);
    if (primary == PSP_UPDATE_PRIMARY_RESTART_REQUIRED) {
        printf(
            "tilefinch-update-e2e: outcome=installed action=cold-reboot\n");
        if (driver->route_armed) {
            psp_update_e2e_log_journal("after", driver->route.paths.data_dir);
            psp_update_e2e_log_install(&driver->route.paths);
        }
        psp_log_checkpoint("update-e2e-restart");
        /* PPSSPP preserves argv[0] across a browser -> launcher -> candidate
           nested LoadExec chain. That makes a correctly launched slot B
           identify itself as slot A and withhold trial health. Leave the
           journal PENDING and let the isolated host harness perform the same
           cold launcher entry a real reboot provides. Production updates do
           not enable this validation driver and keep their in-app restart.
           A recorded route runs from host0:, so it cannot restart into the
           install at all: the next XMB start is the launcher's. */
        psp_exit_plan_request(
            exit_plan, PSP_EXIT_VALIDATION_COMPLETE);
    }
}

#endif
