/* A loader that inserts its scripts one at a time after load, each on the
   previous one's error, while the script quota refuses them all. Every
   insertion is a <head> script, which renders nothing, so no insertion
   may cost a layout pass. Feature-detection libraries put dozens of
   classes on <html> (Amazon's AUI carries 27, Modernizr more). The
   sheet's `.card :has(...)` has no anchor key, so the insertion's own
   :has() probe cannot rule it out, and the head check must compare it
   (and every keyed anchor) with what <head> and <html> carry: that check
   must not give up on a long class list. */
static bool test_refused_head_scripts_skip_relayout(Budget *budget)
{
    const char *test_name =
        "refused head scripts after load cost no relayout";
    enum { SCRIPTS = 24, HTML_CLASSES = 40 };
    static char page[4096];
    size_t used = (size_t) snprintf(page, sizeof(page),
                                    "<!doctype html><html lang=en dir=ltr "
                                    "data-feature=yes class=\"");
    for (int i = 0; i < HTML_CLASSES; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "%sfeature-%d", i == 0 ? "" : " ", i);
    used += (size_t) snprintf(
        page + used, sizeof(page) - used,
        "\"><head><title>Loader</title><style>"
        ".card :has(:not(a)){outline:2px solid #00f}"
        ".card{padding:4px}"
        "</style></head><body><div class=card><a href=#x>FIRST</a></div>"
        "<p>Body text</p><script>globalThis.loaderReady=1;</script>"
        "</body></html>");
    static const char chain[] =
        "globalThis.loaderErrors=0;"
        "(function next(){"
        "if(loaderErrors===24){globalThis.pocSummary='chain:done';return;}"
        "const s=document.createElement('script');"
        "s.src='/module-'+loaderErrors+'.js';"
        "s.onerror=function(){loaderErrors++;next();};"
        "s.onload=function(){globalThis.pocSummary='chain:loaded';};"
        "document.head.appendChild(s);})();"
        "globalThis.pocSummary='chain:pending';";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(used < sizeof(page));
    REQUIRE(browser_session_init(&browser, budget, 1u * MIB));
    browser_ready = true;
    /* One script: the page's own inline script takes it. */
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://refused-loader.test/",
        page, 1, 512u * 1024u, 256u * 1024u));
    navigation_ready = true;
    REQUIRE(navigation.page.runtime != NULL);
    size_t relayouts = navigation.incremental_relayouts;
    size_t fast = navigation.performance.fast_relayouts;
    size_t full = navigation.performance.full_relayouts;
    size_t skips = navigation.performance.semantic_relayout_skips;
    size_t refused =
        navigation.page.script_result.dynamic_scripts_quota_rejected;
    REQUIRE(evaluate(&navigation, chain, "<refused-loader>"));
    REQUIRE(advance_until(&navigation, "chain:done", 4u * SCRIPTS, NULL));
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            == refused + SCRIPTS);
    if (navigation.incremental_relayouts - relayouts > 1u
        || navigation.performance.full_relayouts != full) {
        fprintf(stderr, "%s: relayouts=%zu fast=%zu full=%zu skips=%zu\n",
                test_name, navigation.incremental_relayouts - relayouts,
                navigation.performance.fast_relayouts - fast,
                navigation.performance.full_relayouts - full,
                navigation.performance.semantic_relayout_skips - skips);
    }
    /* Each turn's journal is classified and skipped; at most one pass is
       allowed for whatever else the turns did. */
    REQUIRE(navigation.incremental_relayouts - relayouts <= 1u);
    REQUIRE(navigation.performance.fast_relayouts - fast <= 1u);
    REQUIRE(navigation.performance.full_relayouts == full);
    REQUIRE(navigation.performance.semantic_relayout_skips - skips
            >= SCRIPTS - 1u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}
