#include "tilefinch/script_lazy.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define SCRIPT_LAZY_FACTORY_LIMIT 4096u
#define SCRIPT_LAZY_SCAN_BYTE_LIMIT (8u * 1024u * 1024u)
#define SCRIPT_LAZY_SIGNATURE_SCAN_LIMIT 1024u

#define js_malloc(b, s) \
    budget_malloc_category((b), BUDGET_CATEGORY_JAVASCRIPT, (s))
#define js_realloc(b, p, s) \
    budget_realloc_category((b), BUDGET_CATEGORY_JAVASCRIPT, (p), (s))

typedef enum {
    JS_TOKEN_EOF = 0,
    JS_TOKEN_IDENTIFIER,
    JS_TOKEN_NUMBER,
    JS_TOKEN_STRING,
    JS_TOKEN_TEMPLATE,
    JS_TOKEN_REGEX,
    JS_TOKEN_PUNCTUATOR,
    JS_TOKEN_INVALID
} JsTokenKind;

typedef struct {
    JsTokenKind kind;
    size_t begin;
    size_t end;
} JsToken;

typedef struct {
    const char *source;
    size_t length;
    size_t offset;
    size_t *work_remaining;
    bool expression_expected;
    bool failed;
} JsLexer;

static bool lexer_next(JsLexer *lexer, JsToken *token);

/* Most external scripts are not Webpack chunk registrations.  Reject them
   without tokenizing the whole body: every accepted envelope necessarily
   contains one of these roots before its first factory table.  A banner
   larger than the bounded prefix merely opts that script into the ordinary
   eager path; it never changes author-visible behavior. */
static bool has_bounded_webpack_signature(const char *source, size_t length)
{
    static const char *const signatures[] = {
        "self.webpackChunk",
        "window.webpackChunk",
        "globalThis.webpackChunk"
    };
    size_t limit = length < SCRIPT_LAZY_SIGNATURE_SCAN_LIMIT
        ? length : SCRIPT_LAZY_SIGNATURE_SCAN_LIMIT;
    for (size_t i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++) {
        size_t signature_length = strlen(signatures[i]);
        if (signature_length > limit) continue;
        for (size_t at = 0; at <= limit - signature_length; at++) {
            if (source[at] == signatures[i][0]
                && memcmp(source + at, signatures[i],
                          signature_length) == 0) return true;
        }
    }
    return false;
}

static bool identifier_start(unsigned char byte)
{
    return isalpha(byte) != 0 || byte == '_' || byte == '$' || byte >= 0x80;
}

static bool identifier_continue(unsigned char byte)
{
    return identifier_start(byte) || isdigit(byte) != 0;
}

static bool bytes_equal(const char *source, const JsToken *token,
                        const char *expected)
{
    size_t length = strlen(expected);
    return token->end - token->begin == length
        && memcmp(source + token->begin, expected, length) == 0;
}

static bool keyword_expects_expression(const char *source,
                                       const JsToken *token)
{
    static const char *const keywords[] = {
        "await", "case", "delete", "do", "else", "in", "instanceof",
        "new", "of", "return", "throw", "typeof", "void", "yield"
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (bytes_equal(source, token, keywords[i])) return true;
    }
    return false;
}

static bool skip_quoted(JsLexer *lexer, unsigned char quote, size_t *end)
{
    size_t at = lexer->offset + 1;
    while (at < lexer->length) {
        unsigned char byte = (unsigned char) lexer->source[at++];
        if (byte == quote) {
            *end = at;
            return true;
        }
        if (byte == '\\') {
            if (at >= lexer->length) return false;
            unsigned char escaped = (unsigned char) lexer->source[at++];
            if (escaped == '\r' && at < lexer->length
                && lexer->source[at] == '\n') at++;
            continue;
        }
        if (byte == '\r' || byte == '\n') return false;
    }
    return false;
}

