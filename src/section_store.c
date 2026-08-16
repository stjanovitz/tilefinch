#include "tilefinch/section_store.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/platform.h"

#define LZ_HASH_BITS 14u
#define LZ_HASH_SIZE (1u << LZ_HASH_BITS)
#define LZ_MIN_MATCH 4u
#define LAYOUT_ISLAND_SECTION_MULTIPLIER 4u
#define SECTION_ELEMENT_LIMIT 512u
#define LAYOUT_ISLAND_ELEMENT_LIMIT SECTION_ELEMENT_LIMIT

static void section_store_bump_index_revision(CompressedSectionStore *store)
{
    store->index_revision++;
    if (store->index_revision == 0) store->index_revision = 1;
}

static size_t layout_island_section_limit(
    const CompressedSectionStore *store)
{
    return store->maximum_section_bytes
               > SIZE_MAX / LAYOUT_ISLAND_SECTION_MULTIPLIER
        ? SIZE_MAX
        : store->maximum_section_bytes * LAYOUT_ISLAND_SECTION_MULTIPLIER;
}

static bool reserve_array(Budget *budget, void **data, size_t *capacity,
                          size_t count, size_t item_size)
{
    if (count <= *capacity) return true;
    size_t next = *capacity == 0 ? 16 : *capacity;
    while (next < count) {
        if (next > SIZE_MAX / 2) { next = count; break; }
        next *= 2;
    }
    if (next > SIZE_MAX / item_size) return false;
    void *expanded = budget_realloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, *data, next * item_size);
    if (expanded == NULL) return false;
    *data = expanded;
    *capacity = next;
    return true;
}

static uint32_t load32(const unsigned char *data)
{
    return (uint32_t) data[0]
           | (uint32_t) data[1] << 8
           | (uint32_t) data[2] << 16
           | (uint32_t) data[3] << 24;
}

static size_t lz_hash(const unsigned char *data)
{
    return (load32(data) * UINT32_C(2654435761)) >> (32u - LZ_HASH_BITS);
}

static bool emit_length(unsigned char *output, size_t capacity, size_t *used,
                        size_t length)
{
    while (length >= 255) {
        if (*used >= capacity) return false;
        output[(*used)++] = 255;
        length -= 255;
    }
    if (*used >= capacity) return false;
    output[(*used)++] = (unsigned char) length;
    return true;
}

/* A compact LZ4-shaped block codec kept local to the experiment. It avoids a
   new default runtime dependency and uses only the nearest hashed match. */
static size_t compress_block(const unsigned char *input, size_t length,
                             unsigned char *output, size_t capacity,
                             uint16_t *positions)
{
    memset(positions, 0, LZ_HASH_SIZE * sizeof(*positions));
    size_t anchor = 0, cursor = 0, used = 0;
    while (cursor + LZ_MIN_MATCH <= length) {
        size_t hash = lz_hash(input + cursor);
        uint16_t encoded = positions[hash];
        positions[hash] = cursor < UINT16_MAX
                          ? (uint16_t) (cursor + 1) : UINT16_MAX;
        if (encoded == 0) { cursor++; continue; }
        size_t candidate = (size_t) encoded - 1;
        size_t offset = cursor - candidate;
        if (offset == 0 || offset > UINT16_MAX
            || memcmp(input + candidate, input + cursor,
                      LZ_MIN_MATCH) != 0) {
            cursor++;
            continue;
        }
        size_t match = LZ_MIN_MATCH;
        while (cursor + match < length
               && input[candidate + match] == input[cursor + match]) {
            match++;
        }
        size_t literal = cursor - anchor;
        if (used >= capacity) return 0;
        size_t token_at = used++;
        output[token_at] = (unsigned char) ((literal < 15 ? literal : 15) << 4
                                            | (match - LZ_MIN_MATCH < 15
                                               ? match - LZ_MIN_MATCH : 15));
        if (literal >= 15
            && !emit_length(output, capacity, &used, literal - 15)) return 0;
        if (literal > capacity - used) return 0;
        memcpy(output + used, input + anchor, literal);
        used += literal;
        if (capacity - used < 2) return 0;
        output[used++] = (unsigned char) offset;
        output[used++] = (unsigned char) (offset >> 8);
        if (match - LZ_MIN_MATCH >= 15
            && !emit_length(output, capacity, &used,
                            match - LZ_MIN_MATCH - 15)) return 0;
        cursor += match;
        anchor = cursor;
    }
    size_t literal = length - anchor;
    if (used >= capacity) return 0;
    output[used++] = (unsigned char) ((literal < 15 ? literal : 15) << 4);
    if (literal >= 15
        && !emit_length(output, capacity, &used, literal - 15)) return 0;
    if (literal > capacity - used) return 0;
    memcpy(output + used, input + anchor, literal);
    return used + literal;
}

static bool read_length(const unsigned char *input, size_t length,
                        size_t *cursor, size_t *value)
{
    unsigned byte = 255;
    while (byte == 255) {
        if (*cursor >= length || *value > SIZE_MAX - 255) return false;
        byte = input[(*cursor)++];
        *value += byte;
    }
    return true;
}

static bool decompress_block(const unsigned char *input, size_t length,
                             unsigned char *output, size_t output_length)
{
    size_t source = 0, target = 0;
    while (source < length) {
        unsigned token = input[source++];
        size_t literal = token >> 4;
        if (literal == 15
            && !read_length(input, length, &source, &literal)) return false;
        if (literal > length - source || literal > output_length - target)
            return false;
        memcpy(output + target, input + source, literal);
        source += literal;
        target += literal;
        if (source == length) return target == output_length;
        if (length - source < 2) return false;
        size_t offset = (size_t) input[source]
                        | (size_t) input[source + 1] << 8;
        source += 2;
        if (offset == 0 || offset > target) return false;
        size_t match = (token & 15u) + LZ_MIN_MATCH;
        if ((token & 15u) == 15
            && !read_length(input, length, &source, &match)) return false;
        if (match > output_length - target) return false;
        for (size_t i = 0; i < match; i++) {
            output[target + i] = output[target - offset + i];
        }
        target += match;
    }
    return target == output_length;
}

static bool ascii_equal(const char *data, const char *word, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) data[i])
            != tolower((unsigned char) word[i])) return false;
    }
    return true;
}

static size_t scan_tag_end(const char *html, size_t length, size_t cursor)
{
    char quote = '\0';
    for (; cursor < length; cursor++) {
        char byte = html[cursor];
        if (quote != '\0') {
            if (byte == quote) quote = '\0';
        } else if (byte == '\'' || byte == '"') {
            quote = byte;
        } else if (byte == '>') {
            return cursor + 1;
        }
    }
    return length;
}

static bool tag_name_at(const char *html, size_t length, size_t offset,
                        const char *name, size_t name_length, bool closing,
                        size_t *tag_end)
{
    if (offset >= length || html[offset] != '<') return false;
    size_t cursor = offset + 1;
    while (cursor < length && isspace((unsigned char) html[cursor])) cursor++;
    if (closing) {
        if (cursor >= length || html[cursor] != '/') return false;
        cursor++;
        while (cursor < length && isspace((unsigned char) html[cursor]))
            cursor++;
    } else if (cursor < length && html[cursor] == '/') {
        return false;
    }
    if (name_length > length - cursor
        || !ascii_equal(html + cursor, name, name_length)) return false;
    cursor += name_length;
    if (cursor < length && (isalnum((unsigned char) html[cursor])
                            || html[cursor] == '-' || html[cursor] == '_'))
        return false;
    size_t end = scan_tag_end(html, length, cursor);
    if (end == length && (length == 0 || html[length - 1] != '>')) return false;
    if (tag_end != NULL) *tag_end = end;
    return true;
}

typedef struct {
    uint32_t source_offset;
    uint16_t source_length;
    char name[32];
    uint8_t name_length;
    bool layout_island;
} IndexOpenTag;

static bool append_section_entry(CompressedSectionStore *store, size_t start,
                                 size_t end, unsigned heading_level,
                                 bool continuation,
                                 const IndexOpenTag *context,
                                 size_t context_count)
{
    if (context_count > UINT16_MAX
        || store->context_tag_count > UINT32_MAX
        || context_count > UINT32_MAX - store->context_tag_count
        || context_count > SIZE_MAX - store->context_tag_count
        || !reserve_array(store->budget, (void **) &store->sections,
                          &store->section_capacity, store->section_count + 1,
                          sizeof(*store->sections))
        || !reserve_array(store->budget, (void **) &store->context_tags,
                          &store->context_tag_capacity,
                          store->context_tag_count + context_count,
                          sizeof(*store->context_tags))) return false;
    size_t context_offset = store->context_tag_count;
    for (size_t i = 0; i < context_count; i++) {
        store->context_tags[store->context_tag_count++] =
            (SectionStoreContextTag) {
                .source_offset = context[i].source_offset,
                .source_length = context[i].source_length
            };
    }
    store->sections[store->section_count++] = (SectionStoreEntry) {
        .source_offset = (uint32_t) start,
        .source_length = (uint32_t) (end - start),
        .context_offset = (uint32_t) context_offset,
        .context_count = (uint16_t) context_count,
        .heading_level = (uint8_t) heading_level,
        .continuation = continuation
    };
    return true;
}

static bool append_section(CompressedSectionStore *store, size_t start,
                           size_t end, unsigned heading_level,
                           bool continuation,
                           const IndexOpenTag *context,
                           size_t context_count)
{
    if (end <= start || start > UINT32_MAX || end - start > UINT32_MAX)
        return end <= start;
    /* The indexer still splits ordinary content at maximum_section_bytes.
       This larger hard guard only permits a recognized layout island to be
       carried intact until its closing tag; pathological islands are still
       segmented at a finite boundary. */
    return end - start <= layout_island_section_limit(store)
        && append_section_entry(store, start, end, heading_level,
                                continuation, context, context_count);
}

