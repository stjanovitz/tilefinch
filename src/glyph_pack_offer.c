#include "tilefinch/glyph_pack_offer.h"

#include <string.h>

#include "tilefinch/url.h"

#define OFFER_FNV_OFFSET UINT32_C(2166136261)
#define OFFER_FNV_PRIME UINT32_C(16777619)

static uint32_t offer_hash(const char *text)
{
    uint32_t value = OFFER_FNV_OFFSET;
    for (const unsigned char *at = (const unsigned char *) text;
         at != NULL && *at != '\0'; at++) {
        value ^= *at;
        value *= OFFER_FNV_PRIME;
    }
    return value;
}

static bool offer_site_hash(const char *url, uint32_t *hash)
{
    char site[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (url == NULL || hash == NULL
        || !tilefinch_url_site_key(url, site, sizeof(site))
        || site[0] == '\0') return false;
    *hash = offer_hash(site);
    return true;
}

/* At least half of the recorded page codepoints must be missing. A page
   whose sampled characters the faces can draw is readable already. */
static bool offer_script_missing(const TilefinchGlyphOfferPage *page,
                                 unsigned script_index)
{
    unsigned recorded = 0;
    unsigned missing = 0;
    for (unsigned at = 0; at < DOCUMENT_GLYPH_CENSUS_SAMPLES; at++) {
        uint32_t codepoint = page->census->samples[script_index][at];
        if (codepoint == 0) continue;
        recorded++;
        if (!page->has_glyph(page->opaque, (unsigned) codepoint)) missing++;
    }
    return missing != 0 && missing * 2u >= recorded;
}

bool tilefinch_glyph_offer_candidate(const TilefinchGlyphOfferPage *page,
                                     TilefinchGlyphPack *pack)
{
    if (page == NULL || page->offers_off || page->census == NULL
        || page->has_glyph == NULL || page->installed == NULL
        || pack == NULL) return false;
    uint16_t scripts = page->census->offer_mask;
    if (scripts == 0) return false;
    /* A script any installed pack already covers is not offered again,
       even when that pack is not attached right now (the lazy limit). */
    for (TilefinchGlyphPack at = 0; at < TILEFINCH_GLYPH_PACK_COUNT; at++) {
        const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(at);
        if (spec == NULL || (spec->page_scripts & scripts) == 0) continue;
        if (page->installed(page->opaque, at))
            scripts &= (uint16_t) ~spec->page_scripts;
    }
    for (unsigned index = 0; index < DOCUMENT_GLYPH_SCRIPT_KINDS; index++) {
        uint16_t script = (uint16_t) (1u << index);
        if ((scripts & script) == 0) continue;
        /* The first catalog pack that lists the script is its offer. A
           declined pack declines the script: Han text is offered
           Simplified Chinese, and "Don't ask again" does not roll on to
           Traditional Chinese for the same text. */
        TilefinchGlyphPack offered = TILEFINCH_GLYPH_PACK_COUNT;
        for (TilefinchGlyphPack at = 0; at < TILEFINCH_GLYPH_PACK_COUNT;
             at++) {
            const TilefinchGlyphPackSpec *spec =
                tilefinch_glyph_pack_spec(at);
            if (spec != NULL && (spec->page_scripts & script) != 0) {
                offered = at;
                break;
            }
        }
        if (offered == TILEFINCH_GLYPH_PACK_COUNT
            || (page->declined_mask & (1u << (unsigned) offered)) != 0
            || !offer_script_missing(page, index)) continue;
        *pack = offered;
        return true;
    }
    return false;
}

bool tilefinch_glyph_offer_site_shown(
    const TilefinchGlyphOfferSession *session, const char *url)
{
    uint32_t site = 0;
    if (session == NULL || !offer_site_hash(url, &site)) return false;
    for (unsigned at = 0; at < session->shown_count; at++) {
        if (session->shown_sites[at] == site) return true;
    }
    return false;
}

bool tilefinch_glyph_offer_poll(TilefinchGlyphOfferSession *session,
                                const TilefinchGlyphOfferPage *page,
                                TilefinchGlyphPack *pack)
{
    if (session == NULL || page == NULL || page->offers_off
        || page->census == NULL || page->census->offer_mask == 0
        || page->url == NULL || pack == NULL) return false;
    uint32_t page_hash = offer_hash(page->url);
    if (session->page_evaluated && session->page_url_hash == page_hash
        && session->page_offer_mask == page->census->offer_mask)
        return false;
    session->page_evaluated = true;
    session->page_url_hash = page_hash;
    session->page_offer_mask = page->census->offer_mask;
    uint32_t site = 0;
    if (!offer_site_hash(page->url, &site)
        || tilefinch_glyph_offer_site_shown(session, page->url)
        || !tilefinch_glyph_offer_candidate(page, pack)) return false;
    session->shown_sites[session->shown_next] = site;
    session->shown_next = (uint8_t) ((session->shown_next + 1u)
                                     % TILEFINCH_GLYPH_OFFER_SITE_LIMIT);
    if (session->shown_count < TILEFINCH_GLYPH_OFFER_SITE_LIMIT)
        session->shown_count++;
    return true;
}
