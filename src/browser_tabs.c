#include "tilefinch/browser_tabs.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    BrowserTabSnapshot *snapshot;
    char title[BROWSER_TAB_LABEL_LIMIT];
    char find_query[BROWSER_TAB_FIND_QUERY_LIMIT + 1u];
    bool hibernated;
} BrowserTabSlot;

struct BrowserTabs {
    Budget *budget;
    BrowserTabSlot slots[BROWSER_TAB_LIMIT];
    size_t count;
    size_t active_index;
};

static BrowserTabSnapshot *tab_snapshot_create(BrowserTabs *tabs)
{
    return tabs == NULL ? NULL : budget_calloc_category(
        tabs->budget, BUDGET_CATEGORY_SESSION, 1,
        sizeof(BrowserTabSnapshot));
}

static void tab_slot_release(BrowserTabs *tabs, BrowserTabSlot *slot)
{
    if (tabs == NULL || slot == NULL) return;
    budget_free(tabs->budget, slot->snapshot);
    memset(slot, 0, sizeof(*slot));
}

static void tab_slot_title_from_snapshot(BrowserTabSlot *slot)
{
    if (slot == NULL || slot->snapshot == NULL
        || slot->snapshot->history_count == 0
        || slot->snapshot->history_index
               >= slot->snapshot->history_count) return;
    snprintf(
        slot->title, sizeof(slot->title), "%s",
        slot->snapshot->history[slot->snapshot->history_index].title);
}

static bool tab_url_valid(const char *text)
{
    if (text == NULL || text[0] == '\0') return false;
    return strnlen(text, NAVIGATION_URL_LIMIT) < NAVIGATION_URL_LIMIT;
}

static void tab_entry_set(
    BrowserTabHistoryEntry *entry, const char *url, const char *title,
    int scroll_y, int focus_kind, size_t focus_index)
{
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->url, sizeof(entry->url), "%s", url);
    snprintf(entry->title, sizeof(entry->title), "%s", title);
    entry->scroll_y = scroll_y < 0 ? 0 : scroll_y;
    entry->focus_kind = focus_kind;
    entry->focus_index = focus_index;
}

static bool tab_reset(
    BrowserTabSnapshot *tab, const char *url, const char *title)
{
    if (tab == NULL
        || !tab_url_valid(url) || title == NULL) {
        return false;
    }
    memset(tab, 0, sizeof(*tab));
    tab_entry_set(&tab->history[0], url, title, 0, 0, 0);
    tab->history_count = 1;
    return true;
}

BrowserTabs *browser_tabs_create(
    Budget *budget, const NavigationSession *initial_navigation)
{
    if (budget == NULL || initial_navigation == NULL) return NULL;
    BrowserTabs *tabs = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*tabs));
    if (tabs == NULL) return NULL;
    tabs->budget = budget;
    tabs->count = 1;
    tabs->slots[0].snapshot = tab_snapshot_create(tabs);
    if (tabs->slots[0].snapshot == NULL) {
        browser_tabs_destroy(tabs);
        return NULL;
    }
    if (!browser_tabs_capture_active(tabs, initial_navigation)) {
        browser_tabs_destroy(tabs);
        return NULL;
    }
    return tabs;
}

void browser_tabs_destroy(BrowserTabs *tabs)
{
    if (tabs == NULL) return;
    Budget *budget = tabs->budget;
    for (size_t index = 0; index < tabs->count; index++)
        budget_free(budget, tabs->slots[index].snapshot);
    memset(tabs, 0, sizeof(*tabs));
    budget_free(budget, tabs);
}

size_t browser_tabs_count(const BrowserTabs *tabs)
{
    return tabs == NULL ? 0 : tabs->count;
}

size_t browser_tabs_active_index(const BrowserTabs *tabs)
{
    return tabs == NULL ? 0 : tabs->active_index;
}

const BrowserTabSnapshot *browser_tabs_tab(
    const BrowserTabs *tabs, size_t index)
{
    return tabs == NULL || index >= tabs->count
        ? NULL : tabs->slots[index].snapshot;
}

const BrowserTabSnapshot *browser_tabs_active(const BrowserTabs *tabs)
{
    return browser_tabs_tab(tabs, browser_tabs_active_index(tabs));
}