static bool skip_regular_expression(JsLexer *lexer, size_t *end)
{
    size_t at = lexer->offset + 1;
    bool character_class = false;
    while (at < lexer->length) {
        unsigned char byte = (unsigned char) lexer->source[at++];
        if (byte == '\r' || byte == '\n') return false;
        if (byte == '\\') {
            if (at >= lexer->length) return false;
            at++;
            continue;
        }
        if (byte == '[') character_class = true;
        else if (byte == ']') character_class = false;
        else if (byte == '/' && !character_class) {
            while (at < lexer->length
                   && identifier_continue(
                       (unsigned char) lexer->source[at])) at++;
            *end = at;
            return true;
        }
    }
    return false;
}

/* Template raw text is not JavaScript token text.  Its ${...} substitutions
   are, so scan each substitution with the same lexer and delimiter rules;
   this handles nested strings, regular expressions, and templates without
   treating their braces as factory-body braces. */
static bool skip_template(JsLexer *lexer, size_t *end)
{
    size_t at = lexer->offset + 1;
    while (at < lexer->length) {
        unsigned char byte = (unsigned char) lexer->source[at++];
        if (byte == '\\') {
            if (at >= lexer->length) return false;
            at++;
            continue;
        }
        if (byte == '`') {
            *end = at;
            return true;
        }
        if (byte != '$' || at >= lexer->length
            || lexer->source[at] != '{') continue;
        at++;
        JsLexer expression = {
            .source = lexer->source,
            .length = lexer->length,
            .offset = at,
            .work_remaining = lexer->work_remaining,
            .expression_expected = true
        };
        size_t depth = 1;
        while (depth != 0) {
            JsToken token;
            if (!lexer_next(&expression, &token)
                || token.kind == JS_TOKEN_EOF) return false;
            if (token.kind != JS_TOKEN_PUNCTUATOR) continue;
            if (bytes_equal(expression.source, &token, "{")) depth++;
            else if (bytes_equal(expression.source, &token, "}")) depth--;
        }
        at = expression.offset;
    }
    return false;
}

static void lexer_update_expression_state(JsLexer *lexer,
                                          const JsToken *token)
{
    if (token->kind == JS_TOKEN_IDENTIFIER) {
        lexer->expression_expected = keyword_expects_expression(
            lexer->source, token);
        return;
    }
    if (token->kind == JS_TOKEN_NUMBER || token->kind == JS_TOKEN_STRING
        || token->kind == JS_TOKEN_TEMPLATE || token->kind == JS_TOKEN_REGEX) {
        lexer->expression_expected = false;
        return;
    }
    if (token->kind != JS_TOKEN_PUNCTUATOR) return;
    if (bytes_equal(lexer->source, token, ")")
        || bytes_equal(lexer->source, token, "]")
        || bytes_equal(lexer->source, token, "}")
        || bytes_equal(lexer->source, token, "++")
        || bytes_equal(lexer->source, token, "--")) {
        lexer->expression_expected = false;
    } else {
        lexer->expression_expected = true;
    }
}

static bool lexer_skip_space_and_comments(JsLexer *lexer)
{
    while (lexer->offset < lexer->length) {
        unsigned char byte = (unsigned char) lexer->source[lexer->offset];
        if (isspace(byte) != 0) {
            lexer->offset++;
            continue;
        }
        if (byte != '/' || lexer->offset + 1 >= lexer->length) return true;
        unsigned char next = (unsigned char) lexer->source[lexer->offset + 1];
        if (next == '/') {
            lexer->offset += 2;
            while (lexer->offset < lexer->length
                   && lexer->source[lexer->offset] != '\r'
                   && lexer->source[lexer->offset] != '\n') lexer->offset++;
            continue;
        }
        if (next != '*') return true;
        lexer->offset += 2;
        bool closed = false;
        while (lexer->offset + 1 < lexer->length) {
            if (lexer->source[lexer->offset] == '*'
                && lexer->source[lexer->offset + 1] == '/') {
                lexer->offset += 2;
                closed = true;
                break;
            }
            lexer->offset++;
        }
        if (!closed) {
            lexer->failed = true;
            return false;
        }
    }
    return true;
}

