#include "tilefinch/youtube_subtitles.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    uint64_t start_us;
    uint64_t end_us;
    char text[YOUTUBE_SUBTITLE_TEXT_CAPACITY];
} YoutubeSubtitleCue;

struct YoutubeSubtitleDocument {
    Budget *budget;
    size_t count;
    YoutubeSubtitleCue cues[];
};

static bool subtitle_line(
    const unsigned char *body, size_t length, size_t *cursor,
    const unsigned char **line, size_t *line_length)
{
    if (body == NULL || cursor == NULL || line == NULL
        || line_length == NULL || *cursor >= length) return false;
    size_t start = *cursor;
    size_t at = start;
    while (at < length && body[at] != '\r' && body[at] != '\n') at++;
    *line = body + start;
    *line_length = at - start;
    if (at < length && body[at] == '\r') at++;
    if (at < length && body[at] == '\n') at++;
    *cursor = at;
    return true;
}

static bool subtitle_digits(
    const unsigned char *text, size_t length, size_t *cursor,
    unsigned digits, unsigned *value)
{
    if (text == NULL || cursor == NULL || value == NULL
        || digits == 0 || *cursor > length || digits > length - *cursor)
        return false;
    unsigned result = 0;
    for (unsigned at = 0; at < digits; at++) {
        unsigned char byte = text[*cursor + at];
        if (byte < '0' || byte > '9') return false;
        result = result * 10u + (unsigned) (byte - '0');
    }
    *cursor += digits;
    *value = result;
    return true;
}

static bool subtitle_timestamp(
    const unsigned char *text, size_t length, uint64_t *time_us)
{
    if (text == NULL || time_us == NULL || length < 9u) return false;
    size_t cursor = 0;
    unsigned first = 0, minutes = 0, seconds = 0, millis = 0;
    unsigned first_digits = 0;
    while (cursor + first_digits < length
           && text[cursor + first_digits] >= '0'
           && text[cursor + first_digits] <= '9'
           && first_digits < 3u) first_digits++;
    if (first_digits == 0
        || !subtitle_digits(text, length, &cursor, first_digits, &first)
        || cursor >= length || text[cursor++] != ':') return false;
    if (cursor + 3u < length && text[cursor + 2u] == ':') {
        if (!subtitle_digits(text, length, &cursor, 2u, &minutes)
            || cursor >= length || text[cursor++] != ':') return false;
    } else {
        minutes = first;
        first = 0;
    }
    if (!subtitle_digits(text, length, &cursor, 2u, &seconds)
        || cursor >= length
        || (text[cursor] != '.' && text[cursor] != ',')) return false;
    cursor++;
    if (!subtitle_digits(text, length, &cursor, 3u, &millis)
        || minutes >= 60u || seconds >= 60u) return false;
    uint64_t total_seconds = (uint64_t) first * 3600u
        + (uint64_t) minutes * 60u + seconds;
    *time_us = total_seconds * UINT64_C(1000000)
        + (uint64_t) millis * 1000u;
    return true;
}

static bool subtitle_timing(
    const unsigned char *line, size_t length,
    uint64_t *start_us, uint64_t *end_us)
{
    static const unsigned char arrow[] = " --> ";
    if (line == NULL || length < sizeof(arrow) - 1u) return false;
    size_t arrow_at = SIZE_MAX;
    for (size_t at = 0; at + sizeof(arrow) - 1u <= length; at++) {
        if (memcmp(line + at, arrow, sizeof(arrow) - 1u) == 0) {
            arrow_at = at;
            break;
        }
    }
    if (arrow_at == SIZE_MAX) return false;
    size_t end_start = arrow_at + sizeof(arrow) - 1u;
    size_t end_length = 0;
    while (end_start + end_length < length
           && line[end_start + end_length] != ' ') end_length++;
    return subtitle_timestamp(line, arrow_at, start_us)
        && subtitle_timestamp(line + end_start, end_length, end_us)
        && *end_us > *start_us;
}

