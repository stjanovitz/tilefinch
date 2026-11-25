/* navigator.mediaDevices.getUserMedia({audio}): the MediaStream surface, the
   admission rules (secure context, frame delegation, visibility, the stored
   decision and the prompt's anti-abuse limits), what is remembered, and how
   capture ends (tilefinch/media_capture.h).

   The host has no microphone and no prompt UI. The seam below is the
   platform's media_capture services: a scripted prompt that records every
   request, the real BrowserProfile site table for what is remembered, and a
   fake input that produces a 440 Hz sine. Nothing here is built into a PSP
   executable. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/game_audio.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/media_capture.h"
#include "tilefinch/media_capture_input.h"
#include "tilefinch/navigation.h"
#include "tilefinch/platform.h"
#include "tilefinch/viewport.h"
#include "../src/js_runtime_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "MEDIA CAPTURE CHECK failed at %s:%d: %s\n",        \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

/* ---- the scripted frontend ------------------------------------------- */

typedef struct {
    char host[MEDIA_CAPTURE_HOST_BYTES];
    char origin[128];
    bool from_frame;
    bool can_remember;
} RecordedPrompt;

enum { ANSWER_LIMIT = 16, PROMPT_RECORD_LIMIT = 32 };

static struct {
    BrowserProfile *profile;
    bool available;
    bool prompt_shown;
    MediaCaptureVerdict start_verdict;
    MediaCapturePromptAnswer answers[ANSWER_LIMIT];
    size_t answer_count;
    size_t answer_next;
    RecordedPrompt prompts[PROMPT_RECORD_LIMIT];
    size_t prompt_count;
    unsigned starts;
    unsigned stops;
    bool running;
    bool headphones;
    double level; /* the fake input's DC level for read_since */
    uint32_t since_reads;
    bool failed; /* the input stalls after it started */
    bool input_busy; /* a transfer of the stopped capture never ended */
    unsigned free_checks;
} seam;

static bool seam_available(void *context)
{
    (void) context;
    return seam.available;
}

static MediaCapturePermission seam_permission(void *context,
                                              const char *origin,
                                              bool *can_remember)
{
    (void) context;
    if (can_remember != NULL)
        *can_remember = browser_profile_microphone_site_room(seam.profile,
                                                             origin);
    return browser_profile_microphone_site(seam.profile, origin);
}

static bool seam_prompt(void *context,
                        const TilefinchMediaCapturePrompt *request,
                        MediaCapturePromptAnswer *answer)
{
    (void) context;
    if (seam.prompt_count < PROMPT_RECORD_LIMIT) {
        RecordedPrompt *record = &seam.prompts[seam.prompt_count];
        snprintf(record->host, sizeof(record->host), "%s", request->host);
        snprintf(record->origin, sizeof(record->origin), "%s",
                 request->origin);
        record->from_frame = request->from_frame;
        record->can_remember = request->can_remember;
    }
    seam.prompt_count++;
    if (!seam.prompt_shown) return false;
    *answer = seam.answer_next < seam.answer_count
        ? seam.answers[seam.answer_next++] : MEDIA_CAPTURE_PROMPT_DISMISSED;
    return true;
}

static bool seam_remember(void *context, const char *origin,
                          MediaCapturePermission permission)
{
    (void) context;
    return browser_profile_set_microphone_site(seam.profile, origin,
                                               permission);
}

static MediaCaptureVerdict seam_start(void *context,
                                      MediaCaptureFormat *format)
{
    (void) context;
    (void) format;
    if (seam.start_verdict != MEDIA_CAPTURE_GRANTED)
        return seam.start_verdict;
    seam.starts++;
    seam.running = true;
    return MEDIA_CAPTURE_GRANTED;
}

static void seam_stop(void *context)
{
    (void) context;
    seam.stops++;
    seam.running = false;
}

/* The fake input as the Web Audio pump reads it: 1024 new samples at the
   seam's level per read, after a first read that only sets the cursor. */
static uint32_t seam_read_since(void *context, float *output, uint32_t count,
                                uint32_t *cursor)
{
    (void) context;
    if (!seam.running) return 0;
    seam.since_reads++;
    if (*cursor == UINT32_MAX) {
        *cursor = 0;
        return 0;
    }
    uint32_t fresh = count < 1024u ? count : 1024u;
    for (uint32_t at = 0; at < fresh; at++) output[at] = (float) seam.level;
    *cursor += fresh;
    return fresh;
}

static bool seam_failed(void *context)
{
    (void) context;
    return seam.failed && seam.running;
}

static bool seam_input_free(void *context)
{
    (void) context;
    seam.free_checks++;
    return !seam.running && !seam.input_busy;
}

static bool seam_headphones(void *context)
{
    (void) context;
    return seam.headphones;
}

static Budget profile_budget;

static void seam_install(void)
{
    BrowserProfile *profile = seam.profile;
    memset(&seam, 0, sizeof(seam));
    seam.profile = profile;
    seam.available = true;
    seam.prompt_shown = true;
    seam.start_verdict = MEDIA_CAPTURE_GRANTED;
    TilefinchPlatformServices services = {
        .media_capture_available = seam_available,
        .media_capture_permission = seam_permission,
        .media_capture_prompt = seam_prompt,
        .media_capture_remember = seam_remember,
        .media_capture_start = seam_start,
        .media_capture_stop = seam_stop,
        .media_capture_read_since = seam_read_since,
        .media_capture_failed = seam_failed,
        .media_capture_input_free = seam_input_free,
        .audio_output_private = seam_headphones
    };
    tilefinch_platform_set_services(&services);
}

static void seam_answers(const MediaCapturePromptAnswer *answers,
                         size_t count)
{
    seam.answer_count = count < ANSWER_LIMIT ? count : ANSWER_LIMIT;
    memcpy(seam.answers, answers, seam.answer_count * sizeof(*answers));
    seam.answer_next = 0;
}

static bool profile_fresh(void)
{
    if (seam.profile != NULL) browser_profile_destroy(seam.profile);
    seam.profile = browser_profile_create(&profile_budget);
    return seam.profile != NULL;
}

/* ---- the capture worker's input (media_capture_input.h) --------------- */

/* A scripted sceAudioInput. A transfer ends on its END_AFTER-th poll after
   it began, and only then writes its block; from HANG_AFTER_BEGINS on, a
   transfer never ends until the test says so. */
typedef struct {
    int16_t *writing;
    bool busy;
    uint32_t polls;
    uint32_t end_after;
    unsigned hang_after_begins;
    unsigned stop_after_blocks;
    unsigned begins;
    unsigned blocks;
    bool stop;
    bool poll_error;
    uint64_t slept_us;
    uint64_t slept_at_stop_us;
} FakeInput;

static int fake_begin(void *user, int16_t *block)
{
    FakeInput *input = user;
    if (input->busy) return -1; /* SCE_AUDIO_ERROR_INPUT_BUSY */
    input->writing = block;
    input->busy = true;
    input->polls = 0;
    input->begins++;
    return 0;
}

static int fake_poll(void *user)
{
    FakeInput *input = user;
    if (input->poll_error) return -1;
    if (!input->busy) return 0;
    bool hung = input->hang_after_begins != 0
        && input->begins > input->hang_after_begins;
    if (hung || ++input->polls < input->end_after) return 1;
    for (unsigned at = 0; at < MEDIA_CAPTURE_BLOCK_SAMPLES; at++)
        input->writing[at] = (int16_t) (100 + at);
    input->busy = false;
    return 0;
}

/* The hung transfer completes, writing its block (wherever it points). */
static void fake_finish(FakeInput *input)
{
    input->hang_after_begins = 0;
    input->end_after = 1;
}

static void fake_sleep(void *user, uint32_t microseconds)
{
    ((FakeInput *) user)->slept_us += microseconds;
}

static void fake_arrived(void *user, const int16_t *block)
{
    FakeInput *input = user;
    (void) block;
    if (++input->blocks >= input->stop_after_blocks && !input->stop) {
        input->stop = true;
        input->slept_at_stop_us = input->slept_us;
    }
}

static bool fake_stop_requested(void *user)
{
    return ((FakeInput *) user)->stop;
}

static void fake_running(void *user)
{
    (void) user;
}

static int test_input_custody(void)
{
    puts("test: a block pair the input may still write is never freed");
    static Budget budget;
    budget_init(&budget, MIB);
    FakeInput input = {.end_after = 3, .stop_after_blocks = 3};
    const MediaCaptureInputBackend backend = {
        .user = &input, .begin = fake_begin, .poll = fake_poll,
        .sleep_us = fake_sleep, .arrived = fake_arrived,
        .stop_requested = fake_stop_requested, .running = fake_running
    };
    MediaCaptureInputCustody custody = {0};
    MediaCaptureVerdict verdict = MEDIA_CAPTURE_FAILED;
    const size_t pair_bytes = MEDIA_CAPTURE_INPUT_PAIR_BYTES;
    /* An ordinary stop: the block in flight ends inside the drain. */
    int16_t *pair = media_capture_input_pair_acquire(&custody, &budget,
                                                     &backend, &verdict);
    CHECK(pair != NULL && verdict == MEDIA_CAPTURE_GRANTED
          && budget.current >= pair_bytes);
    bool pending = true;
    CHECK(media_capture_input_run(&backend, pair, &pending)
              == MEDIA_CAPTURE_INPUT_EXITED
          && !pending && !input.busy && input.blocks == 3u);
    media_capture_input_pair_release(&custody, &budget, pair, pending);
    CHECK(budget.current == 0 && custody.held == NULL);
    /* The input stops answering just as capture stops: the drain runs out
       with the transfer still busy. The pair stays allocated. */
    input = (FakeInput) {.end_after = 3, .stop_after_blocks = 2,
                         .hang_after_begins = 2};
    pair = media_capture_input_pair_acquire(&custody, &budget, &backend,
                                            &verdict);
    CHECK(pair != NULL);
    CHECK(media_capture_input_run(&backend, pair, &pending)
              == MEDIA_CAPTURE_INPUT_EXITED);
    CHECK(input.busy && pending);
    media_capture_input_pair_release(&custody, &budget, pair, pending);
    CHECK(custody.held == pair && budget.current >= pair_bytes);
    /* The worker leaves well inside the frontend's join. */
    CHECK(input.slept_us - input.slept_at_stop_us
          <= MEDIA_CAPTURE_INPUT_DRAIN_US + MEDIA_CAPTURE_INPUT_POLL_US);
    /* While it is busy, a start is refused and nothing more is allocated,
       however often it is tried. */
    size_t holding = budget.current;
    for (int attempt = 0; attempt < 8; attempt++) {
        CHECK(media_capture_input_pair_acquire(&custody, &budget, &backend,
                                               &verdict) == NULL
              && verdict == MEDIA_CAPTURE_BUSY && budget.current == holding);
    }
    /* The transfer finally ends, into the pair still held; the next start
       reuses that pair, zeroed. */
    fake_finish(&input);
    int16_t *again = media_capture_input_pair_acquire(&custody, &budget,
                                                      &backend, &verdict);
    CHECK(again == pair && verdict == MEDIA_CAPTURE_GRANTED
          && custody.held == NULL && budget.current == holding);
    for (size_t at = 0; at < 2u * MEDIA_CAPTURE_BLOCK_SAMPLES; at++)
        CHECK(again[at] == 0);
    /* A poll error proves nothing either: a running input that errors
       keeps its pair until a poll answers 0. */
    input = (FakeInput) {.end_after = 3, .stop_after_blocks = 100};
    input.hang_after_begins = 2;
    CHECK(media_capture_input_run(&backend, again, &pending)
              == MEDIA_CAPTURE_INPUT_STALLED && pending);
    media_capture_input_pair_release(&custody, &budget, again, pending);
    CHECK(custody.held == again);
    input.poll_error = true;
    CHECK(media_capture_input_pair_acquire(&custody, &budget, &backend,
                                           &verdict) == NULL
          && verdict == MEDIA_CAPTURE_BUSY);
    input.poll_error = false;
    fake_finish(&input);
    again = media_capture_input_pair_acquire(&custody, &budget, &backend,
                                             &verdict);
    CHECK(again == pair);
    media_capture_input_pair_release(&custody, &budget, again, false);
    CHECK(budget.current == 0 && custody.held == NULL);
    return 0;
}

