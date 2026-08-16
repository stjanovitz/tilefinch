#include "tilefinch/glyph_component.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define GLYPH_FILE_HEADER_BYTES 80u
#define GLYPH_PAGE_MISSING UINT16_C(0xffff)
#define GLYPH_CACHE_SLOTS 4u
#define GLYPH_QUEUE_LIMIT 32u
#define GLYPH_KEY_FLAG UINT32_C(0x80000000)
#define GLYPH_KEY_PACK_SHIFT 27u
#define GLYPH_KEY_PACK_MASK UINT32_C(0x18000000)
#define GLYPH_KEY_INDEX_MASK UINT32_C(0x07ffffff)
#define GLYPH_MONO_STRIDE 32u
#define GLYPH_COLOR_PIXEL_BYTES 3u
#define GLYPH_MAX_SOURCE_SIDE 24u
#define GLYPH_MAX_BLOCK_BYTES \
    (4u * GLYPH_MAX_SOURCE_SIDE * GLYPH_MAX_SOURCE_SIDE \
     * GLYPH_COLOR_PIXEL_BYTES)

typedef struct {
    uint32_t first_glyph;
    uint8_t present[32];
} GlyphPage;

typedef struct {
    uint8_t length;
    uint32_t glyph_index;
    uint32_t codepoints[TILEFINCH_GLYPH_COMPONENT_SEQUENCE_CODEPOINT_LIMIT];
} GlyphSequence;

typedef struct {
    FILE *file;
    uint16_t *page_directory;
    GlyphPage *pages;
    GlyphSequence *sequences;
    uint32_t glyph_count;
    uint32_t payload_offset;
    uint16_t populated_pages;
    uint16_t sequence_count;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t block_glyphs;
    uint16_t glyph_stride;
    TilefinchGlyphComponentKind kind;
    char id[TILEFINCH_GLYPH_COMPONENT_ID_LIMIT + 1u];
} GlyphPack;

typedef struct {
    uint8_t pack;
    uint32_t block;
} GlyphBlockKey;

typedef struct {
    GlyphBlockKey key;
    size_t bytes;
    bool valid;
    uint8_t data[GLYPH_MAX_BLOCK_BYTES];
} GlyphBlockCache;

struct TilefinchGlyphProvider {
    Budget *budget;
    GlyphPack packs[TILEFINCH_GLYPH_COMPONENT_PACK_LIMIT];
    GlyphBlockCache cache[GLYPH_CACHE_SLOTS];
    GlyphBlockKey queue[GLYPH_QUEUE_LIMIT];
    size_t pack_count;
    size_t queue_count;
};

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t) ((uint16_t) bytes[0] << 8 | bytes[1]);
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t) bytes[0] << 24 | (uint32_t) bytes[1] << 16
        | (uint32_t) bytes[2] << 8 | bytes[3];
}

static bool read_exact(FILE *file, uint64_t offset, void *output, size_t bytes)
{
    if (file == NULL || output == NULL || bytes == 0 || offset > LONG_MAX)
        return false;
    return fseek(file, (long) offset, SEEK_SET) == 0
        && fread(output, 1, bytes, file) == bytes;
}

static bool file_length(FILE *file, uint64_t *length)
{
    if (file == NULL || length == NULL || fseek(file, 0, SEEK_END) != 0)
        return false;
    long end = ftell(file);
    if (end < 0) return false;
    *length = (uint64_t) end;
    return true;
}

static bool component_id_valid(const char *id, size_t length)
{
    if (id == NULL || length == 0
        || length > TILEFINCH_GLYPH_COMPONENT_ID_LIMIT) return false;
    for (size_t at = 0; at < length; at++) {
        unsigned value = (unsigned char) id[at];
        if (!((value >= 'a' && value <= 'z')
              || (value >= '0' && value <= '9') || value == '-')) return false;
    }
    return true;
}

static void pack_destroy(Budget *budget, GlyphPack *pack)
{
    if (pack == NULL) return;
    if (pack->file != NULL) fclose(pack->file);
    budget_free(budget, pack->page_directory);
    budget_free(budget, pack->pages);
    budget_free(budget, pack->sequences);
    memset(pack, 0, sizeof(*pack));
}

TilefinchGlyphProvider *tilefinch_glyph_provider_create(Budget *budget)
{
    if (budget == NULL) return NULL;
    TilefinchGlyphProvider *provider = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*provider));
    if (provider != NULL) provider->budget = budget;
    return provider;
}

