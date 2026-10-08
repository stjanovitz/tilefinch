#ifndef TILEFINCH_MEDIA_VIDEO_GEOMETRY_H
#define TILEFINCH_MEDIA_VIDEO_GEOMETRY_H

#include <stdbool.h>

/* Tall 240p-class pictures fit the existing large decoded-surface pool. */
static inline bool psp_video_dimensions_supported(unsigned width, unsigned height)
{
    return width != 0 && height != 0
        && ((width <= 640u && height <= 360u)
            || (width <= 272u && height <= 480u));
}

#endif