static int test_input_settle(void)
{
    puts("test: the input is handed to voice typing only once it is idle");
    static Budget budget;
    budget_init(&budget, MIB);
    FakeInput input = {.end_after = 3, .stop_after_blocks = 2,
                       .hang_after_begins = 2};
    const MediaCaptureInputBackend backend = {
        .user = &input, .begin = fake_begin, .poll = fake_poll,
        .sleep_us = fake_sleep, .arrived = fake_arrived,
        .stop_requested = fake_stop_requested, .running = fake_running
    };
    MediaCaptureInputCustody custody = {0};
    /* Nothing held: free at once, without asking the input. */
    CHECK(media_capture_input_settle(&custody, &budget, &backend));
    CHECK(input.begins == 0u && input.polls == 0u);
    /* A capture stopped while its transfer hung: the pair is held, and the
       input is not free, however often it is asked. Each answer is one
       poll and no wait. */
    MediaCaptureVerdict verdict = MEDIA_CAPTURE_FAILED;
    int16_t *pair = media_capture_input_pair_acquire(&custody, &budget,
                                                     &backend, &verdict);
    CHECK(pair != NULL);
    bool pending = false;
    (void) media_capture_input_run(&backend, pair, &pending);
    CHECK(pending);
    media_capture_input_pair_release(&custody, &budget, pair, pending);
    size_t holding = budget.current;
    CHECK(custody.held == pair && holding > 0u);
    uint64_t slept = input.slept_us;
    for (int ask = 0; ask < 8; ask++)
        CHECK(!media_capture_input_settle(&custody, &budget, &backend)
              && custody.held == pair && budget.current == holding);
    CHECK(input.slept_us == slept);
    /* A poll error proves nothing either. */
    input.poll_error = true;
    CHECK(!media_capture_input_settle(&custody, &budget, &backend));
    input.poll_error = false;
    /* Once a poll shows it ended, the held pair goes back to the budget,
       zeroed, and the input is free. */
    fake_finish(&input);
    CHECK(media_capture_input_settle(&custody, &budget, &backend));
    CHECK(custody.held == NULL && budget.current == 0u);
    return 0;
}

/* ---- a page realm ----------------------------------------------------- */

static Budget page_budget;

typedef struct {
    size_t baseline;
    PocDocument document;
    ScriptRuntime *runtime;
    ScriptResult result;
} CapturePage;

static bool page_open(CapturePage *page, const char *url)
{
    static const char html[] =
        "<!doctype html><html><head><title>mic</title></head>"
        "<body><p>page</p></body></html>";
    memset(page, 0, sizeof(*page));
    page->baseline = page_budget.current;
    if (!document_parse(&page->document, &page_budget, html,
                        sizeof(html) - 1u, sizeof(html)))
        return false;
    page->runtime = script_runtime_create_deferred(
        &page->document, &page_budget, 8u * MIB, 3000, url, NULL,
        &page->result);
    return page->runtime != NULL;
}

/* A page whose Web Audio is the native slots implementation, as shipped. */
static bool page_open_audio(CapturePage *page, const char *url)
{
    static const char html[] =
        "<!doctype html><html><head><title>mic</title></head>"
        "<body><p>page</p></body></html>";
    memset(page, 0, sizeof(*page));
    page->baseline = page_budget.current;
    if (!document_parse(&page->document, &page_budget, html,
                        sizeof(html) - 1u, sizeof(html)))
        return false;
    ScriptRuntimeOptions options = {
        .defer_document_scripts = true,
        .game_audio_internal_slots = true
    };
    if (!viewport_context_init(&options.viewport, 480, 272, 480, 272))
        return false;
    page->runtime = script_runtime_create_configured(
        &page->document, &page_budget, 8u * MIB, 3000, url, &options,
        &page->result);
    return page->runtime != NULL && page->runtime->game_audio_internal_slots;
}

static bool page_close(CapturePage *page)
{
    script_runtime_destroy(page->runtime);
    document_destroy(&page->document);
    bool clean = page_budget.current == page->baseline;
    if (!clean)
        fprintf(stderr, "media capture test leaked %zu bytes\n",
                page_budget.current - page->baseline);
    return clean;
}

/* Evaluates SOURCE, which sets pocSummary (directly or from a promise
   reaction), and runs the page's tasks until it is not "PENDING"; true when
   it says "OK". */
static bool page_run(CapturePage *page, const char *source)
{
    static char block[6144];
    if (snprintf(block, sizeof(block), "{pocSummary='PENDING';%s\n}", source)
        >= (int) sizeof(block)) return false;
    memset(&page->result, 0, sizeof(page->result));
    bool ran = script_runtime_evaluate_diagnostic(
        page->runtime, block, "https://mic.test/probe.js", &page->result);
    for (unsigned turn = 0; ran && turn < 32u
         && strcmp(page->result.summary, "PENDING") == 0; turn++)
        ran = script_runtime_advance(page->runtime, 0, 8, &page->result);
    if (!ran || strcmp(page->result.summary, "OK") != 0) {
        fprintf(stderr, "probe: ran=%d summary=%s error=%s callback=%s "
                "prompts=%zu starts=%u stops=%u\n", ran,
                page->result.summary, page->result.error,
                page->result.last_uncaught_callback_error,
                seam.prompt_count, seam.starts, seam.stops);
        return false;
    }
    return true;
}

/* Runs a few task turns, delivering what the browser queued. */
static bool page_turns(CapturePage *page)
{
    for (unsigned turn = 0; turn < 4u; turn++)
        if (!script_runtime_advance(page->runtime, 0, 8, &page->result))
            return false;
    return true;
}

/* A probe that expects getUserMedia to reject with NAME. */
#define EXPECT_REJECTION(name)                                              \
    "navigator.mediaDevices.getUserMedia({audio:true}).then("               \
    "s=>{s.getTracks().forEach(t=>t.stop());pocSummary='RESOLVED'},"        \
    "e=>{pocSummary=e instanceof DOMException&&e.name==='" name "'?'OK':"   \
    "'BAD '+e.name+': '+e.message})"

#define EXPECT_STREAM(store)                                                \
    "navigator.mediaDevices.getUserMedia({audio:true}).then("               \
    "s=>{" store "=s;pocSummary=s.active?'OK':'INACTIVE'},"                 \
    "e=>{pocSummary='REJECTED '+e.name+': '+e.message})"

/* ---- engine pieces ---------------------------------------------------- */

static int test_prompt_modal(void)
{
    puts("test: the prompt answers only deliberate presses");
    MediaCapturePromptModal modal;
    /* X held when the prompt opened does nothing until it is released, and
       an allow waits for the arm delay; Block acts at once. */
    media_capture_prompt_modal_open(&modal, true, MEDIA_CAPTURE_BUTTON_CROSS,
                                    1000);
    CHECK(modal.selection == 0);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_CROSS,
                                          1700) == MEDIA_CAPTURE_PROMPT_PENDING);
    CHECK(media_capture_prompt_modal_step(&modal, 0, 1710)
          == MEDIA_CAPTURE_PROMPT_PENDING);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_CROSS,
                                          1720)
          == MEDIA_CAPTURE_PROMPT_ALLOW_ONCE);
    media_capture_prompt_modal_open(&modal, true, 0, 1000);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_CROSS,
                                          1100)
          == MEDIA_CAPTURE_PROMPT_PENDING);
    CHECK(media_capture_prompt_modal_step(&modal, 0, 1600)
          == MEDIA_CAPTURE_PROMPT_PENDING);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_DOWN,
                                          1610)
          == MEDIA_CAPTURE_PROMPT_PENDING && modal.selection == 1);
    CHECK(media_capture_prompt_modal_step(&modal, 0, 1620)
          == MEDIA_CAPTURE_PROMPT_PENDING);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_CROSS,
                                          1630)
          == MEDIA_CAPTURE_PROMPT_ALLOW_ALWAYS);
    /* Block, before the arm delay. */
    media_capture_prompt_modal_open(&modal, true, 0, 1000);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_UP,
                                          1010)
          == MEDIA_CAPTURE_PROMPT_PENDING && modal.selection == 2);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_UP
                                          | MEDIA_CAPTURE_BUTTON_CROSS, 1020)
          == MEDIA_CAPTURE_PROMPT_BLOCK);
    /* "Always" is skipped when it cannot be remembered. */
    media_capture_prompt_modal_open(&modal, false, 0, 1000);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_DOWN,
                                          1010)
          == MEDIA_CAPTURE_PROMPT_PENDING && modal.selection == 2);
    CHECK(media_capture_prompt_modal_step(&modal, 0, 1020)
          == MEDIA_CAPTURE_PROMPT_PENDING);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_UP,
                                          1030)
          == MEDIA_CAPTURE_PROMPT_PENDING && modal.selection == 0);
    /* Circle dismisses at once, and so does the timeout. */
    media_capture_prompt_modal_open(&modal, true, 0, 1000);
    CHECK(media_capture_prompt_modal_step(&modal, MEDIA_CAPTURE_BUTTON_CIRCLE,
                                          1001)
          == MEDIA_CAPTURE_PROMPT_DISMISSED);
    media_capture_prompt_modal_open(&modal, true, 0, 1000);
    CHECK(media_capture_prompt_modal_step(
              &modal, 0, 1000 + MEDIA_CAPTURE_PROMPT_MAXIMUM_OPEN_MS)
          == MEDIA_CAPTURE_PROMPT_DISMISSED);
    return 0;
}

