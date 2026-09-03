#ifndef TILEFINCH_READER_MODE_H
#define TILEFINCH_READER_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/document.h"
#include "tilefinch/style.h"

typedef enum {
    READER_PAGE_RAW = 0,
    READER_PAGE_ARTICLE,
    READER_PAGE_LISTING,
    READER_PAGE_WATCH,
    /* A bounded, action-preserving extraction of the complete visible body.
       Unlike Reader's article/listing classifier, Basic view may retain
       explicitly authored GET/search forms. */
    READER_PAGE_BASIC
} ReaderPageKind;

typedef struct {
    ReaderPageKind kind;
    uint32_t visited_nodes;
    uint32_t visible_text_bytes;
    uint32_t extracted_bytes;
    uint16_t listing_entries;
    uint16_t extracted_nodes;
    uint16_t retained_forms;
    uint16_t mapped_anchors;
    bool high_confidence;
    bool prepared;
    bool bounded_out;
    bool extraction_truncated;
} ReaderDocumentAnalysis;

/* Analyze one loaded DOM and install one hidden, bounded semantic Reader tree
   in a single operation. The raw DOM remains intact; the generic Reader
   stylesheet switches which tree is presented. The caller keeps the result
   beside the page and does not repeat this work while scrolling. */
bool reader_document_prepare(PocDocument *document,
                             ReaderDocumentAnalysis *analysis);
/* The navigation path supplies its already-built stylesheet so authored
   display/visibility rules participate in primary-media admission. Tests and
   reduced callers may use reader_document_prepare(), which retains the
   markup-only fallback. */
bool reader_document_prepare_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis);
/* Commit-time recovery keeps the native extracted tree private until author
   work has settled. The returned root is detached, document-owned, and must
   be connected or discarded through the exact-root helpers below. */
bool reader_document_prepare_detached_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis, lxb_dom_node_t **prepared_root);
bool reader_document_connect_prepared_view(
    PocDocument *document, lxb_dom_node_t *root, ReaderPageKind kind);
void reader_document_discard_exact_prepared_view(
    PocDocument *document, lxb_dom_node_t *root, ReaderPageKind kind);
/* Analyze and extract into temporary bounded storage without mutating the
   document. Frontends use this to decide whether automatic Reader admission
   is warranted before consuming the document's one extracted-view slot. */
bool reader_document_analyze_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis);
/* Blank-page recovery has a stricter admission boundary than manual Reader.
   Analyze and report the content shape, but install no marker or extracted
   tree unless the complete source and extraction fit their bounds. A declined
   bounded/truncated result leaves the authored DOM transactionally intact. */
bool reader_document_prepare_complete_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis);

/* Build the separately bounded Basic view from the visible authored body.
   The source subtree remains connected and authoritative. Only explicit,
   labeled GET forms are interactive; POST forms and unowned/inert buttons
   are flattened to non-actionable content. The complete variant builds the
   entire candidate in temporary storage and installs nothing when any
   structural/byte/form bound is reached. */
bool reader_document_prepare_basic_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis);
bool reader_document_prepare_basic_complete_with_stylesheet(
    PocDocument *document, const Stylesheet *stylesheet,
    ReaderDocumentAnalysis *analysis);

/* Remove the exact native extracted root most recently appended for `kind`
   and its body marker. This is the allocation-failure rollback used when the
   BrowserEngine cannot retain provenance for an otherwise prepared view. */
bool reader_document_discard_prepared_view(
    PocDocument *document, ReaderPageKind kind);

const char *reader_page_kind_name(ReaderPageKind kind);

#endif
