#include "tilefinch/youtube_subtitles.h"

#include <stdbool.h>
#include <string.h>

#define YOUTUBE_SUBTITLE_LINE_CAPACITY 512u
#define YOUTUBE_SUBTITLE_TEXT_BLOCK_BYTES 4096u
#define YOUTUBE_SUBTITLE_TEXT_BLOCK_LIMIT \
    (YOUTUBE_SUBTITLE_TEXT_POOL_LIMIT / YOUTUBE_SUBTITLE_TEXT_BLOCK_BYTES)
#define YOUTUBE_SUBTITLE_TRANSITION_BRIDGE_MS 1200u

typedef struct {
    uint32_t start_ms;
    uint32_t end_ms;
    /* Five high bits select one of 32 blocks; the low 12 are its offset. */
    uint16_t text_location;
    uint16_t text_length;
} YoutubeSubtitleCue;

struct YoutubeSubtitleDocument {
    Budget *budget;
    size_t count;
    YoutubeSubtitleCue cues[YOUTUBE_SUBTITLE_CUE_LIMIT];
    char *text_blocks[YOUTUBE_SUBTITLE_TEXT_BLOCK_LIMIT];
    uint16_t text_block_used;
    uint8_t text_block_count;
};

struct YoutubeSubtitleBuilder {
    Budget *budget;
    YoutubeSubtitleDocument *document;
    uint64_t minimum_time_us;
    uint64_t start_us;
    uint64_t end_us;
    size_t wire_bytes;
    size_t line_length;
    size_t cue_text_length;
    bool have_timing;
    bool line_overflow;
    bool previous_was_cr;
    bool truncated;
    unsigned char line[YOUTUBE_SUBTITLE_LINE_CAPACITY];
    char cue_text[YOUTUBE_SUBTITLE_TEXT_CAPACITY];
};

_Static_assert(sizeof(YoutubeSubtitleDocument) < 32u * 1024u,
               "subtitle document exceeded its PSP bound");
_Static_assert(YOUTUBE_SUBTITLE_TEXT_BLOCK_LIMIT <= UINT8_MAX,
               "subtitle text block index overflow");

static unsigned subtitle_cue_block(const YoutubeSubtitleCue *cue)
{
    return cue == NULL ? 0u : (unsigned) (cue->text_location >> 12);
}

