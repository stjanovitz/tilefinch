#ifndef TILEFINCH_DATA_URL_H
#define TILEFINCH_DATA_URL_H

#include "tilefinch/budget.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DATA_URL_NOT_DATA = 0,
    DATA_URL_DECODED,
    DATA_URL_INVALID,
    DATA_URL_TOO_LARGE,
    DATA_URL_NO_MEMORY
} DataUrlDecodeResult;

DataUrlDecodeResult data_url_decode(
    Budget *budget, const char *url, size_t url_length,
    size_t maximum_decoded_bytes, unsigned char **data, size_t *length,
    char *media_type, size_t media_type_size);

#endif
