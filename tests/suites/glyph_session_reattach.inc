/* A completed install re-attaches the selected language and emoji packs at
   once, from the menu as from the in-page offer, instead of leaving the
   embedded fallback until a restart. Compiled with the session source and
   TILEFINCH_GLYPH_SESSION_TEST_SEAM so unsigned fixture generations stand
   in for signed ones; resolving, installing, probing and attaching are the
   real code. Included by tests/test_glyph_component.c. */

#include "tilefinch/psp_glyph_component_session.h"

#include <errno.h>

static bool reattach_fixture_identity(const TilefinchInstallPaths *paths,
                                      TilefinchGlyphPack pack)
{
    char resolved[TILEFINCH_INSTALL_PATH_LIMIT];
    return tilefinch_glyph_component_resolve(paths, pack, resolved,
                                             sizeof(resolved));
}

static bool glyph_install_self_check_refused(
    void *opaque, TilefinchGlyphInstallFaultPoint point)
{
    (void) opaque;
    return point == TILEFINCH_GLYPH_INSTALL_FAULT_SELF_CHECK;
}

/* Installs one fixture generation through the session's own install pump,
   the path a menu download takes once its package is verified. */
static bool reattach_begin_through_session(
    PspGlyphComponentSession *session, const TilefinchInstallPaths *paths,
    TilefinchGlyphPack pack, char package[TILEFINCH_INSTALL_PATH_LIMIT],
    bool fail_check)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    char fixture[128];
    CHECK(spec != NULL && write_fixture(fixture, spec->id));
    uint8_t package_digest[32];
    uint64_t package_size = 0;
    CHECK(hash_file(fixture, package_digest, &package_size));
    CHECK(stage_fixture_download(
        paths->install_root, pack, fixture, package));
    TilefinchUpdateManifest manifest = {
        .package_format = TILEFINCH_UPDATE_PACKAGE_GLYPH,
        .package_size = package_size
    };
    memcpy(manifest.package_sha256, package_digest, 32);
    snprintf(manifest.asset, sizeof(manifest.asset), "%s", spec->pack_asset);
    uint8_t envelope[16] = {0};
    uint8_t manifest_digest[32];
    memset(manifest_digest, 0x6b, sizeof(manifest_digest));
    /* An install detaches the runtime before it promotes. */
    psp_glyph_component_session_detach_runtime(session, NULL);
    CHECK(session->attached_mask == 0 && session->provider == NULL);
    session->installer = tilefinch_glyph_component_install_create(
        session->budget, &(TilefinchGlyphComponentInstallOptions) {
            .envelope = envelope,
            .envelope_length = sizeof(envelope),
            .manifest = &manifest,
            .manifest_digest = manifest_digest,
            .install_root = paths->install_root,
            .pack = pack,
            .fault = fail_check ? glyph_install_self_check_refused : NULL
        });
    CHECK(session->installer != NULL);
    session->operation_pack = pack;
    session->operation_initialized = true;
    CHECK(tilefinch_glyph_component_install_snapshot(
        session->installer, &session->install_snapshot));
    return true;
}

static bool reattach_install_through_session(
    PspGlyphComponentSession *session, const TilefinchInstallPaths *paths,
    TilefinchGlyphPack pack)
{
    char package[TILEFINCH_INSTALL_PATH_LIMIT];
    CHECK(reattach_begin_through_session(session, paths, pack, package, false));
    for (size_t pump = 0; pump < 4096u && session->installer != NULL;
         pump++)
        CHECK(psp_glyph_component_session_pump_operation(
            session, paths, NULL));
    (void) remove(package);
    CHECK(session->installer == NULL && session->reattach_pending
          && psp_glyph_component_session_installed(session, pack));
    CHECK(psp_glyph_component_session_installed_at_least(session, pack, 1));
    CHECK(!psp_glyph_component_session_installed_at_least(session, pack, 2));
    CHECK(!psp_glyph_component_session_installed_at_least(
        session, TILEFINCH_GLYPH_PACK_COUNT, 1));
    CHECK(!psp_glyph_component_session_installed_at_least(
        session, (TilefinchGlyphPack) -1, 1));
    return true;
}

