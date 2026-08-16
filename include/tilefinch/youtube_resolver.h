#ifndef TILEFINCH_YOUTUBE_RESOLVER_H
#define TILEFINCH_YOUTUBE_RESOLVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/session.h"

#define YOUTUBE_VIDEO_ID_CAPACITY 32
#define YOUTUBE_TITLE_CAPACITY 256
#define YOUTUBE_MEDIA_URL_CAPACITY 4096
#define YOUTUBE_MIME_CAPACITY 160
#define YOUTUBE_CLIENT_NAME_CAPACITY 32

typedef enum {
    YOUTUBE_PLAYABILITY_UNKNOWN = 0,
    YOUTUBE_PLAYABILITY_OK,
    YOUTUBE_PLAYABILITY_LOGIN_REQUIRED,
    YOUTUBE_PLAYABILITY_AGE_RESTRICTED,
    YOUTUBE_PLAYABILITY_REGION_BLOCKED,
    YOUTUBE_PLAYABILITY_LIVE_UNSUPPORTED,
    YOUTUBE_PLAYABILITY_UPCOMING_UNSUPPORTED,
    YOUTUBE_PLAYABILITY_UNAVAILABLE,
    YOUTUBE_PLAYABILITY_CLIENT_REJECTED
} YoutubePlayability;

typedef struct {
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY];
    char title[YOUTUBE_TITLE_CAPACITY];
    char media_url[YOUTUBE_MEDIA_URL_CAPACITY];
    char audio_url[YOUTUBE_MEDIA_URL_CAPACITY];
    char mime_type[YOUTUBE_MIME_CAPACITY];
    char audio_mime_type[YOUTUBE_MIME_CAPACITY];
    int itag;
    int audio_itag;
    int width;
    int height;
    uint64_t duration_ms;
    uint64_t content_length;
    uint64_t audio_content_length;
    uint64_t bitrate;
    uint64_t audio_bitrate;
    bool split_streams;
    long watch_status;
    long player_status;
    size_t watch_bytes;
    size_t player_bytes;
    uint64_t expires_unix;
    unsigned client_attempts;
    char client_name[YOUTUBE_CLIENT_NAME_CAPACITY];
} YoutubeStream;

const char *youtube_playability_name(YoutubePlayability playability);
/*
 * A profile can report ordinary UNAVAILABLE for a client-specific reason
 * (embedding disabled, made-for-kids policy, or retired identity). Only
 * restrictions whose meaning is independent of the selected client stop the
 * bounded fallback ladder immediately. Age responses may also be
 * client-specific, so the resolver retains their actionable message while
 * trying the remaining bounded profiles.
 */
bool youtube_playability_is_globally_terminal(
    YoutubePlayability playability);

/*
 * Current unattested mobile identities can expose direct URLs whose first
 * bounded prefix is readable while later ranges are refused. This policy is
 * kept beside the client table so a resolver never hands a known-incomplete
 * source to the MP4 demuxer.
 */
bool youtube_direct_delivery_admitted(
    const char *client_name, const YoutubeStream *stream);

/* True only for a bounded video-id route this resolver can consume. */
bool youtube_watch_url_supported(const char *url);
bool youtube_watch_url_video_id(
    const char *url, char output[YOUTUBE_VIDEO_ID_CAPACITY]);
/* Revalidate both the resolver result and FFmpeg's effective redirect URL.
   Only HTTPS/443 googlevideo.com hosts (or their subdomains) are admitted. */
bool youtube_media_url_supported(const char *url);

/*
 * Resolves an ordinary YouTube watch URL through the same bounded fetch and
 * cookie policy used by the browser. A complete AVC/AAC MP4 remains a
 * fallback; an equal-or-higher-resolution AVC video plus AAC audio pair is
 * selected when both bounded adaptive sources are available, identified by
 * split_streams. The demuxer consumes indexed fragments with a fixed window.
 *
 * This never invokes an external downloader, evaluates a player decipher
 * program, or fabricates challenge state. Ciphered formats fail explicitly.
 */
bool youtube_resolve_progressive_mp4(
    Budget *budget, BrowserSession *session, const char *watch_url,
    int maximum_height, long timeout_ms, YoutubeStream *stream,
    char *error, size_t error_size);

typedef bool (*YoutubeResolverCancelCallback)(void *opaque);

typedef struct YoutubeResolveJob YoutubeResolveJob;

typedef enum {
    YOUTUBE_RESOLVE_JOB_PENDING = 0,
    YOUTUBE_RESOLVE_JOB_COMPLETE,
    YOUTUBE_RESOLVE_JOB_FAILED
} YoutubeResolveJobStatus;

/*
 * PSP-native pumpable resolver. Network production occurs on the bounded
 * transport worker; each pump consumes at most one response chunk or one
 * parser transition on the browser thread. Host/replay callers retain the
 * synchronous deterministic API below.
 */
YoutubeResolveJob *youtube_resolve_job_begin(
    Budget *budget, BrowserSession *session, const char *watch_url,
    int maximum_height, long timeout_ms,
    YoutubeResolverCancelCallback cancel, void *cancel_opaque);
YoutubeResolveJobStatus youtube_resolve_job_pump(YoutubeResolveJob *job);
bool youtube_resolve_job_take(
    YoutubeResolveJob *job, YoutubeStream *stream);
const char *youtube_resolve_job_error(const YoutubeResolveJob *job);
void youtube_resolve_job_cancel(YoutubeResolveJob *job, const char *reason);
void youtube_resolve_job_destroy(YoutubeResolveJob *job);

/*
 * Cancellation-aware form used by interactive frontends. The callback is
 * polled by each transport and between bounded resolver attempts. Returning
 * true requests cancellation; partially built results remain caller-owned
 * only inside the resolver and are released before this function returns.
 */
bool youtube_resolve_progressive_mp4_cancelable(
    Budget *budget, BrowserSession *session, const char *watch_url,
    int maximum_height, long timeout_ms,
    YoutubeResolverCancelCallback cancel, void *cancel_opaque,
    YoutubeStream *stream, char *error, size_t error_size);

/*
 * Bounded pure parser used by deterministic tests and retained captures.
 * The JSON must be a player API response, not a watch-page document.
 */
bool youtube_parse_player_response(
    const char *json, size_t length, const char *video_id,
    int maximum_height, YoutubeStream *stream,
    char *error, size_t error_size);

/*
 * Parser form used by the resolver and diagnostics. A syntactically valid
 * non-playable response returns false while still classifying the reason.
 */
bool youtube_parse_player_response_diagnostic(
    const char *json, size_t length, const char *video_id,
    int maximum_height, YoutubePlayability *playability,
    YoutubeStream *stream, char *error, size_t error_size);

#endif
