#include "tilefinch/linked_video_preview.h"
#include "tilefinch/fetch.h"
#include "tilefinch/platform.h"
#include "tilefinch/url.h"

#include <string.h>
#include <strings.h>
#include <lexbor/dom/interface.h>
#include <lexbor/dom/interfaces/element.h>

enum { SCAN_TEXT, SCAN_TAG, SCAN_COMMENT, SCAN_RAW };
enum { RAW_NONE, RAW_STYLE, RAW_SCRIPT, RAW_TITLE };

static bool name_is(lxb_dom_node_t *node, const char *name)
{
    size_t length = 0;
    const char *actual = document_element_name(node, &length);
    return actual != NULL && length == strlen(name)
        && strncasecmp(actual, name, length) == 0;
}

static bool space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static unsigned char lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char) (c + 'a' - 'A') : c;
}

static void scan_meta(LinkedVideoPreviewScanner *scan)
{
    if (++scan->meta_count > LINKED_VIDEO_PREVIEW_META_TAGS) {
        scan->stopped = true;
        return;
    }
    PocDocument fragment = {0};
    if (!document_parse(&fragment, scan->budget,
            scan->tag, scan->tag_length, scan->tag_length)) {
        document_destroy(&fragment);
        scan->stopped = true;
        return;
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(fragment.html);
    size_t visited = 0;
    for (lxb_dom_node_t *node = root;
         node != NULL && visited++ < LINKED_VIDEO_PREVIEW_TAG_NODES;) {
        if (name_is(node, "meta")) {
            size_t length = 0;
            const char *value = document_meta_image_content(node, &length);
            /* Absolute HTTPS metadata avoids guessing a different base from
               a truncated document. Actual image authority is checked again
               by the normal image loader (CSP, redirects, PNA, quotas). */
            if (value != NULL && length > 8u
                && length < sizeof(scan->image_url)
                && strncasecmp(value, "https://", 8u) == 0) {
                memcpy(scan->image_url, value, length);
                scan->image_url[length] = '\0';
                TilefinchUrl parsed;
                if (!tilefinch_url_parse(scan->image_url, &parsed)) scan->image_url[0] = '\0';
                else scan->stopped = true;
            }
            break;
        }
        if (node->first_child != NULL) { node = node->first_child; continue; }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    document_destroy(&fragment);
}

static void scan_tag(LinkedVideoPreviewScanner *scan)
{
    size_t at = 1u;
    bool closing = at < scan->tag_length && scan->tag[at] == '/';
    if (closing) at++;
    size_t begin = at;
    while (at < scan->tag_length && !space((unsigned char) scan->tag[at])
        && scan->tag[at] != '>' && scan->tag[at] != '/') at++;
    size_t length = at - begin;
#define TAG(name) (length == sizeof(name) - 1u \
    && strncasecmp(scan->tag + begin, name, length) == 0)
    scan->state = SCAN_TEXT;
    if ((closing && TAG("head")) || TAG("body") || TAG("frameset")
        || TAG("template") || TAG("noscript") || TAG("svg") || TAG("math")) {
        scan->stopped = true;
    } else if (!closing && TAG("meta")) {
        scan_meta(scan);
    } else if (!closing && (TAG("style") || TAG("script") || TAG("title"))) {
        scan->raw_kind = TAG("style") ? RAW_STYLE
            : TAG("script") ? RAW_SCRIPT : RAW_TITLE;
        scan->state = SCAN_RAW;
        scan->match = 0;
        scan->comment_tail = 0;
    } else if (!(TAG("html") || TAG("head") || TAG("base") || TAG("link")
                 || TAG("meta") || TAG("!doctype") || TAG("style")
                 || TAG("script") || TAG("title"))) {
        /* Other elements implicitly leave the head even without <body>.
           Do not turn body/inert markup into preview declarations. */
        scan->stopped = true;
    }
#undef TAG
    scan->tag_length = 0;
}

void linked_video_preview_scanner_init(
    LinkedVideoPreviewScanner *scan, Budget *budget)
{
    memset(scan, 0, sizeof(*scan));
    scan->budget = budget;
}

bool linked_video_preview_scanner_feed(
    LinkedVideoPreviewScanner *scan, const unsigned char *bytes, size_t length)
{
    if (scan == NULL || scan->budget == NULL || scan->stopped) return false;
    for (size_t i = 0; i < length; i++) {
        if (scan->bytes == LINKED_VIDEO_PREVIEW_SCAN_BYTES) {
            scan->stopped = true;
            break;
        }
        scan->bytes++;
        unsigned char c = bytes[i];
        if (scan->state == SCAN_COMMENT) {
            if (c == '>' && scan->comment_tail >= 2u) {
                scan->state = SCAN_TEXT;
                scan->comment_tail = 0;
            } else scan->comment_tail = c == '-'
                ? (scan->comment_tail < 2u ? scan->comment_tail + 1u : 2u) : 0u;
        } else if (scan->state == SCAN_RAW) {
            const char *end = scan->raw_kind == RAW_STYLE ? "</style"
                : scan->raw_kind == RAW_SCRIPT ? "</script" : "</title";
            size_t end_length = strlen(end);
            /* HTML script double-escape states are intentionally outside
               this scanner. Decline instead of mistaking a script string
               for authoritative head metadata. */
            if (scan->raw_kind == RAW_SCRIPT) {
                const char *escaped = "<!--";
                scan->comment_tail = c == (unsigned char) escaped[scan->comment_tail]
                    ? scan->comment_tail + 1u : c == '<' ? 1u : 0u;
                if (scan->comment_tail == 4u) { scan->stopped = true; break; }
            }
            if (scan->match == end_length) {
                if (space(c) || c == '/' || c == '>') {
                    memcpy(scan->tag, end, end_length);
                    scan->tag[end_length] = (char) c;
                    scan->tag_length = end_length + 1u;
                    scan->quote = 0;
                    scan->state = SCAN_TAG;
                    if (c == '>') scan_tag(scan);
                    continue;
                }
                scan->match = 0;
            }
            scan->match = lower(c) == (unsigned char) end[scan->match]
                ? scan->match + 1u : c == '<' ? 1u : 0u;
        } else if (scan->state == SCAN_TAG) {
            if (scan->tag_length == sizeof(scan->tag) - 1u) {
                scan->stopped = true;
                break;
            }
            scan->tag[scan->tag_length++] = (char) c;
            if (scan->tag_length == 4u && memcmp(scan->tag, "<!--", 4u) == 0) {
                scan->state = SCAN_COMMENT;
                scan->tag_length = 0;
                scan->comment_tail = 0;
            } else if (scan->quote != 0) {
                if (c == scan->quote) scan->quote = 0;
            } else if (c == '\'' || c == '"') scan->quote = c;
            else if (c == '>') scan_tag(scan);
        } else if (c == '<') {
            scan->tag[0] = '<';
            scan->tag_length = 1u;
            scan->quote = 0;
            scan->state = SCAN_TAG;
        } else if (!space(c)) {
            /* Real head text starts the body in the HTML tree builder. */
            scan->stopped = true;
        }
        if (scan->stopped) break;
    }
    return !scan->stopped;
}

struct LinkedVideoPreviewJob {
    LinkedVideoPreviewScanner scanner;
    NavigationSession *session;
    uint64_t request;
    uint64_t content_generation;
    uint64_t started_us;
    long node_handle;
    lxb_dom_node_t *node;
    ImagePriorityLoadJob *image;
    uint16_t width, height;
    bool headers_ok;
    bool publication_pending;
};

static bool preview_headers(void *opaque, const FetchResult *result)
{
    struct LinkedVideoPreviewJob *job = opaque;
    job->headers_ok = result->status_code >= 200 && result->status_code < 300
        && !result->redirect_origin_tainted
        && strncasecmp(result->content_type, "text/html", 9u) == 0
        && (result->content_type[9] == '\0' || result->content_type[9] == ';'
            || space((unsigned char) result->content_type[9]))
        && tilefinch_url_same_origin(job->session->page.document_url,
                                    result->effective_url);
    return job->headers_ok;
}

static bool preview_body(void *opaque, const unsigned char *bytes, size_t length)
{
    struct LinkedVideoPreviewJob *job = opaque;
    return job->headers_ok
        && linked_video_preview_scanner_feed(&job->scanner, bytes, length);
}

void navigation_destroy_linked_video_preview(
    NavigationSession *session, NavigationPage *page)
{
    if (session == NULL || page == NULL) return;
    struct LinkedVideoPreviewJob *job = page->linked_video_preview;
    if (job == NULL) return;
    if (job->request != 0) {
        (void) fetch_scheduler_cancel(page->resource_scheduler,
                                      job->request, "preview lookup finished");
        (void) fetch_scheduler_discard(page->resource_scheduler, job->request);
    }
    images_priority_load_destroy(job->image);
    budget_free(session->budget, job);
    page->linked_video_preview = NULL;
}

static bool preview_link(NavigationSession *session, lxb_dom_node_t *node,
                         char *url, size_t capacity)
{
    size_t length = 0;
    const char *poster = document_attribute(node, "poster", &length);
    if (poster != NULL && length != 0) return false;
    bool media = document_attribute(node, "src", &length) != NULL && length != 0;
    size_t count = 0;
    for (lxb_dom_node_t *at = node->first_child; !media && at != NULL
         && count++ < 16u; at = at->next) {
        media = name_is(at, "source")
            && document_attribute(at, "src", &length) != NULL && length != 0;
    }
    if (!media) return false;
    count = 0;
    for (lxb_dom_node_t *at = node->parent; at != NULL && count++ < 32u; at = at->parent) {
        if (!name_is(at, "a")) continue;
        const char *href = document_attribute(at, "href", &length);
        if (href == NULL || length == 0 || length >= capacity || href[0] == '#'
            || lxb_dom_element_has_attribute(lxb_dom_interface_element(at),
                (const lxb_char_t *) "download", 8u)) return false;
        memcpy(url, href, length);
        url[length] = '\0';
        char resolved[NAVIGATION_URL_LIMIT];
        if (!fetch_resolve_url(session->page.resource_base_url, url,
                resolved, sizeof(resolved))
            || strncasecmp(resolved, "https://", 8u) != 0
            || !tilefinch_url_same_origin(session->page.document_url, resolved)
            || strlen(resolved) >= capacity) return false;
        size_t current_length = strcspn(session->page.document_url, "#");
        size_t resolved_length = strcspn(resolved, "#");
        if (current_length == resolved_length
            && memcmp(resolved, session->page.document_url, current_length) == 0) return false;
        memcpy(url, resolved, strlen(resolved) + 1u);
        return true;
    }
    return false;
}

/* Drop a job whose document changed, whose video left the DOM, or which
   outlived LINKED_VIDEO_PREVIEW_JOB_LIFETIME_US. */
static bool preview_job_stale(NavigationSession *session,
                              const struct LinkedVideoPreviewJob *job)
{
    NavigationPage *page = &session->page;
    return job->content_generation != page->document.content_generation
        || (job->node_handle != 0 && (page->runtime == NULL
            || script_runtime_node_handle_resolve_connected(
                page->runtime, job->node_handle) != job->node))
        || tilefinch_platform_monotonic_time_us() - job->started_us
            > LINKED_VIDEO_PREVIEW_JOB_LIFETIME_US;
}

/* Create the job for one candidate video and enqueue its head fetch. */
static void preview_start_fetch(NavigationSession *session,
                                const LayoutNodeBox *box, long handle,
                                const char *url)
{
    NavigationPage *page = &session->page;
    unsigned slot = page->linked_video_preview_attempts++;
    page->linked_video_preview_nodes[slot] = box->node;
    page->linked_video_preview_handles[slot] = handle;
    struct LinkedVideoPreviewJob *job = budget_calloc_category(
        session->budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*job));
    if (job == NULL) return;
    page->linked_video_preview = job;
    linked_video_preview_scanner_init(&job->scanner, session->budget);
    job->session = session;
    job->node = box->node;
    job->node_handle = handle;
    job->content_generation = page->document.content_generation;
    job->started_us = tilefinch_platform_monotonic_time_us();
    job->width = box->width > UINT16_MAX ? UINT16_MAX : (uint16_t) box->width;
    job->height = box->height > UINT16_MAX ? UINT16_MAX : (uint16_t) box->height;
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = page->document_url,
        .top_level_url = page->document_url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_OMIT,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    FetchRequest transport = {.accept = "text/html",
        .redirect_same_origin_only = true};
    FetchPreparedPageRequest *prepared = budget_malloc_category(session->budget,
        BUDGET_CATEGORY_RESOURCE, sizeof(*prepared));
    if (prepared != NULL && fetch_prepare_page_request_context(&context,
            page->document_url, page->referrer_policy, session->browser_session,
            &page->document.content_security_policy, NULL, &transport, prepared, NULL)) {
        FetchStreamOptions stream = {
            .on_headers = preview_headers, .on_body = preview_body, .opaque = job,
            .chunk_bytes = 4096u
        };
        job->request = fetch_scheduler_enqueue_stream(page->resource_scheduler,
            url, fetch_prepared_page_request(prepared),
            LINKED_VIDEO_PREVIEW_SCAN_BYTES,
            LINKED_VIDEO_PREVIEW_FETCH_TIMEOUT_MS, &stream);
    }
    budget_free(session->budget, prepared);
    if (job->request == 0) navigation_destroy_linked_video_preview(session, page);
}

