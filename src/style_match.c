/* CSS selector matching: tag/class/attribute predicates, compound and
   descendant selector evaluation, and the public selector-match entry
   point.  Split out of style.c. */

#include "style_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/tag/const.h>

#ifndef TILEFINCH_NO_TRACE
#define STYLE_SELECTOR_COUNT(sheet, field, amount) do { \
    if ((sheet) != NULL) { \
        ((Stylesheet *) (sheet))->field += (amount); \
    } \
} while (0)
#else
#define STYLE_SELECTOR_COUNT(sheet, field, amount) ((void) 0)
#endif

static bool style_selector_cooperate_visit(
    const Stylesheet *sheet, lxb_dom_node_t *node)
{
    if (sheet == NULL) return true;
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    if (sheet->selector_cooperate_cancelled) return false;
    if (sheet->selector_cooperate == NULL) return true;
    mutable_sheet->selector_cooperate_visits++;
    if (sheet->selector_cooperate_visits
        < sheet->selector_cooperate_next) return true;
    mutable_sheet->selector_cooperate_next += sheet->selector_cooperate_quota;
    if (sheet->selector_cooperate(
            sheet->selector_cooperate_opaque, node,
            sheet->selector_cooperate_visits)) return true;
    mutable_sheet->selector_cooperate_cancelled = true;
    return false;
}

void style_match_subject_prepare(lxb_dom_node_t *node,
                                 StyleMatchSubject *subject)
{
    if (subject == NULL) return;
    memset(subject, 0, sizeof(*subject));
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    subject->node = node;
    subject->tag_id = lxb_dom_element_tag_id(element);
    subject->tag = (const char *) lxb_dom_element_local_name(
        element, &subject->tag_length);
    lxb_dom_attr_t *id = lxb_dom_element_id_attribute(element);
    if (id != NULL) {
        subject->id = (const char *) lxb_dom_attr_value(
            id, &subject->id_length);
    }
    lxb_dom_attr_t *classes = lxb_dom_element_class_attribute(element);
    if (classes != NULL) {
        subject->classes = (const char *) lxb_dom_attr_value(
            classes, &subject->classes_length);
    }
}

bool style_tag_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && span_equal(name, length, wanted);
}

static bool style_node_descends_from(lxb_dom_node_t *node,
                                     lxb_dom_node_t *ancestor)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at == ancestor) return true;
    }
    return false;
}

static bool style_node_effectively_disabled(lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "disabled", 8)) return true;
    if (style_tag_is(node, "option")) {
        for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
            if ((style_tag_is(at, "optgroup") || style_tag_is(at, "select"))
                && lxb_dom_element_has_attribute(
                    lxb_dom_interface_element(at),
                    (const lxb_char_t *) "disabled", 8)) return true;
            if (style_tag_is(at, "select")) break;
        }
    }
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        if (!style_tag_is(at, "fieldset")
            || !lxb_dom_element_has_attribute(
                lxb_dom_interface_element(at),
                (const lxb_char_t *) "disabled", 8)) continue;
        lxb_dom_node_t *legend = NULL;
        for (lxb_dom_node_t *child = at->first_child; child != NULL;
             child = child->next) {
            if (style_tag_is(child, "legend")) { legend = child; break; }
        }
        if (legend != NULL && style_node_descends_from(node, legend)) continue;
        return true;
    }
    return false;
}

static void scan_select_options(lxb_dom_node_t *node,
                                lxb_dom_node_t **first,
                                lxb_dom_node_t **selected)
{
    for (lxb_dom_node_t *child = node == NULL ? NULL : node->first_child;
         child != NULL; child = child->next) {
        if (style_tag_is(child, "option")) {
            if (*first == NULL) *first = child;
            size_t state_length = 0;
            const char *state = document_attribute(
                child, "data-tilefinch-option-selected", &state_length);
            bool is_selected = state != NULL
                ? state_length == 4 && memcmp(state, "true", 4) == 0
                : lxb_dom_element_has_attribute(
                      lxb_dom_interface_element(child),
                      (const lxb_char_t *) "selected", 8);
            if (*selected == NULL && is_selected) {
                *selected = child;
            }
        }
        scan_select_options(child, first, selected);
    }
}

bool option_is_displayed_by_default(lxb_dom_node_t *option)
{
    lxb_dom_node_t *select = option == NULL ? NULL : option->parent;
    while (select != NULL && !style_tag_is(select, "select")) {
        select = select->parent;
    }
    if (select == NULL) return true;
    if (lxb_dom_element_has_attribute(
            lxb_dom_interface_element(select),
            (const lxb_char_t *) "multiple", 8)) {
        return true;
    }
    lxb_dom_node_t *first = NULL;
    lxb_dom_node_t *selected = NULL;
    scan_select_options(select, &first, &selected);
    return option == (selected == NULL ? first : selected);
}


bool class_contains_length(const char *classes, size_t length,
                                  const char *wanted,
                                  size_t wanted_length)
{
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) classes[at])) at++;
        size_t end = at;
        while (end < length && !isspace((unsigned char) classes[end])) end++;
        if (end - at == wanted_length && memcmp(classes + at, wanted, wanted_length) == 0) return true;
        at = end;
    }
    return false;
}

static bool decode_css_identifier(const char *text, size_t length,
                                  char *output, size_t capacity,
                                  size_t *output_length)
{
    if (text == NULL || output == NULL || capacity == 0
        || output_length == NULL) return false;
    size_t written = 0;
    for (size_t at = 0; at < length;) {
        unsigned char character = (unsigned char) text[at++];
        if (character != '\\') {
            if (written + 1 >= capacity) return false;
            output[written++] = (char) character;
            continue;
        }
        if (at >= length || text[at] == '\n' || text[at] == '\r'
            || text[at] == '\f') return false;
        unsigned codepoint = 0;
        size_t digits = 0;
        while (at < length && digits < 6
               && isxdigit((unsigned char) text[at])) {
            unsigned char digit_character = (unsigned char) text[at++];
            unsigned digit = isdigit(digit_character)
                ? digit_character - '0'
                : (unsigned) (tolower(digit_character) - 'a' + 10);
            codepoint = (codepoint << 4) | digit;
            digits++;
        }
        if (digits != 0) {
            if (at < length && isspace((unsigned char) text[at])) {
                if (text[at] == '\r' && at + 1 < length
                    && text[at + 1] == '\n') at++;
                at++;
            }
            if (codepoint == 0 || (codepoint >= 0xd800
                                   && codepoint <= 0xdfff)
                || codepoint > 0x10ffff) codepoint = 0xfffd;
            size_t appended = style_append_css_codepoint(
                output, written, capacity, codepoint);
            if (appended == written) return false;
            written = appended;
        } else {
            if (written + 1 >= capacity) return false;
            output[written++] = text[at++];
        }
    }
    output[written] = '\0';
    *output_length = written;
    return true;
}

static bool attribute_span_equal_case(const char *actual,
                                      const char *wanted,
                                      size_t length,
                                      bool case_insensitive)
{
    if (actual == NULL || wanted == NULL) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char left = (unsigned char) actual[i];
        unsigned char right = (unsigned char) wanted[i];
        if (left != right
            && (!case_insensitive
                || tolower(left) != tolower(right))) return false;
    }
    return true;
}