/* Stands in for the transfer a host test has no HTTPS origin for. */
static TilefinchUpdateClientPhase failure_test_phase;
static void failure_test_client(TilefinchUpdateClientSnapshot *snapshot)
{
    snapshot->phase = failure_test_phase;
    if (failure_test_phase == TILEFINCH_UPDATE_CLIENT_ERROR)
        snprintf(snapshot->message, sizeof(snapshot->message), "%s",
                 "SSL peer certificate was not OK");
}

static bool failure_test_message_is(PspGlyphComponentSession *session,
                                    const char *expected)
{
    const char *failure = psp_glyph_component_session_take_failure(session);
    return expected == NULL ? failure == NULL
        : failure != NULL && strcmp(failure, expected) == 0;
}

/* The folders a Cyrillic download creates; selecting the pack (the offer's
   size check) creates none of them. */
static bool remove_selected_cyrillic_directories(const char *root)
{
    static const char *const directories[] = {
        "components/glyph-cyrillic/candidate.tmp",
        "components/glyph-cyrillic",
        "components"
    };
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    for (size_t at = 0; at < sizeof(directories) / sizeof(directories[0]);
         at++) {
        snprintf(path, sizeof(path), "%s/%s", root, directories[at]);
        CHECK(rmdir(path) == 0 || errno == ENOENT);
    }
    return rmdir(root) == 0;
}

/* A failed check or download says why, whichever route started it, and says
   so once per failure rather than every frame. */
static bool test_glyph_session_client_failure_reported(void)
{
    char root[] = "/tmp/tilefinch-glyph-failure-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    snprintf(paths.data_dir, sizeof(paths.data_dir), "%s", root);
    /* Host builds may embed no root; nothing here verifies a signature. */
    PspGlyphComponentSession session = {.root_ready = true};
    const TilefinchGlyphPack pack = TILEFINCH_GLYPH_PACK_CYRILLIC;
    const char *const transport = "SSL peer certificate was not OK";
    psp_glyph_component_session_test_client = failure_test_client;

    CHECK(psp_glyph_component_session_select_operation(
        &session, &budget, &paths, pack));
    CHECK(failure_test_message_is(&session, NULL));
    failure_test_phase = TILEFINCH_UPDATE_CLIENT_CHECKING;
    CHECK(psp_glyph_component_session_pump_operation(&session, &paths, NULL));
    CHECK(failure_test_message_is(&session, NULL));

    failure_test_phase = TILEFINCH_UPDATE_CLIENT_ERROR;
    CHECK(psp_glyph_component_session_pump_operation(&session, &paths, NULL));
    CHECK(failure_test_message_is(&session, transport));
    CHECK(psp_glyph_component_session_pump_operation(&session, &paths, NULL));
    CHECK(failure_test_message_is(&session, NULL));

    /* X again, and the retry fails with the phase never leaving ERROR. */
    CHECK(psp_glyph_component_session_primary(&session, &paths, NULL)
          == PSP_GLYPH_COMPONENT_PRIMARY_CHECK_REQUIRED);
    CHECK(failure_test_message_is(&session, transport));

    /* A retry that was seen running arms the next failure too. */
    failure_test_phase = TILEFINCH_UPDATE_CLIENT_CHECKING;
    CHECK(psp_glyph_component_session_pump_operation(&session, &paths, NULL));
    CHECK(failure_test_message_is(&session, NULL));
    failure_test_phase = TILEFINCH_UPDATE_CLIENT_ERROR;
    CHECK(psp_glyph_component_session_pump_operation(&session, &paths, NULL));
    CHECK(failure_test_message_is(&session, transport));

    psp_glyph_component_session_destroy(&session);
    psp_glyph_component_session_test_client = NULL;
    CHECK(remove_selected_cyrillic_directories(root));
    CHECK(budget.current == 0);
    return true;
}

/* A verified download whose installer cannot start used to end with nothing
   on screen while every later frame retried the allocations and the open. It
   now ends the attempt, gives a reason, and X retries it. */