/* Resumably walk the laid-out boxes for a visible, poster-less video inside a
   same-origin link, and start a job for the first untried one. Returns
   whether work was done or remains. */
static bool preview_find_candidate(NavigationSession *session, int scroll_y)
{
    NavigationPage *page = &session->page;
    if (page->linked_video_preview_attempts >= LINKED_VIDEO_PREVIEW_ATTEMPTS
        || page->resource_scheduler == NULL
        || !navigation_image_publication_allowed(session)) return false;
    if (!page->linked_video_preview_scan_valid
        || page->linked_video_preview_scan_generation != session->incremental_relayouts
        || page->linked_video_preview_scan_content != page->document.content_generation
        || page->linked_video_preview_scan_scroll_y != scroll_y) {
        page->linked_video_preview_scan_valid = true;
        page->linked_video_preview_scan_generation = session->incremental_relayouts;
        page->linked_video_preview_scan_content = page->document.content_generation;
        page->linked_video_preview_scan_scroll_y = scroll_y;
        page->linked_video_preview_scan_cursor = 0;
    }
    size_t scanned = 0;
    size_t limit = page->layout.node_box_count < LINKED_VIDEO_PREVIEW_BOX_LIMIT
        ? page->layout.node_box_count : LINKED_VIDEO_PREVIEW_BOX_LIMIT;
    while (page->linked_video_preview_scan_cursor < limit
           && scanned++ < LINKED_VIDEO_PREVIEW_BOXES_PER_CALL) {
        size_t at = page->linked_video_preview_scan_cursor++;
        const LayoutNodeBox *box = &page->layout.node_boxes[at];
        if (box->node == NULL || box->width <= 0 || box->height <= 0
            || box->y >= (int64_t) scroll_y + session->viewport.css_height
            || (int64_t) box->y + box->height <= scroll_y
            || box->x >= session->viewport.css_width || (int64_t) box->x + box->width <= 0
            || !name_is(box->node, "video") || images_find_node(&page->images, box->node) != NULL)
            continue;
        long handle = page->runtime != NULL
            ? script_runtime_node_weak_handle(page->runtime, box->node) : 0;
        if (page->runtime != NULL && handle == 0) continue;
        bool tried = false;
        for (unsigned i = 0; i < page->linked_video_preview_attempts; i++)
            tried |= page->linked_video_preview_nodes[i] == box->node
                && page->linked_video_preview_handles[i] == handle;
        if (tried) continue;
        ImagePriorityTarget visible = {.node = box->node, .weak_handle = handle};
        if (!navigation_image_target_visible(session, &visible, true)) continue;
        char url[NAVIGATION_URL_LIMIT];
        if (!preview_link(session, box->node, url, sizeof(url))) continue;
        preview_start_fetch(session, box, handle, url);
        return true;
    }
    return page->linked_video_preview_scan_cursor < limit;
}