static bool attribute_matches(lxb_dom_node_t *node, const char *text,
                              size_t length)
{
    enum AttributeOperator {
        ATTRIBUTE_EXACT,
        ATTRIBUTE_WORD,
        ATTRIBUTE_PREFIX,
        ATTRIBUTE_SUFFIX,
        ATTRIBUTE_SUBSTRING,
        ATTRIBUTE_DASH
    } operator = ATTRIBUTE_EXACT;
    trim(&text, &length);
    size_t name_end = skip_selector_identifier(text, length, 0);
    if (name_end == 0) return false;
    size_t name_length = 0, value_length = 0;
    char name[64];
    if (!decode_css_identifier(
            text, name_end, name, sizeof(name), &name_length)
        || name_length == 0) return false;
    for (size_t i = 0; i < name_length; i++) {
        name[i] = (char) tolower((unsigned char) name[i]);
    }
    bool exists = node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) name, name_length);
    if (!exists) return false;
    size_t at = name_end;
    while (at < length && isspace((unsigned char) text[at])) at++;
    if (at == length) return true;
    const char *actual = document_attribute(node, name, &value_length);
    if (actual == NULL) actual = "";
    if (at + 1 < length && text[at + 1] == '=') {
        switch (text[at]) {
        case '~': operator = ATTRIBUTE_WORD; break;
        case '^': operator = ATTRIBUTE_PREFIX; break;
        case '$': operator = ATTRIBUTE_SUFFIX; break;
        case '*': operator = ATTRIBUTE_SUBSTRING; break;
        case '|': operator = ATTRIBUTE_DASH; break;
        default: return false;
        }
        at += 2;
    }
    else if (text[at] == '=') at++;
    else return false;
    while (at < length && isspace((unsigned char) text[at])) at++;
    if (at == length) return false;
    char quote = text[at] == '\'' || text[at] == '"' ? text[at++] : 0;
    size_t wanted_start = at;
    if (quote != 0) {
        while (at < length) {
            if (text[at] == '\\' && at + 1 < length) {
                at += 2;
                continue;
            }
            if (text[at] == quote) break;
            at++;
        }
        if (at == length) return false;
    } else {
        while (at < length && !isspace((unsigned char) text[at])) at++;
    }
    size_t wanted_source_length = at - wanted_start;
    if (quote != 0) at++;
    while (at < length && isspace((unsigned char) text[at])) at++;
    bool case_insensitive = false;
    if (at < length
        && (text[at] == 'i' || text[at] == 'I'
            || text[at] == 's' || text[at] == 'S')) {
        case_insensitive = text[at] == 'i' || text[at] == 'I';
        at++;
        while (at < length && isspace((unsigned char) text[at])) at++;
    }
    if (at != length) return false;
    char wanted[STYLE_SELECTOR_IDENTIFIER_CAPACITY];
    size_t wanted_length = 0;
    if (!decode_css_identifier(
            text + wanted_start, wanted_source_length,
            wanted, sizeof(wanted), &wanted_length)) return false;

#define ATTRIBUTE_SPAN_EQUAL(actual_at, wanted_at, count) \
    attribute_span_equal_case((actual_at), (wanted_at), (count), \
                              case_insensitive)

    if (operator == ATTRIBUTE_WORD) {
        if (wanted_length == 0) return false;
        for (size_t word = 0; word < value_length;) {
            while (word < value_length
                   && isspace((unsigned char) actual[word])) word++;
            size_t end = word;
            while (end < value_length
                   && !isspace((unsigned char) actual[end])) end++;
            if (end - word == wanted_length
                && ATTRIBUTE_SPAN_EQUAL(
                    actual + word, wanted, wanted_length)) return true;
            word = end;
        }
        return false;
    }
    switch (operator) {
    case ATTRIBUTE_EXACT:
        return value_length == wanted_length
               && ATTRIBUTE_SPAN_EQUAL(actual, wanted, wanted_length);
    case ATTRIBUTE_PREFIX:
        return wanted_length != 0 && value_length >= wanted_length
               && ATTRIBUTE_SPAN_EQUAL(actual, wanted, wanted_length);
    case ATTRIBUTE_SUFFIX:
        return wanted_length != 0 && value_length >= wanted_length
               && ATTRIBUTE_SPAN_EQUAL(
                   actual + value_length - wanted_length,
                   wanted, wanted_length);
    case ATTRIBUTE_SUBSTRING:
        if (wanted_length == 0 || value_length < wanted_length) return false;
        for (size_t i = 0; i <= value_length - wanted_length; i++) {
            if (ATTRIBUTE_SPAN_EQUAL(
                    actual + i, wanted, wanted_length)) return true;
        }
        return false;
    case ATTRIBUTE_DASH:
        return wanted_length != 0 && value_length >= wanted_length
               && ATTRIBUTE_SPAN_EQUAL(actual, wanted, wanted_length)
               && (value_length == wanted_length
                   || actual[wanted_length] == '-');
    case ATTRIBUTE_WORD:
        return false;
    }
#undef ATTRIBUTE_SPAN_EQUAL
    return false;
}

static bool style_selector_matches_internal(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *text, size_t length, size_t prepared_offset,
    bool prepared, unsigned functional_depth,
    unsigned relationship_depth,
    const lxb_dom_node_t *scope);

static bool custom_element_navigation_fallback_is_authoritative(
    lxb_dom_node_t *node)
{
    /* Do not expand an unhydrated mega-menu merely to make its light DOM
       visible.  Navigation component fallbacks intentionally reserve a
       compact header while hiding the raw link tree.  Content components,
       on the other hand, usually contain the only readable article/card
       copy and should degrade to that light DOM when author script is
       bounded away. */
    /* A compact header's brand and primary actions are useful fallback
       content, not a mega-menu. Reveal those named custom elements even when
       an unresolved header ancestor contains them; nested contextual-nav,
       menu, and dropdown components remain authoritatively hidden below. */
    if (node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t current_name_length = 0;
        const char *current_name = document_element_name(
            node, &current_name_length);
        static const char *interactive_terms[] = {
            "menu", "nav", "drawer", "dialog", "popover", "dropdown",
            "flyout"
        };
        static const char *essential_terms[] = {
            "brand", "logo", "search", "account", "cart"
        };
        if (current_name != NULL) {
            bool interactive = false;
            for (size_t i = 0;
                 i < sizeof(interactive_terms) / sizeof(interactive_terms[0]);
                 i++) {
                size_t term_length = strlen(interactive_terms[i]);
                for (size_t at = 0;
                     at + term_length <= current_name_length; at++) {
                    bool left_boundary = at == 0
                        || current_name[at - 1] == '-';
                    bool right_boundary = at + term_length
                                              == current_name_length
                        || current_name[at + term_length] == '-';
                    if (left_boundary && right_boundary
                        && memcmp(current_name + at, interactive_terms[i],
                                  term_length) == 0) {
                        interactive = true;
                        break;
                    }
                }
                if (interactive) break;
            }
            for (size_t i = 0;
                 !interactive
                 && i < sizeof(essential_terms) / sizeof(essential_terms[0]);
                 i++) {
                size_t term_length = strlen(essential_terms[i]);
                for (size_t at = 0;
                     at + term_length <= current_name_length; at++) {
                    bool left_boundary = at == 0
                        || current_name[at - 1] == '-';
                    bool right_boundary = at + term_length
                                              == current_name_length
                        || current_name[at + term_length] == '-';
                    if (left_boundary && right_boundary
                        && memcmp(current_name + at, essential_terms[i],
                                  term_length) == 0) return false;
                }
            }
        }
    }
    for (unsigned depth = 0; node != NULL && depth < 32u;
         depth++, node = node->parent) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        static const char *navigation_terms[] = {
            "header", "nav", "menu", "dropdown"
        };
        if (name != NULL) {
            for (size_t i = 0;
                 i < sizeof(navigation_terms) / sizeof(navigation_terms[0]);
                 i++) {
                size_t term_length = strlen(navigation_terms[i]);
                for (size_t at = 0; at + term_length <= name_length; at++) {
                    if (memcmp(name + at, navigation_terms[i], term_length)
                            == 0) return true;
                }
            }
        }
        size_t role_length = 0;
        const char *role = document_attribute(node, "role", &role_length);
        if ((role_length == 10 && memcmp(role, "navigation", 10) == 0)
            || (role_length == 6 && memcmp(role, "banner", 6) == 0)
            || (role_length == 4 && memcmp(role, "menu", 4) == 0)) {
            return true;
        }
    }
    return false;
}

