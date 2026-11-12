/*
 * Page media on the PSP transport worker (HLS playlists and segments) is
 * issued one hop at a time; each redirect comes back to the browser thread,
 * which decides here whether and where to go next. These tests walk an HLS
 * page whose master, variant and segment all redirect to other origins, and
 * every refusal a redirect target must meet. psp_media_hls.c itself is
 * PSP-only; it calls exactly these functions.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "tilefinch/budget.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/content_security_policy.h"
#include "tilefinch/fetch.h"
#include "tilefinch/media_candidates.h"
#include "tilefinch/media_hls.h"
#include "tilefinch/media_http.h"
#include "tilefinch/media_worker_request.h"
#include "tilefinch/session.h"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "media-worker-request failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

#define DOCUMENT "https://video.test/watch/clip"

typedef struct {
    Budget budget;
    BrowserSession session;
    FetchResult *headers;
    FetchPreparedPageRequest *prepared;
    FetchPreparedPageRequest *scratch;
} Fixture;

static bool fixture_init(Fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    budget_init(&fixture->budget, 4u * 1024u * 1024u);
    if (!browser_session_init(
            &fixture->session, &fixture->budget, 128u * 1024u)) return false;
    fixture->headers = fetch_result_create(&fixture->budget);
    fixture->prepared = budget_malloc(
        &fixture->budget, sizeof(*fixture->prepared));
    fixture->scratch = budget_malloc(
        &fixture->budget, sizeof(*fixture->scratch));
    return fixture->headers != NULL && fixture->prepared != NULL
        && fixture->scratch != NULL;
}

static void fixture_destroy(Fixture *fixture)
{
    budget_free(&fixture->budget, fixture->scratch);
    budget_free(&fixture->budget, fixture->prepared);
    fetch_result_free(fixture->headers);
    browser_session_destroy(&fixture->session);
}

/* A redirect's header snapshot as the worker publishes it. */
static const FetchResult *redirect_response(
    Fixture *fixture, long status, const char *location,
    const char *extra)
{
    FetchResult *headers = fixture->headers;
    headers->status_code = status;
    int written = snprintf(
        headers->response_headers, sizeof(headers->response_headers),
        "location: %s\n%s", location, extra == NULL ? "" : extra);
    headers->response_headers_length =
        written < 0 ? 0 : (size_t) written;
    return headers;
}

static MediaWorkerAuthority page_authority(
    Fixture *fixture, const TilefinchContentSecurityPolicy *policy)
{
    return (MediaWorkerAuthority) {
        .session = &fixture->session,
        .document_url = DOCUMENT,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .content_security_policy = policy,
        .page_redirects = true
    };
}

static FetchRequest worker_request(
    Fixture *fixture, const MediaWorkerAuthority *authority,
    const MediaWorkerHop *hop, bool *ok)
{
    FetchRequest transport = {
        .accept = "application/vnd.apple.mpegurl,video/mp2t",
        .connect_timeout_ms = 3000
    };
    FetchRequest request = {0};
    *ok = media_worker_request_prepare(
        authority, hop, &transport, fixture->prepared, &request);
    return request;
}

static bool cookie_has(const FetchRequest *request, const char *name)
{
    return request->cookie != NULL && strstr(request->cookie, name) != NULL;
}

/* Master, variant and segment each redirect to another origin. Every hop is
   followed, and each hop's request carries only its own target's cookies and
   the page's Referer, never the previous hop's. */
