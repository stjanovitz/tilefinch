/* The bounded JSON scanner (tilefinch/json_scan.h): its grammar, its depth
   bound, its string decoding and truncation, member lookup, and hostile
   input (truncation at every byte, deep nesting, overlong keys). */
#include "tilefinch/json_scan.h"
#include "tilefinch/web_app_manifest.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
        return 1;                                                            \
    }                                                                        \
} while (0)

static JsonCursor cursor(const char *text)
{
    return (JsonCursor) {text, text + strlen(text)};
}

static bool skips(const char *text, unsigned depth)
{
    JsonCursor json = cursor(text);
    if (!json_scan_skip_value(&json, depth)) return false;
    json_scan_space(&json);
    return json.at == json.end;
}

static int test_grammar(void)
{
    static const char *const valid[] = {
        "0", "-0", "7", "-12", "1.5", "-0.25", "1e9", "1E+9", "2.5e-3",
        "true", "false", "null", "\"\"", "\"a\\\"b\\\\c\\/\\b\\f\\n\\r\\t\"",
        "\"\\u00e9\\ud83d\\ude00\"", "\"\\ud83d\"", "\"\\ud83d\\u0041\"",
        "\"\\udc00\\ud800\"", "[]", "{}", " [ 1 , \"x\" , null ] ",
        "{\"a\":{\"b\":[true,false]},\"c\":-1.0e2}", "\t\r\n{\"k\" :\t1}"
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (!skips(valid[i], 8u)) fprintf(stderr, "valid[%zu]\n", i);
        CHECK(skips(valid[i], 8u));
    }
    /* strtod and the old any-run scanners accepted most of these. */
    static const char *const invalid[] = {
        "", " ", "+1", "01", "-", "1.", ".5", "1e", "1e+", "0x10", "inf",
        "nan", "NaN", "Infinity", "tru", "truex", "nul", "nulls", "True",
        "1 2", "1:", "\"a", "\"\\x41\"", "\"\\u12\"", "\"\\uzzzz\"",
        "\"a\tb\"", "\"\\ud83d\\u12\"", "[1,]", "[,1]",
        "{\"a\"}", "{\"a\":}", "{a:1}", "{\"a\":1,}", "[1}", "{\"a\":1]",
        "[", "{", "]", "\v1", "1\f", "'a'", "\"\\'\""
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        if (skips(invalid[i], 8u)) fprintf(stderr, "invalid[%zu]\n", i);
        CHECK(!skips(invalid[i], 8u));
    }
    return 0;
}

static int test_depth(void)
{
    /* A value at remaining depth 0 is refused; containers pass depth - 1. */
    CHECK(!skips("1", 0u));
    CHECK(skips("1", 1u));
    CHECK(!skips("[]", 0u));
    CHECK(skips("[]", 1u));
    CHECK(!skips("[1]", 1u));
    CHECK(skips("[1]", 2u));
    CHECK(skips("[[[]]]", 3u));
    CHECK(!skips("[[[]]]", 2u));
    CHECK(skips("{\"a\":{\"b\":1}}", 3u));
    CHECK(!skips("{\"a\":{\"b\":1}}", 2u));
    /* Deep nesting costs no C stack and stops at the bound. */
    static char deep[20001];
    for (size_t i = 0; i < 10000u; i++) {
        deep[i] = '[';
        deep[19999u - i] = ']';
    }
    deep[20000] = '\0';
    CHECK(!skips(deep, 48u));
    CHECK(!skips(deep, 100000u));
    return 0;
}

