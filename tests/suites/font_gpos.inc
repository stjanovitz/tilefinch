/* Authored OpenType tables attached to the permissively licensed test face.
   No downloaded font or browsing capture belongs in the public suite. */
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
static size_t make_pair_position(unsigned char *p, unsigned format,
    unsigned left, unsigned right, int adjustment)
{
    store_be16(p, (uint16_t) format);
    store_be16(p + 4, 4); /* xAdvance of first glyph only. */
    if (format == 1) {
        store_be16(p + 2, 18);
        store_be16(p + 8, 1);
        store_be16(p + 10, 12);
        store_be16(p + 12, 1);
        store_be16(p + 14, (uint16_t) right);
        store_be16(p + 16, (uint16_t) adjustment);
        store_be16(p + 18, 1);
        store_be16(p + 20, 1);
        store_be16(p + 22, (uint16_t) left);
        return 24;
    }
    store_be16(p + 2, 24);
    store_be16(p + 8, 34);
    store_be16(p + 10, 44);
    store_be16(p + 12, 2);
    store_be16(p + 14, 2);
    store_be16(p + 20, 32); /* Class 1 / default class 0. */
    store_be16(p + 22, (uint16_t) adjustment);
    store_be16(p + 24, 2); /* Coverage range format. */
    store_be16(p + 26, 1);
    store_be16(p + 28, (uint16_t) left);
    store_be16(p + 30, (uint16_t) left);
    store_be16(p + 34, 2); /* ClassDef range format. */
    store_be16(p + 36, 1);
    store_be16(p + 38, (uint16_t) left);
    store_be16(p + 40, (uint16_t) left);
    store_be16(p + 42, 1);
    store_be16(p + 44, 1); /* ClassDef array format. */
    store_be16(p + 46, (uint16_t) right);
    store_be16(p + 48, 1);
    store_be16(p + 50, 1);
    return 52;
}

static void make_kerning_table(unsigned char table[384], unsigned format,
    bool extension, bool latin, unsigned left, unsigned right)
{
    memset(table, 0, 384);
    store_be16(table, 1);
    store_be16(table + 4, 16);
    store_be16(table + 6, 64);
    store_be16(table + 8, 96);
    store_be16(table + 16, latin ? 2 : 1);
    memcpy(table + 18, "DFLT", 4); store_be16(table + 22, 16);
    memcpy(table + 24, "latn", 4); store_be16(table + 28, 32);
    for (unsigned i = 0; i < 2; i++) {
        unsigned script = 32 + i * 16, feature = 80 + i * 8;
        store_be16(table + script, 4);
        store_be16(table + script + 6, 65535);
        store_be16(table + script + 8, 1);
        store_be16(table + script + 10, (uint16_t) i);
        memcpy(table + 66 + i * 6, "kern", 4);
        store_be16(table + 70 + i * 6, (uint16_t) (feature - 64));
        store_be16(table + feature + 2, 1);
        store_be16(table + feature + 4, (uint16_t) i);
        unsigned lookup = 104 + i * 8, sub = 128 + i * 128;
        store_be16(table + 98 + i * 2, (uint16_t) (lookup - 96));
        store_be16(table + lookup, extension ? 9 : 2);
        store_be16(table + lookup + 4, 1);
        store_be16(table + lookup + 6, (uint16_t) (sub - lookup));
        if (extension) {
            store_be16(table + sub, 1);
            store_be16(table + sub + 2, 2);
            store_be32(table + sub + 4, 8);
            sub += 8;
        }
        make_pair_position(table + sub, format, left, right, i ? -64 : -128);
    }
    store_be16(table + 64, 2);
    store_be16(table + 96, 2);
}

static unsigned char *font_with_kerning(const unsigned char *source,
    size_t length, const unsigned char *table, size_t table_length, size_t *size)
{
    size_t offset = (length + 3u) & ~(size_t) 3u;
    unsigned char *font = calloc(1, offset + table_length);
    if (font == NULL) return NULL;
    memcpy(font, source, length);
    unsigned count = read_be16(font + 4);
    for (unsigned i = 0; i < count; i++) {
        unsigned char *record = font + 12 + 16 * i;
        if (memcmp(record, "GPOS", 4) != 0) continue;
        store_be32(record + 4, 0);
        store_be32(record + 8, (uint32_t) offset);
        store_be32(record + 12, (uint32_t) table_length);
        memcpy(font + offset, table, table_length);
        *size = offset + table_length;
        return font;
    }
    free(font);
    return NULL;
}

