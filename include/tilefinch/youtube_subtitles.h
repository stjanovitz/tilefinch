#ifndef TILEFINCH_YOUTUBE_SUBTITLES_H
#define TILEFINCH_YOUTUBE_SUBTITLES_H

#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

#define YOUTUBE_SUBTITLE_BODY_LIMIT (256u * 1024u)
#define YOUTUBE_SUBTITLE_CUE_LIMIT 512u
#define YOUTUBE_SUBTITLE_TEXT_CAPACITY 160u

typedef struct YoutubeSubtitleDocument YoutubeSubtitleDocument;

YoutubeSubtitleDocument *youtube_subtitles_parse_vtt(
    Budget *budget, const unsigned char *body, size_t length);
void youtube_subtitles_destroy(YoutubeSubtitleDocument *document);
const char *youtube_subtitles_text_at(
    const YoutubeSubtitleDocument *document, uint64_t time_us,
    size_t *cursor);
size_t youtube_subtitles_cue_count(
    const YoutubeSubtitleDocument *document);

#endif