static bool compound_matches_depth(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *text, size_t length, unsigned functional_depth,
    const lxb_dom_node_t *scope);

static bool compound_matches(const Stylesheet *sheet,
                             lxb_dom_node_t *node, const char *text,
                             size_t length)
{
    return compound_matches_depth(sheet, node, text, length, 0, NULL);
}

static bool node_or_descendant_has_focus(lxb_dom_node_t *node)
{
    if (node == NULL) return false;
    lxb_dom_node_t *current = node;
    while (current != NULL) {
        if (current->type == LXB_DOM_NODE_TYPE_ELEMENT
            && lxb_dom_element_has_attribute(
                lxb_dom_interface_element(current),
                (const lxb_char_t *) "data-tilefinch-focus",
                sizeof("data-tilefinch-focus") - 1)) {
            return true;
        }
        if (current->first_child != NULL) {
            current = current->first_child;
            continue;
        }
        while (current != node && current->next == NULL) {
            current = current->parent;
        }
        if (current == node) break;
        current = current->next;
    }
    return false;
}

static bool nth_expression_matches(size_t index, const char *text,
                                   size_t length)
{
    trim(&text, &length);
    if (index == 0 || length == 0 || length >= 32) return false;
    char expression[32];
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        if (isspace((unsigned char) text[i])) continue;
        expression[used++] = (char) tolower((unsigned char) text[i]);
    }
    expression[used] = '\0';
    if (strcmp(expression, "odd") == 0) return (index & 1u) != 0;
    if (strcmp(expression, "even") == 0) return (index & 1u) == 0;
    char *n = strchr(expression, 'n');
    if (n == NULL) {
        char *end = NULL;
        long expected = strtol(expression, &end, 10);
        return end != expression && *end == '\0' && expected > 0
               && index == (size_t) expected;
    }
    long coefficient = 1;
    if (n != expression) {
        if (n == expression + 1 && expression[0] == '-') coefficient = -1;
        else if (!(n == expression + 1 && expression[0] == '+')) {
            char saved = *n;
            *n = '\0';
            char *end = NULL;
            coefficient = strtol(expression, &end, 10);
            bool coefficient_valid = end == n;
            *n = saved;
            if (end == expression || !coefficient_valid) return false;
        }
    }
    char *tail = n + 1;
    long offset = 0;
    if (*tail != '\0') {
        char *end = NULL;
        offset = strtol(tail, &end, 10);
        if (end == tail || *end != '\0') return false;
    }
    long difference = (long) index - offset;
    if (coefficient == 0) return difference == 0;
    return difference % coefficient == 0 && difference / coefficient >= 0;
}

