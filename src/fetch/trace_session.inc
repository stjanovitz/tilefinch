static bool trace_directory_prepare(const char *directory, bool capture,
                                    char *error, size_t error_size)
{
    if (directory == NULL || directory[0] == '\0'
        || strlen(directory) >= sizeof(fetch_trace.directory)
        || fetch_trace.mode != FETCH_TRACE_OFF) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size, "invalid or active HTTP trace");
        }
        return false;
    }
    struct stat info;
    if (capture && mkdir(directory, 0700) != 0) {
        if (stat(directory, &info) != 0 || !S_ISDIR(info.st_mode)) {
            if (error != NULL && error_size != 0) {
                snprintf(error, error_size,
                         "could not create HTTP trace directory");
            }
            return false;
        }
    } else if (!capture
               && (stat(directory, &info) != 0 || !S_ISDIR(info.st_mode))) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size, "HTTP replay directory is missing");
        }
        return false;
    }
    return true;
}

static bool trace_info_path(char output[FETCH_TRACE_PATH_CAPACITY])
{
    int written = snprintf(output, FETCH_TRACE_PATH_CAPACITY,
                           "%s/trace.meta", fetch_trace.directory);
    return written >= 0 && (size_t) written < FETCH_TRACE_PATH_CAPACITY;
}

static bool trace_info_partial_path(char output[FETCH_TRACE_PATH_CAPACITY])
{
    int written = snprintf(output, FETCH_TRACE_PATH_CAPACITY,
                           "%s/trace.meta.capture", fetch_trace.directory);
    return written >= 0 && (size_t) written < FETCH_TRACE_PATH_CAPACITY;
}

/* A corpus with a missing sequence can accidentally replay stale files from
   an older capture. Remove its authority file and stop capture immediately;
   record bodies may remain, but cannot be opened as a valid replay. */
static void trace_capture_invalidate(const char *reason, size_t sequence)
{
    if (fetch_trace.mode != FETCH_TRACE_CAPTURE) return;
    char info_path[FETCH_TRACE_PATH_CAPACITY];
    char partial_path[FETCH_TRACE_PATH_CAPACITY];
    if (trace_info_path(info_path)) (void) remove(info_path);
    if (trace_info_partial_path(partial_path)) (void) remove(partial_path);
    fprintf(stderr, "HTTP trace invalidated at record %zu: %s\n",
            sequence, reason == NULL ? "capture failed" : reason);
    /* fetch_trace_end() normally finalizes a capture. This path has already
       revoked authority and must not attempt to publish it again. */
    memset(&fetch_trace, 0, sizeof(fetch_trace));
}

static bool trace_capture_info_write(const BrowserSession *session,
                                     bool include_cookie_seed,
                                     char *error, size_t error_size)
{
    char info_path[FETCH_TRACE_PATH_CAPACITY];
    char partial_path[FETCH_TRACE_PATH_CAPACITY];
    if (!trace_info_path(info_path) || !trace_info_partial_path(partial_path)) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size, "HTTP trace metadata path is too long");
        }
        return false;
    }
    /* The authority file is published only by trace_capture_finalize(). A
       process interrupted during capture therefore cannot masquerade as a
       legacy, complete corpus, and stale authority from an older capture is
       revoked before the first new request can be assigned a sequence. */
    bool reset = (remove(info_path) == 0 || errno == ENOENT)
        && (remove(partial_path) == 0 || errno == ENOENT);
    FILE *info_file = reset ? fopen(partial_path, "wb") : NULL;
    bool info_ok = info_file != NULL
        && fprintf(info_file, "psp-http-trace-clock=1\norigin-ms=%llu\n",
                   (unsigned long long) fetch_trace.clock_origin_ms) >= 0;
    /* Immediate capture has no session parameter and therefore cannot make
       a truthful statement about the initial jar. Delayed activation
       explicitly opts in even when the request has no jar, which truthfully
       records seed count zero. */
    if (info_ok && include_cookie_seed) {
        info_ok = fprintf(
                      info_file,
                      "replay-start=delayed-top-level-v1\n") >= 0
            && trace_write_cookie_seed(info_file, session);
    }
    if (info_file != NULL) info_ok = fclose(info_file) == 0 && info_ok;
    if (!info_ok && error != NULL && error_size != 0) {
        snprintf(error, error_size, "could not write HTTP trace metadata");
    }
    if (!info_ok) (void) remove(partial_path);
    return info_ok;
}

