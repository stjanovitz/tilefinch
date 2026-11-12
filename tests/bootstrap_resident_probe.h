#ifndef TILEFINCH_BOOTSTRAP_RESIDENT_PROBE_H
#define TILEFINCH_BOOTSTRAP_RESIDENT_PROBE_H

/* Small realms in tests are sized from what the bootstrap really leaves
   resident, plus the test's own margin. An absolute size a few KiB above the
   bootstrap failed on any bootstrap growth and said nothing about what grew.
   The one limit on that growth is tests/test_bootstrap_footprint.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"

#define TEST_BOOTSTRAP_PROBE_REALM_BYTES (8u * 1024u * 1024u)

/* The headroom the 2 MiB test realms had when the footprint guard came in
   (resident bootstrap 1,922 KiB): enough for a realm that only has to exist
   and run a few statements. */
#define TEST_SMALL_REALM_MARGIN_BYTES (128u * 1024u)

/* Bytes a deferred realm over a trivial document holds right after creation,
   measured once per process with a probe realm. The probe is charged to
   `budget` (a probe budget of its own when NULL) and returned before this
   returns; lexbor is installed on that budget for the probe unless it already
   is, so call this when no other budget holds lexbor allocations. 0 if the
   probe could not be made. */
static inline size_t test_bootstrap_resident_bytes(Budget *budget)
{
    static size_t measured;
    if (measured != 0) return measured;
    Budget own;
    if (budget == NULL) {
        budget_init(&own, 2u * TEST_BOOTSTRAP_PROBE_REALM_BYTES);
        budget = &own;
    }
    bool installed = false;
    if (!budget_lexbor_is_installed(budget)) {
        if (!budget_install_lexbor(budget)) return 0;
        installed = true;
    }
    static const char html[] = "<!doctype html><body>probe</body>";
    PocDocument document;
    memset(&document, 0, sizeof(document));
    if (document_parse(&document, budget, html, sizeof(html) - 1u, 8u)) {
        ScriptResult result;
        memset(&result, 0, sizeof(result));
        ScriptRuntime *runtime = script_runtime_create_deferred(
            &document, budget, TEST_BOOTSTRAP_PROBE_REALM_BYTES, 1000,
            "https://bootstrap-probe.test/", NULL, &result);
        size_t remaining = runtime == NULL
            ? TEST_BOOTSTRAP_PROBE_REALM_BYTES
            : script_runtime_heap_remaining(runtime);
        measured = remaining < TEST_BOOTSTRAP_PROBE_REALM_BYTES
            ? TEST_BOOTSTRAP_PROBE_REALM_BYTES - remaining : 0u;
        script_runtime_destroy(runtime);
        document_destroy(&document);
    }
    if (installed) (void) budget_uninstall_lexbor(budget);
    return measured;
}

/* A realm size `margin` bytes above the measured bootstrap. A failed probe
   aborts: 0 means "the default size" to some configurations, which would
   quietly run the test without its small realm. */
static inline size_t test_small_realm_bytes(Budget *budget, size_t margin)
{
    size_t resident = test_bootstrap_resident_bytes(budget);
    if (resident == 0) {
        fprintf(stderr, "bootstrap resident probe failed\n");
        abort();
    }
    return resident + margin;
}

#endif
