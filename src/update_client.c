#include "tilefinch/update.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/sha256.h"
#include "tilefinch/url.h"

#define UPDATE_METADATA_TIMEOUT_MS 30000L
#define UPDATE_PACKAGE_TIMEOUT_MS 120000L
/* FetchScheduler's byte bound is the response-integrity ceiling, even for a
   streaming consumer. The updater has one request slot and writes package
   chunks directly to its transactional .part file, so admitting the signed
   32 MiB format limit does not allocate or retain a 32 MiB response buffer. */
#define UPDATE_SCHEDULER_RESERVE TILEFINCH_UPDATE_MAX_PACKAGE_BYTES
#define UPDATE_PUMP_BYTES (16u * 1024u)
#define UPDATE_FREE_SPACE_MARGIN (4u * 1024u * 1024u)

struct TilefinchUpdateClient {
    Budget *budget;
    FetchScheduler *scheduler;
    TilefinchUpdateRoot embedded_root;
    uint64_t installed_sequence;
    uint8_t installed_sha256[32];
    bool installed_sequence_valid;
    bool installed_pair_valid;
    uint16_t launcher_protocol;
    char owner[64];
    char repository[64];
    char part_path[768];
    char metadata_url[768];
    char package_url[768];
    char metadata_asset[96];
    char release_tag[64];
    bool package_relative_to_metadata;
    bool package_url_override;
    bool custom_endpoint;
    bool signed_endpoint_override;
    bool allow_downgrade;
    TilefinchUpdateTrust trust;
    TilefinchUpdateArtifact artifact;
    uint64_t request_id;
    bool request_is_download;
    TilefinchUpdateClientPhase phase;
    TilefinchUpdateStatus status;
    TilefinchUpdateVerifiedEnvelope verified;
    uint8_t *envelope;
    size_t envelope_length;
    uint64_t now_unix;
    bool clock_valid;
    FILE *part;
    TilefinchSha256 package_sha;
    uint64_t package_bytes;
    char message[96];
};

static bool update_identifier(const char *value, size_t capacity)
{
    if (value == NULL) return false;
    size_t length = strlen(value);
    if (length == 0 || length >= capacity) return false;
    for (size_t index = 0; index < length; index++) {
        unsigned character = (unsigned char) value[index];
        if (!((character >= 'a' && character <= 'z')
              || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9')
              || character == '-' || character == '_'
              || character == '.')) return false;
    }
    return true;
}

bool tilefinch_update_url_is_valid(const char *url, size_t capacity)
{
    if (url == NULL || capacity < 16u) return false;
    size_t length = strnlen(url, capacity);
    /*
     * https only. This predicate is the entire endpoint policy for the
     * developer channel -- the only channel whose URLs are configurable --
     * and that channel is the one whose payload carries no signature. A
     * cleartext endpoint would let anything on the path substitute the
     * package outright, with nothing downstream able to notice. It also
     * used to disable the per-hop redirect validator, which only engages
     * for https, so plain http quietly lost two protections at once.
     *
     * Stable and Beta are unaffected: their endpoints are built here as
     * fixed GitHub https URLs and never pass through this check.
     */
    if (length == 0 || length >= capacity
        || strncmp(url, "https://", 8) != 0) return false;
    const char *authority = url + 8u;
    const char *path = strchr(authority, '/');
    if (path == NULL || path == authority || path[1] == '\0') return false;
    /* Credentials, fragments, backslashes and control/space bytes make an
       endpoint ambiguous across curl versions. Updates never need them. */
    for (size_t index = 0; index < length; index++) {
        unsigned character = (unsigned char) url[index];
        if (character <= 0x20u || character == 0x7fu
            || character == '\\' || character == '#'
            || character == '@') return false;
    }
    return true;
}

static bool update_url_host_matches(
    const char *url, const char *name, bool allow_subdomains)
{
    const char *host = strstr(url, "://");
    if (host == NULL) return false;
    host += 3;
    const char *end = strchr(host, '/');
    if (end == NULL) return false;
    const char *port = memchr(host, ':', (size_t) (end - host));
    if (port != NULL) end = port;
    size_t host_length = (size_t) (end - host);
    size_t name_length = strlen(name);
    if (host_length == name_length
        && strncasecmp(host, name, name_length) == 0) return true;
    return allow_subdomains && host_length > name_length
        && host[host_length - name_length - 1u] == '.'
        && strncasecmp(
               host + host_length - name_length,
               name, name_length) == 0;
}

