#ifndef TILEFINCH_SWDEC_COMPONENT_H
#define TILEFINCH_SWDEC_COMPONENT_H

#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_SWDEC_COMPONENT_MAGIC UINT32_C(0x54465344)
#define TILEFINCH_SWDEC_COMPONENT_ABI_VERSION 5u
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
    int (*csc_picture)(int slot, void *rgb565, int stride_pixels,
                       size_t capacity_bytes,
                       const TilefinchSwdecPicture *picture);
    int (*csc_close)(void);
    void (*csc_off)(void);
} TilefinchSwdecComponentApi;

typedef struct {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t struct_size;
    TilefinchSwdecComponentApi *api;
} TilefinchSwdecComponentStart;

/* Validation browsers start the component with this longer block; its
   struct_size says the trailing field is there. A component reads the field
   only when struct_size covers it, and an older component ignores it, so
   either side may be older and the ABI stays the same. Only a validation
   component acts on the flags.

   window_mark (when struct_size covers it): the browser's steady-window
   counter, odd while the window is open. A validation component reports
   its stage figures for the window, between swdec-window start and end
   lines, when it sees the value change at a decode. */
typedef struct {
    TilefinchSwdecComponentStart start;
    uint32_t validation_flags;
    const volatile uint32_t *window_mark;
} TilefinchSwdecComponentValidationStart;

/* boot.cfg validation_swdec_reference (a bit mask) for same-binary device
   A/B of a validation add-on: 1 = the motion-compensation kernels before
   the shared-pass versions, 2 = every macroblock on the generic
   reconstruction path, 4 = fixed-size edge-emulation regions, 8 = jobs
   that carry the whole parsed caches, 16 = chroma MC one plane per call,
   32 = the (no-op) prefetch calls made, 64 = chroma prediction kept in
   reconstruction instead of the Media Engine's parser, 128 = the chroma
   qualification replay runs (swdec_chroma_replay.c). */
#define TILEFINCH_SWDEC_VALIDATION_REFERENCE UINT32_C(1)
#define TILEFINCH_SWDEC_VALIDATION_MB_GENERIC UINT32_C(2)
#define TILEFINCH_SWDEC_VALIDATION_EDGE_FULL UINT32_C(4)
#define TILEFINCH_SWDEC_VALIDATION_JOB_FULL UINT32_C(8)
#define TILEFINCH_SWDEC_VALIDATION_CHROMA_SPLIT UINT32_C(16)
#define TILEFINCH_SWDEC_VALIDATION_PREFETCH UINT32_C(32)
#define TILEFINCH_SWDEC_VALIDATION_CHROMA_CPU UINT32_C(64)
#define TILEFINCH_SWDEC_VALIDATION_CHROMA_REPLAY UINT32_C(128)

#endif
