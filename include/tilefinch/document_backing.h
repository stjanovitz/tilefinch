#ifndef TILEFINCH_DOCUMENT_BACKING_H
#define TILEFINCH_DOCUMENT_BACKING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/section_pager.h"
#include "tilefinch/section_store.h"

typedef enum {
    DOCUMENT_BACKING_NONE = 0,
    DOCUMENT_BACKING_FULL,
    DOCUMENT_BACKING_COMPRESSED_SECTIONS
} DocumentBackingKind;

/*
 * A single semantic-operation table replaces the independent callback/context
 * pairs used by the compatibility NavigationSession API.  All callbacks share
 * one context and are optional.  The backing, operation table, and context
 * must remain valid while installed directly on a NavigationSession.
 */
typedef struct {
    ScriptRemoteElementLookupCallback element_lookup;
    ScriptRemoteSelectorLookupCallback selector_lookup;
    ScriptRemoteSelectorCollectCallback selector_collect;
    ScriptRemoteDescendantCollectCallback descendant_collect;
    ScriptRemoteNodeReadCallback node_read;
    ScriptRemoteNodeWriteCallback node_write;
    ScriptNodeVisibilityCallback node_visibility;
} DocumentBackingOps;

typedef struct {
    DocumentBackingKind kind;
    const PocDocument *full_document;
    CompressedSectionStore *section_store;
    SectionPager *section_pager;
    DocumentBackingOps ops;
    void *context;
    uint64_t revision;
} DocumentBacking;

typedef struct {
    DocumentBacking *backing;
    SectionPagerSelection pager_selection;
    uint64_t base_revision;
    bool active;
} DocumentBackingSelection;

void document_backing_clear(DocumentBacking *backing);
bool document_backing_init_full(DocumentBacking *backing,
                                const PocDocument *document);
bool document_backing_init_sections(DocumentBacking *backing,
                                    CompressedSectionStore *store,
                                    SectionPager *pager,
                                    const DocumentBackingOps *ops,
                                    void *context);
bool document_backing_valid(const DocumentBacking *backing);
DocumentBackingKind document_backing_kind(const DocumentBacking *backing);
const PocDocument *document_backing_document(const DocumentBacking *backing);
size_t document_backing_section_count(const DocumentBacking *backing);
size_t document_backing_current_section(const DocumentBacking *backing);
const char *document_backing_current_html(const DocumentBacking *backing,
                                          size_t *length);
bool document_backing_prepare_section(DocumentBacking *backing,
                                      size_t section,
                                      int prefetch_direction,
                                      DocumentBackingSelection *selection);
bool document_backing_commit_section(DocumentBackingSelection *selection);
void document_backing_abort_section(DocumentBackingSelection *selection);
bool document_backing_select_section(DocumentBacking *backing,
                                     size_t section,
                                     int prefetch_direction);

/* Compatibility bridge for the existing seven NavigationSession setters. */
bool document_backing_install(DocumentBacking *backing,
                              NavigationSession *navigation);
void document_backing_uninstall(NavigationSession *navigation);

#endif