static int test_frame_policy(void)
{
    puts("test: the microphone policy's default is 'self'");
    const char *child = "https://child.test";
#define ALLOWED(allow, same) media_capture_frame_allowed(                   \
        allow, (allow) == NULL ? 0 : strlen(allow), same, false, child, child, \
        true)
    CHECK(ALLOWED(NULL, true));
    CHECK(!ALLOWED(NULL, false));
    CHECK(!ALLOWED("camera", false));
    CHECK(ALLOWED("microphone", false));
    CHECK(ALLOWED("  MICROPHONE  ", false));
    CHECK(ALLOWED("camera; microphone", false));
    CHECK(ALLOWED("microphone *", false));
    CHECK(ALLOWED("microphone 'src'", false));
    CHECK(ALLOWED("microphone https://child.test", false));
    CHECK(ALLOWED("microphone https://child.test:443/path", false));
    CHECK(!ALLOWED("microphone https://other.test", false));
    CHECK(!ALLOWED("microphone 'self'", false));
    CHECK(ALLOWED("microphone 'self'", true));
    CHECK(!ALLOWED("microphone 'none'", true));
    CHECK(!ALLOWED("microphone 'none'", false));
    CHECK(ALLOWED("microphone *; microphone 'none'", false));
    CHECK(!ALLOWED("microphone 'none'; microphone *", false));
    CHECK(!ALLOWED("microphones", false));
#undef ALLOWED
    /* An opaque frame, or a parent that may not, never may. */
    CHECK(!media_capture_frame_allowed("microphone", 10, true, true, child,
                                       child, true));
    CHECK(!media_capture_frame_allowed("microphone", 10, true, false, child,
                                       child, false));
    CHECK(!media_capture_frame_allowed(NULL, 0, true, false, child, child,
                                       false));
    /* 'src' is the container's src origin: a child that is now elsewhere
       (redirected, or navigated itself) is not delegated, unless by '*'
       or by its own origin. */
    const char *src = "https://widget.test";
    CHECK(!media_capture_frame_allowed("microphone", 10, false, false, child,
                                       src, true));
    CHECK(!media_capture_frame_allowed("microphone 'src'", 16, false, false,
                                       child, src, true));
    CHECK(!media_capture_frame_allowed("microphone", 10, false, false, child,
                                       "", true));
    CHECK(media_capture_frame_allowed("microphone *", 12, false, false, child,
                                      src, true));
    CHECK(media_capture_frame_allowed("microphone https://child.test", 29,
                                      false, false, child, src, true));
    return 0;
}

static int test_ring(void)
{
    puts("test: the ring keeps only the newest samples");
    enum { CAPACITY = 64 + 4 * MEDIA_CAPTURE_BLOCK_SAMPLES };
    static int16_t storage[CAPACITY];
    static int16_t block[MEDIA_CAPTURE_BLOCK_SAMPLES];
    static float out[128];
    MediaCaptureRing ring;
    media_capture_ring_init(&ring, storage, CAPACITY);
    /* The first read only places the cursor. */
    uint32_t cursor = UINT32_MAX;
    CHECK(media_capture_ring_since(&ring, &cursor, out, 128) == 0
          && cursor == 0u);
    for (unsigned at = 0; at < 16; at++) block[at] = (int16_t) (at * 100);
    media_capture_ring_publish(&ring, block, 16);
    CHECK(media_capture_ring_written(&ring) == 16);
    CHECK(media_capture_ring_since(&ring, &cursor, out, 32) == 16);
    CHECK(out[0] == 0.0f && fabsf(out[15] - 1500.0f / 32768.0f) < 1e-6f);
    /* Wrap many times; a reader a whole ring behind gets the newest it
       may read (not what a writer may be filling), in order. */
    int16_t next = 0;
    for (unsigned round = 0; round < 40; round++) {
        for (unsigned at = 0; at < MEDIA_CAPTURE_BLOCK_SAMPLES; at++)
            block[at] = next++;
        media_capture_ring_publish(&ring, block, MEDIA_CAPTURE_BLOCK_SAMPLES);
    }
    CHECK(media_capture_ring_written(&ring) == CAPACITY);
    CHECK(media_capture_ring_since(&ring, &cursor, out, 128) == 64);
    for (unsigned at = 0; at < 64; at++)
        CHECK(out[at] == (float) (next - 64 + (int) at) / 32768.0f);
    /* A cursor reader gets each sample once, oldest first. */
    CHECK(media_capture_ring_since(&ring, &cursor, out, 128) == 0
          && cursor < CAPACITY);
    for (unsigned at = 0; at < 48; at++) block[at] = next++;
    media_capture_ring_publish(&ring, block, 48);
    CHECK(media_capture_ring_since(&ring, &cursor, out, 128) == 48);
    for (unsigned at = 0; at < 48; at++)
        CHECK(out[at] == (float) (next - 48 + (int) at) / 32768.0f);
    CHECK(media_capture_ring_since(&ring, &cursor, out, 128) == 0);
    /* A reader that fell behind gets the newest it may read. */
    for (unsigned round = 0; round < 3; round++) {
        for (unsigned at = 0; at < 100; at++) block[at] = next++;
        media_capture_ring_publish(&ring, block, 100);
    }
    CHECK(media_capture_ring_since(&ring, &cursor, out, 64) == 64);
    CHECK(out[63] == (float) (next - 1) / 32768.0f);
    unsigned peak = 0;
    uint64_t square = 0;
    int16_t levels[4] = {-32768, 0, 100, -100};
    media_capture_block_levels(levels, 4, &peak, &square);
    CHECK(peak == 32768u
          && square == (UINT64_C(1073741824) + 20000u) / 4u);
    return 0;
}

/* ---- the page surface ------------------------------------------------- */

static int test_interfaces(void)
{
    puts("test: the interfaces exist only in secure contexts");
    seam_install();
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/"));
    CHECK(page_run(&page,
        "const f=[],e=(n,c)=>{if(!c)f.push(n)};"
        "for(const n of ['MediaStream','MediaStreamTrack','MediaDevices',"
        "'MediaStreamTrackEvent','OverconstrainedError','MediaDeviceInfo',"
        "'InputDeviceInfo'])e(n,typeof globalThis[n]==='function');"
        "const d=navigator.mediaDevices;"
        "e('instance',d instanceof MediaDevices&&d instanceof EventTarget);"
        "e('same',d===navigator.mediaDevices);"
        "e('proto',Object.getOwnPropertyDescriptor(Navigator.prototype,"
        "'mediaDevices').get!==undefined&&!Object.hasOwn(navigator,"
        "'mediaDevices'));"
        "e('tag',String(d)==='[object MediaDevices]');"
        "e('ctor',(()=>{try{new MediaDevices();return false}catch(x){"
        "return x instanceof TypeError}})());"
        "e('track ctor',(()=>{try{new MediaStreamTrack();return false}"
        "catch(x){return x instanceof TypeError}})());"
        "e('empty stream',new MediaStream().getTracks().length===0"
        "&&new MediaStream().active===false);"
        "e('supported',d.getSupportedConstraints().sampleRate===true"
        "&&d.getSupportedConstraints().channelCount===true);"
        "e('over',new OverconstrainedError('x','m') instanceof DOMException"
        "&&new OverconstrainedError('x').name==='OverconstrainedError'"
        "&&new OverconstrainedError('x').constraint==='x');"
        "e('handlers','ondevicechange' in d);"
        "e('gum',typeof d.getUserMedia==='function');"
        "pocSummary=f.length?f.join(','):'OK'"));
    CHECK(seam.prompt_count == 0 && seam.starts == 0);
    CHECK(page_close(&page));

    CHECK(page_open(&page, "http://plain.test/"));
    CHECK(page_run(&page,
        "pocSummary=typeof MediaStream==='undefined'"
        "&&typeof MediaDevices==='undefined'"
        "&&navigator.mediaDevices===undefined?'OK':'EXPOSED'"));
    CHECK(page_close(&page));
    /* Loopback is a secure context. */
    CHECK(page_open(&page, "http://localhost:8080/"));
    CHECK(page_run(&page,
        "pocSummary=typeof navigator.mediaDevices==='object'?'OK':'MISSING'"));
    CHECK(page_close(&page));
    return 0;
}