static uint32_t anchor_hash(const char *value, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) value[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool append_tag_anchor(CompressedSectionStore *store,
                              const char *html, size_t tag_start,
                              size_t tag_end, size_t source_base)
{
    size_t cursor = tag_start + 1;
    while (cursor < tag_end && isspace((unsigned char) html[cursor])) cursor++;
    if (cursor >= tag_end || html[cursor] == '/' || html[cursor] == '!'
        || html[cursor] == '?') return true;
    size_t tag_name_start = cursor;
    while (cursor < tag_end && !isspace((unsigned char) html[cursor])
           && html[cursor] != '>' && html[cursor] != '/') cursor++;
    size_t tag_name_length = cursor - tag_name_start;
    while (cursor < tag_end) {
        while (cursor < tag_end
               && (isspace((unsigned char) html[cursor])
                   || html[cursor] == '/')) cursor++;
        if (cursor >= tag_end || html[cursor] == '>') break;
        size_t name_start = cursor;
        while (cursor < tag_end
               && !isspace((unsigned char) html[cursor])
               && html[cursor] != '=' && html[cursor] != '>'
               && html[cursor] != '/') cursor++;
        size_t name_length = cursor - name_start;
        while (cursor < tag_end && isspace((unsigned char) html[cursor]))
            cursor++;
        if (cursor >= tag_end || html[cursor] != '=') continue;
        cursor++;
        while (cursor < tag_end && isspace((unsigned char) html[cursor]))
            cursor++;
        char quote = '\0';
        if (cursor < tag_end
            && (html[cursor] == '\'' || html[cursor] == '"')) {
            quote = html[cursor++];
        }
        size_t value_start = cursor;
        if (quote != '\0') {
            while (cursor < tag_end && html[cursor] != quote) cursor++;
        } else {
            while (cursor < tag_end
                   && !isspace((unsigned char) html[cursor])
                   && html[cursor] != '>') cursor++;
        }
        size_t value_length = cursor - value_start;
        if (quote != '\0' && cursor < tag_end) cursor++;
        if (name_length == 2 && ascii_equal(html + name_start, "id", 2)
            && value_length != 0 && value_length <= UINT16_MAX
            && source_base + value_start <= UINT32_MAX) {
            if (!reserve_array(
                    store->budget, (void **) &store->anchors,
                    &store->anchor_capacity, store->anchor_count + 1,
                    sizeof(*store->anchors))) return false;
            store->anchors[store->anchor_count++] = (SectionStoreAnchor) {
                .hash = anchor_hash(html + value_start, value_length),
                .value_offset = (uint32_t) (source_base + value_start),
                .tag_name_offset = (uint32_t) (source_base + tag_name_start),
                .value_length = (uint16_t) value_length,
                .tag_name_length = tag_name_length < 32
                    ? (uint8_t) tag_name_length : 0
            };
        }
    }
    return true;
}

static int compare_anchors(const void *left_value, const void *right_value)
{
    const SectionStoreAnchor *left = left_value;
    const SectionStoreAnchor *right = right_value;
    if (left->hash != right->hash) return left->hash < right->hash ? -1 : 1;
    if (left->value_offset != right->value_offset)
        return left->value_offset < right->value_offset ? -1 : 1;
    return 0;
}

static bool extract_range(const CompressedSectionStore *store, size_t offset,
                          size_t length, unsigned char *output,
                          unsigned char *scratch);

static bool tag_identity(const char *tag, size_t length, bool *closing,
                         bool *self_closing, char name[32],
                         size_t *name_length)
{
    if (length < 3 || tag[0] != '<') return false;
    size_t cursor = 1;
    while (cursor < length && isspace((unsigned char) tag[cursor])) cursor++;
    *closing = cursor < length && tag[cursor] == '/';
    if (*closing) {
        cursor++;
        while (cursor < length && isspace((unsigned char) tag[cursor]))
            cursor++;
    }
    size_t start = cursor;
    while (cursor < length && (isalnum((unsigned char) tag[cursor])
                               || tag[cursor] == '-' || tag[cursor] == '_'))
        cursor++;
    size_t count = cursor - start;
    if (count == 0 || count >= 32) return false;
    for (size_t i = 0; i < count; i++)
        name[i] = (char) tolower((unsigned char) tag[start + i]);
    name[count] = '\0';
    size_t tail = length - 1;
    while (tail > cursor && (tag[tail] == '>'
                             || isspace((unsigned char) tag[tail]))) tail--;
    *self_closing = tag[tail] == '/';
    *name_length = count;
    return true;
}

static bool context_ignored_element(const char *name)
{
    static const char *const ignored[] = {
        "area", "base", "br", "col", "embed", "head", "hr", "html",
        "img", "input", "link", "meta", "param", "script", "source",
        "style", "template", "track", "wbr"
    };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        if (strcmp(name, ignored[i]) == 0) return true;
    }
    return false;
}

static bool tag_renders_without_text(const char *name)
{
    static const char *const rendered[] = {
        "audio", "br", "canvas", "embed", "hr", "iframe", "img",
        "input", "object", "progress", "select", "svg", "textarea",
        "video"
    };
    for (size_t i = 0; i < sizeof(rendered) / sizeof(rendered[0]); i++) {
        if (strcmp(name, rendered[i]) == 0) return true;
    }
    return false;
}

static bool tag_has_inline_layout_island(const char *tag, size_t length)
{
    for (size_t at = 0; at + 7 < length; at++) {
        if (!ascii_equal(tag + at, "display", 7)) continue;
        if (at != 0 && tag[at - 1] != ';'
            && tag[at - 1] != '\'' && tag[at - 1] != '"'
            && !isspace((unsigned char) tag[at - 1])) continue;
        size_t cursor = at + 7;
        while (cursor < length && isspace((unsigned char) tag[cursor]))
            cursor++;
        if (cursor >= length || tag[cursor++] != ':') continue;
        while (cursor < length && isspace((unsigned char) tag[cursor]))
            cursor++;
        static const char *const values[] = {
            "flex", "grid", "inline-flex", "inline-grid", "table",
            "inline-table"
        };
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            size_t value_length = strlen(values[i]);
            if (cursor + value_length <= length
                && ascii_equal(tag + cursor, values[i], value_length)
                && (cursor + value_length == length
                    || tag[cursor + value_length] == ';'
                    || tag[cursor + value_length] == '\''
                    || tag[cursor + value_length] == '"'
                    || isspace((unsigned char)
                               tag[cursor + value_length]))) return true;
        }
    }
    return false;
}

typedef enum {
    INDEX_LAYOUT_TAG,
    INDEX_LAYOUT_CLASS,
    INDEX_LAYOUT_ID
} IndexLayoutSelectorKind;

typedef struct {
    IndexLayoutSelectorKind kind;
    char value[32];
    uint8_t length;
} IndexLayoutSelector;

#define INDEX_LAYOUT_SELECTOR_LIMIT 128u

/* Keep the indexer's bounded working set in the browser budget rather than on
   the PSP thread stack.  Apart from avoiding a large entry frame, this makes
   the memory visible to pressure accounting while progress callbacks perform
   parsing/layout work of their own. */
typedef struct {
    IndexLayoutSelector layout_selectors[INDEX_LAYOUT_SELECTOR_LIMIT];
    IndexOpenTag stack[64];
    IndexOpenTag section_context[64];
    IndexOpenTag safe_context[64];
} SectionIndexWorkspace;

static bool css_simple_identifier(const char *value, size_t length)
{
    if (length == 0 || length >= 32) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) value[i];
        if (!isalnum(byte) && byte != '-' && byte != '_') return false;
    }
    return true;
}

static void append_layout_selector(IndexLayoutSelector *selectors,
                                   size_t *selector_count,
                                   IndexLayoutSelectorKind kind,
                                   const char *value, size_t length)
{
    if (!css_simple_identifier(value, length)) return;
    for (size_t i = 0; i < *selector_count; i++) {
        if (selectors[i].kind == kind && selectors[i].length == length
            && memcmp(selectors[i].value, value, length) == 0) return;
    }
    if (*selector_count >= INDEX_LAYOUT_SELECTOR_LIMIT) return;
    IndexLayoutSelector *selector = &selectors[(*selector_count)++];
    selector->kind = kind;
    selector->length = (uint8_t) length;
    memcpy(selector->value, value, length);
    selector->value[length] = '\0';
}

static void append_layout_selector_part(
    IndexLayoutSelector *selectors, size_t *selector_count,
    const char *value, size_t length)
{
    size_t start = 0, square = 0, round = 0;
    for (size_t i = 0; i < length; i++) {
        char byte = value[i];
        if (byte == '[') square++;
        else if (byte == ']' && square != 0) square--;
        else if (byte == '(') round++;
        else if (byte == ')' && round != 0) round--;
        else if (square == 0 && round == 0
                 && (isspace((unsigned char) byte)
                     || byte == '>' || byte == '+' || byte == '~')) {
            start = i + 1;
        }
    }
    while (start < length && isspace((unsigned char) value[start])) start++;
    size_t cursor = start;
    if (cursor < length && value[cursor] != '.' && value[cursor] != '#'
        && value[cursor] != ':' && value[cursor] != '[' && value[cursor] != '*') {
        size_t end = cursor;
        while (end < length && (isalnum((unsigned char) value[end])
                                || value[end] == '-'
                                || value[end] == '_')) end++;
        append_layout_selector(selectors, selector_count, INDEX_LAYOUT_TAG,
                               value + cursor, end - cursor);
        cursor = end;
    }
    while (cursor < length) {
        if (value[cursor] != '.' && value[cursor] != '#') {
            cursor++;
            continue;
        }
        IndexLayoutSelectorKind kind = value[cursor] == '.'
            ? INDEX_LAYOUT_CLASS : INDEX_LAYOUT_ID;
        size_t first = ++cursor;
        while (cursor < length && (isalnum((unsigned char) value[cursor])
                                   || value[cursor] == '-'
                                   || value[cursor] == '_')) cursor++;
        append_layout_selector(selectors, selector_count, kind,
                               value + first, cursor - first);
    }
}

/* This bounded lexical pass recognizes only standalone tag, class, and ID
   selectors. It catches common component containers without claiming to
   evaluate the full selector grammar. */
static void collect_style_layout_selectors_depth(
    const char *css, size_t length, IndexLayoutSelector *selectors,
    size_t *selector_count, unsigned nesting)
{
    size_t rule_start = 0, cursor = 0;
    char quote = '\0';
    bool comment = false;
    while (cursor < length) {
        if (comment) {
            if (cursor + 1 < length && css[cursor] == '*'
                && css[cursor + 1] == '/') {
                comment = false; cursor += 2;
            } else cursor++;
            continue;
        }
        if (quote != '\0') {
            if (css[cursor] == '\\' && cursor + 1 < length) cursor += 2;
            else {
                if (css[cursor] == quote) quote = '\0';
                cursor++;
            }
            continue;
        }
        if (cursor + 1 < length && css[cursor] == '/'
            && css[cursor + 1] == '*') {
            comment = true; cursor += 2; continue;
        }
        if (css[cursor] == '\'' || css[cursor] == '"') {
            quote = css[cursor++]; continue;
        }
        if (css[cursor] != '{') { cursor++; continue; }
        size_t open = cursor++, close = cursor;
        unsigned depth = 1;
        quote = '\0'; comment = false;
        while (close < length && depth != 0) {
            if (comment) {
                if (close + 1 < length && css[close] == '*'
                    && css[close + 1] == '/') {
                    comment = false; close += 2;
                } else close++;
                continue;
            }
            if (quote != '\0') {
                if (css[close] == '\\' && close + 1 < length) close += 2;
                else {
                    if (css[close] == quote) quote = '\0';
                    close++;
                }
                continue;
            }
            if (close + 1 < length && css[close] == '/'
                && css[close + 1] == '*') {
                comment = true; close += 2; continue;
            }
            if (css[close] == '\'' || css[close] == '"') {
                quote = css[close++]; continue;
            }
            if (css[close] == '{') depth++;
            else if (css[close] == '}') depth--;
            close++;
        }
        if (depth != 0) break;
        size_t declaration_end = close - 1;
        size_t prelude = rule_start;
        while (prelude < open
               && isspace((unsigned char) css[prelude])) prelude++;
        if (prelude < open && css[prelude] == '@') {
            if (nesting < 4) {
                collect_style_layout_selectors_depth(
                    css + open + 1, declaration_end - open - 1,
                    selectors, selector_count, nesting + 1);
            }
        } else if (tag_has_inline_layout_island(
                       css + open + 1, declaration_end - open - 1)) {
            size_t part = rule_start;
            while (part < open) {
                size_t comma = part;
                while (comma < open && css[comma] != ',') comma++;
                size_t first = part, last = comma;
                while (first < last
                       && isspace((unsigned char) css[first])) first++;
                while (last > first
                       && isspace((unsigned char) css[last - 1])) last--;
                append_layout_selector_part(
                    selectors, selector_count, css + first, last - first);
                part = comma < open ? comma + 1 : open;
            }
        }
        rule_start = close;
        cursor = close;
    }
}

static void collect_style_layout_selectors(
    const char *css, size_t length, IndexLayoutSelector *selectors,
    size_t *selector_count)
{
    collect_style_layout_selectors_depth(
        css, length, selectors, selector_count, 0);
}

