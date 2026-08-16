#include <stdio.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"

#define MIB (1024u * 1024u)

static int fail(const char *message)
{
    fprintf(stderr, "failure-recovery: FAIL: %s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) return fail("expected one local HTTP URL");
    Budget budget;
    budget_init(&budget, 4 * MIB);
    static const FetchInjectedFailure failures[] = {
        FETCH_INJECT_TIMEOUT, FETCH_INJECT_TLS, FETCH_INJECT_REDIRECT,
        FETCH_INJECT_TRUNCATED, FETCH_INJECT_CANCELLED
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        FetchResult *failed = fetch_result_create(&budget);
        if (failed == NULL) return fail("could not reserve failed result");
        fetch_inject_failure_once(failures[i]);
        if (fetch_url(&budget, argv[1], 64 * 1024, 2000, failed)) {
            fetch_result_free(failed);
            return fail("injected request unexpectedly succeeded");
        }
        fetch_result_free(failed);

        FetchResult *recovered = fetch_result_create(&budget);
        if (recovered == NULL) return fail("could not reserve recovery result");
        if (!fetch_url(&budget, argv[1], 64 * 1024, 2000, recovered)
            || recovered->status_code != 200 || recovered->length == 0) {
            fprintf(stderr, "recovery detail: %s\n", recovered->error);
            fetch_result_free(recovered);
            return fail("next request did not recover in the same process");
        }
        fetch_result_free(recovered);
    }

    budget_inject_failure_after(&budget, 0);
    if (fetch_result_create(&budget) != NULL) {
        return fail("injected result allocation unexpectedly succeeded");
    }
    FetchResult *recovered_result = fetch_result_create(&budget);
    if (recovered_result == NULL) return fail("allocator did not recover");
    fetch_result_free(recovered_result);
    if (budget.current != 0) return fail("teardown leaked budget ownership");
    printf("failure-recovery: transport=5 allocator=1 current=0 status=PASS\n");
    return 0;
}
