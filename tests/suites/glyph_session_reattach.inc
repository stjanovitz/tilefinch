/* A completed install re-attaches the selected language and emoji packs at
   once, from the menu as from the in-page offer, instead of leaving the
   embedded fallback until a restart. Compiled with the session source and
   TILEFINCH_GLYPH_SESSION_TEST_SEAM so unsigned fixture generations stand
   in for signed ones; resolving, installing, probing and attaching are the
   real code. Included by tests/test_glyph_component.c. */

#include "tilefinch/psp_glyph_component_session.h"

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
    TilefinchGlyphPack pack, char package[128], bool fail_check)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    CHECK(spec != NULL && write_fixture(package, spec->id));
    uint8_t package_digest[32];
    uint64_t package_size = 0;
    CHECK(hash_file(package, package_digest, &package_size));
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
            .package_path = package,
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
    char package[128];
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
    char package[128];
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