void tilefinch_glyph_provider_destroy(TilefinchGlyphProvider *provider)
{
    if (provider == NULL) return;
    Budget *budget = provider->budget;
    for (size_t at = 0; at < provider->pack_count; at++)
        pack_destroy(budget, &provider->packs[at]);
    budget_free(budget, provider);
}

static bool pack_read_index(
    TilefinchGlyphProvider *provider, GlyphPack *pack, const char *path,
    const char *expected_id)
{
    uint8_t header[GLYPH_FILE_HEADER_BYTES];
    FILE *file = fopen(path, "rb");
    if (file == NULL || !read_exact(file, 0, header, sizeof(header))) {
        if (file != NULL) fclose(file);
        return false;
    }
    static const uint8_t magic[8] = {'T','F','G','F','v','1',0,0};
    uint16_t schema = read_u16(header + 8);
    unsigned kind = header[10], width = header[11], height = header[12];
    unsigned block_glyphs = header[13], id_length = header[14];
    uint32_t glyph_count = read_u32(header + 16);
    uint16_t pages = read_u16(header + 20);
    uint16_t sequences = read_u16(header + 22);
    uint32_t directory_offset = read_u32(header + 24);
    uint32_t pages_offset = read_u32(header + 28);
    uint32_t sequences_offset = read_u32(header + 32);
    uint32_t payload_offset = read_u32(header + 36);
    uint32_t file_size = read_u32(header + 40);
    const char *id = (const char *) header + 44;
    uint32_t notice_length = read_u32(header + 72);
    uint64_t actual_file_size = 0;
    bool mono = kind == TILEFINCH_GLYPH_COMPONENT_MONO;
    bool color = kind == TILEFINCH_GLYPH_COMPONENT_COLOR;
    uint32_t stride = mono ? GLYPH_MONO_STRIDE
        : width * height * GLYPH_COLOR_PIXEL_BYTES;
    uint64_t directory_bytes = TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT * 2u;
    uint64_t pages_bytes = (uint64_t) pages * 36u;
    uint64_t sequence_bytes = (uint64_t) sequences * 40u;
    uint64_t payload_bytes = (uint64_t) glyph_count * stride;
    if (memcmp(header, magic, sizeof(magic)) != 0 || schema != 1
        || (!mono && !color) || width == 0 || height == 0
        || width > GLYPH_MAX_SOURCE_SIDE || height > GLYPH_MAX_SOURCE_SIDE
        || (mono && (width > 16u || height != 16u))
        || block_glyphs == 0 || block_glyphs > 64u
        || (uint64_t) block_glyphs * stride > GLYPH_MAX_BLOCK_BYTES
        || glyph_count == 0
        || glyph_count > TILEFINCH_GLYPH_COMPONENT_GLYPH_LIMIT
        || pages == 0 || pages > TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT
        || sequences > TILEFINCH_GLYPH_COMPONENT_SEQUENCE_LIMIT
        || !component_id_valid(id, id_length)
        || id[id_length] != '\0' || expected_id == NULL
        || strcmp(id, expected_id) != 0
        || notice_length > TILEFINCH_GLYPH_COMPONENT_NOTICE_LIMIT
        || !file_length(file, &actual_file_size)
        || actual_file_size != file_size
        || directory_offset != GLYPH_FILE_HEADER_BYTES
        || pages_offset != directory_offset + directory_bytes
        || sequences_offset != pages_offset + pages_bytes
        || payload_offset != sequences_offset + sequence_bytes
        || (uint64_t) payload_offset + payload_bytes + notice_length
               != file_size) {
        fclose(file);
        return false;
    }
    uint8_t *directory_bytes_raw = budget_malloc_category(
        provider->budget, BUDGET_CATEGORY_RESOURCE, (size_t) directory_bytes);
    uint8_t *page_bytes_raw = budget_malloc_category(
        provider->budget, BUDGET_CATEGORY_RESOURCE, (size_t) pages_bytes);
    uint8_t *sequence_bytes_raw = sequences == 0 ? NULL
        : budget_malloc_category(
              provider->budget, BUDGET_CATEGORY_RESOURCE,
              (size_t) sequence_bytes);
    uint16_t *directory = budget_malloc_category(
        provider->budget, BUDGET_CATEGORY_RESOURCE,
        TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT * sizeof(*directory));
    GlyphPage *page_table = budget_calloc_category(
        provider->budget, BUDGET_CATEGORY_RESOURCE, pages, sizeof(*page_table));
    GlyphSequence *sequence_table = sequences == 0 ? NULL
        : budget_calloc_category(
              provider->budget, BUDGET_CATEGORY_RESOURCE,
              sequences, sizeof(*sequence_table));
    bool allocated = directory_bytes_raw != NULL && page_bytes_raw != NULL
        && directory != NULL && page_table != NULL
        && (sequences == 0
            || (sequence_bytes_raw != NULL && sequence_table != NULL));
    bool read = allocated
        && read_exact(file, directory_offset, directory_bytes_raw,
                      (size_t) directory_bytes)
        && read_exact(file, pages_offset, page_bytes_raw, (size_t) pages_bytes)
        && (sequences == 0
            || read_exact(file, sequences_offset, sequence_bytes_raw,
                          (size_t) sequence_bytes));
    if (!read) {
        budget_free(provider->budget, directory_bytes_raw);
        budget_free(provider->budget, page_bytes_raw);
        budget_free(provider->budget, sequence_bytes_raw);
        budget_free(provider->budget, directory);
        budget_free(provider->budget, page_table);
        budget_free(provider->budget, sequence_table);
        fclose(file);
        return false;
    }
    for (size_t at = 0; at < TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT; at++) {
        directory[at] = read_u16(directory_bytes_raw + at * 2u);
        if (directory[at] != GLYPH_PAGE_MISSING && directory[at] >= pages) {
            allocated = false;
            break;
        }
    }
    uint32_t previous_end = 0;
    for (size_t at = 0; allocated && at < pages; at++) {
        const uint8_t *record = page_bytes_raw + at * 36u;
        page_table[at].first_glyph = read_u32(record);
        memcpy(page_table[at].present, record + 4u, 32u);
        unsigned count = 0;
        for (size_t byte = 0; byte < 32u; byte++) {
            uint8_t bits = page_table[at].present[byte];
            for (; bits != 0; bits &= (uint8_t) (bits - 1u)) count++;
        }
        if (page_table[at].first_glyph != previous_end
            || count > glyph_count - previous_end) allocated = false;
        previous_end += count;
    }
    uint32_t previous_sequence[8] = {0};
    uint8_t previous_length = 0;
    for (size_t at = 0; allocated && at < sequences; at++) {
        const uint8_t *record = sequence_bytes_raw + at * 40u;
        GlyphSequence *sequence = &sequence_table[at];
        sequence->length = record[0];
        sequence->glyph_index = read_u32(record + 4u);
        if (sequence->length < 2u
            || sequence->length
                   > TILEFINCH_GLYPH_COMPONENT_SEQUENCE_CODEPOINT_LIMIT
            || sequence->glyph_index >= glyph_count) {
            allocated = false;
            break;
        }
        for (size_t cp = 0; cp < 8u; cp++)
            sequence->codepoints[cp] = read_u32(record + 8u + cp * 4u);
        if (at != 0) {
            size_t shared = previous_length < sequence->length
                ? previous_length : sequence->length;
            int order = 0;
            for (size_t cp = 0; cp < shared && order == 0; cp++)
                order = previous_sequence[cp] < sequence->codepoints[cp] ? -1
                    : previous_sequence[cp] > sequence->codepoints[cp] ? 1 : 0;
            if (order == 0)
                order = previous_length < sequence->length ? -1
                    : previous_length > sequence->length ? 1 : 0;
            if (order >= 0) allocated = false;
        }
        memcpy(previous_sequence, sequence->codepoints,
               sizeof(previous_sequence));
        previous_length = sequence->length;
    }
    if (previous_end > glyph_count) allocated = false;
    budget_free(provider->budget, directory_bytes_raw);
    budget_free(provider->budget, page_bytes_raw);
    budget_free(provider->budget, sequence_bytes_raw);
    if (!allocated) {
        budget_free(provider->budget, directory);
        budget_free(provider->budget, page_table);
        budget_free(provider->budget, sequence_table);
        fclose(file);
        return false;
    }
    *pack = (GlyphPack) {
        .file = file,
        .page_directory = directory,
        .pages = page_table,
        .sequences = sequence_table,
        .glyph_count = glyph_count,
        .payload_offset = payload_offset,
        .populated_pages = pages,
        .sequence_count = sequences,
        .source_width = (uint16_t) width,
        .source_height = (uint16_t) height,
        .block_glyphs = (uint16_t) block_glyphs,
        .glyph_stride = (uint16_t) stride,
        .kind = (TilefinchGlyphComponentKind) kind
    };
    memcpy(pack->id, id, id_length + 1u);
    return true;
}

