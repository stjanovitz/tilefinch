/* Whole-document commit entry points: navigation_commit_html, its static
   variant, and their shared policy. */

static bool navigation_commit_html_policy(
    NavigationSession *session, uint64_t generation,
    const char *url, const char *html, size_t html_length,
    int viewport_width, const FontSet *fonts,
    const ImageResources *images, bool record_history,
    bool allow_runtime)
{
    if (session == NULL || session->budget == NULL || generation == 0
        || generation != session->generation || session->cancelled
        || url == NULL || html == NULL || viewport_width <= 0) return false;
    PocDocument document = {0};
    uint64_t phase_started = navigation_now_us();
    if (!document_parse(&document, session->budget, html, html_length, 512)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "HTML parsing failed within the page budget");
        return false;
    }
    char document_referrer_policy[
        sizeof(session->pending_response_referrer_policy)];
    snprintf(document_referrer_policy, sizeof(document_referrer_policy), "%s",
             session->pending_response_referrer_policy);
    if (!navigation_scan_document_referrer_policy(
            &document, document_referrer_policy)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "document referrer metadata traversal failed");
        document_destroy(&document);
        return false;
    }
    snprintf(session->pending_response_referrer_policy,
             sizeof(session->pending_response_referrer_policy), "%s",
             document_referrer_policy);
    session->performance.parse_us += navigation_record_slice(
        session, NAVIGATION_SLICE_PARSER, phase_started, html_length);
    bool committed = navigation_commit_document(
        session, generation, url, &document, viewport_width, fonts, images,
        record_history, NULL, NULL, allow_runtime, false, NULL, NULL);
    document_destroy(&document);
    return committed;
}

bool navigation_commit_html(NavigationSession *session, uint64_t generation,
                            const char *url, const char *html,
                            size_t html_length, int viewport_width,
                            const FontSet *fonts,
                            const ImageResources *images,
                            bool record_history)
{
    return navigation_commit_html_policy(
        session, generation, url, html, html_length, viewport_width,
        fonts, images, record_history, true);
}

bool navigation_commit_static_html(
    NavigationSession *session, uint64_t generation,
    const char *url, const char *html, size_t html_length,
    int viewport_width, const FontSet *fonts,
    const ImageResources *images, bool record_history)
{
    return navigation_commit_html_policy(
        session, generation, url, html, html_length, viewport_width,
        fonts, images, record_history, false);
}