static bool lexer_next(JsLexer *lexer, JsToken *token)
{
    memset(token, 0, sizeof(*token));
    if (lexer == NULL || lexer->failed || lexer->work_remaining == NULL
        || *lexer->work_remaining == 0
        || !lexer_skip_space_and_comments(lexer)) return false;
    (*lexer->work_remaining)--;
    token->begin = lexer->offset;
    if (lexer->offset == lexer->length) {
        token->kind = JS_TOKEN_EOF;
        token->end = lexer->offset;
        return true;
    }
    unsigned char byte = (unsigned char) lexer->source[lexer->offset];
    size_t end = lexer->offset + 1;
    if (identifier_start(byte)) {
        while (end < lexer->length
               && identifier_continue(
                   (unsigned char) lexer->source[end])) end++;
        token->kind = JS_TOKEN_IDENTIFIER;
    } else if (isdigit(byte) != 0
               || (byte == '.' && end < lexer->length
                   && isdigit((unsigned char) lexer->source[end]) != 0)) {
        while (end < lexer->length) {
            unsigned char part = (unsigned char) lexer->source[end];
            if (!isalnum(part) && part != '.' && part != '_') break;
            end++;
        }
        token->kind = JS_TOKEN_NUMBER;
    } else if (byte == '\'' || byte == '"') {
        if (!skip_quoted(lexer, byte, &end)) {
            lexer->failed = true;
            token->kind = JS_TOKEN_INVALID;
            return false;
        }
        token->kind = JS_TOKEN_STRING;
    } else if (byte == '`') {
        if (!skip_template(lexer, &end)) {
            lexer->failed = true;
            token->kind = JS_TOKEN_INVALID;
            return false;
        }
        token->kind = JS_TOKEN_TEMPLATE;
    } else if (byte == '/' && lexer->expression_expected
               && (end >= lexer->length
                   || (lexer->source[end] != '/'
                       && lexer->source[end] != '*'))) {
        if (!skip_regular_expression(lexer, &end)) {
            lexer->failed = true;
            token->kind = JS_TOKEN_INVALID;
            return false;
        }
        token->kind = JS_TOKEN_REGEX;
    } else {
        static const char *const operators[] = {
            ">>>=", "**=", "&&=", "||=", "\x3f\x3f=", "===", "!==", ">>>",
            "<<=", ">>=", "=>", "==", "!=", "<=", ">=", "++", "--",
            "&&", "||", "??", "**", "<<", ">>", "+=", "-=", "*=",
            "/=", "%=", "&=", "|=", "^=", "?.", "..."
        };
        size_t best = 1;
        for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); i++) {
            size_t length = strlen(operators[i]);
            if (length > best && lexer->offset <= lexer->length - length
                && memcmp(lexer->source + lexer->offset,
                          operators[i], length) == 0) best = length;
        }
        end = lexer->offset + best;
        token->kind = JS_TOKEN_PUNCTUATOR;
    }
    token->end = end;
    lexer->offset = end;
    lexer_update_expression_state(lexer, token);
    return true;
}

static bool expect_token(JsLexer *lexer, const char *text, JsToken *result)
{
    JsToken token;
    if (!lexer_next(lexer, &token) || !bytes_equal(lexer->source, &token, text)) {
        return false;
    }
    if (result != NULL) *result = token;
    return true;
}