static bool tag_attribute_matches(const char *tag, size_t length,
                                  const char *wanted,
                                  const char *value, size_t value_length,
                                  bool tokenized)
{
    size_t cursor = 1;
    while (cursor < length && isspace((unsigned char) tag[cursor])) cursor++;
    while (cursor < length && !isspace((unsigned char) tag[cursor])
           && tag[cursor] != '>' && tag[cursor] != '/') cursor++;
    while (cursor < length) {
        while (cursor < length
               && (isspace((unsigned char) tag[cursor])
                   || tag[cursor] == '/')) cursor++;
        if (cursor >= length || tag[cursor] == '>') break;
        size_t name_start = cursor;
        while (cursor < length && !isspace((unsigned char) tag[cursor])
               && tag[cursor] != '=' && tag[cursor] != '>'
               && tag[cursor] != '/') cursor++;
        size_t name_length = cursor - name_start;
        while (cursor < length && isspace((unsigned char) tag[cursor]))
            cursor++;
        if (cursor >= length || tag[cursor] != '=') continue;
        cursor++;
        while (cursor < length && isspace((unsigned char) tag[cursor]))
            cursor++;
        char quote = '\0';
        if (cursor < length && (tag[cursor] == '\'' || tag[cursor] == '"'))
            quote = tag[cursor++];
        size_t start = cursor;
        if (quote != '\0') {
            while (cursor < length && tag[cursor] != quote) cursor++;
        } else {
            while (cursor < length && !isspace((unsigned char) tag[cursor])
                   && tag[cursor] != '>') cursor++;
        }
        size_t end = cursor;
        if (quote != '\0' && cursor < length) cursor++;
        size_t wanted_length = strlen(wanted);
        if (name_length != wanted_length
            || !ascii_equal(tag + name_start, wanted, wanted_length)) continue;
        if (!tokenized) {
            if (end - start == value_length
                && memcmp(tag + start, value, value_length) == 0) return true;
            continue;
        }
        size_t token = start;
        while (token < end) {
            while (token < end
                   && isspace((unsigned char) tag[token])) token++;
            size_t token_end = token;
            while (token_end < end
                   && !isspace((unsigned char) tag[token_end])) token_end++;
            if (token_end - token == value_length
                && memcmp(tag + token, value, value_length) == 0) return true;
            token = token_end;
        }
    }
    return false;
}

static bool tag_matches_layout_selector(
    const char *tag, size_t length, const char *name,
    const IndexLayoutSelector *selectors, size_t selector_count)
{
    for (size_t i = 0; i < selector_count; i++) {
        const IndexLayoutSelector *selector = &selectors[i];
        if (selector->kind == INDEX_LAYOUT_TAG) {
            if (strlen(name) == selector->length
                && ascii_equal(name, selector->value, selector->length))
                return true;
        } else if (selector->kind == INDEX_LAYOUT_CLASS) {
            if (tag_attribute_matches(tag, length, "class", selector->value,
                                      selector->length, true)) return true;
        } else if (tag_attribute_matches(tag, length, "id", selector->value,
                                         selector->length, false)) return true;
    }
    return false;
}

enum {
    SELECTOR_SCAN_DATA = 0,
    SELECTOR_SCAN_TAG,
    SELECTOR_SCAN_COMMENT,
    SELECTOR_SCAN_SCRIPT,
    SELECTOR_SCAN_STYLE
};

typedef struct {
    char name[64];
    uint8_t name_length;
    char value[129];
    uint8_t value_length;
    bool has_value;
} SectionSimpleAttribute;

typedef struct {
    char tag[32];
    uint8_t tag_length;
    char id[129];
    uint8_t id_length;
    char classes[4][64];
    uint8_t class_lengths[4];
    size_t class_count;
    SectionSimpleAttribute attributes[4];
    size_t attribute_count;
    bool universal;
} SectionSimpleSelector;

static bool selector_identifier_byte(unsigned char byte, bool attribute)
{
    return isalnum(byte) || byte == '-' || byte == '_'
           || (attribute && byte == ':');
}

static bool parse_selector_identifier(const char *selector, size_t length,
                                      size_t *cursor, char *output,
                                      size_t capacity, size_t *used,
                                      bool attribute)
{
    size_t start = *cursor;
    while (*cursor < length
           && selector_identifier_byte((unsigned char) selector[*cursor],
                                       attribute)) (*cursor)++;
    size_t count = *cursor - start;
    if (count == 0 || count >= capacity) return false;
    memcpy(output, selector + start, count);
    output[count] = '\0';
    *used = count;
    return true;
}

static bool parse_simple_selector(const char *selector, size_t length,
                                  SectionSimpleSelector *parsed)
{
    if (selector == NULL || parsed == NULL || length == 0 || length > 128)
        return false;
    memset(parsed, 0, sizeof(*parsed));
    size_t first = 0, last = length;
    while (first < last && isspace((unsigned char) selector[first])) first++;
    while (last > first && isspace((unsigned char) selector[last - 1])) last--;
    if (first == last) return false;
    size_t cursor = first;
    if (selector[cursor] == '*') {
        parsed->universal = true;
        cursor++;
    } else if (selector[cursor] != '.' && selector[cursor] != '#'
               && selector[cursor] != '[') {
        size_t used = 0;
        if (!parse_selector_identifier(selector, last, &cursor,
                                       parsed->tag, sizeof(parsed->tag),
                                       &used, false)) return false;
        parsed->tag_length = (uint8_t) used;
        for (size_t i = 0; i < used; i++) {
            parsed->tag[i] = (char) tolower(
                (unsigned char) parsed->tag[i]);
        }
    }
    while (cursor < last) {
        char marker = selector[cursor++];
        if (marker == '.' || marker == '#') {
            char *output = marker == '#'
                ? parsed->id : parsed->classes[parsed->class_count];
            size_t capacity = marker == '#' ? sizeof(parsed->id)
                : sizeof(parsed->classes[0]);
            size_t used = 0;
            if ((marker == '#' && parsed->id_length != 0)
                || (marker == '.' && parsed->class_count >= 4)
                || !parse_selector_identifier(selector, last, &cursor,
                                               output, capacity, &used,
                                               false)) return false;
            if (marker == '#') parsed->id_length = (uint8_t) used;
            else {
                parsed->class_lengths[parsed->class_count] = (uint8_t) used;
                parsed->class_count++;
            }
            continue;
        }
        if (marker != '[' || parsed->attribute_count >= 4) return false;
        while (cursor < last
               && isspace((unsigned char) selector[cursor])) cursor++;
        SectionSimpleAttribute *attribute =
            &parsed->attributes[parsed->attribute_count];
        size_t name_length = 0;
        if (!parse_selector_identifier(
                selector, last, &cursor, attribute->name,
                sizeof(attribute->name), &name_length, true)) return false;
        attribute->name_length = (uint8_t) name_length;
        for (size_t i = 0; i < name_length; i++) {
            attribute->name[i] = (char) tolower(
                (unsigned char) attribute->name[i]);
        }
        while (cursor < last
               && isspace((unsigned char) selector[cursor])) cursor++;
        if (cursor < last && selector[cursor] == '=') {
            cursor++;
            while (cursor < last
                   && isspace((unsigned char) selector[cursor])) cursor++;
            char quote = '\0';
            if (cursor < last
                && (selector[cursor] == '\'' || selector[cursor] == '"')) {
                quote = selector[cursor++];
            }
            size_t value_start = cursor;
            if (quote != '\0') {
                while (cursor < last && selector[cursor] != quote
                       && selector[cursor] != '\\') cursor++;
                if (cursor >= last || selector[cursor] != quote) return false;
            } else {
                while (cursor < last && selector[cursor] != ']'
                       && !isspace((unsigned char) selector[cursor])) cursor++;
            }
            size_t value_length = cursor - value_start;
            if (value_length >= sizeof(attribute->value)) return false;
            memcpy(attribute->value, selector + value_start, value_length);
            attribute->value[value_length] = '\0';
            attribute->value_length = (uint8_t) value_length;
            attribute->has_value = true;
            if (quote != '\0') cursor++;
            while (cursor < last
                   && isspace((unsigned char) selector[cursor])) cursor++;
        }
        if (cursor >= last || selector[cursor++] != ']') return false;
        parsed->attribute_count++;
    }
    return parsed->universal || parsed->tag_length != 0
           || parsed->id_length != 0 || parsed->class_count != 0
           || parsed->attribute_count != 0;
}

bool section_store_selector_is_simple(const char *selector, size_t length)
{
    SectionSimpleSelector parsed;
    return parse_simple_selector(selector, length, &parsed);
}