static bool update_url_is_onedrive_share(const char *url)
{
    return update_url_host_matches(url, "1drv.ms", false)
        || update_url_host_matches(url, "onedrive.live.com", false)
        || update_url_host_matches(url, "sharepoint.com", true);
}

bool tilefinch_update_prepare_download_url(
    const char *url, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0
        || !tilefinch_update_url_is_valid(url, capacity)) return false;
    size_t length = strlen(url);
    if (!update_url_is_onedrive_share(url)) {
        if (length >= capacity) return false;
        memcpy(output, url, length + 1u);
        return true;
    }

    /* Public OneDrive links default to a viewer. Download mode is a query
       parameter, and the resulting Microsoft/CDN redirects remain subject
       to the transport's global eight-hop bound and no-credentials policy. */
    const char *query = strchr(url, '?');
    const char *scan = query == NULL ? url + length : query + 1u;
    while (*scan != '\0') {
        const char *end = strchr(scan, '&');
        if (end == NULL) end = url + length;
        const char *equals = memchr(scan, '=', (size_t) (end - scan));
        size_t key_length = (size_t) ((equals == NULL ? end : equals) - scan);
        if (key_length == 8u && strncasecmp(scan, "download", 8u) == 0) {
            size_t prefix = (size_t) (scan - url);
            size_t suffix = length - (size_t) (end - url);
            static const char replacement[] = "download=1";
            if (prefix + sizeof(replacement) - 1u + suffix >= capacity)
                return false;
            memcpy(output, url, prefix);
            memcpy(output + prefix, replacement, sizeof(replacement) - 1u);
            memcpy(output + prefix + sizeof(replacement) - 1u,
                   end, suffix + 1u);
            return true;
        }
        scan = *end == '\0' ? end : end + 1u;
    }
    const bool already_separated = length != 0
        && (url[length - 1u] == '?' || url[length - 1u] == '&');
    const char separator = query == NULL ? '?' : '&';
    static const char parameter[] = "download=1";
    size_t separator_length = already_separated ? 0u : 1u;
    if (length + separator_length + sizeof(parameter) > capacity)
        return false;
    memcpy(output, url, length);
    if (!already_separated) output[length] = separator;
    memcpy(output + length + separator_length,
           parameter, sizeof(parameter));
    return true;
}

static bool update_https_redirect_url_allowed(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8u) == 0
        && tilefinch_update_url_is_valid(
               url, TILEFINCH_URL_SERIALIZED_LIMIT);
}

static void update_client_error(
    TilefinchUpdateClient *client, TilefinchUpdateStatus status,
    const char *message)
{
    client->phase = TILEFINCH_UPDATE_CLIENT_ERROR;
    client->status = status;
    snprintf(client->message, sizeof(client->message), "%s",
             message == NULL ? tilefinch_update_status_name(status) : message);
}

static FetchRequest update_request(
    const TilefinchUpdateClient *client, const char *url)
{
    bool alternate_endpoint = client != NULL
        && (client->custom_endpoint || client->signed_endpoint_override);
    bool onedrive = client != NULL && client->custom_endpoint
        && update_url_is_onedrive_share(url);
    bool https = alternate_endpoint
        && url != NULL && strncmp(url, "https://", 8u) == 0;
    return (FetchRequest) {
        .method = "GET",
        .accept = "application/octet-stream",
        .credentials = FETCH_CREDENTIALS_OMIT,
        .identity_encoding = true,
        .redirect_policy = alternate_endpoint
            ? FETCH_REDIRECT_DEFAULT : FETCH_REDIRECT_GITHUB_RELEASE,
        .redirect_same_origin_only =
            alternate_endpoint && !onedrive,
        .redirect_url_validator =
            https ? update_https_redirect_url_allowed : NULL,
        .user_agent = "Tilefinch-Updater/1"
    };
}

