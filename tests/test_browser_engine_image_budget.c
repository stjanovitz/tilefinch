/* What a page's image bounds leave the reader: the attempt count, the byte
   quotas and memory each refuse some images, and a refusal must not keep
   costing the page work or keep the images the reader can see waiting. The
   replays are written here, one small record per image, on a neutral host. */
#include "browser_engine_test_support.h"
#include "../src/tilefinch_test_faults.h"
#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/resources.h"
#include "tilefinch/user_agent.h"

#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMAGE_BUDGET_HOST "https://image-budget.test"

typedef struct {
    char directory[128];
    unsigned records;
} BudgetReplay;

static uint64_t budget_replay_hash(const void *data, size_t length)
{
    const unsigned char *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* One v10 record (typed resources need one), answered at once. */
static bool budget_replay_add_with_delay(BudgetReplay *replay,
    const char *path, const char *content_type, const void *body,
    size_t length, unsigned delay_pumps)
{
    char file[256], url[256];
    snprintf(url, sizeof(url), IMAGE_BUDGET_HOST "%s", path);
    snprintf(file, sizeof(file), "%s/%04u.body", replay->directory,
             replay->records);
    FILE *out = fopen(file, "wb");
    bool ok = out != NULL && fwrite(body, 1, length, out) == length;
    if (out != NULL) ok = fclose(out) == 0 && ok;
    snprintf(file, sizeof(file), "%s/%04u.meta", replay->directory,
             replay->records);
    out = ok ? fopen(file, "wb") : NULL;
    ok = out != NULL && fprintf(out,
        "psp-http-trace=10\ncookie-values=redacted\n"
        "method=GET\nurl=%s\nlogical-request-url=%s\nsuccess=1\n"
        "async-delay-pumps=%u\nexternal-cancel=0\n"
        "transport-timeout=0\nredirect-origin-tainted=0\nerror=\n"
        "request-body-length=0\nrequest-body-hash=cbf29ce484222325\n"
        "request-content-type=\n"
        "request-cookie-bytes=0\nrequest-has-cf-clearance=0\n"
        "request-extra-header-bytes=0\nrequest-extra-header-shape=\n"
        "request-allow-http-errors=0\nrequest-enforce-cors=0\n"
        "request-redirect-same-origin-only=0\n"
        "request-cors-cached-response-validated=0\n"
        "request-if-none-match=\nrequest-if-modified-since=\n"
        "request-referer=\nrequest-origin=\nrequest-accept=\n"
        "request-sec-fetch-dest=\nrequest-sec-fetch-mode=\n"
        "request-sec-fetch-site=\n"
        "request-send-client-hints=0\nrequest-client-hint-tokens=\n"
        "request-client-hint-origin=\nrequest-send-low-client-hints=1\n"
        "request-sec-fetch-user=0\nrequest-upgrade-insecure=0\n"
        "request-user-agent=" TILEFINCH_BROWSER_USER_AGENT "\n"
        "request-diagnostic-mobile-safari=0\nrequest-credentials=0\n"
        "request-credential-origin=\nrequest-initiator-url=\n"
        "request-referrer-source=\nrequest-referrer-policy=\n"
        "status=200\nlength=%zu\nresponse-body-hash=%016llx\n"
        "effective-url=%s\ncontent-type=%s\netag=\n"
        "last-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
        "server=fixture\ncf-ray=\n"
        "response-referrer-policy-metadata-valid=1\n"
        "response-referrer-policy-present=0\nresponse-referrer-policy=\n"
        "response-security-headers-truncated=0\n"
        "response-header-count=1\nset-cookie-count=0\n"
        "response-header-0=content-type: %s\n",
        url, url, delay_pumps, length,
        (unsigned long long) budget_replay_hash(body, length), url,
        content_type, content_type) > 0;
    if (out != NULL) ok = fclose(out) == 0 && ok;
    if (ok) replay->records++;
    return ok;
}

static bool budget_replay_add(BudgetReplay *replay, const char *path,
                              const char *content_type, const void *body,
                              size_t length)
{
    return budget_replay_add_with_delay(replay, path, content_type, body,
                                        length, 0);
}

/* The document's record, in the older form a document still replays from. */
static bool budget_replay_add_page(BudgetReplay *replay, const char *path,
                                   const char *html, size_t length)
{
    char file[256], url[256];
    snprintf(url, sizeof(url), IMAGE_BUDGET_HOST "%s", path);
    snprintf(file, sizeof(file), "%s/%04u.body", replay->directory,
             replay->records);
    FILE *out = fopen(file, "wb");
    bool ok = out != NULL && fwrite(html, 1, length, out) == length;
    if (out != NULL) ok = fclose(out) == 0 && ok;
    snprintf(file, sizeof(file), "%s/%04u.meta", replay->directory,
             replay->records);
    out = ok ? fopen(file, "wb") : NULL;
    ok = out != NULL && fprintf(out,
        "psp-http-trace=1\nmethod=GET\nurl=%s\nsuccess=1\n"
        "async-delay-pumps=0\nexternal-cancel=0\ntransport-timeout=0\n"
        "error=\nstatus=200\nlength=%zu\neffective-url=%s\n"
        "content-type=text/html; charset=utf-8\netag=\nlast-modified=\n"
        "cf-mitigated=\naccept-ch=\ncritical-ch=\nserver=fixture\n"
        "cf-ray=\nset-cookie-count=0\n", url, length, url) > 0;
    if (out != NULL) ok = fclose(out) == 0 && ok;
    if (ok) replay->records++;
    return ok;
}

static bool budget_replay_open(BudgetReplay *replay)
{
    snprintf(replay->directory, sizeof(replay->directory),
             "/tmp/tilefinch-image-budget-XXXXXX");
    replay->records = 0;
    return mkdtemp(replay->directory) != NULL;
}

static bool budget_replay_begin(BudgetReplay *replay)
{
    char file[256], error[256] = {0};
    snprintf(file, sizeof(file), "%s/trace.meta", replay->directory);
    FILE *out = fopen(file, "wb");
    bool ok = out != NULL && fprintf(out,
        "psp-http-trace-clock=1\norigin-ms=1000\ncapture-complete=yes\n"
        "record-count=%u\n", replay->records) > 0;
    if (out != NULL) ok = fclose(out) == 0 && ok;
    return ok && fetch_trace_replay_begin(replay->directory, error,
                                          sizeof(error));
}

static void budget_replay_remove(const BudgetReplay *replay)
{
    char file[256];
    for (unsigned i = 0; i < replay->records; i++) {
        snprintf(file, sizeof(file), "%s/%04u.body", replay->directory, i);
        (void) unlink(file);
        snprintf(file, sizeof(file), "%s/%04u.meta", replay->directory, i);
        (void) unlink(file);
    }
    snprintf(file, sizeof(file), "%s/trace.meta", replay->directory);
    (void) unlink(file);
    (void) rmdir(replay->directory);
}

static const char budget_icon_svg[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2\" height=\"2\">"
    "<rect width=\"2\" height=\"2\" fill=\"teal\"/></svg>";

static BrowserEngine *budget_engine(size_t maximum_images)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = true;
    config.resources.web_fonts_enabled = false;
    config.resources.maximum_images = maximum_images;
    char error[256] = {0};
    return browser_engine_create(&config, error, sizeof(error));
}

/* One PSP loop frame: the runtime, the page's idle work, then one unit of
   the image queue in its own slot (src/psp_script_main.c). */
static bool budget_frame(BrowserEngine *engine)
{
    bool changed = false;
    if (!browser_engine_advance_runtime(engine, 33, 8, NULL)) return false;
    (void) browser_engine_run_idle_work(engine, &changed);
    (void) browser_engine_run_deferred_image_work(engine, &changed);
    return true;
}

static size_t budget_spent(const NavigationSession *nav)
{
    const ExternalImageStats *stats = &nav->page.images.stats;
    size_t exempt = stats->count_exempt <= stats->attempted
        ? stats->count_exempt : stats->attempted;
    return stats->attempted - exempt;
}

/* Forty eager images below the fold spend the page's twelve attempts. The
   commit's queue then started a refused batch for each of the other
   twenty-eight (about ten frames each, with its retry), and while it did,
   no look at the reader's position ran: an image a timer then put on screen
   waited for all of them. Once the count is spent the queue gives way to
   the reader's position, and the image on screen is requested at once. */
int test_spent_image_count_yields_to_the_reader(void)
{
    enum { IMAGES = 40, COUNT = 12, FRAMES = 24 };
    BudgetReplay replay;
    CHECK(budget_replay_open(&replay));
    static char html[8192];
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><title>Image count</title><style>body{margin:0}"
        "img{display:block;width:8px;height:8px}</style><body>"
        "<div id=top style='height:20px'></div>"
        "<div style='height:4000px'>Text</div>");
    for (unsigned i = 0; i < IMAGES; i++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
                                  "<img src=/i/%u.svg>", i);
    used += (size_t) snprintf(html + used, sizeof(html) - used,
        "<script>setInterval(function(){},1000)</script></body>");
    CHECK(used < sizeof(html));
    CHECK(budget_replay_add_page(&replay, "/page", html, used));
    for (unsigned i = 0; i < IMAGES; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/i/%u.svg", i);
        CHECK(budget_replay_add(&replay, path, "image/svg+xml",
                                budget_icon_svg, sizeof(budget_icon_svg) - 1u));
    }
    CHECK(budget_replay_add(&replay, "/late.svg", "image/svg+xml",
                            budget_icon_svg, sizeof(budget_icon_svg) - 1u));
    CHECK(budget_replay_begin(&replay));
    BrowserEngine *engine = budget_engine(COUNT);
    CHECK(engine != NULL);
    CHECK(browser_engine_load_url(engine, IMAGE_BUDGET_HOST "/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    CHECK(nav->page.loaded && nav->page.runtime != NULL
        && nav->page.deferred_image_targets != NULL
        && !nav->page.deferred_images_from_viewport);
    /* The queue spends the count, and the batch that spent it ends. */
    unsigned frames = 0;
    while (frames++ < 400u
           && (budget_spent(nav) < COUNT || nav->page.deferred_image_job != NULL))
        CHECK(budget_frame(engine));
    CHECK(budget_spent(nav) >= COUNT && nav->page.deferred_image_job == NULL);
    size_t batches = nav->performance.background_image_batches;
    size_t refused = nav->page.images.stats.skipped_limit;
    CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
        "setTimeout(function(){var m=document.createElement('img');"
        "m.id='late';m.src='/late.svg';"
        "document.getElementById('top').append(m)},0)",
        "<image-budget>", &nav->page.script_result));
    lxb_dom_node_t *late = NULL;
    unsigned waited = 0;
    for (; waited < 400u; waited++) {
        CHECK(budget_frame(engine));
        if (late == NULL)
            late = test_reader_find_id(
                lxb_dom_interface_node(nav->page.document.html), "late");
        if (late != NULL && image_resource_available(
                images_find_node(&nav->page.images, late))) break;
    }
    size_t late_batches = nav->performance.background_image_batches - batches;
    size_t late_refused = nav->page.images.stats.skipped_limit - refused;
    fprintf(stderr, "spent count: late image after %u frames, %zu batches, "
            "%zu refusals, queue %zu/%zu from-view=%d\n", waited, late_batches,
            late_refused, nav->page.deferred_image_cursor,
            nav->page.deferred_image_count,
            nav->page.deferred_images_from_viewport);
    CHECK(late != NULL && image_resource_available(
        images_find_node(&nav->page.images, late)));
    CHECK(waited < FRAMES);
    /* No batch ran for an off-screen image: only the late image's, and
       none starts later while the reader stays. */
    CHECK(late_batches <= 4u);
    size_t attempted = nav->page.images.stats.attempted;
    batches = nav->performance.background_image_batches;
    for (unsigned frame = 0; frame < 120u; frame++) CHECK(budget_frame(engine));
    CHECK(nav->page.images.stats.attempted == attempted
        && nav->performance.background_image_batches == batches);
    CHECK(browser_engine_render_frame(engine, NULL));
    fetch_trace_end();
    budget_replay_remove(&replay);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* Requests made for `path` (of the last 16 the replay served). */
