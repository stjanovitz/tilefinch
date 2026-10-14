#include "tilefinch/youtube_playlist.h"
#include "tilefinch/url.h"
#include "youtube_internal.h"
#include <stdio.h>
#include <string.h>

struct YoutubePlaylistQueue {
    Budget *budget;
    BrowserSession *session;
    YoutubeLiteLoadJob *job;
    YoutubePlaylistEntry entries[YOUTUBE_PLAYLIST_QUEUE_LIMIT];
    size_t first, count, current, target; /* target is page-relative until the first publication. */
    char root[2048], next[2048], request[2048];
    char error[160];
    char expected_video[32];
    bool initialized, selecting, relative, ready, failed;
    unsigned pages;
    uint64_t initial_start_us;
};

YoutubePlaylistQueue *youtube_playlist_begin(Budget *budget, BrowserSession *session,
    const char *url, size_t index, bool all, const char *expected_video)
{
    char root[2048];
    if (!budget || !session || !url || strnlen(url, sizeof(root)) >= sizeof(root)
        || index >= YOUTUBE_LITE_MAXIMUM_RESULTS || !youtube_lite_playlist_root_url(url, root, sizeof(root))
        || (expected_video && strnlen(expected_video, 32) >= 32)) return NULL;
    YoutubePlaylistQueue *q = budget_calloc_category(budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*q));
    if (!q) return NULL;
    if (all) { url = root; index = 0; expected_video = NULL; }
    q->budget = budget; q->session = session;
    memcpy(q->root, root, strlen(root) + 1);
    snprintf(q->request, sizeof(q->request), "%s", url);
    q->relative = q->selecting = true;
    q->target = index;
    if (expected_video) snprintf(q->expected_video, sizeof(q->expected_video), "%s", expected_video);
    return q;
}

void youtube_playlist_destroy(YoutubePlaylistQueue *q)
{
    if (!q) return;
    youtube_lite_load_destroy(q->job);
    budget_free(q->budget, q);
}

static bool playlist_fail(YoutubePlaylistQueue *q, const char *error)
{
    q->failed = true;
    snprintf(q->error, sizeof(q->error), "%s", error);
    return false;
}

bool youtube_playlist_accept_page(YoutubePlaylistQueue *q, const YoutubePlaylistPage *p)
{
    if (!q || !p || p->count > YOUTUBE_LITE_MAXIMUM_RESULTS
        || p->first > 65535u || p->count > 65536u - p->first
        || !memchr(p->next_url, 0, sizeof(p->next_url))) return false;
    /* Never follow a service-supplied arbitrary URL or another playlist. */
    char root[2048];
    if (p->next_url[0] && (!youtube_lite_playlist_root_url(p->next_url, root, sizeof(root))
        || strcmp(root, q->root) || !strcmp(p->next_url, q->request)))
        return playlist_fail(q, "Playlist continuation did not advance");
    for (size_t i = 0; i < p->count; i++) {
        const YoutubePlaylistEntry *e = &p->entries[i];
        size_t n = strnlen(e->video_id, sizeof(e->video_id));
        if (!n || n == sizeof(e->video_id) || !memchr(e->title, 0, sizeof(e->title))) return false;
        if (!youtube_api_token_safe(e->video_id, false)) return false;
    }
    if (q->initialized && p->first != q->first + q->count)
        return playlist_fail(q, "Playlist changed; return and start it again");
    if (q->relative) {
        if (q->target >= p->count) return playlist_fail(q, "Selected playlist entry is unavailable");
        if (q->expected_video[0] && strcmp(q->expected_video, p->entries[q->target].video_id))
            return playlist_fail(q, "Playlist changed; return and start it again");
        q->target += p->first;
        q->relative = false;
    }
    if (!q->initialized) { q->first = p->first; q->count = 0; q->initialized = true; }
    size_t drop = q->count + p->count > YOUTUBE_PLAYLIST_QUEUE_LIMIT
        ? q->count + p->count - YOUTUBE_PLAYLIST_QUEUE_LIMIT : 0;
    /* Prefetch must never evict the currently playing position. */
    if (!q->selecting && drop && q->current < q->first + drop) return false;
    memmove(q->entries, q->entries + drop, (q->count - drop) * sizeof(q->entries[0]));
    q->first += drop; q->count -= drop;
    memcpy(q->entries + q->count, p->entries, p->count * sizeof(p->entries[0]));
    q->count += p->count;
    snprintf(q->next, sizeof(q->next), "%s", p->next_url);
    q->request[0] = 0; q->failed = false; q->error[0] = 0;
    if (q->selecting && q->target >= q->first && q->target - q->first < q->count) {
        q->current = q->target; q->selecting = false; q->ready = true;
    } else if (q->selecting && !q->next[0])
        return playlist_fail(q, "No further playlist entries are available");
    return true;
}

bool youtube_playlist_select(YoutubePlaylistQueue *q, size_t position)
{
    if (!q || position > 65535u) return false;
    if (q->initialized && position >= q->first + q->count && !q->next[0]) return false;
    if (q->job) { youtube_lite_load_destroy(q->job); q->job = NULL; }
    q->initial_start_us = 0;
    q->target = position; q->selecting = true; q->relative = false;
    q->failed = false; q->error[0] = 0; q->ready = false; q->pages = 0;
    if (q->initialized && position >= q->first && position - q->first < q->count) {
        q->current = position; q->selecting = false; q->ready = true; q->request[0] = 0;
    } else if (!q->initialized || position < q->first) {
        q->initialized = false; q->count = 0;
        snprintf(q->request, sizeof(q->request), "%s", q->root);
    } else if (q->next[0]) snprintf(q->request, sizeof(q->request), "%s", q->next);
    else return playlist_fail(q, "No further playlist entries are available");
    return true;
}