static int test_capture_and_stop(void)
{
    puts("test: an allowed request gives one live mono track");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/"));
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    CHECK(seam.prompt_count == 1 && seam.starts == 1 && seam.running);
    CHECK(strcmp(seam.prompts[0].host, "mic.test") == 0
          && strcmp(seam.prompts[0].origin, "https://mic.test") == 0
          && !seam.prompts[0].from_frame && seam.prompts[0].can_remember);
    CHECK(tilefinch_media_capture_live()
          && tilefinch_media_capture_indicator());
    CHECK(page_run(&page,
        "const f=[],e=(n,c)=>{if(!c)f.push(n)};"
        "const s=stream,t=s.getAudioTracks()[0];"
        "e('stream',s instanceof MediaStream&&typeof s.id==='string'"
        "&&s.id.length===36);"
        "e('tracks',s.getTracks().length===1&&s.getVideoTracks().length===0"
        "&&s.getTrackById(t.id)===t&&s.getTrackById('x')===null);"
        "e('track',t instanceof MediaStreamTrack&&t.kind==='audio'"
        "&&t.readyState==='live'&&t.enabled===true&&t.muted===false"
        "&&t.label==='PSP microphone'&&t.id!==s.id);"
        "const g=t.getSettings();"
        "e('settings',g.sampleRate===44100&&g.channelCount===1"
        "&&g.sampleSize===16&&g.echoCancellation===false"
        "&&g.deviceId==='default'&&g.latency>0.02&&g.latency<0.03);"
        "const c=t.getCapabilities();"
        "e('caps',c.sampleRate.min===44100&&c.sampleRate.max===44100"
        "&&c.channelCount.max===1);"
        "e('constraints',JSON.stringify(t.getConstraints())==='{}');"
        "t.enabled=false;e('disable',t.enabled===false);t.enabled=true;"
        "e('permission',true);"
        "pocSummary=f.length?f.join(','):'OK'"));
    /* Labels are exposed while capture is live. */
    CHECK(page_run(&page,
        "navigator.mediaDevices.enumerateDevices().then(l=>{"
        "pocSummary=l.length===1&&l[0].kind==='audioinput'"
        "&&l[0].label==='PSP microphone'&&l[0].deviceId==='default'"
        "&&l[0] instanceof InputDeviceInfo?'OK':'BAD '+JSON.stringify(l)})"));
    /* Allow this time covers this page load: no second prompt, one device
       start shared by both tracks. */
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.second")));
    CHECK(seam.prompt_count == 1 && seam.starts == 1);
    CHECK(page_run(&page,
        "const c=stream.getTracks()[0].clone();globalThis.copy=c;"
        "pocSummary=c.readyState==='live'&&c.id!==stream.getTracks()[0].id"
        "&&c.label==='PSP microphone'?'OK':'BAD'"));
    /* Stopping every track releases the device; stop() fires no ended. */
    CHECK(page_run(&page,
        "globalThis.endedCount=0;"
        "for(const t of [...stream.getTracks(),...second.getTracks(),copy])"
        "{t.onended=()=>endedCount++;t.stop();t.stop();}"
        "pocSummary=stream.active===false&&second.active===false"
        "&&copy.readyState==='ended'?'OK':'BAD'"));
    CHECK(seam.stops == 1 && !seam.running && !tilefinch_media_capture_live()
          && !tilefinch_media_capture_indicator());
    CHECK(page_turns(&page));
    CHECK(page_run(&page,
        "pocSummary=endedCount===0?'OK':'ENDED '+endedCount"));
    /* A clone of an ended track is ended, and does not start anything. */
    CHECK(page_run(&page,
        "pocSummary=copy.clone().readyState==='ended'?'OK':'BAD'"));
    CHECK(seam.starts == 1);
    /* Nothing was remembered. */
    CHECK(browser_profile_microphone_site(seam.profile, "https://mic.test/")
          == MEDIA_CAPTURE_PERMISSION_PROMPT);
    CHECK(page_close(&page));
    /* A new page load asks again. */
    CHECK(page_open(&page, "https://mic.test/"));
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    CHECK(seam.prompt_count == 2 && seam.starts == 2);
    /* Closing the page ends capture. */
    CHECK(page_close(&page));
    CHECK(seam.stops == 2 && !tilefinch_media_capture_live());
    return 0;
}

static int test_constraints_and_errors(void)
{
    puts("test: refused requests reject with the specified errors");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/"));
    CHECK(page_run(&page,
        "const d=navigator.mediaDevices,f=[];"
        "const name=p=>p.then(()=>'resolved',x=>x.name);"
        "Promise.all([name(d.getUserMedia()),name(d.getUserMedia({})),"
        "name(d.getUserMedia({audio:false,video:false})),"
        "name(d.getUserMedia({video:true})),"
        "name(d.getUserMedia({audio:true,video:{width:320}})),"
        "name(d.getUserMedia({audio:{sampleRate:{exact:48000}}})),"
        "d.getUserMedia({audio:{channelCount:{min:2}}}).catch(x=>x),"
        "name(d.getUserMedia.call({},{audio:true}))]).then(r=>{"
        "const o=r[6];"
        "pocSummary=r[0]==='TypeError'&&r[1]==='TypeError'&&r[2]==='TypeError'"
        "&&r[3]==='NotFoundError'&&r[4]==='NotFoundError'"
        "&&r[5]==='OverconstrainedError'"
        "&&o instanceof OverconstrainedError&&o.constraint==='channelCount'"
        "&&r[7]==='TypeError'?'OK':'BAD '+r.join()})"));
    /* None of those reached the prompt or the device. */
    CHECK(seam.prompt_count == 0 && seam.starts == 0);
    /* Ideal values the input cannot meet are not errors. */
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    CHECK(page_run(&page,
        "navigator.mediaDevices.getUserMedia({audio:{sampleRate:48000,"
        "echoCancellation:true}}).then(s=>{const t=s.getTracks()[0];"
        "const ok=t.getSettings().sampleRate===44100"
        "&&t.getConstraints().sampleRate===48000;"
        "t.applyConstraints({sampleRate:{exact:8000}}).then(()=>"
        "pocSummary='APPLIED',x=>{t.stop();pocSummary=ok"
        "&&x.name==='OverconstrainedError'&&x.constraint==='sampleRate'"
        "?'OK':'BAD '+x.name})},x=>pocSummary='REJECTED '+x.name)"));
    CHECK(seam.starts == 1 && seam.stops == 1);
    CHECK(page_close(&page));

    /* No microphone hardware: NotFoundError, and no prompt. */
    seam_install();
    seam.available = false;
    CHECK(page_open(&page, "https://mic.test/"));
    CHECK(page_run(&page, EXPECT_REJECTION("NotFoundError")));
    CHECK(page_run(&page,
        "navigator.mediaDevices.enumerateDevices().then(l=>"
        "pocSummary=l.length===0?'OK':'BAD')"));
    CHECK(seam.prompt_count == 0);
    CHECK(page_close(&page));
    /* Granted, but the input is busy (voice typing): NotReadableError. */
    seam_install();
    seam.start_verdict = MEDIA_CAPTURE_BUSY;
    CHECK(page_open(&page, "https://mic.test/"));
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_REJECTION("NotReadableError")));
    CHECK(seam.prompt_count == 1 && !tilefinch_media_capture_live());
    seam.start_verdict = MEDIA_CAPTURE_NO_DEVICE;
    CHECK(page_run(&page, EXPECT_REJECTION("NotFoundError")));
    seam.start_verdict = MEDIA_CAPTURE_FAILED;
    CHECK(page_run(&page, EXPECT_REJECTION("NotReadableError")));
    /* The grant held: one prompt in all. */
    CHECK(seam.prompt_count == 1);
    CHECK(page_close(&page));
    return 0;
}

static int test_prompt_policy(void)
{
    puts("test: a hidden page never prompts; dismissals are limited");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/"));
    CHECK(script_runtime_set_page_visibility(page.runtime, false));
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == 0);
    CHECK(script_runtime_set_page_visibility(page.runtime, true));
    /* A dismissal is NotAllowedError and remembers nothing. */
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == 1);
    CHECK(browser_profile_microphone_site(seam.profile, "https://mic.test/")
          == MEDIA_CAPTURE_PERMISSION_PROMPT);
    /* Asking again without a new activation does not reopen it. */
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == 1);
    script_runtime_notify_user_activation(page.runtime);
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == 2);
    script_runtime_notify_user_activation(page.runtime);
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == 3);
    /* After MEDIA_CAPTURE_DISMISS_LIMIT, never again this page load. */
    script_runtime_notify_user_activation(page.runtime);
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == MEDIA_CAPTURE_DISMISS_LIMIT);
    CHECK(page_close(&page));
    /* A frontend that cannot show the prompt is a refusal, not a grant. */
    seam_install();
    seam.prompt_shown = false;
    CHECK(page_open(&page, "https://mic.test/"));
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == 1 && seam.starts == 0);
    CHECK(page_close(&page));
    /* The prompt cannot be reopened from inside itself: the seam's answer
       does not run page script, but a nested request is refused anyway by
       the per-realm guard; covered by the frontend's own modal. */
    return 0;
}