bool tilefinch_glyph_provider_attach(
    TilefinchGlyphProvider *provider, const char *path,
    const char *expected_component_id)
{
    if (provider == NULL || path == NULL || expected_component_id == NULL
        || provider->pack_count >= TILEFINCH_GLYPH_COMPONENT_PACK_LIMIT)
        return false;
    for (size_t at = 0; at < provider->pack_count; at++)
        if (strcmp(provider->packs[at].id, expected_component_id) == 0)
            return false;
    GlyphPack parsed = {0};
    if (!pack_read_index(
            provider, &parsed, path, expected_component_id)) return false;
    provider->packs[provider->pack_count++] = parsed;
    return true;
}

static bool pack_lookup(
    const GlyphPack *pack, unsigned codepoint, uint32_t *glyph_index)
{
    if (pack == NULL || codepoint > 0x10ffffu) return false;
    unsigned page = codepoint >> 8u, within = codepoint & 255u;
    uint16_t record = pack->page_directory[page];
    if (record == GLYPH_PAGE_MISSING) return false;
    const GlyphPage *entry = &pack->pages[record];
    uint8_t mask = (uint8_t) (1u << (within & 7u));
    if ((entry->present[within >> 3u] & mask) == 0) return false;
    uint32_t rank = 0;
    for (unsigned byte = 0; byte < (within >> 3u); byte++) {
        uint8_t bits = entry->present[byte];
        for (; bits != 0; bits &= (uint8_t) (bits - 1u)) rank++;
    }
    uint8_t bits = (uint8_t) (entry->present[within >> 3u] & (mask - 1u));
    for (; bits != 0; bits &= (uint8_t) (bits - 1u)) rank++;
    if (rank > pack->glyph_count - entry->first_glyph) return false;
    *glyph_index = entry->first_glyph + rank;
    return *glyph_index < pack->glyph_count;
}