#define TRACE_INFO_LINE_CAPACITY (BROWSER_ORIGIN_LIMIT + 160)

/* Read the untrusted corpus byte-by-byte so embedded NULs cannot hide a
   trailing field from strlen-based parsing. A final line without a newline
   remains valid, while overlong records fail closed. */
static bool trace_info_read_line(FILE *file,
                                 char line[TRACE_INFO_LINE_CAPACITY],
                                 bool *available)
{
    if (file == NULL || line == NULL || available == NULL) return false;
    *available = false;
    size_t length = 0;
    for (;;) {
        int byte = fgetc(file);
        if (byte == EOF) {
            if (ferror(file)) return false;
            if (length == 0) return true;
            break;
        }
        if (byte == '\0') return false;
        if (byte == '\n') break;
        if (length + 1 >= TRACE_INFO_LINE_CAPACITY) return false;
        line[length++] = (char) byte;
    }
    if (length != 0 && line[length - 1] == '\r') line[--length] = '\0';
    else line[length] = '\0';
    *available = true;
    return true;
}

static bool trace_parse_u64_decimal(const char *value, uint64_t *parsed)
{
    if (value == NULL || value[0] == '\0' || parsed == NULL) return false;
    uint64_t result = 0;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        if (*at < '0' || *at > '9') return false;
        uint64_t digit = (uint64_t) (*at - '0');
        if (result > (UINT64_MAX - digit) / UINT64_C(10)) return false;
        result = result * UINT64_C(10) + digit;
    }
    *parsed = result;
    return true;
}

static bool trace_replay_info_read(char *error, size_t error_size)
{
    char info_path[FETCH_TRACE_PATH_CAPACITY];
    if (!trace_info_path(info_path)) return false;
    FILE *info_file = fopen(info_path, "rb");
    char line[TRACE_INFO_LINE_CAPACITY];
    bool version = false, origin = false, replay_start = false;
    bool completion_seen = false, count_seen = false;
    size_t record_count = 0;
    bool valid = info_file != NULL;
    while (valid) {
        bool available = false;
        if (!trace_info_read_line(info_file, line, &available)) {
            valid = false;
            break;
        }
        if (!available) break;
        if (strcmp(line, "psp-http-trace-clock=1") == 0) {
            if (version) valid = false;
            version = true;
        } else if (strncmp(line, "origin-ms=", 10) == 0) {
            uint64_t parsed = 0;
            if (origin || !trace_parse_u64_decimal(line + 10, &parsed)
                || parsed == 0) {
                valid = false;
            } else {
                fetch_trace.clock_origin_ms = parsed;
                origin = true;
            }
        } else if (strncmp(line, "replay-start=", 13) == 0) {
            if (replay_start
                || strcmp(line + 13, "delayed-top-level-v1") != 0) {
                valid = false;
            } else {
                replay_start = true;
            }
        } else if (strncmp(line, "capture-complete=", 17) == 0) {
            if (completion_seen || strcmp(line + 17, "yes") != 0) {
                valid = false;
            } else {
                completion_seen = true;
            }
        } else if (strncmp(line, "record-count=", 13) == 0) {
            if (count_seen
                || !trace_parse_size_value(line + 13, &record_count)
                || record_count > FETCH_TRACE_RECORD_LIMIT) {
                valid = false;
            } else {
                count_seen = true;
            }
        }
    }
    if (info_file != NULL && fclose(info_file) != 0) valid = false;
    if (!(valid && version && origin
          && completion_seen == count_seen)) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size,
                     "HTTP replay clock metadata is missing or invalid");
        }
        return false;
    }
    fetch_trace.replay_starts_at_delayed_top_level = replay_start;
    fetch_trace.replay_record_count = record_count;
    fetch_trace.replay_record_count_authoritative = count_seen;
    return true;
}