static uint64_t selector_token_hash(char kind, const char *name,
                                    size_t name_length, const char *value,
                                    size_t value_length, bool fold_name)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    hash ^= (unsigned char) kind;
    hash *= UINT64_C(1099511628211);
    for (size_t i = 0; i < name_length; i++) {
        unsigned char byte = (unsigned char) name[i];
        if (fold_name) byte = (unsigned char) tolower(byte);
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    if (value != NULL) {
        hash ^= 0;
        hash *= UINT64_C(1099511628211);
        for (size_t i = 0; i < value_length; i++) {
            hash ^= (unsigned char) value[i];
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static void selector_bloom_add(uint64_t bloom[4], uint64_t hash)
{
    uint64_t mixed = hash;
    for (unsigned i = 0; i < 3; i++) {
        mixed ^= mixed >> 33;
        mixed *= UINT64_C(0xff51afd7ed558ccd);
        mixed ^= mixed >> 29;
        unsigned bit = (unsigned) (mixed & 255u);
        bloom[bit / 64] |= UINT64_C(1) << (bit % 64);
        mixed += UINT64_C(0x9e3779b97f4a7c15);
    }
}

static bool selector_bloom_maybe(const uint64_t bloom[4], uint64_t hash)
{
    uint64_t mixed = hash;
    for (unsigned i = 0; i < 3; i++) {
        mixed ^= mixed >> 33;
        mixed *= UINT64_C(0xff51afd7ed558ccd);
        mixed ^= mixed >> 29;
        unsigned bit = (unsigned) (mixed & 255u);
        if ((bloom[bit / 64] & (UINT64_C(1) << (bit % 64))) == 0)
            return false;
        mixed += UINT64_C(0x9e3779b97f4a7c15);
    }
    return true;
}

static bool tag_next_attribute(const char *tag, size_t length,
                               size_t *cursor, const char **name,
                               size_t *name_length, const char **value,
                               size_t *value_length, bool *has_value)
{
    while (*cursor < length
           && (isspace((unsigned char) tag[*cursor])
               || tag[*cursor] == '/')) (*cursor)++;
    if (*cursor >= length || tag[*cursor] == '>') return false;
    size_t start = *cursor;
    while (*cursor < length && !isspace((unsigned char) tag[*cursor])
           && tag[*cursor] != '=' && tag[*cursor] != '>'
           && tag[*cursor] != '/') (*cursor)++;
    *name = tag + start;
    *name_length = *cursor - start;
    while (*cursor < length
           && isspace((unsigned char) tag[*cursor])) (*cursor)++;
    *value = NULL;
    *value_length = 0;
    *has_value = *cursor < length && tag[*cursor] == '=';
    if (!*has_value) return *name_length != 0;
    (*cursor)++;
    while (*cursor < length
           && isspace((unsigned char) tag[*cursor])) (*cursor)++;
    char quote = '\0';
    if (*cursor < length
        && (tag[*cursor] == '\'' || tag[*cursor] == '"')) {
        quote = tag[(*cursor)++];
    }
    start = *cursor;
    if (quote != '\0') {
        while (*cursor < length && tag[*cursor] != quote) (*cursor)++;
    } else {
        while (*cursor < length && !isspace((unsigned char) tag[*cursor])
               && tag[*cursor] != '>') (*cursor)++;
    }
    *value = tag + start;
    *value_length = *cursor - start;
    if (quote != '\0' && *cursor < length) (*cursor)++;
    return *name_length != 0;
}

static void selector_bloom_tag(uint64_t bloom[4], const char *tag,
                               size_t length)
{
    bool closing = false, self_closing = false;
    char name[32];
    size_t name_length = 0;
    if (!tag_identity(tag, length, &closing, &self_closing,
                      name, &name_length) || closing) return;
    (void) self_closing;
    selector_bloom_add(bloom, selector_token_hash(
        't', name, name_length, NULL, 0, true));
    size_t cursor = 1;
    while (cursor < length && isspace((unsigned char) tag[cursor])) cursor++;
    while (cursor < length && !isspace((unsigned char) tag[cursor])
           && tag[cursor] != '>' && tag[cursor] != '/') cursor++;
    const char *attribute_name = NULL, *value = NULL;
    size_t attribute_length = 0, value_length = 0;
    bool has_value = false;
    while (tag_next_attribute(tag, length, &cursor, &attribute_name,
                              &attribute_length, &value, &value_length,
                              &has_value)) {
        selector_bloom_add(bloom, selector_token_hash(
            'a', attribute_name, attribute_length, NULL, 0, true));
        if (has_value) {
            selector_bloom_add(bloom, selector_token_hash(
                'v', attribute_name, attribute_length, value, value_length,
                true));
        }
        if (attribute_length == 2
            && ascii_equal(attribute_name, "id", 2) && has_value) {
            selector_bloom_add(bloom, selector_token_hash(
                'i', value, value_length, NULL, 0, false));
        } else if (attribute_length == 5
                   && ascii_equal(attribute_name, "class", 5)
                   && has_value) {
            size_t token = 0;
            while (token < value_length) {
                while (token < value_length
                       && isspace((unsigned char) value[token])) token++;
                size_t end = token;
                while (end < value_length
                       && !isspace((unsigned char) value[end])) end++;
                if (end > token) selector_bloom_add(
                    bloom, selector_token_hash(
                        'c', value + token, end - token, NULL, 0, false));
                token = end;
            }
        }
    }
}

static bool index_section_selector_blooms(CompressedSectionStore *store)
{
    uint64_t slice_started = tilefinch_platform_monotonic_time_us();
    if (store->section_count > SIZE_MAX / (4 * sizeof(uint64_t))) return false;
    store->selector_blooms = budget_calloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->section_count * 4,
        sizeof(uint64_t));
    store->selector_states = budget_calloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->section_count,
        sizeof(uint8_t));
    unsigned char *scratch = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes);
    char *tag = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes + 1);
    if (store->selector_blooms == NULL || store->selector_states == NULL
        || scratch == NULL || tag == NULL) goto failed;
    int state = SELECTOR_SCAN_DATA;
    size_t section = 0, tag_used = 0, tag_start = 0;
    bool tag_overflow = false, tag_from_raw = false;
    char quote = '\0';
    unsigned comment_dashes = 0;
    const char *raw_name = NULL;
    size_t raw_name_length = 0;
    for (size_t block_index = 0; block_index < store->block_count;
         block_index++) {
        const SectionStoreBlock *block = &store->blocks[block_index];
        const unsigned char *source = block->data;
        const char *html = (const char *) source;
        if (block->compressed) {
            if (!decompress_block(source, block->stored_length, scratch,
                                  block->source_length)) goto failed;
            html = (const char *) scratch;
        }
        for (size_t cursor = 0; cursor < block->source_length; cursor++) {
            size_t source_offset = block->source_offset + cursor;
            if (section == 0
                && source_offset == store->sections[0].source_offset) {
                store->selector_states[0] = (uint8_t) state;
            }
            while (section + 1 < store->section_count
                   && source_offset
                        >= store->sections[section + 1].source_offset) {
                section++;
                store->selector_states[section] = (uint8_t) state;
            }
            char byte = html[cursor];
            if (state == SELECTOR_SCAN_COMMENT) {
                if (byte == '-') {
                    if (comment_dashes < 2) comment_dashes++;
                } else if (byte == '>' && comment_dashes >= 2) {
                    state = SELECTOR_SCAN_DATA;
                    comment_dashes = 0;
                } else comment_dashes = 0;
                continue;
            }
            if (state != SELECTOR_SCAN_TAG) {
                if (state == SELECTOR_SCAN_SCRIPT
                    || state == SELECTOR_SCAN_STYLE) {
                    if (byte != '<') continue;
                    tag_from_raw = true;
                } else {
                    if (byte != '<') continue;
                    tag_from_raw = false;
                }
                state = SELECTOR_SCAN_TAG;
                tag_start = source_offset;
                tag_used = 0;
                tag_overflow = false;
                quote = '\0';
            }
            if (tag_used < store->block_bytes) tag[tag_used++] = byte;
            else tag_overflow = true;
            if (!tag_from_raw && tag_used == 4
                && memcmp(tag, "<!--", 4) == 0) {
                state = SELECTOR_SCAN_COMMENT;
                comment_dashes = 0;
                continue;
            }
            if (quote != '\0') {
                if (byte == quote) quote = '\0';
                continue;
            }
            if (byte == '\'' || byte == '"') { quote = byte; continue; }
            if (byte != '>') continue;
            if (!tag_overflow) tag[tag_used] = '\0';
            if (tag_from_raw) {
                bool closes = !tag_overflow && raw_name != NULL
                    && tag_name_at(tag, tag_used, 0, raw_name,
                                   raw_name_length, true, NULL);
                state = closes ? SELECTOR_SCAN_DATA
                    : (raw_name_length == 6 ? SELECTOR_SCAN_SCRIPT
                                            : SELECTOR_SCAN_STYLE);
                if (closes) { raw_name = NULL; raw_name_length = 0; }
                continue;
            }
            if (!tag_overflow && tag_start >= store->prefix_length) {
                size_t owner = section;
                while (owner + 1 < store->section_count
                       && tag_start >= (size_t) store->sections[owner]
                                                .source_offset
                                      + store->sections[owner].source_length) {
                    owner++;
                }
                selector_bloom_tag(store->selector_blooms + owner * 4,
                                   tag, tag_used);
            }
            if (!tag_overflow
                && tag_name_at(tag, tag_used, 0, "script", 6,
                               false, NULL)) {
                raw_name = "script"; raw_name_length = 6;
                state = SELECTOR_SCAN_SCRIPT;
            } else if (!tag_overflow
                       && tag_name_at(tag, tag_used, 0, "style", 5,
                                      false, NULL)) {
                raw_name = "style"; raw_name_length = 5;
                state = SELECTOR_SCAN_STYLE;
            } else state = SELECTOR_SCAN_DATA;
        }
        uint64_t slice_finished = tilefinch_platform_monotonic_time_us();
        uint64_t slice_us = slice_finished >= slice_started
            ? slice_finished - slice_started : 0;
        store->selector_index_work_units++;
        store->selector_index_cooperative_yields++;
        if (slice_us > store->selector_index_max_slice_us)
            store->selector_index_max_slice_us = slice_us;
        if (!tilefinch_platform_cooperate(
                "section-selector-index", block_index + 1)) goto failed;
        slice_started = tilefinch_platform_monotonic_time_us();
    }
    budget_free(store->budget, scratch);
    budget_free(store->budget, tag);
    return true;
failed:
    budget_free(store->budget, scratch);
    budget_free(store->budget, tag);
    budget_free(store->budget, store->selector_blooms);
    budget_free(store->budget, store->selector_states);
    store->selector_blooms = NULL;
    store->selector_states = NULL;
    return false;
}

static bool name_is_structural_layout_island(const char *name)
{
    static const char *const structural[] = {
        "colgroup", "dl", "fieldset", "ol", "optgroup", "select",
        "table", "tbody", "tfoot", "thead", "tr", "ul"
    };
    for (size_t i = 0; i < sizeof(structural) / sizeof(structural[0]); i++) {
        if (strcmp(name, structural[i]) == 0) return true;
    }
    return false;
}

static bool context_is_layout_island(const IndexOpenTag *stack, size_t depth)
{
    for (size_t at = 0; at < depth; at++) {
        if (stack[at].layout_island) return true;
        if (name_is_structural_layout_island(stack[at].name)) return true;
    }
    return false;
}

static void context_pop(IndexOpenTag *stack, size_t *depth,
                        const char *name)
{
    for (size_t i = *depth; i != 0; i--) {
        if (strcmp(stack[i - 1].name, name) != 0) continue;
        *depth = i - 1;
        return;
    }
}

static bool index_completed_tag(CompressedSectionStore *store,
                                const char *tag, size_t length,
                                size_t source_offset, bool *body_found,
                                size_t *section_start,
                                unsigned *section_level,
                                const char **raw_name,
                                size_t *raw_name_length,
                                IndexOpenTag *stack, size_t *stack_depth,
                                IndexOpenTag *section_context,
                                size_t *section_context_count,
                                bool *section_continuation,
                                bool *section_has_content,
                                size_t *section_element_count,
                                const IndexLayoutSelector *layout_selectors,
                                size_t layout_selector_count)
{
    size_t tag_end = 0;
    if (!*body_found) {
        if (tag_name_at(tag, length, 0, "script", 6, false, NULL)) {
            *raw_name = "script";
            *raw_name_length = 6;
            return true;
        }
        if (tag_name_at(tag, length, 0, "style", 5, false, NULL)) {
            *raw_name = "style";
            *raw_name_length = 5;
            return true;
        }
        if (!tag_name_at(tag, length, 0, "body", 4, false, &tag_end))
            return true;
        *body_found = true;
        *section_start = source_offset + tag_end;
        store->prefix_length = *section_start;
        return true;
    }
    bool closing = false, self_closing = false;
    char name[32];
    size_t name_length = 0;
    bool identified = tag_identity(tag, length, &closing, &self_closing,
                                   name, &name_length);
    bool opens_layout_island = identified && !closing && !self_closing
        && (name_is_structural_layout_island(name)
            || tag_has_inline_layout_island(tag, length)
            || tag_matches_layout_selector(
                   tag, length, name, layout_selectors,
                   layout_selector_count));
    bool inside_layout_island = context_is_layout_island(
        stack, *stack_depth);
    size_t active_element_limit = inside_layout_island
        ? LAYOUT_ISLAND_ELEMENT_LIMIT : SECTION_ELEMENT_LIMIT;
    if (identified && !closing
        && *section_element_count >= active_element_limit
        && *section_has_content && source_offset > *section_start) {
        if (!append_section(store, *section_start, source_offset,
                            *section_level, *section_continuation,
                            section_context, *section_context_count)) {
            return false;
        }
        *section_start = source_offset;
        memcpy(section_context, stack,
               *stack_depth * sizeof(*section_context));
        *section_context_count = *stack_depth;
        *section_continuation = true;
        *section_has_content = false;
        *section_element_count = 0;
    }
    if (opens_layout_island && *section_has_content
        && source_offset > *section_start
        && (source_offset - *section_start
                >= store->maximum_section_bytes / 2
            || *section_element_count >= SECTION_ELEMENT_LIMIT / 2)
        && !context_is_layout_island(stack, *stack_depth)) {
        if (!append_section(store, *section_start, source_offset,
                            *section_level, *section_continuation,
                            section_context, *section_context_count)) {
            return false;
        }
        *section_start = source_offset;
        memcpy(section_context, stack,
               *stack_depth * sizeof(*section_context));
        *section_context_count = *stack_depth;
        *section_continuation = false;
        *section_has_content = false;
        *section_element_count = 0;
    }
    if (!append_tag_anchor(store, tag, 0, length, source_offset)) return false;
    if (identified && closing) {
        context_pop(stack, stack_depth, name);
        return true;
    }
    if (tag_name_at(tag, length, 0, "script", 6, false, NULL)) {
        (*section_element_count)++;
        *raw_name = "script";
        *raw_name_length = 6;
        return true;
    }
    if (tag_name_at(tag, length, 0, "style", 5, false, NULL)) {
        (*section_element_count)++;
        *raw_name = "style";
        *raw_name_length = 5;
        return true;
    }
    for (unsigned level = 1; level <= 6; level++) {
        char heading[3] = {'h', (char) ('0' + level), '\0'};
        if (!tag_name_at(tag, length, 0, heading, 2, false, NULL)) continue;
        context_pop(stack, stack_depth, "p");
        if (*section_has_content
            && !context_is_layout_island(stack, *stack_depth)) {
            if (!append_section(store, *section_start, source_offset,
                                *section_level, *section_continuation,
                                section_context,
                                *section_context_count)) return false;
            *section_start = source_offset;
            memcpy(section_context, stack,
                   *stack_depth * sizeof(*section_context));
            *section_context_count = *stack_depth;
            *section_continuation = false;
            *section_element_count = 0;
        }
        *section_level = level;
        *section_has_content = true;
        break;
    }
    if (identified && !closing) (*section_element_count)++;
    if (identified && !closing && tag_renders_without_text(name)) {
        *section_has_content = true;
    }
    if (identified && !self_closing && strcmp(name, "body") != 0
        && !context_ignored_element(name) && length <= UINT16_MAX
        && *stack_depth < 64) {
        IndexOpenTag *open = &stack[(*stack_depth)++];
        open->source_offset = (uint32_t) source_offset;
        open->source_length = (uint16_t) length;
        memcpy(open->name, name, name_length + 1);
        open->name_length = (uint8_t) name_length;
        open->layout_island = opens_layout_island;
    }
    return true;
}

