/* Host rehearsal of the recorded update route's download and install
 * (scripts/make-update-replay.py, src/psp_update_e2e.c).
 *
 *   tilefinch-update-replay-probe TRACE ROOT.tfur METADATA_URL INSTALL_ROOT
 *                                 RUNNING_SLOT(a|b) INSTALLED_SEQUENCE
 *
 * Replays TRACE response-keyed, as a PSP validation build does with
 * trace_keyed=1, and drives the shipping signed client and installer the way
 * psp_update_session does for a signed-URL override: check, download into
 * INSTALL_ROOT/data/update/package.part, then install into the slot that is
 * not RUNNING_SLOT. Prints the journal before and after. Exit 0 only when a
 * PENDING trial of the inactive slot was journaled.
 *
 * tests/test_update_replay_route.py builds throwaway keys and a fake slot,
 * runs the trace maker, and checks the result. Nothing here reaches a
 * network: replay serves both requests or the run fails.
 */
#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/update.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define PROBE_BUDGET_BYTES (64u * 1024u * 1024u)
#define PROBE_PUMP_LIMIT 200000u

static void print_state(const char *when, const TilefinchUpdateState *state)
{
    printf("journal %s generation=%llu active=%d pending=%d previous=%d "
           "trial=%d installed=%llu candidate=%llu\n", when,
           (unsigned long long) state->generation, (int) state->active_slot,
           (int) state->pending_slot, (int) state->previous_slot,
           (int) state->trial,
           (unsigned long long) state->installed_sequence,
           (unsigned long long) state->candidate_sequence);
}

static bool read_file(const char *path, uint8_t *buffer, size_t capacity,
                      size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    *length = fread(buffer, 1, capacity, file);
    bool whole = feof(file) && !ferror(file);
    fclose(file);
    return whole && *length != 0;
}

