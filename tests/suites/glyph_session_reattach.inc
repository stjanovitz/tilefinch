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

/* Installs one fixture generation through the session's own install pump,
   the path a menu download takes once its package is verified. */
static bool reattach_install_through_session(
    PspGlyphComponentSession *session, const TilefinchInstallPaths *paths,
    TilefinchGlyphPack pack)
{
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    char package[128];
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
            .pack = pack
        });
    CHECK(session->installer != NULL);
    session->operation_pack = pack;
    session->operation_initialized = true;
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
              false)
          && session.attached_mask == cyrillic);

    /* A menu install of another pack detaches everything; finishing it
       puts the selected language back immediately. */
    CHECK(reattach_install_through_session(
        &session, &paths, TILEFINCH_GLYPH_PACK_HEBREW));
    CHECK(session.attached_mask == 0);
    CHECK(psp_glyph_component_session_reattach(
              &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
              false, 0, NULL)
          && session.attached_mask == cyrillic && session.provider != NULL);
    /* Only once per completed install. */
    CHECK(!psp_glyph_component_session_reattach(
        &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC, false,
        DOCUMENT_GLYPH_SCRIPT_HEBREW, NULL));

    /* The offer's install also attaches the new pack for the page's
       script, beside the selected one. */
    CHECK(reattach_install_through_session(
        &session, &paths, TILEFINCH_GLYPH_PACK_HEBREW));
    CHECK(psp_glyph_component_session_reattach(
              &session, &budget, &paths, BROWSER_GLYPH_LANGUAGE_CYRILLIC,
              false, DOCUMENT_GLYPH_SCRIPT_HEBREW, NULL)
          && session.attached_mask == (cyrillic | hebrew));

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
