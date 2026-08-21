#include "tilefinch/update_history.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/fetch.h"

#define UPDATE_HISTORY_RESPONSE_LIMIT (256u * 1024u)
#define UPDATE_HISTORY_TIMEOUT_MS 30000L
#define UPDATE_HISTORY_PUMP_BYTES (16u * 1024u)
#define UPDATE_HISTORY_SCAN_DEPTH 24u
/* GitHub returns releases newest-first. Request only the current release plus
   the eight predecessors the UI can display; the byte ceiling remains an
   independent bound for unusually large notes or asset inventories. */
#define UPDATE_HISTORY_RELEASE_REQUEST_LIMIT \
    (TILEFINCH_UPDATE_HISTORY_LIMIT + 1u)

typedef struct {
    const unsigned char *at;
    const unsigned char *end;
} UpdateHistoryJson;

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} UpdateHistoryVersion;

struct TilefinchUpdateHistory {
    Budget *budget;
    FetchScheduler *scheduler;
    uint64_t request_id;
    char owner[64];
    char repository[64];
    char current_version[TILEFINCH_UPDATE_HISTORY_VERSION_CAPACITY];
    TilefinchUpdateHistorySnapshot snapshot;
};

static void history_skip_space(UpdateHistoryJson *json)
{
    while (json->at < json->end && isspace((unsigned char) *json->at))
        json->at++;
}

static bool history_take(UpdateHistoryJson *json, unsigned char character)
{
    history_skip_space(json);
    if (json->at >= json->end || *json->at != character) return false;
    json->at++;
    return true;
}

static bool history_hex(unsigned char value, unsigned *digit)
{
    if (value >= '0' && value <= '9') *digit = value - '0';
    else if (value >= 'a' && value <= 'f') *digit = value - 'a' + 10u;
    else if (value >= 'A' && value <= 'F') *digit = value - 'A' + 10u;
    else return false;
    return true;
}

static bool history_string(
    UpdateHistoryJson *json, char *output, size_t capacity)
{
    history_skip_space(json);
    if (json->at >= json->end || *json->at++ != '"') return false;
    size_t used = 0;
    bool fits = output == NULL || capacity != 0;
    while (json->at < json->end) {
        unsigned char value = *json->at++;
        if (value == '"') {
            if (output != NULL) {
                if (!fits || used >= capacity) return false;
                output[used] = '\0';
            }
            return fits;
        }
        if (value < 0x20u) return false;
        if (value == '\\') {
            if (json->at >= json->end) return false;
            value = *json->at++;
            switch (value) {
                case '"': case '\\': case '/': break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case 'u': {
                    unsigned codepoint = 0;
                    for (unsigned digit_index = 0; digit_index < 4u;
                         digit_index++) {
                        unsigned digit = 0;
                        if (json->at >= json->end
                            || !history_hex(*json->at++, &digit)) return false;
                        codepoint = (codepoint << 4u) | digit;
                    }
                    value = codepoint <= 0x7fu
                        ? (unsigned char) codepoint : '?';
                    break;
                }
                default: return false;
            }
        }
        if (output != NULL) {
            if (used + 1u >= capacity) fits = false;
            else output[used] = (char) value;
        }
        used++;
    }
    return false;
}

static bool history_skip_value(UpdateHistoryJson *json, unsigned depth);

static bool history_skip_compound(
    UpdateHistoryJson *json, unsigned char open, unsigned char close,
    unsigned depth)
{
    if (depth >= UPDATE_HISTORY_SCAN_DEPTH || !history_take(json, open))
        return false;
    history_skip_space(json);
    if (json->at < json->end && *json->at == close) {
        json->at++;
        return true;
    }
    for (;;) {
        if (open == '{') {
            if (!history_string(json, NULL, 0) || !history_take(json, ':'))
                return false;
        }
        if (!history_skip_value(json, depth + 1u)) return false;
        history_skip_space(json);
        if (json->at < json->end && *json->at == close) {
            json->at++;
            return true;
        }
        if (!history_take(json, ',')) return false;
    }
}

static bool history_skip_value(UpdateHistoryJson *json, unsigned depth)
{
    history_skip_space(json);
    if (depth >= UPDATE_HISTORY_SCAN_DEPTH || json->at >= json->end)
        return false;
    if (*json->at == '"') return history_string(json, NULL, 0);
    if (*json->at == '{')
        return history_skip_compound(json, '{', '}', depth);
    if (*json->at == '[')
        return history_skip_compound(json, '[', ']', depth);
    const unsigned char *start = json->at;
    while (json->at < json->end
           && *json->at != ',' && *json->at != '}' && *json->at != ']'
           && !isspace((unsigned char) *json->at)) json->at++;
    return json->at != start;
}