const char *browser_tabs_title(const BrowserTabs *tabs, size_t index)
{
    return tabs == NULL || index >= tabs->count
        ? NULL : tabs->slots[index].title;
}

bool browser_tabs_hibernated(const BrowserTabs *tabs, size_t index)
{
    return tabs != NULL && index < tabs->count
        && tabs->slots[index].hibernated;
}

bool browser_tabs_capture_active(
    BrowserTabs *tabs, const NavigationSession *navigation)
{
    if (tabs == NULL || navigation == NULL
        || tabs->count == 0 || tabs->active_index >= tabs->count) {
        return false;
    }
    BrowserTabSnapshot *staged = tab_snapshot_create(tabs);
    if (staged == NULL) return false;
    bool valid = true;
    if (navigation->history_count == 0) {
        const char *url = navigation->page.document_url;
        const char *title = navigation->page.document.title;
        valid = tab_reset(
            staged, url == NULL ? "" : url,
            title == NULL ? "" : title);
    } else {
        size_t start = 0;
        if (navigation->history_count > BROWSER_TAB_HISTORY_LIMIT) {
            start = navigation->history_index
                        >= BROWSER_TAB_HISTORY_LIMIT - 1u
                ? navigation->history_index
                      - (BROWSER_TAB_HISTORY_LIMIT - 1u)
                : 0;
            if (start + BROWSER_TAB_HISTORY_LIMIT
                    > navigation->history_count) {
                start =
                    navigation->history_count - BROWSER_TAB_HISTORY_LIMIT;
            }
        }
        staged->history_count =
            navigation->history_count - start < BROWSER_TAB_HISTORY_LIMIT
            ? navigation->history_count - start
            : BROWSER_TAB_HISTORY_LIMIT;
        staged->history_index = navigation->history_index - start;
        for (size_t index = 0; valid && index < staged->history_count;
             index++) {
            const NavigationEntry *source =
                &navigation->history[start + index];
            if (!tab_url_valid(source->url) || source->title == NULL) {
                valid = false;
                break;
            }
            tab_entry_set(
                &staged->history[index], source->url, source->title,
                source->scroll_y, source->focus_kind,
                source->focus_index);
        }
    }
    BrowserTabSlot *slot = &tabs->slots[tabs->active_index];
    if (!valid || slot->snapshot == NULL || slot->hibernated) {
        budget_free(tabs->budget, staged);
        return false;
    }
    *slot->snapshot = *staged;
    budget_free(tabs->budget, staged);
    tab_slot_title_from_snapshot(slot);
    return true;
}

bool browser_tabs_select(BrowserTabs *tabs, size_t index)
{
    if (tabs == NULL || index >= tabs->count
        || tabs->slots[index].snapshot == NULL
        || tabs->slots[index].hibernated) return false;
    tabs->active_index = index;
    return true;
}

static uint32_t tab_checksum(const unsigned char *data, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t index = 0; index < length; index++) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool tab_put_u32(
    unsigned char *data, size_t capacity, size_t *used, uint32_t value)
{
    if (data == NULL || used == NULL || *used > capacity
        || capacity - *used < 4u) return false;
    data[*used] = (unsigned char) value;
    data[*used + 1u] = (unsigned char) (value >> 8);
    data[*used + 2u] = (unsigned char) (value >> 16);
    data[*used + 3u] = (unsigned char) (value >> 24);
    *used += 4u;
    return true;
}

static bool tab_get_u32(
    const unsigned char *data, size_t length, size_t *used, uint32_t *value)
{
    if (data == NULL || used == NULL || value == NULL || *used > length
        || length - *used < 4u) return false;
    *value = (uint32_t) data[*used]
        | (uint32_t) data[*used + 1u] << 8
        | (uint32_t) data[*used + 2u] << 16
        | (uint32_t) data[*used + 3u] << 24;
    *used += 4u;
    return true;
}

