/* The hashed catalog format's structural checks and lookup, below the
   SHA-256 pin that keeps crafted bytes away from the public entry points,
   and the 32-bit value offsets that let a v17 file pass 64 KiB. */
#include "../src/ui_language.c"
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

typedef struct { const char *key, *value; } Row;
enum { CAPACITY = TILEFINCH_UI_TRANSLATION_BYTES + 1 };
static unsigned char file[CAPACITY];

static uint32_t key_hash(const char *key)
{ return style_fnv1a(STYLE_FNV1A_SEED, key, strlen(key), true); }

static int by_hash(const void *a, const void *b)
{
    uint32_t x = key_hash(((const Row *) a)->key), y = key_hash(((const Row *) b)->key);
    return x < y ? -1 : x > y;
}

/* What tools/generate_ui_translations.py writes. */
static size_t build(const char *code, Row *rows, size_t count)
{
    qsort(rows, count, sizeof(*rows), by_hash);
    size_t at = (size_t) snprintf((char *) file, CAPACITY, "TFUI2\t%s\n", code);
    file[at++] = (unsigned char) (count >> 8);
    file[at++] = (unsigned char) count;
    for (size_t i = 0; i < count; i++) {
        uint32_t h = key_hash(rows[i].key);
        file[at++] = (unsigned char) (h >> 24); file[at++] = (unsigned char) (h >> 16);
        file[at++] = (unsigned char) (h >> 8); file[at++] = (unsigned char) h;
    }
    for (size_t i = 0; i < count; i++) file[at++] = (unsigned char) strlen(rows[i].key);
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(rows[i].value);
        memcpy(file + at, rows[i].value, length);
        at += length;
        file[at++] = '\n';
    }
    return at;
}

static TilefinchUiTranslation *parse(Budget *budget, size_t length)
{ return translation_parse(budget, TILEFINCH_UI_LANGUAGE_SPANISH, "es", file, length); }

/* Rows whose values end past 64 KiB, in a file just under the bound. With
   16-bit offsets every row after the wrap read another row's text, and an
   Arabic display cache over 64 KiB refused the whole catalog. */
enum { WIDE_ROWS = 1200 };
static char wide_keys[WIDE_ROWS][16], wide_values[WIDE_ROWS][112];
static Row wide[WIDE_ROWS];

static bool wide_catalog(Budget *budget, unsigned language, const char *code,
                         const char *fill)
{
    size_t fill_bytes = strlen(fill);
    for (size_t i = 0; i < WIDE_ROWS; i++) {
        snprintf(wide_keys[i], sizeof(wide_keys[i]), "Wide %zu", i);
        int n = snprintf(wide_values[i], sizeof(wide_values[i]), "%zu ", i);
        while ((size_t) n + fill_bytes < sizeof(wide_values[i]) - 8u) {
            memcpy(wide_values[i] + n, fill, fill_bytes);
            n += (int) fill_bytes;
        }
        wide_values[i][n] = 0;
        wide[i] = (Row) {wide_keys[i], wide_values[i]};
    }
    size_t length = build(code, wide, WIDE_ROWS);
    if (length <= 64u * 1024u || length > TILEFINCH_UI_TRANSLATION_BYTES) {
        fprintf(stderr, "wide catalog is %zu bytes\n", length);
        return false;
    }
    TilefinchUiTranslation *t = translation_parse(budget, language, code, file, length);
    if (t == NULL) {
        fprintf(stderr, "a %zu-byte %s catalog was refused\n", length, code);
        return false;
    }
    bool okay = t->values[t->count - 1] > UINT16_MAX;
    char probe[16];
    for (size_t i = 0; okay && i < WIDE_ROWS; i++) {
        snprintf(probe, sizeof(probe), "Wide %zu", i);
        /* Each value starts with its own row number. */
        tilefinch_ui_translation_bind(t);
        const char *found = tilefinch_ui_logical_text(probe);
        tilefinch_ui_translation_bind(NULL);
        char expected[16];
        snprintf(expected, sizeof(expected), "%zu ", i);
        if (strncmp(found, expected, strlen(expected)) != 0
            || strlen(found) != strlen(wide_values[i])) {
            fprintf(stderr, "%s row %zu read \"%.24s\"\n", code, i, found);
            okay = false;
        }
        const char *display = tilefinch_ui_translation_text(t, probe);
        bool display_okay = display == found;
        if (tilefinch_ui_language_right_to_left(language)) {
            /* The same value's display text from a one-row catalog. */
            Row single[] = {{probe, wide_values[i]}};
            TilefinchUiTranslation *reference = translation_parse(
                budget, language, code, file, build(code, single, 1));
            display_okay = reference != NULL && display != found
                && strcmp(display, tilefinch_ui_translation_text(reference, probe)) == 0;
            tilefinch_ui_translation_destroy(reference);
        }
        if (!display_okay) {
            fprintf(stderr, "%s row %zu display text is wrong\n", code, i);
            okay = false;
        }
    }
    if (tilefinch_ui_language_right_to_left(language))
        okay = okay && t->visual != NULL;
    tilefinch_ui_translation_destroy(t);
    return okay && budget->current == 0;
}