static bool history_boolean(UpdateHistoryJson *json, bool *value)
{
    history_skip_space(json);
    size_t remaining = (size_t) (json->end - json->at);
    if (remaining >= 4u && memcmp(json->at, "true", 4u) == 0) {
        json->at += 4u;
        *value = true;
        return true;
    }
    if (remaining >= 5u && memcmp(json->at, "false", 5u) == 0) {
        json->at += 5u;
        *value = false;
        return true;
    }
    return false;
}

static bool history_asset_object(UpdateHistoryJson *json, bool *found)
{
    if (!history_take(json, '{')) return false;
    history_skip_space(json);
    if (json->at < json->end && *json->at == '}') {
        json->at++;
        return true;
    }
    for (;;) {
        char key[32];
        if (!history_string(json, key, sizeof(key))
            || !history_take(json, ':')) return false;
        if (strcmp(key, "name") == 0) {
            char name[64];
            if (!history_string(json, name, sizeof(name))) return false;
            if (strcmp(name, "tilefinch-update-v1.tfum") == 0) *found = true;
        } else if (!history_skip_value(json, 1u)) {
            return false;
        }
        history_skip_space(json);
        if (json->at < json->end && *json->at == '}') {
            json->at++;
            return true;
        }
        if (!history_take(json, ',')) return false;
    }
}

static bool history_assets(UpdateHistoryJson *json, bool *found)
{
    if (!history_take(json, '[')) return false;
    history_skip_space(json);
    if (json->at < json->end && *json->at == ']') {
        json->at++;
        return true;
    }
    for (;;) {
        history_skip_space(json);
        if (json->at < json->end && *json->at == '{') {
            if (!history_asset_object(json, found)) return false;
        } else if (!history_skip_value(json, 1u)) {
            return false;
        }
        history_skip_space(json);
        if (json->at < json->end && *json->at == ']') {
            json->at++;
            return true;
        }
        if (!history_take(json, ',')) return false;
    }
}