static bool trace_regular_file_state(const char *path, bool *exists)
{
    if (path == NULL || exists == NULL) return false;
    struct stat info;
    if (stat(path, &info) == 0) {
        *exists = S_ISREG(info.st_mode);
        return *exists;
    }
    if (errno == ENOENT) {
        *exists = false;
        return true;
    }
    return false;
}

static bool trace_record_file_state(size_t sequence, const char *suffix,
                                    bool *exists)
{
    char path[FETCH_TRACE_PATH_CAPACITY];
    return trace_path(path, sizeof(path), sequence, suffix)
        && trace_regular_file_state(path, exists);
}

static bool trace_record_pair_present(size_t sequence)
{
    bool meta = false, body = false;
    return trace_record_file_state(sequence, "meta", &meta) && meta
        && trace_record_file_state(sequence, "body", &body) && body;
}

/* Finalized captures carry an authoritative count, so stale numbered files
   from a previous run are outside the corpus. Retained legacy traces have no
   count; discover only their bounded contiguous prefix, preserving their old
   sequence semantics without permitting an unbounded directory walk. */
static bool trace_replay_inventory(char *error, size_t error_size)
{
    size_t count = fetch_trace.replay_record_count;
    bool valid = true;
    if (fetch_trace.replay_record_count_authoritative) {
        for (size_t sequence = 0; valid && sequence < count; sequence++) {
            valid = trace_record_pair_present(sequence);
        }
    } else {
        count = 0;
        while (count < FETCH_TRACE_RECORD_LIMIT) {
            bool meta = false;
            if (!trace_record_file_state(count, "meta", &meta)) {
                valid = false;
                break;
            }
            if (!meta) break;
            if (!trace_record_pair_present(count)) {
                valid = false;
                break;
            }
            count++;
        }
        if (valid && count == FETCH_TRACE_RECORD_LIMIT) {
            bool overflow = false;
            valid = trace_record_file_state(
                FETCH_TRACE_RECORD_LIMIT, "meta", &overflow) && !overflow;
        }
        if (valid) fetch_trace.replay_record_count = count;
    }
    if (!valid && error != NULL && error_size != 0) {
        snprintf(error, error_size,
                 fetch_trace.replay_record_count_authoritative
                   ? "HTTP replay finalized record inventory is incomplete"
                   : "HTTP replay legacy record inventory is invalid");
    }
    return valid;
}

static bool trace_capture_finalize(void)
{
    if (fetch_trace.mode != FETCH_TRACE_CAPTURE
        || fetch_trace.sequence > FETCH_TRACE_RECORD_LIMIT) return false;
    for (size_t sequence = 0; sequence < fetch_trace.sequence; sequence++) {
        if (!trace_record_pair_present(sequence)) return false;
    }
    char info_path[FETCH_TRACE_PATH_CAPACITY];
    char partial_path[FETCH_TRACE_PATH_CAPACITY];
    if (!trace_info_path(info_path)
        || !trace_info_partial_path(partial_path)) return false;
    FILE *partial = fopen(partial_path, "ab");
    bool ok = partial != NULL
        && fprintf(partial, "capture-complete=yes\nrecord-count=%zu\n",
                   fetch_trace.sequence) >= 0;
    if (partial != NULL) ok = fclose(partial) == 0 && ok;
    /* rename() is the publication point. Until the fully written temporary
       file has closed successfully, no trace.meta authority exists. */
    if (ok) ok = rename(partial_path, info_path) == 0;
    if (!ok) {
        (void) remove(partial_path);
        (void) remove(info_path);
    }
    return ok;
}