static bool append_resource_loader_statement(
    ScriptResourceLoaderPlan *plan, size_t offset, size_t length)
{
    if (plan == NULL || length == 0
        || plan->statement_count >= SCRIPT_LAZY_FACTORY_LIMIT
        || length > SIZE_MAX - plan->statement_source_bytes) return false;
    if (plan->statement_count == plan->statement_capacity) {
        size_t capacity = plan->statement_capacity == 0
            ? 32 : plan->statement_capacity * 2;
        if (capacity > SCRIPT_LAZY_FACTORY_LIMIT) {
            capacity = SCRIPT_LAZY_FACTORY_LIMIT;
        }
        if (capacity > SIZE_MAX / sizeof(*plan->statements)) return false;
        ScriptResourceLoaderStatement *grown = js_realloc(
            plan->budget, plan->statements,
            capacity * sizeof(*plan->statements));
        if (grown == NULL) return false;
        plan->statements = grown;
        plan->statement_capacity = capacity;
    }
    plan->statements[plan->statement_count++] =
        (ScriptResourceLoaderStatement) {
            .source_offset = offset,
            .source_length = length
        };
    plan->statement_source_bytes += length;
    if (length > plan->largest_statement_bytes) {
        plan->largest_statement_bytes = length;
    }
    return true;
}

bool script_resource_loader_plan_create(
    Budget *budget, const char *source, size_t source_length,
    ScriptResourceLoaderPlan *plan)
{
    if (plan != NULL) memset(plan, 0, sizeof(*plan));
    static const char signature[] = "mw.loader.impl";
    if (budget == NULL || source == NULL || source_length == 0
        || source_length > SCRIPT_LAZY_SCAN_BYTE_LIMIT || plan == NULL
        || memchr(source, '\0', source_length) != NULL) return false;
    size_t prefix = source_length < SCRIPT_LAZY_SIGNATURE_SCAN_LIMIT
        ? source_length : SCRIPT_LAZY_SIGNATURE_SCAN_LIMIT;
    bool has_signature = false;
    for (size_t at = 0; at + sizeof(signature) - 1 <= prefix; at++) {
        if (source[at] == signature[0]
            && memcmp(source + at, signature, sizeof(signature) - 1) == 0) {
            has_signature = true;
            break;
        }
    }
    if (!has_signature) return false;

    plan->budget = budget;
    size_t work_remaining = source_length + 64u;
    JsLexer lexer = {
        .source = source,
        .length = source_length,
        .work_remaining = &work_remaining,
        .expression_expected = true
    };
    for (;;) {
        JsToken token;
        if (!lexer_next(&lexer, &token)) goto reject;
        if (token.kind == JS_TOKEN_EOF) break;
        size_t statement_begin = token.begin;
        if (!bytes_equal(source, &token, "mw")
            || !expect_token(&lexer, ".", NULL)
            || !expect_token(&lexer, "loader", NULL)
            || !expect_token(&lexer, ".", NULL)
            || !expect_token(&lexer, "impl", NULL)
            || !expect_token(&lexer, "(", NULL)) goto reject;
        size_t round = 1, square = 0, curly = 0;
        JsToken end = {0};
        while (round != 0) {
            if (!lexer_next(&lexer, &end)
                || end.kind == JS_TOKEN_EOF
                || end.kind == JS_TOKEN_INVALID) goto reject;
            if (end.kind != JS_TOKEN_PUNCTUATOR) continue;
            if (bytes_equal(source, &end, "(")) round++;
            else if (bytes_equal(source, &end, ")")) {
                if (round == 0) goto reject;
                round--;
            } else if (bytes_equal(source, &end, "[")) square++;
            else if (bytes_equal(source, &end, "]")) {
                if (square == 0) goto reject;
                square--;
            } else if (bytes_equal(source, &end, "{")) curly++;
            else if (bytes_equal(source, &end, "}")) {
                if (curly == 0) goto reject;
                curly--;
            }
        }
        if (square != 0 || curly != 0) goto reject;
        size_t statement_end = end.end;
        size_t saved_offset = lexer.offset;
        JsToken separator;
        if (!lexer_next(&lexer, &separator)) goto reject;
        if (bytes_equal(source, &separator, ";")) {
            statement_end = separator.end;
        } else {
            lexer.offset = saved_offset;
        }
        if (!append_resource_loader_statement(
                plan, statement_begin, statement_end - statement_begin)) {
            goto reject;
        }
    }
    if (plan->statement_count == 0) goto reject;
    return true;

reject:
    script_resource_loader_plan_destroy(plan);
    return false;
}