static bool tab_snapshot_encode(
    const BrowserTabSnapshot *snapshot, unsigned char *data,
    size_t capacity, size_t *length)
{
    if (snapshot == NULL || data == NULL || length == NULL
        || snapshot->history_count == 0
        || snapshot->history_count > BROWSER_TAB_HISTORY_LIMIT
        || snapshot->history_index >= snapshot->history_count
        || snapshot->history_count > UINT32_MAX
        || snapshot->history_index > UINT32_MAX) return false;
    size_t used = 0;
    if (!tab_put_u32(data, capacity, &used,
                     (uint32_t) snapshot->history_count)
        || !tab_put_u32(data, capacity, &used,
                        (uint32_t) snapshot->history_index)) return false;
    for (size_t index = 0; index < snapshot->history_count; index++) {
        const BrowserTabHistoryEntry *entry = &snapshot->history[index];
        size_t url_length = strnlen(entry->url, sizeof(entry->url));
        size_t title_length = strnlen(entry->title, sizeof(entry->title));
        if (url_length == 0 || url_length >= sizeof(entry->url)
            || title_length >= sizeof(entry->title)
            || entry->focus_index > UINT32_MAX
            || url_length > UINT32_MAX || title_length > UINT32_MAX
            || !tab_put_u32(data, capacity, &used,
                            (uint32_t) entry->scroll_y)
            || !tab_put_u32(data, capacity, &used,
                            (uint32_t) (int32_t) entry->focus_kind)
            || !tab_put_u32(data, capacity, &used,
                            (uint32_t) entry->focus_index)
            || !tab_put_u32(data, capacity, &used,
                            (uint32_t) url_length)
            || !tab_put_u32(data, capacity, &used,
                            (uint32_t) title_length)
            || used > capacity
            || url_length > capacity - used) return false;
        memcpy(data + used, entry->url, url_length);
        used += url_length;
        if (title_length > capacity - used) return false;
        memcpy(data + used, entry->title, title_length);
        used += title_length;
    }
    *length = used;
    return true;
}

