/* The <noscript> image fallback's markup side: the src of the one <img> a
   placeholder's <noscript> sibling holds, read from the raw text a
   scripting parse leaves there, with its character references decoded.
   image.c decides when the fallback applies (image_noscript_source). */

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "fnv1a.h"
#include "tilefinch/resources.h"

/* Decoded sources the page's <noscript> images need. Most have no
   character reference and are answered from the text itself; the rest are
   decoded once into the arena, taken on the first such source, which never
   moves, so an answer stays valid for as long as the document. A full
   arena answers no more of them. */
#define IMAGE_NOSCRIPT_DECODED_LIMIT 32u
#define IMAGE_NOSCRIPT_ARENA_BYTES 4096u
/* A <noscript> holding more text than this is not one image tag. */
#define IMAGE_NOSCRIPT_TEXT_LIMIT 2048u

typedef struct {
    struct {
        uint64_t hash;
        uint16_t offset;
        uint16_t length;
    } decoded[IMAGE_NOSCRIPT_DECODED_LIMIT];
    char arena[IMAGE_NOSCRIPT_ARENA_BYTES];
} ImageNoscriptArena;

struct ImageNoscriptStore {
    Budget *budget;
    ImageNoscriptArena *arena;
    uint16_t used;
    uint8_t count;
};

bool image_noscript_arm(PocDocument *document, Stylesheet *sheet)
{
    if (document == NULL || document->noscript_rendered
        || !document_may_have_noscript(document)) return false;
    if (document->noscript_images == NULL) {
        document->noscript_images = budget_calloc_category(
            document->budget, BUDGET_CATEGORY_RESOURCE, 1u,
            sizeof(*document->noscript_images));
        if (document->noscript_images == NULL) return false;
        document->noscript_images->budget = document->budget;
    }
    if (sheet != NULL) sheet->noscript_images = document->noscript_images;
    return true;
}

void image_noscript_release(PocDocument *document)
{
    if (document == NULL || document->noscript_images == NULL) return;
    budget_free(document->budget, document->noscript_images->arena);
    budget_free(document->budget, document->noscript_images);
    document->noscript_images = NULL;
}

static bool image_noscript_blank(const char *text, size_t length)
{
    for (size_t at = 0; at < length; at++)
        if (!isspace((unsigned char) text[at])) return false;
    return true;
}

/* The <noscript> element right after `node`, past white space and
   comments, holding nothing but one text node. */
static lxb_dom_node_t *image_noscript_sibling(lxb_dom_node_t *node,
                                              const char **text,
                                              size_t *length)
{
    lxb_dom_node_t *next = node->next;
    for (unsigned visited = 0; next != NULL && visited < 8u;
         next = next->next, visited++) {
        if (next->type == LXB_DOM_NODE_TYPE_COMMENT) continue;
        if (next->type != LXB_DOM_NODE_TYPE_TEXT) break;
        size_t blank_length = 0;
        const char *blank = document_text_data(next, &blank_length);
        if (blank != NULL && !image_noscript_blank(blank, blank_length))
            return NULL;
    }
    if (next == NULL || next->type != LXB_DOM_NODE_TYPE_ELEMENT
        || !document_node_is_tag(next, LXB_TAG_NOSCRIPT)
        || next->first_child == NULL
        || next->first_child->type != LXB_DOM_NODE_TYPE_TEXT
        || next->first_child->next != NULL) return NULL;
    *text = document_text_data(next->first_child, length);
    return *text == NULL || *length > IMAGE_NOSCRIPT_TEXT_LIMIT ? NULL : next;
}

/* The src value of `text` when it is exactly one <img ...> tag (white space
   around it allowed), as a slice of the text. */
