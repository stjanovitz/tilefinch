#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"

#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Compile the real transport with its shipping feature boundary, rather than
   replacing it with a fake. The ordinary host core retains replay coverage. */
int main(void)
{
    char directory[] = "/tmp/tilefinch-no-fetch-trace-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char error[80] = {0};
    assert(!fetch_trace_capture_begin(directory, error, sizeof(error)));
    assert(strstr(error, "not compiled") != NULL);
    assert(!fetch_trace_capture_arm_top_level(directory, 1, error,
                                               sizeof(error)));
    assert(!fetch_trace_replay_begin(directory, error, sizeof(error)));
    assert(!fetch_trace_replay_begin_response_keyed(directory, error,
                                                    sizeof(error)));
    assert(!fetch_trace_replay_seed_session(NULL, error, sizeof(error)));
    assert(!fetch_trace_active() && !fetch_trace_replay_active());
    assert(!fetch_trace_replay_record_was_claimed(0));
    assert(fetch_trace_replay_served_count() == 0);
    assert(!fetch_trace_replay_stats(NULL));
    FetchTraceReplayStats stats = {0};
    assert(!fetch_trace_replay_stats(&stats));
    uint64_t opens = 1, reads = 1, bytes = 1, us = 1, origin = 123;
    fetch_trace_replay_io(&opens, &reads, &bytes, &us);
    assert(opens == 0 && reads == 0 && bytes == 0 && us == 0);
    assert(!fetch_trace_clock_origin_ms(&origin) && origin == 123);
    fetch_trace_end();
    assert(rmdir(directory) == 0); /* No capture metadata or body was written. */

    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 1, 4096);
    assert(scheduler != NULL);
    assert(!fetch_scheduler_uses_virtual_replay(scheduler));
    fetch_scheduler_destroy(scheduler);
    assert(budget.current == 0);
    return 0;
}