static bool test_glyph_session_install_refusal_reported(void)
{
    char root[] = "/tmp/tilefinch-glyph-refusal-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    snprintf(paths.data_dir, sizeof(paths.data_dir), "%s", root);
    PspGlyphComponentSession session = {.root_ready = true};
    psp_glyph_component_session_test_client = failure_test_client;
    /* This client has no verified envelope, so the installer refuses. */
    failure_test_phase = TILEFINCH_UPDATE_CLIENT_DOWNLOADED;

    CHECK(psp_glyph_component_session_select_operation(
        &session, &budget, &paths, TILEFINCH_GLYPH_PACK_CYRILLIC));
    session.auto_install = true;
    CHECK(psp_glyph_component_session_pump_operation(&session, &paths, NULL));
    CHECK(session.installer == NULL && !session.auto_install
          && session.install_refused);
    CHECK(!psp_glyph_component_session_active(&session));
    const char *const refused = "FONT PACK INSTALL COULD NOT START";
    CHECK(failure_test_message_is(&session, refused));
    CHECK(psp_glyph_component_session_pump_operation(&session, &paths, NULL));
    CHECK(failure_test_message_is(&session, NULL));

    CHECK(psp_glyph_component_session_primary(&session, &paths, NULL)
          == PSP_GLYPH_COMPONENT_PRIMARY_NONE);
    CHECK(failure_test_message_is(&session, refused));

    /* Reselecting starts over with nothing to report. */
    CHECK(psp_glyph_component_session_select_operation(
        &session, &budget, &paths, TILEFINCH_GLYPH_PACK_CYRILLIC));
    CHECK(!session.install_refused);
    CHECK(failure_test_message_is(&session, NULL));

    psp_glyph_component_session_destroy(&session);
    psp_glyph_component_session_test_client = NULL;
    CHECK(remove_selected_cyrillic_directories(root));
    CHECK(budget.current == 0);
    return true;
}

/* The generic "unavailable" notice hid which of two different problems the
   user has: memory on a heavy page, or a Memory Stick refusing the folder. */
