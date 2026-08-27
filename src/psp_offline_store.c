#include "tilefinch/psp_offline_store.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/web_app_manifest.h"
#include "tilefinch/resources.h"
#include "image_decode_internal.h"

#define OFFLINE_ICON_DOWNLOAD_LIMIT (256u * 1024u)

#define OFFLINE_ROOT "https://tilefinch.local/offline"
#define INTERNAL_ORIGIN "https://tilefinch.local"

struct PspOfflineAppPreparation {
    PspUiOfflineAppPreview view;
    TilefinchWebAppManifest manifest;
    char source_url[OFFLINE_LIBRARY_URL_LIMIT];
    unsigned char icon[OFFLINE_LIBRARY_APP_ICON_LIMIT];
    size_t icon_length;
};

static bool offline_internal_url(const char *url)
{
    size_t origin_length = sizeof(INTERNAL_ORIGIN) - 1u;
    return url != NULL
        && strncmp(url, INTERNAL_ORIGIN, origin_length) == 0
        && (url[origin_length] == '\0' || url[origin_length] == '/');
}

static void offline_status(PspOfflineStore *store, const char *text)
{
    if (store == NULL) return;
    snprintf(store->status, sizeof(store->status), "%s",
             text == NULL ? "OFFLINE LIBRARY ERROR" : text);
}

