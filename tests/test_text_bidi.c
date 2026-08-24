#include "tilefinch/text_bidi.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                             \
    if (!(condition)) {                                                   \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                #condition);                                              \
        return 1;                                                         \
    }                                                                     \
} while (0)

static bool permutation_once(const uint16_t *order, size_t count)
{
    bool seen[TEXT_BIDI_CODEPOINT_LIMIT] = {false};
    if (count > TEXT_BIDI_CODEPOINT_LIMIT) return false;
    for (size_t i = 0; i < count; i++) {
        if (order[i] >= count || seen[order[i]]) return false;
        seen[order[i]] = true;
    }
    return true;
}

int main(void)
{
    Budget budget;
    budget_init(&budget, 128u * 1024u);
    TextBidiStatus status = TEXT_BIDI_MALFORMED;

    static const char latin[] = "Wikipedia (2026)";
    CHECK(!text_bidi_maybe_needed(latin, sizeof(latin) - 1u));
    CHECK(text_bidi_paragraph_create(
              &budget, latin, sizeof(latin) - 1u,
              TEXT_BIDI_BASE_AUTO_LTR, &status) == NULL);
    CHECK(status == TEXT_BIDI_NOT_NEEDED && budget.current == 0);

    /* A conformance-derived mixed LTR/RTL case: the ASCII number remains in
       reading order while the Hebrew run is visually reversed. */
    static const char mixed[] = "abc \xd7\x90\xd7\x91\xd7\x92 123";
    TextBidiParagraph *paragraph = text_bidi_paragraph_create(
        &budget, mixed, sizeof(mixed) - 1u,
        TEXT_BIDI_BASE_AUTO_LTR, &status);
    CHECK(paragraph != NULL && status == TEXT_BIDI_OK);
    CHECK(text_bidi_paragraph_level(paragraph) == 0);
    CHECK(text_bidi_paragraph_count(paragraph) == 11);
    uint16_t order[TEXT_BIDI_CODEPOINT_LIMIT] = {0};
    size_t runs = 0;
    CHECK(text_bidi_line_visual_order(
        paragraph, 0, 11, order, 11, &runs));
    static const uint16_t expected[] = {
        0, 1, 2, 3, 8, 9, 10, 7, 6, 5, 4
    };
    CHECK(memcmp(order, expected, sizeof(expected)) == 0);
    CHECK(permutation_once(order, 11) && runs >= 3);
    text_bidi_paragraph_destroy(paragraph);

    /* Excess explicit-depth controls are malformed author input, not a reason
       to escape the bounded UAX #9 implementation. The algorithm ignores
       embeddings beyond its specified depth and still emits a permutation. */
    char deep_controls[130u * 6u + 2u];
    size_t deep_length = 0;
    for (size_t at = 0; at < 130u; at++) {
        memcpy(deep_controls + deep_length, "\xe2\x80\xab", 3u); /* RLE */
        deep_length += 3u;
    }
    deep_controls[deep_length++] = 'A';
    for (size_t at = 0; at < 130u; at++) {
        memcpy(deep_controls + deep_length, "\xe2\x80\xac", 3u); /* PDF */
        deep_length += 3u;
    }
    paragraph = text_bidi_paragraph_create(
        &budget, deep_controls, deep_length, TEXT_BIDI_BASE_LTR, &status);
    CHECK(paragraph != NULL && text_bidi_line_visual_order(
        paragraph, 0, text_bidi_paragraph_count(paragraph), order,
        TEXT_BIDI_CODEPOINT_LIMIT, &runs));
    CHECK(permutation_once(order, text_bidi_paragraph_count(paragraph)));
    text_bidi_paragraph_destroy(paragraph);

    /* Isolates must not leak their direction into the surrounding suffix. */
    static const char isolate[] =
        "A \xe2\x81\xa7\xd7\x90\xd7\x91 12\xe2\x81\xa9 Z"; /* RLI/PDI */
    paragraph = text_bidi_paragraph_create(
        &budget, isolate, sizeof(isolate) - 1u,
        TEXT_BIDI_BASE_AUTO_LTR, &status);
    CHECK(paragraph != NULL && status == TEXT_BIDI_OK);
    size_t isolate_count = text_bidi_paragraph_count(paragraph);
    CHECK(text_bidi_line_visual_order(
        paragraph, 0, isolate_count, order, isolate_count, &runs));
    CHECK(permutation_once(order, isolate_count));
    const TextBidiUnit *units = text_bidi_paragraph_units(paragraph);
    CHECK(units[isolate_count - 1u].codepoint == 'Z');
    text_bidi_paragraph_destroy(paragraph);

    /* Selected UAX #9 control and paired-punctuation cases. An unmatched PDF
       is ignored safely, the RLO reverses its ASCII payload, and mirrored
       punctuation is exposed through shaped_codepoint without changing the
       logical source scalar. */
    static const char controls[] =
        "A \xe2\x80\xae" "bc" "\xe2\x80\xac Z\xe2\x80\xac";
    paragraph = text_bidi_paragraph_create(
        &budget, controls, sizeof(controls) - 1u,
        TEXT_BIDI_BASE_LTR, &status);
    CHECK(paragraph != NULL && status == TEXT_BIDI_OK);
    size_t control_count = text_bidi_paragraph_count(paragraph);
    CHECK(text_bidi_line_visual_order(
        paragraph, 0, control_count, order, control_count, &runs));
    CHECK(permutation_once(order, control_count));
    text_bidi_paragraph_destroy(paragraph);

    static const char paired[] =
        "\xd7\x90\xd7\x91 (12)";
    paragraph = text_bidi_paragraph_create(
        &budget, paired, sizeof(paired) - 1u,
        TEXT_BIDI_BASE_RTL, &status);
    CHECK(paragraph != NULL);
    units = text_bidi_paragraph_units(paragraph);
    bool mirrored_parenthesis = false;
    for (size_t at = 0; at < text_bidi_paragraph_count(paragraph); at++) {
        if ((units[at].codepoint == '(' && units[at].shaped_codepoint == ')')
            || (units[at].codepoint == ')'
                && units[at].shaped_codepoint == '(')) {
            mirrored_parenthesis = true;
        }
    }
    CHECK(mirrored_parenthesis);
    text_bidi_paragraph_destroy(paragraph);

    /* Arabic joining is contextual, but logical source order remains intact.
       Tilefinch intentionally does not synthesize the optional lam-alef
       ligature: each source codepoint retains one mapping unit. */
    static const char arabic[] =
        "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85"; /* salaam */
    paragraph = text_bidi_paragraph_create(
        &budget, arabic, sizeof(arabic) - 1u,
        TEXT_BIDI_BASE_AUTO_RTL, &status);
    CHECK(paragraph != NULL && text_bidi_paragraph_count(paragraph) == 4);
    units = text_bidi_paragraph_units(paragraph);
    CHECK(units[0].codepoint == 0x0633u
          && units[0].shaped_codepoint == 0xfeb3u);
    CHECK(units[1].codepoint == 0x0644u
          && units[1].shaped_codepoint == 0xfee0u);
    CHECK(units[2].codepoint == 0x0627u
          && units[2].shaped_codepoint == 0xfe8eu);
    CHECK(units[3].codepoint == 0x0645u
          && units[3].shaped_codepoint == 0xfee1u);
    CHECK(text_bidi_line_visual_order(
        paragraph, 0, 4, order, 4, &runs));
    CHECK(order[0] == 3 && order[1] == 2
          && order[2] == 1 && order[3] == 0);
    text_bidi_paragraph_destroy(paragraph);

    /* Rule L3: an Arabic combining mark and a ZWJ emoji sequence remain in
       logical order inside their visually reordered grapheme cluster. */
    static const char marked_arabic[] =
        "\xd8\xa8\xd9\x8e\xd8\xaa"; /* beh + fatha + teh */
    paragraph = text_bidi_paragraph_create(
        &budget, marked_arabic, sizeof(marked_arabic) - 1u,
        TEXT_BIDI_BASE_RTL, &status);
    CHECK(paragraph != NULL && text_bidi_paragraph_count(paragraph) == 3);
    CHECK(text_bidi_line_visual_order(
        paragraph, 0, 3, order, 3, &runs));
    CHECK(order[0] == 2 && order[1] == 0 && order[2] == 1);
    text_bidi_paragraph_destroy(paragraph);

    static const char rtl_emoji[] =
        "\xd7\x90 \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9";
    paragraph = text_bidi_paragraph_create(
        &budget, rtl_emoji, sizeof(rtl_emoji) - 1u,
        TEXT_BIDI_BASE_RTL, &status);
    CHECK(paragraph != NULL);
    size_t emoji_count = text_bidi_paragraph_count(paragraph);
    CHECK(emoji_count == 5 && text_bidi_line_visual_order(
        paragraph, 0, emoji_count, order, emoji_count, &runs));
    bool emoji_cluster_forward = false;
    for (size_t at = 0; at + 2u < emoji_count; at++) {
        if (order[at] == 2 && order[at + 1u] == 3
            && order[at + 2u] == 4) emoji_cluster_forward = true;
    }
    CHECK(emoji_cluster_forward);
    text_bidi_paragraph_destroy(paragraph);

    /* Persian/Urdu additions use the same joining table rather than a
       language-specific branch. */
    static const char persian_urdu[] =
        "\xd9\xbe\xdb\x8c \xda\xaf"; /* peh, Farsi yeh, gaf */
    paragraph = text_bidi_paragraph_create(
        &budget, persian_urdu, sizeof(persian_urdu) - 1u,
        TEXT_BIDI_BASE_RTL, &status);
    CHECK(paragraph != NULL);
    units = text_bidi_paragraph_units(paragraph);
    CHECK(units[0].shaped_codepoint != units[0].codepoint);
    CHECK(units[1].shaped_codepoint != units[1].codepoint);
    CHECK(units[3].shaped_codepoint != units[3].codepoint);
    text_bidi_paragraph_destroy(paragraph);

    /* Arabic join controls are shaping inputs, not paintable glyphs. ZWJ
       preserves the requested connection; ZWNJ breaks it. */
    static const char joined[] =
        "\xd8\xa8\xe2\x80\x8d\xd8\xaa"; /* beh ZWJ teh */
    paragraph = text_bidi_paragraph_create(
        &budget, joined, sizeof(joined) - 1u,
        TEXT_BIDI_BASE_RTL, &status);
    CHECK(paragraph != NULL);
    units = text_bidi_paragraph_units(paragraph);
    CHECK(units[0].shaped_codepoint == 0xfe91u
          && units[2].shaped_codepoint == 0xfe96u);
    text_bidi_paragraph_destroy(paragraph);
    static const char separated[] =
        "\xd8\xa8\xe2\x80\x8c\xd8\xaa"; /* beh ZWNJ teh */
    paragraph = text_bidi_paragraph_create(
        &budget, separated, sizeof(separated) - 1u,
        TEXT_BIDI_BASE_RTL, &status);
    CHECK(paragraph != NULL);
    units = text_bidi_paragraph_units(paragraph);
    CHECK(units[0].shaped_codepoint == 0xfe8fu
          && units[2].shaped_codepoint == 0xfe95u);
    text_bidi_paragraph_destroy(paragraph);

    char oversized[TEXT_BIDI_PARAGRAPH_BYTE_LIMIT + 2u];
    memset(oversized, 'a', sizeof(oversized));
    oversized[0] = (char) 0xd7;
    oversized[1] = (char) 0x90;
    CHECK(text_bidi_paragraph_create(
              &budget, oversized, sizeof(oversized),
              TEXT_BIDI_BASE_AUTO_LTR, &status) == NULL);
    CHECK(status == TEXT_BIDI_LIMIT_EXCEEDED);

    /* The largest admitted paragraph fits the fixed scratch arena. A hostile
       alternating-direction paragraph may exceed the retained run ceiling;
       line reordering then fails cleanly without corrupting paragraph state. */
    char maximum[TEXT_BIDI_CODEPOINT_LIMIT * 2u];
    for (size_t at = 0; at < TEXT_BIDI_CODEPOINT_LIMIT; at++) {
        maximum[at * 2u] = (char) 0xd7;
        maximum[at * 2u + 1u] = (char) 0x90;
    }
    paragraph = text_bidi_paragraph_create(
        &budget, maximum, sizeof(maximum), TEXT_BIDI_BASE_RTL, &status);
    CHECK(paragraph != NULL
          && text_bidi_paragraph_count(paragraph)
                 == TEXT_BIDI_CODEPOINT_LIMIT);
    CHECK(text_bidi_line_visual_order(
        paragraph, 0, TEXT_BIDI_CODEPOINT_LIMIT, order,
        TEXT_BIDI_CODEPOINT_LIMIT, &runs));
    text_bidi_paragraph_destroy(paragraph);

    char many_runs[TEXT_BIDI_CODEPOINT_LIMIT / 2u * 3u];
    for (size_t at = 0; at < TEXT_BIDI_CODEPOINT_LIMIT / 2u; at++) {
        many_runs[at * 3u] = (char) 0xd7;
        many_runs[at * 3u + 1u] = (char) 0x90;
        many_runs[at * 3u + 2u] = 'A';
    }
    paragraph = text_bidi_paragraph_create(
        &budget, many_runs, sizeof(many_runs),
        TEXT_BIDI_BASE_LTR, &status);
    CHECK(paragraph != NULL);
    CHECK(!text_bidi_line_visual_order(
        paragraph, 0, text_bidi_paragraph_count(paragraph), order,
        TEXT_BIDI_CODEPOINT_LIMIT, &runs));
    CHECK(runs > TEXT_BIDI_RUN_LIMIT);
    text_bidi_paragraph_destroy(paragraph);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    puts("text-bidi-tests: ok");
    return 0;
}