static uint32_t make_key(size_t pack, uint32_t glyph)
{
    return GLYPH_KEY_FLAG | ((uint32_t) pack << GLYPH_KEY_PACK_SHIFT) | glyph;
}

static bool decode_key(
    const TilefinchGlyphProvider *provider, uint32_t key, size_t *pack,
    uint32_t *glyph)
{
    if (provider == NULL || (key & GLYPH_KEY_FLAG) == 0) return false;
    size_t decoded_pack = (key & GLYPH_KEY_PACK_MASK) >> GLYPH_KEY_PACK_SHIFT;
    uint32_t decoded_glyph = key & GLYPH_KEY_INDEX_MASK;
    if (decoded_pack >= provider->pack_count
        || decoded_glyph >= provider->packs[decoded_pack].glyph_count)
        return false;
    *pack = decoded_pack;
    *glyph = decoded_glyph;
    return true;
}

bool tilefinch_glyph_provider_has_codepoint(
    TilefinchGlyphProvider *provider, unsigned codepoint,
    uint32_t *glyph_key, unsigned *source_width)
{
    if (provider == NULL) return false;
    for (size_t pack = 0; pack < provider->pack_count; pack++) {
        uint32_t glyph = 0;
        if (!pack_lookup(&provider->packs[pack], codepoint, &glyph)) continue;
        if (glyph_key != NULL) *glyph_key = make_key(pack, glyph);
        if (source_width != NULL)
            *source_width = provider->packs[pack].source_width;
        return true;
    }
    return false;
}

bool tilefinch_glyph_provider_metrics(
    const TilefinchGlyphProvider *provider, uint32_t glyph_key,
    unsigned *source_width, unsigned *source_height,
    TilefinchGlyphComponentKind *kind)
{
    size_t pack = 0;
    uint32_t glyph = 0;
    if (!decode_key(provider, glyph_key, &pack, &glyph)) return false;
    (void) glyph;
    const GlyphPack *found = &provider->packs[pack];
    if (source_width != NULL) *source_width = found->source_width;
    if (source_height != NULL) *source_height = found->source_height;
    if (kind != NULL) *kind = found->kind;
    return true;
}