static bool update_free_space(
    const char *path, uint64_t *available)
{
    if (path == NULL || available == NULL) return false;
    char directory[768];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(directory)) return false;
    memcpy(directory, path, length + 1u);
    char *slash = strrchr(directory, '/');
    if (slash != NULL && slash != directory) *slash = '\0';
    return tilefinch_update_query_free_space(directory, available);
}

TilefinchUpdateClient *tilefinch_update_client_create(
    Budget *budget, const TilefinchUpdateClientOptions *options)
{
    if (budget == NULL || options == NULL
        || options->trust > TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
        || options->artifact > TILEFINCH_UPDATE_ARTIFACT_GLYPH_COMPONENT
        || (options->artifact != TILEFINCH_UPDATE_ARTIFACT_BROWSER
            && options->trust != TILEFINCH_UPDATE_TRUST_SIGNED)
        || (options->artifact == TILEFINCH_UPDATE_ARTIFACT_GLYPH_COMPONENT
            && !update_identifier(options->metadata_asset, 96))
        || (options->trust == TILEFINCH_UPDATE_TRUST_SIGNED
            && options->embedded_root == NULL)
        || (options->trust != TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
            && options->package_url_override != NULL)
        || (options->release_tag != NULL
            && (options->artifact != TILEFINCH_UPDATE_ARTIFACT_BROWSER
                || options->trust != TILEFINCH_UPDATE_TRUST_SIGNED
                || options->metadata_url_override != NULL
                || !update_identifier(options->release_tag, 64)))
        || (options->allow_downgrade
            && (options->release_tag == NULL
                || options->trust != TILEFINCH_UPDATE_TRUST_SIGNED))
        || (options->trust == TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
            && (options->metadata_url_override == NULL
                || (!options->package_relative_to_metadata
                    && options->package_url_override == NULL)))
        || !update_identifier(options->repository_owner, 64)
        || !update_identifier(options->repository_name, 64)
        || (options->metadata_url_override != NULL
            && !tilefinch_update_url_is_valid(
                   options->metadata_url_override,
                   sizeof(((TilefinchUpdateClient *) 0)->metadata_url)))
        || (options->package_url_override != NULL
            && !tilefinch_update_url_is_valid(
                   options->package_url_override,
                   sizeof(((TilefinchUpdateClient *) 0)->package_url)))
        || options->package_part_path == NULL
        || strlen(options->package_part_path) >= 768) return NULL;
    TilefinchUpdateClient *client = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*client));
    if (client == NULL) return NULL;
    client->budget = budget;
    if (options->embedded_root != NULL)
        client->embedded_root = *options->embedded_root;
    client->trust = options->trust;
    client->artifact = options->artifact;
    if (options->metadata_asset != NULL)
        snprintf(client->metadata_asset, sizeof(client->metadata_asset),
                 "%s", options->metadata_asset);
    if (options->release_tag != NULL)
        snprintf(client->release_tag, sizeof(client->release_tag),
                 "%s", options->release_tag);
    client->allow_downgrade = options->allow_downgrade;
    client->installed_sequence = options->installed_sequence;
    client->installed_sequence_valid = options->installed_sequence_valid;
    client->installed_pair_valid = options->installed_pair_valid;
    client->launcher_protocol = options->launcher_protocol;
    if (options->installed_package_sha256 != NULL)
        memcpy(client->installed_sha256,
               options->installed_package_sha256, 32);
    snprintf(client->owner, sizeof(client->owner), "%s",
             options->repository_owner);
    snprintf(client->repository, sizeof(client->repository), "%s",
             options->repository_name);
    snprintf(client->part_path, sizeof(client->part_path), "%s",
             options->package_part_path);
    client->package_relative_to_metadata =
        options->package_relative_to_metadata;
    client->package_url_override = options->package_url_override != NULL;
    client->custom_endpoint =
        options->trust == TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED;
    /* A validation Stable endpoint is still signed release metadata.  It is
       alternate only for transport policy: it must not inherit GitHub's
       hostname allowlist, and every redirect must remain on its HTTPS
       origin.  Trust, downgrade, digest, journal and trial policy remain the
       signed channel's normal implementations. */
    client->signed_endpoint_override =
        options->trust == TILEFINCH_UPDATE_TRUST_SIGNED
        && options->metadata_url_override != NULL;
    int length = options->metadata_url_override != NULL
        ? (tilefinch_update_prepare_download_url(
               options->metadata_url_override, client->metadata_url,
               sizeof(client->metadata_url))
               ? (int) strlen(client->metadata_url) : -1)
        : options->release_tag != NULL
          ? snprintf(
                client->metadata_url, sizeof(client->metadata_url),
                "https://github.com/%s/%s/releases/download/%s/%s",
                client->owner, client->repository, client->release_tag,
                "tilefinch-update-v1.tfum")
        : snprintf(
              client->metadata_url, sizeof(client->metadata_url),
              "https://github.com/%s/%s/releases/latest/download/%s",
              client->owner, client->repository,
              client->artifact == TILEFINCH_UPDATE_ARTIFACT_VOICE_COMPONENT
                  ? "tilefinch-voice-en-us-v1.tfvm"
                  : client->artifact
                            == TILEFINCH_UPDATE_ARTIFACT_GLYPH_COMPONENT
                      ? client->metadata_asset : "tilefinch-update-v1.tfum");
    if (length <= 0 || (size_t) length >= sizeof(client->metadata_url)) {
        budget_free(budget, client);
        return NULL;
    }
    if (options->package_url_override != NULL
        && !tilefinch_update_prepare_download_url(
               options->package_url_override, client->package_url,
               sizeof(client->package_url))) {
        budget_free(budget, client);
        return NULL;
    }
    /*
     * Second latch on the same rule, after the URLs are resolved rather
     * than before. tilefinch_update_url_is_valid() already refused a
     * non-https override, but the share-link rewrite runs between that
     * check and the stored value, and the unsigned channel is the wrong
     * place to trust that a rewrite preserved the scheme.
     */
    if (client->trust == TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
        && (strncmp(client->metadata_url, "https://", 8u) != 0
            || (options->package_url_override != NULL
                && strncmp(client->package_url, "https://", 8u) != 0))) {
        budget_free(budget, client);
        return NULL;
    }
    client->scheduler = fetch_scheduler_create(
        budget, 1, UPDATE_SCHEDULER_RESERVE);
    if (client->scheduler == NULL) {
        budget_free(budget, client);
        return NULL;
    }
    /* Keep signature/downgrade policy, hashing, journal transitions and every
       filesystem mutation on the cooperative update state machine. The PSP
       worker receives only the immutable, already-authorized HTTPS hop. */
    (void) fetch_scheduler_enable_background_transport(
        client->scheduler, true);
    return client;
}