static size_t element_sibling_index(lxb_dom_node_t *node, bool same_type,
                                    bool from_end)
{
    size_t index = 1;
    size_t node_name_length = 0;
    const char *node_name = same_type
        ? document_element_name(node, &node_name_length) : NULL;
    for (lxb_dom_node_t *sibling = from_end ? node->next : node->prev;
         sibling != NULL; sibling = from_end ? sibling->next : sibling->prev) {
        if (sibling->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (same_type) {
            size_t sibling_name_length = 0;
            const char *sibling_name = document_element_name(
                sibling, &sibling_name_length);
            if (sibling_name == NULL || node_name == NULL
                || sibling_name_length != node_name_length
                || memcmp(sibling_name, node_name, node_name_length) != 0) {
                continue;
            }
        }
        index++;
    }
    return index;
}

static size_t selector_list_option_end(const char *text, size_t length,
                                       size_t start)
{
    unsigned parentheses = 0, brackets = 0;
    char quote = 0;
    for (size_t at = start; at < length; at++) {
        char character = text[at];
        if (quote != 0) {
            if (character == '\\' && at + 1 < length) at++;
            else if (character == quote) quote = 0;
            continue;
        }
        if (character == '\'' || character == '"') quote = character;
        else if (character == '(') parentheses++;
        else if (character == ')' && parentheses != 0) parentheses--;
        else if (character == '[') brackets++;
        else if (character == ']' && brackets != 0) brackets--;
        else if (character == ',' && parentheses == 0 && brackets == 0) {
            return at;
        }
    }
    return length;
}

static bool selector_list_matches_node(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *text, size_t length, unsigned functional_depth,
    const lxb_dom_node_t *scope)
{
    if (functional_depth >= 8) return false;
    for (size_t at = 0; at < length;) {
        size_t end = selector_list_option_end(text, length, at);
        const char *option = text + at;
        size_t option_length = end - at;
        trim(&option, &option_length);
        if (option_length != 0
            && style_selector_matches_internal(
                sheet, node, option, option_length, 0, false,
                functional_depth, 0, scope)) return true;
        at = end + (end < length);
    }
    return false;
}

typedef struct {
    size_t visits;
    bool exhausted;
} RelativeSelectorWalk;

#define STYLE_RELATIVE_SELECTOR_VISIT_LIMIT 4096u
#define STYLE_RELATIVE_SELECTOR_DEPTH_LIMIT 64u

void style_relative_selector_cache_begin(Stylesheet *sheet)
{
    if (sheet == NULL) return;
    if (sheet->relative_selector_cache_depth != 0) {
        if (sheet->relative_selector_cache_depth != UINT8_MAX) {
            sheet->relative_selector_cache_depth++;
        }
        return;
    }
    sheet->relative_selector_cache_depth = 1;
    sheet->relative_selector_cache_epoch++;
    if (sheet->relative_selector_cache_epoch == 0) {
        memset(sheet->relative_selector_cache, 0,
               sizeof(sheet->relative_selector_cache));
        sheet->relative_selector_cache_epoch = 1;
    }
}

void style_relative_selector_cache_end(Stylesheet *sheet)
{
    if (sheet != NULL && sheet->relative_selector_cache_depth != 0) {
        sheet->relative_selector_cache_depth--;
    }
}

static uint32_t relative_selector_hash(const char *text, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) text[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static StyleRelativeSelectorCacheEntry *relative_selector_cache_entry(
    const Stylesheet *sheet, lxb_dom_node_t *anchor,
    size_t length, unsigned functional_depth,
    const lxb_dom_node_t *scope, uint32_t hash)
{
    if (sheet == NULL || sheet->relative_selector_cache_depth == 0
        || sheet->relative_selector_cache_epoch == 0
        || length > UINT16_MAX || functional_depth > UINT8_MAX) return NULL;
    uintptr_t identity = (uintptr_t) anchor ^ ((uintptr_t) scope >> 4);
    size_t slot = (hash ^ (uint32_t) identity
                   ^ (uint32_t) (identity >> 16))
                  % STYLE_RELATIVE_SELECTOR_CACHE_CAPACITY;
    return &((Stylesheet *) sheet)->relative_selector_cache[slot];
}

static bool relative_selector_matches_from(
    const Stylesheet *sheet, lxb_dom_node_t *anchor,
    const char *text, size_t length, unsigned functional_depth,
    unsigned tree_depth, RelativeSelectorWalk *walk,
    const lxb_dom_node_t *scope);

static bool relative_candidate_matches(
    const Stylesheet *sheet, lxb_dom_node_t *candidate,
    const char *compound, size_t compound_length,
    const char *remainder, size_t remainder_length,
    unsigned functional_depth, unsigned tree_depth,
    RelativeSelectorWalk *walk, const lxb_dom_node_t *scope)
{
    if (candidate == NULL || walk == NULL || walk->exhausted) return false;
    if (walk->visits >= STYLE_RELATIVE_SELECTOR_VISIT_LIMIT) {
        walk->exhausted = true;
        return false;
    }
    walk->visits++;
    STYLE_SELECTOR_COUNT(sheet, selector_descendant_visits, 1);
    if (candidate->type != LXB_DOM_NODE_TYPE_ELEMENT
        || !compound_matches_depth(
            sheet, candidate, compound, compound_length,
            functional_depth, scope)) return false;
    if (remainder_length == 0) return true;
    return relative_selector_matches_from(
        sheet, candidate, remainder, remainder_length,
        functional_depth, tree_depth, walk, scope);
}

static bool relative_descendant_matches(
    const Stylesheet *sheet, lxb_dom_node_t *anchor,
    const char *compound, size_t compound_length,
    const char *remainder, size_t remainder_length,
    unsigned functional_depth, unsigned tree_depth,
    RelativeSelectorWalk *walk, const lxb_dom_node_t *scope)
{
    if (anchor == NULL || walk == NULL || walk->exhausted
        || tree_depth >= STYLE_RELATIVE_SELECTOR_DEPTH_LIMIT) return false;
    for (lxb_dom_node_t *child = anchor->first_child;
         child != NULL; child = child->next) {
        if (relative_candidate_matches(
                sheet, child, compound, compound_length,
                remainder, remainder_length, functional_depth,
                tree_depth + 1u, walk, scope)) return true;
        if (relative_descendant_matches(
                sheet, child, compound, compound_length,
                remainder, remainder_length, functional_depth,
                tree_depth + 1u, walk, scope)) return true;
        if (walk->exhausted) return false;
    }
    return false;
}

static bool relative_selector_matches_from(
    const Stylesheet *sheet, lxb_dom_node_t *anchor,
    const char *text, size_t length, unsigned functional_depth,
    unsigned tree_depth, RelativeSelectorWalk *walk,
    const lxb_dom_node_t *scope)
{
    trim(&text, &length);
    if (anchor == NULL || length == 0 || functional_depth >= 8
        || tree_depth >= STYLE_RELATIVE_SELECTOR_DEPTH_LIMIT
        || walk == NULL || walk->exhausted) return false;
    char relation = ' ';
    if (text[0] == '>' || text[0] == '+' || text[0] == '~') {
        relation = text[0];
        text++;
        length--;
        trim(&text, &length);
        if (length == 0) return false;
    }
    unsigned parentheses = 0, brackets = 0;
    char quote = 0;
    size_t split = length;
    for (size_t at = 0; at < length; at++) {
        char character = text[at];
        if (quote != 0) {
            if (character == '\\' && at + 1 < length) at++;
            else if (character == quote) quote = 0;
            continue;
        }
        if (character == '\'' || character == '"') quote = character;
        else if (character == '(') parentheses++;
        else if (character == ')' && parentheses != 0) parentheses--;
        else if (character == '[') brackets++;
        else if (character == ']' && brackets != 0) brackets--;
        else if (parentheses == 0 && brackets == 0
                 && (character == '>' || character == '+'
                     || character == '~'
                     || isspace((unsigned char) character))) {
            split = at;
            break;
        }
    }
    const char *compound = text;
    size_t compound_length = split;
    trim(&compound, &compound_length);
    if (compound_length == 0) return false;
    const char *remainder = text + split;
    size_t remainder_length = length - split;
    trim(&remainder, &remainder_length);

    if (relation == '>') {
        for (lxb_dom_node_t *child = anchor->first_child;
             child != NULL; child = child->next) {
            if (relative_candidate_matches(
                    sheet, child, compound, compound_length,
                    remainder, remainder_length, functional_depth,
                    tree_depth + 1u, walk, scope)) return true;
        }
        return false;
    }
    if (relation == '+') {
        lxb_dom_node_t *sibling = anchor->next;
        while (sibling != NULL
               && sibling->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            sibling = sibling->next;
        }
        return relative_candidate_matches(
            sheet, sibling, compound, compound_length,
            remainder, remainder_length, functional_depth,
            tree_depth, walk, scope);
    }
    if (relation == '~') {
        for (lxb_dom_node_t *sibling = anchor->next;
             sibling != NULL; sibling = sibling->next) {
            if (sibling->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            STYLE_SELECTOR_COUNT(sheet, selector_sibling_visits, 1);
            if (!style_selector_cooperate_visit(sheet, sibling)) return false;
            if (relative_candidate_matches(
                    sheet, sibling, compound, compound_length,
                    remainder, remainder_length, functional_depth,
                    tree_depth, walk, scope)) return true;
            if (walk->exhausted) return false;
        }
        return false;
    }
    return relative_descendant_matches(
        sheet, anchor, compound, compound_length,
        remainder, remainder_length, functional_depth,
        tree_depth, walk, scope);
}

static bool relative_selector_list_matches(
    const Stylesheet *sheet, lxb_dom_node_t *anchor,
    const char *text, size_t length, unsigned functional_depth,
    const lxb_dom_node_t *scope)
{
    if (functional_depth >= 8) return false;
    STYLE_SELECTOR_COUNT(sheet, selector_relative_queries, 1);
    uint32_t hash = relative_selector_hash(text, length);
    StyleRelativeSelectorCacheEntry *cached =
        relative_selector_cache_entry(
            sheet, anchor, length, functional_depth, scope, hash);
    if (cached != NULL
        && cached->epoch == sheet->relative_selector_cache_epoch
        && cached->hash == hash && cached->length == length
        && cached->functional_depth == functional_depth
        && cached->anchor == anchor && cached->scope == scope
        && cached->text != NULL
        && memcmp(cached->text, text, length) == 0) {
        STYLE_SELECTOR_COUNT(sheet, selector_relative_cache_hits, 1);
        return cached->matched;
    }
    STYLE_SELECTOR_COUNT(sheet, selector_relative_walks, 1);
    RelativeSelectorWalk walk = {0};
    bool matched = false;
    for (size_t at = 0; at < length;) {
        size_t end = selector_list_option_end(text, length, at);
        const char *option = text + at;
        size_t option_length = end - at;
        trim(&option, &option_length);
        if (option_length != 0
            && relative_selector_matches_from(
                sheet, anchor, option, option_length,
                functional_depth, 0, &walk, scope)) {
            matched = true;
            break;
        }
        if (walk.exhausted) break;
        at = end + (end < length);
    }
    STYLE_SELECTOR_COUNT(sheet, selector_relative_visits, walk.visits);
    if (sheet != NULL
        && walk.visits > sheet->selector_relative_max_visits) {
        ((Stylesheet *) sheet)->selector_relative_max_visits = walk.visits;
    }
    if (walk.exhausted) {
        STYLE_SELECTOR_COUNT(sheet, selector_relative_exhaustions, 1);
    }
    if (cached != NULL) {
        *cached = (StyleRelativeSelectorCacheEntry) {
            .text = text,
            .anchor = anchor,
            .scope = scope,
            .epoch = sheet->relative_selector_cache_epoch,
            .hash = hash,
            .length = (uint16_t) length,
            .functional_depth = (uint8_t) functional_depth,
            .matched = matched
        };
    }
    return matched;
}

static size_t nth_of_separator(const char *text, size_t length)
{
    unsigned parentheses = 0, brackets = 0;
    char quote = 0;
    for (size_t at = 0; at + 1 < length; at++) {
        char character = text[at];
        if (quote != 0) {
            if (character == '\\' && at + 1 < length) at++;
            else if (character == quote) quote = 0;
            continue;
        }
        if (character == '\'' || character == '"') quote = character;
        else if (character == '(') parentheses++;
        else if (character == ')' && parentheses != 0) parentheses--;
        else if (character == '[') brackets++;
        else if (character == ']' && brackets != 0) brackets--;
        else if (parentheses == 0 && brackets == 0
                 && isspace((unsigned char) character)) {
            size_t word = at;
            while (word < length
                   && isspace((unsigned char) text[word])) word++;
            if (word + 2 <= length
                && (text[word] == 'o' || text[word] == 'O')
                && (text[word + 1] == 'f' || text[word + 1] == 'F')
                && word + 2 < length
                && isspace((unsigned char) text[word + 2])) {
                return at;
            }
        }
    }
    return length;
}

static size_t filtered_sibling_index(
    const Stylesheet *sheet, lxb_dom_node_t *node, bool from_end,
    const char *filter, size_t filter_length, unsigned functional_depth,
    const lxb_dom_node_t *scope)
{
    if (!selector_list_matches_node(
            sheet, node, filter, filter_length, functional_depth,
            scope)) return 0;
    size_t index = 1;
    for (lxb_dom_node_t *sibling = from_end ? node->next : node->prev;
         sibling != NULL; sibling = from_end ? sibling->next : sibling->prev) {
        if (sibling->type == LXB_DOM_NODE_TYPE_ELEMENT
            && selector_list_matches_node(
                sheet, sibling, filter, filter_length,
                functional_depth, scope)) index++;
    }
    return index;
}

static bool compound_matches_depth(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *text, size_t length, unsigned functional_depth,
    const lxb_dom_node_t *scope)
{
    STYLE_SELECTOR_COUNT(sheet, selector_compound_calls, 1);
    trim(&text, &length);
    if (functional_depth >= 8 || length == 0 || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    StyleMatchSubject local_subject = {0};
    style_match_subject_prepare(node, &local_subject);
    const StyleMatchSubject *subject = &local_subject;
    size_t at = 0;
    if (text[at] == '*') at++;
    else if (name_character(text[at]) || text[at] == '\\') {
        size_t end = skip_selector_identifier(text, length, at);
        char wanted[STYLE_SELECTOR_IDENTIFIER_CAPACITY];
        size_t wanted_length = 0;
        if (!decode_css_identifier(text + at, end - at, wanted,
                                   sizeof(wanted), &wanted_length)
            || subject->tag == NULL
            || subject->tag_length != wanted_length
            || memcmp(subject->tag, wanted, wanted_length) != 0) {
            return false;
        }
        at = end;
    }
    while (at < length) {
        if (text[at] == '.') {
            at++;
            size_t end = skip_selector_identifier(text, length, at);
            char wanted[STYLE_SELECTOR_IDENTIFIER_CAPACITY];
            size_t wanted_length = 0;
            if (end == at
                || !decode_css_identifier(text + at, end - at, wanted,
                                          sizeof(wanted), &wanted_length)
                || wanted_length == 0 || subject->classes == NULL
                || !class_contains_length(
                    subject->classes, subject->classes_length,
                    wanted, wanted_length)) return false;
            at = end;
        } else if (text[at] == '#') {
            at++;
            size_t end = skip_selector_identifier(text, length, at);
            char wanted[STYLE_SELECTOR_IDENTIFIER_CAPACITY];
            size_t wanted_length = 0;
            if (end == at
                || !decode_css_identifier(text + at, end - at, wanted,
                                          sizeof(wanted), &wanted_length)
                || subject->id == NULL
                || subject->id_length != wanted_length
                || memcmp(subject->id, wanted, wanted_length) != 0) {
                return false;
            }
            at = end;
        } else if (text[at] == '[') {
            STYLE_SELECTOR_COUNT(sheet, selector_attribute_checks, 1);
            size_t end = at + 1;
            char quote = 0;
            while (end < length) {
                if (quote != 0) { if (text[end] == quote) quote = 0; }
                else if (text[end] == '\'' || text[end] == '"') quote = text[end];
                else if (text[end] == ']') break;
                end++;
            }
            if (end == length || !attribute_matches(node, text + at + 1,
                                                     end - at - 1)) return false;
            at = end + 1;
        } else if (text[at] == ':') {
            STYLE_SELECTOR_COUNT(sheet, selector_pseudo_checks, 1);
            if (at + 1 < length && text[at + 1] == ':') return false;
            size_t end = ++at;
            while (end < length && name_character(text[end])) end++;
            size_t pseudo_length = end - at;
            bool interactive = span_equal(text + at, pseudo_length, "hover")
                               || span_equal(text + at, pseudo_length, "active")
                               || span_equal(text + at, pseudo_length, "visited");
            if (interactive) return false;
            bool focus_pseudo = span_equal(text + at, pseudo_length, "focus")
                                || span_equal(text + at, pseudo_length,
                                              "focus-visible");
            bool focus_within_pseudo = span_equal(
                text + at, pseudo_length, "focus-within");
            bool root_pseudo = span_equal(text + at, pseudo_length, "root");
            bool scope_pseudo = span_equal(text + at, pseudo_length, "scope");
            bool defined_pseudo = span_equal(text + at, pseudo_length,
                                              "defined");
            bool first_pseudo = span_equal(text + at, pseudo_length,
                                           "first-child");
            bool last_pseudo = span_equal(text + at, pseudo_length,
                                          "last-child");
            bool first_type_pseudo = span_equal(text + at, pseudo_length,
                                                "first-of-type");
            bool last_type_pseudo = span_equal(text + at, pseudo_length,
                                               "last-of-type");
            bool only_child_pseudo = span_equal(text + at, pseudo_length,
                                                "only-child");
            bool only_type_pseudo = span_equal(text + at, pseudo_length,
                                               "only-of-type");
            bool empty_pseudo = span_equal(text + at, pseudo_length, "empty");
            bool not_pseudo = span_equal(text + at, pseudo_length, "not");
            bool is_pseudo = span_equal(text + at, pseudo_length, "is")
                             || span_equal(text + at, pseudo_length, "where")
                             || span_equal(text + at, pseudo_length,
                                           "-webkit-any")
                             || span_equal(text + at, pseudo_length,
                                           "-moz-any");
            bool has_pseudo = span_equal(text + at, pseudo_length, "has");
            bool disabled_pseudo = span_equal(text + at, pseudo_length,
                                              "disabled");
            bool enabled_pseudo = span_equal(text + at, pseudo_length,
                                             "enabled");
            bool checked_pseudo = span_equal(text + at, pseudo_length,
                                             "checked");
            bool required_pseudo = span_equal(text + at, pseudo_length,
                                              "required");
            bool optional_pseudo = span_equal(text + at, pseudo_length,
                                              "optional");
            bool link_pseudo = span_equal(text + at, pseudo_length, "link")
                               || span_equal(text + at, pseudo_length,
                                             "any-link");
            bool open_pseudo = span_equal(text + at, pseudo_length, "open");
            bool modal_pseudo = span_equal(text + at, pseudo_length,
                                            "modal");
            bool popover_open_pseudo = span_equal(
                text + at, pseudo_length, "popover-open");
            bool nth_child_pseudo = span_equal(text + at, pseudo_length,
                                                "nth-child");
            bool nth_type_pseudo = span_equal(text + at, pseudo_length,
                                               "nth-of-type");
            bool nth_last_child_pseudo = span_equal(text + at, pseudo_length,
                                                    "nth-last-child");
            bool nth_last_type_pseudo = span_equal(text + at, pseudo_length,
                                                   "nth-last-of-type");
            if (!root_pseudo && !scope_pseudo && !defined_pseudo
                && !focus_pseudo && !focus_within_pseudo
                && !first_pseudo && !last_pseudo && !empty_pseudo
                && !not_pseudo && !is_pseudo && !has_pseudo
                && !first_type_pseudo && !last_type_pseudo
                && !only_child_pseudo && !only_type_pseudo
                && !disabled_pseudo && !enabled_pseudo && !checked_pseudo
                && !required_pseudo && !optional_pseudo && !link_pseudo
                && !open_pseudo && !modal_pseudo && !popover_open_pseudo
                && !nth_child_pseudo && !nth_type_pseudo
                && !nth_last_child_pseudo
                && !nth_last_type_pseudo) return false;
            if (focus_pseudo
                && !lxb_dom_element_has_attribute(
                    lxb_dom_interface_element(node),
                    (const lxb_char_t *) "data-tilefinch-focus",
                    sizeof("data-tilefinch-focus") - 1)) {
                return false;
            }
            if (focus_within_pseudo
                && !node_or_descendant_has_focus(node)) return false;
            if (root_pseudo) {
                size_t node_length = 0;
                const char *node_name = document_element_name(node, &node_length);
                if (node_name == NULL || !span_equal(node_name, node_length, "html")) return false;
            }
            if (scope_pseudo) {
                if (scope != NULL) {
                    if (node != scope) return false;
                } else {
                    size_t node_length = 0;
                    const char *node_name =
                        document_element_name(node, &node_length);
                    if (node_name == NULL
                        || !span_equal(node_name, node_length, "html")) {
                        return false;
                    }
                }
            }
            if (defined_pseudo) {
                lxb_dom_element_t *element = lxb_dom_interface_element(node);
                bool built_in = node->local_name >= LXB_TAG__BEGIN
                                && node->local_name < LXB_TAG__LAST_ENTRY;
                if (!built_in && element->custom_state
                           != LXB_DOM_ELEMENT_CUSTOM_STATE_CUSTOM) {
                    return false;
                }
            }
            if (first_pseudo) {
                for (lxb_dom_node_t *sibling = node->prev; sibling != NULL;
                     sibling = sibling->prev) {
                    if (sibling->type == LXB_DOM_NODE_TYPE_ELEMENT) return false;
                }
            }
            if (last_pseudo) {
                for (lxb_dom_node_t *sibling = node->next; sibling != NULL;
                     sibling = sibling->next) {
                    if (sibling->type == LXB_DOM_NODE_TYPE_ELEMENT) return false;
                }
            }
            if ((first_type_pseudo || only_type_pseudo)
                && element_sibling_index(node, true, false) != 1) return false;
            if ((last_type_pseudo || only_type_pseudo)
                && element_sibling_index(node, true, true) != 1) return false;
            if (only_child_pseudo
                && (element_sibling_index(node, false, false) != 1
                    || element_sibling_index(node, false, true) != 1)) {
                return false;
            }
            bool form_control = style_tag_is(node, "button") || style_tag_is(node, "input")
                                || style_tag_is(node, "select")
                                || style_tag_is(node, "textarea")
                                || style_tag_is(node, "option")
                                || style_tag_is(node, "optgroup")
                                || style_tag_is(node, "fieldset");
            bool disableable = form_control
                               && style_node_effectively_disabled(node);
            bool requireable = style_tag_is(node, "input") || style_tag_is(node, "select")
                               || style_tag_is(node, "textarea");
            bool required = requireable
                && lxb_dom_element_has_attribute(
                       lxb_dom_interface_element(node),
                       (const lxb_char_t *) "required", 8);
            if (disabled_pseudo && !disableable) return false;
            if (enabled_pseudo && (!form_control || disableable)) return false;
            if (checked_pseudo
                && !((style_tag_is(node, "input")
                      && lxb_dom_element_has_attribute(
                             lxb_dom_interface_element(node),
                             (const lxb_char_t *) "checked", 7))
                     || (style_tag_is(node, "option")
                         && lxb_dom_element_has_attribute(
                                lxb_dom_interface_element(node),
                                (const lxb_char_t *) "selected", 8)))) {
                return false;
            }
            if (required_pseudo && !required) return false;
            if (optional_pseudo && (!requireable || required)) return false;
            if (link_pseudo
                && (!(style_tag_is(node, "a") || style_tag_is(node, "area"))
                    || !lxb_dom_element_has_attribute(
                           lxb_dom_interface_element(node),
                           (const lxb_char_t *) "href", 4))) return false;
            bool has_open = lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "open", 4);
            bool has_modal = lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "data-tilefinch-modal",
                sizeof("data-tilefinch-modal") - 1);
            bool has_popover_open = lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "data-tilefinch-popover-open",
                sizeof("data-tilefinch-popover-open") - 1);
            if (open_pseudo && !(has_open || has_popover_open)) return false;
            if (modal_pseudo && !(style_tag_is(node, "dialog") && has_open
                                  && has_modal)) return false;
            if (popover_open_pseudo && !has_popover_open) return false;
            if (empty_pseudo) {
                for (lxb_dom_node_t *child = node->first_child;
                     child != NULL; child = child->next) {
                    if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) return false;
                    if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
                        size_t child_length = 0;
                        const char *child_text = document_text_data(
                            child, &child_length);
                        if (child_text != NULL && child_length != 0) return false;
                    }
                }
            }
            at = end;
            bool nth_pseudo = nth_child_pseudo || nth_type_pseudo
                              || nth_last_child_pseudo
                              || nth_last_type_pseudo;
            if (nth_pseudo
                && (at >= length || text[at] != '(')) return false;
            if (at < length && text[at] == '(') {
                int depth = 1;
                size_t close = ++at;
                while (close < length && depth != 0) {
                    if (text[close] == '(') depth++;
                    else if (text[close] == ')') depth--;
                    if (depth != 0) close++;
                }
                if (depth != 0) return false;
                if (nth_pseudo) {
                    bool same_type = nth_type_pseudo || nth_last_type_pseudo;
                    bool from_end = nth_last_child_pseudo
                                    || nth_last_type_pseudo;
                    const char *expression = text + at;
                    size_t expression_length = close - at;
                    size_t of = same_type ? expression_length
                        : nth_of_separator(expression, expression_length);
                    size_t index = 0;
                    if (of < expression_length) {
                        const char *filter = expression + of;
                        size_t filter_length = expression_length - of;
                        while (filter_length != 0
                               && isspace((unsigned char) *filter)) {
                            filter++;
                            filter_length--;
                        }
                        if (filter_length < 2
                            || tolower((unsigned char) filter[0]) != 'o'
                            || tolower((unsigned char) filter[1]) != 'f') {
                            return false;
                        }
                        filter += 2;
                        filter_length -= 2;
                        trim(&filter, &filter_length);
                        if (filter_length == 0) return false;
                        index = filtered_sibling_index(
                            sheet, node, from_end, filter, filter_length,
                            functional_depth + 1u, scope);
                        expression_length = of;
                    } else {
                        index = element_sibling_index(
                            node, same_type, from_end);
                    }
                    if (!nth_expression_matches(
                            index, expression, expression_length)) {
                        return false;
                    }
                    at = close + 1;
                    continue;
                }
                const char *functional_text = text + at;
                size_t functional_length = close - at;
                trim(&functional_text, &functional_length);
                bool suppress_unresolved_hiding = false;
                if (not_pseudo && sheet != NULL
                    && sheet->static_custom_element_fallback
                    && span_equal(functional_text, functional_length,
                                  ":defined")) {
                    lxb_dom_element_t *element =
                        lxb_dom_interface_element(node);
                    bool built_in = node->local_name >= LXB_TAG__BEGIN
                        && node->local_name < LXB_TAG__LAST_ENTRY;
                    suppress_unresolved_hiding = !built_in
                        && element->custom_state
                            != LXB_DOM_ELEMENT_CUSTOM_STATE_CUSTOM
                        && !custom_element_navigation_fallback_is_authoritative(
                               node);
                }
                bool any = suppress_unresolved_hiding || (has_pseudo
                    ? relative_selector_list_matches(
                        sheet, node, functional_text, functional_length,
                        functional_depth + 1u, scope)
                    : selector_list_matches_node(
                        sheet, node, functional_text, functional_length,
                        functional_depth + 1u, scope));
                if (not_pseudo && any) return false;
                if ((is_pseudo || has_pseudo) && !any) return false;
                at = close + 1;
            }
        } else if (isspace((unsigned char) text[at])) at++;
        else return false;
    }
    return true;
}

