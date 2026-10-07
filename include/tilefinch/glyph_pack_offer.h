#ifndef TILEFINCH_GLYPH_PACK_OFFER_H
#define TILEFINCH_GLYPH_PACK_OFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "tilefinch/document.h"
#include "tilefinch/glyph_component_store.h"

/*
 * Whether a page should be offered a language pack, and which one.
 *
 * The page side is the parser census (DocumentGlyphCensus): which scripts
 * the visible text uses meaningfully, with a few of the page's own
 * codepoints per script. A pack is offered for a script when the catalog
 * lists that pack for it, no pack for it is installed, the user has not
 * said "Don't ask again" for it, and the active faces really lack most of
 * the sampled codepoints. The last test is what keeps a Polish page (Latin
 * Extended-A, which the embedded faces carry) from being offered the
 * Extended Latin pack that a Vietnamese page needs.
 *
 * Nothing here touches storage or fonts itself; the caller supplies both
 * predicates, so the policy is host-testable and the PSP keeps its
 * Memory Stick probe behind the once-per-page gate below.
 */

#define TILEFINCH_GLYPH_OFFER_SITE_LIMIT 16u

typedef bool (*TilefinchGlyphOfferHasGlyph)(void *opaque, unsigned codepoint);
typedef bool (*TilefinchGlyphOfferInstalled)(void *opaque,
                                             TilefinchGlyphPack pack);

typedef struct {
    const char *url;
    const DocumentGlyphCensus *census;
    /* Packs the user answered "Don't ask again" for (profile). */
    uint16_t declined_mask;
    /* Offer language packs: Off (profile). Nothing is offered. */
    bool offers_off;
    TilefinchGlyphOfferHasGlyph has_glyph;
    TilefinchGlyphOfferInstalled installed;
    void *opaque;
} TilefinchGlyphOfferPage;

/* Session memory: sites already offered (hashed schemeful sites, oldest
   overwritten past the limit) and the last page identity evaluated, so a
   settled page costs one hash compare per frame instead of a re-probe. */
typedef struct {
    uint32_t shown_sites[TILEFINCH_GLYPH_OFFER_SITE_LIMIT];
    uint8_t shown_count;
    uint8_t shown_next;
    bool page_evaluated;
    uint16_t page_offer_mask;
    uint32_t page_url_hash;
} TilefinchGlyphOfferSession;

/* Pure policy: the pack this page qualifies for, ignoring session memory. */
bool tilefinch_glyph_offer_candidate(const TilefinchGlyphOfferPage *page,
                                     TilefinchGlyphPack *pack);

/* At most once per page identity (URL and census) and once per site per
   session. A true result records the site as offered; the caller must show
   the offer. An empty census returns before any hashing. */
bool tilefinch_glyph_offer_poll(TilefinchGlyphOfferSession *session,
                                const TilefinchGlyphOfferPage *page,
                                TilefinchGlyphPack *pack);

bool tilefinch_glyph_offer_site_shown(
    const TilefinchGlyphOfferSession *session, const char *url);

#endif