static unsigned budget_requests_for(const char *path, unsigned since)
{
    char url[256];
    snprintf(url, sizeof(url), IMAGE_BUDGET_HOST "%s", path);
    const TilefinchTestFaults *faults = tilefinch_test_faults();
    unsigned count = 0;
    for (unsigned at = since; at < faults->replayed_request_count; at++)
        count += strcmp(faults->replayed_requests[at % 16u].url, url) == 0;
    return count;
}

/* The queue's image is on the network when a relayout gives its batch up
   (here a script removes another image, so the image table is rebuilt).
   The request was cancelled ("image pipeline aborted") and sent again: on a
   page that relayouts while a slow photo downloads, every relayout started
   it over, and each start spent an attempt. The given-up request runs on
   and the batch that takes the photo up again adopts it. The replay holds
   one response for the photo: a second request for it gets none. */
int test_given_up_image_fetch_is_adopted(void)
{
    BudgetReplay replay;
    CHECK(budget_replay_open(&replay));
    static const char html[] =
        "<!doctype html><title>Given up</title><style>body{margin:0}"
        "img{display:block;width:8px;height:8px}</style><body>"
        "<img id=gone src=/gone.svg><img id=photo src=/photo.svg>"
        "<div style='height:4000px'>Text</div>"
        "<script>setInterval(function(){},1000)</script></body>";
    CHECK(budget_replay_add_page(&replay, "/page", html, sizeof(html) - 1u));
    CHECK(budget_replay_add(&replay, "/gone.svg", "image/svg+xml",
                            budget_icon_svg, sizeof(budget_icon_svg) - 1u));
    CHECK(budget_replay_add_with_delay(&replay, "/photo.svg",
        "image/svg+xml", budget_icon_svg, sizeof(budget_icon_svg) - 1u, 40));
    CHECK(budget_replay_begin(&replay));
    BrowserEngine *engine = budget_engine(12);
    CHECK(engine != NULL);
    unsigned since = tilefinch_test_faults()->replayed_request_count;
    CHECK(browser_engine_load_url(engine, IMAGE_BUDGET_HOST "/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    CHECK(nav->page.loaded && nav->page.runtime != NULL
        && nav->page.deferred_image_targets != NULL);
    lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
    lxb_dom_node_t *gone = test_reader_find_id(root, "gone");
    lxb_dom_node_t *photo = test_reader_find_id(root, "photo");
    CHECK(gone != NULL && photo != NULL);
    /* The photo's request is out; the other image is in. */
    unsigned frames = 0;
    while (frames++ < 200u
           && !(image_resource_available(images_find_node(&nav->page.images,
                                                          gone))
                && nav->page.deferred_image_job != NULL
                && budget_requests_for("/photo.svg", since) == 1u))
        CHECK(budget_frame(engine));
    CHECK(nav->page.deferred_image_job != NULL
        && !image_resource_available(images_find_node(&nav->page.images,
                                                      photo)));
    CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
        "document.getElementById('gone').remove()", "<image-given-up>",
        &nav->page.script_result));
    CHECK(navigation_relayout(nav));
    CHECK(nav->page.deferred_image_job == NULL);
    for (frames = 0; frames < 200u; frames++) {
        CHECK(budget_frame(engine));
        if (image_resource_available(images_find_node(&nav->page.images,
                                                      photo))) break;
    }
    fprintf(stderr, "given up: photo after %u frames, %u requests, "
            "attempted=%zu failed=%zu\n", frames,
            budget_requests_for("/photo.svg", since),
            nav->page.images.stats.attempted, nav->page.images.stats.failed);
    CHECK(image_resource_available(images_find_node(&nav->page.images, photo)));
    CHECK(budget_requests_for("/photo.svg", since) == 1u
        && nav->page.images.stats.failed == 0);
    CHECK(browser_engine_render_frame(engine, NULL));
    fetch_trace_end();
    budget_replay_remove(&replay);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* A response parked in the page realm's scheduler (as a paused image
   preload parks its request) that finishes while nothing else is in
   flight: the realm's loop pumps its scheduler only while a request is,
   so the response, its slot and its body stayed until the realm fetched
   again. It is released within the park bound, and memory pressure before
   a script is refused releases one at once. */