static void test_hls_redirect_chain_crosses_origins(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    CHECK(browser_session_cookie_set(
        &fixture.session, "https://video.test/", "page=1; Secure"));
    CHECK(browser_session_cookie_set(
        &fixture.session, "https://cdn-a.test/",
        "edge=2; Secure; SameSite=None"));
    MediaWorkerAuthority authority = page_authority(&fixture, NULL);
    MediaWorkerHop hop;
    bool ok = false;

    CHECK(media_worker_hop_init(&hop, "https://video.test/hls/master.m3u8"));
    FetchRequest request = worker_request(&fixture, &authority, &hop, &ok);
    CHECK(ok && cookie_has(&request, "page=1")
          && request.redirect_url_validator != NULL
          && request.page_context == NULL
          && request.cookie_session == NULL
          && !request.block_private_network);
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 302,
                                "https://cdn-a.test/live/master.m3u8", NULL),
              fixture.scratch) == MEDIA_WORKER_REDIRECT_FOLLOW);
    CHECK(strcmp(hop.url, "https://cdn-a.test/live/master.m3u8") == 0
          && hop.redirects == 1u);
    request = worker_request(&fixture, &authority, &hop, &ok);
    CHECK(ok && !cookie_has(&request, "page=1")
          && request.referer != NULL
          && strcmp(request.referer, "https://video.test/") == 0
          && strcmp(request.sec_fetch_site, "cross-site") == 0);

    /* A playlist that answered through a redirect resolves its relative
       variant against the URL that answered. */
    MediaHlsPlaylistStream *stream = media_hls_playlist_stream_create(
        &fixture.budget, "https://video.test/hls/master.m3u8", NULL, 0);
    CHECK(stream != NULL
          && media_hls_playlist_stream_rebase(stream, hop.url));
    static const char master[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=426x240,"
        "CODECS=\"avc1.42c01e,mp4a.40.2\"\n"
        "low/index.m3u8\n";
    CHECK(media_hls_playlist_stream_feed(
        stream, (const unsigned char *) master, sizeof(master) - 1u,
        NULL, 0));
    CHECK(!media_hls_playlist_stream_rebase(
        stream, "https://elsewhere.test/master.m3u8"));
    MediaHlsPlaylist *playlist = media_hls_playlist_stream_finish(
        stream, NULL, 0);
    char variant[TILEFINCH_URL_SERIALIZED_LIMIT] = {0};
    CHECK(playlist != NULL
          && media_hls_playlist_select_variant(
                 playlist, 432u, 240u, 240u, variant, sizeof(variant))
          && strcmp(variant, "https://cdn-a.test/live/low/index.m3u8") == 0);
    media_hls_playlist_destroy(playlist);
    media_hls_playlist_stream_destroy(stream);

    /* The variant redirects to a second CDN, its segment to a third. */
    CHECK(media_worker_hop_init(&hop, variant));
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 301,
                                "https://cdn-b.test/v/low/index.m3u8", NULL),
              fixture.scratch) == MEDIA_WORKER_REDIRECT_FOLLOW
          && strcmp(hop.url, "https://cdn-b.test/v/low/index.m3u8") == 0);
    CHECK(media_worker_hop_init(&hop, "https://cdn-b.test/v/low/seg0.ts"));
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 307, "//edge.test/seg0.ts",
                                "referrer-policy: no-referrer\n"),
              fixture.scratch) == MEDIA_WORKER_REDIRECT_FOLLOW
          && strcmp(hop.url, "https://edge.test/seg0.ts") == 0
          && strcmp(hop.referrer_policy, "no-referrer") == 0);
    request = worker_request(&fixture, &authority, &hop, &ok);
    CHECK(ok && request.referer == NULL);

    /* A 3xx without Location, and any 2xx, are ordinary responses. */
    fixture.headers->status_code = 304;
    fixture.headers->response_headers[0] = '\0';
    fixture.headers->response_headers_length = 0;
    CHECK(media_worker_redirect(
              &authority, &hop, fixture.headers, fixture.scratch)
          == MEDIA_WORKER_RESPONSE);
    fixture.headers->status_code = 200;
    CHECK(media_worker_redirect(
              &authority, &hop, fixture.headers, fixture.scratch)
          == MEDIA_WORKER_RESPONSE);
    fixture_destroy(&fixture);
    CHECK(fixture.budget.current == 0);
}

static void expect_refused(Fixture *fixture,
                           const MediaWorkerAuthority *authority,
                           const char *from, const char *location)
{
    MediaWorkerHop hop;
    CHECK(media_worker_hop_init(&hop, from));
    MediaWorkerRedirectStatus status = media_worker_redirect(
        authority, &hop, redirect_response(fixture, 302, location, NULL),
        fixture->scratch);
    if (status != MEDIA_WORKER_REDIRECT_REFUSED)
        fprintf(stderr, "redirect to %s was not refused (%d, %s)\n",
                location, (int) status, hop.url);
    CHECK(status == MEDIA_WORKER_REDIRECT_REFUSED
          && strcmp(hop.url, from) == 0 && hop.redirects == 0u);
}

