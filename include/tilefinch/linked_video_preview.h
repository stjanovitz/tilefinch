#ifndef TILEFINCH_LINKED_VIDEO_PREVIEW_H
#define TILEFINCH_LINKED_VIDEO_PREVIEW_H

#include "tilefinch/budget.h"
#include "tilefinch/navigation.h"

#define LINKED_VIDEO_PREVIEW_ATTEMPTS 2u
#define LINKED_VIDEO_PREVIEW_SCAN_BYTES (512u * 1024u)
#define LINKED_VIDEO_PREVIEW_TAG_BYTES 4096u

/* A deliberately conservative head scanner, not an HTML rendering parser.
   Complete candidate meta tags are decoded by the ordinary HTML parser.
   Raw text is discarded; ambiguous/oversized markup declines the fallback. */
typedef struct {
    Budget *budget;
    char image_url[2048];
    char tag[LINKED_VIDEO_PREVIEW_TAG_BYTES];
    size_t bytes;
    size_t tag_length;
    unsigned meta_count;
    unsigned state;
    unsigned raw_kind;
    unsigned match;
    unsigned comment_tail;
    unsigned char quote;
    bool stopped;
} LinkedVideoPreviewScanner;

void linked_video_preview_scanner_init(
    LinkedVideoPreviewScanner *scanner, Budget *budget);
bool linked_video_preview_scanner_feed(
    LinkedVideoPreviewScanner *scanner, const unsigned char *bytes, size_t length);

/* Optional page-owned idle work. Never executes the linked document, changes
   author markup, or opens a decoder. Returns whether it did/pends work. */
bool navigation_run_linked_video_preview(NavigationSession *session);
void navigation_destroy_linked_video_preview(
    NavigationSession *session, NavigationPage *page);

#endif
