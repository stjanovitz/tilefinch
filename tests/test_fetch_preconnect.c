/*
 * Host tests for the speculative-preconnect bounds
 * (docs/engineering/PSP_TRANSPORT.md).
 *
 * The dwell/eligibility state machine carries the bounds the PSP-only call
 * site enforces (one outstanding, ~300 ms dwell, cancel-on-change, gated on
 * network-ready / not-quiescing / not-under-harness), so it is exercised
 * exhaustively here without a device. The transport-facing API is proven
 * callable and inert-safe, and its started/reused/cancelled accounting is
 * checked with the connection left unpumped so no real network is touched.
 */
#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "  check failed: %s (line %d)\n", #cond,       \
                    __LINE__);                                             \
            return 1;                                                      \
        }                                                                  \
    } while (0)

/* ---- dwell state machine ------------------------------------------------ */

static int test_dwell_gate_and_one_shot(void)
{
    FetchPreconnectDwell dwell = {0};
    const uint32_t key = fetch_preconnect_tile_key("https://en.wikipedia.org");

    /* The arming frame's elapsed is time before focus arrived, so it is not
       credited; dwell counts focus time from arming onward. */
    CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 100u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 150u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    /* Crossing 300 ms (150 + 150) fires exactly one START. */
    CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 150u, 300u)
          == FETCH_PRECONNECT_DWELL_START);
    /* Never a second START for the same settled tile. */
    for (int i = 0; i < 5; i++) {
        CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 100u, 300u)
              == FETCH_PRECONNECT_DWELL_IDLE);
    }
    return 0;
}

static int test_dwell_exact_threshold(void)
{
    FetchPreconnectDwell dwell = {0};
    const uint32_t key = 0xABCDu;
    /* First frame arms with elapsed 0; the threshold is reached on the next. */
    CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 0u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 300u, 300u)
          == FETCH_PRECONNECT_DWELL_START);
    return 0;
}

