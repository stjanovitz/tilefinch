#ifndef TILEFINCH_YOUTUBE_SUBTITLES_H
#define TILEFINCH_YOUTUBE_SUBTITLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

#define YOUTUBE_SUBTITLE_WIRE_LIMIT (4u * 1024u * 1024u)
#define YOUTUBE_SUBTITLE_CUE_LIMIT 2048u
/* A cue's visible text, in bytes with its terminator: the player's caption
   buffer (PSP_UI_MEDIA_SUBTITLE_TEXT_CAPACITY), enough for two rows of a
   script at two or three bytes per letter. Longer text is cut on a
   character boundary. */
#define YOUTUBE_SUBTITLE_TEXT_CAPACITY 256u
#define YOUTUBE_SUBTITLE_TEXT_POOL_LIMIT (128u * 1024u)

typedef struct YoutubeSubtitleDocument YoutubeSubtitleDocument;
typedef struct YoutubeSubtitleBuilder YoutubeSubtitleBuilder;

/* Incremental parsing keeps a multi-megabyte caption response out of the PSP
   heap. The retained document remains a fixed cue/index plus text-pool bound;
   minimum_time_us lets a track selected mid-video spend that bound on useful
   current and future cues. */
YoutubeSubtitleBuilder *youtube_subtitles_builder_create(
    Budget *budget, uint64_t minimum_time_us);
bool youtube_subtitles_builder_feed(
    YoutubeSubtitleBuilder *builder, const unsigned char *bytes,
    size_t length);
/* True once the cue or text-pool bound refused a cue. No later cue is
   kept after that (the window ends at the last cue before the refusal), so
   the caller can stop the transfer and finish the builder. */
bool youtube_subtitles_builder_full(const YoutubeSubtitleBuilder *builder);
YoutubeSubtitleDocument *youtube_subtitles_builder_finish(
    YoutubeSubtitleBuilder *builder);
void youtube_subtitles_builder_destroy(YoutubeSubtitleBuilder *builder);

YoutubeSubtitleDocument *youtube_subtitles_parse_vtt(
    Budget *budget, const unsigned char *body, size_t length);
void youtube_subtitles_destroy(YoutubeSubtitleDocument *document);
const char *youtube_subtitles_text_at(
    const YoutubeSubtitleDocument *document, uint64_t time_us,
    size_t *cursor);
size_t youtube_subtitles_cue_count(
    const YoutubeSubtitleDocument *document);

/* The playback time range a document covers. start_us is the builder's
   minimum_time_us and end_us the end of its last retained cue. truncated
   means the cue or text-pool bound stopped retention there, so the resource
   holds later cues that need another window. */
typedef struct {
    uint64_t start_us;
    uint64_t end_us;
    bool truncated;
} YoutubeSubtitleWindow;
bool youtube_subtitles_window(const YoutubeSubtitleDocument *document,
                              YoutubeSubtitleWindow *window);

typedef enum {
    YOUTUBE_SUBTITLE_WINDOW_KEEP = 0,
    /* Playback is before the window: rebuild from the new position. */
    YOUTUBE_SUBTITLE_WINDOW_RELOAD,
    /* A truncated window ends within lead_us of playback (or before it):
       fetch a later window while this one keeps showing. */
    YOUTUBE_SUBTITLE_WINDOW_EXTEND
} YoutubeSubtitleWindowAction;
YoutubeSubtitleWindowAction youtube_subtitles_window_action(
    const YoutubeSubtitleDocument *document, uint64_t time_us,
    uint64_t lead_us);

/* Retry a failed extension without discarding the incumbent captions.
   Three attempts per retained window, separated by five monotonic seconds.
   Call only with no request in flight; zero-initialize on track/seek reset. */
typedef struct {
    uint64_t window_end_us;
    uint64_t attempted_us;
    unsigned attempts;
} YoutubeSubtitleRetry;
bool youtube_subtitles_retry_extension(YoutubeSubtitleRetry *retry,
    uint64_t window_end_us, uint64_t monotonic_us);

#endif
