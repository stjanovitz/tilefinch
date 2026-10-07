/* Replay a recorded game-audio command log through the real native mixer.

   tests/treadline_music_harness.js records every command the reference
   bootstrap Web Audio subset (game-audio.js) sends to
   __tilefinchGameAudioCommand, with the JavaScript clock time and the result
   its admission model expected. This tool mixes 512-frame blocks until the
   output frame reaches each command's time (so, as on the PSP, the mixer's
   "now" is block-quantized and never behind the browser clock), applies the
   command to src/game_audio.c, and checks the real result against the
   recorded one.

   Usage: tilefinch-game-audio-render LOG [--wav OUT.wav] [--repeat N]
   Prints one JSON line: command and mismatch counts, block count, output
   peak/RMS/clipping, and host CPU per mixed block (median of repeats). Host
   timings are host numbers only; they are not PSP costs. */
#include "tilefinch/budget.h"
#include "tilefinch/game_audio.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE 44100.0
#define BLOCK TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES
#define MAX_HANDLES 4096u

typedef struct {
    char kind;
    double t;
    int command;
    int argc;
    double args[16];
    float values[TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT];
    size_t count;
    unsigned char *bytes;
    size_t length;
    uint32_t result;
} Entry;

typedef struct {
    Entry *entries;
    size_t count, capacity;
} Log;

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static char *next_token(char **cursor)
{
    char *at = *cursor;
    while (*at == ' ') at++;
    if (*at == '\0' || *at == '\n') return NULL;
    char *start = at;
    while (*at != ' ' && *at != '\0' && *at != '\n') at++;
    if (*at != '\0') *at++ = '\0';
    *cursor = at;
    return start;
}

static bool parse_log(const char *path, Log *log)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    size_t size = 1u << 16;
    char *line = malloc(size);
    if (line == NULL) { fclose(file); return false; }
    bool ok = true;
    for (;;) {
        size_t used = 0;
        int c;
        while ((c = fgetc(file)) != EOF && c != '\n') {
            if (used + 2 >= size) {
                char *grown = realloc(line, size * 2u);
                if (grown == NULL) { ok = false; break; }
                line = grown; size *= 2u;
            }
            line[used++] = (char) c;
        }
        if (!ok || (c == EOF && used == 0)) break;
        line[used] = '\0';
        if (line[0] != 'D' && line[0] != 'C' && line[0] != 'E') continue;
        if (log->count == log->capacity) {
            size_t capacity = log->capacity ? log->capacity * 2u : 1024u;
            Entry *grown = realloc(log->entries, capacity * sizeof(*grown));
            if (grown == NULL) { ok = false; break; }
            log->entries = grown; log->capacity = capacity;
        }
        Entry *entry = &log->entries[log->count];
        memset(entry, 0, sizeof(*entry));
        char *cursor = line + 1, *token = next_token(&cursor);
        entry->kind = line[0];
        entry->t = token ? strtod(token, NULL) : 0;
        if (entry->kind == 'D') {
            char *hex = next_token(&cursor);
            next_token(&cursor);
            char *result = next_token(&cursor);
            size_t digits = hex ? strlen(hex) : 0;
            entry->length = digits / 2u;
            entry->bytes = malloc(entry->length ? entry->length : 1u);
            if (entry->bytes == NULL || result == NULL) { ok = false; break; }
            for (size_t at = 0; at < entry->length; at++)
                entry->bytes[at] = (unsigned char) (hex_value(hex[at * 2u]) * 16
                    + hex_value(hex[at * 2u + 1u]));
            entry->result = (uint32_t) strtoul(result, NULL, 10);
        } else if (entry->kind == 'C') {
            char *command = next_token(&cursor), *argc = next_token(&cursor);
            if (command == NULL || argc == NULL) { ok = false; break; }
            entry->command = atoi(command);
            entry->argc = atoi(argc);
            int stored = 0;
            for (int arg = 0; arg < entry->argc && stored < 16; arg++) {
                char *value = next_token(&cursor);
                if (value == NULL) { ok = false; break; }
                if (entry->command == TILEFINCH_GAME_AUDIO_COMMAND_CURVE && arg == 2) {
                    entry->count = (size_t) strtoul(value, NULL, 10);
                    if (entry->count > TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT) {
                        ok = false; break;
                    }
                    for (size_t at = 0; at < entry->count; at++) {
                        char *sample = next_token(&cursor);
                        if (sample == NULL) { ok = false; break; }
                        entry->values[at] = strtof(sample, NULL);
                    }
                    entry->args[stored++] = 0;
                    continue;
                }
                entry->args[stored++] = strtod(value, NULL);
            }
            if (!ok) break;
            next_token(&cursor);
            char *result = next_token(&cursor);
            if (result == NULL) { ok = false; break; }
            entry->result = (uint32_t) strtoul(result, NULL, 10);
        }
        log->count++;
        if (c == EOF) break;
    }
    free(line);
    fclose(file);
    return ok;
}