static bool check_gpos_face(const unsigned char *font, size_t length,
    int units, unsigned em, bool class_pair)
{
    Budget budget; budget_init(&budget, 4u * MIB);
    FontFace face = {0}, alias = {0};
    CHECK(font_face_load_encoded(&face, &budget, font, length, MIB));
    const int sizes[] = {20 * 64, 971, 28 * 64, 20 * 64};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        CHECK(font_text_width_at_size_fixed_mode(&face, "AVAB", 4, sizes[s], false, true) > 0);
    size_t retained = budget.current;
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int64_t scaled = (int64_t) units * sizes[s] + em / 2;
        int expected = scaled < 0 ? -(int) ((-scaled + em - 1) / em) : (int) (scaled / em);
        for (unsigned repeat = 0; repeat < 3; repeat++) {
            CHECK(font_kerning_at_size_fixed(&face, 'A', 'V', sizes[s]) == expected);
            CHECK(font_kerning_at_size_fixed(&face, 'B', 'V', sizes[s]) == 0);
            int ordinary = font_text_width_at_size_fixed_mode(&face, "AVAV", 4, sizes[s], false, false);
            int kerned = font_text_width_at_size_fixed_mode(&face, "AVAV", 4, sizes[s], false, true);
            CHECK(kerned == ordinary + 2 * expected);
        }
        int class_zero = (int) (((int64_t) 32 * sizes[s] + em / 2) / em);
        CHECK(font_kerning_at_size_fixed(&face, 'A', 'B', sizes[s]) == (class_pair ? class_zero : 0));
    }
    CHECK(budget.current == retained);
    unsigned size_resets = tilefinch_test_faults()->web_font_kerning_cache_resets;
    for (unsigned i = 0; i < 4; i++)
        (void) font_kerning_at_size_fixed(&face, 'A', 'V', sizes[i]);
    CHECK(tilefinch_test_faults()->web_font_kerning_cache_resets
        == size_resets + (units == -192 ? 3u : 0u));
    /* Latin pairs around common-script pairs (DFLT plan) keep their cached
       entries: plans tag entries instead of flushing the shared cache. */
    int latin_pair = font_kerning_fixed(&face, 'A', 'V', 20);
    unsigned resets = tilefinch_test_faults()->web_font_kerning_cache_resets;
    for (unsigned repeat = 0; repeat < 4; repeat++) {
        CHECK(font_kerning_fixed(&face, '1', '2', 20) == 0);
        CHECK(font_kerning_fixed(&face, 'A', 'V', 20) == latin_pair);
    }
    CHECK(tilefinch_test_faults()->web_font_kerning_cache_resets == resets);
    unsigned lookups = tilefinch_test_faults()->web_font_charmap_lookups;
    for (unsigned repeat = 0; repeat < 8; repeat++) {
        CHECK(font_face_has_codepoint(&face, 'A'));
        CHECK(font_text_width_at_size_fixed_mode(&face, "AVAB12", 6,
            sizes[repeat % 4], false, true) > 0);
    }
    CHECK(tilefinch_test_faults()->web_font_charmap_lookups == lookups);
    CHECK(!font_face_has_codepoint(&face, 0));
    unsigned missing_lookup = tilefinch_test_faults()->web_font_charmap_lookups;
    CHECK(!font_face_has_codepoint(&face, 0));
    CHECK(tilefinch_test_faults()->web_font_charmap_lookups == missing_lookup);
    CHECK(font_face_share_encoded(&alias, &face, &budget, font, length, MIB));
    font_face_destroy(&face);
    CHECK(font_kerning_at_size_fixed(&alias, 'A', 'V', 20 * 64)
        == units * 20 * 64 / (int) em);
    font_face_destroy(&alias);
    CHECK(budget.current == 0);
    return true;
}
#endif

