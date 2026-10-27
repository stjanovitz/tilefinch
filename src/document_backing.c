#include "tilefinch/document_backing.h"

#include <string.h>

void document_backing_clear(DocumentBacking *backing)
{
    if (backing != NULL) memset(backing, 0, sizeof(*backing));
}

bool document_backing_init_full(DocumentBacking *backing,
                                const PocDocument *document)
{
    if (backing == NULL || document == NULL) return false;
    *backing = (DocumentBacking) {
        .kind = DOCUMENT_BACKING_FULL,
        .full_document = document
    };
    return true;
}

bool document_backing_valid(const DocumentBacking *backing)
{
    return backing != NULL && backing->kind == DOCUMENT_BACKING_FULL
        && backing->full_document != NULL;
}

DocumentBackingKind document_backing_kind(const DocumentBacking *backing)
{
    return document_backing_valid(backing) ? backing->kind
                                            : DOCUMENT_BACKING_NONE;
}

const PocDocument *document_backing_document(const DocumentBacking *backing)
{
    return document_backing_kind(backing) == DOCUMENT_BACKING_FULL
        ? backing->full_document : NULL;
}