static int test_remembered_decisions(void)
{
    puts("test: Always allow and Block are remembered per origin");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/path"));
    CHECK(page_run(&page,
        "navigator.permissions.query({name:'microphone'}).then(p=>{"
        "globalThis.perm=p;globalThis.changes=[];"
        "p.onchange=()=>changes.push(p.state);"
        "pocSummary=p.state==='prompt'?'OK':'BAD '+p.state})"));
    const MediaCapturePromptAnswer always[] = {
        MEDIA_CAPTURE_PROMPT_ALLOW_ALWAYS};
    seam_answers(always, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    CHECK(browser_profile_microphone_site(seam.profile, "https://mic.test/")
          == MEDIA_CAPTURE_PERMISSION_GRANTED);
    CHECK(page_turns(&page));
    CHECK(page_run(&page,
        "pocSummary=perm.state==='granted'&&changes.join()==='granted'"
        "?'OK':'BAD '+perm.state+' '+changes.join()"));
    CHECK(page_run(&page,
        "stream.getTracks()[0].stop();pocSummary='OK'"));
    /* Labels stay exposed once granted. */
    CHECK(page_run(&page,
        "navigator.mediaDevices.enumerateDevices().then(l=>"
        "pocSummary=l[0].label==='PSP microphone'?'OK':'HIDDEN')"));
    CHECK(page_close(&page));
    /* Another page of the origin starts without asking; another origin
       asks. */
    CHECK(page_open(&page, "https://mic.test/other"));
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    CHECK(seam.prompt_count == 1 && seam.starts == 2);
    CHECK(page_close(&page));
    CHECK(page_open(&page, "https://elsewhere.test/"));
    CHECK(page_run(&page,
        "navigator.mediaDevices.enumerateDevices().then(l=>"
        "pocSummary=l.length===1&&l[0].label===''&&l[0].deviceId===''"
        "&&l[0].groupId===''?'OK':'EXPOSED')"));
    const MediaCapturePromptAnswer block[] = {MEDIA_CAPTURE_PROMPT_BLOCK};
    seam_answers(block, 1);
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == 2);
    /* Blocked: refused without asking, even after a new activation. */
    script_runtime_notify_user_activation(page.runtime);
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == 2);
    CHECK(page_run(&page,
        "navigator.permissions.query({name:'microphone'}).then(p=>"
        "pocSummary=p.state==='denied'?'OK':'BAD '+p.state)"));
    CHECK(page_close(&page));
    CHECK(browser_profile_microphone_site(seam.profile,
                                          "https://elsewhere.test/x")
          == MEDIA_CAPTURE_PERMISSION_DENIED);

    puts("test: decisions persist, and Reset permissions forgets them");
    static const char path[] = "media-capture-profile.cfg";
    CHECK(browser_profile_save(seam.profile, path));
    BrowserProfile *loaded = browser_profile_create(&profile_budget);
    CHECK(loaded != NULL && browser_profile_load(loaded, path));
    CHECK(browser_profile_microphone_site(loaded, "https://mic.test/")
          == MEDIA_CAPTURE_PERMISSION_GRANTED);
    CHECK(browser_profile_microphone_site(loaded, "https://elsewhere.test/")
          == MEDIA_CAPTURE_PERMISSION_DENIED);
    /* Keyed by origin: scheme and port matter. */
    CHECK(browser_profile_microphone_site(loaded, "https://mic.test:8443/")
          == MEDIA_CAPTURE_PERMISSION_PROMPT);
    CHECK(browser_profile_microphone_site(loaded, "http://mic.test/")
          == MEDIA_CAPTURE_PERMISSION_PROMPT);
    /* Only the site and the decision are written. */
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    char line[512];
    unsigned records = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "MIC\t", 4) != 0) continue;
        records++;
        CHECK(strstr(line, "mic.test") != NULL
              || strstr(line, "elsewhere.test") != NULL);
        CHECK(strstr(line, "path") == NULL && strstr(line, "other") == NULL);
    }
    fclose(file);
    CHECK(records == 2);
    CHECK(browser_profile_reset_site_permissions(loaded,
                                                 "https://mic.test/page"));
    CHECK(browser_profile_microphone_site(loaded, "https://mic.test/")
          == MEDIA_CAPTURE_PERMISSION_PROMPT);
    CHECK(browser_profile_microphone_site(loaded, "https://elsewhere.test/")
          == MEDIA_CAPTURE_PERMISSION_DENIED);
    /* Per-site removal (the Site controls row back to Ask). */
    CHECK(browser_profile_set_microphone_site(
        loaded, "https://elsewhere.test/", MEDIA_CAPTURE_PERMISSION_PROMPT));
    CHECK(browser_profile_microphone_site(loaded, "https://elsewhere.test/")
          == MEDIA_CAPTURE_PERMISSION_PROMPT);
    /* The table is bounded; a full table offers only Allow this time. */
    char url[64];
    for (unsigned at = 0; at < 64u; at++) {
        snprintf(url, sizeof(url), "https://site%u.test/", at);
        if (!browser_profile_set_microphone_site(
                loaded, url, MEDIA_CAPTURE_PERMISSION_GRANTED)) break;
    }
    CHECK(!browser_profile_microphone_site_room(loaded, "https://new.test/"));
    CHECK(browser_profile_microphone_site_room(loaded, "https://site0.test/"));
    browser_profile_destroy(loaded);
    (void) remove(path);

    BrowserProfile *full = seam.profile;
    for (unsigned at = 0; at < 64u; at++) {
        snprintf(url, sizeof(url), "https://site%u.test/", at);
        if (!browser_profile_set_microphone_site(
                full, url, MEDIA_CAPTURE_PERMISSION_DENIED)) break;
    }
    CHECK(page_open(&page, "https://new.test/"));
    /* Asked without "Always"; a frontend that answers it anyway is taken
       as a dismissal, and nothing is remembered. */
    seam_answers(always, 1);
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(!seam.prompts[seam.prompt_count - 1u].can_remember);
    CHECK(browser_profile_microphone_site(full, "https://new.test/")
          == MEDIA_CAPTURE_PERMISSION_PROMPT);
    script_runtime_notify_user_activation(page.runtime);
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    CHECK(browser_profile_microphone_site(full, "https://new.test/")
          == MEDIA_CAPTURE_PERMISSION_PROMPT);
    CHECK(page_close(&page));
    return 0;
}

static int test_hand_over(void)
{
    puts("test: voice typing gets the microphone only once the input is free");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/"));
    /* Nothing live and the input idle: handed over at once. */
    CHECK(tilefinch_media_capture_hand_over(MEDIA_CAPTURE_END_VOICE));
    CHECK(seam.stops == 0u && seam.free_checks == 1u);
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    CHECK(page_run(&page,
        "globalThis.ended=false;stream.getTracks()[0].onended=()=>ended=true;"
        "pocSummary='OK'"));
    /* The stop leaves a transfer that never ends: page capture still ends
       (its track sees ended), but the input is not handed over. */
    seam.input_busy = true;
    CHECK(!tilefinch_media_capture_hand_over(MEDIA_CAPTURE_END_VOICE));
    CHECK(seam.stops == 1u && !seam.running && !tilefinch_media_capture_live()
          && !tilefinch_media_capture_indicator());
    CHECK(page_turns(&page));
    CHECK(page_run(&page, "pocSummary=ended?'OK':'LIVE'"));
    /* Asking again stays a no, without stopping anything twice. */
    CHECK(!tilefinch_media_capture_hand_over(MEDIA_CAPTURE_END_VOICE));
    CHECK(seam.stops == 1u);
    /* The transfer ends: free. */
    seam.input_busy = false;
    CHECK(tilefinch_media_capture_hand_over(MEDIA_CAPTURE_END_VOICE));
    CHECK(page_close(&page));
    return 0;
}

static int test_browser_ends_capture(void)
{
    puts("test: the browser's Stop ends every track with an ended event");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/"));
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    CHECK(page_run(&page,
        "globalThis.events=[];const t=stream.getTracks()[0];"
        "t.addEventListener('ended',e=>events.push(e.type+':'+t.readyState"
        "+':'+e.isTrusted));t.onended=()=>events.push('handler');"
        "globalThis.clone=t.clone();clone.onended=()=>events.push('clone');"
        "pocSummary='OK'"));
    tilefinch_media_capture_stop(MEDIA_CAPTURE_END_USER);
    CHECK(seam.stops == 1 && !tilefinch_media_capture_live());
    CHECK(script_runtime_has_pending_author_work(page.runtime));
    CHECK(page_turns(&page));
    CHECK(page_run(&page,
        "pocSummary=events.join()==='ended:ended:true,handler,clone'"
        "&&stream.active===false&&clone.readyState==='ended'"
        "?'OK':'BAD '+events.join()"));
    /* Stopping an ended track is a no-op. */
    CHECK(page_run(&page,
        "stream.getTracks()[0].stop();clone.stop();pocSummary='OK'"));
    CHECK(seam.stops == 1);
    /* The grant of this load still holds: the page may start again. */
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.again")));
    CHECK(seam.prompt_count == 1 && seam.starts == 2);

    puts("test: sleep, voice typing and native media end capture");
    static const MediaCaptureEndReason reasons[] = {
        MEDIA_CAPTURE_END_SUSPEND, MEDIA_CAPTURE_END_VOICE,
        MEDIA_CAPTURE_END_MEDIA, MEDIA_CAPTURE_END_DEVICE};
    for (size_t at = 0; at < sizeof(reasons) / sizeof(reasons[0]); at++) {
        if (at != 0) CHECK(page_run(&page, EXPECT_STREAM("globalThis.again")));
        CHECK(page_run(&page,
            "globalThis.ended=false;again.getTracks()[0].onended="
            "()=>ended=true;pocSummary='OK'"));
        tilefinch_media_capture_stop(reasons[at]);
        CHECK(!tilefinch_media_capture_live()
              && !tilefinch_media_capture_indicator());
        CHECK(page_turns(&page));
        CHECK(page_run(&page,
            "pocSummary=ended&&!again.active?'OK':'LIVE'"));
    }
    CHECK(seam.stops == 1u + 4u);

    puts("test: tracks forgotten by a request before ended still end");
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stale")));
    CHECK(page_run(&page,
        "globalThis.staleEnded=0;stale.getTracks()[0].onended="
        "()=>staleEnded++;pocSummary='OK'"));
    tilefinch_media_capture_stop(MEDIA_CAPTURE_END_USER);
    /* A new request runs before the realm's next task boundary. */
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.fresh")));
    CHECK(page_turns(&page));
    CHECK(page_run(&page,
        "const old=stale.getTracks()[0];"
        "pocSummary=staleEnded===1&&old.readyState==='ended'&&!stale.active"
        "&&fresh.active?'OK':'BAD '+[staleEnded,old.readyState,fresh.active]"));
    CHECK(page_run(&page, "fresh.getTracks()[0].stop();pocSummary='OK'"));
    CHECK(seam.stops == 1u + 4u + 2u);

    puts("test: revoking a permission is seen at the next task");
    CHECK(page_run(&page,
        "navigator.permissions.query({name:'microphone'}).then(p=>{"
        "globalThis.perm=p;globalThis.changes=[];"
        "p.addEventListener('change',()=>changes.push(p.state));"
        "pocSummary=p.state==='granted'?'OK':'BAD '+p.state})"));
    /* The user sets the site to Block in Site controls. */
    CHECK(browser_profile_set_microphone_site(
        seam.profile, "https://mic.test/", MEDIA_CAPTURE_PERMISSION_DENIED));
    tilefinch_media_capture_permissions_changed();
    CHECK(script_runtime_has_pending_author_work(page.runtime));
    CHECK(page_turns(&page));
    CHECK(page_run(&page,
        "pocSummary=perm.state==='denied'&&changes.join()==='denied'"
        "?'OK':'BAD '+perm.state+' '+changes.join()"));
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    /* Back to Ask: the load's "Allow this time" was forgotten too. */
    CHECK(browser_profile_set_microphone_site(
        seam.profile, "https://mic.test/", MEDIA_CAPTURE_PERMISSION_PROMPT));
    tilefinch_media_capture_permissions_changed();
    CHECK(page_turns(&page));
    CHECK(page_run(&page,
        "pocSummary=perm.state==='prompt'&&changes.join()==='denied,prompt'"
        "?'OK':'BAD '+perm.state+' '+changes.join()"));
    size_t prompts = seam.prompt_count;
    script_runtime_notify_user_activation(page.runtime);
    CHECK(page_run(&page, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == prompts + 1u);
    CHECK(page_close(&page));

    puts("test: a replaced Event constructor never builds the trusted ended");
    CHECK(profile_fresh());
    CapturePage replaced;
    CHECK(page_open(&replaced, "https://mic.test/"));
    seam_answers(once, 1);
    /* The page replaces Event before it first names a capture interface,
       so the lazily loaded module runs after the replacement. The wrapper
       keeps Event.prototype, as wrapping polyfills do, so events still
       dispatch. */
    CHECK(page_run(&replaced,
        "globalThis.made=0;const NativeEvent=Event;"
        "globalThis.Event=function(type,init){made++;"
        "return new NativeEvent(type,init)};"
        "Event.prototype=NativeEvent.prototype;pocSummary='OK'"));
    CHECK(page_run(&replaced, EXPECT_STREAM("globalThis.stream")));
    CHECK(page_run(&replaced,
        "globalThis.seen='';stream.getTracks()[0].onended="
        "e=>seen=e.type+':'+e.isTrusted;pocSummary='OK'"));
    tilefinch_media_capture_stop(MEDIA_CAPTURE_END_USER);
    CHECK(page_turns(&replaced));
    CHECK(page_run(&replaced,
        "pocSummary=made===0&&seen==='ended:true'?'OK':'BAD '+[made,seen]"));
    CHECK(page_close(&replaced));
    return 0;
}

static int test_realm_limits(void)
{
    puts("test: a realm holds a bounded number of tracks");
    seam_install();
    CHECK(profile_fresh());
    CHECK(browser_profile_set_microphone_site(
        seam.profile, "https://mic.test/", MEDIA_CAPTURE_PERMISSION_GRANTED));
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/"));
    CHECK(page_run(&page,
        "const d=navigator.mediaDevices,all=[];globalThis.held=all;"
        "const one=()=>d.getUserMedia({audio:true}).then(s=>{"
        "all.push(s.getTracks()[0]);return 'ok'},x=>x.name);"
        "Promise.all(Array.from({length:10},one)).then(r=>{"
        "const c=all[0].clone();"
        "pocSummary=r.slice(0,8).every(v=>v==='ok')&&r[8]==='AbortError'"
        "&&r[9]==='AbortError'&&c.readyState==='ended'?'OK':'BAD '+r.join()"
        "})"));
    CHECK(seam.starts == 1 && seam.prompt_count == 0);
    /* Stopping frees a slot. */
    CHECK(page_run(&page,
        "held[0].stop();navigator.mediaDevices.getUserMedia({audio:true})"
        ".then(s=>pocSummary='OK',x=>pocSummary='BAD '+x.name)"));
    CHECK(page_run(&page,
        "for(const t of held)t.stop();pocSummary='OK'"));
    /* One track (the last request's) is still live. */
    CHECK(tilefinch_media_capture_live());
    CHECK(page_close(&page));
    CHECK(!tilefinch_media_capture_live() && seam.stops == 1);
    return 0;
}

/* configure_messaging requires a callback; a recorder that drops. */
static bool drop_message(void *opaque, ScriptRuntime *source,
                         long target_frame_handle, const char *json,
                         const char *target_origin, const char *task_kind,
                         uint64_t task_sequence,
                         const uint8_t *key_bytes, size_t key_length)
{
    (void) opaque; (void) source; (void) target_frame_handle; (void) json;
    (void) target_origin; (void) task_kind; (void) task_sequence;
    (void) key_bytes; (void) key_length;
    return true;
}

static int test_child_realms(void)
{
    puts("test: a frame captures only when the page delegated to it");
    seam_install();
    CHECK(profile_fresh());
    CapturePage top, child;
    CHECK(page_open(&top, "https://mic.test/"));
    CHECK(page_open(&child, "https://widget.test/frame"));
    CHECK(script_runtime_configure_messaging(
        child.runtime, true, false, false, true, drop_message, NULL));
    CHECK(!script_runtime_media_capture_allowed(child.runtime));
    CHECK(page_run(&child, EXPECT_REJECTION("NotAllowedError")));
    CHECK(seam.prompt_count == 0);
    CHECK(page_run(&child,
        "navigator.permissions.query({name:'microphone'}).then(p=>"
        "pocSummary=p.state==='denied'?'OK':'BAD '+p.state)"));
    /* Delegated: the prompt names the top-level site, and the decision is
       the top-level origin's. */
    script_runtime_set_media_capture_parent(child.runtime, top.runtime, true);
    CHECK(script_runtime_media_capture_allowed(child.runtime));
    const MediaCapturePromptAnswer always[] = {
        MEDIA_CAPTURE_PROMPT_ALLOW_ALWAYS};
    seam_answers(always, 1);
    CHECK(page_run(&child, EXPECT_STREAM("globalThis.stream")));
    CHECK(seam.prompt_count == 1
          && strcmp(seam.prompts[0].host, "mic.test") == 0
          && strcmp(seam.prompts[0].origin, "https://mic.test") == 0
          && seam.prompts[0].from_frame);
    CHECK(browser_profile_microphone_site(seam.profile, "https://mic.test/")
          == MEDIA_CAPTURE_PERMISSION_GRANTED);
    CHECK(browser_profile_microphone_site(seam.profile,
                                          "https://widget.test/")
          == MEDIA_CAPTURE_PERMISSION_PROMPT);
    /* The frame's tracks end when the frame goes away. */
    script_runtime_set_media_capture_parent(child.runtime, NULL, false);
    CHECK(page_close(&child));
    CHECK(!tilefinch_media_capture_live() && seam.stops == 1);
    CHECK(page_close(&top));

    /* A frame of an insecure page is not a secure context. */
    CHECK(page_open(&top, "http://plain.test/"));
    CHECK(page_open(&child, "https://widget.test/frame"));
    CHECK(script_runtime_configure_messaging(
        child.runtime, true, false, false, true, drop_message, NULL));
    script_runtime_set_media_capture_parent(child.runtime, top.runtime, true);
    CHECK(!script_runtime_media_capture_allowed(child.runtime));
    CHECK(page_run(&child, EXPECT_REJECTION("SecurityError")));
    CHECK(seam.prompt_count == 1);
    script_runtime_set_media_capture_parent(child.runtime, NULL, false);
    CHECK(page_close(&child));
    CHECK(page_close(&top));
    return 0;
}

/* ---- frames loaded by navigation -------------------------------------- */

static bool write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fwrite(text, 1, strlen(text), file) == strlen(text);
    return fclose(file) == 0 && ok;
}

