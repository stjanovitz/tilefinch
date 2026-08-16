#include "tilefinch/document_backing.h"

#include <string.h>

static bool backing_element_lookup(
    void *opaque, const char *identifier, size_t length,
    size_t *section_index, char tag_name[32], char stable_key[96])
{
    DocumentBacking *backing = opaque;
    return backing != NULL && backing->ops.element_lookup != NULL
        && backing->ops.element_lookup(
               backing->context, identifier, length, section_index,
               tag_name, stable_key);
}

static bool backing_selector_lookup(
    void *opaque, const char *selector, size_t length,
    size_t *section_index, char tag_name[32], char identifier[129],
    char stable_key[96])
{
    DocumentBacking *backing = opaque;
    return backing != NULL && backing->ops.selector_lookup != NULL
        && backing->ops.selector_lookup(
               backing->context, selector, length, section_index,
               tag_name, identifier, stable_key);
}

static bool backing_selector_collect(
    void *opaque, const char *selector, size_t length,
    TilefinchRemoteSelectorMatch *matches, size_t capacity, size_t *match_count)
{
    DocumentBacking *backing = opaque;
    return backing != NULL && backing->ops.selector_collect != NULL
        && backing->ops.selector_collect(
               backing->context, selector, length, matches, capacity,
               match_count);
}

static bool backing_descendant_collect(
    void *opaque, unsigned root_special, int what_to_show,
    TilefinchRemoteTraversalNode *nodes, size_t capacity, size_t *node_count)
{
    DocumentBacking *backing = opaque;
    return backing != NULL && backing->ops.descendant_collect != NULL
        && backing->ops.descendant_collect(
               backing->context, root_special, what_to_show, nodes,
               capacity, node_count);
}

static bool backing_node_read(
    void *opaque, const char *stable_key, size_t stable_key_length,
    size_t section_index, ScriptRemoteNodeReadKind kind,
    const char *name, size_t name_length, ScriptRemoteNodeReadResult *result)
{
    DocumentBacking *backing = opaque;
    return backing != NULL && backing->ops.node_read != NULL
        && backing->ops.node_read(
               backing->context, stable_key, stable_key_length,
               section_index, kind, name, name_length, result);
}

static bool backing_node_write(
    void *opaque, const char *stable_key, size_t stable_key_length,
    size_t section_index, ScriptRemoteNodeWriteKind kind,
    const char *name, size_t name_length,
    const char *value, size_t value_length)
{
    DocumentBacking *backing = opaque;
    return backing != NULL && backing->ops.node_write != NULL
        && backing->ops.node_write(
               backing->context, stable_key, stable_key_length,
               section_index, kind, name, name_length, value, value_length);
}

static bool backing_node_visibility(
    void *opaque, size_t section_index, size_t element_ordinal,
    size_t node_ordinal, unsigned node_type)
{
    DocumentBacking *backing = opaque;
    return backing != NULL && backing->ops.node_visibility != NULL
        && backing->ops.node_visibility(
               backing->context, section_index, element_ordinal,
               node_ordinal, node_type);
}

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
        .full_document = document,
        .revision = 1
    };
    return true;
}

bool document_backing_init_sections(DocumentBacking *backing,
                                    CompressedSectionStore *store,
                                    SectionPager *pager,
                                    const DocumentBackingOps *ops,
                                    void *context)
{
    if (backing == NULL || store == NULL || pager == NULL
        || !section_pager_valid(pager) || pager->store != store
        || store->section_count == 0) return false;
    *backing = (DocumentBacking) {
        .kind = DOCUMENT_BACKING_COMPRESSED_SECTIONS,
        .section_store = store,
        .section_pager = pager,
        .context = context,
        .revision = 1
    };
    if (ops != NULL) backing->ops = *ops;
    return true;
}