static bool history_version_number(
    const char **cursor, uint32_t *number)
{
    const char *at = *cursor;
    if (*at < '0' || *at > '9') return false;
    uint32_t value = 0;
    do {
        unsigned digit = (unsigned) (*at++ - '0');
        if (value > (UINT32_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    } while (*at >= '0' && *at <= '9');
    *cursor = at;
    *number = value;
    return true;
}

static bool history_version_parse(
    const char *text, bool require_tag, UpdateHistoryVersion *version)
{
    if (text == NULL || version == NULL) return false;
    const char *at = text;
    if (*at == 'v') at++;
    else if (require_tag) return false;
    UpdateHistoryVersion parsed = {0};
    if (!history_version_number(&at, &parsed.major) || *at++ != '.'
        || !history_version_number(&at, &parsed.minor) || *at++ != '.'
        || !history_version_number(&at, &parsed.patch) || *at != '\0')
        return false;
    *version = parsed;
    return true;
}

static bool history_version_older(
    const UpdateHistoryVersion *candidate,
    const UpdateHistoryVersion *current)
{
    if (candidate->major != current->major)
        return candidate->major < current->major;
    if (candidate->minor != current->minor)
        return candidate->minor < current->minor;
    return candidate->patch < current->patch;
}

static bool history_release(
    UpdateHistoryJson *json, const UpdateHistoryVersion *current,
    TilefinchUpdateHistorySnapshot *snapshot)
{
    if (!history_take(json, '{')) return false;
    char tag[TILEFINCH_UPDATE_HISTORY_VERSION_CAPACITY] = {0};
    bool draft = false;
    bool prerelease = false;
    bool asset = false;
    bool tag_present = false;
    history_skip_space(json);
    if (json->at < json->end && *json->at == '}') {
        json->at++;
        return true;
    }
    for (;;) {
        char key[32];
        if (!history_string(json, key, sizeof(key))
            || !history_take(json, ':')) return false;
        if (strcmp(key, "tag_name") == 0) {
            if (!history_string(json, tag, sizeof(tag))) return false;
            tag_present = true;
        } else if (strcmp(key, "draft") == 0) {
            if (!history_boolean(json, &draft)) return false;
        } else if (strcmp(key, "prerelease") == 0) {
            if (!history_boolean(json, &prerelease)) return false;
        } else if (strcmp(key, "assets") == 0) {
            if (!history_assets(json, &asset)) return false;
        } else if (!history_skip_value(json, 1u)) {
            return false;
        }
        history_skip_space(json);
        if (json->at < json->end && *json->at == '}') {
            json->at++;
            break;
        }
        if (!history_take(json, ',')) return false;
    }
    UpdateHistoryVersion candidate;
    if (!tag_present || draft || prerelease || !asset
        || !history_version_parse(tag, true, &candidate)
        || !history_version_older(&candidate, current)
        || snapshot->count >= TILEFINCH_UPDATE_HISTORY_LIMIT) return true;
    const char *version = tag + 1u;
    for (size_t index = 0; index < snapshot->count; index++) {
        if (strcmp(snapshot->versions[index], version) == 0) return true;
    }
    snprintf(snapshot->versions[snapshot->count],
             sizeof(snapshot->versions[snapshot->count]), "%s", version);
    snapshot->count++;
    return true;
}

bool tilefinch_update_history_parse(
    const unsigned char *json_bytes, size_t length,
    const char *current_version, TilefinchUpdateHistorySnapshot *snapshot)
{
    if (json_bytes == NULL || length == 0 || snapshot == NULL) return false;
    UpdateHistoryVersion current;
    if (!history_version_parse(current_version, false, &current)) return false;
    TilefinchUpdateHistorySnapshot parsed = {
        .phase = TILEFINCH_UPDATE_HISTORY_READY
    };
    UpdateHistoryJson json = {json_bytes, json_bytes + length};
    if (!history_take(&json, '[')) return false;
    history_skip_space(&json);
    size_t releases_seen = 0;
    while (json.at < json.end && *json.at != ']') {
        if (++releases_seen > UPDATE_HISTORY_RELEASE_REQUEST_LIMIT
            || !history_release(&json, &current, &parsed)) return false;
        history_skip_space(&json);
        if (json.at < json.end && *json.at == ']') break;
        if (!history_take(&json, ',')) return false;
    }
    if (!history_take(&json, ']')) return false;
    history_skip_space(&json);
    if (json.at != json.end) return false;
    snprintf(parsed.message, sizeof(parsed.message),
             parsed.count == 0 ? "NO EARLIER RELEASES FOUND"
                               : "SIGNED RELEASES FROM GITHUB");
    *snapshot = parsed;
    return true;
}

static bool history_identifier(const char *value, size_t capacity)
{
    if (value == NULL) return false;
    size_t length = strnlen(value, capacity);
    if (length == 0 || length >= capacity) return false;
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char) value[index];
        if (!(isalnum(character) || character == '-' || character == '_'))
            return false;
    }
    return true;
}

TilefinchUpdateHistory *tilefinch_update_history_create(
    Budget *budget, const char *repository_owner,
    const char *repository_name, const char *current_version)
{
    UpdateHistoryVersion ignored;
    if (budget == NULL || !history_identifier(repository_owner, 64u)
        || !history_identifier(repository_name, 64u)
        || !history_version_parse(current_version, false, &ignored))
        return NULL;
    TilefinchUpdateHistory *history = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1u, sizeof(*history));
    if (history == NULL) return NULL;
    history->budget = budget;
    snprintf(history->owner, sizeof(history->owner), "%s", repository_owner);
    snprintf(history->repository, sizeof(history->repository), "%s",
             repository_name);
    snprintf(history->current_version, sizeof(history->current_version), "%s",
             current_version);
    return history;
}

void tilefinch_update_history_destroy(TilefinchUpdateHistory *history)
{
    if (history == NULL) return;
    if (history->request_id != 0 && history->scheduler != NULL)
        (void) fetch_scheduler_discard(
            history->scheduler, history->request_id);
    fetch_scheduler_destroy(history->scheduler);
    Budget *budget = history->budget;
    memset(history, 0, sizeof(*history));
    budget_free(budget, history);
}

bool tilefinch_update_history_url(
    const char *repository_owner, const char *repository_name,
    char *output, size_t capacity)
{
    if (!history_identifier(repository_owner, 64u)
        || !history_identifier(repository_name, 64u)
        || output == NULL || capacity == 0) return false;
    int written = snprintf(
        output, capacity,
        "https://api.github.com/repos/%s/%s/releases?per_page=%u",
        repository_owner, repository_name,
        UPDATE_HISTORY_RELEASE_REQUEST_LIMIT);
    return written > 0 && (size_t) written < capacity;
}