static bool wide_catalogs(Budget *budget)
{
    return wide_catalog(budget, TILEFINCH_UI_LANGUAGE_SPANISH, "es",
                        "abcdefghij")
        /* Two-byte letters become three-byte presentation forms. */
        && wide_catalog(budget, TILEFINCH_UI_LANGUAGE_ARABIC, "ar",
                        "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 ")
        /* Hebrew letters keep their bytes and are only reordered. */
        && wide_catalog(budget, TILEFINCH_UI_LANGUAGE_HEBREW, "he",
                        "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d ");
}

/* Installed v15 and v16 files keep parsing: the wire layout did not
   change. Each published file's rows all load. */
static bool published_catalogs_parse(Budget *budget)
{
    static const unsigned versions[] = {15u, 16u};
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++) {
        /* Hebrew's first catalog is v18. */
        for (unsigned language = 1; language < TILEFINCH_UI_LANGUAGE_HEBREW; language++) {
            const char *code = tilefinch_ui_language_spec(language)->code;
            char path[1024];
            snprintf(path, sizeof(path), "%s/translations/ui/v%u/%s.tful",
                     TILEFINCH_UI_RESOURCE_ROOT, versions[v], code);
            FILE *f = fopen(path, "rb");
            if (f == NULL) { fprintf(stderr, "missing %s\n", path); return false; }
            size_t length = fread(file, 1, sizeof(file), f);
            fclose(f);
            if (length == 0 || length > 64u * 1024u - 1u) {
                fprintf(stderr, "%s is %zu bytes\n", path, length);
                return false;
            }
            TilefinchUiTranslation *t = translation_parse(budget, language, code, file, length);
            size_t rows = (size_t) file[strlen("TFUI2\t") + strlen(code) + 1u] << 8
                | file[strlen("TFUI2\t") + strlen(code) + 2u];
            bool okay = t != NULL && t->count == rows
                && strcmp(tilefinch_ui_translation_text(t, "Settings"), "Settings") != 0;
            tilefinch_ui_translation_destroy(t);
            if (!okay || budget->current != 0) {
                fprintf(stderr, "%s did not parse\n", path);
                return false;
            }
        }
    }
    return true;
}

static bool refused(Budget *budget, size_t length)
{
    TilefinchUiTranslation *t = parse(budget, length);
    tilefinch_ui_translation_destroy(t);
    return t == NULL && budget->current == 0;
}

