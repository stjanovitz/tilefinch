/* CSS declaration-block scanning and the per-property parser that
   writes parsed values into a ComputedStyle + presence mask.
   Split out of style.c. */

#include "style_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Legacy clip:rect() remains common in accessible visually-hidden content.
   The renderer already has bounded overflow clips; detect the important
   zero-area case here so an absolutely positioned box cannot leak pixels
   through minimum layout dimensions. */
static bool parse_empty_clip_rect(const Stylesheet *sheet,
                                  const char *value, size_t value_length,
                                  bool *empty)
{
    if (empty == NULL) return false;
    char resolved[96];
    if (!style_resolve_value(sheet, value, value_length, resolved,
                       sizeof(resolved), 0)) return false;
    char *at = resolved;
    while (isspace((unsigned char) *at)) at++;
    if (strcmp(at, "auto") == 0) {
        *empty = false;
        return true;
    }
    if (strncasecmp(at, "rect(", 5) != 0) return false;
    at += 5;
    double coordinates[4] = {0};
    bool automatic[4] = {false};
    for (size_t i = 0; i < 4; i++) {
        while (isspace((unsigned char) *at)) at++;
        if (strncasecmp(at, "auto", 4) == 0
            && !name_character(at[4])) {
            automatic[i] = true;
            at += 4;
        } else {
            char *end = NULL;
            coordinates[i] = strtod(at, &end);
            if (end == at) return false;
            at = end;
            if (strncasecmp(at, "px", 2) == 0) at += 2;
            else if (coordinates[i] != 0.0
                     && (isalpha((unsigned char) *at) || *at == '%')) {
                return false;
            }
        }
        while (isspace((unsigned char) *at)) at++;
        if (i < 3 && *at == ',') at++;
    }
    while (isspace((unsigned char) *at)) at++;
    if (*at++ != ')') return false;
    while (isspace((unsigned char) *at)) at++;
    if (*at != '\0') return false;
    *empty = (!automatic[1] && !automatic[3]
              && coordinates[1] <= coordinates[3])
        || (!automatic[0] && !automatic[2]
            && coordinates[2] <= coordinates[0]);
    return true;
}

size_t declaration_value_length(const char *value, size_t length)
{
    if (value == NULL) return length;
    size_t end = length;
    while (end != 0 && isspace((unsigned char) value[end - 1])) end--;
    static const char keyword[] = "important";
    if (end < sizeof(keyword) - 1) return length;
    size_t keyword_at = end - (sizeof(keyword) - 1);
    for (size_t i = 0; i < sizeof(keyword) - 1; i++) {
        if (tolower((unsigned char) value[keyword_at + i]) != keyword[i]) {
            return length;
        }
    }
    size_t at = keyword_at;
    while (at != 0) {
        while (at != 0 && isspace((unsigned char) value[at - 1])) at--;
        if (at >= 2 && value[at - 2] == '*' && value[at - 1] == '/') {
            size_t comment_end = at;
            at -= 2;
            while (at >= 2
                   && !(value[at - 2] == '/' && value[at - 1] == '*')) {
                at--;
            }
            if (at < 2) return length;
            at -= 2;
            if (at == comment_end) return length;
            continue;
        }
        break;
    }
    if (at == 0 || value[at - 1] != '!') return length;
    size_t result = at - 1;
    while (result != 0
           && isspace((unsigned char) value[result - 1])) result--;
    return result;
}

bool declaration_is_important(const char *value, size_t length)
{
    return declaration_value_length(value, length) != length;
}

size_t skip_css_space_and_comments(const char *text, size_t length,
                                          size_t at)
{
    while (at < length) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        if (at + 1 >= length || text[at] != '/' || text[at + 1] != '*') {
            break;
        }
        at += 2;
        while (at + 1 < length
               && !(text[at] == '*' && text[at + 1] == '/')) at++;
        at += at + 1 < length ? 2 : 0;
    }
    return at;
}

size_t find_declaration_end(const char *text, size_t length,
                                   size_t at)
{
    int parentheses = 0;
    char quote = 0;
    for (size_t end = at; end < length; end++) {
        if (quote != 0) {
            if (text[end] == '\\' && end + 1 < length) end++;
            else if (text[end] == quote) quote = 0;
            continue;
        }
        if (text[end] == '\'' || text[end] == '"') {
            quote = text[end];
            continue;
        }
        if (text[end] == '/' && end + 1 < length && text[end + 1] == '*') {
            end += 2;
            while (end + 1 < length
                   && !(text[end] == '*' && text[end + 1] == '/')) end++;
            continue;
        }
        if (text[end] == '(') parentheses++;
        else if (text[end] == ')' && parentheses > 0) parentheses--;
        else if ((text[end] == '{' || text[end] == '}')
                 && parentheses == 0) return end;
        else if (text[end] == ';' && parentheses == 0) return end;
    }
    return length;
}

/* Property dispatch.  Each CSS property (or property alias) maps to one
   handler via a table sorted by (name length, name); the former ~1,450-line
   if/else chain dispatched the same bodies through a strcmp cascade.
   Handlers return false only to abort the whole parse (allocation failure),
   matching the old chain's return semantics. */
typedef struct {
    Stylesheet *sheet;
    const char *name;
    size_t name_length;
    const char *value;
    size_t value_length;
    ComputedStyle *style;
    uint64_t *mask;
    uint64_t *mask_high;
    uint64_t *inherit_mask;
    bool important;
} StylePropertyParse;

typedef bool (*StylePropertyHandler)(const StylePropertyParse *parse);

typedef struct {
    const char *name;
    uint8_t name_length;
    StylePropertyHandler handler;
} StylePropertyEntry;

/* Property families remain in one translation unit so the parser's helpers
   and dispatch table stay private while each CSS responsibility is local. */
#include "style_properties/visual_basics.inc"
#include "style_properties/typography.inc"
#include "style_properties/box_model.inc"
#include "style_properties/layout.inc"
#include "style_properties/positioning_visual.inc"
#include "style_properties/dispatch.inc"