/* Advance a running job: scan the linked page's head for an image, then
   load that image and publish it through the ordinary relayout path. */
static bool preview_pump(NavigationSession *session,
                         struct LinkedVideoPreviewJob *job)
{
    NavigationPage *page = &session->page;
    if (job->image == NULL) {
        const FetchPumpQuota quota = {.maximum_body_callbacks = 1,
            .maximum_body_bytes = 4096u, .maximum_time_us = 2000u};
        (void) fetch_scheduler_pump_bounded(page->resource_scheduler, 1, 0, &quota, NULL);
        if (job->scanner.image_url[0] == '\0') {
            if (job->scanner.stopped || fetch_scheduler_request_complete(
                    page->resource_scheduler, job->request)) navigation_destroy_linked_video_preview(session, page);
            return true;
        }
        (void) fetch_scheduler_cancel(page->resource_scheduler, job->request, "preview image found");
        (void) fetch_scheduler_discard(page->resource_scheduler, job->request);
        job->request = 0;
        ImagePriorityTarget target = {.node = job->node, .source = job->scanner.image_url,
            .kind = IMAGE_PRIORITY_KIND_LINKED_VIDEO, .display_width = job->width,
            .display_height = job->height};
        job->image = images_priority_load_begin_batch(&page->document, &page->stylesheet,
            &page->images, &target, 1, session->budget, page->resource_base_url,
            page->document_url, page->referrer_policy, session->maximum_images,
            session->maximum_image_bytes, session->maximum_image_file_bytes,
            session->maximum_decoded_image_bytes, session->resource_timeout_ms,
            page->resource_scheduler, session->browser_session);
        if (job->image == NULL) navigation_destroy_linked_video_preview(session, page);
        return true;
    }
    size_t loaded = page->images.stats.loaded;
    ImagePriorityLoadStatus status = images_priority_load_pump(job->image);
    if (page->images.stats.loaded != loaded) {
        images_priority_load_commit_progress(job->image);
        layout_reuse_cache_update_images(page->layout_reuse, &page->images);
        job->publication_pending = true;
    }
    if (job->publication_pending) {
        /* The existing publication path remains preemptible and retains the
           resource if a relayout yields. No author poster attribute is set. */
        if (session->deferred_image_ready != NULL)
            session->deferred_image_ready(session->deferred_image_ready_opaque, session);
        if (!navigation_relayout(session)) return true;
        job->publication_pending = false;
    }
    if (status != IMAGE_PRIORITY_LOAD_PENDING) navigation_destroy_linked_video_preview(session, page);
    return true;
}

bool navigation_run_linked_video_preview(NavigationSession *session)
{
    if (session == NULL || !session->page.loaded || session->maximum_images == 0
        || session->maximum_image_file_bytes == 0) return false;
    NavigationPage *page = &session->page;
    struct LinkedVideoPreviewJob *job = page->linked_video_preview;
    if (job != NULL && preview_job_stale(session, job)) {
        navigation_destroy_linked_video_preview(session, page);
        return true;
    }
    if (job == NULL) {
        const NavigationEntry *entry = navigation_current(session);
        return preview_find_candidate(session, entry == NULL ? 0 : entry->scroll_y);
    }
    return preview_pump(session, job);
}