typedef struct {
    uint32_t recorded[MAX_HANDLES], real[MAX_HANDLES];
    size_t count;
} HandleMap;

static void map_put(HandleMap *map, uint32_t recorded, uint32_t real)
{
    for (size_t at = 0; at < map->count; at++)
        if (map->recorded[at] == recorded) { map->real[at] = real; return; }
    if (map->count < MAX_HANDLES) {
        map->recorded[map->count] = recorded;
        map->real[map->count++] = real;
    }
}

static uint32_t map_get(const HandleMap *map, uint32_t recorded)
{
    for (size_t at = map->count; at-- > 0;)
        if (map->recorded[at] == recorded) return map->real[at];
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    return (uint64_t) spec.tv_sec * 1000000000u + (uint64_t) spec.tv_nsec;
}

typedef struct {
    int16_t *pcm;
    size_t frames, capacity;
    uint64_t *block_ns;
    size_t blocks, block_capacity;
    size_t commands, mismatches;
    char first_mismatch[160];
} Render;

static bool mix_block(TilefinchGameAudio *audio, Render *render)
{
    if (render->frames + BLOCK > render->capacity) {
        size_t capacity = render->capacity ? render->capacity * 2u : RATE * 8u;
        int16_t *grown = realloc(render->pcm, capacity * 2u * sizeof(int16_t));
        if (grown == NULL) return false;
        render->pcm = grown; render->capacity = capacity;
    }
    if (render->blocks == render->block_capacity) {
        size_t capacity = render->block_capacity ? render->block_capacity * 2u : 1024u;
        uint64_t *grown = realloc(render->block_ns, capacity * sizeof(*grown));
        if (grown == NULL) return false;
        render->block_ns = grown; render->block_capacity = capacity;
    }
    uint64_t before = now_ns();
    bool ok = tilefinch_game_audio_mix(audio, render->pcm + render->frames * 2u, BLOCK);
    render->block_ns[render->blocks++] = now_ns() - before;
    render->frames += BLOCK;
    return ok;
}

static void mismatch(Render *render, const Entry *entry, uint32_t real)
{
    if (render->mismatches++ == 0)
        snprintf(render->first_mismatch, sizeof(render->first_mismatch),
                 "t=%.6f command=%d recorded=%" PRIu32 " real=%" PRIu32,
                 entry->t, entry->command, entry->result, real);
}

