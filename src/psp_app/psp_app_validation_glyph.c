/* Recorded language-pack installs on a device without Wi-Fi.
 *
 * A response-keyed trace holding the release's recorded responses
 * (scripts/record-glyph-pack-replay.py) replaces only the bytes on the wire:
 * the offer or menu route, session, update client, redirects, signature,
 * hash, Memory Stick staging and activation are the shipping code. Two things
 * stop that code running in a scripted PSPLink run, and this unit answers
 * both when the trace's glyph-replay.cfg sidecar asks for it:
 *
 * - trace mode closes the pack routes, because a replay normally has nothing
 *   to install from (psp_app_glyph_component.c, psp_validation_glyph_armed);
 * - the program runs from host0:, so it is not slotted and has no install
 *   root. The pack session alone gets the real Memory Stick paths
 *   (psp_validation_glyph_paths); the profile, logs and every other file stay
 *   where the run keeps them.
 *
 * glyph-replay.cfg, one key per line:
 *   install_root=ms0:/PSP/GAME/TILEFINCH
 *   make_components=1   (optional) create root/components when it is absent
 *   rename_probe=1      (optional) log what a cross-directory rename does,
 *                       using scratch files in components/glyph-cyrillic
 *
 * The run writes only below components/, in the directory of the pack the
 * route installs (and glyph-cyrillic for the probe). If root/components does
 * not exist it refuses to arm rather than create anything outside it. The
 * probe is opt-in: it writes to the Memory Stick on every armed run, whichever
 * pack that run installs.
 *
 * Compiled into psp-browser-script only when TILEFINCH_PSP_VALIDATION_LOG is
 * on (see cmake/TilefinchTargets.cmake). Shipping EBOOTs omit this TU.
 */
#include "psp_app_validation.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tilefinch/budget.h"
#include "tilefinch/psp_glyph_component_session.h"
#include "tilefinch/update.h"

#define GLYPH_REPLAY_PACK_DIRECTORY "components/glyph-cyrillic"

static struct {
    bool loaded;
    bool armed;
    bool manifest_logged;
    TilefinchInstallPaths paths;
    int client_phase;
    int install_phase;
    uint64_t started_us;
    uint64_t stage_us;
} glyph_replay = {.client_phase = -1, .install_phase = -1};

static bool directory_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool glyph_replay_join(char output[TILEFINCH_INSTALL_PATH_LIMIT],
                              const char *left, const char *right)
{
    int written = snprintf(output, TILEFINCH_INSTALL_PATH_LIMIT, "%s/%s",
                           left, right);
    return written > 0 && written < (int) TILEFINCH_INSTALL_PATH_LIMIT;
}

static const char *glyph_replay_entry(const char *pack_dir, const char *name)
{
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    struct stat info;
    return glyph_replay_join(path, pack_dir, name) && stat(path, &info) == 0
        ? "1" : "0";
}

/* What this firmware's rename does across directories, inside the pack's own
   directory, with its temporary files gone again. PPSSPP ignores the target's
   directory and renames within the source's. */
static void glyph_replay_rename_probe(const char *pack_dir)
{
    static const char *const names[] = {
        "probe-a/from", "probe-b/to", "probe-a/to", "probe-b", "probe-a"
    };
    char paths[5][TILEFINCH_INSTALL_PATH_LIMIT];
    for (size_t at = 0; at < 5u; at++) {
        if (!glyph_replay_join(paths[at], pack_dir, names[at])) return;
    }
    (void) mkdir(paths[4], 0777);
    (void) mkdir(paths[3], 0777);
    FILE *file = fopen(paths[0], "wb");
    if (file != NULL) (void) fclose(file);
    int result = rename(paths[0], paths[1]);
    int saved_errno = errno;
    struct stat info;
    printf("tilefinch-glyph-replay: cross-dir-rename result=%d errno=%d "
           "at-target=%d in-source-dir=%d source-left=%d\n", result,
           saved_errno, stat(paths[1], &info) == 0 ? 1 : 0,
           stat(paths[2], &info) == 0 ? 1 : 0,
           stat(paths[0], &info) == 0 ? 1 : 0);
    (void) unlink(paths[0]);
    (void) unlink(paths[1]);
    (void) unlink(paths[2]);
    (void) rmdir(paths[3]);
    (void) rmdir(paths[4]);
}

