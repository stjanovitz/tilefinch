#ifndef TILEFINCH_PSP_MEDIA_RANGE_PROBE_H
#define TILEFINCH_PSP_MEDIA_RANGE_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/session.h"

typedef struct {
    bool opened;
    size_t requests;
    size_t window_installs;
    size_t bytes_received;
    size_t failures;
    /* Reads answered WOULD_BLOCK: the source had been told to fetch those
       bytes and had not landed them yet. Not a failure, and the number a
       wedge investigation wants beside the installs. */
    size_t source_blocks;
    unsigned samples;
    /* Discontinuities in this track's sample offsets. Samples inside one
       fragment are contiguous in the mdat, so a gap is the moof of the next
       one -- which means the demuxer loaded a new sidx window between them. */
    unsigned fragment_boundaries;
} PspMediaRangeSourceReport;

typedef struct {
    PspMediaRangeSourceReport video;
    PspMediaRangeSourceReport audio;
    bool resolved;
    bool split;
    int itag;
    int audio_itag;
} PspMediaRangeProbeReport;

/*
 * The bounded HTTP range sources and the fragmented-MP4 reader, with no
 * decoder behind them.
 *
 * Everything this exercises -- the resolve, both range windows, the moov and
 * sidx reads, and sequential sample reads far enough to cross fragment
 * boundaries -- runs on a device today only as the first second of a playback
 * session, where a firmware decoder, the Media Engine pool and mpeg_vsh are
 * all in the way of reading what the transport did. This is that half on its
 * own: it needs the network and nothing else, so a transport regression can be
 * seen without a decoder that only real hardware has.
 *
 * Network-dependent by construction, and therefore manual or nightly rather
 * than a CTest gate.
 */
bool psp_media_range_probe_run(
    Budget *budget, BrowserSession *session, const char *watch_url,
    PspMediaRangeProbeReport *report, char *error, size_t error_size);

#endif
