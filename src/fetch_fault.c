#include "tilefinch/fetch_fault.h"

#include <stdio.h>

static FetchInjectedFailure injected_failure;

void fetch_inject_failure_once(FetchInjectedFailure failure)
{
    injected_failure = failure;
}

bool tilefinch_fetch_consume_injected_failure(FetchResult *result)
{
    FetchInjectedFailure failure = injected_failure;
    if (failure == FETCH_INJECT_NONE || result == NULL) return false;
    injected_failure = FETCH_INJECT_NONE;
    result->timed_out = failure == FETCH_INJECT_TIMEOUT;
    const char *message = failure == FETCH_INJECT_TIMEOUT
                          ? "injected transport timeout"
                          : failure == FETCH_INJECT_TLS
                            ? "injected TLS failure"
                            : failure == FETCH_INJECT_REDIRECT
                              ? "injected redirect failure"
                            : failure == FETCH_INJECT_TRUNCATED
                              ? "injected truncated response"
                              : "injected request cancellation";
    (void) snprintf(result->error, sizeof(result->error), "%s", message);
    return true;
}
