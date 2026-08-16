#include "tilefinch/media_file.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct MediaFileRange {
    Budget *budget;
    FILE *file;
    uint64_t length;
    char last_error[160];
};

static bool media_file_read(
    void *opaque, uint64_t offset, void *destination, size_t length)
{
    MediaFileRange *range = opaque;
    if (range == NULL || range->file == NULL || destination == NULL
        || offset > range->length || length > range->length - offset
        || offset > LONG_MAX) return false;
    if (fseek(range->file, (long) offset, SEEK_SET) != 0
        || fread(destination, 1, length, range->file) != length) {
        snprintf(
            range->last_error, sizeof(range->last_error),
            "offline media read failed at %llu (%zuB)",
            (unsigned long long) offset, length);
        return false;
    }
    range->last_error[0] = '\0';
    return true;
}

static bool media_file_describe(
    void *opaque, char *error, size_t error_size)
{
    MediaFileRange *range = opaque;
    if (range == NULL || error == NULL || error_size == 0
        || range->last_error[0] == '\0') return false;
    snprintf(error, error_size, "%s", range->last_error);
    return true;
}

MediaFileRange *media_file_range_open(
    Budget *budget, const char *path, uint64_t expected_length,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    struct stat status;
    if (budget == NULL || path == NULL || path[0] == '\0'
        || expected_length == 0
        || expected_length > LONG_MAX || stat(path, &status) != 0
        || status.st_size < 0
        || (uint64_t) status.st_size != expected_length) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "offline media size is invalid");
        return NULL;
    }
    FILE *file = fopen(path, "rb");
    MediaFileRange *range = file == NULL ? NULL
        : budget_calloc_category(
              budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*range));
    if (range == NULL) {
        if (file != NULL) fclose(file);
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "offline media could not be opened");
        return NULL;
    }
    range->budget = budget;
    range->file = file;
    range->length = expected_length;
    return range;
}

MediaRangeReader media_file_range_reader(MediaFileRange *range)
{
    return (MediaRangeReader) {
        .opaque = range,
        .length = range == NULL ? 0 : range->length,
        .read = media_file_read,
        .describe_failure = media_file_describe
    };
}

void media_file_range_close(MediaFileRange *range)
{
    if (range == NULL) return;
    if (range->file != NULL) fclose(range->file);
    Budget *budget = range->budget;
    memset(range, 0, sizeof(*range));
    budget_free(budget, range);
}
