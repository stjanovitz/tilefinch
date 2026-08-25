#ifndef TILEFINCH_PSP_MEDIA_HLS_POLICY_H
#define TILEFINCH_PSP_MEDIA_HLS_POLICY_H

#include <stdbool.h>

/* Playlist text is cheap to scan but expensive to repeatedly pause inside
   libcurl.  This leaves one ordinary 16 KiB body callback of spare capacity
   in the 64 KiB background-stream buffer. */
#define PSP_HLS_PLAYLIST_CHUNK_BYTES (48u * 1024u)
#define PSP_HLS_PLAYLIST_OPEN_ATTEMPTS 2u

static inline bool psp_hls_playlist_retry_allowed(
    unsigned attempts, bool transport_retryable)
{
    return transport_retryable
        && attempts < PSP_HLS_PLAYLIST_OPEN_ATTEMPTS;
}

#endif
