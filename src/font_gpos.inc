/* Bounded horizontal kern-feature positioning, not a general OpenType shaper.
   PairPos formats 1/2 and their extension wrappers are read from the immutable
   sfnt stream already owned by FreeType (including reconstructed WOFF/WOFF2).
   No table copy, pair expansion, or page-controlled allocation is necessary.
   Placement, contextual, mark-filtered and variation positioning need a richer
   shaping API; unsupported plans retain the legacy kern-table fallback.
   FreeType's optional basic GPOS mode merges scripts and stops at the first
   matching pair across lookups; keep script selection, additive lookup order
   and first-applicable-subtable semantics here instead. */
#define FONT_GPOS_LOOKUPS 8u
#define FONT_GPOS_SUBTABLES 8u
#define FONT_GPOS_CACHE 32u

typedef struct {
    const unsigned char *bytes;
    size_t length;
} FontGposView;

typedef struct {
    uint16_t lookups[FONT_GPOS_LOOKUPS];
    unsigned count;
} FontGposPlan;

typedef struct {
    FontGposView table;
    FontGposView glyph_classes;
    /* Default, Latin, Greek and Cyrillic default-language plans. */
    FontGposPlan plans[4];
    /* Single-lookup plans cache design units across all pixel sizes; richer
       plans cache scaled sums so rounding stays independent per lookup. */
    struct { uint32_t pair; int adjustment; } cache[FONT_GPOS_CACHE];
    FT_Fixed cached_scale;
    unsigned cached_script;
    uint8_t script_plan[4];
} FontGpos;

static unsigned gpos_u16(const unsigned char *p)
{
    return (unsigned) p[0] << 8 | p[1];
}

static bool gpos_array(FontGposView view, size_t header,
                        unsigned count, unsigned stride)
{
    return header <= view.length && count <= (view.length - header) / stride;
}

static bool gpos_at(FontGposView view, size_t offset, size_t minimum,
                     FontGposView *out)
{
    if (offset == 0 || offset > view.length || minimum > view.length - offset)
        return false;
    *out = (FontGposView) {view.bytes + offset, view.length - offset};
    return true;
}

/* Binary searches have at most 16 iterations (16-bit OpenType counts).
   Every access is checked even when malformed records are not sorted. */
static int gpos_coverage(FontGposView v, unsigned glyph)
{
    if (v.length < 4) return -1;
    unsigned format = gpos_u16(v.bytes), count = gpos_u16(v.bytes + 2);
    if ((format != 1 && format != 2)
        || !gpos_array(v, 4, count, format == 1 ? 2 : 6)) return -1;
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        const unsigned char *p = v.bytes + 4 + mid * (format == 1 ? 2 : 6);
        unsigned first = gpos_u16(p);
        unsigned last = format == 1 ? first : gpos_u16(p + 2);
        if (glyph < first) hi = mid;
        else if (glyph > last) lo = mid + 1;
        else return format == 1 ? (int) mid
            : (int) gpos_u16(p + 4) + (int) (glyph - first);
    }
    return -1;
}

static int gpos_class(FontGposView v, unsigned glyph)
{
    if (v.length < 4) return -1;
    unsigned format = gpos_u16(v.bytes);
    if (format == 1) {
        if (v.length < 6) return -1;
        unsigned first = gpos_u16(v.bytes + 2), count = gpos_u16(v.bytes + 4);
        if (!gpos_array(v, 6, count, 2)) return -1;
        return glyph >= first && glyph - first < count
            ? (int) gpos_u16(v.bytes + 6 + 2 * (glyph - first)) : 0;
    }
    unsigned count = gpos_u16(v.bytes + 2), lo = 0, hi = count;
    if (format != 2 || !gpos_array(v, 4, count, 6)) return -1;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        const unsigned char *p = v.bytes + 4 + mid * 6;
        if (glyph < gpos_u16(p)) hi = mid;
        else if (glyph > gpos_u16(p + 2)) lo = mid + 1;
        else return (int) gpos_u16(p + 4);
    }
    return 0;
}

static bool gpos_pair_table(FontGposView table, unsigned type,
                             FontGposView *pair)
{
    if (type == 9) {
        if (table.length < 8 || gpos_u16(table.bytes) != 1
            || gpos_u16(table.bytes + 2) != 2
            || !gpos_at(table, read_be32(table.bytes + 4), 8, &table))
            return false;
    } else if (type != 2) return false;
    if (table.length < 8 || gpos_u16(table.bytes) < 1
        || gpos_u16(table.bytes) > 2
        || gpos_u16(table.bytes + 4) != 4
        || gpos_u16(table.bytes + 6) != 0) return false;
    *pair = table;
    return true;
}

