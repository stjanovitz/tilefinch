/* Selector syntax check for querySelector(), matches(), closest() and
   CSS.supports(selector()): a recursive descent over the CSS tokens a
   selector may contain, following the grammar of Selectors Level 4
   section 18. Names compare ASCII case-insensitively. Each step advances
   the cursor, and a ")" at nesting depth above zero ends the argument being
   parsed, so the whole check is one pass over the text. */
#include "tilefinch/selector_syntax.h"

#include <string.h>

typedef struct {
    const unsigned char *text;
    size_t length;
    size_t at;
    unsigned depth;
} SelectorScan;

/* Argument grammars of functional pseudos, as the last character of a
   kind string: an empty kind takes no argument, a kind starting with "?"
   may be written with or without one.
     s complex selector list      r relative selector list
     f :is()/:where() list        c compound selector
     C compound selector list     n An+B [of S]       a An+B
     i one identifier             p identifiers       l :lang() ranges
     d integers */
typedef struct {
    const char *name;
    const char *kind;
} SelectorPseudo;

static const SelectorPseudo selector_pseudo_classes[] = {
    { "active", "" }, { "any-link", "" }, { "autofill", "" },
    { "buffering", "" }, { "checked", "" }, { "current", "?C" },
    { "default", "" }, { "defined", "" }, { "dir", "i" },
    { "disabled", "" }, { "empty", "" }, { "enabled", "" },
    { "first-child", "" }, { "first-of-type", "" }, { "focus", "" },
    { "focus-visible", "" }, { "focus-within", "" }, { "fullscreen", "" },
    { "future", "" }, { "has", "r" }, { "heading", "?d" },
    { "host", "?c" }, { "host-context", "c" }, { "hover", "" },
    { "indeterminate", "" }, { "in-range", "" }, { "invalid", "" },
    { "is", "f" }, { "lang", "l" }, { "last-child", "" },
    { "last-of-type", "" }, { "link", "" }, { "local-link", "" },
    { "modal", "" }, { "muted", "" }, { "not", "s" },
    { "nth-child", "n" }, { "nth-col", "a" }, { "nth-last-child", "n" },
    { "nth-last-col", "a" }, { "nth-last-of-type", "a" },
    { "nth-of-type", "a" }, { "only-child", "" }, { "only-of-type", "" },
    { "open", "" }, { "optional", "" }, { "out-of-range", "" },
    { "past", "" }, { "paused", "" }, { "picture-in-picture", "" },
    { "placeholder-shown", "" }, { "playing", "" }, { "popover-open", "" },
    { "read-only", "" }, { "read-write", "" }, { "required", "" },
    { "root", "" }, { "scope", "" }, { "seeking", "" }, { "stalled", "" },
    { "state", "i" }, { "target", "" }, { "target-current", "" },
    { "user-invalid", "" }, { "user-valid", "" }, { "valid", "" },
    { "visited", "" }, { "volume-locked", "" }, { "where", "f" },
    { "-webkit-any-link", "" }, { "-webkit-autofill", "" },
    { "-webkit-full-screen", "" },
};

static const SelectorPseudo selector_pseudo_elements[] = {
    { "after", "" }, { "backdrop", "" }, { "before", "" }, { "cue", "?s" },
    { "cue-region", "?s" }, { "file-selector-button", "" },
    { "first-letter", "" }, { "first-line", "" }, { "marker", "" },
    { "part", "p" }, { "placeholder", "" }, { "selection", "" },
    { "slotted", "c" },
};

static int byte_at(const SelectorScan *scan, size_t at)
{
    return at < scan->length ? scan->text[at] : -1;
}

static bool is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_digit(int c) { return c >= '0' && c <= '9'; }

static bool is_hex(int c)
{
    return is_digit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f');
}

/* NUL is preprocessed to U+FFFD, a name character, like any non-ASCII
   byte of UTF-8. */
static bool is_name_start(int c)
{
    return ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_' || c >= 0x80
        || c == 0;
}

static bool is_name_char(int c)
{
    return is_name_start(c) || is_digit(c) || c == '-';
}

/* The end of the argument (or of the text) being parsed. */
static bool at_end(const SelectorScan *scan)
{
    int c = byte_at(scan, scan->at);
    return c < 0 || (scan->depth > 0 && c == ')');
}