static bool index_compressed_sections(
    CompressedSectionStore *store, const char *const *external_selectors,
    size_t external_selector_count, SectionStoreProgressCallback on_progress,
    void *progress_opaque)
{
    uint64_t index_started = tilefinch_platform_monotonic_time_us();
    uint64_t slice_started = index_started;
    unsigned char *scratch = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes);
    char *tag = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes + 1);
    char *style = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes + 1);
    SectionIndexWorkspace *workspace = budget_calloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*workspace));
    if (scratch == NULL || tag == NULL || style == NULL || workspace == NULL)
        goto failed;
    enum { INDEX_DATA, INDEX_TAG, INDEX_COMMENT, INDEX_RAW } state = INDEX_DATA;
    bool body_found = false;
    size_t section_start = 0;
    unsigned section_level = 0;
    size_t tag_start = 0, tag_used = 0;
    bool tag_overflow = false, tag_from_raw = false;
    char quote = '\0';
    unsigned comment_dashes = 0;
    const char *raw_name = NULL;
    size_t raw_name_length = 0;
    bool raw_is_style = false, style_overflow = false;
    size_t style_used = 0;
    IndexLayoutSelector *layout_selectors = workspace->layout_selectors;
    size_t layout_selector_count = 0;
    for (size_t i = 0; i < external_selector_count; i++) {
        if (external_selectors == NULL || external_selectors[i] == NULL)
            continue;
        append_layout_selector_part(
            layout_selectors, &layout_selector_count, external_selectors[i],
            strlen(external_selectors[i]));
    }
    IndexOpenTag *stack = workspace->stack;
    IndexOpenTag *section_context = workspace->section_context;
    IndexOpenTag *safe_context = workspace->safe_context;
    size_t stack_depth = 0, section_context_count = 0;
    size_t safe_context_count = 0, safe_offset = 0;
    bool safe_offset_known = false, section_continuation = false;
    bool section_has_content = false;
    size_t section_element_count = 0;
    for (size_t block_index = 0; block_index < store->block_count;
         block_index++) {
        const SectionStoreBlock *block = &store->blocks[block_index];
        const unsigned char *source = block->data;
        const char *html = (const char *) source;
        if (block->compressed) {
            if (!decompress_block(source, block->stored_length, scratch,
                                  block->source_length)) goto failed;
            html = (const char *) scratch;
        }
        size_t length = block->source_length;
        for (size_t cursor = 0; cursor < length; cursor++) {
            char byte = html[cursor];
            size_t source_offset = block->source_offset + cursor;
            if (body_found && state == INDEX_DATA && byte == '<'
                && !context_is_layout_island(stack, stack_depth)) {
                safe_offset = source_offset;
                memcpy(safe_context, stack,
                       stack_depth * sizeof(*safe_context));
                safe_context_count = stack_depth;
                safe_offset_known = true;
            }
            bool inside_layout_island = body_found
                && context_is_layout_island(stack, stack_depth);
            size_t active_section_limit = inside_layout_island
                ? layout_island_section_limit(store)
                : store->maximum_section_bytes;
            if (body_found && source_offset >= section_start
                && source_offset - section_start >= active_section_limit) {
                size_t split = source_offset;
                const IndexOpenTag *next_context = stack;
                size_t next_context_count = stack_depth;
                if (safe_offset_known && safe_offset > section_start
                    && safe_offset - section_start
                         >= store->maximum_section_bytes / 2) {
                    split = safe_offset;
                    next_context = safe_context;
                    next_context_count = safe_context_count;
                }
                if (!append_section(
                        store, section_start, split, section_level,
                        section_continuation, section_context,
                        section_context_count)) goto failed;
                section_start = split;
                memcpy(section_context, next_context,
                       next_context_count * sizeof(*section_context));
                section_context_count = next_context_count;
                section_continuation = true;
                section_has_content = source_offset > split;
                section_element_count = 0;
                safe_offset_known = false;
            }
            if (state == INDEX_COMMENT) {
                if (byte == '-') {
                    if (comment_dashes < 2) comment_dashes++;
                } else if (byte == '>' && comment_dashes >= 2) {
                    state = INDEX_DATA;
                    comment_dashes = 0;
                } else {
                    comment_dashes = 0;
                }
                continue;
            }
            if (state == INDEX_RAW) {
                if (byte != '<') {
                    if (raw_is_style) {
                        if (style_used < store->block_bytes)
                            style[style_used++] = byte;
                        else style_overflow = true;
                    }
                    continue;
                }
                state = INDEX_TAG;
                tag_start = source_offset;
                tag_used = 0;
                tag_overflow = false;
                tag_from_raw = true;
                quote = '\0';
            } else if (state == INDEX_DATA) {
                if (byte != '<') {
                    if (body_found && !isspace((unsigned char) byte))
                        section_has_content = true;
                    continue;
                }
                state = INDEX_TAG;
                tag_start = source_offset;
                tag_used = 0;
                tag_overflow = false;
                tag_from_raw = false;
                quote = '\0';
            }
            if (tag_used < store->block_bytes) tag[tag_used++] = byte;
            else tag_overflow = true;
            if (!tag_from_raw && tag_used == 4
                && memcmp(tag, "<!--", 4) == 0) {
                state = INDEX_COMMENT;
                comment_dashes = 0;
                continue;
            }
            if (quote != '\0') {
                if (byte == quote) quote = '\0';
                continue;
            }
            if (byte == '\'' || byte == '"') {
                quote = byte;
                continue;
            }
            if (byte != '>') continue;
            if (!tag_overflow) tag[tag_used] = '\0';
            if (tag_from_raw) {
                bool closes_raw = !tag_overflow && raw_name != NULL
                    && tag_name_at(tag, tag_used, 0, raw_name,
                                   raw_name_length, true, NULL);
                if (closes_raw) {
                    if (raw_is_style && !style_overflow) {
                        collect_style_layout_selectors(
                            style, style_used, layout_selectors,
                            &layout_selector_count);
                    }
                    raw_name = NULL;
                    raw_name_length = 0;
                    raw_is_style = false;
                    state = INDEX_DATA;
                } else {
                    state = INDEX_RAW;
                }
            } else {
                const char *next_raw_name = NULL;
                size_t next_raw_name_length = 0;
                if (!tag_overflow
                    && !index_completed_tag(
                        store, tag, tag_used, tag_start, &body_found,
                        &section_start, &section_level, &next_raw_name,
                        &next_raw_name_length, stack, &stack_depth,
                        section_context, &section_context_count,
                        &section_continuation, &section_has_content,
                        &section_element_count,
                        layout_selectors,
                        layout_selector_count)) goto failed;
                if (next_raw_name != NULL) {
                    raw_name = next_raw_name;
                    raw_name_length = next_raw_name_length;
                    raw_is_style = raw_name_length == 5
                        && ascii_equal(raw_name, "style", 5);
                    style_used = 0;
                    style_overflow = false;
                    state = INDEX_RAW;
                } else {
                    state = INDEX_DATA;
                }
            }
        }
        uint64_t slice_finished = tilefinch_platform_monotonic_time_us();
        uint64_t slice_us = slice_finished >= slice_started
            ? slice_finished - slice_started : 0;
        store->index_work_units++;
        if (slice_us > store->index_max_slice_us)
            store->index_max_slice_us = slice_us;
        if (store->section_count != 0
            && store->first_section_ready_us == 0) {
            store->first_section_ready_us = slice_finished >= index_started
                ? slice_finished - index_started : 0;
        }
        store->index_cooperative_yields++;
        if (!tilefinch_platform_cooperate(
                "section-index", block_index + 1)) goto failed;
        if (on_progress != NULL) {
            uint64_t callback_started = tilefinch_platform_monotonic_time_us();
            bool keep_going = on_progress(
                progress_opaque, store,
                (size_t) block->source_offset + block->source_length,
                false);
            store->progress_callback_us +=
                tilefinch_platform_monotonic_time_us() - callback_started;
            if (!keep_going) goto failed;
        }
        slice_started = tilefinch_platform_monotonic_time_us();
    }
    if (!body_found) section_start = 0;
    if (!append_section(store, section_start, store->source_length,
                        section_level, section_continuation, section_context,
                        section_context_count)) goto failed;
    if (store->first_section_ready_us == 0) {
        uint64_t ready = tilefinch_platform_monotonic_time_us();
        store->first_section_ready_us = ready >= index_started
            ? ready - index_started : 0;
    }
    uint64_t phase_started = tilefinch_platform_monotonic_time_us();
    qsort(store->anchors, store->anchor_count, sizeof(*store->anchors),
          compare_anchors);
    store->anchor_sort_us +=
        tilefinch_platform_monotonic_time_us() - phase_started;
    if (store->anchor_count != 0
        && store->anchor_capacity > store->anchor_count) {
        SectionStoreAnchor *trimmed = budget_realloc_category(
            store->budget, BUDGET_CATEGORY_RESOURCE, store->anchors,
            store->anchor_count * sizeof(*store->anchors));
        if (trimmed != NULL) {
            store->anchors = trimmed;
            store->anchor_capacity = store->anchor_count;
        }
    }
    store->index_cooperative_yields++;
    if (!tilefinch_platform_cooperate(
            "section-index", store->block_count + 1)) goto failed;
    if (on_progress != NULL) {
        uint64_t callback_started = tilefinch_platform_monotonic_time_us();
        bool keep_going = on_progress(
            progress_opaque, store, store->source_length, true);
        store->progress_callback_us +=
            tilefinch_platform_monotonic_time_us() - callback_started;
        if (!keep_going) goto failed;
    }
    budget_free(store->budget, scratch);
    budget_free(store->budget, tag);
    budget_free(store->budget, style);
    budget_free(store->budget, workspace);
    return store->section_count != 0;

failed:
    budget_free(store->budget, scratch);
    budget_free(store->budget, tag);
    budget_free(store->budget, style);
    budget_free(store->budget, workspace);
    return false;
}

