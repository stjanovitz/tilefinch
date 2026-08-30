#ifndef TILEFINCH_PSP_WEBGL_GE_PROBE_H
#define TILEFINCH_PSP_WEBGL_GE_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSP_WEBGL_GE_PROBE_SCENE_COUNT 5u

typedef struct {
    const char *name;
    unsigned frames;
    unsigned draw_calls_per_frame;
    unsigned vertices_per_frame;
    size_t upload_bytes_per_frame;
    uint64_t prepare_us;
    uint64_t emit_us;
    uint64_t wait_us;
    uint64_t total_us;
    uint64_t maximum_frame_us;
    size_t list_bytes;
    size_t maximum_list_bytes;
    uint32_t checksum;
    bool passed;
} PspWebglGeProbeScene;

typedef struct {
    unsigned frames;
    uint64_t cpu_convert_us;
    uint64_t cpu_convert_max_us;
    uint64_t cpu_copy_us;
    uint64_t cpu_copy_max_us;
    uint64_t ge_submit_us;
    uint64_t ge_wait_us;
    uint64_t ge_total_us;
    uint64_t ge_total_max_us;
    size_t compared_pixels;
    size_t mismatched_pixels;
    uint32_t cpu_checksum;
    uint32_t ge_checksum;
    bool available;
    bool pixel_exact;
} PspWebglGeConversionProbe;

typedef struct {
    PspWebglGeProbeScene scenes[PSP_WEBGL_GE_PROBE_SCENE_COUNT];
    PspWebglGeConversionProbe conversion;
    size_t color_bytes;
    size_t depth_bytes;
    size_t texture_cache_bytes;
    unsigned context_initializations;
    unsigned synchronizations;
    bool passed;
    char detail[160];
} PspWebglGeProbeReport;

/* Validation-only research probe. `page_destination` is the current RGB565
   page back buffer and `cpu_destination` is the engine's ordinary main-memory
   480x272 frame. The probe renders into the unused page-mode EDRAM tail,
   compares the shipping CPU 320x180 -> 480x270 conversion/copy path with a
   direct GE conversion, composites its last frame, and returns with the GE
   idle. */
bool psp_webgl_ge_probe_run(
    uint16_t *page_destination,
    uint16_t *cpu_destination, size_t cpu_destination_pixels,
    PspWebglGeProbeReport *report);

#endif