static void test_csp_media_src_refuses_redirect_target(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    static const char header[] =
        "content-security-policy: media-src 'self' https://cdn-a.test\n";
    TilefinchContentSecurityPolicy *policy = budget_malloc(
        &fixture.budget, sizeof(*policy));
    CHECK(policy != NULL);
    if (policy == NULL) return;
    tilefinch_csp_init(policy);
    CHECK(tilefinch_csp_parse_response_headers(
        policy, DOCUMENT, header, sizeof(header) - 1u, false));
    MediaWorkerAuthority authority = page_authority(&fixture, policy);
    MediaWorkerHop hop;
    CHECK(media_worker_hop_init(&hop, "https://video.test/master.m3u8"));
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 302,
                                "https://cdn-a.test/master.m3u8", NULL),
              fixture.scratch) == MEDIA_WORKER_REDIRECT_FOLLOW);
    expect_refused(&fixture, &authority, "https://cdn-a.test/master.m3u8",
                   "https://cdn-b.test/master.m3u8");
    /* Without the policy the same hop is followed. */
    authority.content_security_policy = NULL;
    CHECK(media_worker_hop_init(&hop, "https://cdn-a.test/master.m3u8"));
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 302,
                                "https://cdn-b.test/master.m3u8", NULL),
              fixture.scratch) == MEDIA_WORKER_REDIRECT_FOLLOW);
    budget_free(&fixture.budget, policy);
    fixture_destroy(&fixture);
}

static bool write_blocker_list(char *path, size_t size)
{
    /* A per-process name created exclusively in the test's working
       directory (the build tree). */
    snprintf(path, size, "tilefinch-media-worker-list-%ld",
             (long) getpid());
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) return false;
    static const char list[] = "||ads-video.test^$media\n";
    bool ok = write(descriptor, list, sizeof(list) - 1u)
        == (ssize_t) (sizeof(list) - 1u);
    ok = close(descriptor) == 0 && ok;
    if (!ok) unlink(path);
    return ok;
}

static void test_content_blocker_refuses_redirect_target(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    char path[64];
    CHECK(write_blocker_list(path, sizeof(path)));
    ContentBlocker *blocker = content_blocker_create(&fixture.budget);
    CHECK(blocker != NULL
          && content_blocker_configure(
                 blocker, CONTENT_BLOCKER_CUSTOM, path));
    unlink(path);
    fixture.session.content_blocker = blocker;
    MediaWorkerAuthority authority = page_authority(&fixture, NULL);
    expect_refused(&fixture, &authority, "https://video.test/seg.ts",
                   "https://ads-video.test/seg.ts");
    fixture.session.content_blocker = NULL;
    content_blocker_destroy(blocker);
    fixture_destroy(&fixture);
}