static bool store_pending_block(SectionStoreStreamBuilder *builder)
{
    CompressedSectionStore *store = builder->store;
    size_t raw_length = builder->pending_length;
    if (raw_length == 0) return true;
    uint64_t started = tilefinch_platform_monotonic_time_us();
    size_t compressed = compress_block(builder->pending, raw_length,
                                       builder->temporary,
                                       builder->temporary_capacity,
                                       builder->positions);
    bool use_compressed = compressed != 0 && compressed < raw_length;
    size_t stored_length = use_compressed ? compressed : raw_length;
    if (store->source_length > UINT32_MAX - raw_length) return false;
    unsigned char *data = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, stored_length);
    if (data == NULL) return false;
    if (!reserve_array(store->budget, (void **) &store->blocks,
                       &store->block_capacity, store->block_count + 1,
                       sizeof(*store->blocks))) {
        budget_free(store->budget, data);
        return false;
    }
    memcpy(data, use_compressed ? builder->temporary : builder->pending,
           stored_length);
    store->blocks[store->block_count++] = (SectionStoreBlock) {
            .data = data,
            .source_offset = (uint32_t) store->source_length,
            .source_length = (uint32_t) raw_length,
            .stored_length = (uint32_t) stored_length,
            .compressed = use_compressed
    };
    store->source_length += raw_length;
    store->stored_length += stored_length;
    if (use_compressed) store->compressed_blocks++;
    else store->raw_blocks++;
    store->compression_us += tilefinch_platform_monotonic_time_us() - started;
    builder->pending_length = 0;
    return true;
}

bool section_store_stream_begin(SectionStoreStreamBuilder *builder,
                                CompressedSectionStore *store,
                                Budget *budget, size_t block_bytes,
                                size_t maximum_section_bytes)
{
    if (builder == NULL) return false;
    /* A failed begin is still safe to pass to abort.  Several transactional
       callers deliberately use that uniform cleanup path, so establish the
       empty state before validating the remaining arguments. */
    memset(builder, 0, sizeof(*builder));
    if (store == NULL || budget == NULL
        || block_bytes < 4096 || block_bytes > UINT16_MAX
        || maximum_section_bytes < block_bytes
        || maximum_section_bytes > UINT32_MAX) return false;
    memset(store, 0, sizeof(*store));
    builder->started_ns = tilefinch_platform_monotonic_time_ns();
    store->budget = budget;
    store->index_revision = 1;
    store->block_bytes = block_bytes;
    store->maximum_section_bytes = maximum_section_bytes;
    builder->store = store;
    builder->temporary_capacity = block_bytes + block_bytes / 255 + 32;
    builder->pending = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, block_bytes);
    builder->temporary = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, builder->temporary_capacity);
    builder->positions = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, LZ_HASH_SIZE * sizeof(*builder->positions));
    if (builder->pending == NULL || builder->temporary == NULL
        || builder->positions == NULL) {
        section_store_stream_abort(builder);
        return false;
    }
    builder->active = true;
    return true;
}

bool section_store_stream_append(SectionStoreStreamBuilder *builder,
                                 const unsigned char *data, size_t length)
{
    if (builder == NULL || !builder->active
        || (data == NULL && length != 0)) return false;
    while (length != 0) {
        size_t room = builder->store->block_bytes - builder->pending_length;
        size_t copy = length < room ? length : room;
        memcpy(builder->pending + builder->pending_length, data, copy);
        builder->pending_length += copy;
        data += copy;
        length -= copy;
        if (builder->pending_length == builder->store->block_bytes
            && !store_pending_block(builder)) return false;
    }
    return true;
}

bool section_store_stream_seal_section(
    SectionStoreStreamBuilder *builder, size_t prefix_length,
    size_t source_start, size_t source_end)
{
    if (builder == NULL || !builder->active || builder->store == NULL
        || prefix_length > source_start || source_end <= source_start
        || source_end - source_start
               > builder->store->maximum_section_bytes
        || source_end > builder->store->source_length
                           + builder->pending_length
        || (builder->store->sealed_section_count != 0
            && source_start != builder->store->sealed_source_bytes)
        || !store_pending_block(builder)
        || source_end > builder->store->source_length) return false;
    CompressedSectionStore *store = builder->store;
    if (store->sealed_section_count == 0) {
        store->prefix_length = prefix_length;
    } else if (store->prefix_length != prefix_length) {
        return false;
    }
    if (!append_section_entry(
            store, source_start, source_end, 0, false, NULL, 0)) {
        return false;
    }
    section_store_bump_index_revision(store);
    store->sealed_section_count = store->section_count;
    store->sealed_source_bytes = source_end;
    if (store->first_section_ready_us == 0) {
        uint64_t ready_ns = tilefinch_platform_monotonic_time_ns();
        uint64_t elapsed_ns = ready_ns >= builder->started_ns
            ? ready_ns - builder->started_ns : 0;
        /* Round a genuine sub-microsecond event upward so zero continues to
           mean that no section has become materializable. */
        store->first_section_ready_us =
            (elapsed_ns + UINT64_C(999)) / UINT64_C(1000);
        if (store->first_section_ready_us == 0) {
            store->first_section_ready_us = 1;
        }
    }
    if (builder->on_progress != NULL) {
        uint64_t started = tilefinch_platform_monotonic_time_us();
        bool keep_going = builder->on_progress(
            builder->progress_opaque, store, source_end, false);
        store->progress_callback_us +=
            tilefinch_platform_monotonic_time_us() - started;
        if (!keep_going) return false;
    }
    return true;
}

static void clear_provisional_section_index(CompressedSectionStore *store)
{
    budget_free(store->budget, store->selector_states);
    budget_free(store->budget, store->selector_blooms);
    budget_free(store->budget, store->anchors);
    budget_free(store->budget, store->context_tags);
    budget_free(store->budget, store->sections);
    store->selector_states = NULL;
    store->selector_blooms = NULL;
    store->anchors = NULL;
    store->context_tags = NULL;
    store->sections = NULL;
    store->anchor_count = 0;
    store->anchor_capacity = 0;
    store->context_tag_count = 0;
    store->context_tag_capacity = 0;
    store->section_count = 0;
    store->section_capacity = 0;
    store->prefix_length = 0;
    store->sealed_section_count = 0;
    store->sealed_source_bytes = 0;
}

void section_store_stream_set_progress(
    SectionStoreStreamBuilder *builder,
    SectionStoreProgressCallback callback, void *opaque)
{
    if (builder == NULL || !builder->active) return;
    builder->on_progress = callback;
    builder->progress_opaque = opaque;
}

bool section_store_stream_finish(SectionStoreStreamBuilder *builder)
{
    if (builder == NULL || !builder->active
        || !store_pending_block(builder)
        || builder->store->source_length == 0) {
        section_store_stream_abort(builder);
        return false;
    }
    /* Invalidate any provisional pager before the old index is cleared. The
       rebuild invokes progress callbacks and can transiently reproduce the
       old section count with entirely different boundaries. */
    section_store_bump_index_revision(builder->store);
    clear_provisional_section_index(builder->store);
    uint64_t callback_before = builder->store->progress_callback_us;
    uint64_t index_started = tilefinch_platform_monotonic_time_us();
    if (!index_compressed_sections(
            builder->store, NULL, 0, builder->on_progress,
            builder->progress_opaque)) {
        section_store_stream_abort(builder);
        return false;
    }
    uint64_t index_elapsed =
        tilefinch_platform_monotonic_time_us() - index_started;
    uint64_t callback_elapsed = builder->store->progress_callback_us
        >= callback_before
        ? builder->store->progress_callback_us - callback_before : 0;
    builder->store->index_us += index_elapsed >= callback_elapsed
        ? index_elapsed - callback_elapsed : 0;
    builder->store->sealed_section_count =
        builder->store->section_count;
    builder->store->sealed_source_bytes =
        builder->store->source_length;
    builder->store->stream_complete = true;
    section_store_bump_index_revision(builder->store);
    Budget *budget = builder->store->budget;
    budget_free(budget, builder->pending);
    budget_free(budget, builder->temporary);
    budget_free(budget, builder->positions);
    memset(builder, 0, sizeof(*builder));
    return true;
}

void section_store_stream_abort(SectionStoreStreamBuilder *builder)
{
    if (builder == NULL) return;
    CompressedSectionStore *store = builder->store;
    Budget *budget = store == NULL ? NULL : store->budget;
    if (budget != NULL) {
        budget_free(budget, builder->pending);
        budget_free(budget, builder->temporary);
        budget_free(budget, builder->positions);
    }
    if (store != NULL) section_store_destroy(store);
    memset(builder, 0, sizeof(*builder));
}

bool section_store_build(CompressedSectionStore *store, Budget *budget,
                         const char *html, size_t length,
                         size_t block_bytes, size_t maximum_section_bytes)
{
    if (html == NULL || length == 0) return false;
    SectionStoreStreamBuilder builder = {0};
    if (!section_store_stream_begin(&builder, store, budget, block_bytes,
                                    maximum_section_bytes)
        || !section_store_stream_append(&builder,
                                        (const unsigned char *) html, length)
        || !section_store_stream_finish(&builder)) {
        section_store_stream_abort(&builder);
        return false;
    }
    return true;
}

static bool extract_range(const CompressedSectionStore *store, size_t offset,
                          size_t length, unsigned char *output,
                          unsigned char *scratch)
{
    size_t end = offset + length;
    for (size_t i = 0; i < store->block_count; i++) {
        const SectionStoreBlock *block = &store->blocks[i];
        size_t block_start = block->source_offset;
        size_t block_end = block_start + block->source_length;
        if (block_end <= offset || block_start >= end) continue;
        const unsigned char *source = block->data;
        const unsigned char *decoded = source;
        if (block->compressed) {
            if (!decompress_block(source, block->stored_length, scratch,
                                  block->source_length)) return false;
            decoded = scratch;
        }
        size_t copy_start = block_start > offset ? block_start : offset;
        size_t copy_end = block_end < end ? block_end : end;
        memcpy(output + copy_start - offset,
               decoded + copy_start - block_start, copy_end - copy_start);
    }
    return true;
}

bool section_store_extract_source(const CompressedSectionStore *store,
                                  char **html, size_t *length)
{
    if (store == NULL || store->budget == NULL || html == NULL
        || length == NULL || store->source_length == 0) return false;
    char *source = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->source_length + 1);
    unsigned char *scratch = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes);
    if (source == NULL || scratch == NULL
        || !extract_range(store, 0, store->source_length,
                          (unsigned char *) source, scratch)) {
        budget_free(store->budget, source);
        budget_free(store->budget, scratch);
        return false;
    }
    budget_free(store->budget, scratch);
    source[store->source_length] = '\0';
    *html = source;
    *length = store->source_length;
    return true;
}