static void update_close_part(TilefinchUpdateClient *client)
{
    if (client->part != NULL) {
        fclose(client->part);
        client->part = NULL;
    }
}

void tilefinch_update_client_destroy(TilefinchUpdateClient *client)
{
    if (client == NULL) return;
    update_close_part(client);
    if (client->request_id != 0)
        (void) fetch_scheduler_discard(client->scheduler, client->request_id);
    fetch_scheduler_destroy(client->scheduler);
    budget_free(client->budget, client->envelope);
    Budget *budget = client->budget;
    memset(client, 0, sizeof(*client));
    budget_free(budget, client);
}

bool tilefinch_update_client_begin_check(
    TilefinchUpdateClient *client, uint64_t now_unix, bool clock_valid)
{
    if (client == NULL
        || (client->phase != TILEFINCH_UPDATE_CLIENT_IDLE
            && client->phase != TILEFINCH_UPDATE_CLIENT_ERROR
            && client->phase != TILEFINCH_UPDATE_CLIENT_UP_TO_DATE
            && client->phase != TILEFINCH_UPDATE_CLIENT_AVAILABLE)) {
        return false;
    }
    budget_free(client->budget, client->envelope);
    client->envelope = NULL;
    client->envelope_length = 0;
    client->now_unix = now_unix;
    client->clock_valid = clock_valid;
    client->status = TILEFINCH_UPDATE_OK;
    snprintf(client->message, sizeof(client->message), "CHECKING...");
    FetchRequest request = update_request(client, client->metadata_url);
    client->request_id = fetch_scheduler_enqueue(
        client->scheduler, client->metadata_url, &request,
        TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES, UPDATE_METADATA_TIMEOUT_MS);
    if (client->request_id == 0) {
        char message[sizeof(client->message)];
        snprintf(message, sizeof(message), "CHECK COULD NOT START: %.68s",
                 fetch_scheduler_last_error(client->scheduler));
        update_client_error(
            client, TILEFINCH_UPDATE_IO, message);
        return false;
    }
    client->phase = TILEFINCH_UPDATE_CLIENT_CHECKING;
    client->request_is_download = false;
    return true;
}

