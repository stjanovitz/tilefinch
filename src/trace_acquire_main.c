#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/url.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MIB (1024u * 1024u)
#define ACQUIRE_MAX_RESPONSE_BYTES (64u * MIB)
#define ACQUIRE_MAX_TIMEOUT_MS 120000L
#define ACQUIRE_MAX_ORIGIN_MS 8640000000000000ULL

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --method GET|HEAD --url HTTPS_URL --output DIR "
            "--max-bytes N --timeout-ms N\n",
            program);
}

static bool parse_size(const char *value, size_t maximum, size_t *parsed)
{
    if (value == NULL || value[0] == '\0' || parsed == NULL) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long number = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || number == 0
        || number > maximum || number > SIZE_MAX) return false;
    *parsed = (size_t) number;
    return true;
}

static bool parse_timeout(const char *value, long *parsed)
{
    size_t number = 0;
    if (!parse_size(value, (size_t) ACQUIRE_MAX_TIMEOUT_MS, &number)
        || number > (size_t) LONG_MAX) return false;
    *parsed = (long) number;
    return true;
}

static bool valid_origin_ms(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    return errno == 0 && end != value && *end == '\0' && parsed != 0
        && parsed <= ACQUIRE_MAX_ORIGIN_MS;
}

static bool output_does_not_exist(const char *path)
{
    struct stat info;
    if (lstat(path, &info) == 0) return false;
    return errno == ENOENT;
}

static bool capture_authority_complete(const char *directory)
{
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/trace.meta", directory);
    if (written <= 0 || (size_t) written >= sizeof(path)) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    char line[256];
    bool clock = false, origin = false, complete = false, count = false;
    bool valid = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
            valid = false;
            break;
        }
        while (length != 0
               && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (strcmp(line, "psp-http-trace-clock=1") == 0) {
            if (clock) valid = false;
            clock = true;
        } else if (strncmp(line, "origin-ms=", 10) == 0) {
            if (origin || !valid_origin_ms(line + 10)) {
                valid = false;
            }
            origin = true;
        } else if (strcmp(line, "capture-complete=yes") == 0) {
            if (complete) valid = false;
            complete = true;
        } else if (strcmp(line, "record-count=1") == 0) {
            if (count) valid = false;
            count = true;
        } else if (strncmp(line, "record-count=", 13) == 0) {
            valid = false;
        }
    }
    bool read_failed = ferror(file) != 0;
    if (fclose(file) != 0 || read_failed) valid = false;
    static const char *required[] = {"0000.meta", "0000.body"};
    for (size_t i = 0; valid && i < sizeof(required) / sizeof(required[0]); i++) {
        struct stat info;
        written = snprintf(
            path, sizeof(path), "%s/%s", directory, required[i]);
        valid = written > 0 && (size_t) written < sizeof(path)
            && stat(path, &info) == 0 && S_ISREG(info.st_mode);
    }
    struct stat overflow;
    written = snprintf(path, sizeof(path), "%s/0001.meta", directory);
    if (valid && written > 0 && (size_t) written < sizeof(path)
        && stat(path, &overflow) == 0) valid = false;
    return valid && clock && origin && complete && count;
}

static bool exact_https_url(const char *value)
{
    TilefinchUrl parsed;
    char normalized[TILEFINCH_URL_SERIALIZED_LIMIT];
    return tilefinch_url_parse(value, &parsed)
        && parsed.scheme == TILEFINCH_URL_SCHEME_HTTPS
        && !parsed.has_fragment
        && tilefinch_url_normalize(value, normalized, sizeof(normalized))
        && strcmp(value, normalized) == 0;
}

int main(int argc, char **argv)
{
    const char *method = NULL;
    const char *url = NULL;
    const char *output = NULL;
    const char *max_bytes_text = NULL;
    const char *timeout_text = NULL;
    for (int i = 1; i < argc; i++) {
        const char *option = argv[i];
        if (strcmp(option, "--help") == 0
            || strcmp(option, "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        const char *value = argv[++i];
        if (strcmp(option, "--method") == 0 && method == NULL) {
            method = value;
        } else if (strcmp(option, "--url") == 0 && url == NULL) {
            url = value;
        } else if (strcmp(option, "--output") == 0 && output == NULL) {
            output = value;
        } else if (strcmp(option, "--max-bytes") == 0
                   && max_bytes_text == NULL) {
            max_bytes_text = value;
        } else if (strcmp(option, "--timeout-ms") == 0
                   && timeout_text == NULL) {
            timeout_text = value;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    size_t maximum_bytes = 0;
    long timeout_ms = 0;
    if (method == NULL || url == NULL || output == NULL
        || max_bytes_text == NULL || timeout_text == NULL
        || (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
        || !exact_https_url(url)
        || !parse_size(max_bytes_text, ACQUIRE_MAX_RESPONSE_BYTES,
                       &maximum_bytes)
        || !parse_timeout(timeout_text, &timeout_ms)
        || !output_does_not_exist(output)) {
        usage(argv[0]);
        return 2;
    }
    /* Raw response-cookie values are forbidden even if a caller tries to
       opt into the general trace capture escape hatch. No Cookie, cookie jar,
       Authorization, client certificate, or application header is supplied.
       INCLUDE is intentional only so response Set-Cookie fields survive long
       enough for the trace writer to redact and retain their replay shape. */
    if (getenv("TILEFINCH_TRACE_RAW_COOKIES") != NULL) {
        fprintf(stderr,
                "trace acquisition refuses TILEFINCH_TRACE_RAW_COOKIES\n");
        return 2;
    }
    if (maximum_bytes > SIZE_MAX - 16u * MIB) {
        fprintf(stderr, "trace acquisition budget overflow\n");
        return 2;
    }

    Budget budget;
    budget_init(&budget, maximum_bytes + 16u * MIB);
    char trace_error[256] = {0};
    if (!fetch_trace_capture_begin(
            output, trace_error, sizeof(trace_error))) {
        fprintf(stderr, "trace acquisition could not start: %s\n",
                trace_error[0] == '\0' ? "unknown error" : trace_error);
        return 1;
    }

    FetchRequest request = {
        .method = method,
        .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_INCLUDE
    };
    FetchResult result = {.budget = &budget};
    bool fetched = fetch_request_single_hop(
        &budget, url, &request, maximum_bytes, timeout_ms, &result);
    fetch_trace_end();
    bool valid = fetched && result.status_code >= 200
        && result.status_code <= 599
        && strcmp(result.effective_url, url) == 0
        && (strcmp(method, "HEAD") != 0 || result.length == 0);
    if (valid) valid = capture_authority_complete(output);
    if (valid) {
        printf("trace-acquire-ok status=%ld length=%zu\n",
               result.status_code, result.length);
    } else {
        fprintf(stderr, "trace acquisition failed: %s\n",
                result.error[0] == '\0'
                    ? "non-terminal or invalid response" : result.error);
    }
    fetch_result_destroy(&result);
    if (budget.current != 0) {
        fprintf(stderr, "trace acquisition leaked %zu budget bytes\n",
                budget.current);
        return 1;
    }
    return valid ? 0 : 1;
}
