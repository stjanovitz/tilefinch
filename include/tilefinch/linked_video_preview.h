#ifndef TILEFINCH_LINKED_VIDEO_PREVIEW_H
#define TILEFINCH_LINKED_VIDEO_PREVIEW_H

#include "tilefinch/budget.h"
#include "tilefinch/navigation.h"

#define LINKED_VIDEO_PREVIEW_ATTEMPTS 2u
#define LINKED_VIDEO_PREVIEW_SCAN_BYTES (512u * 1024u)
#define LINKED_VIDEO_PREVIEW_TAG_BYTES 4096u
/* The head scanner decodes at most this many candidate <meta> tags, each
   parsed into at most this many DOM nodes. */
#define LINKED_VIDEO_PREVIEW_META_TAGS 64u
#define LINKED_VIDEO_PREVIEW_TAG_NODES 16u
/* The candidate search examines at most this many layout boxes per call,
   and no more than the first LINKED_VIDEO_PREVIEW_BOX_LIMIT of the page. */
#define LINKED_VIDEO_PREVIEW_BOXES_PER_CALL 256u
#define LINKED_VIDEO_PREVIEW_BOX_LIMIT 16384u
/* The linked page's head fetch times out after 3 s. The job as a whole --
   that fetch plus loading and publishing the image it names -- is dropped
   after 5 s, so a slow image load gets at least the remaining 2 s and an
   idle preview never holds its slot longer than that. */
#define LINKED_VIDEO_PREVIEW_FETCH_TIMEOUT_MS 3000L
#define LINKED_VIDEO_PREVIEW_JOB_LIFETIME_US UINT64_C(5000000)

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
