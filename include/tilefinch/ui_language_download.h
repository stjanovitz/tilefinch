#ifndef TILEFINCH_UI_LANGUAGE_DOWNLOAD_H
#define TILEFINCH_UI_LANGUAGE_DOWNLOAD_H

#include "tilefinch/fetch.h"

/* Construct the native language selection's anonymous, bounded catalog
   request. Failure publishes no request; the URL buffer may be truncated. */
bool tilefinch_ui_language_request(
    unsigned language, char *url, size_t capacity, FetchRequest *request);

#endif