static bool offline_route_id(
    const char *url, const char *route, uint32_t *id)
{
    if (url == NULL || route == NULL || id == NULL) return false;
    char prefix[128];
    int written = snprintf(prefix, sizeof(prefix), "%s/%s?id=", OFFLINE_ROOT,
                           route);
    if (written < 0 || (size_t) written >= sizeof(prefix)
        || strncmp(url, prefix, (size_t) written) != 0) return false;
    const unsigned char *at = (const unsigned char *) url + written;
    if (!isdigit(*at)) return false;
    uint32_t parsed = 0;
    while (isdigit(*at)) {
        unsigned digit = *at++ - '0';
        if (parsed > (UINT32_MAX - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    if (*at != 0 || parsed == 0) return false;
    *id = parsed;
    return true;
}

static bool offline_commit(
    BrowserEngine *engine, const char *url, char *html, size_t length,
    bool record_history)
{
    if (engine == NULL || url == NULL || html == NULL) return false;
    (void) browser_engine_set_user_css(engine, "", 0);
    bool loaded = browser_engine_commit_html(
        engine, url, html, length, record_history);
    budget_free(browser_engine_budget(engine), html);
    return loaded && browser_engine_refresh_shell(engine)
        && browser_engine_render_frame(engine, NULL);
}

void psp_offline_store_init(
    PspOfflineStore *store, Budget *budget, BrowserSession *session,
    const char *directory)
{
    if (store == NULL) return;
    memset(store, 0, sizeof(*store));
    offline_library_init(&store->library, budget, directory);
    store->session = session;
    offline_download_manager_init(
        &store->download, budget, session, &store->library);
}

static bool offline_fetch_page_resource(
    PspOfflineStore *store, NavigationSession *navigation,
    const char *page_url, const char *url,
    TilefinchRequestDestination destination, size_t maximum,
    const char *accept, FetchResult *result)
{
    FetchPageSecurityContext security = {
        .document_url = page_url, .top_level_url = page_url,
        .referrer_source_url = page_url,
        .referrer_policy = navigation->page.referrer_policy,
        .session = store->session,
        .content_security_policy =
            &navigation->page.document.content_security_policy
    };
    FetchPageRequestDescriptor descriptor = {
        .target_url = url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .destination = destination,
        .transport = {.accept = accept, .allow_http_errors = true}
    };
    FetchPreparedPageRequest prepared;
    if (!fetch_prepare_page_request(&security, &descriptor, &prepared, NULL))
        return false;
    const FetchRequest *request = fetch_prepared_page_request(&prepared);
    return request != NULL && navigation->page.resource_scheduler != NULL
        && fetch_scheduler_request(navigation->page.resource_scheduler, url,
                                   request, maximum, 15000, result)
        && result->status_code >= 200 && result->status_code < 300;
}

void psp_offline_store_discard_app_preparation(PspOfflineStore *store)
{
    if (store == NULL || store->app_preparation == NULL) return;
    budget_free(store->library.budget, store->app_preparation);
    store->app_preparation = NULL;
}

const PspUiOfflineAppPreview *psp_offline_store_app_preview(
    const PspOfflineStore *store)
{
    return store == NULL || store->app_preparation == NULL
        ? NULL : &store->app_preparation->view;
}

static uint16_t offline_known_unavailable_resources(
    const NavigationPage *page)
{
    if (page == NULL) return 0;
    size_t count = page->external_stylesheets.failed
        + page->external_stylesheets.skipped_limit
        + page->external_stylesheets.skipped_pressure
        + page->images.stats.failed
        + page->images.stats.unsupported
        + page->images.stats.skipped_limit
        + page->external_fonts.failed
        + page->external_fonts.unsupported
        + page->external_fonts.skipped_limit;
    return count > UINT16_MAX ? UINT16_MAX : (uint16_t) count;
}

bool psp_offline_store_prepare_current_app(
    PspOfflineStore *store, BrowserEngine *engine)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    const NavigationEntry *entry = navigation_current(navigation);
    if (store == NULL || store->session == NULL || navigation == NULL
        || !navigation->page.loaded || entry == NULL || entry->url == NULL
        || strncmp(entry->url, "https://", 8u) != 0
        || offline_internal_url(entry->url)) {
        offline_status(store, "THIS PAGE CANNOT BE INSTALLED");
        return false;
    }
    psp_offline_store_discard_app_preparation(store);
    size_t href_length = 0;
    const char *href = document_web_app_manifest_href(
        &navigation->page.document, &href_length);
    char href_copy[OFFLINE_LIBRARY_URL_LIMIT];
    if (href != NULL && href_length < sizeof(href_copy)) {
        memcpy(href_copy, href, href_length);
        href_copy[href_length] = '\0';
    } else href = NULL;
    char manifest_url[OFFLINE_LIBRARY_URL_LIMIT];
    if (href == NULL || !fetch_resolve_url(
            navigation->page.resource_base_url[0] == '\0' ? entry->url
                : navigation->page.resource_base_url,
            href == NULL ? "" : href_copy, manifest_url,
            sizeof(manifest_url))) {
        offline_status(store, "THIS PAGE HAS NO WEB APP MANIFEST");
        return false;
    }
    FetchResult *manifest_result = fetch_result_create(
        browser_engine_budget(engine));
    if (manifest_result == NULL) return false;
    bool fetched = offline_fetch_page_resource(
        store, navigation, entry->url, manifest_url,
        TILEFINCH_DESTINATION_FETCH, TILEFINCH_WEB_APP_MANIFEST_LIMIT,
        "application/manifest+json,application/json;q=0.9,*/*;q=0.1",
        manifest_result);
    TilefinchWebAppManifest manifest;
    char error[160] = {0};
    bool parsed = fetched && tilefinch_web_app_manifest_parse(
        manifest_result->data, manifest_result->length, manifest_url,
        entry->url, &manifest, error, sizeof(error));
    fetch_result_free(manifest_result);
    if (!parsed) {
        offline_status(store, error[0] == '\0'
                       ? "WEB APP MANIFEST COULD NOT BE LOADED" : error);
        return false;
    }
    unsigned char *icon = NULL;
    size_t icon_length = 0;
    if (manifest.icon_url[0] != '\0') {
        FetchResult *icon_result = fetch_result_create(
            browser_engine_budget(engine));
        if (icon_result != NULL
            && offline_fetch_page_resource(
                store, navigation, entry->url, manifest.icon_url,
                TILEFINCH_DESTINATION_IMAGE, OFFLINE_ICON_DOWNLOAD_LIMIT,
                "image/avif,image/webp,image/png,image/jpeg,*/*;q=0.1",
                icon_result)) {
            icon = budget_malloc_category(
                browser_engine_budget(engine), BUDGET_CATEGORY_SESSION,
                icon_result->length);
            if (icon != NULL) {
                memcpy(icon, icon_result->data, icon_result->length);
                icon_length = icon_result->length;
            }
        }
        fetch_result_free(icon_result);
    }
    unsigned char thumbnail[OFFLINE_LIBRARY_APP_ICON_LIMIT];
    size_t thumbnail_length = 0;
    if (icon != NULL) {
        int width = 0, height = 0, components = 0;
        if (image_decode_probe_info(
                browser_engine_budget(engine), icon, icon_length,
                &width, &height, &components, NULL)
                == IMAGE_DECODE_PROBE_SUPPORTED
            && width > 0 && height > 0) {
            ImageResource source = {
                .encoded = icon, .encoded_length = icon_length,
                .source_width = width, .source_height = height,
                .width = OFFLINE_LIBRARY_APP_ICON_EDGE,
                .height = OFFLINE_LIBRARY_APP_ICON_EDGE
            };
            unsigned char *pixels = NULL;
            if (image_resource_decode_checked(
                    &source, browser_engine_budget(engine), &pixels)
                    == IMAGE_DECODE_SUCCEEDED) {
                memcpy(thumbnail, pixels, sizeof(thumbnail));
                thumbnail_length = sizeof(thumbnail);
                image_resource_free_decoded(browser_engine_budget(engine),
                                             pixels);
            }
        }
    }
    PspOfflineAppPreparation *preparation = budget_calloc_category(
        browser_engine_budget(engine), BUDGET_CATEGORY_SESSION,
        1u, sizeof(*preparation));
    OfflineWebAppPreview preview = {0};
    bool prepared = preparation != NULL
        && offline_library_preview_web_app(
            &store->library, &navigation->page.document, store->session,
            entry->url, &manifest,
            thumbnail_length == 0 ? NULL : thumbnail, thumbnail_length,
            &preview, error, sizeof(error));
    if (prepared) {
        preparation->manifest = manifest;
        snprintf(preparation->source_url, sizeof(preparation->source_url),
                 "%s", entry->url);
        preparation->icon_length = thumbnail_length;
        if (thumbnail_length != 0)
            memcpy(preparation->icon, thumbnail, thumbnail_length);
        const char *name = manifest.short_name[0] != '\0'
            ? manifest.short_name : manifest.name;
        snprintf(preparation->view.name, sizeof(preparation->view.name),
                 "%s", name[0] == '\0'
                    ? (navigation->page.document.title == NULL
                           ? "Offline app" : navigation->page.document.title)
                    : name);
        preparation->view.estimated_bytes = preview.estimated_bytes;
        preparation->view.captured_resources =
            preview.resource_count > UINT16_MAX
                ? UINT16_MAX : (uint16_t) preview.resource_count;
        preparation->view.unavailable_resources =
            offline_known_unavailable_resources(&navigation->page);
        preparation->view.theme_color = manifest.theme_color;
        preparation->view.theme_alpha = manifest.theme_alpha;
        preparation->view.theme_color_valid = manifest.theme_color_valid;
        preparation->view.display_mode = (uint8_t) manifest.display_mode;
        preparation->view.operation = (uint8_t) preview.operation;
        store->app_preparation = preparation;
    } else {
        budget_free(browser_engine_budget(engine), preparation);
    }
    budget_free(browser_engine_budget(engine), icon);
    offline_status(store, prepared ? "OFFLINE APP PREVIEW READY"
                                   : (error[0] == '\0'
                                      ? "OFFLINE APP COULD NOT BE PREPARED"
                                      : error));
    return prepared;
}

bool psp_offline_store_install_current_app(
    PspOfflineStore *store, BrowserEngine *engine)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    const NavigationEntry *entry = navigation_current(navigation);
    if (store == NULL || store->app_preparation == NULL
        || navigation == NULL || !navigation->page.loaded || entry == NULL
        || entry->url == NULL
        || strcmp(entry->url, store->app_preparation->source_url) != 0) {
        offline_status(store, "INSTALL PREVIEW EXPIRED - TRY AGAIN");
        psp_offline_store_discard_app_preparation(store);
        return false;
    }
    PspOfflineAppPreparation *preparation = store->app_preparation;
    uint8_t operation = preparation->view.operation;
    char error[160] = {0};
    uint32_t id = 0;
    bool saved = offline_library_save_web_app(
        &store->library, &navigation->page.document, store->session,
        preparation->source_url, &preparation->manifest,
        preparation->icon_length == 0 ? NULL : preparation->icon,
        preparation->icon_length, &id, error, sizeof(error));
    if (saved)
        for (size_t at = 0; at < OFFLINE_LIBRARY_ITEM_LIMIT; at++)
            if (store->app_icon_ids[at] == id)
                store->app_icon_ids[at] = 0;
    psp_offline_store_discard_app_preparation(store);
    offline_status(store, saved
                                ? (operation == OFFLINE_WEB_APP_UPDATE
                                       ? "OFFLINE APP UPDATED"
                                       : operation == OFFLINE_WEB_APP_REINSTALL
                                             ? "OFFLINE APP REINSTALLED"
                                             : "OFFLINE APP INSTALLED")
                                : (error[0] == '\0'
                                   ? "OFFLINE APP COULD NOT BE INSTALLED"
                                   : error));
    return saved;
}

const unsigned char *psp_offline_store_app_icon(
    PspOfflineStore *store, uint32_t id)
{
    if (store == NULL || id == 0) return NULL;
    if (store->app_icon_cache == NULL) {
        store->app_icon_cache = budget_calloc_category(
            store->library.budget, BUDGET_CATEGORY_SESSION,
            OFFLINE_LIBRARY_ITEM_LIMIT, OFFLINE_LIBRARY_APP_ICON_LIMIT);
        if (store->app_icon_cache == NULL) return NULL;
    }
    for (size_t at = 0; at < OFFLINE_LIBRARY_ITEM_LIMIT; at++)
        if (store->app_icon_ids[at] == id)
            return store->app_icon_cache
                + at * OFFLINE_LIBRARY_APP_ICON_LIMIT;
    size_t slot = 0;
    while (slot < OFFLINE_LIBRARY_ITEM_LIMIT
           && store->app_icon_ids[slot] != 0) slot++;
    if (slot == OFFLINE_LIBRARY_ITEM_LIMIT) slot = id % OFFLINE_LIBRARY_ITEM_LIMIT;
    unsigned char *pixels = store->app_icon_cache
        + slot * OFFLINE_LIBRARY_APP_ICON_LIMIT;
    /* A short/torn read may have overwritten part of an evicted cache line.
       Retire its old identity before I/O so it can never be returned as a
       valid but corrupted icon after the failed fill. */
    store->app_icon_ids[slot] = 0;
    if (!offline_library_read_web_app_icon(&store->library, id, pixels))
        return NULL;
    store->app_icon_ids[slot] = id;
    return pixels;
}

bool psp_offline_store_save_current(
    PspOfflineStore *store, BrowserEngine *engine)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    const NavigationEntry *entry = navigation_current(navigation);
    if (store == NULL || navigation == NULL || !navigation->page.loaded
        || entry == NULL || entry->url == NULL
        || offline_internal_url(entry->url)) {
        offline_status(store, "THIS PAGE CANNOT BE SAVED");
        return false;
    }
    uint32_t id = 0;
    char error[160] = {0};
    bool saved = offline_library_save_article(
        &store->library, &navigation->page.document, entry->url,
        &id, error, sizeof(error));
    offline_status(store, saved ? "ARTICLE SAVED FOR LATER" : error);
    return saved;
}