static bool style_selector_matches_internal(const Stylesheet *sheet,
                                            lxb_dom_node_t *node,
                                            const char *text, size_t length,
                                            size_t prepared_offset,
                                            bool prepared,
                                            unsigned functional_depth,
                                            unsigned relationship_depth,
                                            const lxb_dom_node_t *scope)
{
    if (functional_depth >= 8 || relationship_depth >= 64) return false;
    trim(&text, &length);
    if (length == 0) return false;
    int square = 0;
    int round = 0;
    size_t split = length;
    char combinator = 0;
    if (prepared && prepared_offset != 0 && prepared_offset < length) {
        size_t left = prepared_offset;
        while (left != 0
               && isspace((unsigned char) text[left - 1])) left--;
        if (left != 0 && (text[left - 1] == '>'
                          || text[left - 1] == '+'
                          || text[left - 1] == '~')) {
            split = left - 1;
            combinator = text[left - 1];
        } else {
            split = left;
            combinator = ' ';
        }
    } else if (!prepared || prepared_offset >= length) {
        for (size_t i = length; i != 0; i--) {
            char value = text[i - 1];
            if (value == ']') square++;
            else if (value == '[' && square > 0) square--;
            else if (value == ')') round++;
            else if (value == '(' && round > 0) round--;
            else if (square == 0 && round == 0
                     && (value == '>' || value == '+' || value == '~')) {
                split = i - 1; combinator = value; break;
            } else if (square == 0 && round == 0
                       && isspace((unsigned char) value)) {
                size_t escaped = i - 1;
                size_t backslashes = 0;
                while (escaped != 0 && text[escaped - 1] == '\\') {
                    escaped--;
                    backslashes++;
                }
                if ((backslashes & 1u) != 0) continue;
                size_t right = i;
                while (right < length
                       && isspace((unsigned char) text[right])) right++;
                if (right < length) {
                    size_t left = i - 1;
                    while (left != 0
                           && isspace((unsigned char) text[left - 1])) left--;
                    if (left != 0 && (text[left - 1] == '>'
                                      || text[left - 1] == '+'
                                      || text[left - 1] == '~')) {
                        split = left - 1;
                        combinator = text[left - 1];
                    } else {
                        split = i - 1;
                        combinator = ' ';
                    }
                    break;
                }
            }
        }
    }
    const char *compound = prepared && prepared_offset < length
        ? text + prepared_offset
        : (combinator == 0 ? text : text + split + 1);
    size_t compound_length = prepared && prepared_offset < length
        ? length - prepared_offset
        : (combinator == 0 ? length : length - split - 1);
    trim(&compound, &compound_length);
    if (!compound_matches_depth(
            sheet, node, compound, compound_length,
            functional_depth, scope)) {
        return false;
    }
    if (combinator == 0) return true;
    size_t prefix_length = split;
    while (prefix_length != 0
           && isspace((unsigned char) text[prefix_length - 1])) prefix_length--;
    if (combinator == '>') {
        lxb_dom_node_t *parent = node->parent;
        while (parent != NULL && parent->type != LXB_DOM_NODE_TYPE_ELEMENT) parent = parent->parent;
        if (parent != NULL) {
            STYLE_SELECTOR_COUNT(sheet, selector_ancestor_visits, 1);
            if (!style_selector_cooperate_visit(sheet, parent)) return false;
        }
        return parent != NULL
               && style_selector_matches_internal(
                   sheet, parent, text, prefix_length, 0, false,
                   functional_depth, relationship_depth + 1u, scope);
    }
    if (combinator == '+') {
        lxb_dom_node_t *sibling = node->prev;
        while (sibling != NULL
               && sibling->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            sibling = sibling->prev;
        }
        if (sibling != NULL) {
            STYLE_SELECTOR_COUNT(sheet, selector_sibling_visits, 1);
            if (!style_selector_cooperate_visit(sheet, sibling)) return false;
        }
        return sibling != NULL
               && style_selector_matches_internal(
                   sheet, sibling, text, prefix_length, 0, false,
                   functional_depth, relationship_depth + 1u, scope);
    }
    if (combinator == '~') {
        size_t visits = 0;
        for (lxb_dom_node_t *sibling = node->prev;
             sibling != NULL && visits < STYLE_RELATIVE_SELECTOR_VISIT_LIMIT;
             sibling = sibling->prev, visits++) {
            STYLE_SELECTOR_COUNT(sheet, selector_sibling_visits, 1);
            if (!style_selector_cooperate_visit(sheet, sibling)) return false;
            if (sibling->type == LXB_DOM_NODE_TYPE_ELEMENT
                && style_selector_matches_internal(
                    sheet, sibling, text, prefix_length, 0, false,
                    functional_depth, relationship_depth + 1u, scope)) {
                return true;
            }
        }
        return false;
    }
    unsigned ancestor_depth = 0;
    for (lxb_dom_node_t *parent = node->parent;
         parent != NULL
             && ancestor_depth < STYLE_RELATIVE_SELECTOR_DEPTH_LIMIT;
         parent = parent->parent, ancestor_depth++) {
        STYLE_SELECTOR_COUNT(sheet, selector_ancestor_visits, 1);
        if (!style_selector_cooperate_visit(sheet, parent)) return false;
        if (parent->type == LXB_DOM_NODE_TYPE_ELEMENT
            && style_selector_matches_internal(
                sheet, parent, text, prefix_length, 0, false,
                functional_depth, relationship_depth + 1u, scope)) return true;
    }
    return false;
}