static bool write_fixture(const char *directory)
{
    static const struct {
        const char *url;
        const char *body;
        const char *effective; /* a redirect's final URL */
    } pages[] = {
        {"https://mic.test/top",
         "<!doctype html><title>Top</title><p>Top document.</p>"
         "<iframe src=\"https://widget.test/plain\"></iframe>"
         "<iframe allow=\"microphone\" src=\"https://widget.test/allowed\">"
         "</iframe>"
         "<iframe allow=\"microphone 'none'\" src=\"https://mic.test/none\">"
         "</iframe>"
         "<iframe src=\"https://mic.test/same\"></iframe>", NULL},
        {"https://widget.test/plain", NULL, NULL},
        {"https://widget.test/allowed", NULL, NULL},
        {"https://mic.test/none", NULL, NULL},
        {"https://mic.test/same", NULL, NULL},
        /* 'src' (and a bare "microphone") follow the container's src
           origin, not where a redirect lands; '*' delegates anywhere. */
        {"https://mic.test/redirected",
         "<!doctype html><title>Top</title><p>Top document.</p>"
         "<iframe allow=\"microphone\" src=\"https://widget.test/hop\">"
         "</iframe>"
         "<iframe allow=\"microphone 'src'\" src=\"https://widget.test/stay\">"
         "</iframe>"
         "<iframe allow=\"microphone *\" src=\"https://widget.test/any\">"
         "</iframe>", NULL},
        {"https://widget.test/hop", NULL, "https://elsewhere.test/landed"},
        {"https://widget.test/stay", NULL, "https://widget.test/stayed"},
        {"https://widget.test/any", NULL, "https://elsewhere.test/any"},
    };
    if (mkdir(directory, 0755) != 0 && errno != EEXIST) return false;
    char path[512], meta[1024], body[512];
    size_t count = sizeof(pages) / sizeof(pages[0]);
    for (size_t at = 0; at < count; at++) {
        snprintf(body, sizeof(body), "%s", pages[at].body != NULL
                 ? pages[at].body
                 : "<!doctype html><title>Child</title><p>Child.</p>"
                   "<script>document.title='child'</script>");
        snprintf(meta, sizeof(meta),
                 "psp-http-trace=1\nmethod=GET\nurl=%s\nsuccess=1\n"
                 "async-delay-pumps=1\nexternal-cancel=0\n"
                 "transport-timeout=0\nerror=\nstatus=200\nlength=%zu\n"
                 "effective-url=%s\n"
                 "content-type=text/html; charset=utf-8\nserver=fixture\n"
                 "set-cookie-count=0\nresponse-header-count=1\n"
                 "response-header-0=content-type: text/html; charset=utf-8\n",
                 pages[at].url, strlen(body),
                 pages[at].effective != NULL ? pages[at].effective
                                             : pages[at].url);
        snprintf(path, sizeof(path), "%s/%04zu.meta", directory, at);
        if (!write_text(path, meta)) return false;
        snprintf(path, sizeof(path), "%s/%04zu.body", directory, at);
        if (!write_text(path, body)) return false;
    }
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    snprintf(meta, sizeof(meta),
             "psp-http-trace-clock=1\norigin-ms=1700000000000\n"
             "capture-complete=yes\nrecord-count=%zu\n", count);
    return write_text(path, meta);
}

static int navigation_frames_case(const char *directory, const char *top,
                                  size_t frames, const char *const *allowed)
{
    seam_install();
    CHECK(profile_fresh());
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(directory, error,
                                                  sizeof(error)));
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 32u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    BrowserEngine *engine = browser_engine_create(&config, error,
                                                  sizeof(error));
    if (engine == NULL) fprintf(stderr, "engine: %s\n", error);
    CHECK(engine != NULL);
    browser_engine_set_defer_script_navigation(engine, true);
    CHECK(browser_engine_begin_navigation_url(
        engine, top, 1u * MIB, 5000, true));
    BrowserNavigationJobStatus status =
        browser_engine_navigation_status(engine);
    for (unsigned pump = 0;
         pump < 512 && status == BROWSER_NAVIGATION_JOB_PENDING; pump++)
        status = browser_engine_pump_navigation(engine, NULL);
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    NavigationSession *navigation = browser_engine_navigation(engine);
    for (unsigned tick = 0; tick < 32u; tick++)
        (void) browser_engine_advance_runtime(engine, 16, 4, NULL);
    CHECK(navigation->page.frame_count == frames);
    CHECK(script_runtime_media_capture_allowed(navigation->page.runtime));
    size_t loaded = 0;
    for (size_t at = 0; at < navigation->page.frame_count; at++) {
        NavigationFrame *frame = &navigation->page.frames[at];
        if (!frame->loaded || frame->runtime == NULL) continue;
        loaded++;
        bool expected = false;
        for (const char *const *url = allowed; *url != NULL; url++)
            expected = expected || strcmp(frame->url, *url) == 0;
        if (script_runtime_media_capture_allowed(frame->runtime)
            != expected) {
            fprintf(stderr, "frame %s: allowed=%d\n", frame->url,
                    !expected);
            return 1;
        }
    }
    CHECK(loaded == frames);
    CHECK(seam.prompt_count == 0);
    browser_engine_destroy(engine);
    fetch_trace_end();
    tilefinch_platform_set_services(NULL);
    return 0;
}