static bool tab_snapshot_decode(
    const unsigned char *data, size_t length, BrowserTabSnapshot *snapshot)
{
    if (data == NULL || snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    size_t used = 0;
    uint32_t count = 0, current = 0;
    if (!tab_get_u32(data, length, &used, &count)
        || !tab_get_u32(data, length, &used, &current)
        || count == 0 || count > BROWSER_TAB_HISTORY_LIMIT
        || current >= count) return false;
    snapshot->history_count = count;
    snapshot->history_index = current;
    for (size_t index = 0; index < count; index++) {
        uint32_t scroll = 0, focus_kind = 0, focus_index = 0;
        uint32_t url_length = 0, title_length = 0;
        if (!tab_get_u32(data, length, &used, &scroll)
            || !tab_get_u32(data, length, &used, &focus_kind)
            || !tab_get_u32(data, length, &used, &focus_index)
            || !tab_get_u32(data, length, &used, &url_length)
            || !tab_get_u32(data, length, &used, &title_length)
            || url_length == 0 || url_length >= NAVIGATION_URL_LIMIT
            || title_length >= NAVIGATION_TITLE_LIMIT
            || used > length || url_length > length - used) return false;
        BrowserTabHistoryEntry *entry = &snapshot->history[index];
        memcpy(entry->url, data + used, url_length);
        entry->url[url_length] = '\0';
        used += url_length;
        if (title_length > length - used) return false;
        memcpy(entry->title, data + used, title_length);
        entry->title[title_length] = '\0';
        used += title_length;
        if (!tab_url_valid(entry->url) || scroll > INT32_MAX) return false;
        entry->scroll_y = (int) scroll;
        entry->focus_kind = (int) (int32_t) focus_kind;
        entry->focus_index = focus_index;
    }
    if (used != length) return false;
    return true;
}

static unsigned char *tab_payload_create(const BrowserTabs *tabs)
{
    return tabs == NULL ? NULL : budget_malloc_category(
        tabs->budget, BUDGET_CATEGORY_SESSION,
        BROWSER_TAB_HIBERNATION_FILE_LIMIT);
}

static bool tab_read_hibernation_payload(
    const char *path, unsigned char payload[BROWSER_TAB_HIBERNATION_FILE_LIMIT],
    size_t *payload_length)
{
    static const unsigned char magic[8] = {
        'T', 'F', 'T', 'A', 'B', '0', '1', 0
    };
    if (path == NULL || path[0] == '\0' || payload == NULL
        || payload_length == NULL) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    unsigned char header[16];
    bool okay = fread(header, 1, sizeof(header), file) == sizeof(header)
        && memcmp(header, magic, sizeof(magic)) == 0;
    size_t header_used = sizeof(magic);
    uint32_t encoded_length = 0, expected_checksum = 0;
    if (okay) {
        okay = tab_get_u32(
                    header, sizeof(header), &header_used, &encoded_length)
            && tab_get_u32(
                    header, sizeof(header), &header_used, &expected_checksum)
            && encoded_length <= BROWSER_TAB_HIBERNATION_FILE_LIMIT;
    }
    if (okay) {
        okay = fread(payload, 1, encoded_length, file) == encoded_length
            && fgetc(file) == EOF
            && tab_checksum(payload, encoded_length) == expected_checksum;
    }
    if (fclose(file) != 0) okay = false;
    if (!okay) return false;
    *payload_length = encoded_length;
    return true;
}

bool browser_tabs_hibernate(
    BrowserTabs *tabs, size_t index, const char *path)
{
    static const unsigned char magic[8] = {
        'T', 'F', 'T', 'A', 'B', '0', '1', 0
    };
    if (tabs == NULL || path == NULL || path[0] == '\0'
        || index >= tabs->count || index == tabs->active_index
        || tabs->slots[index].snapshot == NULL
        || tabs->slots[index].hibernated) return false;
    for (size_t at = 0; at < tabs->count; at++)
        if (tabs->slots[at].hibernated) return false;
    unsigned char *payload = tab_payload_create(tabs);
    if (payload == NULL) return false;
    size_t payload_length = 0;
    if (!tab_snapshot_encode(
            tabs->slots[index].snapshot, payload,
            BROWSER_TAB_HIBERNATION_FILE_LIMIT,
            &payload_length) || payload_length > UINT32_MAX) {
        budget_free(tabs->budget, payload);
        return false;
    }
    unsigned char header[16];
    memcpy(header, magic, sizeof(magic));
    size_t header_used = sizeof(magic);
    if (!tab_put_u32(header, sizeof(header), &header_used,
                     (uint32_t) payload_length)
        || !tab_put_u32(header, sizeof(header), &header_used,
                        tab_checksum(payload, payload_length))) {
        budget_free(tabs->budget, payload);
        return false;
    }
    char temporary[1024];
    int written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (written < 0 || (size_t) written >= sizeof(temporary)) {
        budget_free(tabs->budget, payload);
        return false;
    }
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        budget_free(tabs->budget, payload);
        return false;
    }
    bool okay = fwrite(header, 1, sizeof(header), file) == sizeof(header)
        && fwrite(payload, 1, payload_length, file) == payload_length
        && fflush(file) == 0 && ferror(file) == 0;
    if (fclose(file) != 0) okay = false;
    if (!okay) {
        remove(temporary);
        budget_free(tabs->budget, payload);
        return false;
    }
    if (rename(temporary, path) != 0) {
        if (remove(path) != 0 || rename(temporary, path) != 0) {
            remove(temporary);
            budget_free(tabs->budget, payload);
            return false;
        }
    }
    budget_free(tabs->budget, payload);
    BrowserTabSlot *slot = &tabs->slots[index];
    budget_free(tabs->budget, slot->snapshot);
    slot->snapshot = NULL;
    slot->hibernated = true;
    return true;
}

bool browser_tabs_rehydrate(
    BrowserTabs *tabs, size_t index, const char *path)
{
    if (tabs == NULL || path == NULL || path[0] == '\0'
        || index >= tabs->count || !tabs->slots[index].hibernated
        || tabs->slots[index].snapshot != NULL) return false;
    unsigned char *payload = tab_payload_create(tabs);
    if (payload == NULL) return false;
    size_t payload_length = 0;
    BrowserTabSnapshot *snapshot = tab_snapshot_create(tabs);
    if (snapshot == NULL
        || !tab_read_hibernation_payload(path, payload, &payload_length)
        || !tab_snapshot_decode(payload, payload_length, snapshot)) {
        budget_free(tabs->budget, snapshot);
        budget_free(tabs->budget, payload);
        return false;
    }
    budget_free(tabs->budget, payload);
    BrowserTabSlot *slot = &tabs->slots[index];
    slot->snapshot = snapshot;
    slot->hibernated = false;
    tab_slot_title_from_snapshot(slot);
    (void) remove(path);
    return true;
}

