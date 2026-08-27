#ifndef TILEFINCH_YOUTUBE_SUBTITLES_H
#define TILEFINCH_YOUTUBE_SUBTITLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

#define YOUTUBE_SUBTITLE_WIRE_LIMIT (4u * 1024u * 1024u)
#define YOUTUBE_SUBTITLE_CUE_LIMIT 2048u
#define YOUTUBE_SUBTITLE_TEXT_CAPACITY 160u
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

#endif