static bool trace_capture_activate(const BrowserSession *session,
                                   bool include_cookie_seed,
                                   char *error, size_t error_size)
{
    fetch_trace.mode = FETCH_TRACE_CAPTURE;
    fetch_trace.sequence = 0;
    fetch_trace.clock_origin_ms = trace_wall_time_ms();
    const char *raw_cookie_opt_in = getenv("TILEFINCH_TRACE_RAW_COOKIES");
    fetch_trace.capture_raw_cookie_values = raw_cookie_opt_in != NULL
        && strcmp(raw_cookie_opt_in, "1") == 0;
    if (!trace_capture_info_write(
            session, include_cookie_seed, error, error_size)) {
        memset(&fetch_trace, 0, sizeof(fetch_trace));
        return false;
    }
    return true;
}

static bool fetch_trace_begin(FetchTraceMode mode, const char *directory,
                              char *error, size_t error_size)
{
    if ((mode != FETCH_TRACE_CAPTURE && mode != FETCH_TRACE_REPLAY)
        || !trace_directory_prepare(
               directory, mode == FETCH_TRACE_CAPTURE,
               error, error_size)) return false;
    free(fetch_trace.replay_route_index);
    memset(&fetch_trace, 0, sizeof(fetch_trace));
    fetch_trace.mode = mode;
    snprintf(fetch_trace.directory, sizeof(fetch_trace.directory), "%s",
             directory);
    if (mode == FETCH_TRACE_CAPTURE) {
        return trace_capture_activate(NULL, false, error, error_size);
    }
    if (!trace_replay_info_read(error, error_size)
        || !trace_replay_inventory(error, error_size)) {
        fetch_trace_end();
        return false;
    }
    return true;
}

bool fetch_trace_capture_begin(const char *directory, char *error,
                               size_t error_size)
{
    return fetch_trace_begin(FETCH_TRACE_CAPTURE, directory, error, error_size);
}

bool fetch_trace_capture_arm_top_level(
    const char *directory, size_t request_ordinal,
    char *error, size_t error_size)
{
    if (request_ordinal == 0
        || !trace_directory_prepare(directory, true, error, error_size)) {
        if (request_ordinal == 0 && error != NULL && error_size != 0) {
            snprintf(error, error_size,
                     "top-level capture ordinal must be positive");
        }
        return false;
    }
    memset(&fetch_trace, 0, sizeof(fetch_trace));
    fetch_trace.mode = FETCH_TRACE_CAPTURE_ARMED;
    fetch_trace.top_level_request_ordinal = request_ordinal;
    snprintf(fetch_trace.directory, sizeof(fetch_trace.directory), "%s",
             directory);
    /* As with immediate capture, arming a directory invalidates any prior
       corpus. Do not leave stale clock metadata making old records appear
       replayable while preparatory live traffic is still running. */
    char info_path[FETCH_TRACE_PATH_CAPACITY];
    char partial_path[FETCH_TRACE_PATH_CAPACITY];
    if (!trace_info_path(info_path)
        || !trace_info_partial_path(partial_path)
        || (remove(info_path) != 0 && errno != ENOENT)
        || (remove(partial_path) != 0 && errno != ENOENT)) {
        fetch_trace_end();
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size,
                     "could not reset HTTP trace metadata");
        }
        return false;
    }
    return true;
}

bool fetch_trace_replay_begin(const char *directory, char *error,
                              size_t error_size)
{
    bool began = fetch_trace_begin(
        FETCH_TRACE_REPLAY, directory, error, error_size);
    if (began) trace_response_key_index_build();
    return began;
}

bool fetch_trace_replay_begin_response_keyed(
    const char *directory, char *error, size_t error_size)
{
    bool began = fetch_trace_begin(
        FETCH_TRACE_REPLAY, directory, error, error_size);
    if (began) {
        fetch_trace.replay_response_keyed = true;
        trace_response_key_index_build();
    }
    return began;
}