static size_t subtitle_cue_offset(const YoutubeSubtitleCue *cue)
{
    return cue == NULL ? 0u : (size_t) (cue->text_location & 0x0fffu);
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
        /* Timed-text uses zero-width spaces around styled fragments. They
           are presentation metadata, not caption glyphs. */
        if (at + 2u < length && byte == 0xe2u
            && line[at + 1u] == 0x80u && line[at + 2u] == 0x8bu) {
            at += 2u;
            continue;
        }
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

static void subtitle_builder_reset_cue(YoutubeSubtitleBuilder *builder)
{
    if (builder == NULL) return;
    builder->have_timing = false;
    builder->start_us = 0;
    builder->end_us = 0;
    builder->cue_text_length = 0;
    builder->cue_text[0] = '\0';
}

static void subtitle_builder_finish_cue(YoutubeSubtitleBuilder *builder)
{
    if (builder == NULL || !builder->have_timing) return;
    YoutubeSubtitleDocument *document = builder->document;
    size_t first = 0;
    size_t last = builder->cue_text_length;
    while (first < last
           && (builder->cue_text[first] == ' '
               || builder->cue_text[first] == '\t')) first++;
    while (last > first
           && (builder->cue_text[last - 1u] == ' '
               || builder->cue_text[last - 1u] == '\t')) last--;
    size_t length = last - first;
    if (length == 0 || builder->end_us <= builder->minimum_time_us) {
        subtitle_builder_reset_cue(builder);
        return;
    }
    uint64_t start_ms64 = builder->start_us / 1000u;
    uint64_t end_ms64 = (builder->end_us + 999u) / 1000u;
    if (start_ms64 > UINT32_MAX || end_ms64 > UINT32_MAX) {
        builder->truncated = true;
        subtitle_builder_reset_cue(builder);
        return;
    }
    uint32_t start_ms = (uint32_t) start_ms64;
    uint32_t end_ms = (uint32_t) end_ms64;
    if (document->count != 0) {
        const YoutubeSubtitleCue *previous =
            &document->cues[document->count - 1u];
        unsigned previous_block = subtitle_cue_block(previous);
        if (previous->start_ms == start_ms && previous->end_ms == end_ms
            && previous->text_length == length
            && previous_block < document->text_block_count
            && memcmp(document->text_blocks[previous_block]
                          + subtitle_cue_offset(previous),
                      builder->cue_text + first, length) == 0) {
            subtitle_builder_reset_cue(builder);
            return;
        }
    }
    if (document->count >= YOUTUBE_SUBTITLE_CUE_LIMIT) {
        builder->truncated = true;
        subtitle_builder_reset_cue(builder);
        return;
    }
    if (document->text_block_count == 0u
        || length + 1u > YOUTUBE_SUBTITLE_TEXT_BLOCK_BYTES
                              - document->text_block_used) {
        if (document->text_block_count
                >= YOUTUBE_SUBTITLE_TEXT_BLOCK_LIMIT) {
            builder->truncated = true;
            subtitle_builder_reset_cue(builder);
            return;
        }
        char *block = budget_malloc_category(
            document->budget, BUDGET_CATEGORY_SESSION,
            YOUTUBE_SUBTITLE_TEXT_BLOCK_BYTES);
        if (block == NULL) {
            builder->truncated = true;
            subtitle_builder_reset_cue(builder);
            return;
        }
        document->text_blocks[document->text_block_count++] = block;
        document->text_block_used = 0u;
    }
    YoutubeSubtitleCue *cue = &document->cues[document->count++];
    cue->start_ms = start_ms;
    cue->end_ms = end_ms;
    unsigned text_block = (unsigned) document->text_block_count - 1u;
    cue->text_location = (uint16_t) (
        (text_block << 12) | document->text_block_used);
    cue->text_length = (uint16_t) length;
    char *destination = document->text_blocks[text_block]
        + document->text_block_used;
    memcpy(destination,
           builder->cue_text + first, length);
    destination[length] = '\0';
    document->text_block_used += (uint16_t) (length + 1u);
    subtitle_builder_reset_cue(builder);
}

static void subtitle_builder_line(
    YoutubeSubtitleBuilder *builder,
    const unsigned char *line, size_t length)
{
    if (builder == NULL) return;
    if (length == 0) {
        subtitle_builder_finish_cue(builder);
        return;
    }
    uint64_t start_us = 0, end_us = 0;
    if (subtitle_timing(line, length, &start_us, &end_us)) {
        subtitle_builder_finish_cue(builder);
        builder->have_timing = true;
        builder->start_us = start_us;
        builder->end_us = end_us;
        return;
    }
    if (builder->have_timing)
        subtitle_append_text(
            builder->cue_text, &builder->cue_text_length, line, length);
}

YoutubeSubtitleBuilder *youtube_subtitles_builder_create(
    Budget *budget, uint64_t minimum_time_us)
{
    if (budget == NULL) return NULL;
    YoutubeSubtitleBuilder *builder = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, sizeof(*builder));
    if (builder == NULL) return NULL;
    memset(builder, 0, sizeof(*builder));
    builder->document = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, sizeof(*builder->document));
    if (builder->document == NULL) {
        budget_free(budget, builder);
        return NULL;
    }
    builder->budget = budget;
    builder->document->budget = budget;
    builder->document->count = 0;
    builder->document->text_block_count = 0u;
    builder->document->text_block_used = 0u;
    builder->minimum_time_us = minimum_time_us;
    return builder;
}

bool youtube_subtitles_builder_feed(
    YoutubeSubtitleBuilder *builder, const unsigned char *bytes,
    size_t length)
{
    if (builder == NULL || (bytes == NULL && length != 0)
        || length > YOUTUBE_SUBTITLE_WIRE_LIMIT - builder->wire_bytes)
        return false;
    builder->wire_bytes += length;
    for (size_t at = 0; at < length; at++) {
        unsigned char byte = bytes[at];
        if (byte == '\n' && builder->previous_was_cr) {
            builder->previous_was_cr = false;
            continue;
        }
        if (byte == '\r' || byte == '\n') {
            if (!builder->line_overflow)
                subtitle_builder_line(
                    builder, builder->line, builder->line_length);
            builder->line_length = 0;
            builder->line_overflow = false;
            builder->previous_was_cr = byte == '\r';
            continue;
        }
        builder->previous_was_cr = false;
        if (builder->line_length < sizeof(builder->line))
            builder->line[builder->line_length++] = byte;
        else
            builder->line_overflow = true;
    }
    return true;
}

