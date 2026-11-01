#ifndef TILEFINCH_PSP_MEDIA_HLS_POLICY_H
#define TILEFINCH_PSP_MEDIA_HLS_POLICY_H

#include <stdbool.h>

/* Playlist text is cheap to scan but expensive to repeatedly pause inside
   libcurl.  This leaves one ordinary 16 KiB body callback of spare capacity
   in the 64 KiB background-stream buffer. */
#define PSP_HLS_PLAYLIST_CHUNK_BYTES (48u * 1024u)
#define PSP_HLS_PLAYLIST_OPEN_ATTEMPTS 2u
/* HLS hops run on the background transport worker, so a long connect never
   hides a Circle press from the main thread. The budget covers name
   resolution too: PSP Wi-Fi lookups of 2.4 s were seen on a device, which
   left a 3 s budget too little for TCP and TLS. */
#define PSP_HLS_CONNECT_TIMEOUT_MS 8000L

static inline bool psp_hls_playlist_retry_allowed(
    unsigned attempts, bool transport_retryable)
{
    return transport_retryable
        && attempts < PSP_HLS_PLAYLIST_OPEN_ATTEMPTS;
}

#endif