bool psp_offline_store_open_library(
    PspOfflineStore *store, BrowserEngine *engine, bool record_history)
{
    if (store == NULL || engine == NULL
        || !offline_library_load(&store->library)) {
        offline_status(store, "OFFLINE LIBRARY UNAVAILABLE");
        return false;
    }
    char *html = NULL;
    size_t length = 0;
    if (!offline_library_build_page(
            &store->library, browser_engine_budget(engine),
            &html, &length)) {
        offline_status(store, "OFFLINE LIBRARY UNAVAILABLE");
        return false;
    }
    bool opened = offline_commit(
        engine, OFFLINE_ROOT, html, length, record_history);
    offline_status(store, opened ? "OFFLINE LIBRARY" : "LIBRARY PAGE FAILED");
    return opened;
}

static bool offline_open_article(
    PspOfflineStore *store, BrowserEngine *engine, uint32_t id,
    bool record_history)
{
    char *html = NULL;
    size_t length = 0;
    char error[160] = {0};
    if (!offline_library_read_article(
            &store->library, browser_engine_budget(engine), id,
            &html, &length, error, sizeof(error))) {
        offline_status(store, error);
        return false;
    }
    char url[96];
    snprintf(url, sizeof(url), OFFLINE_ROOT "/article?id=%u", (unsigned) id);
    bool opened = offline_commit(
        engine, url, html, length, record_history);
    offline_status(store, opened ? "SAVED ARTICLE" : "ARTICLE PAGE FAILED");
    return opened;
}