static bool render_log(const Log *log, Render *render)
{
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    TilefinchGameAudio *audio = tilefinch_game_audio_create(&budget);
    if (audio == NULL) return false;
    static HandleMap voices, buffers;
    memset(&voices, 0, sizeof(voices));
    memset(&buffers, 0, sizeof(buffers));
    bool ok = true;
    for (size_t at = 0; ok && at < log->count; at++) {
        const Entry *entry = &log->entries[at];
        size_t target = (size_t) llround(entry->t * RATE);
        while (ok && render->frames < target) ok = mix_block(audio, render);
        if (!ok || entry->kind == 'E') continue;
        if (entry->kind == 'D') {
            TilefinchGameAudioBufferInfo info;
            bool decoded = tilefinch_game_audio_decode_wav(
                audio, entry->bytes, entry->length, &info);
            if (!decoded) mismatch(render, entry, 0);
            else map_put(&buffers, entry->result, info.handle);
            continue;
        }
        const double *a = entry->args;
        uint32_t real = 0, voice = 0;
        render->commands++;
        switch (entry->command) {
        case TILEFINCH_GAME_AUDIO_COMMAND_RESUME: real = tilefinch_game_audio_resume(audio); break;
        case TILEFINCH_GAME_AUDIO_COMMAND_SUSPEND: tilefinch_game_audio_suspend(audio); real = 1; break;
        case TILEFINCH_GAME_AUDIO_COMMAND_CLOSE: real = 1; break;
        case TILEFINCH_GAME_AUDIO_COMMAND_START_BUFFER:
            real = tilefinch_game_audio_start(audio, map_get(&buffers, (uint32_t) a[0]),
                a[1], a[2], a[3], a[4], a[5], a[6] != 0, a[7], a[8], a[9], &voice)
                ? voice : 0;
            if (real) map_put(&voices, entry->result, real);
            break;
        case TILEFINCH_GAME_AUDIO_COMMAND_STOP:
            tilefinch_game_audio_stop(audio, map_get(&voices, (uint32_t) a[0]), a[1]);
            real = 1;
            break;
        case TILEFINCH_GAME_AUDIO_COMMAND_START_OSCILLATOR:
            real = tilefinch_game_audio_start_oscillator(audio,
                (TilefinchGameAudioOscillatorType) a[0], a[1], a[2], a[3], a[4], &voice)
                ? voice : 0;
            if (real) map_put(&voices, entry->result, real);
            break;
        case TILEFINCH_GAME_AUDIO_COMMAND_SET_GAIN:
            real = tilefinch_game_audio_update_voice(audio,
                map_get(&voices, (uint32_t) a[0]), a[1], a[2]);
            break;
        case TILEFINCH_GAME_AUDIO_COMMAND_SET_FREQUENCY:
            real = tilefinch_game_audio_update_oscillator(audio,
                map_get(&voices, (uint32_t) a[0]), a[1]);
            break;
        case TILEFINCH_GAME_AUDIO_COMMAND_GAIN_TARGET:
            real = tilefinch_game_audio_schedule_envelope_target(audio,
                map_get(&voices, (uint32_t) a[0]), a[1], a[2], a[3], a[4]);
            break;
        case TILEFINCH_GAME_AUDIO_COMMAND_CANCEL_GAIN:
            real = tilefinch_game_audio_cancel_envelope(audio,
                map_get(&voices, (uint32_t) a[0]));
            break;
        case TILEFINCH_GAME_AUDIO_COMMAND_CANCEL_PITCH:
            real = tilefinch_game_audio_cancel_pitch_curve(audio,
                map_get(&voices, (uint32_t) a[0]));
            break;
        case TILEFINCH_GAME_AUDIO_COMMAND_CURVE:
            real = tilefinch_game_audio_schedule_curve(audio,
                map_get(&voices, (uint32_t) a[0]), a[1] != 0, entry->values,
                entry->count, a[3], a[4], a[5], a[6]);
            break;
        default: real = 0; break;
        }
        /* Handles are compared by success; their bit patterns may differ. */
        bool recorded = entry->result != 0, actual = real != 0;
        if (recorded != actual) mismatch(render, entry, real);
    }
    tilefinch_game_audio_destroy(audio);
    return ok && budget.current == 0;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *) left, b = *(const uint64_t *) right;
    return a < b ? -1 : a > b;
}

