#ifndef TILEFINCH_FETCH_FAULT_H
#define TILEFINCH_FETCH_FAULT_H

#include <stdbool.h>

#include "tilefinch/fetch.h"

/* Internal host-lab hook. A custom transport may opt into consuming it. */
bool tilefinch_fetch_consume_injected_failure(FetchResult *result);

#endif
