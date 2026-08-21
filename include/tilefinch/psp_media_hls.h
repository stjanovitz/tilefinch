#ifndef TILEFINCH_PSP_MEDIA_HLS_H
#define TILEFINCH_PSP_MEDIA_HLS_H

#include "tilefinch/media_hls.h"
#include "tilefinch/session.h"

typedef struct PspMediaHlsContext PspMediaHlsContext;

typedef enum {
    PSP_MEDIA_HLS_OPEN_PENDING = 0,
    PSP_MEDIA_HLS_OPEN_READY,
    PSP_MEDIA_HLS_OPEN_FAILED
} PspMediaHlsOpenStatus;

PspMediaHlsContext *psp_media_hls_create(
    Budget *budget, BrowserSession *session,
    const char *playlist_url, const char *document_url,
    TilefinchRequestMode mode, TilefinchCredentialsMode credentials,
    char *error, size_t error_size);
PspMediaHlsOpenStatus psp_media_hls_pump(
    PspMediaHlsContext *context, char *error, size_t error_size);
bool psp_media_hls_sample_source(
    PspMediaHlsContext *context, MediaSampleSource *source);
bool psp_media_hls_stream_info(
    PspMediaHlsContext *context, MediaMp4TrackInfo *video,
    MediaMp4TrackInfo *audio);
bool psp_media_hls_stats(
    PspMediaHlsContext *context, MediaHlsStats *stats);
void psp_media_hls_destroy(PspMediaHlsContext *context);

#endif