void script_resource_loader_plan_destroy(ScriptResourceLoaderPlan *plan)
{
    if (plan == NULL) return;
    budget_free(plan->budget, plan->statements);
    memset(plan, 0, sizeof(*plan));
}

static bool recognized_root(const char *source, const JsToken *token)
{
    return token->kind == JS_TOKEN_IDENTIFIER
        && (bytes_equal(source, token, "self")
            || bytes_equal(source, token, "window")
            || bytes_equal(source, token, "globalThis"));
}

static bool recognized_chunk_name(const char *source, const JsToken *token)
{
    static const char prefix[] = "webpackChunk";
    size_t length = token->end - token->begin;
    return token->kind == JS_TOKEN_IDENTIFIER
        && length >= sizeof(prefix) - 1
        && memcmp(source + token->begin, prefix, sizeof(prefix) - 1) == 0;
}

static bool append_factory(ScriptLazyWebpackPlan *plan,
                           const ScriptLazyFactory *factory)
{
    if (plan->factory_count >= SCRIPT_LAZY_FACTORY_LIMIT) return false;
    if (plan->factory_count == plan->factory_capacity) {
        size_t capacity = plan->factory_capacity == 0
            ? 64 : plan->factory_capacity * 2;
        if (capacity > SCRIPT_LAZY_FACTORY_LIMIT) {
            capacity = SCRIPT_LAZY_FACTORY_LIMIT;
        }
        if (capacity > SIZE_MAX / sizeof(*plan->factories)) return false;
        ScriptLazyFactory *grown = js_realloc(
            plan->budget, plan->factories,
            capacity * sizeof(*plan->factories));
        if (grown == NULL) return false;
        plan->factories = grown;
        plan->factory_capacity = capacity;
    }
    if (factory->source_length > SIZE_MAX - plan->factory_source_bytes) {
        return false;
    }
    plan->factories[plan->factory_count++] = *factory;
    plan->factory_source_bytes += factory->source_length;
    if (factory->source_length > plan->largest_factory_bytes) {
        plan->largest_factory_bytes = factory->source_length;
    }
    return true;
}

static bool parse_parameters(JsLexer *lexer, bool parenthesized,
                             uint8_t *arity)
{
    size_t count = 0;
    if (parenthesized) {
        JsToken token;
        if (!lexer_next(lexer, &token)) return false;
        if (bytes_equal(lexer->source, &token, ")")) {
            *arity = 0;
            return true;
        }
        for (;;) {
            if (token.kind != JS_TOKEN_IDENTIFIER || count >= 3) return false;
            count++;
            if (!lexer_next(lexer, &token)) return false;
            if (bytes_equal(lexer->source, &token, ")")) break;
            if (!bytes_equal(lexer->source, &token, ",")
                || !lexer_next(lexer, &token)) return false;
        }
    } else {
        count = 1;
    }
    *arity = (uint8_t) count;
    return true;
}

