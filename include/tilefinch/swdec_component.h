#ifndef TILEFINCH_SWDEC_COMPONENT_H
#define TILEFINCH_SWDEC_COMPONENT_H

#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_SWDEC_COMPONENT_MAGIC UINT32_C(0x54465344)
#define TILEFINCH_SWDEC_COMPONENT_ABI_VERSION 4u
#define TILEFINCH_SWDEC_COMPONENT_HELPER_PATH_LIMIT 192u
#define TILEFINCH_SWDEC_CSC_SLOT_COUNT 25u
#define TILEFINCH_SWDEC_PCM_SLOT_COUNT 128u

typedef struct {
    const uint8_t *plane[3];
    int stride[3];
    int width;
    int height;
    uint64_t pts;
} TilefinchSwdecPicture;

typedef struct {
    size_t arena_bytes;
    size_t peak_bytes;
    uint32_t frames_out;
    uint32_t aus_in;
    uint32_t errors;
} TilefinchSwdecStats;

typedef struct TilefinchSwdecComponentApi {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t struct_size;

    size_t (*arena_bytes)(int max_width, int max_height, int max_refs);
    void *(*open)(void *arena, size_t arena_bytes,
                  int max_width, int max_height, int max_refs);
    int (*decode)(void *decoder, const uint8_t *access_unit,
                  size_t access_unit_bytes, uint64_t pts,
                  TilefinchSwdecPicture *picture);
    void (*stats)(const void *decoder, TilefinchSwdecStats *stats);
    void (*set_speed)(void *decoder, int speed);
    void (*close)(void *decoder);

    void (*bind_aux_arena)(void *arena, size_t arena_bytes);
    int (*attach_me)(const char *helper_path,
                     void (*log)(const char *format, ...));
    int (*recover_me)(void);
    void (*detach_me)(void);
    int (*restore_me)(void);
    int (*me_failed)(void);

    int (*audio_setup)(void);
    int (*audio_submit)(const void *adts, unsigned bytes);
    int (*audio_poll)(unsigned index, unsigned *crc, unsigned *samples);
    unsigned (*audio_done)(void);
    const short *(*audio_pcm)(unsigned index);
    int (*audio_reset)(unsigned timeout_us);
    int (*audio_shutdown)(unsigned timeout_us);

    void (*csc_begin)(int slot, void *rgb565, int stride_pixels,
                      size_t capacity_bytes);
    int (*csc_close)(void);
    void (*csc_off)(void);
} TilefinchSwdecComponentApi;

typedef struct {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t struct_size;
    TilefinchSwdecComponentApi *api;
} TilefinchSwdecComponentStart;

#endif