static bool gpos_pair(FontGposView v, unsigned left, unsigned right, int *units)
{
    FontGposView coverage;
    if (!gpos_at(v, gpos_u16(v.bytes + 2), 4, &coverage)) return false;
    int index = gpos_coverage(coverage, left);
    if (index < 0) return false;
    if (gpos_u16(v.bytes) == 1) {
        if (v.length < 10) return false;
        unsigned count = gpos_u16(v.bytes + 8);
        FontGposView set;
        if (!gpos_array(v, 10, count, 2) || (unsigned) index >= count
            || !gpos_at(v, gpos_u16(v.bytes + 10 + 2 * index), 2, &set))
            return false;
        count = gpos_u16(set.bytes);
        if (!gpos_array(set, 2, count, 4)) return false;
        unsigned lo = 0, hi = count;
        while (lo < hi) {
            unsigned mid = lo + (hi - lo) / 2;
            const unsigned char *p = set.bytes + 2 + mid * 4;
            unsigned glyph = gpos_u16(p);
            if (right < glyph) hi = mid;
            else if (right > glyph) lo = mid + 1;
            else { *units = (int16_t) gpos_u16(p + 2); return true; }
        }
        return false;
    }
    FontGposView first, second;
    if (v.length < 16
        || !gpos_at(v, gpos_u16(v.bytes + 8), 4, &first)
        || !gpos_at(v, gpos_u16(v.bytes + 10), 4, &second)) return false;
    unsigned rows = gpos_u16(v.bytes + 12), columns = gpos_u16(v.bytes + 14);
    if (columns == 0 || rows > (v.length - 16) / 2 / columns) return false;
    int a = gpos_class(first, left), b = gpos_class(second, right);
    if (a < 0 || b < 0 || (unsigned) a >= rows || (unsigned) b >= columns)
        return false;
    *units = (int16_t) gpos_u16(v.bytes + 16 + 2 * ((unsigned) a * columns + (unsigned) b));
    return true;
}

static bool gpos_plan(FontGposView table, FontGposView lang,
                       FontGposView features, FontGposView lookups,
                       FontGposPlan *plan)
{
    unsigned count = gpos_u16(lang.bytes + 4);
    unsigned feature_count = gpos_u16(features.bytes);
    unsigned lookup_count = gpos_u16(lookups.bytes);
    if (count > 256 || !gpos_array(lang, 6, count, 2)) return false;
    for (unsigned at = 0; at <= count; at++) {
        unsigned feature = at == count ? gpos_u16(lang.bytes + 2)
                                      : gpos_u16(lang.bytes + 6 + 2 * at);
        if (feature == 65535) continue;
        if (feature >= feature_count) return false;
        const unsigned char *record = features.bytes + 2 + feature * 6;
        if (read_be32(record) != UINT32_C(0x6b65726e)) continue; /* kern */
        FontGposView f;
        if (!gpos_at(features, gpos_u16(record + 4), 4, &f)) return false;
        unsigned n = gpos_u16(f.bytes + 2);
        if (n > FONT_GPOS_LOOKUPS || !gpos_array(f, 4, n, 2)) return false;
        for (unsigned i = 0; i < n; i++) {
            unsigned index = gpos_u16(f.bytes + 4 + 2 * i);
            if (index >= lookup_count) return false;
            size_t offset = (size_t) (lookups.bytes - table.bytes)
                          + gpos_u16(lookups.bytes + 2 + 2 * index);
            FontGposView lookup;
            if (!gpos_at(table, offset, 6, &lookup)
                || (gpos_u16(lookup.bytes + 2) & ~8u) != 0) return false;
            unsigned sub_count = gpos_u16(lookup.bytes + 4);
            if (sub_count > FONT_GPOS_SUBTABLES
                || !gpos_array(lookup, 6, sub_count, 2)) return false;
            for (unsigned sub = 0; sub < sub_count; sub++) {
                FontGposView v, pair;
                if (!gpos_at(lookup, gpos_u16(lookup.bytes + 6 + 2 * sub), 8, &v)
                    || !gpos_pair_table(v, gpos_u16(lookup.bytes), &pair)) return false;
            }
            unsigned pos = 0;
            while (pos < plan->count && plan->lookups[pos] < index) pos++;
            if (pos < plan->count && plan->lookups[pos] == index) continue;
            if (plan->count == FONT_GPOS_LOOKUPS) return false;
            memmove(plan->lookups + pos + 1, plan->lookups + pos,
                    (plan->count - pos) * sizeof(plan->lookups[0]));
            plan->lookups[pos] = (uint16_t) index;
            plan->count++;
        }
    }
    return true;
}

