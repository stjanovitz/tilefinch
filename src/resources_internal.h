#ifndef TILEFINCH_RESOURCES_INTERNAL_H
#define TILEFINCH_RESOURCES_INTERNAL_H

/* Shared by the stylesheet loader (resources.c) and the web-font loader
   (resources_fonts.c). */

#include "tilefinch/platform.h"

static inline double resource_now_ms(void)
{
    return (double) tilefinch_platform_monotonic_time_us() * 0.001;
}

#endif