static bool tab_session_replace(const char *path, const char *temporary)
{
    char backup[1024];
    int written = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (written < 0 || (size_t) written >= sizeof(backup)) return false;
    (void) remove(backup);
    FILE *existing = fopen(path, "rb");
    bool had_existing = existing != NULL;
    if (existing != NULL) fclose(existing);
    if (had_existing && rename(path, backup) != 0) return false;
    if (rename(temporary, path) != 0) {
        if (had_existing) (void) rename(backup, path);
        return false;
    }
    if (had_existing) (void) remove(backup);
    return true;
}

bool browser_tabs_save_session(
    const BrowserTabs *tabs, const char *path, const char *hibernation_path)
{
    static const unsigned char magic[8] = {
        'T', 'F', 'S', 'E', 'S', '0', '1', 0
    };
    if (tabs == NULL || path == NULL || path[0] == '\0'
        || tabs->count == 0 || tabs->count > BROWSER_TAB_LIMIT
        || tabs->active_index >= tabs->count) return false;
    char temporary[1024];
    int written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (written < 0 || (size_t) written >= sizeof(temporary)) return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    unsigned char *payload = tab_payload_create(tabs);
    if (payload == NULL) {
        fclose(file);
        (void) remove(temporary);
        return false;
    }
    unsigned char header[16];
    memcpy(header, magic, sizeof(magic));
    size_t header_used = sizeof(magic);
    bool okay = tab_put_u32(
            header, sizeof(header), &header_used, (uint32_t) tabs->count)
        && tab_put_u32(
            header, sizeof(header), &header_used,
            (uint32_t) tabs->active_index)
        && fwrite(header, 1, sizeof(header), file) == sizeof(header);
    for (size_t index = 0; okay && index < tabs->count; index++) {
        const BrowserTabSlot *slot = &tabs->slots[index];
        size_t payload_length = 0;
        if (slot->hibernated) {
            okay = tab_read_hibernation_payload(
                hibernation_path, payload, &payload_length);
        } else {
            okay = slot->snapshot != NULL
                && tab_snapshot_encode(
                    slot->snapshot, payload,
                    BROWSER_TAB_HIBERNATION_FILE_LIMIT,
                    &payload_length);
        }
        size_t find_length = strnlen(
            slot->find_query, sizeof(slot->find_query));
        unsigned char record[16];
        size_t record_used = 0;
        okay = okay && payload_length <= UINT32_MAX
            && find_length <= BROWSER_TAB_FIND_QUERY_LIMIT
            && tab_put_u32(
                record, sizeof(record), &record_used,
                (uint32_t) payload_length)
            && tab_put_u32(
                record, sizeof(record), &record_used,
                tab_checksum(payload, payload_length))
            && tab_put_u32(
                record, sizeof(record), &record_used,
                (uint32_t) find_length)
            && tab_put_u32(
                record, sizeof(record), &record_used,
                tab_checksum(
                    (const unsigned char *) slot->find_query,
                    find_length))
            && fwrite(record, 1, sizeof(record), file) == sizeof(record)
            && fwrite(payload, 1, payload_length, file) == payload_length
            && fwrite(slot->find_query, 1, find_length, file) == find_length;
    }
    budget_free(tabs->budget, payload);
    if (okay) okay = fflush(file) == 0 && ferror(file) == 0;
    if (fclose(file) != 0) okay = false;
    if (!okay || !tab_session_replace(path, temporary)) {
        (void) remove(temporary);
        return false;
    }
    return true;
}