static bool update_package_body(
    void *opaque, const unsigned char *data, size_t length)
{
    TilefinchUpdateClient *client = opaque;
    if (client == NULL || client->part == NULL
        || client->package_bytes
               > client->verified.manifest.package_size
        || length > client->verified.manifest.package_size
                         - client->package_bytes
        || fwrite(data, 1, length, client->part) != length
        || !tilefinch_sha256_update(&client->package_sha, data, length)) {
        return false;
    }
    client->package_bytes += length;
    return true;
}

bool tilefinch_update_client_begin_download(TilefinchUpdateClient *client)
{
    if (client == NULL
        || client->phase != TILEFINCH_UPDATE_CLIENT_AVAILABLE
        || client->envelope == NULL) return false;
    uint64_t available = 0;
    uint64_t package_size = client->verified.manifest.package_size;
    uint64_t required = package_size > (UINT64_MAX
                            - UPDATE_FREE_SPACE_MARGIN) / 2u
        ? UINT64_MAX
        : package_size * 2u + UPDATE_FREE_SPACE_MARGIN;
    if (!update_free_space(client->part_path, &available)
        || available < required) {
        update_client_error(
            client, TILEFINCH_UPDATE_NO_SPACE,
            "NOT ENOUGH FREE SPACE FOR UPDATE");
        return false;
    }
    int url_length = 0;
    if (client->package_url_override) {
        url_length = (int) strlen(client->package_url);
    } else if (client->package_relative_to_metadata) {
        char base[sizeof(client->metadata_url)];
        snprintf(base, sizeof(base), "%s", client->metadata_url);
        char *query = strchr(base, '?');
        if (query != NULL) *query = '\0';
        char *slash = strrchr(base, '/');
        if (slash == NULL) {
            update_client_error(
                client, TILEFINCH_UPDATE_BAD_STRING,
                "DEVELOPER UPDATE URL HAS NO DIRECTORY");
            return false;
        }
        slash[1] = '\0';
        url_length = snprintf(
            client->package_url, sizeof(client->package_url), "%s%s",
            base, client->verified.manifest.asset);
    } else {
        url_length = snprintf(
            client->package_url, sizeof(client->package_url),
            "https://github.com/%s/%s/releases/download/%s/%s",
            client->owner, client->repository,
            client->verified.manifest.tag, client->verified.manifest.asset);
    }
    if (url_length <= 0
        || (size_t) url_length >= sizeof(client->package_url)) {
        update_client_error(
            client, TILEFINCH_UPDATE_BAD_STRING, "UPDATE URL IS TOO LONG");
        return false;
    }
    client->part = fopen(client->part_path, "wb");
    if (client->part == NULL) {
        update_client_error(
            client, TILEFINCH_UPDATE_IO, "UPDATE FILE COULD NOT BE CREATED");
        return false;
    }
    tilefinch_sha256_init(&client->package_sha);
    client->package_bytes = 0;
    FetchRequest request = update_request(client, client->package_url);
    FetchStreamOptions stream = {
        .on_body = update_package_body,
        .opaque = client
    };
    client->request_id = fetch_scheduler_enqueue_stream(
        client->scheduler, client->package_url, &request,
        (size_t) client->verified.manifest.package_size,
        UPDATE_PACKAGE_TIMEOUT_MS, &stream);
    if (client->request_id == 0) {
        update_close_part(client);
        char message[sizeof(client->message)];
        snprintf(message, sizeof(message), "DOWNLOAD START FAILED: %.68s",
                 fetch_scheduler_last_error(client->scheduler));
        update_client_error(
            client, TILEFINCH_UPDATE_IO, message);
        return false;
    }
    client->phase = TILEFINCH_UPDATE_CLIENT_DOWNLOADING;
    client->request_is_download = true;
    snprintf(client->message, sizeof(client->message), "DOWNLOADING...");
    return true;
}