static bool parse_factory(JsLexer *lexer, const JsToken *first,
                          ScriptLazyFactory *factory)
{
    memset(factory, 0, sizeof(*factory));
    factory->source_offset = first->begin;
    JsToken token;
    if (bytes_equal(lexer->source, first, "function")) {
        factory->kind = SCRIPT_LAZY_FACTORY_FUNCTION;
        if (!expect_token(lexer, "(", NULL)
            || !parse_parameters(lexer, true, &factory->arity)
            || !expect_token(lexer, "{", &token)) return false;
    } else {
        factory->kind = SCRIPT_LAZY_FACTORY_ARROW;
        if (bytes_equal(lexer->source, first, "(")) {
            if (!parse_parameters(lexer, true, &factory->arity)) return false;
        } else if (first->kind == JS_TOKEN_IDENTIFIER) {
            factory->arity = 1;
        } else {
            return false;
        }
        if (!expect_token(lexer, "=>", NULL)
            || !expect_token(lexer, "{", &token)) return false;
    }
    size_t braces = 1;
    while (braces != 0) {
        if (!lexer_next(lexer, &token) || token.kind == JS_TOKEN_EOF) {
            return false;
        }
        if (token.kind != JS_TOKEN_PUNCTUATOR) continue;
        if (bytes_equal(lexer->source, &token, "{")) braces++;
        else if (bytes_equal(lexer->source, &token, "}")) braces--;
    }
    factory->source_length = token.end - factory->source_offset;
    return factory->source_length != 0;
}

static bool parse_chunk_ids(JsLexer *lexer)
{
    bool saw_id = false;
    bool expect_id = true;
    for (;;) {
        JsToken token;
        if (!lexer_next(lexer, &token)) return false;
        if (bytes_equal(lexer->source, &token, "]")) {
            return saw_id && !expect_id;
        }
        if (expect_id) {
            if (token.kind != JS_TOKEN_NUMBER && token.kind != JS_TOKEN_STRING) {
                return false;
            }
            saw_id = true;
            expect_id = false;
        } else {
            if (!bytes_equal(lexer->source, &token, ",")) return false;
            expect_id = true;
        }
    }
}

static bool parse_factory_object(JsLexer *lexer,
                                 ScriptLazyWebpackPlan *plan)
{
    bool saw_factory = false;
    for (;;) {
        JsToken key;
        if (!lexer_next(lexer, &key)) return false;
        if (bytes_equal(lexer->source, &key, "}")) return saw_factory;
        if (key.kind != JS_TOKEN_NUMBER
            && key.kind != JS_TOKEN_IDENTIFIER) return false;
        if (!expect_token(lexer, ":", NULL)) return false;
        JsToken first;
        if (!lexer_next(lexer, &first)) return false;
        ScriptLazyFactory factory;
        if (!parse_factory(lexer, &first, &factory)
            || !append_factory(plan, &factory)) return false;
        plan->factories[plan->factory_count - 1].key_offset = key.begin;
        plan->factories[plan->factory_count - 1].key_length =
            key.end - key.begin;
        saw_factory = true;
        JsToken separator;
        if (!lexer_next(lexer, &separator)) return false;
        if (bytes_equal(lexer->source, &separator, "}")) return true;
        if (!bytes_equal(lexer->source, &separator, ",")) return false;
    }
}

static bool parse_optional_runtime_and_tail(JsLexer *lexer)
{
    JsToken token;
    if (!lexer_next(lexer, &token)) return false;
    if (!bytes_equal(lexer->source, &token, "]")) {
        if (!bytes_equal(lexer->source, &token, ",")) return false;
        size_t round = 0, square = 0, curly = 0;
        bool saw_expression = false;
        for (;;) {
            if (!lexer_next(lexer, &token) || token.kind == JS_TOKEN_EOF) {
                return false;
            }
            if (token.kind != JS_TOKEN_PUNCTUATOR) {
                saw_expression = true;
                continue;
            }
            if (bytes_equal(lexer->source, &token, "(") ) round++;
            else if (bytes_equal(lexer->source, &token, "[")) square++;
            else if (bytes_equal(lexer->source, &token, "{")) curly++;
            else if (bytes_equal(lexer->source, &token, ")")) {
                if (round == 0) return false;
                round--;
            } else if (bytes_equal(lexer->source, &token, "}")) {
                if (curly == 0) return false;
                curly--;
            } else if (bytes_equal(lexer->source, &token, "]")) {
                if (square != 0) square--;
                else if (round == 0 && curly == 0) break;
                else return false;
            }
            saw_expression = true;
        }
        if (!saw_expression) return false;
    }
    if (!expect_token(lexer, ")", NULL)) return false;
    if (!lexer_next(lexer, &token)) return false;
    if (bytes_equal(lexer->source, &token, ";")) {
        if (!lexer_next(lexer, &token)) return false;
    }
    return token.kind == JS_TOKEN_EOF;
}

