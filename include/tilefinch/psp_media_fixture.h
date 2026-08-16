#ifndef TILEFINCH_PSP_MEDIA_FIXTURE_H
#define TILEFINCH_PSP_MEDIA_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/budget.h"

typedef struct {
    unsigned clips_completed;
    unsigned frames_decoded;
    unsigned frames_observed;
    unsigned distinct_frame_signatures;
    unsigned seek_completions;
    unsigned replay_completions;
    int last_native_error;
} PspMediaFixtureReport;

/* Validation-build, real-firmware contract probe. It is intentionally
   independent of YouTube, curl, and the Memory Stick. PPSSPP may return the
   documented raw-NAL-unavailable result; that is UNTESTED, never PASS. */
bool psp_media_fixture_run(
    Budget *budget, PspMediaFixtureReport *report,
    char *error, size_t error_size);

typedef struct {
    unsigned candidates_tried;
    /* Candidates whose surface, read the way the graphics engine's texture
       unit reads it, agrees with the software scaler's answer for the
       baseline picture. Nonzero means Smooth can ship. */
    unsigned candidates_usable;
    unsigned candidates_refused;
    int last_native_error;
    bool decoded_picture;
    bool quarantined;
} PspMediaCscOrderReport;

/*
 * Decode one picture out of the embedded 240p fixture and re-run the firmware
 * colour conversion over it with a short list of candidate mode words, logging
 * what each candidate wrote. It uses the fixture rather than a stream because
 * this needs exactly one picture and no network: the fixture reaches a decoded
 * frame in about a second with no resolver, transport, or Memory Stick in the
 * way.
 */
bool psp_media_fixture_csc_order_probe(
    Budget *budget, PspMediaCscOrderReport *report,
    char *error, size_t error_size);

#endif