static bool offline_open_app(
    PspOfflineStore *store, BrowserEngine *engine, uint32_t id,
    bool record_history)
{
    const OfflineLibraryItem *item = offline_library_find(&store->library, id);
    char *html = NULL;
    size_t length = 0;
    char error[160] = {0};
    if (item == NULL || !offline_library_read_web_app(
            &store->library, browser_engine_budget(engine), store->session,
            id, &html, &length, error, sizeof(error))) {
        offline_status(store, error);
        return false;
    }
    bool opened = offline_commit(
        engine, item->source_url, html, length, record_history);
    NavigationSession *navigation = browser_engine_navigation(engine);
    bool scripts_missing = opened && navigation != NULL
        && navigation->script_discovered != 0
        && navigation->script_loaded == 0;
    offline_status(store,
        !opened ? "OFFLINE APP FAILED"
        : scripts_missing ? "OFFLINE APP NEEDS REINSTALL"
                          : "OFFLINE APP");
    return opened;
}

static bool offline_open_video_page(
    PspOfflineStore *store, BrowserEngine *engine, uint32_t id,
    bool record_history)
{
    const OfflineLibraryItem *item = offline_library_find(&store->library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_YOUTUBE
        || item->state != OFFLINE_ITEM_READY) {
        offline_status(store, "SAVED VIDEO IS NOT READY");
        return false;
    }
    char *html = budget_malloc_category(
        browser_engine_budget(engine), BUDGET_CATEGORY_SESSION, 1024u);
    if (html == NULL) return false;
    int length = snprintf(
        html, 1024u,
        "<!doctype html><meta name=viewport content=\"width=device-width\">"
        "<style>body{font:17px/1.5 sans-serif;padding:18px;background:#111;"
        "color:#eee}</style><h1>Saved video</h1>"
        "<p>The native player is opening the verified local copy.</p>");
    if (length < 0 || length >= 1024) {
        budget_free(browser_engine_budget(engine), html);
        return false;
    }
    char url[96];
    snprintf(url, sizeof(url), OFFLINE_ROOT "/video?id=%u", (unsigned) id);
    bool opened = offline_commit(
        engine, url, html, (size_t) length, record_history);
    offline_status(store, opened ? "OPENING SAVED VIDEO"
                                 : "SAVED VIDEO PAGE FAILED");
    return opened;
}