static void test_disallowed_targets_are_refused(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    MediaWorkerAuthority authority = page_authority(&fixture, NULL);
    const char *from = "https://video.test/seg.ts";
    expect_refused(&fixture, &authority, from, "ftp://cdn-a.test/seg.ts");
    expect_refused(&fixture, &authority, from, "file:///etc/passwd");
    expect_refused(&fixture, &authority, from, "data:video/mp2t,AAAA");
    expect_refused(&fixture, &authority, from, "javascript:alert(1)");
    expect_refused(&fixture, &authority, from,
                   "https://user:secret@cdn-a.test/seg.ts");
    /* Mixed content: a secure hop may not downgrade to plain HTTP. */
    expect_refused(&fixture, &authority, from, "http://cdn-a.test/seg.ts");

    /* A public page's media may not reach the private network. The target
       URL itself is admitted; the hop carries the flag the worker enforces
       on the resolved peer, exactly as a scheduler hop does. */
    MediaWorkerHop hop;
    CHECK(media_worker_hop_init(&hop, from));
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 302,
                                "https://192.168.1.20/seg.ts", NULL),
              fixture.scratch) == MEDIA_WORKER_REDIRECT_FOLLOW
          && hop.block_private_network);
    bool ok = false;
    FetchRequest request = worker_request(&fixture, &authority, &hop, &ok);
    CHECK(ok && request.block_private_network);

    /* A CORS-mode request stays on its origin on this path. */
    authority.mode = TILEFINCH_REQUEST_MODE_CORS;
    authority.credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN;
    expect_refused(&fixture, &authority, from, "https://cdn-a.test/seg.ts");
    CHECK(media_worker_hop_init(&hop, from));
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 302,
                                "https://video.test/other/seg.ts", NULL),
              fixture.scratch) == MEDIA_WORKER_REDIRECT_FOLLOW);

    /* During captive-portal sign-in no page media request is admitted. */
    authority.mode = TILEFINCH_REQUEST_MODE_NO_CORS;
    authority.credentials = TILEFINCH_CREDENTIALS_INCLUDE;
    CHECK(browser_session_captive_portal_begin(
        &fixture.session, "http://192.168.4.1/login"));
    expect_refused(&fixture, &authority, from, "https://cdn-a.test/seg.ts");
    fixture_destroy(&fixture);
}

static void test_redirect_loop_stops_at_the_hop_limit(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    MediaWorkerAuthority authority = page_authority(&fixture, NULL);
    MediaWorkerHop hop;
    CHECK(media_worker_hop_init(&hop, "https://video.test/a.ts"));
    for (unsigned i = 0; i < MEDIA_WORKER_REDIRECT_LIMIT; i++) {
        const char *next = i % 2u == 0u
            ? "https://cdn-a.test/a.ts" : "https://video.test/a.ts";
        CHECK(media_worker_redirect(
                  &authority, &hop,
                  redirect_response(&fixture, 302, next, NULL),
                  fixture.scratch) == MEDIA_WORKER_REDIRECT_FOLLOW);
    }
    CHECK(hop.redirects == MEDIA_WORKER_REDIRECT_LIMIT);
    /* Past the limit the redirect is refused even without scratch. */
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 302, "https://cdn-a.test/a.ts",
                                NULL),
              NULL) == MEDIA_WORKER_REDIRECT_REFUSED);
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 302, "https://cdn-a.test/a.ts",
                                NULL),
              fixture.scratch) == MEDIA_WORKER_REDIRECT_REFUSED);
    fixture_destroy(&fixture);
}

/* Native provider streams share the worker transport but not this policy:
   the worker follows their redirects itself, same-origin only, and a 3xx
   that reaches this layer is never followed. Their request shape is the
   historical one. */
static void test_provider_streams_keep_the_fixed_origin_rule(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    MediaWorkerAuthority authority = {
        .session = &fixture.session,
        .document_url = "https://provider.test/watch",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE
    };
    MediaWorkerHop hop;
    CHECK(media_worker_hop_init(&hop, "https://media.provider.test/live.m3u8"));
    bool ok = false;
    FetchRequest request = worker_request(&fixture, &authority, &hop, &ok);
    CHECK(ok && request.redirect_same_origin_only
          && request.redirect_url_validator
                 == media_worker_http_url_supported
          && !request.block_private_network
          && request.page_context == NULL
          && request.content_blocker == NULL
          && request.content_security_policy == NULL);
    CHECK(fetch_background_transport_stream_shape_supported(
        hop.url, &request, 64u * 1024u, 30000, false));
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 302,
                                "https://cdn-a.test/live.m3u8", NULL),
              fixture.scratch) == MEDIA_WORKER_RESPONSE
          && strcmp(hop.url, "https://media.provider.test/live.m3u8") == 0);
    CHECK(!media_worker_http_url_supported("ftp://media.provider.test/x"));
    fixture_destroy(&fixture);
}

/* The first hop of every playlist, variant and segment request meets the
   gate a redirect target does, before anything reaches the worker. The
   master below is an https page's, with CSP media-src and a content
   blocker; each URI it lists is admitted the way psp_hls_start_sized
   admits it. */
