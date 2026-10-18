#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <lexbor/core/sbst.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/html/html.h>
#include <lexbor/html/interface.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/interfaces/element.h>
#include <lexbor/html/tokenizer.h>
#define LXB_HTML_INTERFACE_RES_CONSTRUCTORS
#define LXB_HTML_INTERFACE_RES_DESTRUCTOR
#include <lexbor/html/interface_res.h>

#define lxb_html_tokenizer_res_entities_sbst original_entities
#include <lexbor/html/tokenizer/res.h>
#undef lxb_html_tokenizer_res_entities_sbst
#ifdef LXB_HTML_COMPACT_ENTITIES
#include "tilefinch_lexbor_entities.inc"
#else
typedef lexbor_sbst_entry_static_t lxb_html_entity_entry_t;
#define lxb_html_tokenizer_res_entities_sbst original_entities
#endif

#define CAPACITY (1024u * 1024u)
static char markup[CAPACITY], expected[CAPACITY];
static size_t markup_length, expected_length, entity_count;

static void check_interfaces(void)
{
    lxb_html_document_t *document = lxb_html_document_create();
    assert(document);
    for (lxb_tag_id_t tag = LXB_TAG_A; tag < LXB_TAG__LAST_ENTRY; tag++) {
        for (lxb_ns_id_t ns = 0; ns < LXB_NS__LAST_ENTRY; ns++) {
            lxb_dom_node_t *oracle = lxb_html_interface_res_constructors[tag][ns](document);
            lxb_dom_node_t *actual = lxb_html_interface_create(document, tag, ns);
            assert(oracle && actual && oracle->type == actual->type);
            assert(oracle->owner_document == actual->owner_document);
            assert(actual->local_name == tag && actual->ns == ns);
            assert(oracle->first_child == actual->first_child
                   && oracle->next == actual->next && oracle->parent == actual->parent);
            if (actual->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element_t *a = lxb_dom_interface_element(actual);
                lxb_dom_element_t *b = lxb_dom_interface_element(oracle);
                assert(a->first_attr == b->first_attr && a->last_attr == b->last_attr);
            }
            (void)lxb_html_interface_res_destructor[tag][ns](oracle);
            (void)lxb_html_interface_destroy(actual);
        }
    }
    lxb_html_document_destroy(document);
}

static void append(char *buffer, size_t *length, const void *text, size_t size)
{
    assert(size < CAPACITY - *length);
    memcpy(buffer + *length, text, size);
    *length += size;
    buffer[*length] = 0;
}

static void walk(size_t index, char *name, size_t length, unsigned depth)
{
    if (!index) return;
    assert(index < sizeof(original_entities) / sizeof(original_entities[0]));
    assert(depth < 128 && length < 64);
    const lexbor_sbst_entry_static_t *entry = &original_entities[index];
    walk(entry->left, name, length, depth + 1);
    walk(entry->right, name, length, depth + 1);
    name[length] = (char)entry->key;
    if (entry->value) {
        append(markup, &markup_length, "<span data-e=\"&", 15);
        append(markup, &markup_length, name, length + 1);
        append(markup, &markup_length, "!\">&", 4);
        append(markup, &markup_length, name, length + 1);
        append(markup, &markup_length, "!</span>", 8);
        append(expected, &expected_length, entry->value, entry->value_len);
        append(expected, &expected_length, "!", 1);
        entity_count++;
    }
    walk(entry->next, name, length + 1, depth + 1);
}

