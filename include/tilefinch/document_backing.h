#ifndef TILEFINCH_DOCUMENT_BACKING_H
#define TILEFINCH_DOCUMENT_BACKING_H

#include <stdbool.h>

#include "tilefinch/document.h"

typedef enum {
    DOCUMENT_BACKING_NONE = 0,
    DOCUMENT_BACKING_FULL
} DocumentBackingKind;

/* The document the BrowserEngine facade presents: the committed page's
   complete parsed DOM. */
typedef struct {
    DocumentBackingKind kind;
    const PocDocument *full_document;
} DocumentBacking;

void document_backing_clear(DocumentBacking *backing);
bool document_backing_init_full(DocumentBacking *backing,
                                const PocDocument *document);
bool document_backing_valid(const DocumentBacking *backing);
DocumentBackingKind document_backing_kind(const DocumentBacking *backing);
const PocDocument *document_backing_document(const DocumentBacking *backing);

#endif