static bool tab_session_restore_file(BrowserTabs *tabs, const char *path)
{
    static const unsigned char magic[8] = {
        'T', 'F', 'S', 'E', 'S', '0', '1', 0
    };
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    unsigned char header[16];
    bool okay = fread(header, 1, sizeof(header), file) == sizeof(header)
        && memcmp(header, magic, sizeof(magic)) == 0;
    size_t header_used = sizeof(magic);
    uint32_t count = 0, active = 0;
    okay = okay
        && tab_get_u32(header, sizeof(header), &header_used, &count)
        && tab_get_u32(header, sizeof(header), &header_used, &active)
        && count != 0 && count <= BROWSER_TAB_LIMIT && active < count;
    BrowserTabs *staged = NULL;
    if (okay) {
        staged = budget_calloc_category(
            tabs->budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*staged));
        okay = staged != NULL;
        if (staged != NULL) {
            staged->budget = tabs->budget;
            staged->count = count;
            staged->active_index = active;
        }
    }
    unsigned char *payload = okay ? tab_payload_create(tabs) : NULL;
    if (okay && payload == NULL) okay = false;
    for (size_t index = 0; okay && index < count; index++) {
        unsigned char record[16];
        size_t record_used = 0;
        uint32_t payload_length = 0, expected_payload_checksum = 0;
        uint32_t find_length = 0, expected_find_checksum = 0;
        okay = fread(record, 1, sizeof(record), file) == sizeof(record)
            && tab_get_u32(
                record, sizeof(record), &record_used, &payload_length)
            && tab_get_u32(
                record, sizeof(record), &record_used,
                &expected_payload_checksum)
            && tab_get_u32(
                record, sizeof(record), &record_used, &find_length)
            && tab_get_u32(
                record, sizeof(record), &record_used,
                &expected_find_checksum)
            && payload_length <= BROWSER_TAB_HIBERNATION_FILE_LIMIT
            && find_length <= BROWSER_TAB_FIND_QUERY_LIMIT
            && fread(payload, 1, payload_length, file) == payload_length
            && tab_checksum(payload, payload_length)
                   == expected_payload_checksum;
        if (!okay) break;
        BrowserTabSlot *slot = &staged->slots[index];
        slot->snapshot = tab_snapshot_create(staged);
        okay = slot->snapshot != NULL
            && tab_snapshot_decode(
                payload, payload_length, slot->snapshot)
            && fread(slot->find_query, 1, find_length, file) == find_length;
        if (!okay) break;
        slot->find_query[find_length] = '\0';
        okay = tab_checksum(
                (const unsigned char *) slot->find_query, find_length)
            == expected_find_checksum;
        if (okay) tab_slot_title_from_snapshot(slot);
    }
    if (okay) okay = fgetc(file) == EOF;
    if (fclose(file) != 0) okay = false;
    if (!okay) {
        budget_free(tabs->budget, payload);
        browser_tabs_destroy(staged);
        return false;
    }
    budget_free(tabs->budget, payload);
    for (size_t index = 0; index < tabs->count; index++)
        tab_slot_release(tabs, &tabs->slots[index]);
    memcpy(tabs->slots, staged->slots, sizeof(tabs->slots));
    tabs->count = staged->count;
    tabs->active_index = staged->active_index;
    memset(staged->slots, 0, sizeof(staged->slots));
    staged->count = 0;
    browser_tabs_destroy(staged);
    return true;
}

bool browser_tabs_restore_session(BrowserTabs *tabs, const char *path)
{
    if (tabs == NULL || path == NULL || path[0] == '\0') return false;
    if (tab_session_restore_file(tabs, path)) return true;
    char backup[1024];
    int written = snprintf(backup, sizeof(backup), "%s.bak", path);
    return written >= 0 && (size_t) written < sizeof(backup)
        && tab_session_restore_file(tabs, backup);
}

size_t browser_tabs_resident_bytes(const BrowserTabs *tabs)
{
    if (tabs == NULL) return 0;
    size_t bytes = sizeof(*tabs);
    for (size_t index = 0; index < tabs->count; index++)
        if (tabs->slots[index].snapshot != NULL)
            bytes += sizeof(BrowserTabSnapshot);
    return bytes;
}

bool browser_tabs_add(
    BrowserTabs *tabs, const char *url, const char *title,
    size_t *created_index)
{
    if (tabs == NULL || tabs->count >= BROWSER_TAB_LIMIT) return false;
    BrowserTabSnapshot *snapshot = tab_snapshot_create(tabs);
    if (snapshot == NULL || !tab_reset(snapshot, url, title)) {
        budget_free(tabs->budget, snapshot);
        return false;
    }
    size_t index = tabs->count++;
    tabs->slots[index].snapshot = snapshot;
    tab_slot_title_from_snapshot(&tabs->slots[index]);
    tabs->active_index = index;
    if (created_index != NULL) *created_index = index;
    return true;
}