bool tilefinch_update_client_cancel(TilefinchUpdateClient *client)
{
    if (client == NULL || client->request_id == 0
        || (client->phase != TILEFINCH_UPDATE_CLIENT_CHECKING
            && client->phase != TILEFINCH_UPDATE_CLIENT_DOWNLOADING))
        return false;
    if (!fetch_scheduler_cancel(
            client->scheduler, client->request_id, "user cancelled update"))
        return false;
    client->phase = TILEFINCH_UPDATE_CLIENT_CANCELLING;
    snprintf(client->message, sizeof(client->message), "STOPPING...");
    return true;
}

static void update_finish_check(
    TilefinchUpdateClient *client, bool success, FetchResult *result)
{
    if (!success || result->status_code != 200 || result->data == NULL) {
        update_client_error(
            client, TILEFINCH_UPDATE_IO,
            result->error[0] == '\0' ? "UPDATE CHECK FAILED" : result->error);
        return;
    }
    client->envelope = budget_malloc_category(
        client->budget, BUDGET_CATEGORY_SESSION, result->length);
    if (client->envelope == NULL) {
        update_client_error(
            client, TILEFINCH_UPDATE_IO, "NOT ENOUGH MEMORY FOR METADATA");
        return;
    }
    memcpy(client->envelope, result->data, result->length);
    client->envelope_length = result->length;
    TilefinchUpdateStatus status;
    if (client->trust == TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED) {
        status = tilefinch_update_parse_developer_envelope(
            client->envelope, client->envelope_length,
            client->launcher_protocol, &client->verified);
    } else {
        TilefinchUpdateVerifyOptions options = {
            .embedded_root = &client->embedded_root,
            .crypto = tilefinch_update_default_crypto(),
            .now_unix = client->now_unix,
            .clock_valid = client->clock_valid,
            .launcher_protocol = client->launcher_protocol,
            .installed_sequence = client->installed_sequence,
            .installed_sequence_valid = client->installed_sequence_valid,
            .installed_pair_valid = client->installed_pair_valid,
            .expected_tag = client->release_tag[0] == '\0'
                ? NULL : client->release_tag,
            .allow_downgrade = client->allow_downgrade
        };
        memcpy(
            options.installed_package_sha256, client->installed_sha256, 32);
        if (client->artifact == TILEFINCH_UPDATE_ARTIFACT_VOICE_COMPONENT) {
            status = tilefinch_update_verify_voice_envelope(
                client->envelope, client->envelope_length,
                &options, &client->verified);
        } else if (client->artifact
                       == TILEFINCH_UPDATE_ARTIFACT_GLYPH_COMPONENT) {
            status = tilefinch_update_verify_glyph_envelope(
                client->envelope, client->envelope_length,
                &options, &client->verified);
        } else {
            status = tilefinch_update_verify_envelope(
                client->envelope, client->envelope_length,
                &options, &client->verified);
        }
    }
    if (status != TILEFINCH_UPDATE_OK) {
        update_client_error(
            client, status, tilefinch_update_status_name(status));
        return;
    }
    if (client->trust == TILEFINCH_UPDATE_TRUST_SIGNED
        && client->installed_sequence_valid
        && client->verified.manifest.release_sequence
               == client->installed_sequence) {
        client->phase = TILEFINCH_UPDATE_CLIENT_UP_TO_DATE;
        snprintf(client->message, sizeof(client->message), "UP TO DATE");
    } else {
        client->phase = TILEFINCH_UPDATE_CLIENT_AVAILABLE;
        snprintf(client->message, sizeof(client->message), "UPDATE AVAILABLE");
    }
}

