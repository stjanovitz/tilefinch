/* libFuzzer target: smaller network-fed parsers.
   Byte 0 selects the parser (mod 5); the rest is the response body:
     0  web app manifest JSON
     1  WebVTT captions, one-shot
     2  WebVTT captions, incremental builder with input-derived chunks
     3  multiplayer wire packet decode + re-encode round trip
     4  YouTube player-response JSON (stream + caption catalog) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/multiplayer_protocol.h"
#include "tilefinch/web_app_manifest.h"
#include "tilefinch/youtube_resolver.h"
#include "tilefinch/youtube_subtitles.h"

#define MIB (1024u * 1024u)

static void exercise_captions(YoutubeSubtitleDocument *document)
{
    if (document == NULL) return;
    size_t cursor = 0;
    (void) youtube_subtitles_cue_count(document);
    for (uint64_t t = 0; t < UINT64_C(120000000); t += UINT64_C(700000)) {
        const char *text = youtube_subtitles_text_at(document, t, &cursor);
        if (text != NULL) (void) strlen(text);
    }
    cursor = 0;
    (void) youtube_subtitles_text_at(document, UINT64_MAX, &cursor);
    youtube_subtitles_destroy(document);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 256 * 1024) return 0;
    unsigned mode = data[0] % 5u;
    size_t length = size - 1;
    char *body = malloc(length == 0 ? 1 : length);
    if (body == NULL) return 0;
    if (length != 0) memcpy(body, data + 1, length);

    Budget budget;
    budget_init(&budget, 24u * MIB);
    char error[256];
    if (mode == 0) {
        TilefinchWebAppManifest manifest;
        (void) tilefinch_web_app_manifest_parse(
            body, length, "https://app.test/m/manifest.json",
            "https://app.test/index.html", &manifest, error, sizeof(error));
    } else if (mode == 1) {
        exercise_captions(youtube_subtitles_parse_vtt(
            &budget, (const unsigned char *) body, length));
    } else if (mode == 2) {
        YoutubeSubtitleBuilder *builder = youtube_subtitles_builder_create(
            &budget, length != 0 ? (uint64_t) (unsigned char) body[0] * 100000u
                                 : 0);
        if (builder != NULL) {
            bool ok = true;
            size_t chunk = 1 + (length != 0 ? (unsigned char) body[0] % 61u : 0);
            for (size_t at = 0; ok && at < length; at += chunk) {
                size_t n = length - at < chunk ? length - at : chunk;
                ok = youtube_subtitles_builder_feed(
                    builder, (const unsigned char *) body + at, n);
            }
            /* finish consumes the builder; destroy only on the error path. */
            if (ok) exercise_captions(youtube_subtitles_builder_finish(builder));
            else youtube_subtitles_builder_destroy(builder);
        }
    } else if (mode == 3) {
        TilefinchMultiplayerWirePacket packet;
        if (tilefinch_multiplayer_packet_decode(
                (const unsigned char *) body, length, &packet)) {
            if (packet.payload_length > length) abort();
            unsigned char *output = malloc(length + 64);
            if (output != NULL) {
                (void) tilefinch_multiplayer_packet_encode(
                    packet.kind, packet.game_hash, packet.sender,
                    packet.receiver, packet.cookie, packet.sequence,
                    packet.binary, packet.payload, packet.payload_length,
                    output, length + 64);
                free(output);
            }
        }
    } else {
        static YoutubeStream stream;
        static YoutubeCaptionCatalog catalog;
        YoutubeTrackPreferences preferences;
        memset(&preferences, 0, sizeof(preferences));
        memcpy(preferences.caption_language, "en", 3);
        (void) youtube_parse_player_response_with_preferences_and_caption_catalog(
            body, length, "dQw4w9WgXcQ", 272, &preferences, &stream, &catalog,
            error, sizeof(error));
        YoutubePlayability playability;
        (void) youtube_parse_player_response_diagnostic(
            body, length, "dQw4w9WgXcQ", 360, &playability, &stream, error,
            sizeof(error));
    }
    if (budget.current != 0) {
        fprintf(stderr, "fuzz_misc: budget leak of %zu bytes (mode %u)\n",
                budget.current, mode);
        abort();
    }
    free(body);
    return 0;
}
