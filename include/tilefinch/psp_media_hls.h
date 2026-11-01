#ifndef TILEFINCH_PSP_MEDIA_HLS_H
#define TILEFINCH_PSP_MEDIA_HLS_H

#include "tilefinch/content_security_policy.h"
#include "tilefinch/media_hls.h"
#include "tilefinch/session.h"

typedef struct PspMediaHlsContext PspMediaHlsContext;

typedef enum {
    PSP_MEDIA_HLS_OPEN_PENDING = 0,
    PSP_MEDIA_HLS_OPEN_READY,
    PSP_MEDIA_HLS_OPEN_FAILED
} PspMediaHlsOpenStatus;

/* `page_redirects` marks a page's own HLS: its requests follow redirects to
   other origins one browser-thread-admitted hop at a time, each checked
   against `content_security_policy` (which must outlive the context) and
   the session's content blocker, captive portal and mixed-content rules.
   Provider streams pass false and NULL and keep the worker's fixed-origin
   redirect rule. */
/* How far master selection may trust the firmware decoder. Each level only
   narrows the one before it. */
typedef enum {
    /* Baseline/Main and undeclared variants up to the firmware geometry. */
    PSP_MEDIA_HLS_BOUND_FIRMWARE = 0,
    /* An undeclared variant proved High: undeclared ones at the software
       bound. */
    PSP_MEDIA_HLS_BOUND_UNDECLARED_SOFTWARE,
    /* The firmware decoder refused a variant's coding: all at the software
       bound. */
    PSP_MEDIA_HLS_BOUND_SOFTWARE
} PspMediaHlsSelectionBound;

PspMediaHlsContext *psp_media_hls_create(
    Budget *budget, BrowserSession *session,
    const char *playlist_url, const char *document_url,
    TilefinchRequestMode mode, TilefinchCredentialsMode credentials,
    PspMediaHlsSelectionBound selection_bound,
    const TilefinchContentSecurityPolicy *content_security_policy,
    bool page_redirects, char *error, size_t error_size);
/* Optional browser-owned recovery for source-buffer allocation refusal.
   Called only after partial allocations have been released. The callback
   must preserve this context, its session and the page's CSP; it may reclaim
   optional data or stop page scripts while retaining the page. At most two
   stages are attempted across the context, including separate audio. */
void psp_media_hls_set_memory_recovery(
    PspMediaHlsContext *context, void *opaque,
    bool (*make_room)(void *, size_t, unsigned *));
PspMediaHlsOpenStatus psp_media_hls_pump(
    PspMediaHlsContext *context, char *error, size_t error_size);
void psp_media_hls_pump_delivery(
    PspMediaHlsContext *context, uint64_t now_us);
bool psp_media_hls_failed(const PspMediaHlsContext *context);
bool psp_media_hls_sample_sources(
    PspMediaHlsContext *context, MediaSampleSource *video_source,
    MediaSampleSource *audio_source, bool *has_separate_audio);
bool psp_media_hls_stream_info(
    PspMediaHlsContext *context, MediaMp4TrackInfo *video,
    MediaMp4TrackInfo *audio);
bool psp_media_hls_stats(
    PspMediaHlsContext *context, MediaHlsStats *stats);
/* True when the selected variant was admitted beyond the software decoder's
   bound without a declared Baseline/Main profile. */
bool psp_media_hls_selection_unverified(const PspMediaHlsContext *context);
void psp_media_hls_destroy(PspMediaHlsContext *context);

#endif