static bool escape_at(const SelectorScan *scan, size_t at)
{
    if (byte_at(scan, at) != '\\') return false;
    int next = byte_at(scan, at + 1);
    return next != '\n' && next != '\r' && next != '\f';
}

static bool ident_at(const SelectorScan *scan, size_t at)
{
    int c = byte_at(scan, at);
    if (c == '-') {
        int next = byte_at(scan, at + 1);
        return is_name_start(next) || next == '-' || escape_at(scan, at + 1);
    }
    return is_name_start(c) || escape_at(scan, at);
}

/* Consumes a name the caller found with ident_at(). With `out`, stores its
   ASCII-lowercased, unescaped spelling; a non-ASCII character or a name
   that does not fit stores 0x80, which matches no keyword. */
static void consume_name(SelectorScan *scan, char *out, size_t capacity)
{
    size_t used = 0;
    bool fits = true;
    for (;;) {
        int c = byte_at(scan, scan->at), value;
        if (is_name_char(c)) {
            value = c;
            scan->at++;
        } else if (escape_at(scan, scan->at)) {
            scan->at++;
            c = byte_at(scan, scan->at);
            if (is_hex(c)) {
                unsigned long code = 0;
                for (unsigned digits = 0;
                     digits < 6u && is_hex(byte_at(scan, scan->at));
                     digits++) {
                    int h = byte_at(scan, scan->at++);
                    code = code * 16u
                        + (unsigned long) (is_digit(h) ? h - '0'
                                                       : (h | 32) - 'a' + 10);
                }
                if (byte_at(scan, scan->at) == '\r'
                    && byte_at(scan, scan->at + 1) == '\n')
                    scan->at += 2;
                else if (is_space(byte_at(scan, scan->at)))
                    scan->at++;
                value = code != 0 && code < 0x80 ? (int) code : 0x80;
            } else if (c < 0) {
                value = 0x80;
            } else {
                value = c;
                scan->at++;
            }
        } else {
            break;
        }
        if (out == NULL) continue;
        if (used + 1u < capacity)
            out[used++] = (char) (value >= 'A' && value <= 'Z'
                                  ? value + 32 : value);
        else
            fits = false;
    }
    if (out == NULL) return;
    if (!fits) {
        out[0] = (char) 0x80;
        used = 1;
    }
    out[used] = '\0';
}

static bool skip_space(SelectorScan *scan)
{
    size_t start = scan->at;
    while (is_space(byte_at(scan, scan->at))) scan->at++;
    return scan->at > start;
}

static bool consume_string(SelectorScan *scan)
{
    int quote = byte_at(scan, scan->at++);
    while (scan->at <= scan->length) {
        int c = byte_at(scan, scan->at++);
        if (c == quote) return true;
        if (c < 0 || c == '\n' || c == '\r' || c == '\f') return false;
        if (c == '\\' && byte_at(scan, scan->at++) < 0) return false;
    }
    return false;
}

static bool consume_digits(SelectorScan *scan)
{
    size_t start = scan->at;
    while (is_digit(byte_at(scan, scan->at))) scan->at++;
    return scan->at > start;
}

static bool consume_keyword(SelectorScan *scan, const char *word)
{
    size_t length = strlen(word);
    if (length > scan->length - scan->at) return false;
    for (size_t i = 0; i < length; i++)
        if ((scan->text[scan->at + i] | 32) != word[i]) return false;
    scan->at += length;
    return true;
}

/* An+B: odd, even, an integer, or [+-]?<digits>?n with an optional
   [+-] <digits> that may be spaced from both. */
static bool consume_anb(SelectorScan *scan)
{
    if (consume_keyword(scan, "odd") || consume_keyword(scan, "even"))
        return true;
    int c = byte_at(scan, scan->at);
    if (c == '+' || c == '-') scan->at++;
    bool digits = consume_digits(scan);
    if ((byte_at(scan, scan->at) | 32) != 'n') return digits;
    scan->at++;
    size_t after_n = scan->at;
    skip_space(scan);
    c = byte_at(scan, scan->at);
    if (c == '+' || c == '-') {
        scan->at++;
        skip_space(scan);
        if (consume_digits(scan)) return true;
    }
    scan->at = after_n;
    return true;
}

