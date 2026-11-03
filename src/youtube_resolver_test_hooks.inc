/* Host-only test entry points into the shipping resolver checks. They call
   the exact functions the resolver job uses, so a regression in either one
   fails the unit test. */
bool youtube_resolver_test_watch_unsigned(const char *html, size_t length,
                                          const char *key, uint64_t *output)
{
    return html != NULL && key != NULL && output != NULL
        && youtube_watch_unsigned(html, length, key, output);
}

bool youtube_resolver_test_accept_direct_stream(
    const char *client_name, const YoutubeStream *resolved,
    YoutubeStream *stream, char *error, size_t error_size)
{
    if (client_name == NULL || resolved == NULL || stream == NULL)
        return false;
    for (size_t at = 0;
         at < sizeof(youtube_client_profiles)
                  / sizeof(youtube_client_profiles[0]);
         at++) {
        if (strcmp(youtube_client_profiles[at].name, client_name) != 0)
            continue;
        YoutubeStream copy = *resolved;
        return youtube_accept_direct_stream(
            &copy, &youtube_client_profiles[at], 1u, 0, 200, stream,
            error, error_size);
    }
    return false;
}