bool tilefinch_update_history_begin(TilefinchUpdateHistory *history)
{
    if (history == NULL || history->request_id != 0
        || history->snapshot.phase == TILEFINCH_UPDATE_HISTORY_LOADING)
        return false;
    if (history->scheduler == NULL) {
        history->scheduler = fetch_scheduler_create(
            history->budget, 1u, UPDATE_HISTORY_RESPONSE_LIMIT);
        if (history->scheduler == NULL) goto unavailable;
        (void) fetch_scheduler_enable_background_transport(
            history->scheduler, true);
    }
    char url[256];
    if (!tilefinch_update_history_url(
            history->owner, history->repository, url, sizeof(url)))
        goto unavailable;
    FetchRequest request = {
        .method = "GET",
        .extra_headers = "X-GitHub-Api-Version: 2022-11-28",
        .accept = "application/vnd.github+json",
        .credentials = FETCH_CREDENTIALS_OMIT,
        .redirect_same_origin_only = true,
        .user_agent = "Tilefinch-Updater/1"
    };
    history->request_id = fetch_scheduler_enqueue(
        history->scheduler, url, &request,
        UPDATE_HISTORY_RESPONSE_LIMIT, UPDATE_HISTORY_TIMEOUT_MS);
    if (history->request_id == 0) goto unavailable;
    memset(&history->snapshot, 0, sizeof(history->snapshot));
    history->snapshot.phase = TILEFINCH_UPDATE_HISTORY_LOADING;
    snprintf(history->snapshot.message, sizeof(history->snapshot.message),
             "LOADING RELEASES...");
    return true;

unavailable:
    memset(&history->snapshot, 0, sizeof(history->snapshot));
    history->snapshot.phase = TILEFINCH_UPDATE_HISTORY_ERROR;
    snprintf(history->snapshot.message, sizeof(history->snapshot.message),
             "RELEASE LIST COULD NOT START");
    return false;
}

bool tilefinch_update_history_pump(
    TilefinchUpdateHistory *history, uint64_t maximum_time_us)
{
    if (history == NULL || history->scheduler == NULL
        || history->request_id == 0) return false;
    FetchPumpQuota quota = {
        .maximum_body_callbacks = 1u,
        .maximum_body_bytes = UPDATE_HISTORY_PUMP_BYTES,
        .maximum_time_us = maximum_time_us == 0 ? 2000u : maximum_time_us
    };
    FetchPumpMetrics metrics;
    (void) fetch_scheduler_pump_bounded(
        history->scheduler, 1u, 0u, &quota, &metrics);
    FetchResult result;
    bool success = false;
    if (!fetch_scheduler_take(
            history->scheduler, history->request_id, &success, &result))
        return true;
    history->request_id = 0;
    TilefinchUpdateHistorySnapshot parsed = {0};
    if (success && result.status_code == 200
        && tilefinch_update_history_parse(
               (const unsigned char *) result.data, result.length,
               history->current_version, &parsed)) {
        history->snapshot = parsed;
    } else {
        memset(&history->snapshot, 0, sizeof(history->snapshot));
        history->snapshot.phase = TILEFINCH_UPDATE_HISTORY_ERROR;
        snprintf(history->snapshot.message,
                 sizeof(history->snapshot.message),
                 result.status_code == 403
                     ? "GITHUB RATE LIMIT REACHED"
                     : "RELEASE LIST COULD NOT LOAD");
    }
    fetch_result_destroy(&result);
    return true;
}

bool tilefinch_update_history_cancel(TilefinchUpdateHistory *history)
{
    if (history == NULL || history->request_id == 0
        || history->scheduler == NULL) return false;
    return fetch_scheduler_cancel(
        history->scheduler, history->request_id, "versions closed");
}

bool tilefinch_update_history_snapshot(
    const TilefinchUpdateHistory *history,
    TilefinchUpdateHistorySnapshot *snapshot)
{
    if (history == NULL || snapshot == NULL) return false;
    *snapshot = history->snapshot;
    return true;
}

bool tilefinch_update_history_tag(
    const TilefinchUpdateHistorySnapshot *snapshot, size_t index,
    char *output, size_t capacity)
{
    if (snapshot == NULL || index >= snapshot->count
        || output == NULL || capacity == 0) return false;
    int written = snprintf(
        output, capacity, "v%s", snapshot->versions[index]);
    return written > 0 && (size_t) written < capacity;
}