static void gpos_initialize(FontGpos *gpos, FT_Face face)
{
    FT_Stream stream = face->stream;
    /* The pinned FreeType memory-face and WOFF reconstructors keep this
       public memory stream immutable until FT_Done_Face. Never borrow from
       a callback stream, collection, or unknown directory representation. */
    if (stream == NULL || stream->read != NULL || stream->base == NULL
        || stream->size < 12 || read_be32(stream->base) != SFNT_TRUETYPE_SIGNATURE)
        return;
    unsigned tables = gpos_u16(stream->base + 4);
    if (tables > 256 || tables > (stream->size - 12) / 16) return;
    FontGposView table = {0}, gdef = {0};
    for (unsigned i = 0; i < tables; i++) {
        const unsigned char *record = stream->base + 12 + 16 * i;
        uint32_t tag = read_be32(record);
        if (tag != UINT32_C(0x47504f53) /* GPOS */
            && tag != UINT32_C(0x47444546)) continue; /* GDEF */
        size_t offset = read_be32(record + 8), length = read_be32(record + 12);
        if (offset > stream->size || length > stream->size - offset) return;
        if (tag == UINT32_C(0x47504f53))
            table = (FontGposView) {stream->base + offset, length};
        else gdef = (FontGposView) {stream->base + offset, length};
    }
    if (gdef.bytes != NULL) {
        if (gdef.length < 12 || gpos_u16(gdef.bytes) != 1) return;
        unsigned offset = gpos_u16(gdef.bytes + 4);
        if (offset != 0
            && !gpos_at(gdef, offset, 4, &gpos->glyph_classes)) return;
    }
    FontGposView scripts, features, lookups;
    if (table.length < 10 || gpos_u16(table.bytes) != 1
        || gpos_u16(table.bytes + 2) > 1
        || (gpos_u16(table.bytes + 2) == 1
            && (table.length < 14 || read_be32(table.bytes + 10) != 0))
        || !gpos_at(table, gpos_u16(table.bytes + 4), 2, &scripts)
        || !gpos_at(table, gpos_u16(table.bytes + 6), 2, &features)
        || !gpos_at(table, gpos_u16(table.bytes + 8), 2, &lookups)) return;
    unsigned count = gpos_u16(scripts.bytes);
    if (count > 256 || gpos_u16(features.bytes) > 256
        || gpos_u16(lookups.bytes) > 256
        || !gpos_array(scripts, 2, count, 6)
        || !gpos_array(features, 2, gpos_u16(features.bytes), 6)
        || !gpos_array(lookups, 2, gpos_u16(lookups.bytes), 2)) return;
    bool found[4] = {false};
    static const uint32_t tags[4] = {UINT32_C(0x44464c54), UINT32_C(0x6c61746e),
                                    UINT32_C(0x6772656b), UINT32_C(0x6379726c)};
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *record = scripts.bytes + 2 + 6 * i;
        for (unsigned script = 0; script < 4; script++) {
            if (read_be32(record) != tags[script] || found[script]) continue;
            found[script] = true;
            FontGposView s, lang;
            if (!gpos_at(scripts, gpos_u16(record + 4), 4, &s)
                || !gpos_at(s, gpos_u16(s.bytes), 6, &lang)
                || !gpos_plan(table, lang, features, lookups, &gpos->plans[script]))
                gpos->plans[script].count = 0;
        }
    }
    for (unsigned script = 1; script < 4; script++)
        if (!found[script]) gpos->plans[script] = gpos->plans[0];
    /* Common punctuation/numbers and adjacent letters often select exactly
       the same lookups. Share their cache identity rather than flushing all
       pairs at every such boundary; distinct script plans stay distinct. */
    for (unsigned script = 0; script < 4; script++) {
        gpos->script_plan[script] = (uint8_t) script;
        for (unsigned previous = 0; previous < script; previous++) {
            if (gpos->plans[script].count == gpos->plans[previous].count
                && memcmp(gpos->plans[script].lookups, gpos->plans[previous].lookups,
                    gpos->plans[script].count * sizeof(gpos->plans[script].lookups[0])) == 0) {
                gpos->script_plan[script] = gpos->script_plan[previous];
                break;
            }
        }
    }
    gpos->table = table;
}