int test_finished_parked_response_is_released(void)
{
    BudgetReplay replay;
    CHECK(budget_replay_open(&replay));
    static const char html[] =
        "<!doctype html><title>Parked</title><body><p>Text</p>"
        "<script>setInterval(function(){},1000)</script></body>";
    CHECK(budget_replay_add_page(&replay, "/page", html, sizeof(html) - 1u));
    CHECK(budget_replay_add_with_delay(&replay, "/held.svg", "image/svg+xml",
        budget_icon_svg, sizeof(budget_icon_svg) - 1u, 2));
    CHECK(budget_replay_add_with_delay(&replay, "/kept.svg", "image/svg+xml",
        budget_icon_svg, sizeof(budget_icon_svg) - 1u, 2));
    CHECK(budget_replay_begin(&replay));
    BrowserEngine *engine = budget_engine(12);
    CHECK(engine != NULL);
    CHECK(browser_engine_load_url(engine, IMAGE_BUDGET_HOST "/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    CHECK(nav->page.loaded && nav->page.runtime != NULL);
    FetchScheduler *realm = script_runtime_fetch_scheduler(nav->page.runtime);
    CHECK(realm != NULL && fetch_scheduler_pending(realm) == 0);
    const FetchRequest request = {
        .method = "GET", .send_low_client_hints = true
    };
    uint64_t held = fetch_scheduler_enqueue(
        realm, IMAGE_BUDGET_HOST "/held.svg", &request, 64u * 1024u, 5000);
    CHECK(held != 0 && fetch_scheduler_park(realm, held, 1u));
    unsigned frames = 0;
    while (frames++ < 20u && fetch_scheduler_pending(realm) != 0)
        CHECK(budget_frame(engine));
    CHECK(fetch_scheduler_request_complete(realm, held)
        && fetch_scheduler_parked_bytes(realm) != 0);
    for (frames = 0; frames < 200u
         && fetch_scheduler_request_complete(realm, held); frames++)
        CHECK(budget_frame(engine));
    fprintf(stderr, "finished parked response released after %u frames\n",
            frames);
    CHECK(!fetch_scheduler_request_complete(realm, held)
        && frames <= FETCH_SCHEDULER_PARK_PUMPS + 1u);
    uint64_t kept = fetch_scheduler_enqueue(
        realm, IMAGE_BUDGET_HOST "/kept.svg", &request, 64u * 1024u, 5000);
    CHECK(kept != 0 && fetch_scheduler_park(realm, kept, 2u));
    for (frames = 0; frames < 20u && fetch_scheduler_pending(realm) != 0;
         frames++)
        CHECK(budget_frame(engine));
    CHECK(fetch_scheduler_request_complete(realm, kept));
    (void) script_runtime_reclaim_for_admission(nav->page.runtime,
                                                32u * MIB, false);
    CHECK(!fetch_scheduler_request_complete(realm, kept)
        && fetch_scheduler_parked_bytes(realm) == 0);
    fetch_trace_end();
    budget_replay_remove(&replay);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* A given-up request whose image the page then removed has no taker: it is
   cancelled within a bound of pumps and never requested again, and it
   leaves no entry and no attempt behind. */
int test_removed_image_fetch_is_not_requested_again(void)
{
    BudgetReplay replay;
    CHECK(budget_replay_open(&replay));
    static const char html[] =
        "<!doctype html><title>Removed</title><style>body{margin:0}"
        "img{display:block;width:8px;height:8px}</style><body>"
        "<img id=photo src=/photo.svg>"
        "<div style='height:4000px'>Text</div>"
        "<script>setInterval(function(){},1000)</script></body>";
    CHECK(budget_replay_add_page(&replay, "/page", html, sizeof(html) - 1u));
    for (unsigned i = 0; i < 2u; i++)
        CHECK(budget_replay_add_with_delay(&replay, "/photo.svg",
            "image/svg+xml", budget_icon_svg, sizeof(budget_icon_svg) - 1u,
            400));
    CHECK(budget_replay_begin(&replay));
    BrowserEngine *engine = budget_engine(12);
    CHECK(engine != NULL);
    unsigned since = tilefinch_test_faults()->replayed_request_count;
    CHECK(browser_engine_load_url(engine, IMAGE_BUDGET_HOST "/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    CHECK(nav->page.loaded && nav->page.runtime != NULL);
    lxb_dom_node_t *photo = test_reader_find_id(
        lxb_dom_interface_node(nav->page.document.html), "photo");
    CHECK(photo != NULL);
    unsigned frames = 0;
    while (frames++ < 200u && budget_requests_for("/photo.svg", since) == 0)
        CHECK(budget_frame(engine));
    CHECK(nav->page.deferred_image_job != NULL
        && budget_requests_for("/photo.svg", since) == 1u);
    size_t attempted = nav->page.images.stats.attempted;
    CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
        "document.getElementById('photo').remove()", "<image-removed>",
        &nav->page.script_result));
    CHECK(navigation_relayout(nav));
    for (frames = 0; frames < 300u; frames++) CHECK(budget_frame(engine));
    fprintf(stderr, "removed: %u requests, attempted %zu -> %zu, "
            "in flight %zu\n", budget_requests_for("/photo.svg", since),
            attempted, nav->page.images.stats.attempted,
            fetch_scheduler_pending(nav->page.resource_scheduler));
    CHECK(budget_requests_for("/photo.svg", since) == 1u
        && nav->page.deferred_image_job == NULL
        && fetch_scheduler_pending(nav->page.resource_scheduler) == 0
        && nav->page.images.count == 0
        && nav->page.images.stats.attempted < attempted);
    CHECK(browser_engine_render_frame(engine, NULL));
    fetch_trace_end();
    budget_replay_remove(&replay);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static bool budget_read_fixture(const char *name, unsigned char *data,
                                size_t capacity, size_t *length)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/tests/fixtures/image-budget/%s",
             TILEFINCH_TEST_SOURCE_DIR, name);
    FILE *in = fopen(path, "rb");
    if (in == NULL) return false;
    *length = fread(data, 1, capacity, in);
    bool ok = *length != 0 && *length < capacity && ferror(in) == 0;
    return fclose(in) == 0 && ok;
}

/* A photograph whose reduced decode the Budget cannot hold (here a test
   fault refuses the worker's arena, as a 32 MiB page near its limit refuses
   a large cover's) leaves no entry and no failure. Each look at the
   reader's position, and the queue's own retry, fetched and refused it
   again. It is fetched once, and waits until memory frees up. */
int test_memory_refused_raster_is_fetched_once(void)
{
    enum { LOOKS = 6 };
    /* Drawn at 24x24, the 1024x768 photo is a reduced decode whose working
       set (its source and target pixels) is about 3 MB. */
    static unsigned char photo_jpeg[16384];
    size_t photo_length = 0;
    CHECK(budget_read_fixture("photo-1024x768.jpg", photo_jpeg,
                              sizeof(photo_jpeg), &photo_length));
    BudgetReplay replay;
    CHECK(budget_replay_open(&replay));
    static const char html[] =
        "<!doctype html><title>Refused photo</title><style>body{margin:0}"
        "#photo{display:block;width:24px;height:24px}</style><body>"
        "<img id=photo src=/photo.jpg><div style='height:4000px'>Text</div>"
        "<script>setInterval(function(){},1000)</script></body>";
    CHECK(budget_replay_add_page(&replay, "/page", html, sizeof(html) - 1u));
    for (unsigned i = 0; i < 4u; i++)
        CHECK(budget_replay_add(&replay, "/photo.jpg", "image/jpeg",
                                photo_jpeg, photo_length));
    CHECK(budget_replay_begin(&replay));
    BrowserEngine *engine = budget_engine(12);
    CHECK(engine != NULL);
    tilefinch_test_faults()->refuse_image_decode_arena = true;
    CHECK(browser_engine_load_url(engine, IMAGE_BUDGET_HOST "/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    CHECK(nav->page.loaded && nav->page.runtime != NULL);
    lxb_dom_node_t *photo = test_reader_find_id(
        lxb_dom_interface_node(nav->page.document.html), "photo");
    CHECK(photo != NULL);
    for (unsigned frame = 0; frame < 60u; frame++) CHECK(budget_frame(engine));
    size_t first = nav->page.images.stats.attempted;
    /* The reader moves within the photo's band and back, a look each. */
    for (unsigned look = 0; look < LOOKS; look++) {
        CHECK(navigation_set_scroll(nav, look % 2u == 0 ? 300 : 0));
        for (unsigned frame = 0; frame < 30u; frame++)
            CHECK(budget_frame(engine));
    }
    tilefinch_test_faults()->refuse_image_decode_arena = false;
    const ImageResources *images = &nav->page.images;
    fprintf(stderr, "memory refusal: attempted=%zu after load, %zu after %u "
            "looks, skipped=%zu looks=%zu\n", first, images->stats.attempted,
            LOOKS, images->stats.skipped_limit,
            nav->performance.viewport_image_looks);
    CHECK(first == 1u && images->stats.attempted == 1u);
    CHECK(images->stats.skipped_limit != 0
        && !image_resource_available(images_find_node(images, photo)));
    /* Unchanged pressure is not a retry. A modest release, much smaller
       than the old source-size estimate, now permits a successful retry. */
    CHECK(images_node_memory_refused(images, photo,
                                     budget_remaining(nav->budget))
        && !images_node_memory_refused(images, photo, SIZE_MAX));
    nav->budget->limit += 512u * 1024u;
    CHECK(!images_node_memory_refused(images, photo,
                                     budget_remaining(nav->budget)));
    CHECK(navigation_set_scroll(nav, 300));
    CHECK(budget_frame(engine));
    CHECK(navigation_set_scroll(nav, 0));
    for (unsigned frame = 0; frame < 60u; frame++) {
        CHECK(budget_frame(engine));
        if (image_resource_available(images_find_node(images, photo))) break;
    }
    CHECK(images->stats.attempted == 2u
        && image_resource_available(images_find_node(images, photo)));
    CHECK(browser_engine_render_frame(engine, NULL));
    fetch_trace_end();
    budget_replay_remove(&replay);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* An icon drawn into its bottom-right quarter, with only a viewBox (so no
   natural size of its own): a 1em box shows the whole icon, fitted. */
static int budget_icon_markup(char *out, size_t capacity, unsigned variant)
{
    return snprintf(out, capacity,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 512 512\">"
        "<rect x=\"256\" y=\"256\" width=\"256\" height=\"%u\"/></svg>",
        256u - variant);
}

typedef enum {
    SVG_RETRY_BUDGET,
    SVG_RETRY_QUOTA,
    SVG_RETRY_TARGET
} SvgRetryChange;

static bool budget_svg_load(PocDocument *document, Stylesheet *stylesheet,
                             ImageResources *images, Budget *budget,
                             size_t decoded_quota)
{
    return images_load_external(document, stylesheet, images, budget,
        IMAGE_BUDGET_HOST "/page", IMAGE_BUDGET_HOST "/page", NULL,
        8, 64u * 1024u, 16u * 1024u, decoded_quota, 1000, NULL, NULL);
}

/* The first and only SVG can fail with zero decoded bytes. A change in
   its limiting resource (Budget, decoded quota, or requested size) must
   let the unchanged URL load; an unchanged look must not spend an attempt. */
static int budget_svg_retry_case(SvgRetryChange change)
{
    static const char html[] =
        "<!doctype html><body><i id=icon style='display:block;width:128px;"
        "height:128px;background:#000;mask-image:url(/retry.svg)'></i>";
    static const char intrinsic_svg[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='512' height='512'>"
        "<rect width='512' height='512'/></svg>";
    static const char budget_svg[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='1024' height='1024'>"
        "<rect width='1024' height='1024'/></svg>";
    char viewbox_svg[256];
    (void) budget_icon_markup(viewbox_svg, sizeof(viewbox_svg), 0);
    const char *svg = change == SVG_RETRY_TARGET ? viewbox_svg
        : change == SVG_RETRY_BUDGET ? budget_svg : intrinsic_svg;
    BudgetReplay replay;
    CHECK(budget_replay_open(&replay));
    for (unsigned i = 0; i < 2; i++)
        CHECK(budget_replay_add(&replay, "/retry.svg", "image/svg+xml",
                                svg, strlen(svg)));
    CHECK(budget_replay_begin(&replay));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    CHECK(stylesheet_build(&stylesheet, &budget, &document, 480));
    lxb_dom_node_t *icon = test_reader_find_id(
        lxb_dom_interface_node(document.html), "icon");
    CHECK(icon != NULL);
    size_t quota = change == SVG_RETRY_TARGET ? 4096u : MIB / 2u;
    if (change == SVG_RETRY_BUDGET) {
        quota = 6u * MIB;
        budget.limit = budget.current + 3u * MIB;
    }
    CHECK(budget_svg_load(&document, &stylesheet, &images, &budget, quota));
    CHECK(images.stats.attempted == 1u && images.stats.decoded_bytes == 0
        && images.stats.skipped_limit != 0u && images.stats.failed == 0
        && images_find_mask_node(&images, icon) == NULL);
    CHECK(budget_svg_load(&document, &stylesheet, &images, &budget, quota));
    CHECK(images.stats.attempted == 1u && images.stats.decoded_bytes == 0);
    if (change == SVG_RETRY_BUDGET) {
        budget.limit = 8u * MIB;
    } else if (change == SVG_RETRY_QUOTA) {
        quota = MIB;
    } else {
        static const char small[] =
            "display:block;width:16px;height:16px;"
            "background:#000;mask-image:url(/retry.svg)";
        CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(icon),
            (const lxb_char_t *) "style", 5, (const lxb_char_t *) small,
            sizeof(small) - 1u) != NULL);
        stylesheet_destroy(&stylesheet);
        CHECK(stylesheet_build(&stylesheet, &budget, &document, 480));
    }
    CHECK(budget_svg_load(&document, &stylesheet, &images, &budget, quota));
    CHECK(images.stats.attempted == 2u
        && image_resource_available(images_find_mask_node(&images, icon)));
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    fetch_trace_end();
    budget_replay_remove(&replay);
    return 0;
}

/* Delay the shared response so both roles are pending when it is decoded.
   A large box makes the first group unaffordable, whichever DOM order
   discovered the two masks. The small box must load on the next look
   without being grouped again with the remembered oversized use. */
static int budget_svg_mixed_target_case(bool small_first)
{
    static const char css[] =
        "<!doctype html><style>i{display:block;background:#000;"
        "mask-image:url(/shared.svg)}"
        "#big{width:128px;height:128px}"
        "#small{width:16px;height:16px}</style><body>";
    char html[512], svg[256];
    int html_length = snprintf(html, sizeof(html), "%s%s", css,
        small_first ? "<i id=small></i><i id=big></i>"
                    : "<i id=big></i><i id=small></i>");
    int svg_length = budget_icon_markup(svg, sizeof(svg), 0);
    CHECK(html_length > 0 && (size_t) html_length < sizeof(html)
        && svg_length > 0 && (size_t) svg_length < sizeof(svg));
    BudgetReplay replay;
    CHECK(budget_replay_open(&replay));
    for (unsigned i = 0; i < 2; i++)
        CHECK(budget_replay_add_with_delay(&replay, "/shared.svg",
            "image/svg+xml", svg, (size_t) svg_length, 5));
    CHECK(budget_replay_begin(&replay));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    CHECK(document_parse(&document, &budget, html, (size_t) html_length, 17));
    CHECK(stylesheet_build(&stylesheet, &budget, &document, 480));
    lxb_dom_node_t *small = test_reader_find_id(
        lxb_dom_interface_node(document.html), "small");
    CHECK(small != NULL);
    CHECK(budget_svg_load(&document, &stylesheet, &images, &budget, 4096u));
    CHECK(images.stats.attempted == 1u && images.stats.decoded_bytes == 0u
        && images.stats.skipped_limit != 0u && images.stats.failed == 0);
    CHECK(budget_svg_load(&document, &stylesheet, &images, &budget, 4096u));
    CHECK(images.stats.attempted == 2u
        && image_resource_available(images_find_mask_node(&images, small)));
    CHECK(budget_svg_load(&document, &stylesheet, &images, &budget, 4096u));
    CHECK(images.stats.attempted == 2u);
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    fetch_trace_end();
    budget_replay_remove(&replay);
    return 0;
}

int test_svg_limit_recovers_when_its_constraint_changes(void)
{
    return budget_svg_retry_case(SVG_RETRY_BUDGET)
        || budget_svg_retry_case(SVG_RETRY_QUOTA)
        || budget_svg_retry_case(SVG_RETRY_TARGET)
        || budget_svg_mixed_target_case(true)
        || budget_svg_mixed_target_case(false);
}

static bool budget_pixel_dark(BrowserEngine *engine, int x, int y)
{
    size_t pixels = 0;
    uint16_t *frame = browser_engine_framebuffer(engine, &pixels);
    return frame != NULL && (size_t) y * 480u + (size_t) x < pixels
        && frame[(size_t) y * 480u + (size_t) x] == 0;
}

/* Thirty elements masked by one icon and four more icons, each a 512-unit
   viewBox with no natural size, masking a 16 px box. Rasterized at the
   viewBox size, each took 1 MiB of the page's 4 MiB decoded quota, so the
   fifth was refused, and each painted at that size, the box showing only
   its empty top-left corner. Contained in the box, as CSS sizes an image
   with only a ratio, and rasterized at that size, all five fit in a few
   kilobytes, are fetched once each and show whole. */
int test_painted_size_svg_masks_fit_the_decoded_quota(void)
{
    enum { REPEATS = 30, ICONS = 5 };
    BudgetReplay replay;
    CHECK(budget_replay_open(&replay));
    static char html[8192];
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Icons</title><style>body{margin:0;font-size:0;"
        "background:#fff}i{display:inline-block;width:16px;height:16px;"
        "vertical-align:top;background:#000;mask-repeat:no-repeat}");
    for (unsigned icon = 0; icon < ICONS; icon++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
            ".i%u{mask-image:url(/icon/%u.svg)}", icon, icon);
    used += (size_t) snprintf(html + used, sizeof(html) - used,
                              "</style><body><div>");
    for (unsigned i = 0; i < REPEATS; i++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
                                  "<i class=i0></i>");
    used += (size_t) snprintf(html + used, sizeof(html) - used, "</div><div>");
    for (unsigned icon = 1; icon < ICONS; icon++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
                                  "<i class=i%u></i>", icon);
    used += (size_t) snprintf(html + used, sizeof(html) - used,
        "</div><div style='height:2000px'>Text</div>"
        "<script>setInterval(function(){},1000)</script></body>");
    CHECK(used < sizeof(html));
    CHECK(budget_replay_add_page(&replay, "/page", html, used));
    for (unsigned copy = 0; copy < 4u; copy++) {
        for (unsigned icon = 0; icon < ICONS; icon++) {
            char path[32], svg[256];
            snprintf(path, sizeof(path), "/icon/%u.svg", icon);
            int length = budget_icon_markup(svg, sizeof(svg), icon);
            CHECK(budget_replay_add(&replay, path, "image/svg+xml", svg,
                                    (size_t) length));
        }
    }
    CHECK(budget_replay_begin(&replay));
    BrowserEngine *engine = budget_engine(24);
    CHECK(engine != NULL);
    CHECK(browser_engine_load_url(engine, IMAGE_BUDGET_HOST "/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    CHECK(nav->page.loaded);
    for (unsigned frame = 0; frame < 90u; frame++) {
        CHECK(budget_frame(engine));
        if (frame % 30u == 29u) CHECK(browser_engine_render_frame(engine, NULL));
    }
    CHECK(browser_engine_render_frame(engine, NULL));
    const ImageResources *images = &nav->page.images;
    size_t held = 0;
    for (size_t i = 0; i < images->count; i++)
        if (images->items[i].is_mask
            && image_resource_available(&images->items[i])) held++;
    fprintf(stderr, "painted-size masks: held=%zu attempted=%zu "
            "decoded=%zu skipped=%zu\n", held, images->stats.attempted,
            images->stats.decoded_bytes, images->stats.skipped_limit);
    /* Every icon paints its bottom-right quarter, fitted to its box. */
    bool painted = true;
    for (unsigned i = 0; i < REPEATS + ICONS - 1u; i++) {
        int x = (int) (i % REPEATS) * 16, y = (int) (i / REPEATS) * 16;
        if (!budget_pixel_dark(engine, x + 12, y + 12)
            || budget_pixel_dark(engine, x + 3, y + 3)) {
            fprintf(stderr, "painted-size masks: icon %u not drawn\n", i);
            painted = false;
        }
    }
    CHECK(held == REPEATS + ICONS - 1u);
    CHECK(images->stats.attempted <= ICONS);
    CHECK(images->stats.decoded_bytes < 100u * 1024u);
    CHECK(painted);
    fetch_trace_end();
    budget_replay_remove(&replay);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* Six icons with a natural size, masking 16 px boxes that a script keeps
   changing the page around: CSS paints each at that size, so the first
   (256x256) costs 256 KiB and the others (512x512) 1 MiB each. The 4 MiB
   decoded quota holds four, and the bytes left over (768 KiB) let each
   later look at the page fetch and parse the other two only to refuse
   them, spending the image count each time. A refused URL now waits until
   the page's decoded bytes drop, and then loads once. */
int test_svg_decode_limit_waits_for_decoded_bytes(void)
{
    enum { ICONS = 6, LOOKS = 6 };
    BudgetReplay replay;
    CHECK(budget_replay_open(&replay));
    static char html[4096];
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Large icons</title><style>body{margin:0;font-size:0}"
        "i{display:inline-block;width:16px;height:16px;vertical-align:top;"
        "background:#000;mask-repeat:no-repeat}");
    for (unsigned icon = 0; icon < ICONS; icon++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
            ".i%u{mask-image:url(/large/%u.svg)}", icon, icon);
    used += (size_t) snprintf(html + used, sizeof(html) - used,
                              "</style><body><div id=icons>");
    for (unsigned icon = 0; icon < ICONS; icon++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
                                  "<i class=i%u></i>", icon);
    used += (size_t) snprintf(html + used, sizeof(html) - used,
        "</div><div id=text style='height:2000px'>Text</div>"
        "<script>setInterval(function(){var t=document.getElementById("
        "'text');t.appendChild(document.createElement('p')).textContent="
        "'More'},100)</script></body>");
    CHECK(used < sizeof(html));
    CHECK(budget_replay_add_page(&replay, "/page", html, used));
    for (unsigned copy = 0; copy < 8u; copy++) {
        for (unsigned icon = 0; icon < ICONS; icon++) {
            char path[32], svg[256];
            snprintf(path, sizeof(path), "/large/%u.svg", icon);
            unsigned side = icon == 0 ? 256u : 512u;
            int length = snprintf(svg, sizeof(svg),
                "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%u\" "
                "height=\"%u\"><rect width=\"%u\" height=\"%u\"/></svg>",
                side, side, side - icon, side);
            CHECK(budget_replay_add(&replay, path, "image/svg+xml", svg,
                                    (size_t) length));
        }
    }
    CHECK(budget_replay_begin(&replay));
    BrowserEngine *engine = budget_engine(24);
    CHECK(engine != NULL);
    CHECK(browser_engine_load_url(engine, IMAGE_BUDGET_HOST "/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    CHECK(nav->page.loaded && nav->page.runtime != NULL);
    for (unsigned frame = 0; frame < 60u; frame++) CHECK(budget_frame(engine));
    const ImageResources *images = &nav->page.images;
    size_t first = images->stats.attempted;
    size_t decoded = images->stats.decoded_bytes;
    for (unsigned look = 0; look < LOOKS; look++) {
        CHECK(navigation_set_scroll(nav, look % 2u == 0 ? 200 : 0));
        for (unsigned frame = 0; frame < 30u; frame++)
            CHECK(budget_frame(engine));
        CHECK(browser_engine_render_frame(engine, NULL));
    }
    size_t held = 0;
    for (size_t i = 0; i < images->count; i++)
        if (images->items[i].is_mask
            && image_resource_available(&images->items[i])) held++;
    fprintf(stderr, "decode limit: attempted=%zu after load, %zu after %u "
            "looks, decoded=%zu->%zu held=%zu skipped=%zu\n", first,
            images->stats.attempted, LOOKS, decoded,
            images->stats.decoded_bytes, held, images->stats.skipped_limit);
    CHECK(first == ICONS && held == 4u);
    CHECK(images->stats.attempted == first);
    /* Two held icons (the first two) leave the page; their bytes free the
       quota, and the two refused icons load, each fetched once more. */
    CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
        "var b=document.getElementById('icons');"
        "b.removeChild(b.children[0]);b.removeChild(b.children[0])",
        "<image-budget>", &nav->page.script_result));
    for (unsigned frame = 0; frame < 90u; frame++) {
        CHECK(budget_frame(engine));
        if (frame % 30u == 29u) CHECK(browser_engine_render_frame(engine, NULL));
    }
    lxb_dom_node_t *icons = test_reader_find_id(
        lxb_dom_interface_node(nav->page.document.html), "icons");
    CHECK(icons != NULL);
    held = 0;
    for (lxb_dom_node_t *icon = icons->first_child; icon != NULL;
         icon = icon->next)
        if (image_resource_available(images_find_mask_node(images, icon)))
            held++;
    fprintf(stderr, "decode limit: after removal attempted=%zu held=%zu "
            "decoded=%zu\n", images->stats.attempted, held,
            images->stats.decoded_bytes);
    CHECK(held == ICONS - 2u && images->stats.attempted == first + 2u);
    fetch_trace_end();
    budget_replay_remove(&replay);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}