static PspOfflineRouteResult offline_enqueue_video(
    PspOfflineStore *store, BrowserEngine *engine,
    const char *video_id, const char *title, bool record_history)
{
    char watch_url[128];
    int length = snprintf(
        watch_url, sizeof(watch_url),
        "https://www.youtube.com/watch?v=%s", video_id);
    uint32_t id = 0;
    char error[160] = {0};
    if (length < 0 || (size_t) length >= sizeof(watch_url)
        || !youtube_watch_url_supported(watch_url)
        || !offline_library_enqueue_youtube(
            &store->library, watch_url, title, &id, error, sizeof(error))) {
        offline_status(store, error[0] == 0 ? "VIDEO CANNOT BE SAVED" : error);
        return PSP_OFFLINE_ROUTE_ERROR;
    }
    const OfflineLibraryItem *item = offline_library_find(&store->library, id);
    bool ready = item != NULL && item->state == OFFLINE_ITEM_READY;
    uint32_t active_id = 0;
    bool another_active = offline_download_manager_active(
        &store->download, &active_id) && active_id != id;
    if (!ready && !another_active
        && !offline_download_manager_start(&store->download, id)) {
        offline_status(store, "VIDEO DOWNLOAD COULD NOT START");
        return PSP_OFFLINE_ROUTE_ERROR;
    }
    store->last_active_id = ready ? 0 : id;
    (void) psp_offline_store_open_library(
        store, engine, record_history);
    offline_status(store, ready ? "VIDEO IS ALREADY SAVED"
        : (another_active ? "VIDEO ADDED TO DOWNLOAD QUEUE"
                          : "VIDEO DOWNLOAD STARTED"));
    return PSP_OFFLINE_ROUTE_STATE_CHANGED;
}