static int test_strings(void)
{
    char out[16];
    JsonCursor json = cursor("  \"caf\\u00e9 \\ud83d\\ude00\" ,");
    CHECK(json_scan_string(&json, out, sizeof(out))
          && strcmp(out, "caf\xc3\xa9 \xf0\x9f\x98\x80") == 0
          && *json.at == ' ');
    /* A surrogate without its other half is valid JSON, shown as U+FFFD; an
       escape after a lone high surrogate is read on its own. */
    json = cursor("\"a\\udc00b\"");
    CHECK(json_scan_string(&json, out, sizeof(out))
          && strcmp(out, "a\xef\xbf\xbd" "b") == 0);
    json = cursor("\"a\\ud800b\"");
    CHECK(json_scan_string(&json, out, sizeof(out))
          && strcmp(out, "a\xef\xbf\xbd" "b") == 0);
    json = cursor("\"\\ud800\\u0041\\ud800\"");
    CHECK(json_scan_string(&json, out, sizeof(out))
          && strcmp(out, "\xef\xbf\xbd" "A\xef\xbf\xbd") == 0);
    /* Exact fit, one byte over, and truncation on a UTF-8 boundary. */
    char four[4];
    json = cursor("\"abc\"");
    CHECK(json_scan_string(&json, four, sizeof(four))
          && strcmp(four, "abc") == 0);
    json = cursor("\"abcd\"");
    CHECK(!json_scan_string(&json, four, sizeof(four)));
    bool truncated = false;
    json = cursor("\"ab\xc3\xa9z\" ");
    CHECK(json_scan_string_copy(&json, four, sizeof(four),
                                JSON_SCAN_TRUNCATE, &truncated)
          && truncated && strcmp(four, "ab") == 0 && *json.at == ' ');
    json = cursor("\"a\\u00e9\\u00e9\"");
    CHECK(json_scan_string_copy(&json, four, sizeof(four),
                                JSON_SCAN_TRUNCATE, &truncated)
          && truncated && strcmp(four, "a\xc3\xa9") == 0);
    /* Truncation still validates the rest of the string. */
    json = cursor("\"abcdef\\q\"");
    CHECK(!json_scan_string_copy(&json, four, sizeof(four),
                                 JSON_SCAN_TRUNCATE, &truncated));
    json = cursor("\"abc\"");
    CHECK(json_scan_string_copy(&json, four, sizeof(four),
                                JSON_SCAN_TRUNCATE, &truncated)
          && !truncated);
    CHECK(!json_scan_string(&json, NULL, 4u));
    json = cursor("\"x\"");
    CHECK(!json_scan_string(&json, out, 0u));
    return 0;
}

static int test_members(void)
{
    const char *text = "{\"long_key_" "0123456789012345678901234567890123456789"
        "0123456789\":1,\"skip\":{\"name\":\"inner\"},"
        "\"esc\\u0061ped\":[1,2],\"name\":\"outer\"} trailing";
    JsonCursor value;
    char out[16];
    CHECK(json_scan_member(cursor(text), "name", 8u, 16u, &value)
          && json_scan_string(&value, out, sizeof(out))
          && strcmp(out, "outer") == 0);
    CHECK(json_scan_member(cursor(text), "escaaped", 8u, 16u, &value) == false);
    CHECK(json_scan_member(cursor(text), "escaped", 8u, 16u, &value)
          && *value.at == '[');
    CHECK(!json_scan_member(cursor(text), "missing", 8u, 16u, &value));
    /* The member limit and the depth bound both stop the walk. */
    CHECK(!json_scan_member(cursor(text), "name", 8u, 3u, &value));
    CHECK(!json_scan_member(cursor(text), "name", 2u, 16u, &value));
    CHECK(!json_scan_member(cursor("[1]"), "name", 8u, 16u, &value));
    CHECK(!json_scan_member(cursor("{\"a\":1 \"name\":2}"), "name", 8u, 16u,
                            &value));

    JsonCursor span, json = cursor("  {\"a\":[1,{\"b\":null}]} ,");
    CHECK(json_scan_object_span(&json, 4u, &span)
          && (size_t) (span.end - span.at) == strlen("{\"a\":[1,{\"b\":null}]}")
          && *json.at == ' ');
    json = cursor("[1]");
    CHECK(!json_scan_object_span(&json, 4u, &span));
    /* A \u0000 escape cannot live in a C string: decoding refuses it, so
       a key holding one never matches (it used to be cut at the NUL and
       alias "a"), and a value holding one is not cut short either. */
    CHECK(json_scan_member(cursor("{\"a\\u0000b\":1,\"a\":2}"), "a", 8u,
                           16u, &value)
          && *value.at == '2');
    json = cursor("\"https://a\\u0000.evil/\"");
    CHECK(!json_scan_string(&json, out, sizeof(out)));
    json = cursor("\"\\u0000\"");
    CHECK(json_scan_skip_string(&json) && json.at == json.end);
    /* Duplicate keys: the first one wins (JSON.parse keeps the last). */
    CHECK(json_scan_member(cursor("{\"a\":1,\"a\":2}"), "a", 8u, 16u,
                           &value)
          && *value.at == '1');
    char key[8];
    json = cursor("\"k\" : 5");
    CHECK(json_scan_member_key(&json, key, sizeof(key))
          && strcmp(key, "k") == 0 && *json.at == ' ');
    return 0;
}