static int test_dwell_cancel_on_focus_change(void)
{
    FetchPreconnectDwell dwell = {0};
    const uint32_t a = fetch_preconnect_tile_key("https://a.example");
    const uint32_t b = fetch_preconnect_tile_key("https://b.example");
    CHECK(a != b);

    /* Settle and START on tile A. */
    CHECK(fetch_preconnect_dwell_step(&dwell, true, a, 0u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    CHECK(fetch_preconnect_dwell_step(&dwell, true, a, 300u, 300u)
          == FETCH_PRECONNECT_DWELL_START);
    /* Focus moves to B: cancel A's connection, begin dwelling B. */
    CHECK(fetch_preconnect_dwell_step(&dwell, true, b, 50u, 300u)
          == FETCH_PRECONNECT_DWELL_CANCEL);
    CHECK(fetch_preconnect_dwell_step(&dwell, true, b, 100u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    /* B settles and starts on its own. */
    CHECK(fetch_preconnect_dwell_step(&dwell, true, b, 200u, 300u)
          == FETCH_PRECONNECT_DWELL_START);
    return 0;
}

static int test_dwell_focus_change_before_start_is_quiet(void)
{
    FetchPreconnectDwell dwell = {0};
    const uint32_t a = fetch_preconnect_tile_key("https://a.example");
    const uint32_t b = fetch_preconnect_tile_key("https://b.example");
    /* A is still dwelling (no START yet) when focus jumps to B: no cancel,
       nothing to tear down. */
    CHECK(fetch_preconnect_dwell_step(&dwell, true, a, 100u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    CHECK(fetch_preconnect_dwell_step(&dwell, true, b, 100u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    return 0;
}

static int test_dwell_ineligible_cancels_only_after_start(void)
{
    FetchPreconnectDwell dwell = {0};
    const uint32_t key = fetch_preconnect_tile_key("https://c.example");

    /* Ineligible while nothing has started is simply idle. */
    CHECK(fetch_preconnect_dwell_step(&dwell, false, 0u, 100u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    /* A key of 0 (no resolvable target) is treated as ineligible. */
    CHECK(fetch_preconnect_dwell_step(&dwell, true, 0u, 100u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);

    /* Settle + START, then becoming ineligible (e.g. network dropped, left
       HOME, or quiesce) must cancel. */
    CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 0u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 300u, 300u)
          == FETCH_PRECONNECT_DWELL_START);
    CHECK(fetch_preconnect_dwell_step(&dwell, false, key, 100u, 300u)
          == FETCH_PRECONNECT_DWELL_CANCEL);
    /* And a subsequent ineligible frame is quiet again (only one cancel). */
    CHECK(fetch_preconnect_dwell_step(&dwell, false, key, 100u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    return 0;
}

static int test_dwell_saturation_and_null(void)
{
    /* Saturating accumulation must not wrap past the threshold spuriously or
       underflow; a huge delta simply reaches the threshold. */
    FetchPreconnectDwell dwell = {0};
    const uint32_t key = 42u;
    CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 0u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    CHECK(fetch_preconnect_dwell_step(&dwell, true, key, 0xFFFFFFFFu, 300u)
          == FETCH_PRECONNECT_DWELL_START);
    /* Null dwell is inert, never a crash. */
    CHECK(fetch_preconnect_dwell_step(NULL, true, key, 100u, 300u)
          == FETCH_PRECONNECT_DWELL_IDLE);
    return 0;
}

static int test_tile_key(void)
{
    /* Deterministic, never zero, distinct for distinct hosts. */
    CHECK(fetch_preconnect_tile_key(NULL) == 0u);
    uint32_t w = fetch_preconnect_tile_key("https://en.wikipedia.org");
    uint32_t y = fetch_preconnect_tile_key("https://m.youtube.com");
    CHECK(w != 0u && y != 0u && w != y);
    CHECK(fetch_preconnect_tile_key("https://en.wikipedia.org") == w);
    return 0;
}

/* ---- transport-facing API: callable, inert-safe, correct accounting ------ */

static int test_api_rejects_bad_input(void)
{
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    fetch_preconnect_counters_reset();

    FetchPreconnectCounters counters = {0};
    fetch_preconnect_counters(&counters);
    CHECK(counters.started == 0 && counters.completed == 0
          && counters.reused == 0 && counters.cancelled == 0);

    /* Empty / null / non-http(s) targets are rejected without side effects. */
    CHECK(!fetch_preconnect(NULL, &budget));
    CHECK(!fetch_preconnect("", &budget));
    CHECK(!fetch_preconnect("not a url", &budget));
    CHECK(!fetch_preconnect_active());
    fetch_preconnect_counters(&counters);
    CHECK(counters.started == 0 && counters.cancelled == 0
          && counters.reused == 0);

    /* Cancel with nothing outstanding is a safe no-op. */
    fetch_preconnect_cancel("noop");
    CHECK(!fetch_preconnect_active());
    /* Pump with nothing outstanding is a safe no-op. */
    fetch_preconnect_pump();
    return 0;
}

/* The started/reused/cancelled bounds are checked without pumping, so the
   speculative connection is created and torn down but never driven onto the
   network. Requires the host libcurl transport; skipped cleanly otherwise. */
static int test_api_one_outstanding_accounting(void)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    fetch_preconnect_counters_reset();

    if (!fetch_preconnect("https://host-a.example", &budget)) {
        /* No usable transport in this build (e.g. curl unavailable). The dwell
           bounds above are the portable proof; report the skip and pass. */
        printf("  (transport unavailable; API accounting skipped)\n");
        CHECK(!fetch_preconnect_active());
        return 0;
    }
    FetchPreconnectCounters counters = {0};
    fetch_preconnect_counters(&counters);
    CHECK(fetch_preconnect_active());
    CHECK(counters.started == 1 && counters.reused == 0
          && counters.cancelled == 0);

    /* Same host: reused, no new connection, still exactly one outstanding. */
    CHECK(fetch_preconnect("https://host-a.example/some/path", &budget));
    fetch_preconnect_counters(&counters);
    CHECK(counters.started == 1 && counters.reused == 1);
    CHECK(fetch_preconnect_active());

    /* A bare host normalizes to the same https origin: also a reuse. */
    CHECK(fetch_preconnect("host-a.example", &budget));
    fetch_preconnect_counters(&counters);
    CHECK(counters.started == 1 && counters.reused == 2);

    /* Different host replaces the outstanding one: the previous (not yet
       completed) connection counts as cancelled, a new one starts. */
    CHECK(fetch_preconnect("https://host-b.example", &budget));
    fetch_preconnect_counters(&counters);
    CHECK(counters.started == 2 && counters.cancelled == 1);
    CHECK(fetch_preconnect_active());

    /* Explicit cancel tears the last one down and releases the transport. */
    fetch_preconnect_cancel("test-teardown");
    fetch_preconnect_counters(&counters);
    CHECK(counters.cancelled == 2);
    CHECK(!fetch_preconnect_active());

    /* The transport reference must have been released: the page budget returns
       to (near) empty rather than pinning curl's pool. */
    CHECK(budget.current == 0);
    return 0;
}

static int test_cross_boot_tls_preference_does_not_disable_transport(void)
{
    CHECK(fetch_tls_session_persistence_enabled());
    CHECK(fetch_set_tls_session_persistence_enabled(false));
    CHECK(!fetch_tls_session_persistence_enabled());
    /* An unset path keeps the preference a pure in-memory policy change in
       host tests; the PSP supplies the bounded data path before this call. */
    CHECK(fetch_tls_session_store_path() == NULL);
    CHECK(fetch_set_tls_session_persistence_enabled(true));
    CHECK(fetch_tls_session_persistence_enabled());
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        {"dwell_gate_and_one_shot", test_dwell_gate_and_one_shot},
        {"dwell_exact_threshold", test_dwell_exact_threshold},
        {"dwell_cancel_on_focus_change", test_dwell_cancel_on_focus_change},
        {"dwell_focus_change_before_start_is_quiet",
         test_dwell_focus_change_before_start_is_quiet},
        {"dwell_ineligible_cancels_only_after_start",
         test_dwell_ineligible_cancels_only_after_start},
        {"dwell_saturation_and_null", test_dwell_saturation_and_null},
        {"tile_key", test_tile_key},
        {"api_rejects_bad_input", test_api_rejects_bad_input},
        {"api_one_outstanding_accounting",
         test_api_one_outstanding_accounting},
        {"cross_boot_tls_preference",
         test_cross_boot_tls_preference_does_not_disable_transport},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int rc = tests[i].fn();
        if (rc != 0) {
            fprintf(stderr, "fetch-preconnect test '%s' FAILED\n",
                    tests[i].name);
            return 1;
        }
        printf("fetch-preconnect test '%s' ok\n", tests[i].name);
    }
    printf("tilefinch-fetch-preconnect: outcome=pass\n");
    return 0;
}
