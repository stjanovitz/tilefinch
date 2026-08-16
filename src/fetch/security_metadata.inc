static bool security_name_is(const char *name, size_t length,
                             const char *expected)
{
    return name != NULL && expected != NULL && strlen(expected) == length
        && strncasecmp(name, expected, length) == 0;
}

static bool referrer_policy_known(const char *value, size_t length)
{
    static const char *const known[] = {
        "no-referrer", "no-referrer-when-downgrade", "origin",
        "origin-when-cross-origin", "same-origin", "strict-origin",
        "strict-origin-when-cross-origin", "unsafe-url"
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strlen(known[i]) == length
            && strncasecmp(value, known[i], length) == 0) return true;
    }
    return false;
}

static bool referrer_policy_normalized(const char *value)
{
    if (value == NULL) return false;
    size_t length = strlen(value);
    if (length == 0) return true;
    if (!referrer_policy_known(value, length)) return false;
    for (size_t i = 0; i < length; i++) {
        if (isupper((unsigned char) value[i])) return false;
    }
    return true;
}

static bool update_referrer_policy_tokens(
    const char *header, size_t length,
    char output[FETCH_REFERRER_POLICY_LIMIT])
{
    if (header == NULL || output == NULL) return false;
    bool updated = false;
    const char *at = header;
    const char *limit = header + length;
    while (at < limit) {
        const char *separator = memchr(at, ',', (size_t) (limit - at));
        const char *end = separator == NULL ? limit : separator;
        while (at < end && isspace((unsigned char) *at)) at++;
        while (end > at && isspace((unsigned char) end[-1])) end--;
        size_t token_length = (size_t) (end - at);
        if (token_length < FETCH_REFERRER_POLICY_LIMIT
            && referrer_policy_known(at, token_length)) {
            for (size_t i = 0; i < token_length; i++) {
                output[i] = (char) tolower((unsigned char) at[i]);
            }
            output[token_length] = '\0';
            updated = true;
        }
        at = separator == NULL ? limit : separator + 1;
    }
    return updated;
}

static bool security_hsts_token_character(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || c == '!' || c == '#' || c == '$' || c == '%'
        || c == '&' || c == '\'' || c == '*' || c == '+' || c == '-'
        || c == '.' || c == '^' || c == '_' || c == '`' || c == '|'
        || c == '~';
}

static bool security_hsts_quoted_character(unsigned char c)
{
    return c == '\t' || c == ' ' || c == 0x21u
        || (c >= 0x23u && c <= 0x5bu)
        || (c >= 0x5du && c <= 0x7eu) || c >= 0x80u;
}

static bool security_hsts_delta_seconds(
    const char *start, const char *end, bool quoted, uint64_t *seconds)
{
    if (start == NULL || end == NULL || seconds == NULL || start >= end) {
        return false;
    }
    uint64_t value = 0;
    for (const char *at = start; at < end; at++) {
        unsigned char c = (unsigned char) *at;
        if (quoted && c == '\\') {
            if (++at >= end) return false;
            c = (unsigned char) *at;
        }
        if (c < '0' || c > '9') return false;
        unsigned digit = (unsigned) (c - '0');
        value = value > (UINT64_MAX - digit) / 10u
            ? UINT64_MAX : value * 10u + digit;
    }
    *seconds = value;
    return true;
}

static bool security_hsts_field(
    const char *start, const char *end, uint64_t *max_age,
    bool *include_subdomains)
{
#define FETCH_HSTS_DIRECTIVE_LIMIT 32u
    bool saw_max_age = false;
    bool saw_include_subdomains = false;
    const char *names[FETCH_HSTS_DIRECTIVE_LIMIT];
    size_t name_lengths[FETCH_HSTS_DIRECTIVE_LIMIT];
    size_t name_count = 0;
    const char *at = start;
    while (at < end) {
        while (at < end && (*at == ' ' || *at == '\t')) at++;
        if (at == end) break;
        if (*at == ';') { at++; continue; }
        const char *name = at;
        while (at < end
               && security_hsts_token_character((unsigned char) *at)) at++;
        size_t name_length = (size_t) (at - name);
        if (name_length == 0 || name_count == FETCH_HSTS_DIRECTIVE_LIMIT) {
            return false;
        }
        for (size_t i = 0; i < name_count; i++) {
            if (name_lengths[i] == name_length
                && strncasecmp(names[i], name, name_length) == 0) {
                return false;
            }
        }
        names[name_count] = name;
        name_lengths[name_count++] = name_length;
        while (at < end && (*at == ' ' || *at == '\t')) at++;

        bool has_value = at < end && *at == '=';
        bool quoted = false;
        const char *value_start = NULL;
        const char *value_end = NULL;
        if (has_value) {
            at++;
            while (at < end && (*at == ' ' || *at == '\t')) at++;
            if (at >= end) return false;
            if (*at == '"') {
                quoted = true;
                value_start = ++at;
                while (at < end && *at != '"') {
                    unsigned char c = (unsigned char) *at;
                    if (c == '\\') {
                        at++;
                        if (at >= end
                            || !security_hsts_quoted_character(
                                   (unsigned char) *at)) return false;
                    } else if (!security_hsts_quoted_character(c)) {
                        return false;
                    }
                    at++;
                }
                if (at >= end) return false;
                value_end = at++;
            } else {
                value_start = at;
                while (at < end
                       && security_hsts_token_character(
                              (unsigned char) *at)) at++;
                value_end = at;
                if (value_start == value_end) return false;
            }
            while (at < end && (*at == ' ' || *at == '\t')) at++;
        }
        if (at < end && *at != ';') return false;

        if (security_name_is(name, name_length, "max-age")) {
            if (saw_max_age || !has_value
                || !security_hsts_delta_seconds(
                       value_start, value_end, quoted, max_age)) return false;
            saw_max_age = true;
        } else if (security_name_is(
                       name, name_length, "includesubdomains")) {
            if (saw_include_subdomains || has_value) return false;
            saw_include_subdomains = true;
        }
        if (at < end) at++;
    }
    *include_subdomains = saw_include_subdomains;
#undef FETCH_HSTS_DIRECTIVE_LIMIT
    return saw_max_age;
}