static int test_navigation_frames(void)
{
    puts("test: navigation applies the iframe allow attribute");
    static const char directory[] = "media-capture-frames-fixture";
    CHECK(write_fixture(directory));
    static const char *const direct[] = {
        "https://widget.test/allowed", "https://mic.test/same", NULL};
    if (navigation_frames_case(directory, "https://mic.test/top", 4u, direct))
        return 1;
    static const char *const redirected[] = {
        "https://widget.test/stayed", "https://elsewhere.test/any", NULL};
    return navigation_frames_case(directory, "https://mic.test/redirected", 3u,
                                  redirected);
}

/* ---- Web Audio ------------------------------------------------------- */

static bool mix_block(CapturePage *page, int16_t *pcm)
{
    TilefinchGameAudio *audio = page->runtime->game_audio;
    return audio != NULL
        && tilefinch_game_audio_mix(audio, pcm, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES);
}

static int test_web_audio_microphone(void)
{
    puts("test: a live track feeds createMediaStreamSource, heard only in headphones");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open_audio(&page, "https://mic.test/"));
    /* No live track: refused before anything plays. */
    CHECK(page_run(&page,
        "globalThis.c=new AudioContext();const n=[];"
        "const t=f=>{try{f();n.push('ok')}catch(e){n.push(e.name)}};"
        "t(()=>c.createMediaStreamSource());"
        "t(()=>c.createMediaStreamSource({getAudioTracks(){return[1]}}));"
        "t(()=>new MediaStreamAudioSourceNode(c,{}));"
        "t(()=>c.createMediaStreamSource(new MediaStream()));"
        "pocSummary=n.join()==='TypeError,TypeError,TypeError,InvalidStateError'"
        "?'OK':n.join()"));
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    seam.level = 0.25;
    CHECK(page_run(&page,
        "globalThis.src=c.createMediaStreamSource(stream);"
        "globalThis.an=c.createAnalyser();an.fftSize=1024;src.connect(an);"
        "pocSummary=src instanceof MediaStreamAudioSourceNode&&src.mediaStream===stream"
        "&&an.frequencyBinCount===512&&an.fftSize===1024?'OK':'BAD'"));
    TilefinchGameAudio *audio = page.runtime->game_audio;
    CHECK(audio != NULL && tilefinch_game_audio_resume(audio));
    int16_t pcm[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    /* The pump runs at each advance with no script; two reads fill the
       prefill. The analyser hears the microphone; nothing reaches the
       output (not connected, and no headphones). */
    for (int turn = 0; turn < 3; turn++) CHECK(page_turns(&page));
    CHECK(seam.since_reads >= 3u);
    for (int block = 0; block < 4; block++) {
        CHECK(page_turns(&page));
        CHECK(mix_block(&page, pcm));
        for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u; at++)
            CHECK(pcm[at] == 0);
    }
    CHECK(page_run(&page,
        "const f=new Float32Array(1024),b=new Uint8Array(1024);"
        "an.getFloatTimeDomainData(f);an.getByteTimeDomainData(b);"
        "pocSummary=Math.abs(f[1023]-.25)<1e-3&&b[1023]===160?'OK':f[1023]+' '+b[1023]"));
    /* Connected to the destination: still silent through the speaker. */
    CHECK(page_run(&page, "an.connect(c.destination);pocSummary='OK'"));
    CHECK(page_turns(&page));
    CHECK(mix_block(&page, pcm));
    CHECK(pcm[0] == 0 && pcm[100] == 0);
    seam.headphones = true;
    CHECK(page_turns(&page));
    CHECK(mix_block(&page, pcm));
    CHECK(abs(pcm[0] - 8192) <= 1 && abs(pcm[1] - 8192) <= 1);
    /* Byte frequency data: DC lands in bin 0; dB settings apply. */
    CHECK(page_run(&page,
        "an.smoothingTimeConstant=0;const b=new Uint8Array(512),f=new Float32Array(512);"
        "an.getByteFrequencyData(b);an.getFloatFrequencyData(f);"
        "const n=[];const t=g=>{try{g();n.push('ok')}catch(e){n.push(e.name)}};"
        "t(()=>an.fftSize=4096);t(()=>an.fftSize=1000);t(()=>an.minDecibels=-20);"
        "t(()=>an.smoothingTimeConstant=2);t(()=>an.getFloatFrequencyData(b));"
        "t(()=>an.getByteFrequencyData(new Uint8ClampedArray(4)));"
        "t(()=>c.createAnalyser().maxDecibels=-200);"
        "pocSummary=b[0]===255&&b[100]===0&&f[0]>-30&&f[100]<-100"
        "&&n.join()==='NotSupportedError,IndexSizeError,IndexSizeError,"
        "IndexSizeError,TypeError,TypeError,IndexSizeError'?'OK':b[0]+' '+f[0]+' '+n.join()"));
    /* Two analysers at most. */
    CHECK(page_run(&page,
        "globalThis.second=c.createAnalyser();let r='';"
        "try{c.createAnalyser()}catch(e){r=e.name}"
        "pocSummary=r==='QuotaExceededError'?'OK':'BAD '+r"));
    /* Disconnecting silences the output; the analyser then hears nothing. */
    CHECK(page_run(&page, "src.disconnect();pocSummary='OK'"));
    CHECK(page_turns(&page));
    CHECK(mix_block(&page, pcm));
    CHECK(pcm[0] == 0 && pcm[1000] == 0);
    /* Ending the track stops the source, without an ended event. */
    CHECK(page_run(&page,
        "globalThis.ends=0;src.addEventListener('ended',()=>ends++);"
        "stream.getTracks()[0].stop();pocSummary='OK'"));
    for (int turn = 0; turn < 3; turn++) {
        CHECK(page_turns(&page));
        CHECK(mix_block(&page, pcm));
    }
    unsigned counts[5] = {0};
    CHECK(js_rt_audio_slot_test_snapshot(page.runtime, counts) && counts[4] == 0u);
    CHECK(page_run(&page, "pocSummary=ends===0?'OK':'ENDED'"));
    /* A new source needs a live track again. */
    CHECK(page_run(&page,
        "let r='';try{c.createMediaStreamSource(stream)}catch(e){r=e.name}"
        "pocSummary=r==='InvalidStateError'?'OK':'BAD '+r"));
    CHECK(page_close(&page));
    return 0;
}

/* Whether every sample of one mixed block is silent, or (LOUD) near the
   seam's 0.25 level on both channels. */
static bool mix_block_is(CapturePage *page, bool loud)
{
    int16_t pcm[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    if (!page_turns(page) || !mix_block(page, pcm)) return false;
    if (loud) return abs(pcm[0] - 8192) <= 1 && abs(pcm[1] - 8192) <= 1;
    for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u; at++)
        if (pcm[at] != 0) {
            fprintf(stderr, "sample %zu is %d, not silence\n", at, pcm[at]);
            return false;
        }
    return true;
}

static int test_stream_source_follows_its_track(void)
{
    puts("test: a stream source is silent while its own track is disabled or stopped");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open_audio(&page, "https://mic.test/"));
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    seam.level = 0.25;
    seam.headphones = true;
    CHECK(page_run(&page,
        "globalThis.c=new AudioContext();globalThis.t=stream.getAudioTracks()[0];"
        "globalThis.src=c.createMediaStreamSource(stream);"
        "globalThis.an=c.createAnalyser();an.fftSize=1024;"
        "src.connect(an);an.connect(c.destination);pocSummary='OK'"));
    TilefinchGameAudio *audio = page.runtime->game_audio;
    CHECK(audio != NULL && tilefinch_game_audio_resume(audio));
    for (int turn = 0; turn < 3; turn++) CHECK(page_turns(&page));
    CHECK(mix_block_is(&page, true));
    /* enabled = false: silence to the output and to the analyser, while the
       microphone stays on for the track. */
    CHECK(page_run(&page, "t.enabled=false;pocSummary='OK'"));
    for (int block = 0; block < 3; block++) CHECK(mix_block_is(&page, false));
    CHECK(seam.running && tilefinch_media_capture_live());
    CHECK(page_run(&page,
        "const f=new Float32Array(1024);an.getFloatTimeDomainData(f);"
        "pocSummary=f[1023]===0&&f[512]===0?'OK':'HEARD '+f[1023]"));
    /* enabled = true again: heard again. */
    CHECK(page_run(&page, "t.enabled=true;pocSummary='OK'"));
    CHECK(mix_block_is(&page, true));
    /* Stopping the source's track ends it even while a clone keeps the
       microphone on. */
    CHECK(page_run(&page,
        "globalThis.copy=t.clone();t.stop();"
        "pocSummary=copy.readyState==='live'&&t.readyState==='ended'?'OK':'BAD'"));
    for (int block = 0; block < 3; block++) CHECK(mix_block_is(&page, false));
    CHECK(seam.running && tilefinch_media_capture_live());
    /* Re-enabling the ended track brings nothing back. */
    CHECK(page_run(&page, "t.enabled=false;t.enabled=true;pocSummary='OK'"));
    CHECK(mix_block_is(&page, false));
    /* The clone's own source hears; disabling the clone silences it. A
       source made from a disabled track starts silent. */
    CHECK(page_run(&page,
        "src.disconnect();globalThis.mine=new MediaStream([copy]);"
        "globalThis.src2=c.createMediaStreamSource(mine);"
        "src2.connect(c.destination);pocSummary='OK'"));
    for (int turn = 0; turn < 3; turn++) CHECK(page_turns(&page));
    CHECK(mix_block_is(&page, true));
    CHECK(page_run(&page, "copy.enabled=false;pocSummary='OK'"));
    CHECK(mix_block_is(&page, false));
    CHECK(page_run(&page,
        "src2.disconnect();globalThis.src3=c.createMediaStreamSource(mine);"
        "src3.connect(c.destination);pocSummary='OK'"));
    for (int turn = 0; turn < 3; turn++) CHECK(mix_block_is(&page, false));
    CHECK(page_run(&page, "copy.enabled=true;pocSummary='OK'"));
    CHECK(mix_block_is(&page, true));
    /* A source keeps the track it chose: removing that track from its
       stream changes nothing. */
    CHECK(page_run(&page, "mine.removeTrack(copy);pocSummary='OK'"));
    CHECK(mix_block_is(&page, true));
    CHECK(page_run(&page, "copy.stop();pocSummary='OK'"));
    CHECK(mix_block_is(&page, false));
    CHECK(!seam.running && !tilefinch_media_capture_live());
    CHECK(page_close(&page));
    return 0;
}