/* Reads the sidecar and prepares the two directories the run needs. */
static void glyph_replay_load(PspApp *app)
{
    glyph_replay.loaded = true;
    const PspBootConfig *config = &app->process->config;
    if (strcmp(config->trace, "none") == 0 || config->trace_keyed == 0) return;
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    char relative[TILEFINCH_INSTALL_PATH_LIMIT];
    bool named = strstr(config->trace, ":/") != NULL
        ? glyph_replay_join(path, config->trace, "glyph-replay.cfg")
        : glyph_replay_join(relative, config->trace, "glyph-replay.cfg")
            && tilefinch_install_program_path(
                   &app->process->install_paths, relative, path, sizeof(path));
    FILE *file = named ? fopen(path, "rb") : NULL;
    if (file == NULL) return;
    char root[TILEFINCH_INSTALL_PATH_LIMIT] = {0};
    bool make_components = false;
    bool rename_probe = false;
    char line[TILEFINCH_INSTALL_PATH_LIMIT + 32u];
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "install_root=", 13) == 0)
            snprintf(root, sizeof(root), "%s", line + 13);
        else if (strcmp(line, "make_components=1") == 0)
            make_components = true;
        else if (strcmp(line, "rename_probe=1") == 0)
            rename_probe = true;
    }
    (void) fclose(file);
    char components[TILEFINCH_INSTALL_PATH_LIMIT];
    char pack_dir[TILEFINCH_INSTALL_PATH_LIMIT];
    if (strncmp(root, "ms0:/", 5) != 0 || strstr(root, "..") != NULL
        || !glyph_replay_join(components, root, "components")
        || !glyph_replay_join(pack_dir, root, GLYPH_REPLAY_PACK_DIRECTORY)) {
        printf("tilefinch-glyph-replay: refused bad-config root=\"%s\"\n",
               root);
        return;
    }
    bool components_existed = directory_exists(components);
    if (!directory_exists(root)
        || (!components_existed && !make_components)) {
        printf("tilefinch-glyph-replay: refused missing root=%d components=%d "
               "(nothing written; make_components=1 creates components)\n",
               directory_exists(root) ? 1 : 0, components_existed ? 1 : 0);
        return;
    }
    bool pack_existed = directory_exists(pack_dir);
    if ((!components_existed && mkdir(components, 0777) != 0)
        || (rename_probe && !pack_existed && mkdir(pack_dir, 0777) != 0)) {
        printf("tilefinch-glyph-replay: refused mkdir-failed\n");
        return;
    }
    if (rename_probe) glyph_replay_rename_probe(pack_dir);
    glyph_replay.paths = app->process->install_paths;
    snprintf(glyph_replay.paths.install_root,
             sizeof(glyph_replay.paths.install_root), "%s", root);
    glyph_replay.paths.slotted = true;
    glyph_replay.armed = true;
    printf("tilefinch-glyph-replay: armed install_root=%s "
           "components-created=%d pack-dir-created=%d\n", root,
           components_existed ? 0 : 1,
           rename_probe && !pack_existed ? 1 : 0);
    printf("tilefinch-glyph-replay: pack-dir-before active=%s previous=%s "
           "candidate=%s uninstalled=%s\n",
           glyph_replay_entry(pack_dir, "active"),
           glyph_replay_entry(pack_dir, "previous"),
           glyph_replay_entry(pack_dir, "candidate.tmp"),
           glyph_replay_entry(pack_dir, "UNINSTALLED"));
}

bool psp_validation_glyph_armed(PspApp *app)
{
    if (!glyph_replay.loaded) glyph_replay_load(app);
    return glyph_replay.armed;
}

const TilefinchInstallPaths *psp_validation_glyph_paths(PspApp *app)
{
    return psp_validation_glyph_armed(app)
        ? &glyph_replay.paths : &app->process->install_paths;
}