void fetch_response_security_metadata_reset(
    FetchResponseSecurityMetadata *metadata)
{
    if (metadata == NULL) return;
    memset(metadata, 0, sizeof(*metadata));
    metadata->version = FETCH_RESPONSE_SECURITY_METADATA_VERSION;
}

static FetchSecurityFieldState security_duplicate_state(
    FetchSecurityFieldState state)
{
    return state == FETCH_SECURITY_FIELD_ABSENT
        ? FETCH_SECURITY_FIELD_VALID : FETCH_SECURITY_FIELD_DUPLICATE;
}

bool fetch_response_security_metadata_collect(
    FetchResponseSecurityMetadata *metadata,
    const char *name, size_t name_length,
    const char *value, size_t value_length, bool snapshot_stored)
{
    if (metadata == NULL || metadata->version
            != FETCH_RESPONSE_SECURITY_METADATA_VERSION
        || name == NULL || value == NULL) return false;

    if (security_name_is(name, name_length,
                         "access-control-allow-origin")) {
        FetchSecurityFieldState next = security_duplicate_state(
            metadata->allow_origin_state);
        if (next == FETCH_SECURITY_FIELD_DUPLICATE) {
            metadata->allow_origin_state = next;
            metadata->allow_origin[0] = '\0';
            return true;
        }
        char canonical[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
        bool valid = value_length != 0
            && value_length < sizeof(metadata->allow_origin);
        if (valid) {
            memcpy(metadata->allow_origin, value, value_length);
            metadata->allow_origin[value_length] = '\0';
            valid = strcmp(metadata->allow_origin, "*") == 0
                || strcmp(metadata->allow_origin, "null") == 0
                || (tilefinch_url_origin(
                        metadata->allow_origin, canonical,
                        sizeof(canonical))
                    && strcmp(canonical, metadata->allow_origin) == 0);
        }
        metadata->allow_origin_state = valid
            ? FETCH_SECURITY_FIELD_VALID : FETCH_SECURITY_FIELD_INVALID;
        if (!valid) metadata->allow_origin[0] = '\0';
        return true;
    }
    if (security_name_is(name, name_length,
                         "access-control-allow-credentials")) {
        FetchSecurityFieldState next = security_duplicate_state(
            metadata->allow_credentials_state);
        if (next == FETCH_SECURITY_FIELD_DUPLICATE) {
            metadata->allow_credentials_state = next;
            metadata->allow_credentials = false;
            return true;
        }
        metadata->allow_credentials = value_length == 4u
            && memcmp(value, "true", 4u) == 0;
        metadata->allow_credentials_state = metadata->allow_credentials
            ? FETCH_SECURITY_FIELD_VALID : FETCH_SECURITY_FIELD_INVALID;
        return true;
    }
    if (security_name_is(name, name_length,
                         "cross-origin-resource-policy")) {
        FetchSecurityFieldState next = security_duplicate_state(
            metadata->corp_state);
        if (next == FETCH_SECURITY_FIELD_DUPLICATE) {
            metadata->corp_state = next;
            metadata->corp = TILEFINCH_CORP_UNSPECIFIED;
            return true;
        }
        if (value_length == 11u
            && strncasecmp(value, "same-origin", 11u) == 0) {
            metadata->corp = TILEFINCH_CORP_SAME_ORIGIN;
        } else if (value_length == 9u
                   && strncasecmp(value, "same-site", 9u) == 0) {
            metadata->corp = TILEFINCH_CORP_SAME_SITE;
        } else if (value_length == 12u
                   && strncasecmp(value, "cross-origin", 12u) == 0) {
            metadata->corp = TILEFINCH_CORP_CROSS_ORIGIN;
        } else {
            metadata->corp_state = FETCH_SECURITY_FIELD_INVALID;
            return true;
        }
        metadata->corp_state = FETCH_SECURITY_FIELD_VALID;
        return true;
    }
    if (security_name_is(name, name_length, "x-content-type-options")) {
        FetchSecurityFieldState next = security_duplicate_state(
            metadata->nosniff_state);
        if (next == FETCH_SECURITY_FIELD_DUPLICATE) {
            metadata->nosniff_state = next;
            metadata->nosniff = false;
            return true;
        }
        metadata->nosniff = value_length == 7u
            && strncasecmp(value, "nosniff", 7u) == 0;
        metadata->nosniff_state = metadata->nosniff
            ? FETCH_SECURITY_FIELD_VALID : FETCH_SECURITY_FIELD_INVALID;
        return true;
    }
    if (security_name_is(name, name_length, "strict-transport-security")) {
        /* RFC 6797 deliberately processes only the first field. */
        if (metadata->hsts_state != FETCH_SECURITY_FIELD_ABSENT) return true;
        metadata->hsts_state = security_hsts_field(
            value, value + value_length, &metadata->hsts_max_age,
            &metadata->hsts_include_subdomains)
            ? FETCH_SECURITY_FIELD_VALID : FETCH_SECURITY_FIELD_INVALID;
        return true;
    }
    if (security_name_is(name, name_length, "referrer-policy")) {
        metadata->referrer_policy_state = FETCH_SECURITY_FIELD_VALID;
        (void) update_referrer_policy_tokens(
            value, value_length, metadata->referrer_policy);
        return true;
    }
    if (security_name_is(name, name_length, "content-security-policy")) {
        if (!snapshot_stored) {
            metadata->csp_snapshot_state = FETCH_SECURITY_FIELD_TRUNCATED;
            metadata->snapshot_truncated = true;
        } else if (metadata->csp_snapshot_state
                       == FETCH_SECURITY_FIELD_ABSENT) {
            metadata->csp_snapshot_state = FETCH_SECURITY_FIELD_VALID;
        }
        return true;
    }
    if (security_name_is(name, name_length, "x-frame-options")) {
        if (!snapshot_stored) {
            metadata->frame_policy_snapshot_state =
                FETCH_SECURITY_FIELD_TRUNCATED;
            metadata->snapshot_truncated = true;
        } else if (metadata->frame_policy_snapshot_state
                       == FETCH_SECURITY_FIELD_ABSENT) {
            metadata->frame_policy_snapshot_state =
                FETCH_SECURITY_FIELD_VALID;
        }
        return true;
    }
    return false;
}

bool fetch_response_security_metadata_from_snapshot(
    FetchResponseSecurityMetadata *metadata,
    const char *headers, size_t headers_length, bool snapshot_truncated)
{
    if (metadata == NULL || (headers == NULL && headers_length != 0)) {
        return false;
    }
    fetch_response_security_metadata_reset(metadata);
    for (size_t offset = 0; offset < headers_length;) {
        const char *line = headers + offset;
        const char *newline = memchr(line, '\n', headers_length - offset);
        size_t length = newline == NULL ? headers_length - offset
                                        : (size_t) (newline - line);
        const char *colon = memchr(line, ':', length);
        if (colon != NULL && colon != line) {
            const char *value = colon + 1;
            const char *end = line + length;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            while (end > value && (end[-1] == '\r' || end[-1] == ' '
                                   || end[-1] == '\t')) end--;
            (void) fetch_response_security_metadata_collect(
                metadata, line, (size_t) (colon - line), value,
                (size_t) (end - value), true);
        }
        offset += length + (newline == NULL ? 0u : 1u);
    }
    if (snapshot_truncated) {
        metadata->snapshot_truncated = true;
#define MARK_UNKNOWN_TRUNCATED(field) do {                                  \
    if ((field) == FETCH_SECURITY_FIELD_ABSENT) {                           \
        (field) = FETCH_SECURITY_FIELD_TRUNCATED;                           \
    }                                                                       \
} while (0)
        MARK_UNKNOWN_TRUNCATED(metadata->allow_origin_state);
        MARK_UNKNOWN_TRUNCATED(metadata->allow_credentials_state);
        MARK_UNKNOWN_TRUNCATED(metadata->corp_state);
        MARK_UNKNOWN_TRUNCATED(metadata->nosniff_state);
        MARK_UNKNOWN_TRUNCATED(metadata->hsts_state);
        MARK_UNKNOWN_TRUNCATED(metadata->csp_snapshot_state);
        MARK_UNKNOWN_TRUNCATED(metadata->frame_policy_snapshot_state);
#undef MARK_UNKNOWN_TRUNCATED
    }
    return true;
}