/* One page turn (1024 new microphone samples) mixed as two blocks, so the
   stream voice keeps up with the input. False on any non-zero sample when
   SILENT; *PEAK is the block pair's largest magnitude. */
static bool mix_turn(CapturePage *page, bool silent, int *peak)
{
    int16_t pcm[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    if (!page_turns(page)) return false;
    *peak = 0;
    for (int block = 0; block < 2; block++) {
        if (!mix_block(page, pcm)) return false;
        for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u; at++)
            if (abs(pcm[at]) > *peak) *peak = abs(pcm[at]);
    }
    if (silent && *peak != 0) {
        fprintf(stderr, "peak %d, not silence\n", *peak);
        return false;
    }
    return true;
}

static int test_disabled_track_feeds_silence(void)
{
    puts("test: a disabled track feeds silence into its source's filter");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open_audio(&page, "https://mic.test/"));
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    seam.level = 0.25;
    seam.headphones = true;
    /* A resonant low-pass (100 Hz, Q 20 dB) rings for tens of
       milliseconds after its input changes. */
    CHECK(page_run(&page,
        "globalThis.c=new AudioContext();globalThis.t=stream.getAudioTracks()[0];"
        "globalThis.src=c.createMediaStreamSource(stream);"
        "globalThis.lp=c.createBiquadFilter();lp.frequency.value=100;lp.Q.value=20;"
        "src.connect(lp);lp.connect(c.destination);pocSummary='OK'"));
    TilefinchGameAudio *audio = page.runtime->game_audio;
    CHECK(audio != NULL && tilefinch_game_audio_resume(audio));
    int peak = 0;
    for (int turn = 0; turn < 40; turn++) CHECK(mix_turn(&page, false, &peak));
    CHECK(peak > 4000);
    /* Disabled for about a second while the microphone still hears the
       level: the filter receives silence, so its ringing dies away. */
    CHECK(page_run(&page, "t.enabled=false;pocSummary='OK'"));
    for (int turn = 0; turn < 40; turn++) CHECK(mix_turn(&page, false, &peak));
    CHECK(peak == 0);
    /* The room goes quiet just before the page turns the track back on:
       nothing may come out, as with a source that was never disabled. */
    seam.level = 0;
    for (int turn = 0; turn < 2; turn++) CHECK(mix_turn(&page, true, &peak));
    CHECK(page_run(&page, "t.enabled=true;pocSummary='OK'"));
    for (int turn = 0; turn < 4; turn++) CHECK(mix_turn(&page, true, &peak));
    /* Heard again once the room is. */
    seam.level = 0.25;
    for (int turn = 0; turn < 40; turn++) CHECK(mix_turn(&page, false, &peak));
    CHECK(peak > 4000);
    CHECK(page_close(&page));
    return 0;
}

/* Replaces crypto.randomUUID so that its Nth call from now throws. */
#define FAIL_ID_CALL(n)                                                     \
    "if(!globalThis.realId)globalThis.realId=crypto.randomUUID;let calls=0;"                    \
    "__tilefinchTestBrowserPatch(crypto,'randomUUID',function(){"           \
    "if(++calls===" #n ")"                                                  \
    "throw new Error('no ids');return realId.call(crypto)});"
#define RESTORE_IDS "__tilefinchTestBrowserPatch(crypto,'randomUUID',realId);"

static int test_failed_construction_releases(void)
{
    puts("test: a track or stream that cannot be built gives the microphone back");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/"));
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    /* The track's id fails, then the stream's: each request rejects and
       the device stops again. */
    static const char *const failing[] = {FAIL_ID_CALL(1), FAIL_ID_CALL(2)};
    for (unsigned at = 0; at < 2u; at++) {
        char probe[1024];
        snprintf(probe, sizeof(probe),
            "%slet p;try{p=navigator.mediaDevices.getUserMedia({audio:true})}"
            "catch(e){p=Promise.reject(new Error('sync '+e.message))}"
            RESTORE_IDS
            "p.then(()=>{pocSummary='RESOLVED'},"
            "e=>{pocSummary=e.message==='no ids'?'OK':'BAD '+e.message})",
            failing[at]);
        CHECK(page_run(&page, probe));
        CHECK(seam.starts == at + 1u && seam.stops == at + 1u
              && !seam.running && !tilefinch_media_capture_live());
        CHECK(page.runtime->media_capture_live_tracks == 0);
    }
    /* A clone that cannot be built releases its share; the original track
       keeps the microphone. */
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    CHECK(seam.running && page.runtime->media_capture_live_tracks == 1);
    CHECK(page_run(&page,
        FAIL_ID_CALL(1)
        "let r='';try{stream.getTracks()[0].clone()}catch(e){r=e.message}"
        RESTORE_IDS
        "pocSummary=r==='no ids'?'OK':'BAD '+r"));
    CHECK(seam.running && page.runtime->media_capture_live_tracks == 1);
    /* stream.clone(): when a later track cannot be cloned, the clones made
       before it are stopped too. */
    CHECK(page_run(&page,
        "stream.addTrack(stream.getTracks()[0].clone());"
        "pocSummary=stream.getTracks().length===2?'OK':'BAD'"));
    CHECK(page.runtime->media_capture_live_tracks == 2);
    CHECK(page_run(&page,
        FAIL_ID_CALL(3)
        "let r='';try{stream.clone()}catch(e){r=e.message}"
        RESTORE_IDS
        "pocSummary=r==='no ids'?'OK':'BAD '+r"));
    CHECK(seam.running && page.runtime->media_capture_live_tracks == 2);
    CHECK(page_run(&page,
        "for(const t of stream.getTracks())t.stop();pocSummary='OK'"));
    CHECK(!seam.running && page.runtime->media_capture_live_tracks == 0);
    CHECK(page_close(&page));
    return 0;
}

static int test_device_failure_ends_capture(void)
{
    puts("test: an input that stops delivering ends capture with an ended event");
    seam_install();
    CHECK(profile_fresh());
    CapturePage page;
    CHECK(page_open(&page, "https://mic.test/"));
    const MediaCapturePromptAnswer once[] = {MEDIA_CAPTURE_PROMPT_ALLOW_ONCE};
    seam_answers(once, 1);
    CHECK(page_run(&page, EXPECT_STREAM("globalThis.stream")));
    CHECK(page_run(&page,
        "globalThis.ends=0;stream.getTracks()[0].onended=e=>{"
        "if(e.isTrusted)ends++};pocSummary='OK'"));
    CHECK(tilefinch_media_capture_live() && tilefinch_media_capture_indicator());
    /* The worker stalls after it started. */
    seam.failed = true;
    CHECK(page_turns(&page));
    CHECK(!tilefinch_media_capture_live() && !tilefinch_media_capture_indicator()
          && seam.stops == 1 && !seam.running);
    CHECK(page_run(&page,
        "pocSummary=ends===1&&stream.active===false"
        "&&stream.getTracks()[0].readyState==='ended'?'OK':'BAD '+ends"));
    CHECK(page.runtime->media_capture_live_tracks == 0);
    seam.failed = false;
    CHECK(page_close(&page));
    return 0;
}

static int test_unconnected_sources(void)
{
    puts("test: a source that does not reach the destination is silent");
    seam_install();
    CapturePage page;
    CHECK(page_open_audio(&page, "https://mic.test/"));
    CHECK(page_run(&page,
        "globalThis.c=new AudioContext();globalThis.an=c.createAnalyser();"
        "globalThis.o=c.createOscillator();"
        "globalThis.g=c.createGain();o.type='square';o.connect(g);o.start();"
        "pocSummary='OK'"));
    TilefinchGameAudio *audio = page.runtime->game_audio;
    CHECK(audio != NULL && tilefinch_game_audio_resume(audio));
    int16_t pcm[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    CHECK(mix_block(&page, pcm));
    for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u; at++)
        CHECK(pcm[at] == 0);
    CHECK(page_run(&page, "g.connect(c.destination);pocSummary='OK'"));
    CHECK(mix_block(&page, pcm));
    CHECK(abs(pcm[100]) > 30000);
    /* An analyser passes the signal on and hears it. */
    CHECK(page_run(&page,
        "g.disconnect();g.connect(an);"
        "an.connect(c.destination);pocSummary='OK'"));
    CHECK(mix_block(&page, pcm));
    CHECK(abs(pcm[100]) > 30000);
    CHECK(page_run(&page,
        "const f=new Float32Array(2048);an.getFloatTimeDomainData(f);"
        "pocSummary=Math.abs(Math.abs(f[2047])-1)<1e-3&&f[0]===0?'OK':f[2047]+' '+f[0]"));
    CHECK(page_run(&page, "o.disconnect();pocSummary='OK'"));
    CHECK(mix_block(&page, pcm));
    CHECK(pcm[100] == 0);
    CHECK(page_close(&page));
    return 0;
}

int main(void)
{
    budget_init(&profile_budget, 4u * MIB);
    budget_init(&page_budget, 64u * MIB);
    if (!budget_install_lexbor(&page_budget)) return 1;
    int failures = 0;
    failures += test_prompt_modal();
    failures += test_frame_policy();
    failures += test_ring();
    failures += test_input_custody();
    failures += test_input_settle();
    failures += test_interfaces();
    failures += test_capture_and_stop();
    failures += test_constraints_and_errors();
    failures += test_prompt_policy();
    failures += test_remembered_decisions();
    failures += test_browser_ends_capture();
    failures += test_hand_over();
    failures += test_device_failure_ends_capture();
    failures += test_realm_limits();
    failures += test_child_realms();
    failures += test_web_audio_microphone();
    failures += test_stream_source_follows_its_track();
    failures += test_disabled_track_feeds_silence();
    failures += test_failed_construction_releases();
    failures += test_unconnected_sources();
    /* The engine installs its own lexbor allocator. */
    (void) budget_uninstall_lexbor(&page_budget);
    failures += test_navigation_frames();
    tilefinch_platform_set_services(NULL);
    if (seam.profile != NULL) browser_profile_destroy(seam.profile);
    if (failures == 0) puts("media capture tests passed");
    return failures == 0 ? 0 : 1;
}