bool script_lazy_webpack_plan_create(Budget *budget, const char *source,
                                     size_t source_length,
                                     ScriptLazyWebpackPlan *plan)
{
    if (plan != NULL) memset(plan, 0, sizeof(*plan));
    if (budget == NULL || source == NULL || source_length == 0
        || source_length > SCRIPT_LAZY_SCAN_BYTE_LIMIT || plan == NULL
        || !has_bounded_webpack_signature(source, source_length)
        || memchr(source, '\0', source_length) != NULL) {
        return false;
    }
    plan->budget = budget;
    /* One token always consumes at least one previously unconsumed source
       byte.  Nested template-expression lexers share this counter, keeping
       adversarial nesting within a single linear token-work allowance. */
    size_t work_remaining = source_length + 64u;
    JsLexer lexer = {
        .source = source,
        .length = source_length,
        .work_remaining = &work_remaining,
        .expression_expected = true
    };
    JsToken token;
    if (!lexer_next(&lexer, &token)) goto reject;
    while (token.kind == JS_TOKEN_STRING) {
        if (bytes_equal(source, &token, "\"use strict\"")
            || bytes_equal(source, &token, "'use strict'")) {
            plan->strict_mode = true;
        }
        if (!expect_token(&lexer, ";", NULL)
            || !lexer_next(&lexer, &token)) goto reject;
    }
    if (!bytes_equal(source, &token, "(")) goto reject;
    JsToken first_root, first_name, second_root, second_name;
    if (!lexer_next(&lexer, &first_root) || !recognized_root(source, &first_root)
        || !expect_token(&lexer, ".", NULL)
        || !lexer_next(&lexer, &first_name)
        || !recognized_chunk_name(source, &first_name)
        || !expect_token(&lexer, "=", NULL)
        || !lexer_next(&lexer, &second_root)
        || !recognized_root(source, &second_root)
        || !expect_token(&lexer, ".", NULL)
        || !lexer_next(&lexer, &second_name)
        || !recognized_chunk_name(source, &second_name)) goto reject;
    if (first_root.end - first_root.begin != second_root.end - second_root.begin
        || memcmp(source + first_root.begin, source + second_root.begin,
                  first_root.end - first_root.begin) != 0
        || first_name.end - first_name.begin != second_name.end - second_name.begin
        || memcmp(source + first_name.begin, source + second_name.begin,
                  first_name.end - first_name.begin) != 0) goto reject;
    if (!expect_token(&lexer, "||", NULL)
        || !expect_token(&lexer, "[", NULL)
        || !expect_token(&lexer, "]", NULL)
        || !expect_token(&lexer, ")", NULL)
        || !expect_token(&lexer, ".", NULL)
        || !expect_token(&lexer, "push", NULL)
        || !expect_token(&lexer, "(", NULL)
        || !expect_token(&lexer, "[", NULL)
        || !expect_token(&lexer, "[", NULL)
        || !parse_chunk_ids(&lexer)
        || !expect_token(&lexer, ",", NULL)
        || !expect_token(&lexer, "{", NULL)
        || !parse_factory_object(&lexer, plan)
        || !parse_optional_runtime_and_tail(&lexer)) goto reject;
    return true;

reject:
    script_lazy_webpack_plan_destroy(plan);
    return false;
}

