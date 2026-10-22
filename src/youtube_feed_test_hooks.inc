/* Host-only test entry points into the shipping feed sign-in checks. They
   call the exact functions the feed job uses, so a regression in either one
   fails the unit test. */
bool youtube_lite_test_feed_authorization(BrowserSession *session,
    const TilefinchRequestContext *context, uint64_t seconds,
    char *output, size_t capacity)
{
    return lite_feed_authorization(session, context, seconds, output, capacity);
}

int youtube_lite_test_feed_authentication(const char *json, size_t length)
{
    YoutubeLiteSpan root, context;
    if (json == NULL || length == 0
        || !lite_json_span(json, json + length, &root) || *root.start != '{'
        || !lite_json_member(&root, "responseContext", &context)) return 0;
    return lite_feed_authentication(&context);
}