static MediaWorkerHopAdmission admit_listed(
    Fixture *fixture, const MediaWorkerAuthority *authority,
    const char *master, const char *uri, MediaWorkerHop *hop)
{
    char url[TILEFINCH_URL_SERIALIZED_LIMIT];
    CHECK(tilefinch_url_resolve(master, uri, url, sizeof(url)));
    CHECK(media_worker_hop_init(hop, url));
    MediaWorkerHopAdmission admission = media_worker_hop_admit(
        authority, hop, fixture->scratch);
    if (admission == MEDIA_WORKER_HOP_REFUSED) {
        /* A refused hop is left exactly as it was. */
        CHECK(strcmp(hop->url, url) == 0 && !hop->block_private_network);
    }
    return admission;
}

static void test_first_hops_meet_the_page_policy(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    static const char header[] =
        "content-security-policy: media-src 'self' https://cdn-a.test "
        "https://ads-video.test https://192.168.1.20\n";
    TilefinchContentSecurityPolicy *policy = budget_malloc(
        &fixture.budget, sizeof(*policy));
    CHECK(policy != NULL);
    if (policy == NULL) return;
    tilefinch_csp_init(policy);
    CHECK(tilefinch_csp_parse_response_headers(
        policy, DOCUMENT, header, sizeof(header) - 1u, false));
    char path[64];
    CHECK(write_blocker_list(path, sizeof(path)));
    ContentBlocker *blocker = content_blocker_create(&fixture.budget);
    CHECK(blocker != NULL
          && content_blocker_configure(
                 blocker, CONTENT_BLOCKER_CUSTOM, path));
    unlink(path);
    fixture.session.content_blocker = blocker;
    MediaWorkerAuthority authority = page_authority(&fixture, policy);
    const char *master = "https://video.test/hls/master.m3u8";
    MediaWorkerHop hop;
    bool ok = false;

    /* The master itself, then an allowed variant: admitted, and the worker
       request carries the page's private-network protection, as a
       scheduler request from a public page does. */
    CHECK(admit_listed(&fixture, &authority, master, "master.m3u8", &hop)
          == MEDIA_WORKER_HOP_ADMITTED);
    CHECK(admit_listed(&fixture, &authority, master,
                       "https://cdn-a.test/v/720/index.m3u8", &hop)
          == MEDIA_WORKER_HOP_ADMITTED
          && strcmp(hop.url, "https://cdn-a.test/v/720/index.m3u8") == 0
          && hop.block_private_network);
    FetchRequest request = worker_request(&fixture, &authority, &hop, &ok);
    CHECK(ok && request.block_private_network
          && request.page_context == NULL);

    /* (a) A plain-HTTP variant on a secure page is passive mixed content:
       auto-upgraded, never sent over HTTP, exactly as the scheduler treats
       a page media request. Upgraded onto a host the policy does not list,
       it is refused. */
    CHECK(admit_listed(&fixture, &authority, master,
                       "http://cdn-a.test/v/480/index.m3u8", &hop)
          == MEDIA_WORKER_HOP_ADMITTED
          && strcmp(hop.url, "https://cdn-a.test/v/480/index.m3u8") == 0);
    CHECK(admit_listed(&fixture, &authority, master,
                       "http://plain.test/v/480/index.m3u8", &hop)
          == MEDIA_WORKER_HOP_REFUSED);
    /* (b) A variant on a host outside media-src. */
    CHECK(admit_listed(&fixture, &authority, master,
                       "https://cdn-b.test/v/360/index.m3u8", &hop)
          == MEDIA_WORKER_HOP_REFUSED);
    /* (c) A content-blocked host the policy itself allows. */
    CHECK(admit_listed(&fixture, &authority, master,
                       "https://ads-video.test/v/240/index.m3u8", &hop)
          == MEDIA_WORKER_HOP_REFUSED);
    /* (d) A private-network segment on a public page: the URL is admitted
       with the flag the worker enforces on the resolved peer, so the hop
       can never connect to a private address. */
    CHECK(admit_listed(&fixture, &authority, master,
                       "https://192.168.1.20/seg0.ts", &hop)
          == MEDIA_WORKER_HOP_ADMITTED && hop.block_private_network);
    request = worker_request(&fixture, &authority, &hop, &ok);
    CHECK(ok && request.block_private_network);
    /* Schemes the worker cannot fetch never get that far. */
    CHECK(media_worker_hop_init(&hop, "ftp://cdn-a.test/seg0.ts"));
    CHECK(media_worker_hop_admit(&authority, &hop, fixture.scratch)
          == MEDIA_WORKER_HOP_REFUSED);

    /* Without the policy, the off-list variant is admitted. */
    authority.content_security_policy = NULL;
    CHECK(admit_listed(&fixture, &authority, master,
                       "https://cdn-b.test/v/360/index.m3u8", &hop)
          == MEDIA_WORKER_HOP_ADMITTED);

    /* No scratch: a memory failure, not a decision. */
    CHECK(media_worker_hop_init(&hop, "https://cdn-a.test/seg0.ts"));
    CHECK(media_worker_hop_admit(&authority, &hop, NULL)
          == MEDIA_WORKER_HOP_NO_MEMORY);
    /* A hop reached by an admitted redirect is not re-admitted. */
    hop.redirects = 1u;
    CHECK(media_worker_hop_admit(&authority, &hop, NULL)
          == MEDIA_WORKER_HOP_ADMITTED);

    fixture.session.content_blocker = NULL;
    content_blocker_destroy(blocker);
    budget_free(&fixture.budget, policy);
    fixture_destroy(&fixture);
}