static void update_finish_download(
    TilefinchUpdateClient *client, bool success, FetchResult *result)
{
    bool close_ok = client->part != NULL && fflush(client->part) == 0;
    if (client->part != NULL && fclose(client->part) != 0) close_ok = false;
    client->part = NULL;
    uint8_t digest[32];
    bool hash_ok = tilefinch_sha256_final(&client->package_sha, digest);
    if (!success || result->status_code != 200 || !close_ok || !hash_ok
        || client->package_bytes != client->verified.manifest.package_size
        || memcmp(
               digest, client->verified.manifest.package_sha256, 32) != 0) {
        remove(client->part_path);
        update_client_error(
            client,
            success ? TILEFINCH_UPDATE_PACKAGE_MISMATCH
                    : TILEFINCH_UPDATE_IO,
            success ? "DOWNLOADED FILE DID NOT VERIFY"
                    : (result->error[0] == '\0'
                           ? "DOWNLOAD FAILED" : result->error));
        return;
    }
    client->phase = TILEFINCH_UPDATE_CLIENT_DOWNLOADED;
    snprintf(
        client->message, sizeof(client->message),
        client->trust == TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
            ? "UNSIGNED DOWNLOAD HASHED"
            : "DOWNLOAD VERIFIED");
}

bool tilefinch_update_client_pump(
    TilefinchUpdateClient *client, uint64_t maximum_time_us)
{
    if (client == NULL || client->request_id == 0) return false;
    FetchPumpQuota quota = {
        .maximum_body_callbacks = 1,
        .maximum_body_bytes = UPDATE_PUMP_BYTES,
        .maximum_time_us = maximum_time_us == 0 ? 2000u : maximum_time_us
    };
    FetchPumpMetrics metrics;
    (void) fetch_scheduler_pump_bounded(
        client->scheduler, 1, 0, &quota, &metrics);
    bool success = false;
    /* Both take functions overwrite their complete result.  Leaving these
       uninitialized avoids clearing the roughly 14 KiB FetchResult on every
       incomplete update pump; neither object is observed unless take says
       the request completed. */
    FetchResult result;
    FetchStreamMetrics stream_metrics;
    bool complete = client->request_is_download
        ? fetch_scheduler_take_stream(
              client->scheduler, client->request_id, &success,
              &stream_metrics, &result)
        : fetch_scheduler_take(
              client->scheduler, client->request_id, &success, &result);
    if (!complete) return true;
    TilefinchUpdateClientPhase completed_phase = client->phase;
    client->request_id = 0;
    client->request_is_download = false;
    if (completed_phase == TILEFINCH_UPDATE_CLIENT_CANCELLING) {
        update_close_part(client);
        remove(client->part_path);
        client->phase = TILEFINCH_UPDATE_CLIENT_IDLE;
        client->status = TILEFINCH_UPDATE_CANCELLED;
        snprintf(client->message, sizeof(client->message), "CANCELLED");
    } else if (completed_phase == TILEFINCH_UPDATE_CLIENT_CHECKING) {
        update_finish_check(client, success, &result);
    } else {
        update_finish_download(client, success, &result);
    }
    fetch_result_destroy(&result);
    return true;
}

bool tilefinch_update_client_snapshot(
    const TilefinchUpdateClient *client,
    TilefinchUpdateClientSnapshot *snapshot)
{
    if (client == NULL || snapshot == NULL) return false;
    *snapshot = (TilefinchUpdateClientSnapshot) {
        .phase = client->phase,
        .status = client->status,
        .manifest = client->verified.manifest,
        .bytes_received = client->package_bytes,
        .bytes_total = client->phase >= TILEFINCH_UPDATE_CLIENT_AVAILABLE
            ? client->verified.manifest.package_size : 0
    };
    memcpy(
        snapshot->manifest_digest, client->verified.manifest_digest, 32);
    snprintf(snapshot->message, sizeof(snapshot->message), "%s",
             client->message);
    return true;
}

const uint8_t *tilefinch_update_client_envelope(
    const TilefinchUpdateClient *client, size_t *length)
{
    if (length != NULL)
        *length = client == NULL ? 0 : client->envelope_length;
    return client == NULL ? NULL : client->envelope;
}
