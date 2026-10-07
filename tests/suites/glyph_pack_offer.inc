/* In-page language-pack offer: the parser census, the catalog-keyed policy
   against the faces the PSP actually ships, the store's installed state,
   the profile's "Don't ask again" record, and once-per-site memory. Included
   by tests/test_glyph_component.c after its install fixture helpers. */

#include "tilefinch/document.h"
#include "tilefinch/glyph_pack_offer.h"

typedef struct {
    const FontFace *face;
    const TilefinchInstallPaths *paths;
} OfferProbe;

static bool offer_probe_has_glyph(void *opaque, unsigned codepoint)
{
    const OfferProbe *probe = opaque;
    return font_face_has_codepoint(probe->face, codepoint);
}

static bool offer_probe_installed(void *opaque, TilefinchGlyphPack pack)
{
    const OfferProbe *probe = opaque;
    char resolved[TILEFINCH_INSTALL_PATH_LIMIT];
    return tilefinch_glyph_component_resolve(
        probe->paths, pack, resolved, sizeof(resolved));
}

static const char offer_russian[] =
    "Привет! Это тестовая страница на русском языке. Мы проверяем, что "
    "браузер замечает кириллицу и предлагает установить языковой пакет, "
    "когда встроенные шрифты не содержат нужных знаков.";
static const char offer_english[] =
    "Tilefinch is a web browser for the PlayStation Portable. This "
    "paragraph is plain English text with nothing outside ASCII. ";

/* Parses <body><p>{prefix}{body x repeat}{suffix}</p> and returns its
   census. */
static bool offer_parse(Budget *budget, const char *prefix, const char *body,
                        unsigned repeat, const char *suffix,
                        DocumentGlyphCensus *census, uint16_t *script_mask)
{
    size_t capacity = 256u + strlen(prefix) + strlen(suffix)
        + strlen(body) * repeat;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = (size_t) snprintf(
        html, capacity, "<!doctype html><body><p>%s", prefix);
    for (unsigned at = 0; at < repeat; at++)
        used += (size_t) snprintf(html + used, capacity - used, "%s", body);
    used += (size_t) snprintf(html + used, capacity - used, "%s</p>",
                              suffix);
    PocDocument document = {0};
    CHECK(budget_install_lexbor(budget));
    bool parsed = document_parse(&document, budget, html, used, 4096u);
    free(html);
    if (parsed) {
        *census = document.glyph_census;
        if (script_mask != NULL) *script_mask = document.glyph_script_mask;
    }
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(budget) && parsed);
    return true;
}

static bool offer_for(const DocumentGlyphCensus *census, OfferProbe *probe,
                      uint16_t declined, TilefinchGlyphPack *pack)
{
    TilefinchGlyphOfferPage page = {
        .url = "https://ru.example.org/статья",
        .census = census,
        .declined_mask = declined,
        .has_glyph = offer_probe_has_glyph,
        .installed = offer_probe_installed,
        .opaque = probe
    };
    return tilefinch_glyph_offer_candidate(&page, pack);
}