PspOfflineRouteResult psp_offline_store_handle_url(
    PspOfflineStore *store, BrowserEngine *engine, const char *url,
    const char *source_title, bool record_history)
{
    size_t root_length = strlen(OFFLINE_ROOT);
    if (store == NULL || engine == NULL || url == NULL
        || strncmp(url, OFFLINE_ROOT, root_length) != 0
        || (url[root_length] != '\0' && url[root_length] != '/'))
        return PSP_OFFLINE_ROUTE_NONE;
    if (!offline_library_load(&store->library)) {
        offline_status(store, "OFFLINE LIBRARY UNAVAILABLE");
        return PSP_OFFLINE_ROUTE_ERROR;
    }
    if (strcmp(url, OFFLINE_ROOT) == 0)
        return psp_offline_store_open_library(
            store, engine, record_history)
            ? PSP_OFFLINE_ROUTE_PAGE : PSP_OFFLINE_ROUTE_ERROR;
    if (strcmp(url, OFFLINE_ROOT "/start-queue") == 0) {
        uint32_t id = 0;
        bool started = offline_download_manager_start_next_queued(
            &store->download, &id);
        store->last_active_id = started ? id : 0;
        (void) psp_offline_store_open_library(
            store, engine, record_history);
        offline_status(store, started ? "DOWNLOAD QUEUE STARTED"
                                      : "NO QUEUED DOWNLOADS");
        return started ? PSP_OFFLINE_ROUTE_STATE_CHANGED
                       : PSP_OFFLINE_ROUTE_PAGE;
    }
    static const char youtube_prefix[] = OFFLINE_ROOT "/youtube?id=";
    if (strncmp(url, youtube_prefix, sizeof(youtube_prefix) - 1u) == 0) {
        const char *video_id = url + sizeof(youtube_prefix) - 1u;
        return offline_enqueue_video(
            store, engine, video_id, source_title, record_history);
    }
    uint32_t id = 0;
    if (offline_route_id(url, "article", &id))
        return offline_open_article(store, engine, id, record_history)
            ? PSP_OFFLINE_ROUTE_PAGE : PSP_OFFLINE_ROUTE_ERROR;
    if (offline_route_id(url, "app", &id))
        return offline_open_app(store, engine, id, record_history)
            ? PSP_OFFLINE_ROUTE_PAGE : PSP_OFFLINE_ROUTE_ERROR;
    if (offline_route_id(url, "video", &id))
        return offline_open_video_page(store, engine, id, record_history)
            ? PSP_OFFLINE_ROUTE_PAGE : PSP_OFFLINE_ROUTE_ERROR;
    if (offline_route_id(url, "delete", &id)) {
        uint32_t active = 0;
        if (offline_download_manager_active(&store->download, &active)
            && active == id)
            (void) offline_download_manager_pause(&store->download, id);
        bool removed = offline_library_remove(&store->library, id);
        (void) psp_offline_store_open_library(
            store, engine, record_history);
        offline_status(store, removed ? "SAVED ITEM DELETED"
                                      : "ITEM COULD NOT BE DELETED");
        return removed ? PSP_OFFLINE_ROUTE_STATE_CHANGED
                       : PSP_OFFLINE_ROUTE_ERROR;
    }
    if (offline_route_id(url, "resume", &id)) {
        uint32_t active = 0;
        bool is_active = offline_download_manager_active(
            &store->download, &active) && active == id;
        bool changed = is_active
            ? offline_download_manager_pause(&store->download, id)
            : offline_download_manager_start(&store->download, id);
        store->last_active_id = is_active ? 0 : id;
        (void) psp_offline_store_open_library(
            store, engine, record_history);
        offline_status(store, changed
            ? (is_active ? "DOWNLOAD PAUSED" : "DOWNLOAD RESUMED")
            : "DOWNLOAD STATE DID NOT CHANGE");
        return changed ? PSP_OFFLINE_ROUTE_STATE_CHANGED
                       : PSP_OFFLINE_ROUTE_ERROR;
    }
    offline_status(store, "OFFLINE LINK IS INVALID");
    return PSP_OFFLINE_ROUTE_ERROR;
}