static unsigned long long parse(size_t chunk, const char *html, size_t length,
                                const char *text, size_t text_length)
{
    lxb_html_document_t *document = lxb_html_document_create();
    assert(document);
    if (!chunk) {
        assert(lxb_html_document_parse(document, (const lxb_char_t *)html, length) == LXB_STATUS_OK);
    } else {
        assert(lxb_html_document_parse_chunk_begin(document) == LXB_STATUS_OK);
        for (size_t offset = 0; offset < length;) {
            size_t amount = length - offset < chunk ? length - offset : chunk;
            assert(lxb_html_document_parse_chunk(document, (const lxb_char_t *)html + offset, amount) == LXB_STATUS_OK);
            offset += amount;
        }
        assert(lxb_html_document_parse_chunk_end(document) == LXB_STATUS_OK);
    }
    size_t actual_length = 0;
    lxb_dom_node_t *body = lxb_dom_interface_node(lxb_html_document_body_element(document));
    lxb_char_t *actual = lxb_dom_node_text_content(body, &actual_length);
    assert(actual && actual_length == text_length && !memcmp(actual, text, text_length));
    unsigned long long hash = 1469598103934665603ULL;
    for (size_t i = 0; i < actual_length; i++) hash = (hash ^ actual[i]) * 1099511628211ULL;
    /* Every entity's attribute has the same decoded value as its text. */
    for (lxb_dom_node_t *node = body->first_child; node; node = node->next) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        size_t attr_length = 0, node_length = 0;
        const lxb_char_t *attr = lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
            (const lxb_char_t *)"data-e", 6, &attr_length);
        if (attr) {
            lxb_char_t *value = lxb_dom_node_text_content(node, &node_length);
            assert(value && attr_length == node_length && !memcmp(attr, value, node_length));
            lxb_dom_document_destroy_text(lxb_dom_interface_document(document), value);
        }
    }
    lxb_dom_document_destroy_text(lxb_dom_interface_document(document), actual);
    lxb_html_document_destroy(document);
    return hash;
}

int main(int argc, char **argv)
{
    char name[64];
    walk(1, name, 0, 0);
    assert(entity_count == 2231);
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        for (unsigned round = 0; round < 11; round++) {
            clock_t begin = clock();
            unsigned long long hash = 0;
            for (unsigned repeat = 0; repeat < 40; repeat++)
                hash ^= parse(0, markup, markup_length, expected, expected_length) + repeat;
            double us = (double)(clock() - begin) * 1000000.0 / CLOCKS_PER_SEC;
            printf("entities round=%u cpu-us=%.0f hash=%llu bytes=%zu\n", round, us, hash, markup_length * 40);
        }
        return 0;
    }
    assert(sizeof(lxb_html_entity_entry_t) == (sizeof(void *) == 4 ? 12u : 16u));
    check_interfaces();
    const size_t count = sizeof(original_entities) / sizeof(original_entities[0]);
    assert(count == sizeof(lxb_html_tokenizer_res_entities_sbst) / sizeof(lxb_html_tokenizer_res_entities_sbst[0]));
    for (size_t i = 0; i < count; i++) {
        const lexbor_sbst_entry_static_t *a = &original_entities[i];
        const lxb_html_entity_entry_t *b = &lxb_html_tokenizer_res_entities_sbst[i];
        assert(a->key == b->key && a->left == b->left && a->right == b->right
            && a->next == b->next && a->value_len == b->value_len);
        assert((a->value == NULL) == (b->value == NULL));
        if (a->value) assert(!memcmp(a->value, b->value, a->value_len));
    }
    const size_t chunks[] = {0, 1, 2, 7, 64, 4096};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++)
        (void)parse(chunks[i], markup, markup_length, expected, expected_length);
    static const char edge[] = "<p a='&noti;' b='&amp=1'>&noti; &amp=1 &bogus; &#0; &#x1f600;</p>";
    static const char answer[] = "\xc2\xaci; &=1 &bogus; \xef\xbf\xbd \xf0\x9f\x98\x80";
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++)
        (void)parse(chunks[i], edge, sizeof(edge)-1, answer, sizeof(answer)-1);
    printf("lexbor-tables: PASS entries=%zu names=%zu record=%zu\n", count, entity_count, sizeof(lxb_html_entity_entry_t));
    return 0;
}