bool browser_tabs_remove(BrowserTabs *tabs, size_t index)
{
    if (tabs == NULL || tabs->count <= 1 || index >= tabs->count)
        return false;
    if (index + 1u < tabs->count) {
        tab_slot_release(tabs, &tabs->slots[index]);
        memmove(
            &tabs->slots[index], &tabs->slots[index + 1u],
            (tabs->count - index - 1u) * sizeof(tabs->slots[0]));
    } else {
        tab_slot_release(tabs, &tabs->slots[index]);
    }
    memset(&tabs->slots[tabs->count - 1u], 0, sizeof(tabs->slots[0]));
    tabs->count--;
    if (tabs->active_index > index) {
        tabs->active_index--;
    } else if (tabs->active_index == index
               && tabs->active_index >= tabs->count) {
        tabs->active_index = tabs->count - 1u;
    }
    return true;
}

bool browser_tabs_reset(
    BrowserTabs *tabs, size_t index, const char *url, const char *title)
{
    if (tabs == NULL || index >= tabs->count) return false;
    BrowserTabSlot *slot = &tabs->slots[index];
    if (slot->snapshot == NULL || slot->hibernated) return false;
    bool reset = tab_reset(slot->snapshot, url, title);
    if (reset) tab_slot_title_from_snapshot(slot);
    return reset;
}

bool browser_tabs_update_active_document(
    BrowserTabs *tabs, const char *url, const char *title)
{
    if (tabs == NULL || tabs->count == 0
        || tabs->active_index >= tabs->count
        || !tab_url_valid(url) || title == NULL) {
        return false;
    }
    BrowserTabSlot *slot = &tabs->slots[tabs->active_index];
    BrowserTabSnapshot *tab = slot->snapshot;
    if (tab == NULL || slot->hibernated) return false;
    if (tab->history_count == 0
        || tab->history_index >= tab->history_count) return false;
    BrowserTabHistoryEntry *entry = &tab->history[tab->history_index];
    snprintf(entry->url, sizeof(entry->url), "%s", url);
    snprintf(entry->title, sizeof(entry->title), "%s", title);
    tab_slot_title_from_snapshot(slot);
    return true;
}

const char *browser_tabs_active_find_query(const BrowserTabs *tabs)
{
    return tabs == NULL || tabs->count == 0
        || tabs->active_index >= tabs->count
        ? "" : tabs->slots[tabs->active_index].find_query;
}

bool browser_tabs_set_active_find_query(BrowserTabs *tabs,
                                        const char *query)
{
    if (tabs == NULL || query == NULL || tabs->count == 0
        || tabs->active_index >= tabs->count
        || strnlen(query, BROWSER_TAB_FIND_QUERY_LIMIT + 1u)
               > BROWSER_TAB_FIND_QUERY_LIMIT) return false;
    snprintf(tabs->slots[tabs->active_index].find_query,
             sizeof(tabs->slots[tabs->active_index].find_query), "%s",
             query);
    return true;
}

bool browser_tab_history_records(
    const BrowserTabSnapshot *tab,
    NavigationHistoryRecord records[BROWSER_TAB_HISTORY_LIMIT],
    size_t *count, size_t *current_index)
{
    if (tab == NULL || records == NULL || tab->history_count == 0
        || tab->history_count > BROWSER_TAB_HISTORY_LIMIT
        || tab->history_index >= tab->history_count) return false;
    for (size_t index = 0; index < tab->history_count; index++) {
        records[index] = (NavigationHistoryRecord) {
            .url = tab->history[index].url,
            .title = tab->history[index].title,
            .scroll_y = tab->history[index].scroll_y,
            .focus_kind = tab->history[index].focus_kind,
            .focus_index = tab->history[index].focus_index
        };
    }
    if (count != NULL) *count = tab->history_count;
    if (current_index != NULL) *current_index = tab->history_index;
    return true;
}

size_t browser_tabs_owned_bytes(void)
{
    return sizeof(BrowserTabs)
        + BROWSER_TAB_LIMIT * sizeof(BrowserTabSnapshot);
}