bool document_backing_valid(const DocumentBacking *backing)
{
    if (backing == NULL) return false;
    if (backing->kind == DOCUMENT_BACKING_FULL)
        return backing->full_document != NULL;
    return backing->kind == DOCUMENT_BACKING_COMPRESSED_SECTIONS
        && backing->section_store != NULL
        && backing->section_pager != NULL
        && section_pager_valid(backing->section_pager)
        && backing->section_pager->store == backing->section_store
        && backing->section_pager->current_section
               < backing->section_store->section_count;
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

size_t document_backing_section_count(const DocumentBacking *backing)
{
    return document_backing_kind(backing)
               == DOCUMENT_BACKING_COMPRESSED_SECTIONS
        ? backing->section_store->section_count : 0;
}

size_t document_backing_current_section(const DocumentBacking *backing)
{
    return document_backing_kind(backing)
               == DOCUMENT_BACKING_COMPRESSED_SECTIONS
        ? backing->section_pager->current_section : 0;
}

const char *document_backing_current_html(const DocumentBacking *backing,
                                          size_t *length)
{
    if (length != NULL) *length = 0;
    if (document_backing_kind(backing)
        != DOCUMENT_BACKING_COMPRESSED_SECTIONS) return NULL;
    if (length != NULL) *length = backing->section_pager->current_length;
    return backing->section_pager->current_html;
}

bool document_backing_select_section(DocumentBacking *backing,
                                     size_t section,
                                     int prefetch_direction)
{
    DocumentBackingSelection selection;
    if (!document_backing_prepare_section(
            backing, section, prefetch_direction, &selection)) return false;
    if (document_backing_commit_section(&selection)) return true;
    document_backing_abort_section(&selection);
    return false;
}

bool document_backing_prepare_section(DocumentBacking *backing,
                                      size_t section,
                                      int prefetch_direction,
                                      DocumentBackingSelection *selection)
{
    if (selection == NULL) return false;
    memset(selection, 0, sizeof(*selection));
    if (document_backing_kind(backing)
            != DOCUMENT_BACKING_COMPRESSED_SECTIONS
        || section >= backing->section_store->section_count) return false;
    selection->backing = backing;
    selection->base_revision = backing->revision;
    if (!section_pager_prepare(backing->section_pager, section,
                               prefetch_direction,
                               &selection->pager_selection)) {
        memset(selection, 0, sizeof(*selection));
        return false;
    }
    selection->active = true;
    return true;
}

bool document_backing_commit_section(DocumentBackingSelection *selection)
{
    if (selection == NULL || !selection->active
        || selection->backing == NULL
        || selection->backing->revision != selection->base_revision
        || !section_pager_commit(&selection->pager_selection)) return false;
    DocumentBacking *backing = selection->backing;
    backing->revision++;
    if (backing->revision == 0) backing->revision = 1;
    memset(selection, 0, sizeof(*selection));
    return true;
}

void document_backing_abort_section(DocumentBackingSelection *selection)
{
    if (selection == NULL) return;
    section_pager_abort(&selection->pager_selection);
    memset(selection, 0, sizeof(*selection));
}

void document_backing_uninstall(NavigationSession *navigation)
{
    if (navigation == NULL) return;
    /* Section identity and pending materialization requests are part of the
       binding contract. Clear the complete live top-level binding before its
       former backing/context may be released. */
    (void) navigation_rebind_top_level_remote_document(navigation, NULL);
}

bool document_backing_install(DocumentBacking *backing,
                              NavigationSession *navigation)
{
    if (navigation == NULL || !document_backing_valid(backing)) return false;
    ScriptRemoteDocumentBinding binding = {0};
    if (backing->kind == DOCUMENT_BACKING_COMPRESSED_SECTIONS) {
        binding = (ScriptRemoteDocumentBinding) {
            .element_lookup = backing->ops.element_lookup != NULL
                ? backing_element_lookup : NULL,
            .element_opaque = backing->ops.element_lookup != NULL
                ? backing : NULL,
            .selector_lookup = backing->ops.selector_lookup != NULL
                ? backing_selector_lookup : NULL,
            .selector_opaque = backing->ops.selector_lookup != NULL
                ? backing : NULL,
            .selector_collect = backing->ops.selector_collect != NULL
                ? backing_selector_collect : NULL,
            .selector_collect_opaque =
                backing->ops.selector_collect != NULL ? backing : NULL,
            .descendant_collect = backing->ops.descendant_collect != NULL
                ? backing_descendant_collect : NULL,
            .descendant_collect_opaque =
                backing->ops.descendant_collect != NULL ? backing : NULL,
            .node_read = backing->ops.node_read != NULL
                ? backing_node_read : NULL,
            .node_read_opaque = backing->ops.node_read != NULL
                ? backing : NULL,
            .node_write = backing->ops.node_write != NULL
                ? backing_node_write : NULL,
            .node_write_opaque = backing->ops.node_write != NULL
                ? backing : NULL,
            .node_visibility = backing->ops.node_visibility != NULL
                ? backing_node_visibility : NULL,
            .node_visibility_opaque =
                backing->ops.node_visibility != NULL ? backing : NULL,
            .section_identity = backing->section_pager->current_section
        };
    }
    return navigation_rebind_top_level_remote_document(
        navigation, &binding);
}