bool section_store_extract_html(const CompressedSectionStore *store,
                                size_t section_index, char **html,
                                size_t *length)
{
    static const char fallback_prefix[] = "<!doctype html><html><body>";
    static const char suffix[] = "</body></html>";
    if (store == NULL || store->budget == NULL || html == NULL
        || length == NULL || section_index >= store->section_count) return false;
    const SectionStoreEntry *entry = &store->sections[section_index];
    size_t prefix_length = store->prefix_length != 0
                           ? store->prefix_length
                           : sizeof(fallback_prefix) - 1;
    if ((size_t) entry->context_offset + entry->context_count
          > store->context_tag_count)
        return false;
    size_t context_bytes = 0;
    for (size_t i = 0; i < entry->context_count; i++) {
        size_t tag_length = store->context_tags[entry->context_offset + i]
                                .source_length;
        if (context_bytes > SIZE_MAX - tag_length) return false;
        context_bytes += tag_length;
    }
    size_t closing_capacity = (size_t) entry->context_count * 35;
    if (prefix_length > SIZE_MAX - entry->source_length
        || prefix_length + entry->source_length > SIZE_MAX - context_bytes
        || prefix_length + entry->source_length + context_bytes
             > SIZE_MAX - closing_capacity - sizeof(suffix)) return false;
    size_t capacity = prefix_length + entry->source_length + context_bytes
                      + closing_capacity + sizeof(suffix);
    char *fragment = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, capacity);
    unsigned char *scratch = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes);
    if (fragment == NULL || scratch == NULL) {
        budget_free(store->budget, fragment);
        budget_free(store->budget, scratch);
        return false;
    }
    bool ok = true;
    if (store->prefix_length != 0) {
        ok = extract_range(store, 0, store->prefix_length,
                           (unsigned char *) fragment, scratch);
    } else {
        memcpy(fragment, fallback_prefix, prefix_length);
    }
    size_t used = prefix_length;
    for (size_t i = 0; ok && i < entry->context_count; i++) {
        const SectionStoreContextTag *context =
            &store->context_tags[entry->context_offset + i];
        ok = extract_range(store, context->source_offset,
                           context->source_length,
                           (unsigned char *) fragment + used, scratch);
        used += context->source_length;
    }
    if (ok) {
        ok = extract_range(store, entry->source_offset, entry->source_length,
                           (unsigned char *) fragment + used, scratch);
        used += entry->source_length;
    }
    for (size_t i = entry->context_count; ok && i != 0; i--) {
        const SectionStoreContextTag *context =
            &store->context_tags[entry->context_offset + i - 1];
        size_t opening_offset = prefix_length;
        for (size_t j = 0; j + 1 < i; j++) {
            opening_offset += store->context_tags[entry->context_offset + j]
                                  .source_length;
        }
        bool closing = false, self_closing = false;
        char name[32];
        size_t name_length = 0;
        if (!tag_identity(fragment + opening_offset, context->source_length,
                                 &closing,
                                 &self_closing, name, &name_length)) {
            ok = false;
            break;
        }
        fragment[used++] = '<';
        fragment[used++] = '/';
        memcpy(fragment + used, name, name_length);
        used += name_length;
        fragment[used++] = '>';
    }
    if (ok) {
        memcpy(fragment + used, suffix, sizeof(suffix) - 1);
        used += sizeof(suffix) - 1;
        fragment[used] = '\0';
    }
    budget_free(store->budget, scratch);
    if (!ok) {
        budget_free(store->budget, fragment);
        return false;
    }
    *html = fragment;
    *length = used;
    return true;
}

bool section_store_extract_query_html(const CompressedSectionStore *store,
                                      size_t section_index, char **html,
                                      size_t *length)
{
    static const char start_marker[] =
        "<!--tilefinch-semantic-section-start-->";
    static const char end_marker[] =
        "<!--tilefinch-semantic-section-end-->";
    if (store == NULL || html == NULL || length == NULL
        || section_index >= store->section_count) return false;
    char *plain = NULL;
    size_t plain_length = 0;
    if (!section_store_extract_html(store, section_index, &plain,
                                    &plain_length)) return false;
    const SectionStoreEntry *entry = &store->sections[section_index];
    size_t source_start = store->prefix_length != 0
        ? store->prefix_length : sizeof("<!doctype html><html><body>") - 1;
    for (size_t i = 0; i < entry->context_count; i++) {
        source_start += store->context_tags[entry->context_offset + i]
                            .source_length;
    }
    size_t marker_bytes = sizeof(start_marker) - 1 + sizeof(end_marker) - 1;
    if (source_start > plain_length
        || entry->source_length > plain_length - source_start
        || plain_length > SIZE_MAX - marker_bytes - 1) {
        budget_free(store->budget, plain);
        return false;
    }
    char *marked = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE,
        plain_length + marker_bytes + 1);
    if (marked == NULL) {
        budget_free(store->budget, plain);
        return false;
    }
    size_t used = 0;
    memcpy(marked + used, plain, source_start);
    used += source_start;
    memcpy(marked + used, start_marker, sizeof(start_marker) - 1);
    used += sizeof(start_marker) - 1;
    memcpy(marked + used, plain + source_start, entry->source_length);
    used += entry->source_length;
    memcpy(marked + used, end_marker, sizeof(end_marker) - 1);
    used += sizeof(end_marker) - 1;
    size_t tail_start = source_start + entry->source_length;
    memcpy(marked + used, plain + tail_start, plain_length - tail_start);
    used += plain_length - tail_start;
    marked[used] = '\0';
    budget_free(store->budget, plain);
    *html = marked;
    *length = used;
    return true;
}

bool section_store_find_anchor_element(const CompressedSectionStore *store,
                                       const char *identifier, size_t length,
                                       size_t *section_index,
                                       char tag_name[32])
{
    if (store == NULL || store->budget == NULL || identifier == NULL
        || length == 0 || section_index == NULL || length > UINT16_MAX)
        return false;
    uint32_t hash = anchor_hash(identifier, length);
    size_t low = 0, high = store->anchor_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (store->anchors[middle].hash < hash) low = middle + 1;
        else high = middle;
    }
    unsigned char *value = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, length);
    unsigned char *scratch = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes);
    if (value == NULL || scratch == NULL) {
        budget_free(store->budget, value);
        budget_free(store->budget, scratch);
        return false;
    }
    bool found = false;
    for (size_t i = low; i < store->anchor_count
                         && store->anchors[i].hash == hash; i++) {
        const SectionStoreAnchor *anchor = &store->anchors[i];
        if (anchor->value_length != length
            || !extract_range(store, anchor->value_offset, length, value,
                              scratch)
            || memcmp(value, identifier, length) != 0) continue;
        size_t section = 0;
        size_t section_high = store->section_count;
        while (section < section_high) {
            size_t middle = section + (section_high - section) / 2;
            const SectionStoreEntry *entry = &store->sections[middle];
            if (anchor->value_offset
                >= (size_t) entry->source_offset + entry->source_length) {
                section = middle + 1;
            } else {
                section_high = middle;
            }
        }
        if (section >= store->section_count) continue;
        *section_index = section;
        if (tag_name != NULL) {
            if (anchor->tag_name_length == 0
                || !extract_range(store, anchor->tag_name_offset,
                                  anchor->tag_name_length,
                                  (unsigned char *) tag_name, scratch)) {
                memcpy(tag_name, "div", 4);
            } else {
                for (size_t at = 0; at < anchor->tag_name_length; at++) {
                    tag_name[at] = (char) tolower(
                        (unsigned char) tag_name[at]);
                }
                tag_name[anchor->tag_name_length] = '\0';
            }
        }
        found = true;
        break;
    }
    budget_free(store->budget, value);
    budget_free(store->budget, scratch);
    return found;
}

bool section_store_find_anchor(const CompressedSectionStore *store,
                               const char *identifier, size_t length,
                               size_t *section_index)
{
    return section_store_find_anchor_element(
        store, identifier, length, section_index, NULL);
}

static bool simple_selector_bloom_maybe(const SectionSimpleSelector *selector,
                                        const uint64_t bloom[4])
{
    if (selector->tag_length != 0
        && !selector_bloom_maybe(bloom, selector_token_hash(
               't', selector->tag, selector->tag_length, NULL, 0, true))) {
        return false;
    }
    if (selector->id_length != 0
        && !selector_bloom_maybe(bloom, selector_token_hash(
               'i', selector->id, selector->id_length, NULL, 0, false))) {
        return false;
    }
    for (size_t i = 0; i < selector->class_count; i++) {
        if (!selector_bloom_maybe(bloom, selector_token_hash(
                'c', selector->classes[i], selector->class_lengths[i],
                NULL, 0, false))) return false;
    }
    for (size_t i = 0; i < selector->attribute_count; i++) {
        const SectionSimpleAttribute *attribute = &selector->attributes[i];
        if (!selector_bloom_maybe(bloom, selector_token_hash(
                'a', attribute->name, attribute->name_length,
                NULL, 0, true))) return false;
        if (attribute->has_value
            && !selector_bloom_maybe(bloom, selector_token_hash(
                   'v', attribute->name, attribute->name_length,
                   attribute->value, attribute->value_length, true))) {
            return false;
        }
    }
    return true;
}

static bool simple_tag_attribute(const char *tag, size_t length,
                                 const char *wanted, size_t wanted_length,
                                 const char **value, size_t *value_length,
                                 bool *has_value)
{
    size_t cursor = 1;
    while (cursor < length && isspace((unsigned char) tag[cursor])) cursor++;
    while (cursor < length && !isspace((unsigned char) tag[cursor])
           && tag[cursor] != '>' && tag[cursor] != '/') cursor++;
    const char *name = NULL, *found_value = NULL;
    size_t name_length = 0, found_length = 0;
    bool found_has_value = false;
    while (tag_next_attribute(tag, length, &cursor, &name, &name_length,
                              &found_value, &found_length,
                              &found_has_value)) {
        if (name_length != wanted_length
            || !ascii_equal(name, wanted, wanted_length)) continue;
        if (value != NULL) *value = found_value;
        if (value_length != NULL) *value_length = found_length;
        if (has_value != NULL) *has_value = found_has_value;
        return true;
    }
    return false;
}

static bool simple_selector_tag_matches(const SectionSimpleSelector *selector,
                                        const char *tag, size_t length,
                                        char tag_name[32])
{
    bool closing = false, self_closing = false;
    char name[32];
    size_t name_length = 0;
    if (!tag_identity(tag, length, &closing, &self_closing,
                      name, &name_length) || closing) return false;
    (void) self_closing;
    if (selector->tag_length != 0
        && (selector->tag_length != name_length
            || !ascii_equal(selector->tag, name, name_length))) return false;
    const char *value = NULL;
    size_t value_length = 0;
    bool has_value = false;
    if (selector->id_length != 0
        && (!simple_tag_attribute(tag, length, "id", 2, &value,
                                  &value_length, &has_value)
            || !has_value || value_length != selector->id_length
            || memcmp(value, selector->id, value_length) != 0)) return false;
    for (size_t i = 0; i < selector->class_count; i++) {
        if (!simple_tag_attribute(tag, length, "class", 5, &value,
                                  &value_length, &has_value)
            || !has_value) return false;
        bool found = false;
        size_t token = 0;
        while (token < value_length) {
            while (token < value_length
                   && isspace((unsigned char) value[token])) token++;
            size_t end = token;
            while (end < value_length
                   && !isspace((unsigned char) value[end])) end++;
            if (end - token == selector->class_lengths[i]
                && memcmp(value + token, selector->classes[i],
                          end - token) == 0) {
                found = true;
                break;
            }
            token = end;
        }
        if (!found) return false;
    }
    for (size_t i = 0; i < selector->attribute_count; i++) {
        const SectionSimpleAttribute *attribute = &selector->attributes[i];
        if (!simple_tag_attribute(tag, length, attribute->name,
                                  attribute->name_length, &value,
                                  &value_length, &has_value)) return false;
        if (attribute->has_value
            && (!has_value || value_length != attribute->value_length
                || memcmp(value, attribute->value, value_length) != 0)) {
            return false;
        }
    }
    if (tag_name != NULL) memcpy(tag_name, name, name_length + 1);
    return true;
}