bool style_selector_matches_profiled(const Stylesheet *sheet,
                                     lxb_dom_node_t *node,
                                     const char *text, size_t length)
{
    STYLE_SELECTOR_COUNT(sheet, selector_match_calls, 1);
    STYLE_SELECTOR_COUNT(sheet, selector_match_characters, length);
    bool matched = style_selector_matches_internal(
        sheet, node, text, length, 0, false, 0, 0, NULL);
    if (matched) STYLE_SELECTOR_COUNT(sheet, selector_match_successes, 1);
    return matched;
}

bool style_selector_matches_prepared(const Stylesheet *sheet,
                                     lxb_dom_node_t *node,
                                     const char *text, size_t length,
                                     size_t rightmost_compound_offset)
{
    STYLE_SELECTOR_COUNT(sheet, selector_match_calls, 1);
    STYLE_SELECTOR_COUNT(sheet, selector_match_characters, length);
    bool matched = style_selector_matches_internal(
        sheet, node, text, length, rightmost_compound_offset, true, 0, 0,
        NULL);
    if (matched) STYLE_SELECTOR_COUNT(sheet, selector_match_successes, 1);
    return matched;
}

static bool style_selector_program_matches_at(
    const Stylesheet *sheet, const StyleRule *rule, lxb_dom_node_t *node,
    size_t instruction, unsigned depth,
    const StyleMatchSubject *known_subject)
{
    if (sheet == NULL || rule == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || depth > STYLE_SELECTOR_PROGRAM_DEPTH_LIMIT) return false;
    if (sheet->selector_cooperate_cancelled) return false;
    STYLE_SELECTOR_COUNT(sheet, selector_compound_calls, 1);
    StyleMatchSubject local_subject = {0};
    const StyleMatchSubject *subject = known_subject;
    if (subject != NULL && subject->node == node) {
        STYLE_SELECTOR_COUNT(sheet, selector_subject_cache_hits, 1);
    } else {
        STYLE_SELECTOR_COUNT(sheet, selector_subject_cache_misses, 1);
        style_match_subject_prepare(node, &local_subject);
        subject = &local_subject;
    }
    while (instruction < sheet->selector_program_instruction_count) {
        const StyleSelectorInstruction *op =
            &sheet->selector_program[instruction++];
        if (op->opcode == STYLE_SELECTOR_TAG_ID) {
            STYLE_SELECTOR_COUNT(sheet, selector_tag_id_checks, 1);
            if (subject->tag_id != (uintptr_t) op->text_offset) return false;
            continue;
        }
        const char *wanted = rule->selector + op->text_offset;
        size_t wanted_length = op->text_length;
        if (op->opcode == STYLE_SELECTOR_TAG) {
            if (subject->tag == NULL
                || subject->tag_length != wanted_length
                || memcmp(subject->tag, wanted, wanted_length) != 0) {
                return false;
            }
            continue;
        }
        if (op->opcode == STYLE_SELECTOR_CLASS) {
            if (subject->classes == NULL
                || !class_contains_length(
                    subject->classes, subject->classes_length,
                    wanted, wanted_length)) return false;
            continue;
        }
        if (op->opcode == STYLE_SELECTOR_ID) {
            if (subject->id == NULL
                || subject->id_length != wanted_length
                || memcmp(subject->id, wanted, wanted_length) != 0) {
                return false;
            }
            continue;
        }
        if (op->opcode == STYLE_SELECTOR_COMPOUND) {
            if (!compound_matches(sheet, node, wanted, wanted_length)) {
                return false;
            }
            continue;
        }
        if (op->opcode == STYLE_SELECTOR_END) return true;
        if (op->opcode == STYLE_SELECTOR_PARENT) {
            lxb_dom_node_t *parent = node->parent;
            while (parent != NULL
                   && parent->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                parent = parent->parent;
            }
            if (parent != NULL) {
                STYLE_SELECTOR_COUNT(sheet, selector_ancestor_visits, 1);
                if (!style_selector_cooperate_visit(sheet, parent)) {
                    return false;
                }
            }
            return parent != NULL && style_selector_program_matches_at(
                sheet, rule, parent, instruction, depth + 1u, NULL);
        }
        if (op->opcode == STYLE_SELECTOR_ADJACENT) {
            lxb_dom_node_t *sibling = node->prev;
            while (sibling != NULL
                   && sibling->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                sibling = sibling->prev;
            }
            if (sibling != NULL) {
                STYLE_SELECTOR_COUNT(sheet, selector_sibling_visits, 1);
                if (!style_selector_cooperate_visit(sheet, sibling)) {
                    return false;
                }
            }
            return sibling != NULL && style_selector_program_matches_at(
                sheet, rule, sibling, instruction, depth + 1u, NULL);
        }
        if (op->opcode == STYLE_SELECTOR_GENERAL_SIBLING) {
            for (lxb_dom_node_t *sibling = node->prev; sibling != NULL;
                 sibling = sibling->prev) {
                STYLE_SELECTOR_COUNT(sheet, selector_sibling_visits, 1);
                if (!style_selector_cooperate_visit(sheet, sibling)) {
                    return false;
                }
                if (sibling->type == LXB_DOM_NODE_TYPE_ELEMENT
                    && style_selector_program_matches_at(
                        sheet, rule, sibling, instruction, depth + 1u,
                        NULL)) {
                    return true;
                }
            }
            return false;
        }
        if (op->opcode == STYLE_SELECTOR_ANCESTOR) {
            for (lxb_dom_node_t *parent = node->parent; parent != NULL;
                 parent = parent->parent) {
                STYLE_SELECTOR_COUNT(sheet, selector_ancestor_visits, 1);
                if (!style_selector_cooperate_visit(sheet, parent)) {
                    return false;
                }
                if (parent->type == LXB_DOM_NODE_TYPE_ELEMENT
                    && style_selector_program_matches_at(
                        sheet, rule, parent, instruction, depth + 1u,
                        NULL)) {
                    return true;
                }
            }
            return false;
        }
        return false;
    }
    return false;
}

