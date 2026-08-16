int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
                "usage: %s PORT HINT_PORT PROXY_PORT TRACE_DIRECTORY\n",
                argv[0]);
        return 2;
    }
    unsigned long parsed = strtoul(argv[1], NULL, 10);
    unsigned long hint_port = strtoul(argv[2], NULL, 10);
    unsigned long proxy_port = strtoul(argv[3], NULL, 10);
    if (parsed == 0 || parsed > 65535
        || hint_port == 0 || hint_port > 65535
        || proxy_port == 0 || proxy_port > 65535) return 2;
    Budget budget;
    /* Redirect navigation tests may briefly retain the incumbent document
       while the scripted candidate owns its realm and authoritative layout.
       Keep this harness above that transactional high-water mark; individual
       allocation-failure tests install their own deliberately tight budgets. */
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
#define RUN_REDIRECT_TEST(label, expression) do {                            \
    if (!(expression)) {                                                    \
        fprintf(stderr, "redirect-test stage failed: %s\n", (label));      \
        return 1;                                                           \
    }                                                                       \
} while (0)
    RUN_REDIRECT_TEST(
        "callback-abort-classification",
        test_redirect_callback_abort_classification());
    RUN_REDIRECT_TEST(
        "private-network-admission",
        test_private_network_admission(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "hsts-security-header",
        test_hsts_is_security_header(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "hsts-scheduled-redirect",
        test_hsts_applies_to_scheduled_redirects(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "stylesheet-redirect-csp",
        test_stylesheet_redirect_rechecks_csp(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "callback-abort-shape",
        test_redirect_callback_abort_shape(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "supported-accept-encoding",
        test_supported_accept_encoding(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST("same-host",
                      test_same_host_redirect(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "redirect-url-validator",
        test_redirect_url_validator(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "single-hop-redirect-boundary",
        test_single_hop_redirect_boundary(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "logical-url-fragments",
        test_logical_url_fragments(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "redirect-referrer-policy",
        test_redirect_referrer_policy(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "referrer-policy-metadata-helper",
        test_referrer_policy_metadata_helper());
    RUN_REDIRECT_TEST(
        "referrer-policy-beyond-header-snapshot",
        test_referrer_policy_beyond_header_snapshot(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "empty-location-reference",
        test_empty_location_reference(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "javascript-cors",
        test_javascript_cors(
            &budget, (unsigned) parsed, (unsigned) hint_port));
    RUN_REDIRECT_TEST(
        "redirect-cors-final-response",
        test_redirect_cors_final_response_boundary(
            &budget, (unsigned) parsed, (unsigned) hint_port));
    RUN_REDIRECT_TEST(
        "cross-host",
        test_cross_host_cookie_isolation(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "policy-aware-redirect-cookies",
        test_policy_aware_redirect_cookies(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "sticky-taint",
        test_same_origin_credentials_sticky_taint(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "omit-cookies",
        test_omit_drops_response_cookies(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "redirect-body-and-cookie-bounds",
        test_redirect_short_circuit_and_cookie_bounds(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "response-cookie-allocation-failure",
        test_response_cookie_allocation_failure((unsigned) parsed));
    RUN_REDIRECT_TEST(
        "informational-response-isolation",
        test_informational_response_isolation(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "client-hint-header-combination",
        test_duplicate_client_hint_headers(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "proxy-connect-header-suppression",
        test_proxy_connect_headers_suppressed(&budget));
    RUN_REDIRECT_TEST(
        "scheduler", test_scheduled_redirect(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "bounded-buffered-scheduler",
        test_scheduled_buffered_body_is_cooperative(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "overlapping-scheduler-transport-pool",
        test_overlapping_scheduler_transport_pool(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "methods", test_redirect_methods(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "generic-methods-and-head",
        test_generic_method_bodies_and_head(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "fetch-site-none",
        test_fetch_site_none_survives_redirects(
            &budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "limit", test_redirect_limit(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "client-hint-scope",
        test_client_hint_redirect_scope(
            &budget, (unsigned) hint_port, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "navigation-client-hint-scope",
        test_navigation_critical_hint_redirect_scope(
            &budget, (unsigned) parsed, (unsigned) hint_port));
    RUN_REDIRECT_TEST(
        "navigation",
        test_navigation_redirect_fragment(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "single-hop-trace-acquisition",
        test_single_hop_trace_acquisition(
            &budget, (unsigned) parsed, argv[4]));
    RUN_REDIRECT_TEST(
        "delayed-top-level-trace-seed",
        test_delayed_top_level_trace_seed(
            &budget, (unsigned) parsed, argv[4]));
    RUN_REDIRECT_TEST(
        "scheduled-cookie-trace",
        test_scheduled_cookie_trace_roundtrip(
            &budget, (unsigned) parsed, argv[4]));
    RUN_REDIRECT_TEST(
        "response-cookie-trace-capacity",
        test_response_cookie_trace_capacity(
            &budget, (unsigned) parsed, argv[4]));
    RUN_REDIRECT_TEST(
        "trace",
        test_redirect_trace_roundtrip(
            &budget, (unsigned) parsed, argv[4]));
    RUN_REDIRECT_TEST(
        "javascript",
        test_javascript_credentials(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "javascript-binary-responses",
        test_javascript_binary_responses(&budget, (unsigned) parsed));
    RUN_REDIRECT_TEST(
        "browser-engine-url-replay-promotion",
        test_browser_engine_url_replay_promotion(
            &budget, (unsigned) parsed, argv[4]));
    RUN_REDIRECT_TEST(
        "trace-logical-url-and-referrer-metadata",
        test_trace_logical_url_and_referrer_metadata(
            &budget, (unsigned) parsed, argv[4]));
    RUN_REDIRECT_TEST(
        "trace-reordered-repeated-cancelled",
        test_trace_reordered_repeated_and_cancelled(
            &budget, (unsigned) parsed, argv[4]));
    RUN_REDIRECT_TEST(
        "trace-dynamic-context-and-legacy-inventory",
        test_trace_dynamic_source_context_and_legacy_inventory(
            &budget, (unsigned) parsed, argv[4]));
    RUN_REDIRECT_TEST(
        "redirect-cors-trace-taint",
        test_redirect_cors_trace_taint(
            &budget, (unsigned) parsed, argv[4]));
#undef RUN_REDIRECT_TEST
    if (budget.current != 0) {
        fprintf(stderr, "redirect-test leaked %zu budget bytes\n",
                budget.current);
        return 1;
    }
    puts("redirect-test: per-hop policy, metadata, streaming, methods, and navigation fragments passed");
    return 0;
}