static size_t subtitle_count_timings(
    const unsigned char *body, size_t length)
{
    size_t cursor = 0, count = 0;
    const unsigned char *line;
    size_t line_length;
    while (count < YOUTUBE_SUBTITLE_CUE_LIMIT
           && subtitle_line(body, length, &cursor, &line, &line_length)) {
        uint64_t start_us, end_us;
        if (subtitle_timing(line, line_length, &start_us, &end_us)) count++;
    }
    return count;
}

static void subtitle_append_text(
    char output[YOUTUBE_SUBTITLE_TEXT_CAPACITY], size_t *used,
    const unsigned char *line, size_t length)
{
    if (output == NULL || used == NULL || line == NULL) return;
    bool in_tag = false;
    if (*used != 0 && *used + 1u < YOUTUBE_SUBTITLE_TEXT_CAPACITY)
        output[(*used)++] = ' ';
    for (size_t at = 0; at < length
         && *used + 1u < YOUTUBE_SUBTITLE_TEXT_CAPACITY; at++) {
        unsigned char byte = line[at];
        if (byte == '<') { in_tag = true; continue; }
        if (byte == '>' && in_tag) { in_tag = false; continue; }
        if (in_tag) continue;
        const struct { const char *entity; char value; } entities[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
            {"&quot;", '"'}, {"&#39;", '\''}
        };
        bool decoded = false;
        for (size_t entity = 0;
             entity < sizeof(entities) / sizeof(entities[0]); entity++) {
            size_t entity_length = strlen(entities[entity].entity);
            if (entity_length <= length - at
                && memcmp(line + at, entities[entity].entity,
                          entity_length) == 0) {
                output[(*used)++] = entities[entity].value;
                at += entity_length - 1u;
                decoded = true;
                break;
            }
        }
        if (!decoded) output[(*used)++] = (char) byte;
    }
    output[*used] = '\0';
}

YoutubeSubtitleDocument *youtube_subtitles_parse_vtt(
    Budget *budget, const unsigned char *body, size_t length)
{
    if (budget == NULL || body == NULL || length == 0
        || length > YOUTUBE_SUBTITLE_BODY_LIMIT) return NULL;
    size_t count = subtitle_count_timings(body, length);
    if (count == 0 || count > YOUTUBE_SUBTITLE_CUE_LIMIT) return NULL;
    if (count > (SIZE_MAX - sizeof(YoutubeSubtitleDocument))
                    / sizeof(YoutubeSubtitleCue)) return NULL;
    size_t bytes = sizeof(YoutubeSubtitleDocument)
        + count * sizeof(YoutubeSubtitleCue);
    YoutubeSubtitleDocument *document = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, bytes);
    if (document == NULL) return NULL;
    memset(document, 0, bytes);
    document->budget = budget;
    size_t cursor = 0;
    const unsigned char *line;
    size_t line_length;
    while (document->count < count
           && subtitle_line(body, length, &cursor, &line, &line_length)) {
        uint64_t start_us, end_us;
        if (!subtitle_timing(line, line_length, &start_us, &end_us)) continue;
        YoutubeSubtitleCue *cue = &document->cues[document->count];
        cue->start_us = start_us;
        cue->end_us = end_us;
        size_t used = 0;
        while (subtitle_line(body, length, &cursor, &line, &line_length)
               && line_length != 0)
            subtitle_append_text(cue->text, &used, line, line_length);
        if (cue->text[0] != '\0') document->count++;
    }
    if (document->count == 0) {
        budget_free(budget, document);
        return NULL;
    }
    return document;
}

void youtube_subtitles_destroy(YoutubeSubtitleDocument *document)
{
    if (document != NULL) budget_free(document->budget, document);
}

const char *youtube_subtitles_text_at(
    const YoutubeSubtitleDocument *document, uint64_t time_us,
    size_t *cursor)
{
    if (document == NULL || cursor == NULL || document->count == 0) return NULL;
    size_t at = *cursor < document->count ? *cursor : 0;
    if (at != 0 && time_us < document->cues[at].start_us) at = 0;
    while (at < document->count && time_us >= document->cues[at].end_us) at++;
    *cursor = at;
    if (at >= document->count || time_us < document->cues[at].start_us)
        return NULL;
    return document->cues[at].text;
}

size_t youtube_subtitles_cue_count(
    const YoutubeSubtitleDocument *document)
{
    return document == NULL ? 0u : document->count;
}