bool style_rule_selector_matches_subject(
    const Stylesheet *sheet, size_t rule_index, lxb_dom_node_t *node,
    const StyleMatchSubject *subject)
{
    if (sheet == NULL || rule_index >= sheet->count) return false;
    const StyleRule *rule = &sheet->rules[rule_index];
    if (sheet->selector_program_ready
        && sheet->selector_program_offsets != NULL
        && rule_index < sheet->count) {
        uint16_t instruction = sheet->selector_program_offsets[rule_index];
        if (instruction != UINT16_MAX
            && instruction < sheet->selector_program_instruction_count) {
            STYLE_SELECTOR_COUNT(sheet, selector_compiled_rule_calls, 1);
            STYLE_SELECTOR_COUNT(sheet, selector_match_calls, 1);
            bool matched = style_selector_program_matches_at(
                sheet, rule, node, instruction, 0, subject);
            if (matched) {
                STYLE_SELECTOR_COUNT(sheet, selector_match_successes, 1);
                STYLE_SELECTOR_COUNT(
                    sheet, selector_compiled_rule_matches, 1);
            }
            return matched;
        }
    }
    STYLE_SELECTOR_COUNT(sheet, selector_fallback_rule_calls, 1);
    return style_selector_matches_prepared(
        sheet, node, rule->selector, rule->selector_length,
        style_rule_rightmost_compound(rule));
}

bool style_rule_selector_matches(const Stylesheet *sheet,
                                 size_t rule_index,
                                 lxb_dom_node_t *node)
{
    return style_rule_selector_matches_subject(
        sheet, rule_index, node, NULL);
}

bool style_selector_matches(lxb_dom_node_t *node, const char *text,
                            size_t length)
{
    return style_selector_matches_profiled(NULL, node, text, length);
}

bool style_selector_matches_scoped(lxb_dom_node_t *node, const char *text,
                                   size_t length,
                                   const lxb_dom_node_t *scope)
{
    return style_selector_matches_internal(
        NULL, node, text, length, 0, false, 0, 0, scope);
}
