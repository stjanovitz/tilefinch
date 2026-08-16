#ifndef TILEFINCH_MEDIA_FILE_H
#define TILEFINCH_MEDIA_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_mp4.h"

typedef struct MediaFileRange MediaFileRange;

MediaFileRange *media_file_range_open(
    Budget *budget, const char *path, uint64_t expected_length,
    char *error, size_t error_size);
MediaRangeReader media_file_range_reader(MediaFileRange *range);
void media_file_range_close(MediaFileRange *range);

#endif