static const char *pseudo_kind(const SelectorPseudo *table, size_t count,
                               const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(table[i].name, name) == 0) return table[i].kind;
    return NULL;
}

static int parse_compound(SelectorScan *scan, bool allow_element,
                          bool in_has, bool compound_context);
static bool parse_list(SelectorScan *scan, bool relative,
                       bool allow_element, bool in_has,
                       bool compound_context);

static bool parse_attribute(SelectorScan *scan)
{
    scan->at++;
    skip_space(scan);
    if (!ident_at(scan, scan->at)) return false;
    consume_name(scan, NULL, 0);
    skip_space(scan);
    int c = byte_at(scan, scan->at);
    if (c == ']') {
        scan->at++;
        return true;
    }
    if (c == '~' || c == '|' || c == '^' || c == '$' || c == '*')
        c = byte_at(scan, ++scan->at);
    if (c != '=') return false;
    scan->at++;
    skip_space(scan);
    c = byte_at(scan, scan->at);
    if (c == '"' || c == '\'') {
        if (!consume_string(scan)) return false;
    } else if (ident_at(scan, scan->at)) {
        consume_name(scan, NULL, 0);
    } else {
        return false;
    }
    skip_space(scan);
    if (ident_at(scan, scan->at)) {
        char modifier[4];
        consume_name(scan, modifier, sizeof(modifier));
        if (strcmp(modifier, "i") != 0 && strcmp(modifier, "s") != 0)
            return false;
        skip_space(scan);
    }
    if (byte_at(scan, scan->at) != ']') return false;
    scan->at++;
    return true;
}

static bool parse_argument(SelectorScan *scan, char grammar, bool in_has,
                           bool compound_context)
{
    skip_space(scan);
    switch (grammar) {
    case 's':
        /* :not() inside :host() or ::slotted() takes compounds only. */
        if (compound_context) grammar = 'C';
        else return parse_list(scan, false, false, in_has, false);
        break;
    case 'r':
        return parse_list(scan, true, false, in_has, compound_context);
    case 'f':
        /* Forgiving in the standard, but the native matcher has no notion
           of a dropped entry: only the empty list (which matches nothing)
           is let through. */
        return at_end(scan)
            || parse_list(scan, false, false, in_has, compound_context);
    case 'c': {
        int kind = parse_compound(scan, false, in_has, true);
        skip_space(scan);
        return kind == 1 && at_end(scan);
    }
    case 'n':
    case 'a': {
        if (!consume_anb(scan)) return false;
        bool spaced = skip_space(scan);
        if (at_end(scan)) return true;
        if (grammar != 'n' || !spaced || !ident_at(scan, scan->at))
            return false;
        char word[4];
        consume_name(scan, word, sizeof(word));
        return strcmp(word, "of") == 0
            && parse_list(scan, false, false, in_has, compound_context);
    }
    case 'i':
        if (!ident_at(scan, scan->at)) return false;
        consume_name(scan, NULL, 0);
        skip_space(scan);
        return at_end(scan);
    case 'p':
        while (scan->at < scan->length) {
            if (!ident_at(scan, scan->at)) return false;
            consume_name(scan, NULL, 0);
            bool spaced = skip_space(scan);
            if (at_end(scan)) return true;
            if (!spaced) return false;
        }
        return false;
    default:
        break;
    }
    /* Comma-separated lists of compounds, :lang() ranges or integers. */
    while (scan->at < scan->length) {
        int c = byte_at(scan, scan->at);
        if (grammar == 'C') {
            if (parse_compound(scan, false, in_has, true) != 1) return false;
        } else if (grammar == 'd') {
            if (!consume_digits(scan)) return false;
        } else if (grammar == 'l' && (c == '"' || c == '\'')) {
            if (!consume_string(scan)) return false;
        } else if (grammar == 'l' && ident_at(scan, scan->at)) {
            consume_name(scan, NULL, 0);
        } else {
            return false;
        }
        skip_space(scan);
        if (at_end(scan)) return true;
        if (byte_at(scan, scan->at) != ',') return false;
        scan->at++;
        skip_space(scan);
    }
    return false;
}