typedef enum {
    TRACE_SEED_DOMAIN,
    TRACE_SEED_PATH,
    TRACE_SEED_NAME,
    TRACE_SEED_VALUE_LENGTH,
    TRACE_SEED_HOST_ONLY,
    TRACE_SEED_SECURE,
    TRACE_SEED_HTTP_ONLY,
    TRACE_SEED_SAME_SITE,
    TRACE_SEED_PARTITIONED,
    TRACE_SEED_PARTITION_KEY,
    TRACE_SEED_CREATION_SEQUENCE,
    TRACE_SEED_FIELD_COUNT
} TraceSeedField;

static bool trace_seed_index_and_field(const char *name, size_t *index,
                                       TraceSeedField *field)
{
    static const char prefix[] = "cookie-seed-";
    if (name == NULL || index == NULL || field == NULL
        || strncmp(name, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *at = name + sizeof(prefix) - 1;
    if (*at < '0' || *at > '9') return false;
    size_t parsed = 0;
    while (*at >= '0' && *at <= '9') {
        size_t digit = (size_t) (*at++ - '0');
        if (parsed > (SIZE_MAX - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    if (*at++ != '-') return false;
    static const struct {
        const char *name;
        TraceSeedField field;
    } fields[] = {
        {"domain", TRACE_SEED_DOMAIN},
        {"path", TRACE_SEED_PATH},
        {"name", TRACE_SEED_NAME},
        {"value-length", TRACE_SEED_VALUE_LENGTH},
        {"host-only", TRACE_SEED_HOST_ONLY},
        {"secure", TRACE_SEED_SECURE},
        {"http-only", TRACE_SEED_HTTP_ONLY},
        {"same-site", TRACE_SEED_SAME_SITE},
        {"partitioned", TRACE_SEED_PARTITIONED},
        {"partition-key", TRACE_SEED_PARTITION_KEY},
        {"creation-sequence", TRACE_SEED_CREATION_SEQUENCE}
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (strcmp(at, fields[i].name) == 0) {
            *index = parsed;
            *field = fields[i].field;
            return true;
        }
    }
    return false;
}

static bool trace_seed_placeholder(const BrowserCookieEntry *entry)
{
    if (entry == NULL || entry->value == NULL) return false;
    for (size_t i = 0; i < entry->value_length; i++) {
        if (entry->value[i] != 'x') return false;
    }
    return entry->value[entry->value_length] == '\0';
}

/* Reopening an immutable replay after a compatibility probe should be
   idempotent when the session still contains exactly the imported boundary
   seed. It must not silently accept a jar changed by replayed responses. */
static bool trace_session_matches_seed(
    const BrowserSession *session, const BrowserCookieSeedEntry *entries,
    size_t count)
{
    if (session == NULL || session->budget == NULL) return false;
    size_t active = 0, total = 0, maximum_sequence = 0;
    bool matched[BROWSER_COOKIE_ENTRIES] = {false};
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        if (session->cookies[i].value != NULL) active++;
    }
    if (active != count) return false;
    for (size_t seed_index = 0; seed_index < count; seed_index++) {
        const BrowserCookieSeedEntry *seed = &entries[seed_index];
        bool found = false;
        for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
            const BrowserCookieEntry *entry = &session->cookies[i];
            if (matched[i] || entry->value == NULL
                || strcmp(entry->domain, seed->domain) != 0
                || strcmp(browser_cookie_entry_path(entry),
                          seed->path) != 0
                || strcmp(entry->name, seed->name) != 0
                || strcmp(entry->partition_key, seed->partition_key) != 0
                || entry->value_length != seed->value_length
                || entry->host_only != seed->host_only
                || entry->secure != seed->secure
                || entry->http_only != seed->http_only
                || entry->same_site != seed->same_site
                || entry->partitioned != seed->partitioned
                || entry->creation_sequence != seed->creation_sequence
                || entry->expires_at != 0
                || !trace_seed_placeholder(entry)) continue;
            matched[i] = true;
            found = true;
            break;
        }
        if (!found || seed->value_length > SIZE_MAX - total) return false;
        total += seed->value_length;
        if (seed->creation_sequence > maximum_sequence) {
            maximum_sequence = seed->creation_sequence;
        }
    }
    return session->cookie_bytes == total
        && session->cookie_clock == maximum_sequence;
}

bool fetch_trace_replay_seed_session(
    BrowserSession *session, char *error, size_t error_size)
{
    if (fetch_trace.mode != FETCH_TRACE_REPLAY || session == NULL
        || session->budget == NULL) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size,
                     "HTTP replay cookie seed requires an active replay");
        }
        return false;
    }
    char info_path[FETCH_TRACE_PATH_CAPACITY];
    if (!trace_info_path(info_path)) return false;
    FILE *file = fopen(info_path, "rb");
    char line[TRACE_INFO_LINE_CAPACITY];
    bool valid = file != NULL, saw_seed = false;
    bool version_seen = false, count_seen = false;
    size_t seed_count = 0;
    while (valid) {
        bool available = false;
        if (!trace_info_read_line(file, line, &available)) {
            valid = false;
            break;
        }
        if (!available) break;
        if (strncmp(line, "cookie-seed-", 12) != 0) continue;
        saw_seed = true;
        char *equals = strchr(line, '=');
        if (equals == NULL) {
            valid = false;
            break;
        }
        *equals++ = '\0';
        if (strcmp(line, "cookie-seed-version") == 0) {
            if (version_seen || strcmp(equals, "1") != 0) valid = false;
            version_seen = true;
        } else if (strcmp(line, "cookie-seed-count") == 0) {
            if (count_seen || !trace_parse_size_value(equals, &seed_count)
                || seed_count > BROWSER_COOKIE_ENTRIES) valid = false;
            count_seen = true;
        }
    }
    if (file != NULL && fclose(file) != 0) valid = false;
    if (!saw_seed) return valid; /* Legacy trace: deliberately a no-op. */
    if (!(valid && version_seen && count_seen)) goto invalid;

    BrowserCookieSeedEntry *entries = seed_count == 0 ? NULL
        : budget_calloc(session->budget, seed_count, sizeof(*entries));
    if (seed_count != 0 && entries == NULL) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size,
                     "HTTP replay cookie seed exceeded shared memory budget");
        }
        return false;
    }
    uint16_t fields_seen[BROWSER_COOKIE_ENTRIES] = {0};
    file = fopen(info_path, "rb");
    valid = file != NULL;
    while (valid) {
        bool available = false;
        if (!trace_info_read_line(file, line, &available)) {
            valid = false;
            break;
        }
        if (!available) break;
        if (strncmp(line, "cookie-seed-", 12) != 0) continue;
        char *equals = strchr(line, '=');
        if (equals == NULL) {
            valid = false;
            break;
        }
        *equals++ = '\0';
        if (strcmp(line, "cookie-seed-version") == 0
            || strcmp(line, "cookie-seed-count") == 0) continue;
        size_t index = 0;
        TraceSeedField field;
        if (!trace_seed_index_and_field(line, &index, &field)
            || index >= seed_count) {
            valid = false;
            break;
        }
        uint16_t bit = (uint16_t) (1u << (unsigned) field);
        if ((fields_seen[index] & bit) != 0) {
            valid = false;
            break;
        }
        fields_seen[index] |= bit;
        BrowserCookieSeedEntry *entry = &entries[index];
        size_t parsed = 0;
        int parsed_bool = 0;
        switch (field) {
        case TRACE_SEED_DOMAIN:
            valid = trace_copy_value(
                entry->domain, sizeof(entry->domain), equals);
            break;
        case TRACE_SEED_PATH:
            valid = trace_copy_value(entry->path, sizeof(entry->path), equals);
            break;
        case TRACE_SEED_NAME:
            valid = trace_copy_value(entry->name, sizeof(entry->name), equals);
            break;
        case TRACE_SEED_VALUE_LENGTH:
            valid = trace_parse_size_value(equals, &parsed)
                && parsed <= session->maximum_cookie_bytes;
            if (valid) entry->value_length = parsed;
            break;
        case TRACE_SEED_HOST_ONLY:
            valid = trace_parse_bool_value(equals, &parsed_bool);
            if (valid) entry->host_only = parsed_bool != 0;
            break;
        case TRACE_SEED_SECURE:
            valid = trace_parse_bool_value(equals, &parsed_bool);
            if (valid) entry->secure = parsed_bool != 0;
            break;
        case TRACE_SEED_HTTP_ONLY:
            valid = trace_parse_bool_value(equals, &parsed_bool);
            if (valid) entry->http_only = parsed_bool != 0;
            break;
        case TRACE_SEED_SAME_SITE:
            valid = trace_parse_size_value(equals, &parsed)
                && parsed <= BROWSER_COOKIE_SAME_SITE_NONE;
            if (valid) entry->same_site = (BrowserCookieSameSite) parsed;
            break;
        case TRACE_SEED_PARTITIONED:
            valid = trace_parse_bool_value(equals, &parsed_bool);
            if (valid) entry->partitioned = parsed_bool != 0;
            break;
        case TRACE_SEED_PARTITION_KEY:
            valid = trace_copy_value(
                entry->partition_key, sizeof(entry->partition_key), equals);
            break;
        case TRACE_SEED_CREATION_SEQUENCE:
            valid = trace_parse_size_value(equals, &parsed)
                && parsed != 0 && parsed != SIZE_MAX;
            if (valid) entry->creation_sequence = parsed;
            break;
        case TRACE_SEED_FIELD_COUNT:
            valid = false;
            break;
        }
    }
    if (file != NULL && fclose(file) != 0) valid = false;
    const uint16_t all_fields =
        (uint16_t) ((1u << TRACE_SEED_FIELD_COUNT) - 1u);
    for (size_t i = 0; valid && i < seed_count; i++) {
        if (fields_seen[i] != all_fields) valid = false;
    }
    bool imported = valid
        && (browser_session_cookie_import_redacted_seed(
                session, entries, seed_count)
            || trace_session_matches_seed(session, entries, seed_count));
    budget_free(session->budget, entries);
    if (imported) return true;