static int fail(const char *what, const char *detail)
{
    fprintf(stderr, "update-replay-probe: %s: %s\n", what,
            detail == NULL ? "" : detail);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 7 || (strcmp(argv[5], "a") != 0 && strcmp(argv[5], "b") != 0))
        return fail("usage", "TRACE ROOT URL INSTALL_ROOT a|b SEQUENCE");
    const char *trace = argv[1];
    const char *install_root = argv[4];
    TilefinchUpdateSlot running = argv[5][0] == 'a'
        ? TILEFINCH_UPDATE_SLOT_A : TILEFINCH_UPDATE_SLOT_B;
    uint64_t release_sequence = strtoull(argv[6], NULL, 10);

    uint8_t root_bytes[4096];
    size_t root_length = 0;
    TilefinchUpdateRoot root;
    if (!read_file(argv[2], root_bytes, sizeof(root_bytes), &root_length)
        || tilefinch_update_parse_root(root_bytes, root_length, &root)
               != TILEFINCH_UPDATE_OK)
        return fail("root", argv[2]);

    char argv0[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(argv0, sizeof(argv0), "%s/slot-%s/EBOOT.PBP", install_root,
             argv[5]);
    TilefinchInstallPaths paths;
    if (!tilefinch_install_paths_derive(argv0, &paths) || !paths.slotted)
        return fail("paths", argv0);
    char update_dir[TILEFINCH_INSTALL_PATH_LIMIT];
    char part[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!tilefinch_install_data_path(&paths, "update", update_dir,
                                     sizeof(update_dir))
        || (mkdir(update_dir, 0777) != 0 && errno != EEXIST)
        || !tilefinch_install_data_path(&paths, "update/package.part", part,
                                        sizeof(part)))
        return fail("data", paths.data_dir);

    /* As psp_update_session_initialize: the journal, or a first install's
       in-memory default. */
    TilefinchUpdateState state;
    if (tilefinch_update_journal_load(paths.data_dir, &state, NULL)) {
        print_state("before", &state);
    } else {
        memset(&state, 0, sizeof(state));
        state.generation = 1;
        state.active_slot = running;
        state.installed_sequence = release_sequence;
        printf("journal before none\n");
    }

    char error[256] = "";
    if (!fetch_trace_replay_begin_response_keyed(trace, error, sizeof(error)))
        return fail("replay", error);
    Budget budget;
    budget_init(&budget, PROBE_BUDGET_BYTES);
    bool pair_valid = false;
    for (size_t at = 0; at < 32u; at++)
        pair_valid |= state.installed_sha256[at] != 0;
    TilefinchUpdateClientOptions options = {
        .embedded_root = &root,
        .installed_sequence = state.installed_sequence,
        .installed_package_sha256 = state.installed_sha256,
        .installed_sequence_valid = state.installed_sequence != 0,
        .installed_pair_valid = pair_valid,
        .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL,
        .repository_owner = "stjanovitz",
        .repository_name = "tilefinch",
        .metadata_url_override = argv[3],
        .package_relative_to_metadata = true,
        .trust = TILEFINCH_UPDATE_TRUST_SIGNED,
        .package_part_path = part
    };
    TilefinchUpdateClient *client =
        tilefinch_update_client_create(&budget, &options);
    if (client == NULL) return fail("client", "create refused");
    TilefinchUpdateClientSnapshot snapshot;
    int status = 1;
    TilefinchUpdateInstallJob *job = NULL;
    if (!tilefinch_update_client_begin_check(client, (uint64_t) time(NULL),
                                             true)) {
        tilefinch_update_client_snapshot(client, &snapshot);
        fail("check", snapshot.message);
        goto done;
    }
    bool download_started = false;
    for (unsigned pump = 0; pump < PROBE_PUMP_LIMIT; pump++) {
        (void) tilefinch_update_client_pump(client, 2000u);
        (void) tilefinch_update_client_snapshot(client, &snapshot);
        if (snapshot.phase == TILEFINCH_UPDATE_CLIENT_AVAILABLE
            && !download_started) {
            printf("client signature-verified sequence=%llu version=%s "
                   "asset=%s size=%llu\n",
                   (unsigned long long) snapshot.manifest.release_sequence,
                   snapshot.manifest.version, snapshot.manifest.asset,
                   (unsigned long long) snapshot.manifest.package_size);
            download_started = true;
            if (!tilefinch_update_client_begin_download(client)) {
                (void) tilefinch_update_client_snapshot(client, &snapshot);
                fail("download", snapshot.message);
                goto done;
            }
        }
        if (snapshot.phase == TILEFINCH_UPDATE_CLIENT_DOWNLOADED
            || snapshot.phase == TILEFINCH_UPDATE_CLIENT_ERROR
            || snapshot.phase == TILEFINCH_UPDATE_CLIENT_UP_TO_DATE) break;
    }
    if (snapshot.phase != TILEFINCH_UPDATE_CLIENT_DOWNLOADED) {
        fprintf(stderr, "update-replay-probe: client phase=%d status=%s "
                "message=%s\n", (int) snapshot.phase,
                tilefinch_update_status_name(snapshot.status),
                snapshot.message);
        goto done;
    }
    printf("client hash-verified bytes=%llu\n",
           (unsigned long long) snapshot.bytes_received);

    size_t envelope_length = 0;
    const uint8_t *envelope =
        tilefinch_update_client_envelope(client, &envelope_length);
    TilefinchUpdateDownloadedPackageProof proof;
    bool have_proof = tilefinch_update_client_download_proof(client, &proof);
    TilefinchUpdateSlot inactive = running == TILEFINCH_UPDATE_SLOT_A
        ? TILEFINCH_UPDATE_SLOT_B : TILEFINCH_UPDATE_SLOT_A;
    TilefinchUpdateInstallOptions install = {
        .package_path = part,
        .envelope = envelope,
        .envelope_length = envelope_length,
        .manifest = &snapshot.manifest,
        .manifest_digest = snapshot.manifest_digest,
        .install_root = paths.install_root,
        .data_dir = paths.data_dir,
        .inactive_slot = inactive,
        .running_slot = running,
        .current_state = state,
        .trust = TILEFINCH_UPDATE_TRUST_SIGNED,
        .download_proof = have_proof ? &proof : NULL
    };
    job = tilefinch_update_install_create(&budget, &install);
    if (job == NULL) {
        fail("install", "create refused");
        goto done;
    }
    TilefinchUpdateInstallSnapshot progress;
    for (unsigned pump = 0; pump < PROBE_PUMP_LIMIT; pump++) {
        (void) tilefinch_update_install_pump(job, 64u * 1024u);
        (void) tilefinch_update_install_snapshot(job, &progress);
        if (progress.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE) break;
    }
    if (progress.phase != TILEFINCH_UPDATE_INSTALL_COMPLETE) {
        fprintf(stderr, "update-replay-probe: install phase=%d status=%s "
                "message=%s\n", (int) progress.phase,
                tilefinch_update_status_name(progress.status),
                progress.message);
        goto done;
    }
    printf("install complete files=%zu bytes=%llu\n", progress.files_completed,
           (unsigned long long) progress.bytes_processed);
    TilefinchUpdateState after;
    if (!tilefinch_update_journal_load(paths.data_dir, &after, NULL)) {
        fail("journal", "unreadable after install");
        goto done;
    }
    print_state("after", &after);
    if (after.trial == TILEFINCH_UPDATE_TRIAL_PENDING
        && after.pending_slot == inactive
        && after.active_slot == running
        && after.candidate_sequence == snapshot.manifest.release_sequence)
        status = 0;
    else
        fail("journal", "no pending trial of the inactive slot");

done:
    tilefinch_update_install_destroy(job);
    tilefinch_update_client_destroy(client);
    fetch_trace_end();
    if (budget.current != 0) {
        fprintf(stderr, "update-replay-probe: %zu bytes still owned\n",
                budget.current);
        status = 1;
    }
    return status;
}