bool youtube_playlist_step(YoutubePlaylistQueue *q, int direction)
{
    if (!q || !q->initialized || (direction < 0 && !q->current)
        || (direction > 0 && !youtube_playlist_has_next(q))) return false;
    return youtube_playlist_select(q, direction < 0 ? q->current - 1 : q->current + 1);
}

void youtube_playlist_retry(YoutubePlaylistQueue *q)
{
    /* A terminal empty page has no request to retry. Keep its failure visible
       instead of turning it into an unscheduled, forever-loading selection. */
    if (q && q->failed && (q->request[0] || q->next[0])) {
        youtube_lite_forget_playlist_page(q->session,
            q->request[0] ? q->request : q->next);
        q->failed = false; q->error[0] = 0; q->pages = 0;
    }
}

void youtube_playlist_set_initial_start(YoutubePlaylistQueue *q, uint64_t start_us)
{
    if (q && !q->initialized) q->initial_start_us = start_us;
}

bool youtube_playlist_pump(YoutubePlaylistQueue *q, const FetchPumpQuota *quota)
{
    if (!q || q->failed) return false;
    if (!q->request[0] && q->next[0] && (q->selecting
        || (q->initialized && q->current >= q->first
            && q->first + q->count - q->current <= 3u)))
        snprintf(q->request, sizeof(q->request), "%s", q->next);
    if (!q->request[0]) return false;
    bool started = false;
    if (!q->job) {
        if (++q->pages > 5462u) { playlist_fail(q, "Playlist navigation limit reached"); return true; }
        YoutubePlaylistPage page;
        if (youtube_lite_copy_playlist(q->session, q->request, &page)) {
            if (!youtube_playlist_accept_page(q, &page) && !q->failed)
                playlist_fail(q, "Playlist page is unavailable");
            return true;
        }
        q->job = youtube_lite_load_begin(q->budget, q->session, q->request,
            YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES, 30000, q->error, sizeof(q->error));
        if (!q->job) { q->failed = true; return true; }
        started = true;
    }
    YoutubeLiteLoadStatus status = youtube_lite_load_pump(q->job, quota);
    if (status == YOUTUBE_LITE_LOAD_PENDING) return started;
    YoutubeLiteDocument document = {0};
    if (status == YOUTUBE_LITE_LOAD_SUCCEEDED
        && youtube_lite_load_take_document(q->job, &document) && document.playlist) {
        if (!youtube_playlist_accept_page(q, document.playlist) && !q->failed)
            playlist_fail(q, "Playlist page is unavailable");
    } else {
        const char *error = youtube_lite_load_error(q->job);
        playlist_fail(q, error && error[0] ? error : "Playlist request failed");
    }
    youtube_lite_document_destroy(&document);
    youtube_lite_load_destroy(q->job); q->job = NULL;
    return true;
}

bool youtube_playlist_take_video(YoutubePlaylistQueue *q, char *url, size_t capacity)
{
    if (!q || !url || !capacity || !q->ready || q->current < q->first || q->current - q->first >= q->count) return false;
    int n = q->initial_start_us
        ? snprintf(url, capacity, "https://www.youtube.com/watch?v=%s&t=%llu",
            q->entries[q->current - q->first].video_id,
            (unsigned long long) (q->initial_start_us / UINT64_C(1000000)))
        : snprintf(url, capacity, "https://www.youtube.com/watch?v=%s", q->entries[q->current - q->first].video_id);
    if (n <= 0 || (size_t) n >= capacity) return false;
    q->initial_start_us = 0;
    q->ready = false; return true;
}

size_t youtube_playlist_position(const YoutubePlaylistQueue *q) { return q ? q->current : 0; }
size_t youtube_playlist_view(const YoutubePlaylistQueue *q, YoutubePlaylistEntry *out, size_t capacity)
{
    if (!q || !out || !capacity || !q->initialized || q->current < q->first || q->current - q->first >= q->count) return 0;
    size_t n = q->first + q->count - q->current;
    if (n > capacity) n = capacity;
    memcpy(out, q->entries + q->current - q->first, n * sizeof(*out)); return n;
}
bool youtube_playlist_has_next(const YoutubePlaylistQueue *q)
{ return q && q->initialized && (q->current + 1 < q->first + q->count || q->next[0]); }
bool youtube_playlist_loading(const YoutubePlaylistQueue *q)
{ return q && !q->failed && (q->job || q->request[0] || q->selecting); }
bool youtube_playlist_selecting(const YoutubePlaylistQueue *q) { return q && q->selecting; }
bool youtube_playlist_failed(const YoutubePlaylistQueue *q) { return q && q->failed; }
bool youtube_playlist_can_retry(const YoutubePlaylistQueue *q)
{ return q && q->failed && (q->request[0] || q->next[0]); }
const char *youtube_playlist_error(const YoutubePlaylistQueue *q) { return q ? q->error : ""; }
