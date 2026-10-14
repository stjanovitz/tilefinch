#ifndef TILEFINCH_YOUTUBE_PLAYLIST_H
#define TILEFINCH_YOUTUBE_PLAYLIST_H

#include "tilefinch/youtube_lite.h"

#define YOUTUBE_PLAYLIST_QUEUE_LIMIT 36u
#define YOUTUBE_PLAYLIST_VIEW_LIMIT 6u
typedef struct YoutubePlaylistQueue YoutubePlaylistQueue;

/* One RAM-only rolling window. Old positions can be revisited by bounded,
   cancellable replay of provider pages, not an unbounded token journal. */
YoutubePlaylistQueue *youtube_playlist_begin(Budget *, BrowserSession *,
    const char *page_url, size_t index, bool all, const char *expected_video);
void youtube_playlist_destroy(YoutubePlaylistQueue *);
bool youtube_playlist_pump(YoutubePlaylistQueue *, const FetchPumpQuota *);
bool youtube_playlist_select(YoutubePlaylistQueue *, size_t position);
bool youtube_playlist_step(YoutubePlaylistQueue *, int direction);
void youtube_playlist_retry(YoutubePlaylistQueue *);
/* Explicit initial thumbnail timestamp only; never applied to later entries. */
void youtube_playlist_set_initial_start(YoutubePlaylistQueue *, uint64_t start_us);
bool youtube_playlist_take_video(YoutubePlaylistQueue *, char *url, size_t capacity);
size_t youtube_playlist_position(const YoutubePlaylistQueue *);
size_t youtube_playlist_view(const YoutubePlaylistQueue *, YoutubePlaylistEntry *, size_t);
bool youtube_playlist_has_next(const YoutubePlaylistQueue *);
bool youtube_playlist_loading(const YoutubePlaylistQueue *);
bool youtube_playlist_selecting(const YoutubePlaylistQueue *);
bool youtube_playlist_failed(const YoutubePlaylistQueue *);
bool youtube_playlist_can_retry(const YoutubePlaylistQueue *);
const char *youtube_playlist_error(const YoutubePlaylistQueue *);
/* Same transactional publication boundary used by the loader and tests. */
bool youtube_playlist_accept_page(YoutubePlaylistQueue *, const YoutubePlaylistPage *);

#endif
