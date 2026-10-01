/* Public ABI remains available for shared frontends, but a shipping binary
   cannot arm capture, read replay files, or silently fall back to a live
   request after a requested replay. No filesystem or session mutation. */
static bool trace_unavailable(char *error, size_t size)
{
    if (error != NULL && size != 0)
        snprintf(error, size, "HTTP capture/replay is not compiled in");
    return false;
}

bool fetch_trace_capture_begin(const char *directory, char *error, size_t size)
{
    (void) directory;
    return trace_unavailable(error, size);
}

bool fetch_trace_capture_arm_top_level(const char *directory, size_t ordinal,
                                      char *error, size_t size)
{
    (void) ordinal;
    return fetch_trace_capture_begin(directory, error, size);
}

bool fetch_trace_replay_begin(const char *directory, char *error, size_t size)
{
    return fetch_trace_capture_begin(directory, error, size);
}

bool fetch_trace_replay_begin_response_keyed(const char *directory,
                                             char *error, size_t size)
{
    return fetch_trace_capture_begin(directory, error, size);
}

bool fetch_trace_replay_seed_session(BrowserSession *session, char *error,
                                     size_t size)
{
    (void) session;
    return trace_unavailable(error, size);
}

bool fetch_trace_replay_active(void) { return false; }
bool fetch_trace_active(void) { return false; }
size_t fetch_trace_replay_served_count(void) { return 0; }
bool fetch_trace_replay_stats(FetchTraceReplayStats *stats)
{
    (void) stats;
    return false;
}
bool fetch_trace_replay_record_was_claimed(size_t sequence)
{
    (void) sequence;
    return false;
}
void fetch_trace_replay_io(uint64_t *opens, uint64_t *reads,
                           uint64_t *bytes, uint64_t *us)
{
    if (opens != NULL) *opens = 0;
    if (reads != NULL) *reads = 0;
    if (bytes != NULL) *bytes = 0;
    if (us != NULL) *us = 0;
}
void fetch_trace_end(void) {}
bool fetch_trace_clock_origin_ms(uint64_t *origin_ms)
{
    (void) origin_ms;
    return false;
}

static bool trace_activate_for_request(const FetchRequest *request,
                                       const char *url)
{
    (void) request;
    (void) url;
    return true;
}

static void trace_capture_invalidate(const char *reason, size_t sequence)
{
    (void) reason;
    (void) sequence;
}