/* Returns 0 (invalid), 1 (pseudo-class) or 2 (pseudo-element). */
static int parse_pseudo(SelectorScan *scan, bool allow_element, bool in_has,
                        bool compound_context)
{
    bool element = byte_at(scan, ++scan->at) == ':';
    if (element) scan->at++;
    if (!ident_at(scan, scan->at)) return 0;
    char name[24];
    consume_name(scan, name, sizeof(name));
    /* CSS 2 spells these four pseudo-elements with one colon too. */
    if (!element
        && (!strcmp(name, "before") || !strcmp(name, "after")
            || !strcmp(name, "first-line") || !strcmp(name, "first-letter")))
        element = true;
    if (element && !allow_element) return 0;
    const char *kind = element
        ? pseudo_kind(selector_pseudo_elements,
                      sizeof(selector_pseudo_elements)
                          / sizeof(selector_pseudo_elements[0]), name)
        : pseudo_kind(selector_pseudo_classes,
                      sizeof(selector_pseudo_classes)
                          / sizeof(selector_pseudo_classes[0]), name);
    if (kind == NULL) return 0;
    int result = element ? 2 : 1;
    if (byte_at(scan, scan->at) != '(')
        return kind[0] == '\0' || kind[0] == '?' ? result : 0;
    bool has = strcmp(name, "has") == 0;
    if (kind[0] == '\0' || (has && in_has)
        || scan->depth >= SELECTOR_SYNTAX_NESTING_LIMIT)
        return 0;
    scan->at++;
    scan->depth++;
    bool valid = parse_argument(scan, kind[strlen(kind) - 1u],
                                in_has || has, compound_context);
    scan->depth--;
    if (!valid || byte_at(scan, scan->at) != ')') return 0;
    scan->at++;
    return result;
}

/* Returns 0 (invalid or empty), 1 (compound), or 2 (compound ending in a
   pseudo-element, which only further pseudos may follow). */
static int parse_compound(SelectorScan *scan, bool allow_element,
                          bool in_has, bool compound_context)
{
    size_t start = scan->at;
    int kind = 1;
    if (byte_at(scan, scan->at) == '*') scan->at++;
    else if (ident_at(scan, scan->at)) consume_name(scan, NULL, 0);
    while (scan->at < scan->length) {
        int c = byte_at(scan, scan->at);
        if (c == '#' || c == '.') {
            if (kind == 2) return 0;
            /* An ID selector's hash must be an identifier: "#1" is not. */
            if (!ident_at(scan, ++scan->at)) return 0;
            consume_name(scan, NULL, 0);
        } else if (c == '[') {
            if (kind == 2 || !parse_attribute(scan)) return 0;
        } else if (c == ':') {
            int pseudo = parse_pseudo(scan, allow_element, in_has,
                                      compound_context);
            if (pseudo == 0) return 0;
            if (pseudo == 2) kind = 2;
        } else {
            break;
        }
    }
    return scan->at > start ? kind : 0;
}

static bool is_combinator(int c) { return c == '>' || c == '+' || c == '~'; }

static bool parse_complex(SelectorScan *scan, bool relative,
                          bool allow_element, bool in_has,
                          bool compound_context)
{
    skip_space(scan);
    if (relative && is_combinator(byte_at(scan, scan->at))) {
        scan->at++;
        skip_space(scan);
    }
    int kind = parse_compound(scan, allow_element, in_has,
                              compound_context);
    while (kind != 0) {
        bool spaced = skip_space(scan);
        if (at_end(scan) || byte_at(scan, scan->at) == ',') return true;
        if (kind == 2) return false;
        if (is_combinator(byte_at(scan, scan->at))) {
            scan->at++;
            skip_space(scan);
        } else if (!spaced) {
            return false;
        }
        kind = parse_compound(scan, allow_element, in_has, compound_context);
    }
    return false;
}

static bool parse_list(SelectorScan *scan, bool relative,
                       bool allow_element, bool in_has,
                       bool compound_context)
{
    while (parse_complex(scan, relative, allow_element, in_has,
                         compound_context)) {
        if (at_end(scan)) return true;
        scan->at++; /* the comma */
    }
    return false;
}

bool selector_syntax_valid(const char *text, size_t length)
{
    if (text == NULL) return false;
    SelectorScan scan = {
        .text = (const unsigned char *) text, .length = length,
    };
    return parse_list(&scan, false, true, false, false);
}