static bool simple_selector_scan_section(
    const CompressedSectionStore *store, size_t section,
    const SectionSimpleSelector *selector,
    TilefinchRemoteSelectorMatch *matches, size_t capacity,
    size_t *match_count, bool *overflow)
{
    const SectionStoreEntry *entry = &store->sections[section];
    char *html = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE,
        (size_t) entry->source_length + 1);
    unsigned char *scratch = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes);
    char *tag = budget_malloc_category(
        store->budget, BUDGET_CATEGORY_RESOURCE, store->block_bytes + 1);
    if (html == NULL || scratch == NULL || tag == NULL
        || !extract_range(store, entry->source_offset, entry->source_length,
                          (unsigned char *) html, scratch)) {
        budget_free(store->budget, html);
        budget_free(store->budget, scratch);
        budget_free(store->budget, tag);
        return false;
    }
    html[entry->source_length] = '\0';
    int state = store->selector_states == NULL
        ? SELECTOR_SCAN_DATA : store->selector_states[section];
    const char *raw_name = state == SELECTOR_SCAN_SCRIPT ? "script"
        : (state == SELECTOR_SCAN_STYLE ? "style" : NULL);
    size_t raw_name_length = raw_name == NULL ? 0 : strlen(raw_name);
    size_t tag_used = 0;
    bool tag_overflow = false;
    bool tag_from_raw = state == SELECTOR_SCAN_SCRIPT
                        || state == SELECTOR_SCAN_STYLE;
    char quote = '\0';
    unsigned comment_dashes = 0;
    size_t tag_start = 0;
    for (size_t cursor = 0; cursor < entry->source_length; cursor++) {
        char byte = html[cursor];
        if (state == SELECTOR_SCAN_COMMENT) {
            if (byte == '-') {
                if (comment_dashes < 2) comment_dashes++;
            } else if (byte == '>' && comment_dashes >= 2) {
                state = SELECTOR_SCAN_DATA;
                comment_dashes = 0;
            } else comment_dashes = 0;
            continue;
        }
        if (state != SELECTOR_SCAN_TAG) {
            if (state == SELECTOR_SCAN_SCRIPT
                || state == SELECTOR_SCAN_STYLE) {
                if (byte != '<') continue;
                tag_from_raw = true;
            } else {
                if (byte != '<') continue;
                tag_from_raw = false;
            }
            state = SELECTOR_SCAN_TAG;
            tag_start = cursor;
            tag_used = 0;
            tag_overflow = false;
            quote = '\0';
        }
        if (tag_used < store->block_bytes) tag[tag_used++] = byte;
        else tag_overflow = true;
        if (!tag_from_raw && tag_used == 4
            && memcmp(tag, "<!--", 4) == 0) {
            state = SELECTOR_SCAN_COMMENT;
            comment_dashes = 0;
            continue;
        }
        if (quote != '\0') {
            if (byte == quote) quote = '\0';
            continue;
        }
        if (byte == '\'' || byte == '"') { quote = byte; continue; }
        if (byte != '>') continue;
        if (!tag_overflow) tag[tag_used] = '\0';
        if (tag_from_raw) {
            bool closes = !tag_overflow && raw_name != NULL
                && tag_name_at(tag, tag_used, 0, raw_name,
                               raw_name_length, true, NULL);
            state = closes ? SELECTOR_SCAN_DATA
                : (raw_name_length == 6 ? SELECTOR_SCAN_SCRIPT
                                        : SELECTOR_SCAN_STYLE);
            if (closes) { raw_name = NULL; raw_name_length = 0; }
            continue;
        }
        if (!tag_overflow
            && simple_selector_tag_matches(selector, tag, tag_used,
                                           NULL)) {
            if (*match_count >= capacity) {
                *overflow = true;
                break;
            }
            TilefinchRemoteSelectorMatch *match = &matches[*match_count];
            memset(match, 0, sizeof(*match));
            match->section_index = section;
            match->source_offset = entry->source_offset + tag_start;
            simple_selector_tag_matches(selector, tag, tag_used,
                                        match->tag_name);
            {
                const char *id = NULL;
                size_t id_length = 0;
                bool has_id = false;
                if (simple_tag_attribute(tag, tag_used, "id", 2, &id,
                                         &id_length, &has_id)
                    && has_id && id_length != 0 && id_length <= 128) {
                    memcpy(match->identifier, id, id_length);
                    match->identifier[id_length] = '\0';
                }
            }
            (*match_count)++;
        }
        if (!tag_overflow
            && tag_name_at(tag, tag_used, 0, "script", 6, false, NULL)) {
            raw_name = "script"; raw_name_length = 6;
            state = SELECTOR_SCAN_SCRIPT;
        } else if (!tag_overflow
                   && tag_name_at(tag, tag_used, 0, "style", 5,
                                  false, NULL)) {
            raw_name = "style"; raw_name_length = 5;
            state = SELECTOR_SCAN_STYLE;
        } else state = SELECTOR_SCAN_DATA;
    }
    budget_free(store->budget, html);
    budget_free(store->budget, scratch);
    budget_free(store->budget, tag);
    return true;
}

bool section_store_find_simple_selector(const CompressedSectionStore *store,
                                        const char *selector, size_t length,
                                        size_t *section_index,
                                        char tag_name[32],
                                        char identifier[129])
{
    if (store == NULL || store->budget == NULL || selector == NULL
        || section_index == NULL)
        return false;
    if (store->selector_blooms == NULL) {
        CompressedSectionStore *mutable_store =
            (CompressedSectionStore *) store;
        uint64_t started = tilefinch_platform_monotonic_time_us();
        if (!index_section_selector_blooms(mutable_store)) return false;
        mutable_store->selector_bloom_us +=
            tilefinch_platform_monotonic_time_us() - started;
    }
    SectionSimpleSelector parsed;
    if (!parse_simple_selector(selector, length, &parsed)) return false;
    if (identifier != NULL) identifier[0] = '\0';
    for (size_t section = 0; section < store->section_count; section++) {
        if (!simple_selector_bloom_maybe(
                &parsed, store->selector_blooms + section * 4)) continue;
        TilefinchRemoteSelectorMatch match;
        size_t count = 0;
        bool overflow = false;
        if (!simple_selector_scan_section(
                store, section, &parsed, &match, 1, &count, &overflow)) {
            return false;
        }
        if (count == 0) continue;
        *section_index = match.section_index;
        if (tag_name != NULL) memcpy(tag_name, match.tag_name, 32);
        if (identifier != NULL) memcpy(identifier, match.identifier, 129);
        return true;
    }
    return false;
}

bool section_store_simple_selector_maybe_matches_section(
    const CompressedSectionStore *store, size_t section_index,
    const char *selector, size_t length)
{
    if (store == NULL || selector == NULL
        || section_index >= store->section_count) return true;
    SectionSimpleSelector parsed;
    if (!parse_simple_selector(selector, length, &parsed)) return true;
    /* A bare universal selector has no token that a Bloom index could reject.
       Avoid constructing and scanning the whole deferred index merely to
       return "maybe" for every section. */
    if (parsed.universal && parsed.id_length == 0
        && parsed.class_count == 0 && parsed.attribute_count == 0) {
        return true;
    }
    if (store->selector_blooms == NULL) {
        CompressedSectionStore *mutable_store =
            (CompressedSectionStore *) store;
        uint64_t started = tilefinch_platform_monotonic_time_us();
        if (!index_section_selector_blooms(mutable_store)) return true;
        mutable_store->selector_bloom_us +=
            tilefinch_platform_monotonic_time_us() - started;
    }
    return simple_selector_bloom_maybe(
        &parsed, store->selector_blooms + section_index * 4);
}

bool section_store_collect_simple_selector(
    const CompressedSectionStore *store, const char *selector, size_t length,
    TilefinchRemoteSelectorMatch *matches, size_t capacity, size_t *match_count)
{
    if (store == NULL || store->budget == NULL || selector == NULL
        || matches == NULL || capacity == 0 || match_count == NULL)
        return false;
    if (store->selector_blooms == NULL) {
        CompressedSectionStore *mutable_store =
            (CompressedSectionStore *) store;
        uint64_t started = tilefinch_platform_monotonic_time_us();
        if (!index_section_selector_blooms(mutable_store)) return false;
        mutable_store->selector_bloom_us +=
            tilefinch_platform_monotonic_time_us() - started;
    }
    SectionSimpleSelector parsed;
    if (!parse_simple_selector(selector, length, &parsed)) return false;
    size_t count = 0;
    bool overflow = false;
    for (size_t section = 0; section < store->section_count; section++) {
        if (!simple_selector_bloom_maybe(
                &parsed, store->selector_blooms + section * 4)) continue;
        size_t first = count;
        if (!simple_selector_scan_section(store, section, &parsed, matches,
                                          capacity, &count, &overflow)
            || overflow) return false;
        /* Anonymous deferred wrappers currently reconnect by selector.  More
           than one such match in one section would make their identities
           ambiguous, so leave that collection on the local-DOM fallback. */
        size_t anonymous = 0;
        for (size_t i = first; i < count; i++) {
            if (matches[i].identifier[0] == '\0') anonymous++;
        }
        if (anonymous > 1) return false;
    }
    *match_count = count;
    return true;
}

static void section_store_free_index(CompressedSectionStore *store)
{
    if (store == NULL || store->budget == NULL) return;
    budget_free(store->budget, store->sections);
    budget_free(store->budget, store->context_tags);
    budget_free(store->budget, store->anchors);
    budget_free(store->budget, store->selector_blooms);
    budget_free(store->budget, store->selector_states);
    store->sections = NULL;
    store->section_count = 0;
    store->section_capacity = 0;
    store->context_tags = NULL;
    store->context_tag_count = 0;
    store->context_tag_capacity = 0;
    store->anchors = NULL;
    store->anchor_count = 0;
    store->anchor_capacity = 0;
    store->selector_blooms = NULL;
    store->selector_states = NULL;
    store->prefix_length = 0;
}

bool section_store_reindex_layout_selectors(
    CompressedSectionStore *store, const char *const *selectors,
    size_t selector_count)
{
    if (store == NULL || store->budget == NULL || store->blocks == NULL
        || store->block_count == 0
        || (selectors == NULL && selector_count != 0)) return false;
    CompressedSectionStore candidate = *store;
    candidate.sections = NULL;
    candidate.section_count = 0;
    candidate.section_capacity = 0;
    candidate.context_tags = NULL;
    candidate.context_tag_count = 0;
    candidate.context_tag_capacity = 0;
    candidate.anchors = NULL;
    candidate.anchor_count = 0;
    candidate.anchor_capacity = 0;
    candidate.selector_blooms = NULL;
    candidate.selector_states = NULL;
    candidate.prefix_length = 0;
    if (!index_compressed_sections(
            &candidate, selectors, selector_count, NULL, NULL)) {
        section_store_free_index(&candidate);
        return false;
    }
    section_store_free_index(store);
    store->sections = candidate.sections;
    store->section_count = candidate.section_count;
    store->section_capacity = candidate.section_capacity;
    store->context_tags = candidate.context_tags;
    store->context_tag_count = candidate.context_tag_count;
    store->context_tag_capacity = candidate.context_tag_capacity;
    store->anchors = candidate.anchors;
    store->anchor_count = candidate.anchor_count;
    store->anchor_capacity = candidate.anchor_capacity;
    store->selector_blooms = candidate.selector_blooms;
    store->selector_states = candidate.selector_states;
    store->prefix_length = candidate.prefix_length;
    section_store_bump_index_revision(store);
    return true;
}

void section_store_destroy(CompressedSectionStore *store)
{
    if (store == NULL) return;
    Budget *budget = store->budget;
    if (budget != NULL) {
        for (size_t i = 0; i < store->block_count; i++) {
            budget_free(budget, store->blocks[i].data);
        }
        budget_free(budget, store->blocks);
        section_store_free_index(store);
    }
    memset(store, 0, sizeof(*store));
}
