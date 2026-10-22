/* Shared native signing and secure release for RAM-only provider requests. */
static void lite_secure_wipe(void *data, size_t length)
{
    volatile unsigned char *at = data;
    while (length-- != 0) *at++ = 0;
}

static void lite_secure_release(FetchResult *response)
{
    if (response->data != NULL && response->shared_body == NULL)
        lite_secure_wipe(response->data, response->length);
    fetch_result_destroy(response);
}

/* Small bounded SHA-1 adapter for the service's request signature, not a
   security digest for cache or update integrity. No copied extractor code. */
static uint32_t lite_sha1_rotate(uint32_t word, unsigned bits)
{
    return (word << bits) | (word >> (32u - bits));
}

static void lite_sha1_hex(const unsigned char *bytes, size_t length,
                          char hex[41])
{
    uint32_t state[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu,
                         0x10325476u, 0xc3d2e1f0u};
    size_t blocks = (length + 9u + 63u) / 64u;
    uint64_t bit_length = (uint64_t) length * 8u;
    for (size_t block = 0; block < blocks; ++block) {
        uint32_t words[80] = {0};
        for (size_t i = 0; i < 64u; ++i) {
            size_t at = block * 64u + i;
            unsigned byte = at < length ? bytes[at] : at == length ? 0x80u : 0u;
            if (at >= blocks * 64u - 8u)
                byte = (unsigned) (bit_length >> ((blocks * 64u - 1u - at) * 8u)) & 255u;
            words[i / 4u] |= (uint32_t) byte << ((3u - i % 4u) * 8u);
        }
        for (unsigned i = 16; i < 80u; ++i)
            words[i] = lite_sha1_rotate(words[i-3] ^ words[i-8]
                                         ^ words[i-14] ^ words[i-16], 1);
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (unsigned i = 0; i < 80u; ++i) {
            uint32_t f = i < 20u ? (b & c) | (~b & d)
                : i < 40u ? b ^ c ^ d
                : i < 60u ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            uint32_t k = i < 20u ? 0x5a827999u : i < 40u ? 0x6ed9eba1u
                : i < 60u ? 0x8f1bbcdcu : 0xca62c1d6u;
            uint32_t next = lite_sha1_rotate(a, 5) + f + e + k + words[i];
            e = d; d = c; c = lite_sha1_rotate(b, 30); b = a; a = next;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
        lite_secure_wipe(words, sizeof(words));
    }
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 40u; ++i)
        hex[i] = digits[(state[i / 8u] >> ((7u - i % 8u) * 4u)) & 15u];
    hex[40] = '\0';
    lite_secure_wipe(state, sizeof(state));
}

/* The account cookie this request would send: SAPISID, else
   __Secure-3PAPISID. Read from the jar, so it never depends on the whole
   Cookie header fitting a buffer. The one reader for both the signed-in
   identity and the request signature, so the two always come from the same
   cookie. */
static bool lite_session_sapisid(const BrowserSession *session,
                                 const TilefinchRequestContext *context,
                                 const char **value, size_t *length)
{
    if (!browser_session_cookie_value_context(session, context, "SAPISID",
                                              value, length))
        (void) browser_session_cookie_value_context(
            session, context, "__Secure-3PAPISID", value, length);
    return *value != NULL && *length != 0u;
}

static bool lite_feed_authorization(BrowserSession *session,
    const TilefinchRequestContext *context, uint64_t seconds,
    char *output, size_t capacity)
{
    if (output == NULL || capacity == 0u) return false;
    output[0] = '\0';
    if (session == NULL || context == NULL || seconds == 0u
        || context->initiator_opaque || context->mode != TILEFINCH_REQUEST_MODE_CORS
        || !tilefinch_url_same_origin(context->target_url, "https://m.youtube.com")
        || !tilefinch_url_same_origin(context->initiator_url, context->target_url))
        return false;
    char message[4352] = {0}, digest[41] = {0}, signature[96] = {0};
    const char *sid = NULL;
    size_t sid_length = 0;
    bool okay = lite_session_sapisid(session, context, &sid, &sid_length);
    int count = okay ? snprintf(message, sizeof(message), "%llu %.*s https://m.youtube.com",
        (unsigned long long) seconds, (int) sid_length, sid) : -1;
    okay = okay && count > 0 && (size_t) count < sizeof(message);
    if (okay) {
        lite_sha1_hex((const unsigned char *) message, (size_t) count, digest);
        count = snprintf(signature, sizeof(signature), "SAPISIDHASH %llu_%s",
                         (unsigned long long) seconds, digest);
        okay = count > 0 && (size_t) count < sizeof(signature) && (size_t) count < capacity;
        if (okay) memcpy(output, signature, (size_t) count + 1u);
    }
    lite_secure_wipe(message, sizeof(message));
    lite_secure_wipe(digest, sizeof(digest)); lite_secure_wipe(signature, sizeof(signature));
    return okay;
}