static bool write_wav(const char *path, const int16_t *pcm, size_t frames)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    uint32_t data = (uint32_t) (frames * 4u);
    unsigned char header[44];
    memcpy(header, "RIFF", 4);
    uint32_t words[] = {36u + data};
    memcpy(header + 4, words, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    uint32_t fmt_size = 16, rate = 44100, bytes = 44100u * 4u;
    uint16_t format = 1, channels = 2, align = 4, bits = 16;
    memcpy(header + 16, &fmt_size, 4); memcpy(header + 20, &format, 2);
    memcpy(header + 22, &channels, 2); memcpy(header + 24, &rate, 4);
    memcpy(header + 28, &bytes, 4); memcpy(header + 32, &align, 2);
    memcpy(header + 34, &bits, 2); memcpy(header + 36, "data", 4);
    memcpy(header + 40, &data, 4);
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header)
        && fwrite(pcm, 4, frames, file) == frames;
    return fclose(file) == 0 && ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s LOG [--wav OUT] [--repeat N]\n", argv[0]);
        return 2;
    }
    const char *wav = NULL;
    int repeat = 1;
    for (int at = 2; at < argc; at++) {
        if (strcmp(argv[at], "--wav") == 0 && at + 1 < argc) wav = argv[++at];
        else if (strcmp(argv[at], "--repeat") == 0 && at + 1 < argc)
            repeat = atoi(argv[++at]);
    }
    if (repeat < 1) repeat = 1;
    Log log = {0};
    if (!parse_log(argv[1], &log)) {
        fprintf(stderr, "cannot parse %s\n", argv[1]);
        return 2;
    }
    double *means = calloc((size_t) repeat, sizeof(double));
    double *p95s = calloc((size_t) repeat, sizeof(double));
    Render keep = {0};
    bool ok = means != NULL && p95s != NULL;
    for (int pass = 0; ok && pass < repeat; pass++) {
        Render render = {0};
        ok = render_log(&log, &render);
        if (ok && render.blocks) {
            uint64_t total = 0;
            for (size_t at = 0; at < render.blocks; at++) total += render.block_ns[at];
            means[pass] = (double) total / (double) render.blocks / 1000.0;
            qsort(render.block_ns, render.blocks, sizeof(uint64_t), compare_u64);
            p95s[pass] = render.block_ns[render.blocks * 95u / 100u] / 1000.0;
        }
        if (pass == 0) keep = render;
        else { free(render.pcm); free(render.block_ns); }
    }
    if (!ok) {
        fprintf(stderr, "render failed\n");
        return 1;
    }
    /* Median of repeats, by insertion sort (few repeats). */
    for (int i = 1; i < repeat; i++)
        for (int j = i; j > 0 && means[j - 1] > means[j]; j--) {
            double swap = means[j]; means[j] = means[j - 1]; means[j - 1] = swap;
        }
    for (int i = 1; i < repeat; i++)
        for (int j = i; j > 0 && p95s[j - 1] > p95s[j]; j--) {
            double swap = p95s[j]; p95s[j] = p95s[j - 1]; p95s[j - 1] = swap;
        }
    int peak = 0;
    size_t clipped = 0;
    double square = 0;
    for (size_t at = 0; at < keep.frames * 2u; at++) {
        int value = keep.pcm[at];
        int magnitude = value < 0 ? -value : value;
        if (magnitude > peak) peak = magnitude;
        if (value >= 32767 || value <= -32768) clipped++;
        square += (double) value * value;
    }
    double rms = keep.frames ? sqrt(square / (double) (keep.frames * 2u)) : 0;
    if (wav != NULL && !write_wav(wav, keep.pcm, keep.frames)) {
        fprintf(stderr, "cannot write %s\n", wav);
        return 1;
    }
    printf("{\"commands\":%zu,\"mismatches\":%zu,\"firstMismatch\":\"%s\","
           "\"blocks\":%zu,\"seconds\":%.3f,\"peak\":%d,\"rms\":%.1f,"
           "\"clipped\":%zu,\"repeat\":%d,\"hostMixMeanUs\":%.3f,"
           "\"hostMixP95Us\":%.3f}\n",
           keep.commands, keep.mismatches, keep.first_mismatch, keep.blocks,
           keep.frames / RATE, peak, rms, clipped, repeat,
           means[repeat / 2], p95s[repeat / 2]);
    for (size_t at = 0; at < log.count; at++) free(log.entries[at].bytes);
    free(log.entries); free(keep.pcm); free(keep.block_ns);
    free(means); free(p95s);
    return keep.mismatches == 0 ? 0 : 1;
}