static size_t utf8_next(const char *text, size_t length, uint32_t *value)
{
    if (text == NULL || length == 0 || value == NULL) return 0;
    const uint8_t *bytes = (const uint8_t *) text;
    if (bytes[0] < 0x80u) { *value = bytes[0]; return 1; }
    size_t count = bytes[0] >= 0xf0u ? 4u : bytes[0] >= 0xe0u ? 3u : 2u;
    if (count > length || bytes[0] < 0xc2u || bytes[0] > 0xf4u) {
        *value = 0xfffdu; return 1;
    }
    uint32_t parsed = bytes[0] & (0x7fu >> count);
    for (size_t at = 1; at < count; at++) {
        if ((bytes[at] & 0xc0u) != 0x80u) { *value = 0xfffdu; return 1; }
        parsed = parsed << 6u | (bytes[at] & 0x3fu);
    }
    uint32_t minimum = count == 2u ? 0x80u : count == 3u ? 0x800u : 0x10000u;
    if (parsed < minimum || parsed > 0x10ffffu
        || (parsed >= 0xd800u && parsed <= 0xdfffu)) {
        *value = 0xfffdu; return 1;
    }
    *value = parsed;
    return count;
}

static int sequence_compare_prefix(
    const GlyphSequence *sequence, const uint32_t codepoints[8], size_t count)
{
    size_t shared = sequence->length < count ? sequence->length : count;
    for (size_t at = 0; at < shared; at++) {
        if (sequence->codepoints[at] < codepoints[at]) return -1;
        if (sequence->codepoints[at] > codepoints[at]) return 1;
    }
    return sequence->length < count ? -1 : sequence->length > count ? 1 : 0;
}

static bool pack_match_sequence(
    const GlyphPack *pack, const char *text, size_t length,
    size_t *used, uint32_t *glyph_index)
{
    if (pack->sequence_count == 0) return false;
    uint32_t values[8] = {0};
    size_t offsets[9] = {0};
    size_t count = 0;
    while (count < 8u && offsets[count] < length) {
        size_t bytes = utf8_next(
            text + offsets[count], length - offsets[count], &values[count]);
        if (bytes == 0) break;
        offsets[count + 1u] = offsets[count] + bytes;
        count++;
    }
    if (count < 2u) return false;
    /* Sequences are sorted lexicographically. Narrow the scan to the first
       codepoint's run so an installed catalog's 4,096-entry bound does not
       become per-emoji hot-path work. */
    size_t low = 0, high = pack->sequence_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (pack->sequences[middle].codepoints[0] < values[0])
            low = middle + 1u;
        else
            high = middle;
    }
    size_t best = 0;
    uint32_t best_glyph = 0;
    for (size_t at = low; at < pack->sequence_count; at++) {
        const GlyphSequence *candidate = &pack->sequences[at];
        if (candidate->codepoints[0] != values[0]) break;
        if (candidate->length > count || candidate->length <= best) continue;
        if (sequence_compare_prefix(candidate, values, candidate->length) == 0) {
            best = candidate->length;
            best_glyph = candidate->glyph_index;
        }
    }
    if (best == 0) return false;
    *used = offsets[best];
    *glyph_index = best_glyph;
    return true;
}

bool tilefinch_glyph_provider_match(
    TilefinchGlyphProvider *provider, const char *text, size_t length,
    size_t *used, uint32_t *glyph_key, unsigned *source_width)
{
    if (provider == NULL || text == NULL || length == 0
        || used == NULL || glyph_key == NULL) return false;
    uint32_t first = 0;
    size_t first_bytes = utf8_next(text, length, &first);
    if (first_bytes == 0) return false;
    /* Sequences in the shipped catalog begin in the emoji/keycap/flag
       ranges. Keep ordinary CJK and Latin at the O(1) codepoint lookup. */
    bool sequence_candidate = first == '#' || first == '*'
        || (first >= '0' && first <= '9')
        || first == 0x00a9u || first == 0x00aeu
        || first == 0x203cu || first == 0x2049u
        || first == 0x2122u || first == 0x2139u
        || (first >= 0x2194u && first <= 0x21ffu)
        || (first >= 0x2300u && first <= 0x27bfu)
        || (first >= 0x2934u && first <= 0x2935u)
        || (first >= 0x2b05u && first <= 0x2b55u)
        || first == 0x3030u || first == 0x303du
        || first == 0x3297u || first == 0x3299u
        || (first >= 0x1f000u && first <= 0x1faffu);
    if (sequence_candidate) {
        for (size_t pack = 0; pack < provider->pack_count; pack++) {
            uint32_t glyph = 0;
            size_t matched = 0;
            if (!pack_match_sequence(
                    &provider->packs[pack], text, length,
                    &matched, &glyph)) continue;
            *used = matched;
            *glyph_key = make_key(pack, glyph);
            if (source_width != NULL)
                *source_width = provider->packs[pack].source_width;
            return true;
        }
    }
    if (!tilefinch_glyph_provider_has_codepoint(
            provider, first, glyph_key, source_width)) return false;
    *used = first_bytes;
    return true;
}