YoutubeSubtitleDocument *youtube_subtitles_builder_finish(
    YoutubeSubtitleBuilder *builder)
{
    if (builder == NULL) return NULL;
    if (builder->line_length != 0 && !builder->line_overflow)
        subtitle_builder_line(builder, builder->line, builder->line_length);
    subtitle_builder_finish_cue(builder);
    YoutubeSubtitleDocument *document = builder->document;
    builder->document = NULL;
    Budget *budget = builder->budget;
    budget_free(budget, builder);
    if (document->count == 0) {
        youtube_subtitles_destroy(document);
        return NULL;
    }
    return document;
}

void youtube_subtitles_builder_destroy(YoutubeSubtitleBuilder *builder)
{
    if (builder == NULL) return;
    Budget *budget = builder->budget;
    youtube_subtitles_destroy(builder->document);
    budget_free(budget, builder);
}

YoutubeSubtitleDocument *youtube_subtitles_parse_vtt(
    Budget *budget, const unsigned char *body, size_t length)
{
    if (budget == NULL || body == NULL || length == 0
        || length > YOUTUBE_SUBTITLE_WIRE_LIMIT) return NULL;
    YoutubeSubtitleBuilder *builder = youtube_subtitles_builder_create(
        budget, 0);
    if (builder == NULL) return NULL;
    if (!youtube_subtitles_builder_feed(builder, body, length)) {
        youtube_subtitles_builder_destroy(builder);
        return NULL;
    }
    return youtube_subtitles_builder_finish(builder);
}

void youtube_subtitles_destroy(YoutubeSubtitleDocument *document)
{
    if (document == NULL) return;
    Budget *budget = document->budget;
    size_t count = document->text_block_count;
    if (count > YOUTUBE_SUBTITLE_TEXT_BLOCK_LIMIT)
        count = YOUTUBE_SUBTITLE_TEXT_BLOCK_LIMIT;
    for (size_t at = 0; at < count; at++)
        budget_free(budget, document->text_blocks[at]);
    budget_free(budget, document);
}

const char *youtube_subtitles_text_at(
    const YoutubeSubtitleDocument *document, uint64_t time_us,
    size_t *cursor)
{
    if (document == NULL || cursor == NULL || document->count == 0)
        return NULL;
    uint64_t time_ms64 = time_us / 1000u;
    if (time_ms64 > UINT32_MAX) return NULL;
    uint32_t time_ms = (uint32_t) time_ms64;
    size_t at = *cursor <= document->count ? *cursor : 0;
    /* `cursor` names either the active cue or the next cue during a gap. Only
       restart the bounded search after a genuine backward seek; treating a
       normal inter-cue gap as a seek made every gap rescan the document. */
    if (at != 0 && time_ms < document->cues[at - 1u].end_ms) at = 0;
    while (at < document->count && time_ms >= document->cues[at].end_ms)
        at++;
    *cursor = at;
    if (at >= document->count) return NULL;
    if (time_ms < document->cues[at].start_ms) {
        /* YouTube's generated timed text commonly leaves a short gap between
           adjacent cues. Dropping the opaque caption ground partway through
           such a gap makes it blink independently of the moving picture.
           Bridge the complete gap when it is tightly bounded; leave a longer
           authored pause blank from its beginning. The all-or-nothing rule
           avoids a ground transition in the middle of either kind of gap. */
        if (at == 0u)
            return NULL;
        const YoutubeSubtitleCue *previous = &document->cues[at - 1u];
        const YoutubeSubtitleCue *next = &document->cues[at];
        if (time_ms < previous->end_ms
            || next->start_ms < previous->end_ms
            || next->start_ms - previous->end_ms
                   > YOUTUBE_SUBTITLE_TRANSITION_BRIDGE_MS)
            return NULL;
        unsigned previous_block = subtitle_cue_block(previous);
        if (previous_block >= document->text_block_count) return NULL;
        return document->text_blocks[previous_block]
            + subtitle_cue_offset(previous);
    }
    const YoutubeSubtitleCue *cue = &document->cues[at];
    unsigned block = subtitle_cue_block(cue);
    if (block >= document->text_block_count) return NULL;
    return document->text_blocks[block] + subtitle_cue_offset(cue);
}

size_t youtube_subtitles_cue_count(
    const YoutubeSubtitleDocument *document)
{
    return document == NULL ? 0u : document->count;
}