/* A page that is itself on the private network may keep using it; an HSTS
   host's plain-HTTP URL is upgraded before it is admitted. */
static void test_first_hop_private_page_and_hsts(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    MediaWorkerAuthority authority = page_authority(&fixture, NULL);
    authority.document_url = "http://192.168.1.10/player";
    MediaWorkerHop hop;
    CHECK(media_worker_hop_init(&hop, "http://192.168.1.20/seg0.ts"));
    CHECK(media_worker_hop_admit(&authority, &hop, fixture.scratch)
              == MEDIA_WORKER_HOP_ADMITTED
          && !hop.block_private_network);

    authority.document_url = "http://video.test/player";
    static const char hsts[] =
        "strict-transport-security: max-age=3600\n";
    CHECK(browser_session_hsts_observe(
        &fixture.session, "https://cdn-a.test/", hsts, sizeof(hsts) - 1u));
    CHECK(media_worker_hop_init(&hop, "http://cdn-a.test/seg0.ts"));
    CHECK(media_worker_hop_admit(&authority, &hop, fixture.scratch)
              == MEDIA_WORKER_HOP_ADMITTED
          && strcmp(hop.url, "https://cdn-a.test/seg0.ts") == 0);
    fixture_destroy(&fixture);
}

/* Native provider streams have no page policy to meet. */
static void test_provider_first_hop_passes_through(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    MediaWorkerAuthority authority = {
        .session = &fixture.session,
        .document_url = "https://provider.test/watch",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE
    };
    MediaWorkerHop hop;
    CHECK(media_worker_hop_init(&hop, "https://media.provider.test/a.ts"));
    CHECK(media_worker_hop_admit(&authority, &hop, NULL)
              == MEDIA_WORKER_HOP_ADMITTED
          && !hop.block_private_network
          && strcmp(hop.url, "https://media.provider.test/a.ts") == 0);
    fixture_destroy(&fixture);
}

/* A redirect that finds no scratch storage is a memory failure of that
   request: it is not followed, and it is not reported as a refusal that
   would move playback to the next rendition. */
static void test_redirect_without_scratch_is_a_memory_failure(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    MediaWorkerAuthority authority = page_authority(&fixture, NULL);
    MediaWorkerHop hop;
    CHECK(media_worker_hop_init(&hop, "https://video.test/a.ts"));
    CHECK(media_worker_redirect(
              &authority, &hop,
              redirect_response(&fixture, 302, "https://cdn-a.test/a.ts",
                                NULL),
              NULL) == MEDIA_WORKER_REDIRECT_NO_MEMORY
          && strcmp(hop.url, "https://video.test/a.ts") == 0
          && hop.redirects == 0u);
    /* The PSP HLS glue's message for it (psp_hls_admit_headers). */
    CHECK(!media_candidate_refused("HLS redirect budget refused"));
    CHECK(media_candidate_refused("HLS redirect refused (HTTP 302)"));
    fixture_destroy(&fixture);
}