static const char *image_noscript_tag_source(const char *text, size_t length,
                                             size_t *source_length)
{
    size_t at = 0;
    while (at < length && isspace((unsigned char) text[at])) at++;
    if (length - at < 5u || strncasecmp(text + at, "<img", 4u) != 0
        || !(isspace((unsigned char) text[at + 4u]) || text[at + 4u] == '/'
             || text[at + 4u] == '>')) return NULL;
    at += 4u;
    const char *source = NULL;
    bool closed = false;
    while (at < length && !closed) {
        unsigned char c = (unsigned char) text[at];
        if (isspace(c) || c == '/') {
            at++;
            continue;
        }
        if (c == '>') {
            closed = true;
            at++;
            break;
        }
        if (c == '<') return NULL;
        size_t name = at;
        while (at < length && !isspace((unsigned char) text[at])
               && text[at] != '=' && text[at] != '>' && text[at] != '/'
               && text[at] != '<') at++;
        size_t name_length = at - name;
        while (at < length && isspace((unsigned char) text[at])) at++;
        const char *value = NULL;
        size_t value_length = 0;
        if (at < length && text[at] == '=') {
            at++;
            while (at < length && isspace((unsigned char) text[at])) at++;
            if (at < length && (text[at] == '"' || text[at] == '\'')) {
                char quote = text[at++];
                value = text + at;
                while (at < length && text[at] != quote) at++;
                if (at == length) return NULL;
                value_length = (size_t) (text + at - value);
                at++;
            } else {
                value = text + at;
                while (at < length && !isspace((unsigned char) text[at])
                       && text[at] != '>') at++;
                value_length = (size_t) (text + at - value);
            }
        }
        if (source == NULL && value != NULL && name_length == 3u
            && strncasecmp(text + name, "src", 3u) == 0) {
            source = value;
            *source_length = value_length;
        }
    }
    if (!closed || source == NULL
        || !image_noscript_blank(text + at, length - at)) return NULL;
    while (*source_length != 0 && isspace((unsigned char) *source)) {
        source++;
        (*source_length)--;
    }
    while (*source_length != 0
           && isspace((unsigned char) source[*source_length - 1u]))
        (*source_length)--;
    return *source_length == 0 ? NULL : source;
}

/* The character `reference` (after its '&', `length` bytes left) stands
   for, and how many bytes it spans with its ';'; 0 for any reference but
   the few an image URL carries. */
static size_t image_noscript_reference(const char *reference, size_t length,
                                       char *decoded)
{
    static const struct { const char *name; char value; } named[] = {
        {"amp;", '&'}, {"quot;", '"'}, {"apos;", '\''}, {"lt;", '<'},
        {"gt;", '>'}, {"#38;", '&'}, {"#39;", '\''}, {"#x26;", '&'},
        {"#X26;", '&'}, {"#34;", '"'}
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        size_t span = strlen(named[i].name);
        if (span <= length && memcmp(reference, named[i].name, span) == 0) {
            *decoded = named[i].value;
            return span;
        }
    }
    return 0;
}

const char *image_noscript_markup_source(ImageNoscriptStore *store,
                                         lxb_dom_node_t *node,
                                         size_t *length)
{
    *length = 0;
    const char *text = NULL;
    size_t text_length = 0;
    if (store == NULL || node == NULL
        || image_noscript_sibling(node, &text, &text_length) == NULL)
        return NULL;
    size_t raw_length = 0;
    const char *raw = image_noscript_tag_source(text, text_length,
                                                &raw_length);
    if (raw == NULL) return NULL;
    if (memchr(raw, '&', raw_length) == NULL) {
        *length = raw_length;
        return raw;
    }
    uint64_t hash = fnv1a64(FNV1A64_SEED, raw, raw_length);
    ImageNoscriptArena *arena = store->arena;
    for (size_t i = 0; arena != NULL && i < store->count; i++) {
        if (arena->decoded[i].hash != hash) continue;
        *length = arena->decoded[i].length;
        return arena->arena + arena->decoded[i].offset;
    }
    if (arena == NULL) {
        arena = store->arena = budget_calloc_category(
            store->budget, BUDGET_CATEGORY_RESOURCE, 1u, sizeof(*arena));
        if (arena == NULL) return NULL;
    }
    if (store->count == IMAGE_NOSCRIPT_DECODED_LIMIT
        || raw_length > sizeof(arena->arena) - store->used) return NULL;
    char *out = arena->arena + store->used;
    size_t written = 0;
    for (size_t at = 0; at < raw_length; at++) {
        char decoded = raw[at];
        size_t span = raw[at] == '&' ? image_noscript_reference(
            raw + at + 1u, raw_length - at - 1u, &decoded) : 0;
        out[written++] = decoded;
        at += span;
    }
    arena->decoded[store->count].hash = hash;
    arena->decoded[store->count].offset = store->used;
    arena->decoded[store->count].length = (uint16_t) written;
    store->count++;
    store->used = (uint16_t) (store->used + written);
    *length = written;
    return out;
}
