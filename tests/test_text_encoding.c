/* Encoding Standard labels, index tables and decoders, and HTML encoding
   sniffing for documents (BOM, transport charset, <meta> prescan of the
   first 1024 bytes, UTF-8 default). Decoded text is read back as DOM
   textContent. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/text_encoding.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/interfaces/node.h>

#define MIB (1024u * 1024u)

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
        return 1;                                                            \
    }                                                                        \
} while (0)

/* "Привет, мир" in UTF-8. */
static const char hello_utf8[] =
    "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, "
    "\xd0\xbc\xd0\xb8\xd1\x80";
/* The same text in windows-1251 and in KOI8-R. */
static const char hello_1251[] = "\xcf\xf0\xe8\xe2\xe5\xf2, \xec\xe8\xf0";
static const char hello_koi8r[] = "\xf0\xd2\xc9\xd7\xc5\xd4, \xcd\xc9\xd2";

static TilefinchEncoding label(const char *text)
{
    return tilefinch_encoding_for_label(text, strlen(text));
}

static int test_labels(void)
{
    /* windows-1252 also answers to the Latin-1 and ASCII labels. */
    CHECK(label("windows-1252") == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(label("iso-8859-1") == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(label("ISO_8859-1:1987") == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(label("latin1") == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(label("l1") == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(label("us-ascii") == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(label("ascii") == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(label("cp1252") == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(label(" \tWindows-1251\n") == TILEFINCH_ENCODING_WINDOWS_1251);
    CHECK(label("cp1251") == TILEFINCH_ENCODING_WINDOWS_1251);
    CHECK(label("x-cp1251") == TILEFINCH_ENCODING_WINDOWS_1251);
    CHECK(label("KOI8-R") == TILEFINCH_ENCODING_KOI8_R);
    CHECK(label("koi8") == TILEFINCH_ENCODING_KOI8_R);
    CHECK(label("koi") == TILEFINCH_ENCODING_KOI8_R);
    CHECK(label("cskoi8r") == TILEFINCH_ENCODING_KOI8_R);
    CHECK(label("koi8-ru") == TILEFINCH_ENCODING_KOI8_U);
    CHECK(label("koi8-u") == TILEFINCH_ENCODING_KOI8_U);
    CHECK(label("iso-8859-2") == TILEFINCH_ENCODING_ISO_8859_2);
    CHECK(label("latin2") == TILEFINCH_ENCODING_ISO_8859_2);
    CHECK(label("cyrillic") == TILEFINCH_ENCODING_ISO_8859_5);
    CHECK(label("iso8859-5") == TILEFINCH_ENCODING_ISO_8859_5);
    CHECK(label("l9") == TILEFINCH_ENCODING_ISO_8859_15);
    CHECK(label("iso-8859-15") == TILEFINCH_ENCODING_ISO_8859_15);
    CHECK(label("cp1250") == TILEFINCH_ENCODING_WINDOWS_1250);
    CHECK(label("unicode-1-1-utf-8") == TILEFINCH_ENCODING_UTF8);
    CHECK(label("utf8") == TILEFINCH_ENCODING_UTF8);
    CHECK(label("utf-16") == TILEFINCH_ENCODING_UTF16LE);
    CHECK(label("unicodefffe") == TILEFINCH_ENCODING_UTF16BE);
    CHECK(label("iso-2022-kr") == TILEFINCH_ENCODING_REPLACEMENT);
    /* Real encodings without a decoder here, and non-labels. */
    CHECK(label("gbk") == TILEFINCH_ENCODING_UNSUPPORTED);
    CHECK(label("shift_jis") == TILEFINCH_ENCODING_UNSUPPORTED);
    CHECK(label("windows-1251x") == TILEFINCH_ENCODING_NONE);
    CHECK(label("") == TILEFINCH_ENCODING_NONE);
    CHECK(strcmp(tilefinch_encoding_name(
                     TILEFINCH_ENCODING_WINDOWS_1251), "windows-1251") == 0);
    CHECK(strcmp(tilefinch_encoding_name(TILEFINCH_ENCODING_KOI8_R),
                 "KOI8-R") == 0);
    CHECK(strcmp(tilefinch_encoding_name(
                     TILEFINCH_ENCODING_X_MAC_CYRILLIC), "x-mac-cyrillic")
          == 0);
    CHECK(tilefinch_encoding_from_content_type(
              "text/html; charset=windows-1251")
          == TILEFINCH_ENCODING_WINDOWS_1251);
    CHECK(tilefinch_encoding_from_content_type(
              "text/html;CHARSET=\"KOI8-R\"") == TILEFINCH_ENCODING_KOI8_R);
    CHECK(tilefinch_encoding_from_content_type(
              "text/html; q=1; charset=latin1; charset=koi8-r")
          == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(tilefinch_encoding_from_content_type("text/html")
          == TILEFINCH_ENCODING_NONE);
    CHECK(tilefinch_encoding_from_content_type("text/html; charset=")
          == TILEFINCH_ENCODING_NONE);
    return 0;
}

static size_t decode_all(TilefinchEncoding encoding, const char *input,
                         size_t length, char *output, size_t capacity)
{
    TilefinchDecoder decoder;
    tilefinch_decoder_init(&decoder, encoding);
    const unsigned char *in = (const unsigned char *) input;
    size_t written = 0;
    while (length != 0 || decoder.has_lead_byte
           || decoder.lead_surrogate != 0) {
        size_t step = tilefinch_decoder_decode(
            &decoder, &in, &length, (unsigned char *) output + written,
            capacity - 1u - written, true);
        written += step;
        if (step == 0) break;
    }
    output[written] = '\0';
    return written;
}

static int test_index_tables(void)
{
    char out[64];
    /* WHATWG maps 0x80 to the euro sign and 0x81 to U+0081. */
    decode_all(TILEFINCH_ENCODING_WINDOWS_1252, "\x80\x81\xe9", 3, out,
               sizeof(out));
    CHECK(strcmp(out, "\xe2\x82\xac\xc2\x81\xc3\xa9") == 0);
    decode_all(TILEFINCH_ENCODING_WINDOWS_1251, hello_1251,
               strlen(hello_1251), out, sizeof(out));
    CHECK(strcmp(out, hello_utf8) == 0);
    decode_all(TILEFINCH_ENCODING_KOI8_R, hello_koi8r, strlen(hello_koi8r),
               out, sizeof(out));
    CHECK(strcmp(out, hello_utf8) == 0);
    /* KOI8-U 0xA4 is Ukrainian ie (U+0454); KOI8-R has a box glyph. */
    decode_all(TILEFINCH_ENCODING_KOI8_U, "\xa4", 1, out, sizeof(out));
    CHECK(strcmp(out, "\xd1\x94") == 0);
    decode_all(TILEFINCH_ENCODING_KOI8_R, "\xa4", 1, out, sizeof(out));
    CHECK(strcmp(out, "\xe2\x95\x93") == 0);
    decode_all(TILEFINCH_ENCODING_ISO_8859_2, "\xa1", 1, out, sizeof(out));
    CHECK(strcmp(out, "\xc4\x84") == 0);
    decode_all(TILEFINCH_ENCODING_ISO_8859_5, "\xb0", 1, out, sizeof(out));
    CHECK(strcmp(out, "\xd0\x90") == 0);
    decode_all(TILEFINCH_ENCODING_ISO_8859_15, "\xa4", 1, out, sizeof(out));
    CHECK(strcmp(out, "\xe2\x82\xac") == 0);
    decode_all(TILEFINCH_ENCODING_WINDOWS_1250, "\x8a", 1, out, sizeof(out));
    CHECK(strcmp(out, "\xc5\xa0") == 0);
    /* An unmapped byte is an error: U+FFFD. */
    decode_all(TILEFINCH_ENCODING_WINDOWS_874, "\xdb", 1, out, sizeof(out));
    CHECK(strcmp(out, "\xef\xbf\xbd") == 0);
    decode_all(TILEFINCH_ENCODING_X_USER_DEFINED, "\x80", 1, out,
               sizeof(out));
    CHECK(strcmp(out, "\xef\x9e\x80") == 0);
    decode_all(TILEFINCH_ENCODING_REPLACEMENT, "abc", 3, out, sizeof(out));
    CHECK(strcmp(out, "\xef\xbf\xbd") == 0);
    /* UTF-16LE: a surrogate pair, a lone trail, and a dangling byte. */
    decode_all(TILEFINCH_ENCODING_UTF16LE,
               "A\x00\x3d\xd8\x00\xde\x00\xdc" "B", 9, out, sizeof(out));
    CHECK(strcmp(out, "A\xf0\x9f\x98\x80\xef\xbf\xbd\xef\xbf\xbd") == 0);
    decode_all(TILEFINCH_ENCODING_UTF16BE, "\x04\x1f\x00!", 4, out,
               sizeof(out));
    CHECK(strcmp(out, "\xd0\x9f!") == 0);
    CHECK(tilefinch_utf8_valid((const unsigned char *) hello_utf8,
                               strlen(hello_utf8)));
    CHECK(!tilefinch_utf8_valid((const unsigned char *) hello_1251,
                                strlen(hello_1251)));
    CHECK(!tilefinch_utf8_valid((const unsigned char *) "\xed\xa0\x80", 3));
    CHECK(!tilefinch_utf8_valid((const unsigned char *) "\xc0\xaf", 2));
    return 0;
}

static TilefinchEncoding prescan(const char *html)
{
    return tilefinch_encoding_prescan((const unsigned char *) html,
                                      strlen(html));
}

static int test_prescan(void)
{
    CHECK(prescan("<meta charset=windows-1251>")
          == TILEFINCH_ENCODING_WINDOWS_1251);
    CHECK(prescan("<!doctype html><META CHARSET='KOI8-R'>")
          == TILEFINCH_ENCODING_KOI8_R);
    CHECK(prescan("<meta http-equiv=Content-Type "
                  "content=\"text/html; charset=koi8-r\">")
          == TILEFINCH_ENCODING_KOI8_R);
    /* content= without the pragma does not count. */
    CHECK(prescan("<meta content=\"text/html; charset=koi8-r\">")
          == TILEFINCH_ENCODING_NONE);
    /* Comments, other tags and their attributes are skipped. */
    CHECK(prescan("<!-- <meta charset=koi8-r> --><p title='<meta "
                  "charset=latin2>'><meta charset=cp1251>")
          == TILEFINCH_ENCODING_WINDOWS_1251);
    CHECK(prescan("<!--><meta charset=koi8-r>") == TILEFINCH_ENCODING_KOI8_R);
    /* An unknown label keeps looking; UTF-16 declarations mean UTF-8. */
    CHECK(prescan("<meta charset=bogus><meta charset=latin2>")
          == TILEFINCH_ENCODING_ISO_8859_2);
    CHECK(prescan("<meta charset=utf-16le>") == TILEFINCH_ENCODING_UTF8);
    CHECK(prescan("<meta charset=x-user-defined>")
          == TILEFINCH_ENCODING_WINDOWS_1252);
    CHECK(prescan("<meta charset=gbk>") == TILEFINCH_ENCODING_UNSUPPORTED);
    /* Incomplete markup aborts. */
    CHECK(prescan("<meta charset=\"koi8-r") == TILEFINCH_ENCODING_NONE);
    return 0;
}

static int text_content_is(Budget *budget, PocDocument *document,
                           const char *id, const char *expected)
{
    (void) budget;
    lxb_dom_node_t *node = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *found = NULL;
    while (node != NULL && found == NULL) {
        size_t length = 0;
        const char *value = document_attribute(node, "id", &length);
        if (value != NULL && length == strlen(id)
            && memcmp(value, id, length) == 0) {
            found = node;
            break;
        }
        if (node->first_child != NULL) node = node->first_child;
        else {
            while (node != NULL && node->next == NULL) node = node->parent;
            if (node != NULL) node = node->next;
        }
    }
    CHECK(found != NULL);
    size_t length = 0;
    lxb_char_t *text = lxb_dom_node_text_content(found, &length);
    bool equal = text != NULL && length == strlen(expected)
        && memcmp(text, expected, length) == 0;
    if (!equal) {
        fprintf(stderr, "textContent #%s = \"%.*s\"\n", id, (int) length,
                text == NULL ? "" : (const char *) text);
    }
    if (text != NULL) lxb_dom_document_destroy_text(found->owner_document,
                                                    text);
    CHECK(equal);
    return 0;
}

typedef struct {
    const char *name;
    TilefinchEncoding transport;
    const char *html;
    size_t length;
    size_t chunk;
    TilefinchEncoding expected_encoding;
    const char *expected_text;
} SniffCase;

static int run_sniff_case(Budget *budget, const SniffCase *test)
{
    DocumentParser parser;
    PocDocument document;
    CHECK(document_parser_begin(&parser, budget));
    CHECK(document_parser_set_transport_encoding(&parser, test->transport));
    for (size_t at = 0; at < test->length;) {
        size_t step = test->length - at;
        if (test->chunk != 0 && step > test->chunk) step = test->chunk;
        CHECK(document_parser_feed(&parser, test->html + at, step));
        at += step;
    }
    CHECK(document_parser_finish(&parser, &document));
    if (document.encoding != (uint8_t) test->expected_encoding) {
        fprintf(stderr, "%s: encoding %u\n", test->name,
                (unsigned) document.encoding);
    }
    CHECK(document.encoding == (uint8_t) test->expected_encoding);
    int failed = text_content_is(budget, &document, "t",
                                 test->expected_text);
    document_destroy(&document);
    CHECK(failed == 0);
    return 0;
}

static int test_document_sniffing(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    char html[4096];
    static const char padding[] = "<!-- padding -->";

    /* 1. windows-1251 named by the HTTP charset. */
    int n = snprintf(html, sizeof(html),
                     "<!doctype html><p id=t>%s</p>", hello_1251);
    SniffCase header_1251 = { "header windows-1251",
        TILEFINCH_ENCODING_WINDOWS_1251, html, (size_t) n, 0,
        TILEFINCH_ENCODING_WINDOWS_1251, hello_utf8 };
    CHECK(run_sniff_case(&budget, &header_1251) == 0);

    /* 2. KOI8-R named by the HTTP charset. */
    n = snprintf(html, sizeof(html), "<p id=t>%s</p>", hello_koi8r);
    SniffCase header_koi8 = { "header KOI8-R", TILEFINCH_ENCODING_KOI8_R,
        html, (size_t) n, 0, TILEFINCH_ENCODING_KOI8_R, hello_utf8 };
    CHECK(run_sniff_case(&budget, &header_koi8) == 0);

    /* 3. windows-1251 named by <meta charset>; 4. KOI8-R by the pragma,
       fed in 7-byte chunks so the declaration completes late. */
    n = snprintf(html, sizeof(html),
                 "<!doctype html><meta charset=\"windows-1251\">"
                 "<p id=t>%s</p>", hello_1251);
    SniffCase meta_1251 = { "meta windows-1251", TILEFINCH_ENCODING_NONE,
        html, (size_t) n, 0, TILEFINCH_ENCODING_WINDOWS_1251, hello_utf8 };
    CHECK(run_sniff_case(&budget, &meta_1251) == 0);
    n = snprintf(html, sizeof(html),
                 "<html><head><meta http-equiv=content-type "
                 "content='text/html; charset=koi8-r'></head>"
                 "<p id=t>%s</p>", hello_koi8r);
    SniffCase meta_koi8 = { "meta KOI8-R chunked", TILEFINCH_ENCODING_NONE,
        html, (size_t) n, 7, TILEFINCH_ENCODING_KOI8_R, hello_utf8 };
    CHECK(run_sniff_case(&budget, &meta_koi8) == 0);

    /* 5. The transport charset outranks <meta>. */
    n = snprintf(html, sizeof(html),
                 "<meta charset=koi8-r><p id=t>%s</p>", hello_1251);
    SniffCase header_over_meta = { "header over meta",
        TILEFINCH_ENCODING_WINDOWS_1251, html, (size_t) n, 0,
        TILEFINCH_ENCODING_WINDOWS_1251, hello_utf8 };
    CHECK(run_sniff_case(&budget, &header_over_meta) == 0);

    /* 6. A UTF-8 BOM outranks both and is not content. */
    n = snprintf(html, sizeof(html),
                 "\xef\xbb\xbf<meta charset=windows-1251><p id=t>%s</p>",
                 hello_utf8);
    SniffCase bom = { "BOM override", TILEFINCH_ENCODING_WINDOWS_1251,
        html, (size_t) n, 0, TILEFINCH_ENCODING_UTF8, hello_utf8 };
    CHECK(run_sniff_case(&budget, &bom) == 0);

    /* 7. A UTF-16LE BOM. */
    {
        static const char utf16[] =
            "\xff\xfe<\0p\0 \0i\0d\0=\0t\0>\0\x1f\x04!\0<\0/\0p\0>\0";
        SniffCase bom16 = { "UTF-16LE BOM", TILEFINCH_ENCODING_NONE, utf16,
            sizeof(utf16) - 1u, 3, TILEFINCH_ENCODING_UTF16LE,
            "\xd0\x9f!" };
        CHECK(run_sniff_case(&budget, &bom16) == 0);
    }

    /* 8. A <meta> starting past the first 1024 bytes is ignored: the
       document stays UTF-8 and its windows-1251 bytes reach the DOM
       undecoded (the parser keeps ill-formed UTF-8 as it is). */
    size_t length = 0;
    while (length + sizeof(padding) - 1u <= 1030u) {
        memcpy(html + length, padding, sizeof(padding) - 1u);
        length += sizeof(padding) - 1u;
    }
    n = snprintf(html + length, sizeof(html) - length,
                 "<meta charset=windows-1251><p id=t>\xcf\xf0</p>");
    length += (size_t) n;
    SniffCase late_meta = { "meta after 1024 bytes",
        TILEFINCH_ENCODING_NONE, html, length, 0, TILEFINCH_ENCODING_UTF8,
        "\xcf\xf0" };
    CHECK(run_sniff_case(&budget, &late_meta) == 0);

    /* 9. No declaration at all: UTF-8, as before. */
    n = snprintf(html, sizeof(html), "<p id=t>%s</p>", hello_utf8);
    SniffCase plain = { "default UTF-8", TILEFINCH_ENCODING_NONE, html,
        (size_t) n, 5, TILEFINCH_ENCODING_UTF8, hello_utf8 };
    CHECK(run_sniff_case(&budget, &plain) == 0);

    /* 10. A late declaration after non-ASCII content cannot re-decode
       bytes already parsed; the document keeps UTF-8. */
    n = snprintf(html, sizeof(html),
                 "<p id=t>\xd0\x9f</p><meta charset=koi8-r>");
    SniffCase too_late = { "declaration after non-ASCII",
        TILEFINCH_ENCODING_NONE, html, (size_t) n, 12,
        TILEFINCH_ENCODING_UTF8, "\xd0\x9f" };
    CHECK(run_sniff_case(&budget, &too_late) == 0);

    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return 0;
}

int main(void)
{
    CHECK(test_labels() == 0);
    CHECK(test_index_tables() == 0);
    CHECK(test_prescan() == 0);
    CHECK(test_document_sniffing() == 0);
    puts("text encoding tests passed");
    return 0;
}