invalid:
    if (error != NULL && error_size != 0) {
        snprintf(error, error_size,
                 "HTTP replay cookie seed is invalid or session changed");
    }
    return false;
}

bool fetch_trace_replay_active(void)
{
    return fetch_trace.mode == FETCH_TRACE_REPLAY;
}

bool fetch_trace_replay_stats(FetchTraceReplayStats *stats)
{
    if (stats == NULL || fetch_trace.mode != FETCH_TRACE_REPLAY) return false;
    size_t claimed = 0;
    for (size_t sequence = 0;
         sequence < fetch_trace.replay_record_count; sequence++) {
        if (trace_replay_record_claimed(sequence)) claimed++;
    }
    *stats = (FetchTraceReplayStats) {
        .response_keyed = fetch_trace.replay_response_keyed,
        .record_count = fetch_trace.replay_record_count,
        .claimed_record_count = claimed,
        .request_count = fetch_trace.replay_request_count,
        .matched_request_count = fetch_trace.replay_matched_request_count,
        .served_request_count = fetch_trace.replay_served_request_count,
        .rejected_request_count = fetch_trace.replay_rejected_request_count,
        .unmatched_request_count = fetch_trace.replay_unmatched_request_count,
        .conflicting_request_count =
            fetch_trace.replay_conflicting_request_count,
        .invalid_route_request_count =
            fetch_trace.replay_invalid_route_request_count,
        .request_shape_mismatch_count =
            fetch_trace.replay_request_shape_mismatch_count,
        .occurrence_claim_count =
            fetch_trace.replay_occurrence_claim_count,
        .reusable_claim_count =
            fetch_trace.replay_reusable_claim_count,
        .occurrence_exhausted_count =
            fetch_trace.replay_occurrence_exhausted_count
    };
    return true;
}