static bool block_equal(GlyphBlockKey left, GlyphBlockKey right)
{
    return left.pack == right.pack && left.block == right.block;
}

static bool queue_block(TilefinchGlyphProvider *provider, GlyphBlockKey key)
{
    for (size_t at = 0; at < GLYPH_CACHE_SLOTS; at++)
        if (provider->cache[at].valid
            && block_equal(provider->cache[at].key, key)) return true;
    for (size_t at = 0; at < provider->queue_count; at++)
        if (block_equal(provider->queue[at], key)) return true;
    if (provider->queue_count >= GLYPH_QUEUE_LIMIT) return false;
    provider->queue[provider->queue_count++] = key;
    return true;
}

bool tilefinch_glyph_provider_source(
    TilefinchGlyphProvider *provider, uint32_t glyph_key,
    TilefinchGlyphSource *source)
{
    size_t pack_index = 0;
    uint32_t glyph = 0;
    if (source == NULL
        || !decode_key(provider, glyph_key, &pack_index, &glyph)) return false;
    GlyphPack *pack = &provider->packs[pack_index];
    GlyphBlockKey key = {
        .pack = (uint8_t) pack_index,
        .block = glyph / pack->block_glyphs
    };
    for (size_t at = 0; at < GLYPH_CACHE_SLOTS; at++) {
        GlyphBlockCache *cache = &provider->cache[at];
        if (!cache->valid || !block_equal(cache->key, key)) continue;
        size_t local = glyph % pack->block_glyphs;
        size_t offset = local * pack->glyph_stride;
        if (offset > cache->bytes
            || pack->glyph_stride > cache->bytes - offset) return false;
        *source = (TilefinchGlyphSource) {
            .pixels = cache->data + offset,
            .width = pack->source_width,
            .height = pack->source_height,
            .kind = pack->kind
        };
        return true;
    }
    (void) queue_block(provider, key);
    return false;
}

bool tilefinch_glyph_provider_key_pending(
    const TilefinchGlyphProvider *provider, uint32_t glyph_key)
{
    size_t pack = 0;
    uint32_t glyph = 0;
    if (!decode_key(provider, glyph_key, &pack, &glyph)) return false;
    GlyphBlockKey key = {
        .pack = (uint8_t) pack,
        .block = glyph / provider->packs[pack].block_glyphs
    };
    for (size_t at = 0; at < provider->queue_count; at++)
        if (block_equal(provider->queue[at], key)) return true;
    return false;
}

bool tilefinch_glyph_provider_pump(
    TilefinchGlyphProvider *provider, size_t maximum_bytes, bool *changed,
    size_t *bytes_read)
{
    if (changed != NULL) *changed = false;
    if (bytes_read != NULL) *bytes_read = 0;
    if (provider == NULL || maximum_bytes == 0) return false;
    if (provider->queue_count == 0) return true;
    GlyphBlockKey key = provider->queue[0];
    GlyphPack *pack = key.pack < provider->pack_count
        ? &provider->packs[key.pack] : NULL;
    if (pack == NULL) return false;
    uint32_t first = key.block * pack->block_glyphs;
    if (first >= pack->glyph_count) return false;
    uint32_t count = pack->glyph_count - first;
    if (count > pack->block_glyphs) count = pack->block_glyphs;
    size_t bytes = (size_t) count * pack->glyph_stride;
    if (bytes > maximum_bytes) return true;
    size_t slot = (key.block ^ key.pack) & (GLYPH_CACHE_SLOTS - 1u);
    GlyphBlockCache *cache = &provider->cache[slot];
    uint64_t offset = (uint64_t) pack->payload_offset
        + (uint64_t) first * pack->glyph_stride;
    if (!read_exact(pack->file, offset, cache->data, bytes)) return false;
    cache->key = key;
    cache->bytes = bytes;
    cache->valid = true;
    memmove(provider->queue, provider->queue + 1u,
            (provider->queue_count - 1u) * sizeof(provider->queue[0]));
    provider->queue_count--;
    if (changed != NULL) *changed = true;
    if (bytes_read != NULL) *bytes_read = bytes;
    return true;
}

size_t tilefinch_glyph_provider_pack_count(
    const TilefinchGlyphProvider *provider)
{
    return provider == NULL ? 0 : provider->pack_count;
}
