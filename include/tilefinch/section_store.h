#ifndef TILEFINCH_SECTION_STORE_H
#define TILEFINCH_SECTION_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/remote_selector.h"

typedef struct {
    unsigned char *data;
    uint32_t source_offset;
    uint32_t source_length;
    uint32_t stored_length;
    bool compressed;
} SectionStoreBlock;

typedef struct {
    uint32_t source_offset;
    uint32_t source_length;
    uint32_t context_offset;
    uint16_t context_count;
    uint8_t heading_level;
    bool continuation;
} SectionStoreEntry;

typedef struct {
    uint32_t source_offset;
    uint16_t source_length;
} SectionStoreContextTag;

typedef struct {
    uint32_t hash;
    uint32_t value_offset;
    uint32_t tag_name_offset;
    uint16_t value_length;
    uint8_t tag_name_length;
} SectionStoreAnchor;

typedef struct {
    Budget *budget;
    size_t stored_length;
    SectionStoreBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    SectionStoreEntry *sections;
    size_t section_count;
    size_t section_capacity;
    /* Changes whenever externally visible section boundaries are replaced. */
    uint64_t index_revision;
    SectionStoreContextTag *context_tags;
    size_t context_tag_count;
    size_t context_tag_capacity;
    SectionStoreAnchor *anchors;
    size_t anchor_count;
    size_t anchor_capacity;
    uint64_t *selector_blooms;
    uint8_t *selector_states;
    size_t source_length;
    size_t prefix_length;
    size_t block_bytes;
    size_t maximum_section_bytes;
    size_t compressed_blocks;
    size_t raw_blocks;
    uint64_t compression_us;
    uint64_t index_us;
    uint64_t progress_callback_us;
    uint64_t selector_bloom_us;
    uint64_t anchor_sort_us;
    uint64_t index_max_slice_us;
    uint64_t first_section_ready_us;
    uint64_t selector_index_max_slice_us;
    size_t index_work_units;
    size_t index_cooperative_yields;
    size_t selector_index_work_units;
    size_t selector_index_cooperative_yields;
    /* During network streaming these are the only section entries safe to
       materialize before EOF. The completed index replaces the provisional
       entries transactionally and then seals every final section. */
    size_t sealed_section_count;
    size_t sealed_source_bytes;
    bool stream_complete;
} CompressedSectionStore;

/* Called only at safe compressed-block boundaries while the derived index is
   being built. Returning false cancels the transaction. Sections already in
   the store are complete and may be materialized for a provisional paint;
   final is true only after structural indexing and anchor sorting complete. */
typedef bool (*SectionStoreProgressCallback)(
    void *opaque, const CompressedSectionStore *store,
    size_t indexed_source_bytes, bool final);

typedef struct {
    CompressedSectionStore *store;
    unsigned char *pending;
    size_t pending_length;
    unsigned char *temporary;
    size_t temporary_capacity;
    uint16_t *positions;
    SectionStoreProgressCallback on_progress;
    void *progress_opaque;
    uint64_t started_ns;
    bool active;
} SectionStoreStreamBuilder;

bool section_store_stream_begin(SectionStoreStreamBuilder *builder,
                                CompressedSectionStore *store,
                                Budget *budget, size_t block_bytes,
                                size_t maximum_section_bytes);
bool section_store_stream_append(SectionStoreStreamBuilder *builder,
                                 const unsigned char *data, size_t length);
/* Seal one lexically self-contained body range after its bytes have been
   appended. This flushes at most one partial compression block. */
bool section_store_stream_seal_section(
    SectionStoreStreamBuilder *builder, size_t prefix_length,
    size_t source_start, size_t source_end);
void section_store_stream_set_progress(
    SectionStoreStreamBuilder *builder,
    SectionStoreProgressCallback callback, void *opaque);
bool section_store_stream_finish(SectionStoreStreamBuilder *builder);
void section_store_stream_abort(SectionStoreStreamBuilder *builder);

/* Experimental static-document store. The source is divided into independently
   decodable blocks and indexed at generic HTML heading boundaries. Long spans
   are additionally capped so selecting one section has bounded working memory. */
bool section_store_build(CompressedSectionStore *store, Budget *budget,
                         const char *html, size_t length,
                         size_t block_bytes, size_t maximum_section_bytes);

/* Creates a standalone, parser-ready HTML fragment containing the original
   document prefix and one indexed body section. The caller owns *html. */
bool section_store_extract_html(const CompressedSectionStore *store,
                                size_t section_index, char **html,
                                size_t *length);
/* Reconstructs the exact source for a safe adaptive full-document fallback. */
bool section_store_extract_source(const CompressedSectionStore *store,
                                  char **html, size_t *length);
/* Temporary semantic-query view. Marker comments bracket only the selected
   source interval; they are never exposed to the live document. */
bool section_store_extract_query_html(const CompressedSectionStore *store,
                                      size_t section_index, char **html,
                                      size_t *length);

bool section_store_find_anchor(const CompressedSectionStore *store,
                               const char *identifier, size_t length,
                               size_t *section_index);
bool section_store_find_anchor_element(const CompressedSectionStore *store,
                                       const char *identifier, size_t length,
                                       size_t *section_index,
                                       char tag_name[32]);
/* Finds the first source-order match for a bounded single-compound selector.
   Supported compounds combine a tag or '*', IDs, classes, and presence or
   exact-value attribute selectors. Combinators, lists, pseudos, and escapes
   deliberately fall back to the materialized DOM only. */
bool section_store_find_simple_selector(const CompressedSectionStore *store,
                                        const char *selector, size_t length,
                                        size_t *section_index,
                                        char tag_name[32],
                                        char identifier[129]);
bool section_store_selector_is_simple(const char *selector, size_t length);
/* Uses the per-section token Bloom index only as a conservative rejection
   test. A true result is a candidate, not proof of a selector match. */
bool section_store_simple_selector_maybe_matches_section(
    const CompressedSectionStore *store, size_t section_index,
    const char *selector, size_t length);
/* Collects a complete, bounded source-order result for the same selector
   subset.  It returns false rather than truncating when the result exceeds
   capacity or anonymous matches cannot be represented with stable identity. */
bool section_store_collect_simple_selector(
    const CompressedSectionStore *store, const char *selector, size_t length,
    TilefinchRemoteSelectorMatch *matches, size_t capacity, size_t *match_count);
/* Rebuilds only the derived section/anchor/query index while retaining the
   compressed source blocks. Selectors are active author rules known to create
   flex/grid/table containers (for example after external CSS has loaded). */
bool section_store_reindex_layout_selectors(
    CompressedSectionStore *store, const char *const *selectors,
    size_t selector_count);

void section_store_destroy(CompressedSectionStore *store);

#endif