static bool test_opentype_pair_kerning(void)
{
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    unsigned char *source = NULL; size_t length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf", &source, &length));
    FT_Library library = NULL; FT_Face reference = NULL;
    CHECK(FT_Init_FreeType(&library) == 0
        && FT_New_Memory_Face(library, source, (FT_Long) length, 0, &reference) == 0);
    unsigned left = FT_Get_Char_Index(reference, 'A'), right = FT_Get_Char_Index(reference, 'V');
    unsigned em = reference->units_per_EM;
    CHECK(left != 0 && right != 0 && em == 2048);
    FT_Done_Face(reference); FT_Done_FreeType(library);
    unsigned char table[384];
    for (unsigned format = 1; format <= 2; format++) {
        for (unsigned extension = 0; extension < 2; extension++) {
            make_kerning_table(table, format, extension, false, left, right);
            size_t size = 0, encoded_size = 0;
            unsigned char *font = font_with_kerning(source, length, table, sizeof(table), &size);
            unsigned char *encoded = NULL;
            CHECK(font != NULL && check_gpos_face(font, size, -128, em, format == 2));
            CHECK(build_compressed_woff(font, size, &encoded, &encoded_size)
                && check_gpos_face(encoded, encoded_size, -128, em, format == 2));
            free(encoded); encoded = NULL;
#ifdef TILEFINCH_TEST_WOFF2
            CHECK(build_null_transform_woff2(font, size, &encoded, &encoded_size)
                && check_gpos_face(encoded, encoded_size, -128, em, format == 2));
            free(encoded);
#endif
            free(font);
        }
    }
    /* Script selection, duplicate feature references and additive lookups. */
    for (unsigned variant = 0; variant < 6; variant++) {
        make_kerning_table(table, 1, false, variant == 0, left, right);
        if (variant == 1) { store_be16(table + 40, 2); store_be16(table + 44, 0); }
        if (variant == 2) { store_be16(table + 82, 2); store_be16(table + 86, 1); }
        if (variant == 3) {
            store_be16(table + 108, 2); store_be16(table + 112, 152);
            store_be16(table + 144, 0); /* Explicit zero in first subtable. */
            memset(table + 256, 0, 128);
            make_pair_position(table + 256, 2, left, right, -256);
        }
        if (variant == 4) store_be16(table + 144, (uint16_t) -28); /* Negative half-pixel tie. */
        if (variant == 5) store_be16(table + 106, 8); /* IgnoreMarks, non-mark pair. */
        size_t size = 0;
        unsigned char *font = font_with_kerning(source, length, table, sizeof(table), &size);
        CHECK(font != NULL && check_gpos_face(font, size,
            variant == 0 ? -64 : variant == 2 ? -192 : variant == 3 ? 0 : variant == 4 ? -28 : -128,
            em, variant == 3));
        free(font);
    }
    /* IgnoreMarks uses glyph classes, not the input codepoint's script.
       A malformed class offset declines the plan without reading past GDEF. */
    for (unsigned variant = 0; variant < 3; variant++) {
        make_kerning_table(table, 1, false, false, left, right);
        store_be16(table + 106, 8);
        size_t size = 0;
        unsigned char *font = font_with_kerning(source, length, table, sizeof(table), &size);
        CHECK(font != NULL);
        unsigned char *grown = realloc(font, size + 20);
        CHECK(grown != NULL); font = grown;
        unsigned char *gdef = font + size;
        memset(gdef, 0, 20);
        store_be16(gdef, 1);
        store_be16(gdef + 4, variant == 2 ? 65535 : 12);
        store_be16(gdef + 12, 1);
        store_be16(gdef + 14, (uint16_t) left);
        store_be16(gdef + 16, 1);
        store_be16(gdef + 18, variant == 1 ? 3 : 1);
        for (unsigned i = 0; i < read_be16(font + 4); i++) {
            unsigned char *record = font + 12 + 16 * i;
            if (memcmp(record, "GDEF", 4) != 0) continue;
            store_be32(record + 8, (uint32_t) size);
            store_be32(record + 12, 20);
        }
        Budget budget; budget_init(&budget, 4u * MIB); FontFace face = {0};
        CHECK(font_face_load_encoded(&face, &budget, font, size + 20, MIB));
        CHECK(font_kerning_fixed(&face, 'A', 'V', 20) == (variant == 0 ? -80 : 0));
        font_face_destroy(&face); free(font); CHECK(budget.current == 0);
    }
    /* Refuse malformed/unsupported plans without rejecting the face. */
    for (unsigned variant = 0; variant < 7; variant++) {
        make_kerning_table(table, 2, variant == 4, false, left, right);
        if (variant == 0) store_be16(table + 6, 65535);
        if (variant == 1) store_be16(table + 106, 16); /* Mark-filtered lookup. */
        if (variant == 2) store_be16(table + 82, 65535);
        if (variant == 3) store_be16(table + 132, 5); /* Placement unsupported. */
        if (variant == 4) store_be32(table + 132, UINT32_MAX);
        if (variant == 5) store_be16(table + 140, 65535); /* Matrix exceeds table. */
        if (variant == 6) store_be16(table + 178, 65535); /* Invalid class. */
        size_t size = 0;
        unsigned char *font = font_with_kerning(source, length, table, sizeof(table), &size);
        Budget budget; budget_init(&budget, 4u * MIB); FontFace face = {0};
        CHECK(font != NULL && font_face_load_encoded(&face, &budget, font, size, MIB));
        unsigned before = tilefinch_test_faults()->web_font_charmap_lookups;
        CHECK(font_kerning_fixed(&face, 'A', 'V', 20) == 0);
        if (variant < 5)
            CHECK(tilefinch_test_faults()->web_font_charmap_lookups == before);
        font_face_destroy(&face); free(font); CHECK(budget.current == 0);
    }
    /* A selected GPOS kern replaces legacy kern rather than adding it.
       An unsupported GPOS plan still permits the legacy-table fallback. */
    for (unsigned unsupported = 0; unsupported < 2; unsupported++) {
        unsigned char combined[408] = {0};
        make_kerning_table(combined, 1, false, false, left, right);
        if (unsupported) store_be16(combined + 106, 16);
        unsigned char *kern = combined + 384;
        store_be16(kern + 2, 1);
        store_be16(kern + 6, 20);
        store_be16(kern + 8, 1);
        store_be16(kern + 10, 1);
        store_be16(kern + 12, 6);
        store_be16(kern + 18, (uint16_t) left);
        store_be16(kern + 20, (uint16_t) right);
        store_be16(kern + 22, (uint16_t) -256);
        size_t size = 0;
        unsigned char *font = font_with_kerning(source, length, combined, sizeof(combined), &size);
        CHECK(font != NULL);
        for (unsigned i = 0; i < read_be16(font + 4); i++) {
            unsigned char *record = font + 12 + 16 * i;
            if (memcmp(record, "GDEF", 4) != 0) continue;
            memcpy(record, "kern", 4);
            store_be32(record + 4, 0);
            store_be32(record + 8, (uint32_t) (size - 24));
            store_be32(record + 12, 24);
            break;
        }
        CHECK(check_gpos_face(font, size, unsupported ? -256 : -128, em, false));
        free(font);
    }
    /* Every table truncation and allocation refusal must unwind cleanly. */
    make_kerning_table(table, 1, true, false, left, right);
    for (size_t cut = 1; cut <= sizeof(table); cut++) {
        size_t size = 0;
        unsigned char *font = font_with_kerning(source, length, table, cut, &size);
        Budget budget; budget_init(&budget, 4u * MIB); FontFace face = {0};
        CHECK(font != NULL && font_face_load_encoded(&face, &budget, font, size, MIB));
        (void) font_kerning_fixed(&face, 'A', 'V', 20);
        font_face_destroy(&face); free(font); CHECK(budget.current == 0);
    }
    size_t size = 0;
    unsigned char *font = font_with_kerning(source, length, table, sizeof(table), &size);
    CHECK(font != NULL);
    bool done = false;
    for (size_t fail = 0; fail < 512; fail++) {
        Budget budget; budget_init(&budget, 4u * MIB); FontFace face = {0};
        budget_inject_failure_after(&budget, fail);
        bool loaded = font_face_load_encoded(&face, &budget, font, size, MIB);
        if (loaded) (void) font_kerning_fixed(&face, 'A', 'V', 20);
        bool injected = budget.injected_failure_count != 0;
        budget_clear_failure_injection(&budget);
        font_face_destroy(&face); CHECK(budget.current == 0);
        if (!injected) { CHECK(loaded); done = true; break; }
    }
    CHECK(done); free(font); free(source);
#endif
    return true;
}
