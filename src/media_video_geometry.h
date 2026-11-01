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

/* The software (High profile) decoder's picture bound. */
static inline bool psp_swdec_dimensions_supported(
    unsigned width, unsigned height)
{
    return width != 0 && height != 0 && width <= 432u && height <= 240u;
}

/* The firmware AVC decoder's level_idc range: anything up to Level 3.0.
   Low levels (1, 1b, 1.1-1.3, 2.x) are subsets of 3.0 and decode on the
   same programs; geometry, not level, picks the program. */
#define PSP_VIDEO_AVC_MAXIMUM_LEVEL 30u
static inline bool psp_video_avc_level_supported(unsigned level_idc)
{
    return level_idc != 0 && level_idc <= PSP_VIDEO_AVC_MAXIMUM_LEVEL;
}

#endif