static bool test_glyph_offer_detection(void)
{
    char root[] = "/tmp/tilefinch-glyph-offer-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    FontSet fonts;
    /* The face staged beside the PSP EBOOT, not the desktop lab's full
       DejaVuSans: the lab face has Cyrillic and would hide the trigger. */
    CHECK(font_set_load(&fonts, &budget, TILEFINCH_TEST_PSP_SANS_FONT,
                        NULL, NULL, NULL, NULL, NULL, NULL,
                        2u * 1024u * 1024u));
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    OfferProbe probe = {
        .face = font_set_face(&fonts, FONT_SANS), .paths = &paths
    };
    CHECK(probe.face != NULL
          && !font_face_has_codepoint(probe.face, 0x0436u)
          && font_face_has_codepoint(probe.face, 0x0142u));

    /* A Cyrillic page triggers the Cyrillic pack, keyed off the catalog. */
    DocumentGlyphCensus census;
    uint16_t scripts = 0;
    TilefinchGlyphPack pack = TILEFINCH_GLYPH_PACK_COUNT;
    CHECK(offer_parse(&budget, "", offer_russian, 1u, "", &census,
                      &scripts));
    if (census.offer_mask != DOCUMENT_GLYPH_SCRIPT_CYRILLIC)
        fprintf(stderr, "russian census offer=0x%04x scripts=0x%04x\n",
                (unsigned) census.offer_mask, (unsigned) scripts);
    CHECK(census.offer_mask == DOCUMENT_GLYPH_SCRIPT_CYRILLIC);
    CHECK(census.samples[3][0] == 0x041fu /* П */
          && census.samples[3][1] != 0 && census.samples[3][3] != 0);
    CHECK(offer_for(&census, &probe, 0, &pack)
          && pack == TILEFINCH_GLYPH_PACK_CYRILLIC);
    const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
    CHECK(spec != NULL && strcmp(spec->label, "Cyrillic") == 0
          && spec->approximate_kib == 1024u);

    /* A Latin page does not. */
    CHECK(offer_parse(&budget, "", offer_english, 4u, "", &census, &scripts));
    CHECK(census.offer_mask == 0 && scripts == 0
          && !offer_for(&census, &probe, 0, &pack));

    /* A single stray character does not: the lazy-attach hint still sees
       it, the offer census does not count it as use. */
    CHECK(offer_parse(&budget, "", offer_english, 4u, " Ж", &census,
                      &scripts));
    CHECK(scripts == DOCUMENT_GLYPH_SCRIPT_CYRILLIC
          && census.offer_mask == 0
          && !offer_for(&census, &probe, 0, &pack));

    /* Neither does a long article's language list: well over the absolute
       minimum, far under the share of the page's text. */
    CHECK(offer_parse(&budget, "",
                      offer_english, 40u,
                      " Русский Українська Беларуская Български Српски "
                      "Қазақша Татарча Кыргызча Монгол Македонски Тоҷикӣ",
                      &census, &scripts));
    CHECK(scripts == DOCUMENT_GLYPH_SCRIPT_CYRILLIC
          && census.offer_mask == 0);

    /* The trigger is the missing glyph, not the script range: Polish uses
       Latin Extended-A, which the embedded faces carry; Vietnamese needs
       Latin Extended Additional, which they do not. */
    CHECK(offer_parse(&budget, "", "Zażółć gęślą jaźń. ", 12u, "", &census,
                      NULL));
    CHECK(census.offer_mask == DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED
          && !offer_for(&census, &probe, 0, &pack));
    CHECK(offer_parse(&budget, "",
                      "Tiếng Việt là ngôn ngữ chính thức của Việt Nam. ", 8u,
                      "", &census, NULL));
    CHECK(census.offer_mask == DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED
          && offer_for(&census, &probe, 0, &pack)
          && pack == TILEFINCH_GLYPH_PACK_LATIN_EXTENDED);

    /* Offer language packs: Off suppresses every offer. */
    {
        TilefinchGlyphOfferPage off = {
            .url = "https://ru.example.org/",
            .census = &census,
            .offers_off = true,
            .has_glyph = offer_probe_has_glyph,
            .installed = offer_probe_installed,
            .opaque = &probe
        };
        TilefinchGlyphOfferSession quiet;
        memset(&quiet, 0, sizeof(quiet));
        CHECK(!tilefinch_glyph_offer_candidate(&off, &pack)
              && !tilefinch_glyph_offer_poll(&quiet, &off, &pack)
              && !quiet.page_evaluated);
        /* Turning it back on offers on that same page. */
        off.offers_off = false;
        CHECK(tilefinch_glyph_offer_poll(&quiet, &off, &pack)
              && pack == TILEFINCH_GLYPH_PACK_LATIN_EXTENDED);
    }

    /* "Don't ask again" is per pack. */
    CHECK(offer_parse(&budget, "", offer_russian, 1u, "", &census, NULL));
    CHECK(!offer_for(&census, &probe,
                     (uint16_t) (1u << TILEFINCH_GLYPH_PACK_CYRILLIC),
                     &pack));
    CHECK(offer_for(&census, &probe,
                    (uint16_t) (1u << TILEFINCH_GLYPH_PACK_ARABIC), &pack)
          && pack == TILEFINCH_GLYPH_PACK_CYRILLIC);

    /* An installed pack is not offered, even before it is attached. The
       generation goes through the same signed-store installer as the
       menu's download. */
    CHECK(install_test_generation(&budget, root,
                                  TILEFINCH_GLYPH_PACK_CYRILLIC, false));
    CHECK(offer_probe_installed(&probe, TILEFINCH_GLYPH_PACK_CYRILLIC)
          && !offer_for(&census, &probe, 0, &pack));
    CHECK(tilefinch_glyph_component_remove(
              &paths, TILEFINCH_GLYPH_PACK_CYRILLIC)
          && offer_for(&census, &probe, 0, &pack));

    char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(cleanup, sizeof(cleanup),
             "%s/components/glyph-cyrillic/UNINSTALLED", root);
    CHECK(unlink(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/components/glyph-cyrillic", root);
    CHECK(rmdir(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/components", root);
    CHECK(rmdir(cleanup) == 0 && rmdir(root) == 0);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    return true;
}

static bool test_glyph_offer_session_memory(void)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    FontSet fonts;
    CHECK(font_set_load(&fonts, &budget, TILEFINCH_TEST_PSP_SANS_FONT,
                        NULL, NULL, NULL, NULL, NULL, NULL,
                        2u * 1024u * 1024u));
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s",
             "/nonexistent/tilefinch-offer");
    OfferProbe probe = {
        .face = font_set_face(&fonts, FONT_SANS), .paths = &paths
    };
    DocumentGlyphCensus census;
    CHECK(offer_parse(&budget, "", offer_russian, 1u, "", &census, NULL));
    TilefinchGlyphOfferSession session;
    memset(&session, 0, sizeof(session));
    TilefinchGlyphOfferPage page = {
        .url = "https://news.example.ru/a",
        .census = &census,
        .has_glyph = offer_probe_has_glyph,
        .installed = offer_probe_installed,
        .opaque = &probe
    };
    TilefinchGlyphPack pack = TILEFINCH_GLYPH_PACK_COUNT;
    CHECK(tilefinch_glyph_offer_poll(&session, &page, &pack)
          && pack == TILEFINCH_GLYPH_PACK_CYRILLIC);
    /* The same page again, another page and another host of the same site
       are all quiet for the rest of the session. */
    CHECK(!tilefinch_glyph_offer_poll(&session, &page, &pack));
    page.url = "https://news.example.ru/b";
    CHECK(!tilefinch_glyph_offer_poll(&session, &page, &pack));
    page.url = "https://sport.example.ru/";
    CHECK(tilefinch_glyph_offer_site_shown(&session, page.url)
          && !tilefinch_glyph_offer_poll(&session, &page, &pack));
    /* A different site is offered once. */
    page.url = "https://other.example.org/";
    CHECK(tilefinch_glyph_offer_poll(&session, &page, &pack)
          && !tilefinch_glyph_offer_poll(&session, &page, &pack));
    /* An empty census never even hashes the URL or records a page. */
    DocumentGlyphCensus empty = {0};
    page.census = &empty;
    page.url = "https://third.example.net/";
    CHECK(!tilefinch_glyph_offer_poll(&session, &page, &pack)
          && !tilefinch_glyph_offer_site_shown(&session, page.url));
    /* The site memory is bounded and forgets the oldest. */
    page.census = &census;
    for (unsigned at = 0; at < TILEFINCH_GLYPH_OFFER_SITE_LIMIT; at++) {
        char url[64];
        snprintf(url, sizeof(url), "https://site%u-example.com/", at);
        page.url = url;
        CHECK(tilefinch_glyph_offer_poll(&session, &page, &pack));
    }
    CHECK(session.shown_count == TILEFINCH_GLYPH_OFFER_SITE_LIMIT
          && !tilefinch_glyph_offer_site_shown(
                 &session, "https://news.example.ru/"));
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    return true;
}

static bool test_glyph_offer_decline_persists(void)
{
    char root[] = "/tmp/tilefinch-glyph-offer-profile-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char path[256];
    snprintf(path, sizeof(path), "%s/profile.tsv", root);
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    BrowserProfile *profile = browser_profile_create(&budget);
    CHECK(profile != NULL
          && browser_profile_glyph_offer_declined_mask(profile) == 0);
    browser_profile_set_glyph_offer_declined_mask(
        profile, (uint16_t) ((1u << TILEFINCH_GLYPH_PACK_CYRILLIC)
                             | (1u << TILEFINCH_GLYPH_PACK_HEBREW)
                             | 0x8000u));
    CHECK(browser_profile_glyph_offer_declined_mask(profile)
          == ((1u << TILEFINCH_GLYPH_PACK_CYRILLIC)
              | (1u << TILEFINCH_GLYPH_PACK_HEBREW)));
    CHECK(browser_profile_save(profile, path));
    browser_profile_destroy(profile);
    BrowserProfile *reloaded = browser_profile_create(&budget);
    CHECK(reloaded != NULL && browser_profile_load(reloaded, path)
          && browser_profile_glyph_offer_declined_mask(reloaded)
                 == ((1u << TILEFINCH_GLYPH_PACK_CYRILLIC)
                     | (1u << TILEFINCH_GLYPH_PACK_HEBREW)));
    CHECK(browser_profile_glyph_offers_enabled(reloaded));
    /* Off persists beside the declines; Reset declined offers clears the
       mask and leaves the switch alone. */
    browser_profile_set_glyph_offers_enabled(reloaded, false);
    browser_profile_set_glyph_offer_declined_mask(reloaded, 0);
    CHECK(browser_profile_save(reloaded, path));
    browser_profile_destroy(reloaded);
    reloaded = browser_profile_create(&budget);
    CHECK(reloaded != NULL && browser_profile_load(reloaded, path)
          && browser_profile_glyph_offer_declined_mask(reloaded) == 0
          && !browser_profile_glyph_offers_enabled(reloaded));
    /* Back to the defaults: nothing to record, so no GLYPHASK line. */
    browser_profile_set_glyph_offers_enabled(reloaded, true);
    CHECK(browser_profile_save(reloaded, path));
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    char line[256];
    bool recorded = false;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strncmp(line, "GLYPHASK", 8) == 0) recorded = true;
    fclose(file);
    CHECK(!recorded);
    browser_profile_destroy(reloaded);
    char backup[272];
    snprintf(backup, sizeof(backup), "%s.bak", path);
    (void) unlink(backup);
    CHECK(unlink(path) == 0 && rmdir(root) == 0);
    return true;
}