static size_t wrapper_length(const ScriptLazyFactory *factory,
                             uint32_t bundle_id, size_t factory_index)
{
    static const char *const parameters[] = {"", "$a", "$a,$b", "$a,$b,$c"};
    int result;
    if (factory->kind == SCRIPT_LAZY_FACTORY_ARROW) {
        result = snprintf(NULL, 0, "__tilefinchLazyWebpackWrap(%u,%zu)",
                          bundle_id, factory_index);
    } else {
        result = snprintf(
            NULL, 0,
            "__tilefinchLazyWebpackWrap(%u,%zu,function(%s){return "
            "__tilefinchLazyWebpackInvoke(%u,%zu,this,arguments)})",
            bundle_id, factory_index, parameters[factory->arity],
            bundle_id, factory_index);
    }
    return result < 0 ? SIZE_MAX : (size_t) result;
}

static bool render_wrapper(char *output, size_t capacity,
                           const ScriptLazyFactory *factory,
                           uint32_t bundle_id, size_t factory_index,
                           size_t *written)
{
    static const char *const parameters[] = {"", "$a", "$a,$b", "$a,$b,$c"};
    int result;
    if (factory->kind == SCRIPT_LAZY_FACTORY_ARROW) {
        result = snprintf(output, capacity,
                          "__tilefinchLazyWebpackWrap(%u,%zu)",
                          bundle_id, factory_index);
    } else {
        result = snprintf(
            output, capacity,
            "__tilefinchLazyWebpackWrap(%u,%zu,function(%s){return "
            "__tilefinchLazyWebpackInvoke(%u,%zu,this,arguments)})",
            bundle_id, factory_index, parameters[factory->arity],
            bundle_id, factory_index);
    }
    if (result < 0 || (size_t) result >= capacity) return false;
    *written = (size_t) result;
    return true;
}

bool script_lazy_webpack_render(Budget *budget, const char *source,
                                size_t source_length,
                                const ScriptLazyWebpackPlan *plan,
                                uint32_t bundle_id, char **output,
                                size_t *output_length)
{
    if (output != NULL) *output = NULL;
    if (output_length != NULL) *output_length = 0;
    if (budget == NULL || source == NULL || plan == NULL
        || plan->factory_count == 0 || output == NULL
        || output_length == NULL) return false;
    size_t length = source_length;
    size_t previous_end = 0;
    for (size_t i = 0; i < plan->factory_count; i++) {
        const ScriptLazyFactory *factory = &plan->factories[i];
        if (factory->source_offset < previous_end
            || factory->source_offset > source_length
            || factory->source_length > source_length - factory->source_offset
            || factory->arity > 3) return false;
        size_t replacement = wrapper_length(factory, bundle_id, i);
        if (length < factory->source_length
            || replacement > SIZE_MAX - (length - factory->source_length)) {
            return false;
        }
        length = length - factory->source_length + replacement;
        previous_end = factory->source_offset + factory->source_length;
    }
    if (length == SIZE_MAX) return false;
    char *rendered = js_malloc(budget, length + 1);
    if (rendered == NULL) return false;
    size_t input_at = 0, output_at = 0;
    for (size_t i = 0; i < plan->factory_count; i++) {
        const ScriptLazyFactory *factory = &plan->factories[i];
        size_t prefix = factory->source_offset - input_at;
        memcpy(rendered + output_at, source + input_at, prefix);
        output_at += prefix;
        size_t added = 0;
        if (!render_wrapper(rendered + output_at, length + 1 - output_at,
                            factory, bundle_id, i, &added)) {
            budget_free(budget, rendered);
            return false;
        }
        output_at += added;
        input_at = factory->source_offset + factory->source_length;
    }
    memcpy(rendered + output_at, source + input_at, source_length - input_at);
    output_at += source_length - input_at;
    if (output_at != length) {
        budget_free(budget, rendered);
        return false;
    }
    rendered[length] = '\0';
    *output = rendered;
    *output_length = length;
    return true;
}

void script_lazy_webpack_plan_destroy(ScriptLazyWebpackPlan *plan)
{
    if (plan == NULL) return;
    budget_free(plan->budget, plan->factories);
    memset(plan, 0, sizeof(*plan));
}