bool psp_offline_store_resolve_media(
    void *context, const char *url, PspMediaOfflineSource *source)
{
    PspOfflineStore *store = context;
    uint32_t id = 0;
    if (store == NULL || source == NULL
        || !offline_library_load(&store->library)
        || !offline_route_id(url, "video", &id)) return false;
    const OfflineLibraryItem *item = offline_library_find(&store->library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_YOUTUBE
        || item->state != OFFLINE_ITEM_READY
        || !offline_library_item_path(
               &store->library, id, ".video.mp4",
               source->video_path, sizeof(source->video_path))
        || (item->split_streams
            && !offline_library_item_path(
                   &store->library, id, ".audio.mp4",
                   source->audio_path, sizeof(source->audio_path)))) return false;
    source->stream = (YoutubeStream) {
        .itag = item->itag,
        .audio_itag = item->audio_itag,
        .width = item->width,
        .height = item->height,
        .duration_ms = item->duration_ms,
        .content_length = item->content_bytes,
        .audio_content_length = item->audio_bytes,
        .split_streams = item->split_streams,
        .player_status = 200
    };
    snprintf(source->stream.video_id, sizeof(source->stream.video_id), "%s",
             item->video_id);
    snprintf(source->stream.title, sizeof(source->stream.title), "%s",
             item->title);
    snprintf(source->stream.mime_type, sizeof(source->stream.mime_type),
             "video/mp4; codecs=avc1");
    snprintf(source->stream.audio_mime_type,
             sizeof(source->stream.audio_mime_type),
             "audio/mp4; codecs=mp4a");
    snprintf(source->stream.client_name,
             sizeof(source->stream.client_name), "OFFLINE");
    return true;
}

bool psp_offline_store_pump(PspOfflineStore *store)
{
    if (store == NULL) return false;
    uint32_t before = 0;
    bool active_before = offline_download_manager_active(
        &store->download, &before);
    (void) offline_download_manager_pump(&store->download);
    uint32_t after = 0;
    bool active_after = offline_download_manager_active(
        &store->download, &after);
    if (active_before && !active_after) {
        const OfflineLibraryItem *item = offline_library_find(
            &store->library, before);
        offline_status(store,
            item != NULL && item->state == OFFLINE_ITEM_READY
                ? "VIDEO SAVED OFFLINE"
                : (store->download.error[0] != 0
                    ? store->download.error : "DOWNLOAD PAUSED"));
        uint32_t next_id = 0;
        bool started_next = item != NULL
            && item->state == OFFLINE_ITEM_READY
            && offline_download_manager_start_next_queued(
                   &store->download, &next_id);
        store->last_active_id = started_next ? next_id : 0;
        if (started_next)
            offline_status(store, "VIDEO SAVED - NEXT DOWNLOAD STARTED");
        return true;
    }
    return false;
}

void psp_offline_store_destroy(PspOfflineStore *store)
{
    if (store == NULL) return;
    psp_offline_store_discard_app_preparation(store);
    offline_download_manager_destroy(&store->download);
    budget_free(store->library.budget, store->app_icon_cache);
    store->app_icon_cache = NULL;
}

const char *psp_offline_store_status(const PspOfflineStore *store)
{
    return store == NULL || store->status[0] == 0
        ? "OFFLINE LIBRARY" : store->status;
}
