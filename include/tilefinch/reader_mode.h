#ifndef TILEFINCH_READER_MODE_H
#define TILEFINCH_READER_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/document.h"

typedef enum {
    READER_PAGE_RAW = 0,
    READER_PAGE_ARTICLE,
    READER_PAGE_LISTING,
    READER_PAGE_WATCH
} ReaderPageKind;

typedef struct {
    ReaderPageKind kind;
    uint32_t visited_nodes;
    uint32_t visible_text_bytes;
    uint16_t listing_entries;
    bool high_confidence;
    bool prepared;
    bool bounded_out;
} ReaderDocumentAnalysis;

/* Analyze and mark one loaded DOM in a single bounded operation. The markers
   are inert until the generic Reader stylesheet is enabled. The caller keeps
   the result beside the page and does not repeat this work while scrolling. */
bool reader_document_prepare(PocDocument *document,
                             ReaderDocumentAnalysis *analysis);

const char *reader_page_kind_name(ReaderPageKind kind);

#endif