/* Every prefix of a valid document is refused without reading past its end
   (run under the sanitize preset this proves the bound); only the whole
   document is accepted. */
static int test_truncated_inputs(void)
{
    static const char doc[] =
        "{\"a\":[1,-2.5e3,true,false,null,\"x\\u00e9\\ud83d\\ude00\\n\"],"
        "\"b\":{\"c\":{}}}";
    size_t length = sizeof(doc) - 1u;
    for (size_t cut = 0; cut <= length; cut++) {
        char copy[sizeof(doc)];
        memcpy(copy, doc, cut);
        JsonCursor json = {copy, copy + cut};
        bool ok = json_scan_skip_value(&json, 8u);
        CHECK(ok == (cut == length));
        if (ok) CHECK(json.at == json.end);
        else CHECK(json.at <= json.end);
    }
    return 0;
}

/* The web app manifest parser on the shared scanner: scalars must be JSON,
   escapes decode to UTF-8, and unknown members are skipped to a depth. */
static bool manifest(const char *json, TilefinchWebAppManifest *out)
{
    char error[128];
    return tilefinch_web_app_manifest_parse(
        json, strlen(json), "https://app.test/manifest.json",
        "https://app.test/", out, error, sizeof(error));
}

static int test_manifest(void)
{
    TilefinchWebAppManifest parsed;
    CHECK(manifest("{\"name\":\"Caf\\u00e9 \\ud83c\\udfae\","
                   "\"extra\":[1,2.5,{\"x\":null}],\"display\":\"standalone\"}",
                   &parsed)
          && strcmp(parsed.name, "Caf\xc3\xa9 \xf0\x9f\x8e\xae") == 0
          && parsed.display_mode == TILEFINCH_WEB_APP_DISPLAY_STANDALONE);
    /* The old scanner took any run of bytes as a scalar. */
    CHECK(!manifest("{\"name\":\"A\",\"x\":tru}", &parsed));
    CHECK(!manifest("{\"name\":\"A\",\"x\":0x10}", &parsed));
    CHECK(!manifest("{\"name\":\"A\",\"icons\":[{\"src\":\"a.png\","
                    "\"purpose\":any}]}", &parsed));
    /* A lone surrogate is valid JSON: shown as U+FFFD, and an ignored
       member holding one does not reject the manifest. */
    CHECK(manifest("{\"name\":\"\\ud83c\"}", &parsed)
          && strcmp(parsed.name, "\xef\xbf\xbd") == 0);
    CHECK(manifest("{\"x\":\"\\ud800\",\"name\":\"A\"}", &parsed)
          && strcmp(parsed.name, "A") == 0);
    /* A start URL holding \u0000 is refused, not cut at the NUL. */
    CHECK(!manifest("{\"name\":\"A\","
                    "\"start_url\":\"/app\\u0000/../evil\"}", &parsed));
    char deep[64] = "{\"x\":";
    for (unsigned i = 0; i < 16u; i++) strcat(deep, "[");
    for (unsigned i = 0; i < 16u; i++) strcat(deep, "]");
    strcat(deep, "}");
    CHECK(manifest(deep, &parsed));
    char deeper[64] = "{\"x\":";
    for (unsigned i = 0; i < 17u; i++) strcat(deeper, "[");
    for (unsigned i = 0; i < 17u; i++) strcat(deeper, "]");
    strcat(deeper, "}");
    CHECK(!manifest(deeper, &parsed));
    return 0;
}

int main(void)
{
    CHECK(test_grammar() == 0);
    CHECK(test_depth() == 0);
    CHECK(test_strings() == 0);
    CHECK(test_members() == 0);
    CHECK(test_truncated_inputs() == 0);
    CHECK(test_manifest() == 0);
    puts("json scan tests passed");
    return 0;
}