bool fetch_trace_replay_record_was_claimed(size_t sequence)
{
    return fetch_trace.mode == FETCH_TRACE_REPLAY
        && sequence < fetch_trace.replay_record_count
        && trace_replay_record_claimed(sequence);
}

void fetch_trace_end(void)
{
    if (fetch_trace.mode == FETCH_TRACE_CAPTURE
        && !trace_capture_finalize()) {
        char info_path[FETCH_TRACE_PATH_CAPACITY];
        char partial_path[FETCH_TRACE_PATH_CAPACITY];
        if (trace_info_path(info_path)) (void) remove(info_path);
        if (trace_info_partial_path(partial_path)) {
            (void) remove(partial_path);
        }
        fprintf(stderr,
                "HTTP trace finalization failed; replay authority removed\n");
    }
    free(fetch_trace.replay_route_index);
    memset(&fetch_trace, 0, sizeof(fetch_trace));
}

bool fetch_trace_clock_origin_ms(uint64_t *origin_ms)
{
    if (origin_ms == NULL || fetch_trace.mode == FETCH_TRACE_OFF
        || fetch_trace.clock_origin_ms == 0) return false;
    *origin_ms = fetch_trace.clock_origin_ms;
    return true;
}

static bool trace_request_is_top_level_document(const FetchRequest *request,
                                                const char *canonical_url)
{
    if (request == NULL || canonical_url == NULL) return false;
    if (request->cookie_context != NULL) {
        const TilefinchRequestContext *context = request->cookie_context;
        char target[TILEFINCH_URL_SERIALIZED_LIMIT];
        const char *method = request->method == NULL ? "GET" : request->method;
        TilefinchCredentialsMode credentials =
            request->credentials == FETCH_CREDENTIALS_OMIT
                ? TILEFINCH_CREDENTIALS_OMIT
                : (request->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
                      ? TILEFINCH_CREDENTIALS_SAME_ORIGIN
                      : TILEFINCH_CREDENTIALS_INCLUDE);
        return context->top_level_navigation
            && context->mode == TILEFINCH_REQUEST_MODE_NAVIGATE
            && context->destination == TILEFINCH_DESTINATION_DOCUMENT
            && tilefinch_url_request_key(
                   context->target_url, target, sizeof(target))
            && strcmp(target, canonical_url) == 0
            && strcasecmp(context->method == NULL ? "GET" : context->method,
                          method) == 0
            && context->credentials == credentials;
    }
    /* Context-free embedders retain a bounded structured fallback. Both
       browser-owned Fetch Metadata fields must identify a navigation; a page
       cannot inject either field through extra_headers. */
    return request->sec_fetch_dest != NULL
        && request->sec_fetch_mode != NULL
        && strcasecmp(request->sec_fetch_dest, "document") == 0
        && strcasecmp(request->sec_fetch_mode, "navigate") == 0;
}

static bool trace_activate_for_request(const FetchRequest *request,
                                       const char *canonical_url)
{
    if (fetch_trace.mode != FETCH_TRACE_CAPTURE_ARMED
        || !trace_request_is_top_level_document(
               request, canonical_url)) return true;
    if (fetch_trace.top_level_requests_seen == SIZE_MAX) {
        fetch_trace_end();
        return false;
    }
    fetch_trace.top_level_requests_seen++;
    if (fetch_trace.top_level_requests_seen
        != fetch_trace.top_level_request_ordinal) return true;
    char error[256] = {0};
    if (trace_capture_activate(request->cookie_session, true,
                               error, sizeof(error))) return true;
    fprintf(stderr, "delayed HTTP trace activation failed: %s\n",
            error[0] == '\0' ? "unknown metadata error" : error);
    return false;
}

static void *curl_malloc_hook(size_t size)
{
    return budget_concurrent_pool_malloc(&curl_concurrent_pool, size);
}