static const char *client_phase_name(int phase)
{
    switch (phase) {
        case TILEFINCH_UPDATE_CLIENT_IDLE: return "idle";
        case TILEFINCH_UPDATE_CLIENT_CHECKING: return "checking";
        case TILEFINCH_UPDATE_CLIENT_AVAILABLE: return "signature-verified";
        case TILEFINCH_UPDATE_CLIENT_DOWNLOADING: return "downloading";
        case TILEFINCH_UPDATE_CLIENT_CANCELLING: return "cancelling";
        case TILEFINCH_UPDATE_CLIENT_DOWNLOADED: return "hash-verified";
        case TILEFINCH_UPDATE_CLIENT_UP_TO_DATE: return "up-to-date";
        case TILEFINCH_UPDATE_CLIENT_ERROR: return "error";
        default: return "none";
    }
}

static const char *install_phase_name(int phase)
{
    switch (phase) {
        case TILEFINCH_UPDATE_INSTALL_VERIFYING: return "verifying";
        case TILEFINCH_UPDATE_INSTALL_READING_TABLE: return "reading-table";
        case TILEFINCH_UPDATE_INSTALL_PREPARING: return "preparing";
        case TILEFINCH_UPDATE_INSTALL_EXTRACTING: return "extracting";
        case TILEFINCH_UPDATE_INSTALL_FINALIZING: return "finalizing";
        case TILEFINCH_UPDATE_INSTALL_PROMOTING: return "promoting";
        case TILEFINCH_UPDATE_INSTALL_PREPARING_STARTUP:
            return "preparing-startup";
        case TILEFINCH_UPDATE_INSTALL_COMPLETE: return "complete";
        case TILEFINCH_UPDATE_INSTALL_ERROR: return "error";
        case TILEFINCH_UPDATE_INSTALL_CANCELLED: return "cancelled";
        default: return "none";
    }
}

/* One line per stage: where a run ended and how long each stage took. */
void psp_validation_glyph_stage(PspApp *app,
                                const PspGlyphComponentSession *session)
{
    int client = session->client == NULL
        ? -1 : (int) session->client_snapshot.phase;
    int install = session->installer == NULL
        ? -1 : (int) session->install_snapshot.phase;
    if (client == glyph_replay.client_phase
        && install == glyph_replay.install_phase) return;
    uint64_t now = tilefinch_platform_monotonic_time_us();
    if (glyph_replay.started_us == 0) glyph_replay.started_us = now;
    if (glyph_replay.stage_us == 0) glyph_replay.stage_us = now;
    const Budget *budget = app->browser->budget;
    printf("tilefinch-glyph-stage: client=%s install=%s since-ms=%u "
           "total-ms=%u bytes=%llu/%llu status=%s budget=%zu peak=%zu\n",
           client_phase_name(client), install_phase_name(install),
           (unsigned) ((now - glyph_replay.stage_us) / 1000u),
           (unsigned) ((now - glyph_replay.started_us) / 1000u),
           (unsigned long long) session->client_snapshot.bytes_received,
           (unsigned long long) session->client_snapshot.bytes_total,
           tilefinch_update_status_name(session->client_snapshot.status),
           budget->current, budget->peak);
    /* The install route's check starts the download on the same frame, so
       this cannot wait for the AVAILABLE phase. */
    const TilefinchUpdateManifest *manifest =
        &session->client_snapshot.manifest;
    if (!glyph_replay.manifest_logged && manifest->asset[0] != '\0') {
        glyph_replay.manifest_logged = true;
        printf("tilefinch-glyph-stage: manifest tag=%s asset=%s sequence=%llu "
               "expires=%llu size=%llu\n", manifest->tag, manifest->asset,
               (unsigned long long) manifest->release_sequence,
               (unsigned long long) manifest->expires_unix,
               (unsigned long long) manifest->package_size);
    }
    glyph_replay.client_phase = client;
    glyph_replay.install_phase = install;
    glyph_replay.stage_us = now;
}
