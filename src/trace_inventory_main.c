#include "tilefinch/fetch.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --response-keyed DIR --expect-records N\n",
            program);
}

static bool parse_count(const char *value, size_t *count)
{
    if (value == NULL || value[0] == '\0' || count == NULL) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0
        || parsed > SIZE_MAX) return false;
    *count = (size_t) parsed;
    return true;
}

int main(int argc, char **argv)
{
    const char *directory = NULL;
    const char *count_text = NULL;
    for (int index = 1; index < argc; index++) {
        const char *option = argv[index];
        if (strcmp(option, "--help") == 0
            || strcmp(option, "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (index + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        const char *value = argv[++index];
        if (strcmp(option, "--response-keyed") == 0
            && directory == NULL) {
            directory = value;
        } else if (strcmp(option, "--expect-records") == 0
                   && count_text == NULL) {
            count_text = value;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    size_t expected = 0;
    if (directory == NULL || !parse_count(count_text, &expected)) {
        usage(argv[0]);
        return 2;
    }
    char error[256] = {0};
    if (!fetch_trace_replay_begin_response_keyed(
            directory, error, sizeof(error))) {
        fprintf(stderr, "native trace inventory failed: %s\n",
                error[0] == '\0' ? "unknown error" : error);
        return 1;
    }
    FetchTraceReplayStats stats;
    bool valid = fetch_trace_replay_stats(&stats)
        && stats.response_keyed && stats.record_count == expected
        && stats.claimed_record_count == 0 && stats.request_count == 0
        && stats.matched_request_count == 0
        && stats.served_request_count == 0
        && stats.rejected_request_count == 0
        && stats.unmatched_request_count == 0
        && stats.conflicting_request_count == 0
        && stats.invalid_route_request_count == 0
        && stats.request_shape_mismatch_count == 0;
    fetch_trace_end();
    if (!valid) {
        fprintf(stderr,
                "native response-keyed inventory differs from expected count\n");
        return 1;
    }
    printf("trace-inventory-ok mode=response-keyed records=%zu\n", expected);
    return 0;
}