/* A secure page's plain-HTTP audio stream (SECURITY_MODEL.md, "Unencrypted
   audio"). It is upgraded once, like any passive media, until the user says
   yes for the page's site; then that site's audio hops alone may use HTTP,
   on the first hop and on every redirect. Video, other sites and the broad
   mixed-content grant are untouched, and the answer lives only in the
   session. */
static void test_insecure_audio_answer_is_narrow(void)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    MediaWorkerAuthority audio = page_authority(&fixture, NULL);
    audio.audio = true;
    MediaWorkerAuthority video = page_authority(&fixture, NULL);
    MediaWorkerAuthority elsewhere = audio;
    elsewhere.document_url = "https://other.test/radio";
    const char *station = "http://station.test:8002/stream?fromyp=true";
    const char *upgraded = "https://station.test:8002/stream?fromyp=true";
    MediaWorkerHop hop;

    /* Unasked and declined: upgraded, and an HTTP redirect is refused. */
    for (unsigned pass = 0; pass < 2u; pass++) {
        CHECK(media_worker_hop_init(&hop, station)
              && media_worker_hop_admit(&audio, &hop, fixture.scratch)
                     == MEDIA_WORKER_HOP_ADMITTED
              && strcmp(hop.url, upgraded) == 0);
        expect_refused(&fixture, &audio, upgraded, "http://relay.test/live");
        CHECK(browser_session_set_insecure_audio_site(
            &fixture.session, DOCUMENT, BROWSER_INSECURE_AUDIO_DECLINED));
    }
    CHECK(browser_session_insecure_audio_site(&fixture.session, DOCUMENT)
          == BROWSER_INSECURE_AUDIO_DECLINED);

    /* Yes, for the page's site: any page of it, its audio only. */
    CHECK(browser_session_set_insecure_audio_site(
              &fixture.session, "https://video.test/another/page",
              BROWSER_INSECURE_AUDIO_ALLOWED)
          && browser_session_insecure_audio_site(&fixture.session, DOCUMENT)
                 == BROWSER_INSECURE_AUDIO_ALLOWED
          && !browser_session_mixed_content_site_allowed(
                 &fixture.session, DOCUMENT));
    CHECK(media_worker_hop_init(&hop, station)
          && media_worker_hop_admit(&audio, &hop, fixture.scratch)
                 == MEDIA_WORKER_HOP_ADMITTED
          && strcmp(hop.url, station) == 0);
    bool ok = false;
    FetchRequest request = worker_request(&fixture, &audio, &hop, &ok);
    CHECK(ok && request.block_private_network
          && strcmp(request.sec_fetch_dest, "audio") == 0);
    CHECK(media_worker_redirect(
              &audio, &hop,
              redirect_response(&fixture, 302, "http://relay.test/live",
                                NULL),
              fixture.scratch) == MEDIA_WORKER_REDIRECT_FOLLOW
          && strcmp(hop.url, "http://relay.test/live") == 0);
    /* The page's video, and another site's audio, are still upgraded, and
       still refuse an HTTP redirect. */
    CHECK(media_worker_hop_init(&hop, station)
          && media_worker_hop_admit(&video, &hop, fixture.scratch)
                 == MEDIA_WORKER_HOP_ADMITTED
          && strcmp(hop.url, upgraded) == 0);
    expect_refused(&fixture, &video, upgraded, "http://relay.test/live");
    CHECK(media_worker_hop_init(&hop, station)
          && media_worker_hop_admit(&elsewhere, &hop, fixture.scratch)
                 == MEDIA_WORKER_HOP_ADMITTED
          && strcmp(hop.url, upgraded) == 0);
    expect_refused(&fixture, &elsewhere, upgraded, "http://relay.test/live");

    /* The question itself: asked for a refused or empty probe of a plain
       HTTP address on a secure page, never for loopback, an HTTPS address,
       an insecure page or a probe that found audio. */
    BrowserSession *session = &fixture.session;
    CHECK(media_http_insecure_audio_step(
              session, station, "https://new.test/", MEDIA_HTTP_PROBE_REFUSED)
          == MEDIA_INSECURE_AUDIO_ASK
          && media_http_insecure_audio_step(
              session, station, "https://new.test/",
              MEDIA_HTTP_PROBE_UNAVAILABLE) == MEDIA_INSECURE_AUDIO_ASK
          && media_http_insecure_audio_step(
              session, station, "https://new.test/", MEDIA_HTTP_PROBE_MP3)
                 == MEDIA_INSECURE_AUDIO_NONE
          && media_http_insecure_audio_step(
              session, "http://127.0.0.1:8002/stream", "https://new.test/",
              MEDIA_HTTP_PROBE_REFUSED) == MEDIA_INSECURE_AUDIO_NONE
          && media_http_insecure_audio_step(
              session, upgraded, "https://new.test/",
              MEDIA_HTTP_PROBE_REFUSED) == MEDIA_INSECURE_AUDIO_NONE
          && media_http_insecure_audio_step(
              session, station, "http://new.test/",
              MEDIA_HTTP_PROBE_REFUSED) == MEDIA_INSECURE_AUDIO_NONE
          && media_http_insecure_audio_step(
              session, station, DOCUMENT, MEDIA_HTTP_PROBE_REFUSED)
                 == MEDIA_INSECURE_AUDIO_NONE);
    CHECK(browser_session_set_insecure_audio_site(
              session, "https://new.test/", BROWSER_INSECURE_AUDIO_DECLINED)
          && media_http_insecure_audio_step(
              session, station, "https://new.test/a",
              MEDIA_HTTP_PROBE_REFUSED) == MEDIA_INSECURE_AUDIO_DECLINED);
    /* Ask again: the answer is forgotten. */
    CHECK(browser_session_set_insecure_audio_site(
              session, "https://new.test/", BROWSER_INSECURE_AUDIO_UNASKED)
          && browser_session_insecure_audio_site(session, "https://new.test/")
                 == BROWSER_INSECURE_AUDIO_UNASKED);

    /* The table is bounded; a full table refuses a new site but still
       changes one it holds. */
    char site[64];
    bool stored = true;
    unsigned held = 0;
    for (unsigned at = 0; at < 64u && stored; at++) {
        snprintf(site, sizeof(site), "https://s%u.test/", at);
        stored = browser_session_set_insecure_audio_site(
            session, site, BROWSER_INSECURE_AUDIO_DECLINED);
        if (stored) held++;
    }
    CHECK(!stored && held >= 8u
          && browser_session_set_insecure_audio_site(
                 session, DOCUMENT, BROWSER_INSECURE_AUDIO_DECLINED)
          && browser_session_insecure_audio_site(session, DOCUMENT)
                 == BROWSER_INSECURE_AUDIO_DECLINED);

    /* A new session is the restart boundary: nothing carries over. */
    CHECK(browser_session_set_insecure_audio_site(
        session, DOCUMENT, BROWSER_INSECURE_AUDIO_ALLOWED));
    browser_session_destroy(session);
    CHECK(browser_session_init(session, &fixture.budget, 128u * 1024u)
          && browser_session_insecure_audio_site(session, DOCUMENT)
                 == BROWSER_INSECURE_AUDIO_UNASKED);
    fixture_destroy(&fixture);
    CHECK(fixture.budget.current == 0);
}

int main(void)
{
    test_hls_redirect_chain_crosses_origins();
    test_csp_media_src_refuses_redirect_target();
    test_content_blocker_refuses_redirect_target();
    test_disallowed_targets_are_refused();
    test_redirect_loop_stops_at_the_hop_limit();
    test_provider_streams_keep_the_fixed_origin_rule();
    test_first_hops_meet_the_page_policy();
    test_first_hop_private_page_and_hsts();
    test_provider_first_hop_passes_through();
    test_redirect_without_scratch_is_a_memory_failure();
    test_insecure_audio_answer_is_narrow();
    if (failures != 0) {
        printf("media-worker-request-tests: %d check(s) failed\n", failures);
        return 1;
    }
    puts("media-worker-request-tests: all checks passed");
    return 0;
}