static bool test_glyph_session_select_failure_reasons(void)
{
    char root[] = "/tmp/tilefinch-glyph-select-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s/blocked", root);
    snprintf(paths.data_dir, sizeof(paths.data_dir), "%s", root);
    FILE *blocker = fopen(paths.install_root, "wb");
    CHECK(blocker != NULL && fclose(blocker) == 0);
    PspGlyphComponentSession session = {.root_ready = true};
    const TilefinchGlyphPack pack = TILEFINCH_GLYPH_PACK_CYRILLIC;
    CHECK(psp_glyph_component_session_select_failure(&session) == NULL);
    /* IS-13: selecting (what the offer's size check does) writes nothing,
       so a card that refuses the folder is found when the download
       begins. */
    CHECK(psp_glyph_component_session_select_operation(
        &session, &budget, &paths, pack));
    psp_glyph_component_session_test_client = failure_test_client;
    failure_test_phase = TILEFINCH_UPDATE_CLIENT_AVAILABLE;
    CHECK(psp_glyph_component_session_primary(&session, &paths, NULL)
          == PSP_GLYPH_COMPONENT_PRIMARY_NONE);
    psp_glyph_component_session_test_client = NULL;
    CHECK(session.install_refused && !session.auto_install
          && failure_test_message_is(
                 &session, "FONT PACK FOLDER COULD NOT BE CREATED"));
    const char *reason = NULL;

    Budget tight;
    budget_init(&tight, 1024u);
    CHECK(remove(paths.install_root) == 0);
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    CHECK(!psp_glyph_component_session_select_operation(
        &session, &tight, &paths, pack));
    reason = psp_glyph_component_session_select_failure(&session);
    CHECK(reason != NULL
          && strcmp(reason, "NOT ENOUGH MEMORY FOR FONT PACK") == 0);
    CHECK(tight.current == 0);

    CHECK(psp_glyph_component_session_select_operation(
        &session, &budget, &paths, pack));
    CHECK(psp_glyph_component_session_select_failure(&session) == NULL);
    char component[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(component, sizeof(component), "%s/components", root);
    CHECK(access(component, F_OK) != 0);

    /* The download lands in the directory the installer stages in, and
       nothing goes in the data directory: PPSSPP's rename cannot move a file
       between directories, and the firmware's has never been shown to. */
    char expected[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(expected, sizeof(expected),
             "%s/components/glyph-cyrillic/candidate.tmp/glyph-component.part",
             root);
    CHECK(strcmp(session.package_path, expected) == 0);
    char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(cleanup, sizeof(cleanup), "%s/update", root);
    CHECK(access(cleanup, F_OK) != 0);

    psp_glyph_component_session_destroy(&session);
    CHECK(remove_selected_cyrillic_directories(root));
    CHECK(budget.current == 0);
    return true;
}

/* The line under the pack's name once an install completes. The color
   emoji pack is not a language pack and must not say it is (review 1021b
   F8); every line is a catalog key (tests/test_ui_toast_catalog.py). */
static bool test_glyph_session_installed_status(void)
{
    for (unsigned pack = 0; pack < TILEFINCH_GLYPH_PACK_COUNT; pack++) {
        bool emoji = pack == TILEFINCH_GLYPH_PACK_COLOR_EMOJI;
        const char *relaid = psp_glyph_component_installed_status(
            (TilefinchGlyphPack) pack, true);
        CHECK(strcmp(relaid, emoji ? "Installed" : "Language pack installed") == 0);
        CHECK(strcmp(psp_glyph_component_installed_status(
                  (TilefinchGlyphPack) pack, false),
              "Installed. Reload the page to redraw it") == 0);
    }
    return true;
}

static bool test_glyph_session_install_reattaches(void)
{
    char root[] = "/tmp/tilefinch-glyph-reattach-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    psp_glyph_component_session_test_identity = reattach_fixture_identity;
    static PspGlyphComponentSession session;
    memset(&session, 0, sizeof(session));
    const uint16_t cyrillic = 1u << TILEFINCH_GLYPH_PACK_CYRILLIC;
    const uint16_t hebrew = 1u << TILEFINCH_GLYPH_PACK_HEBREW;

    /* Boot: Cyrillic is the selected language and its pack attaches. */
    CHECK(install_test_generation(&budget, root,
                                  TILEFINCH_GLYPH_PACK_CYRILLIC, false));
    CHECK(psp_glyph_component_session_attach_selected(
              &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
              BROWSER_GLYPH_LANGUAGE_EMBEDDED, false)
          && session.attached_mask == cyrillic);

    /* A menu install of another pack detaches everything; finishing it
       puts the selected language back immediately. */
    CHECK(reattach_install_through_session(
        &session, &paths, TILEFINCH_GLYPH_PACK_HEBREW));
    CHECK(session.attached_mask == 0);
    CHECK(psp_glyph_component_session_reattach(
              &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
              BROWSER_GLYPH_LANGUAGE_EMBEDDED, false, 0, NULL)
          && session.attached_mask == cyrillic && session.provider != NULL);
    /* Only once per completed install. */
    CHECK(!psp_glyph_component_session_reattach(
        &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
        BROWSER_GLYPH_LANGUAGE_EMBEDDED, false,
        DOCUMENT_GLYPH_SCRIPT_HEBREW, NULL));

    /* The offer's install also attaches the new pack for the page's
       script, beside the selected one. */
    CHECK(reattach_install_through_session(
        &session, &paths, TILEFINCH_GLYPH_PACK_HEBREW));
    CHECK(psp_glyph_component_session_reattach(
              &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
              BROWSER_GLYPH_LANGUAGE_EMBEDDED, false,
              DOCUMENT_GLYPH_SCRIPT_HEBREW, NULL)
          && session.attached_mask == (cyrillic | hebrew));

    /* A non-Latin interface language attaches its pack beside the page
       selection instead of replacing it, at boot and after an install;
       one language serving both attaches once. */
    psp_glyph_component_session_destroy(&session);
    memset(&session, 0, sizeof(session));
    CHECK(psp_glyph_component_session_attach_selected(
              &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_HEBREW,
              BROWSER_GLYPH_LANGUAGE_CYRILLIC, false)
          && session.attached_mask == (cyrillic | hebrew)
          && tilefinch_glyph_provider_pack_count(session.provider) == 2u);
    CHECK(reattach_install_through_session(
        &session, &paths, TILEFINCH_GLYPH_PACK_HEBREW));
    CHECK(psp_glyph_component_session_reattach(
              &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_HEBREW,
              BROWSER_GLYPH_LANGUAGE_CYRILLIC, false, 0, NULL)
          && session.attached_mask == (cyrillic | hebrew));
    psp_glyph_component_session_destroy(&session);
    memset(&session, 0, sizeof(session));
    CHECK(psp_glyph_component_session_attach_selected(
              &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
              BROWSER_GLYPH_LANGUAGE_CYRILLIC, false)
          && session.attached_mask == cyrillic
          && tilefinch_glyph_provider_pack_count(session.provider) == 1u);
    psp_glyph_component_session_destroy(&session);
    memset(&session, 0, sizeof(session));
    CHECK(psp_glyph_component_session_attach_selected(
              &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_EMBEDDED,
              BROWSER_GLYPH_LANGUAGE_CYRILLIC, false)
          && session.attached_mask == cyrillic);

    psp_glyph_component_session_destroy(&session);
    psp_glyph_component_session_test_identity = NULL;
    static const TilefinchGlyphPack installed[] = {
        TILEFINCH_GLYPH_PACK_CYRILLIC, TILEFINCH_GLYPH_PACK_HEBREW
    };
    for (size_t at = 0; at < 2u; at++) {
        const TilefinchGlyphPackSpec *spec =
            tilefinch_glyph_pack_spec(installed[at]);
        CHECK(tilefinch_glyph_component_remove(&paths, installed[at]));
        char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
        snprintf(cleanup, sizeof(cleanup), "%s/components/%s/UNINSTALLED",
                 root, spec->id);
        CHECK(unlink(cleanup) == 0);
        snprintf(cleanup, sizeof(cleanup), "%s/components/%s", root,
                 spec->id);
        CHECK(rmdir(cleanup) == 0);
    }
    char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(cleanup, sizeof(cleanup), "%s/components", root);
    CHECK(rmdir(cleanup) == 0 && rmdir(root) == 0);
    CHECK(budget.current == 0);
    return true;
}

/* Exercise the session's real install pump at the failed/cancelled boundary,
   with an incumbent pack that must stay detached until mutation retires. */
static bool test_glyph_session_remove_retirement(bool fail_remove)
{
    char root[] = "/tmp/tilefinch-glyph-remove-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    psp_glyph_component_session_test_identity = reattach_fixture_identity;
    PspGlyphComponentSession session = {0};
    static const TilefinchGlyphPack packs[] = {
        TILEFINCH_GLYPH_PACK_CYRILLIC, TILEFINCH_GLYPH_PACK_HEBREW
    };
    for (size_t at = 0; at < 2u; at++)
        CHECK(install_test_generation(&budget, root, packs[at], false));
    CHECK(psp_glyph_component_session_attach_selected(
        &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
        BROWSER_GLYPH_LANGUAGE_HEBREW, false));
    uint16_t both = (1u << packs[0]) | (1u << packs[1]);
    CHECK(session.attached_mask == both && session.provider != NULL);

    char blocked[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(blocked, sizeof(blocked),
        "%s/components/%s/active/ACTIVATED", root,
        tilefinch_glyph_pack_spec(packs[0])->id);
    if (fail_remove) {
        /* A directory where unlink expects a file forces a real I/O refusal,
           including when tests run with elevated filesystem privileges. */
        CHECK(mkdir(blocked, 0777) == 0);
    }
    CHECK(psp_glyph_component_session_remove(
        &session, &paths, packs[0], NULL) == !fail_remove);
    if (fail_remove) CHECK(rmdir(blocked) == 0);
    CHECK(session.provider == NULL && session.attached_mask == 0);
    CHECK(session.reattach_pending);
    CHECK(psp_glyph_component_session_installed(&session, packs[0]) == fail_remove);
    CHECK(psp_glyph_component_session_installed(&session, packs[1]));
    CHECK(psp_glyph_component_session_reattach(
        &session, &budget, &paths,
        fail_remove ? BROWSER_GLYPH_LANGUAGE_CYRILLIC : BROWSER_GLYPH_LANGUAGE_EMBEDDED,
        BROWSER_GLYPH_LANGUAGE_HEBREW, false, 0, NULL));
    CHECK(!session.reattach_pending && session.provider != NULL
        && session.attached_mask == (fail_remove ? both : (1u << packs[1])));
    psp_glyph_component_session_destroy(&session);
    psp_glyph_component_session_test_identity = NULL;
    for (size_t at = 0; at < 2u; at++) {
        const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(packs[at]);
        if (fail_remove || at != 0)
            CHECK(tilefinch_glyph_component_remove(&paths, packs[at]));
        char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
        snprintf(cleanup, sizeof(cleanup), "%s/components/%s/UNINSTALLED", root, spec->id);
        CHECK(unlink(cleanup) == 0);
        snprintf(cleanup, sizeof(cleanup), "%s/components/%s", root, spec->id);
        CHECK(rmdir(cleanup) == 0);
    }
    char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(cleanup, sizeof(cleanup), "%s/components", root);
    CHECK(rmdir(cleanup) == 0 && rmdir(root) == 0 && budget.current == 0);
    return true;
}

static bool test_glyph_session_install_retirement(unsigned mode)
{
    char root[] = "/tmp/tilefinch-glyph-retirement-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    psp_glyph_component_session_test_identity = reattach_fixture_identity;
    PspGlyphComponentSession session = {0};
    const TilefinchGlyphPack pack = TILEFINCH_GLYPH_PACK_CYRILLIC;
    const uint16_t bit = 1u << pack;
    CHECK(install_test_generation(&budget, root, pack, false));
    CHECK(psp_glyph_component_session_attach_selected(
        &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
        BROWSER_GLYPH_LANGUAGE_EMBEDDED, false));
    CHECK(session.attached_mask == bit && session.provider != NULL);
    char package[TILEFINCH_INSTALL_PATH_LIMIT];
    CHECK(reattach_begin_through_session(
        &session, &paths, pack, package, mode == 2u));
    CHECK(psp_glyph_component_session_active(&session));
    if (mode == 0u) {
        CHECK(!psp_glyph_component_session_attach_hinted(
            &session, &paths, DOCUMENT_GLYPH_SCRIPT_CYRILLIC, NULL));
        CHECK(!psp_glyph_component_session_attach_selected(
            &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
            BROWSER_GLYPH_LANGUAGE_EMBEDDED, false));
        session.reattach_pending = true;
        CHECK(!psp_glyph_component_session_reattach(
            &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
            BROWSER_GLYPH_LANGUAGE_EMBEDDED, false, 0, NULL));
        CHECK(session.reattach_pending && session.provider == NULL);
    }
    if (mode != 2u) CHECK(psp_glyph_component_session_cancel(&session));
    for (unsigned pump = 0; pump < 4096u
         && psp_glyph_component_session_active(&session); pump++)
        CHECK(psp_glyph_component_session_pump_operation(&session, &paths, NULL));
    CHECK(session.install_snapshot.phase == (mode == 2u
          ? TILEFINCH_UPDATE_INSTALL_ERROR : TILEFINCH_UPDATE_INSTALL_CANCELLED));
    CHECK(session.installer != NULL && session.reattach_pending);
    /* A failure gives its reason, once; a cancellation the user asked for is
       not a failure. */
    const char *reason = psp_glyph_component_session_take_failure(&session);
    if (mode == 2u)
        CHECK(reason != NULL
              && strcmp(reason, "FONT PACK SELF-CHECK FAILED") == 0);
    else
        CHECK(reason == NULL);
    CHECK(psp_glyph_component_session_take_failure(&session) == NULL);
    CHECK(psp_glyph_component_session_reattach(
        &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
        BROWSER_GLYPH_LANGUAGE_EMBEDDED, false, 0, NULL));
    CHECK(session.attached_mask == bit && session.provider != NULL
          && !session.reattach_pending);
    /* Repeated terminal pumps cannot relatch restoration or erase the error. */
    CHECK(psp_glyph_component_session_pump_operation(&session, &paths, NULL));
    CHECK(!session.reattach_pending && session.attached_mask == bit);
    psp_glyph_component_session_destroy(&session);
    psp_glyph_component_session_test_identity = NULL;
    CHECK(remove(package) == 0);
    CHECK(tilefinch_glyph_component_remove(&paths, pack));
    char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(cleanup, sizeof(cleanup), "%s/components/glyph-cyrillic/UNINSTALLED", root);
    CHECK(unlink(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/components/glyph-cyrillic", root);
    CHECK(rmdir(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/components", root);
    CHECK(rmdir(cleanup) == 0 && rmdir(root) == 0);
    CHECK(budget.current == 0);
    return true;
}

/* A page in a script whose pack is installed but not selected attaches that
   pack while the page is open. Mode 0: another language is selected. Mode 1:
   no language is selected. Mode 2: boot saw unslotted paths (a PSPLink host0
   run) and slotted paths arrive later, as the validation replay seam gives
   them; boot's refusal must not leave the session unable to attach. */
static bool test_glyph_session_lazy_attach(unsigned mode)
{
    char root[] = "/tmp/tilefinch-glyph-lazy-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    TilefinchInstallPaths host0 = paths;
    host0.slotted = false;
    psp_glyph_component_session_test_identity = reattach_fixture_identity;
    static PspGlyphComponentSession session;
    memset(&session, 0, sizeof(session));
    const TilefinchGlyphPack packs[2] = {
        TILEFINCH_GLYPH_PACK_CYRILLIC, TILEFINCH_GLYPH_PACK_HEBREW};
    const uint16_t cyrillic = 1u << TILEFINCH_GLYPH_PACK_CYRILLIC;
    const uint16_t hebrew = 1u << TILEFINCH_GLYPH_PACK_HEBREW;
    for (size_t at = 0; at < 2u; at++)
        CHECK(install_test_generation(&budget, root, packs[at], false));

    BrowserGlyphLanguage selected = mode == 0u
        ? BROWSER_GLYPH_LANGUAGE_CYRILLIC : BROWSER_GLYPH_LANGUAGE_EMBEDDED;
    if (mode == 2u) {
        CHECK(!psp_glyph_component_session_attach_selected(
            &session, &budget, &host0, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
            BROWSER_GLYPH_LANGUAGE_EMBEDDED, false));
        CHECK(session.attached_mask == 0 && session.provider == NULL);
    } else {
        CHECK(psp_glyph_component_session_attach_selected(
            &session, &budget, &paths, selected,
            BROWSER_GLYPH_LANGUAGE_EMBEDDED, false));
        CHECK(session.attached_mask == (mode == 0u ? cyrillic : 0u));
    }

    CHECK(psp_glyph_component_session_attach_hinted(
        &session, &paths, DOCUMENT_GLYPH_SCRIPT_HEBREW, NULL));
    CHECK((session.attached_mask & hebrew) != 0 && session.provider != NULL);
    CHECK(session.attached_mask
          == (uint16_t) (hebrew | (mode == 0u ? cyrillic : 0u)));
    /* Once per script: a second hint for the same page does nothing. */
    CHECK(!psp_glyph_component_session_attach_hinted(
        &session, &paths, DOCUMENT_GLYPH_SCRIPT_HEBREW, NULL));

    psp_glyph_component_session_destroy(&session);
    psp_glyph_component_session_test_identity = NULL;
    for (size_t at = 0; at < 2u; at++) {
        const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(packs[at]);
        CHECK(tilefinch_glyph_component_remove(&paths, packs[at]));
        char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
        snprintf(cleanup, sizeof(cleanup), "%s/components/%s/UNINSTALLED", root, spec->id);
        CHECK(unlink(cleanup) == 0);
        snprintf(cleanup, sizeof(cleanup), "%s/components/%s", root, spec->id);
        CHECK(rmdir(cleanup) == 0);
    }
    char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(cleanup, sizeof(cleanup), "%s/components", root);
    CHECK(rmdir(cleanup) == 0 && rmdir(root) == 0 && budget.current == 0);
    return true;
}