static unsigned gpos_script(unsigned c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= 0xc0 && c <= 0x2af) || (c >= 0x1e00 && c <= 0x1eff)) return 1;
    if ((c >= 0x370 && c <= 0x3ff) || (c >= 0x1f00 && c <= 0x1fff)) return 2;
    if ((c >= 0x400 && c <= 0x52f) || (c >= 0x2de0 && c <= 0x2dff)
        || (c >= 0xa640 && c <= 0xa69f)) return 3;
    return 0;
}

static int gpos_scale(int units, FT_Fixed scale)
{
    if (units == 0) return 0;
    int delta = (int) FT_MulFix(units, scale);
    /* Match signed OpenType half-unit rounding, not FT's negative ties
       away from zero. The low product bits avoid another wide division. */
    if (units < 0
        && (((uint32_t) units * (uint32_t) scale) & 65535u) == 32768u)
        delta++;
    return delta;
}

static bool gpos_kerning(FontGpos *gpos, unsigned left, unsigned right,
                          unsigned left_codepoint, unsigned right_codepoint,
                          FT_Fixed scale, int *adjustment)
{
    unsigned a = gpos_script(left_codepoint), b = gpos_script(right_codepoint);
    unsigned script = a == 0 ? b : b == 0 || a == b ? a : 0;
    script = gpos->script_plan[script];
    const FontGposPlan *plan = &gpos->plans[script];
    if (plan->count == 0) return false;
    if ((plan->count > 1 && gpos->cached_scale != scale)
        || gpos->cached_script != script) {
#ifndef __PSP__
        tilefinch_test_faults()->web_font_kerning_cache_resets++;
#endif
        memset(gpos->cache, 0, sizeof(gpos->cache));
        gpos->cached_scale = scale;
        gpos->cached_script = script;
    }
    uint32_t key = (uint32_t) left << 16 | right;
    unsigned slot = (key ^ (key >> 11)) & (FONT_GPOS_CACHE - 1);
    if (gpos->cache[slot].pair != key) {
        int total = 0;
        FontGposView lookups;
        if (!gpos_at(gpos->table, gpos_u16(gpos->table.bytes + 8), 2, &lookups))
            return false;
        for (unsigned i = 0; i < plan->count; i++) {
            FontGposView lookup;
            if (!gpos_at(lookups,
                    gpos_u16(lookups.bytes + 2 + 2 * plan->lookups[i]), 6, &lookup))
                continue;
            unsigned count = gpos_u16(lookup.bytes + 4);
            /* IgnoreMarks does not change an adjacent non-mark pair. Use
               GDEF glyph classes, not Unicode guesses; mark filtering sets,
               attachment classes and other flags remain unsupported. */
            if ((gpos_u16(lookup.bytes + 2) & 8u) != 0
                && gpos->glyph_classes.bytes != NULL) {
                int a = gpos_class(gpos->glyph_classes, left);
                int b = gpos_class(gpos->glyph_classes, right);
                if (a < 0 || b < 0) return false;
                if (a == 3 || b == 3) continue;
            }
            for (unsigned j = 0; j < count; j++) {
                FontGposView sub, pair;
                int units;
                if (gpos_at(lookup, gpos_u16(lookup.bytes + 6 + 2 * j), 8, &sub)
                    && gpos_pair_table(sub, gpos_u16(lookup.bytes), &pair)
                    && gpos_pair(pair, left, right, &units)) {
                    total += plan->count == 1 ? units : gpos_scale(units, scale);
                    break; /* First applicable subtable, including zero. */
                }
            }
        }
        gpos->cache[slot].pair = key;
        gpos->cache[slot].adjustment = total;
    }
    *adjustment = plan->count == 1
        ? gpos_scale(gpos->cache[slot].adjustment, scale)
        : gpos->cache[slot].adjustment;
    return true; /* Active GPOS kern replaces, never adds to, legacy kern. */
}
