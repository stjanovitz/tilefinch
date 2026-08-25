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
    PspWebglGeProbeScene scenes[PSP_WEBGL_GE_PROBE_SCENE_COUNT];
    size_t color_bytes;
    size_t depth_bytes;
    size_t texture_cache_bytes;
    unsigned context_initializations;
    unsigned synchronizations;
    bool passed;
    char detail[160];
} PspWebglGeProbeReport;

/* Validation-only research probe. `page_destination` is the current RGB565
   page back buffer. The probe renders into the unused page-mode EDRAM tail,
   composites its last frame into that buffer, and returns with the GE idle. */
bool psp_webgl_ge_probe_run(
    uint16_t *page_destination, PspWebglGeProbeReport *report);

#endif
