#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/url.h"

#include <errno.h>
#include <ctype.h>
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
            "usage: %s --method GET|HEAD|OPTIONS --url HTTPS_URL --output DIR "
            "--max-bytes N --timeout-ms N [--origin HTTPS_ORIGIN] "
            "[--headers-file FILE | --preflight-method GET|HEAD "
            "--preflight-headers NAMES]\n",
            program);
}

static bool read_public_headers(const char *path, char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    size_t length = fread(buffer, 1, capacity, file);
    bool valid = !ferror(file) && length < capacity;
    fclose(file);
    if (!valid) return false;
    if (length != 0 && buffer[length - 1] == '\n') length--;
    buffer[length] = '\0';
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char) buffer[i];
        if (c != '\n' && c != '\t' && (c < 32 || c > 126)) return false;
    }
    for (const char *at = buffer; *at != '\0';) {
        const char *end = strchr(at, '\n');
        if (end == NULL) end = at + strlen(at);
        const char *colon = memchr(at, ':', (size_t) (end - at));
        char name[128];
        if (colon == NULL || colon == at || (size_t) (colon - at) >= sizeof(name)) return false;
        size_t n = (size_t) (colon - at);
        for (size_t i = 0; i < n; i++) name[i] = (char) tolower((unsigned char) at[i]);
        name[n] = '\0';
        if (strstr(name, "authorization") || strstr(name, "cookie")
            || strstr(name, "token") || strstr(name, "secret")
            || strstr(name, "credential") || strstr(name, "api-key")
            || strstr(name, "api_key") || strstr(name, "apikey")) return false;
        at = *end == '\0' ? end : end + 1;
    }
    return true;
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
    const char *origin = NULL;
    const char *headers_file = NULL, *preflight_method = NULL, *preflight_headers = NULL;
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
        } else if (strcmp(option, "--origin") == 0 && origin == NULL) {
            origin = value;
        } else if (strcmp(option, "--headers-file") == 0 && headers_file == NULL) {
            headers_file = value;
        } else if (strcmp(option, "--preflight-method") == 0 && preflight_method == NULL) {
            preflight_method = value;
        } else if (strcmp(option, "--preflight-headers") == 0 && preflight_headers == NULL) {
            preflight_headers = value;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    size_t maximum_bytes = 0;
    long timeout_ms = 0;
    char normalized_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    bool preflight = method != NULL && strcmp(method, "OPTIONS") == 0;
    char extra_headers[8192] = {0};
    if (method == NULL || url == NULL || output == NULL
        || max_bytes_text == NULL || timeout_text == NULL
        || (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 && !preflight)
        || (preflight && (origin == NULL || headers_file != NULL || preflight_method == NULL
            || (strcmp(preflight_method, "GET") != 0 && strcmp(preflight_method, "HEAD") != 0)
            || preflight_headers == NULL || strlen(preflight_headers) > 1024
            || strpbrk(preflight_headers, "\r\n") != NULL))
        || (!preflight && (preflight_method != NULL || preflight_headers != NULL))
        || !exact_https_url(url)
        || (origin != NULL
            && (!tilefinch_url_origin(origin, normalized_origin,
                                     sizeof(normalized_origin))
                || strncmp(origin, "https://", 8) != 0
                || strcmp(origin, normalized_origin) != 0))
        || !parse_size(max_bytes_text, ACQUIRE_MAX_RESPONSE_BYTES,
                       &maximum_bytes)
        || !parse_timeout(timeout_text, &timeout_ms)
        || !output_does_not_exist(output)) {
        usage(argv[0]);
        return 2;
    }
    if (preflight) {
        int length = snprintf(extra_headers, sizeof(extra_headers),
            "Access-Control-Request-Method: %s%s%s", preflight_method,
            preflight_headers[0] ? "\nAccess-Control-Request-Headers: " : "",
            preflight_headers);
        if (length < 0 || (size_t) length >= sizeof(extra_headers)) return 2;
    } else if (headers_file != NULL
               && !read_public_headers(headers_file, extra_headers, sizeof(extra_headers))) {
        fprintf(stderr, "trace acquisition refuses invalid or credential-bearing headers\n");
        return 2;
    }
    FetchRequest request = {
        .method = method, .origin = origin, .allow_http_errors = true,
        .extra_headers = extra_headers[0] ? extra_headers : NULL,
        .cors_preflight = preflight, .enforce_cors = preflight,
        .redirect_same_origin_only = preflight,
        .credential_origin = preflight ? origin : NULL,
        .initiator_url = preflight ? origin : NULL,
        .credentials = preflight ? FETCH_CREDENTIALS_SAME_ORIGIN : FETCH_CREDENTIALS_INCLUDE
    };
    FetchRequestValidationError validation;
    if (!fetch_request_validate(&request, &validation)) {
        fprintf(stderr, "trace acquisition refuses invalid request policy\n");
        return 2;
    }
    /* Raw response-cookie values are forbidden even if a caller tries to
       opt into the general trace capture escape hatch. No Cookie, cookie jar,
       Authorization or client certificate is supplied. Application headers
       require an explicit bounded file and cannot carry credentials.
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

    FetchResult result = {.budget = &budget};
    bool fetched = fetch_request_single_hop(
        &budget, url, &request, maximum_bytes, timeout_ms, &result);
    fetch_trace_end();
    bool valid = fetched && result.status_code >= 200
        && result.status_code <= 599
        && strcmp(result.effective_url, url) == 0
        && (strcmp(method, "HEAD") != 0 || result.length == 0);
    if (valid && preflight) {
        char metadata_path[4096];
        int length = snprintf(metadata_path, sizeof(metadata_path), "%s/0000.meta", output);
        FILE *metadata = length >= 0 && (size_t) length < sizeof(metadata_path)
            ? fopen(metadata_path, "ab") : NULL;
        valid = metadata != NULL;
        if (metadata != NULL) {
            valid = fprintf(metadata, "request-preflight-method=%s\nrequest-preflight-headers=%s\n",
                            preflight_method, preflight_headers) >= 0;
            if (fclose(metadata) != 0) valid = false;
        }
    }
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
