#ifndef TILEFINCH_MEDIA_MP4_POLICY_H
#define TILEFINCH_MEDIA_MP4_POLICY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The lazy sidx implementation advances one shared fragment-window cursor.
 * That preserves cross-sample ordering only when the file has one track.
 * Multi-track fragmented files use the bounded eager planner instead.
 */
static inline bool media_mp4_lazy_sidx_admitted(size_t track_count)
{
    return track_count == 1u;
}

#endif