int main(void)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    Row rows[] = {{"Settings", "Ajustes"}, {"Back", "Volver"}, {"queued", "en cola"},
                  {"PAGE READY", "PÁGINA LISTA"}, {"X Select   O Back", "X Elegir   O Volver"}};
    size_t count = sizeof(rows) / sizeof(rows[0]);
    size_t length = build("es", rows, count);
    TilefinchUiTranslation *t = parse(&budget, length);
    CHECK(t);
    CHECK(!strcmp(tilefinch_ui_translation_text(t, "Settings"), "Ajustes"));
    /* strcasecmp matched any ASCII case; so does the folded hash. */
    CHECK(!strcmp(tilefinch_ui_translation_text(t, "SETTINGS"), "Ajustes"));
    CHECK(!strcmp(tilefinch_ui_translation_text(t, "page ready"), "PÁGINA LISTA"));
    CHECK(!strcmp(tilefinch_ui_translation_text(t, "x select   o back"), "X Elegir   O Volver"));
    const char *other = "Settings ";
    CHECK(tilefinch_ui_translation_text(t, other) == other);
    /* A non-key string whose FNV-1a hash equals the key "queued" (found
       by search): the key length keeps it English. */
    const char *collision = "Untranslated probe 3466157";
    CHECK(key_hash(collision) == key_hash("queued"));
    CHECK(tilefinch_ui_translation_text(t, collision) == collision);
    CHECK(!strcmp(tilefinch_ui_translation_text(t, "Queued"), "en cola"));
    tilefinch_ui_translation_destroy(t);
    CHECK(budget.current == 0);

    /* Truncated anywhere, or with anything after the last value. */
    for (size_t cut = 0; cut < length; cut++) CHECK(refused(&budget, cut));
    file[length] = 'x';
    CHECK(refused(&budget, length + 1));
    file[length] = '\n';
    CHECK(refused(&budget, length + 1));
    size_t header = strlen("TFUI2\tes\n");
    size_t hashes = header + 2, lengths = hashes + 4 * count, values = lengths + count;
    /* Another language's header, or the old keyed format. */
    CHECK(!translation_parse(&budget, TILEFINCH_UI_LANGUAGE_FRENCH, "fr", file, length));
    file[4] = '1'; CHECK(refused(&budget, length)); file[4] = '2';
    /* Row count: zero, above the row bound, more rows than values. */
    unsigned char saved = file[hashes - 1];
    file[hashes - 1] = 0; CHECK(refused(&budget, length));
    file[hashes - 2] = (unsigned char) ((TILEFINCH_UI_TRANSLATION_ROWS + 1u) >> 8);
    file[hashes - 1] = (unsigned char) (TILEFINCH_UI_TRANSLATION_ROWS + 1u);
    CHECK(refused(&budget, length));
    file[hashes - 2] = 0; file[hashes - 1] = (unsigned char) (count + 1);
    CHECK(refused(&budget, length));
    file[hashes - 1] = saved;
    CHECK(refused(&budget, length - 1) && (t = parse(&budget, length)) != NULL);
    tilefinch_ui_translation_destroy(t);
    /* Hashes out of order or repeated. */
    unsigned char first[4];
    memcpy(first, file + hashes, 4);
    memcpy(file + hashes, file + hashes + 4, 4); CHECK(refused(&budget, length));
    memcpy(file + hashes, first, 4);
    file[hashes + 4] = 0; file[hashes + 5] = 0; file[hashes + 6] = 0; file[hashes + 7] = 0;
    CHECK(refused(&budget, length));
    length = build("es", rows, count);
    /* A zero key length. */
    saved = file[lengths]; file[lengths] = 0; CHECK(refused(&budget, length));
    file[lengths] = saved;
    /* Values: control bytes, '%', NUL, empty, too long. */
    const unsigned char bad[] = {'%', 0, '\t', '\r', 0x7f};
    for (size_t i = 0; i < sizeof(bad); i++) {
        saved = file[values]; file[values] = bad[i];
        CHECK(refused(&budget, length));
        file[values] = saved;
    }
    Row empty[] = {{"Settings", ""}};
    CHECK(refused(&budget, build("es", empty, 1)));
    char longest[TILEFINCH_UI_TRANSLATION_VALUE_BYTES + 2];
    memset(longest, 'a', sizeof(longest) - 1);
    longest[TILEFINCH_UI_TRANSLATION_VALUE_BYTES] = 0;
    Row edge[] = {{"Settings", longest}};
    CHECK((t = parse(&budget, build("es", edge, 1))) != NULL);
    tilefinch_ui_translation_destroy(t);
    longest[TILEFINCH_UI_TRANSLATION_VALUE_BYTES] = 'a';
    longest[TILEFINCH_UI_TRANSLATION_VALUE_BYTES + 1] = 0;
    CHECK(refused(&budget, build("es", edge, 1)));
    /* A full catalog of TILEFINCH_UI_TRANSLATION_ROWS rows loads; one more
       is refused. The allocation grows by exactly a hash, a value offset,
       a key length and the value per row: nothing is sized to the bound. */
    CHECK(TILEFINCH_UI_TRANSLATION_ROWS == 2048u);
    static char full_keys[TILEFINCH_UI_TRANSLATION_ROWS + 1][16];
    static Row full[TILEFINCH_UI_TRANSLATION_ROWS + 1];
    for (size_t i = 0; i <= TILEFINCH_UI_TRANSLATION_ROWS; i++) {
        snprintf(full_keys[i], sizeof(full_keys[i]), "Label %zu", i);
        full[i] = (Row) {full_keys[i], "v"};
    }
    Row one[] = {{"Label 0", "v"}};
    CHECK((t = parse(&budget, build("es", one, 1))) != NULL);
    size_t one_row = budget.current;
    tilefinch_ui_translation_destroy(t);
    length = build("es", full, TILEFINCH_UI_TRANSLATION_ROWS);
    CHECK((t = parse(&budget, length)) != NULL);
    CHECK(t->count == TILEFINCH_UI_TRANSLATION_ROWS);
    CHECK(budget.current - one_row == (TILEFINCH_UI_TRANSLATION_ROWS - 1u)
          * (sizeof(uint32_t) + sizeof(uint32_t) + 1u + strlen("v\n")));
    char probe[16];
    for (size_t i = 0; i < TILEFINCH_UI_TRANSLATION_ROWS; i++) {
        snprintf(probe, sizeof(probe), "Label %zu", i);
        CHECK(!strcmp(tilefinch_ui_translation_text(t, probe), "v"));
    }
    tilefinch_ui_translation_destroy(t);
    CHECK(budget.current == 0);
    CHECK(refused(&budget, build("es", full, TILEFINCH_UI_TRANSLATION_ROWS + 1u)));
    /* A refused allocation leaves nothing behind. */
    length = build("es", rows, count);
    budget.limit = budget.current + 64;
    CHECK(refused(&budget, length));
    CHECK(budget.current == 0);
    budget.limit = 1024u * 1024u;
    if (!wide_catalogs(&budget)) return 1;
    if (!published_catalogs_parse(&budget)) return 1;
    return 0;
}
