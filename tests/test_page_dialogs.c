/* window.alert(), confirm() and prompt(): the WebIDL surface, the answers a
   page gets, what the user is shown, and the anti-abuse policy
   (tilefinch/page_dialog.h).

   The host has no dialog UI. The seam below is the platform's page_dialog
   service answering from a script of canned answers, the way the PSP modal
   would after the user pressed a button, and it records every request. A
   fake monotonic clock lets the rate window and the watchdog be tested
   without waiting. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/frame_sandbox.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/page_dialog.h"
#include "tilefinch/platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "PAGE DIALOG CHECK failed at %s:%d: %s\n",          \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

/* ---- the scripted frontend ------------------------------------------- */

typedef struct {
    bool shown;               /* false: the frontend cannot show one now */
    PageDialogOutcome outcome;
    const char *text;         /* prompt: the accepted text, or NULL to keep */
    uint64_t open_ms;         /* how long the user takes to answer */
} ScriptedAnswer;

typedef struct {
    PageDialogKind kind;
    char host[PAGE_DIALOG_HOST_BYTES];
    char message[PAGE_DIALOG_MESSAGE_BYTES];
    char default_value[PAGE_DIALOG_PROMPT_BYTES];
    bool offer_suppress;
} RecordedRequest;

enum { SCRIPT_LIMIT = 32, RECORD_LIMIT = 64 };

static struct {
    ScriptedAnswer answers[SCRIPT_LIMIT];
    size_t answer_count;
    size_t answer_next;
    ScriptedAnswer fallback;
    RecordedRequest requests[RECORD_LIMIT];
    size_t request_count;
    uint64_t now_ns;
    bool fake_clock;
} seam;

static uint64_t seam_time_ns(void *context)
{
    (void) context;
    return seam.now_ns;
}

static uint64_t seam_time_us(void *context)
{
    (void) context;
    return seam.now_ns / 1000u;
}

static bool seam_dialog(void *context,
                        const TilefinchPageDialogRequest *request,
                        TilefinchPageDialogResponse *response)
{
    (void) context;
    if (seam.request_count < RECORD_LIMIT) {
        RecordedRequest *record = &seam.requests[seam.request_count];
        record->kind = request->kind;
        snprintf(record->host, sizeof(record->host), "%s", request->host);
        snprintf(record->message, sizeof(record->message), "%s",
                 request->message);
        snprintf(record->default_value, sizeof(record->default_value), "%s",
                 request->default_value);
        record->offer_suppress = request->offer_suppress;
    }
    seam.request_count++;
    ScriptedAnswer answer = seam.answer_next < seam.answer_count
        ? seam.answers[seam.answer_next++] : seam.fallback;
    seam.now_ns += answer.open_ms * 1000000u;
    if (!answer.shown) return false;
    response->outcome = answer.outcome;
    if (answer.text != NULL && response->text != NULL)
        snprintf(response->text, response->text_capacity, "%s", answer.text);
    return true;
}

static void seam_install(bool fake_clock)
{
    memset(&seam, 0, sizeof(seam));
    seam.fallback = (ScriptedAnswer) {
        .shown = true, .outcome = PAGE_DIALOG_OUTCOME_ACCEPTED
    };
    seam.now_ns = UINT64_C(1000000000000);
    seam.fake_clock = fake_clock;
    TilefinchPlatformServices services = {
        .page_dialog = seam_dialog,
        .monotonic_time_ns = fake_clock ? seam_time_ns : NULL,
        .monotonic_time_us = fake_clock ? seam_time_us : NULL
    };
    tilefinch_platform_set_services(&services);
}

static void seam_answers(const ScriptedAnswer *answers, size_t count)
{
    seam.answer_count = count < SCRIPT_LIMIT ? count : SCRIPT_LIMIT;
    memcpy(seam.answers, answers, seam.answer_count * sizeof(*answers));
    seam.answer_next = 0;
}

static void seam_advance_ms(uint64_t milliseconds)
{
    seam.now_ns += milliseconds * 1000000u;
}

/* ---- a page realm ----------------------------------------------------- */

/* One page budget for the process, as lexbor's allocator is process-wide;
   each page checks it gives back everything it took. Pages close in the
   reverse order they opened. */
static Budget page_budget;

typedef struct {
    size_t baseline;
    PocDocument document;
    ScriptRuntime *runtime;
    ScriptResult result;
} DialogPage;

static bool page_open(DialogPage *page, const char *url)
{
    static const char html[] =
        "<!doctype html><html><head><title>dialogs</title></head>"
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

static bool page_close(DialogPage *page)
{
    script_runtime_destroy(page->runtime);
    document_destroy(&page->document);
    bool clean = page_budget.current == page->baseline;
    if (!clean)
        fprintf(stderr, "page dialog test leaked %zu bytes\n",
                page_budget.current - page->baseline);
    return clean;
}

/* Evaluates SOURCE, which sets pocSummary; true when it says "OK". */
static bool page_run(DialogPage *page, const char *source)
{
    /* A block, so each probe's declarations are its own. */
    static char block[4096];
    if (snprintf(block, sizeof(block), "{%s\n}", source)
        >= (int) sizeof(block)) return false;
    memset(&page->result, 0, sizeof(page->result));
    bool ran = script_runtime_evaluate_diagnostic(
        page->runtime, block, "https://dialogs.test/probe.js",
        &page->result);
    if (!ran || strcmp(page->result.summary, "OK") != 0) {
        PageDialogPolicy policy;
        (void) script_runtime_page_dialog_policy(page->runtime, &policy);
        fprintf(stderr, "probe: ran=%d summary=%s error=%s requests=%zu "
                "shown=%u since_gesture=%u suppressed=%d denied:",
                ran, page->result.summary, page->result.error,
                seam.request_count, policy.shown, policy.since_gesture,
                policy.suppressed);
        for (int at = 1; at < PAGE_DIALOG_VERDICT_COUNT; at++)
            fprintf(stderr, " %u", policy.denied[at]);
        fputc('\n', stderr);
        return false;
    }
    return true;
}

/* A fresh gesture and an empty rate window, so a probe that shows a few
   dialogs is judged on its own. */
static void page_fresh_turn(DialogPage *page)
{
    script_runtime_notify_user_activation(page->runtime);
    seam_advance_ms(PAGE_DIALOG_RATE_WINDOW_MS + 1u);
}

static PageDialogPolicy page_policy(DialogPage *page)
{
    PageDialogPolicy policy;
    (void) script_runtime_page_dialog_policy(page->runtime, &policy);
    return policy;
}

/* ---- tests ------------------------------------------------------------ */

static int test_global_shape(void)
{
    puts("test: alert/confirm/prompt are own Window operations");
    seam_install(true);
    DialogPage page;
    CHECK(page_open(&page, "https://dialogs.test/"));
    CHECK(page_run(&page,
        "const f=[],e=(n,c)=>{if(!c)f.push(n)};"
        "for(const n of ['alert','confirm','prompt']){"
        "const d=Object.getOwnPropertyDescriptor(globalThis,n);"
        "e(n+' descriptor',d&&d.writable&&d.enumerable&&d.configurable"
        "&&typeof d.value==='function');"
        "e(n+' name',globalThis[n].name===n);"
        "e(n+' length',globalThis[n].length===0);"
        "e(n+' window',window[n]===globalThis[n]&&self[n]===globalThis[n]);"
        "e(n+' not constructor',(()=>{try{new globalThis[n]();return false}"
        "catch(x){return x instanceof TypeError}})());}"
        "e('typeof',typeof alert==='function'&&window.alert===alert);"
        "pocSummary=f.length?f.join(','):'OK'"));
    /* Constructing threw before any dialog. */
    CHECK(seam.request_count == 0);
    CHECK(page_close(&page));
    return 0;
}

static int test_return_values(void)
{
    puts("test: each dialog returns its answer");
    seam_install(true);
    DialogPage page;
    CHECK(page_open(&page, "https://dialogs.test/"));
    const ScriptedAnswer answers[] = {
        {true, PAGE_DIALOG_OUTCOME_ACCEPTED, NULL, 0},
        {true, PAGE_DIALOG_OUTCOME_DISMISSED, NULL, 0},
        {true, PAGE_DIALOG_OUTCOME_ACCEPTED, NULL, 0},
    };
    seam_answers(answers, 3);
    CHECK(page_run(&page,
        "const a=alert('hi'),b=confirm('no?'),c=confirm('yes?');"
        "pocSummary=a===undefined&&b===false&&c===true?'OK':"
        "'BAD '+[a,b,c].join()"));
    CHECK(seam.request_count == 3
          && seam.requests[0].kind == PAGE_DIALOG_ALERT
          && strcmp(seam.requests[0].message, "hi") == 0
          && strcmp(seam.requests[0].host, "dialogs.test") == 0
          && seam.requests[1].kind == PAGE_DIALOG_CONFIRM
          && seam.requests[2].kind == PAGE_DIALOG_CONFIRM);
    /* Dismissing an alert also returns undefined. */
    page_fresh_turn(&page);
    const ScriptedAnswer dismissed[] = {
        {true, PAGE_DIALOG_OUTCOME_DISMISSED, NULL, 0}};
    seam_answers(dismissed, 1);
    CHECK(page_run(&page,
        "pocSummary=alert('x')===undefined?'OK':'BAD'"));

    puts("test: prompt returns the text, its default, or null");
    page_fresh_turn(&page);
    const ScriptedAnswer prompts[] = {
        {true, PAGE_DIALOG_OUTCOME_ACCEPTED, "typed text", 0},
        {true, PAGE_DIALOG_OUTCOME_ACCEPTED, NULL, 0},
        {true, PAGE_DIALOG_OUTCOME_DISMISSED, "ignored", 0},
    };
    seam_answers(prompts, 3);
    CHECK(page_run(&page,
        "const t=prompt('Name?','Ada'),d=prompt('Keep?','default value'),"
        "n=prompt('Cancel?','x');"
        "pocSummary=t==='typed text'&&d==='default value'&&n===null?'OK':"
        "'BAD '+JSON.stringify([t,d,n])"));
    CHECK(seam.request_count == 7
          && seam.requests[4].kind == PAGE_DIALOG_PROMPT
          && strcmp(seam.requests[4].message, "Name?") == 0
          && strcmp(seam.requests[4].default_value, "Ada") == 0);
    /* No default: accepting unchanged returns the empty string. */
    page_fresh_turn(&page);
    seam_answers(answers, 1);
    CHECK(page_run(&page,
        "const v=prompt('Empty?');pocSummary=v===''?'OK':'BAD '+v"));
    /* What the user answers reaches the page as it is, as in Chromium. */
    page_fresh_turn(&page);
    const ScriptedAnswer typed[] = {
        {true, PAGE_DIALOG_OUTCOME_ACCEPTED, "  two\nlines\xe2\x80\xae\a  !  ", 0}};
    seam_answers(typed, 1);
    CHECK(page_run(&page,
        "const v=prompt('?');pocSummary=v==='  two\\nlines\\u202e\\u0007  !  '?"
        "'OK':'BAD '+JSON.stringify(v)"));
    /* A frontend's malformed bytes do not reach the page. */
    page_fresh_turn(&page);
    const ScriptedAnswer malformed[] = {
        {true, PAGE_DIALOG_OUTCOME_ACCEPTED, "a\xff  b", 0}};
    seam_answers(malformed, 1);
    CHECK(page_run(&page,
        "const v=prompt('?');pocSummary=v==='a\\ufffd  b'?'OK':"
        "'BAD '+JSON.stringify(v)"));
    CHECK(page_close(&page));
    return 0;
}

static int test_stringification(void)
{
    puts("test: arguments are stringified as WebIDL DOMString");
    seam_install(true);
    DialogPage page;
    CHECK(page_open(&page, "https://dialogs.test/"));
    static const struct {
        const char *call;
        const char *message;
        const char *default_value;
    } cases[] = {
        {"alert()", "", ""},
        {"alert(undefined)", "undefined", ""},
        {"alert(null)", "null", ""},
        {"alert(42)", "42", ""},
        {"alert({toString(){return 'object text'}})", "object text", ""},
        {"confirm()", "", ""},
        {"confirm(undefined)", "", ""},
        {"confirm(false)", "false", ""},
        {"prompt()", "", ""},
        {"prompt(undefined,undefined)", "", ""},
        {"prompt('m',null)", "m", "null"},
        {"prompt(1.5,{toString(){return 'd'}})", "1.5", "d"},
    };
    for (size_t at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
        page_fresh_turn(&page);
        size_t before = seam.request_count;
        char source[256];
        snprintf(source, sizeof(source), "%s;pocSummary='OK'",
                 cases[at].call);
        CHECK(page_run(&page, source));
        CHECK(seam.request_count == before + 1u);
        RecordedRequest *record = &seam.requests[before];
        if (strcmp(record->message, cases[at].message) != 0
            || strcmp(record->default_value, cases[at].default_value) != 0) {
            fprintf(stderr, "%s: message=\"%s\" default=\"%s\"\n",
                    cases[at].call, record->message, record->default_value);
            return 1;
        }
    }
    /* Conversion throws before anything is shown, even for a refused call:
       a Symbol, and a throwing toString, propagate. */
    page_fresh_turn(&page);
    size_t before = seam.request_count;
    CHECK(page_run(&page,
        "let s=0,t=0;try{alert(Symbol('x'))}catch(x){s=x instanceof TypeError}"
        "try{confirm({toString(){throw new RangeError('no')}})}"
        "catch(x){t=x instanceof RangeError}"
        "try{prompt('ok',Symbol())}catch(x){t=t&&x instanceof TypeError}"
        "pocSummary=s&&t?'OK':'BAD'"));
    CHECK(seam.request_count == before);
    CHECK(page_close(&page));
    return 0;
}

static int test_message_neutralised(void)
{
    puts("test: messages are bounded plain text");
    seam_install(true);
    DialogPage page;
    CHECK(page_open(&page, "https://dialogs.test:8443/"));
    page_fresh_turn(&page);
    CHECK(page_run(&page,
        "alert('pay \\u202eLIVE\\u202c to \\u2066x\\u2069 \\u200fnow\\u0000"
        "\\u0007\\u001b[31m\\tok\\r\\nnext\\u2028line\\ufeff\\ud800!');"
        "pocSummary='OK'"));
    CHECK(seam.request_count == 1);
    const char *shown = seam.requests[0].message;
    if (strcmp(shown, "pay LIVE to x now[31m ok\nnext\nline\xef\xbf\xbd!") != 0) {
        fprintf(stderr, "neutralised: \"%s\"\n", shown);
        return 1;
    }
    /* The title is the host and its explicit port. */
    CHECK(strcmp(seam.requests[0].host, "dialogs.test:8443") == 0);

    /* A huge message is cut before it is copied, then ends with an
       ellipsis within the displayed bound. */
    page_fresh_turn(&page);
    CHECK(page_run(&page,
        "alert('x'.repeat(4*1024*1024));pocSummary='OK'"));
    shown = seam.requests[1].message;
    size_t length = strlen(shown);
    CHECK(length < PAGE_DIALOG_MESSAGE_BYTES && length > 600u
          && strcmp(shown + length - 3u, "\xe2\x80\xa6") == 0);
    /* Lines are bounded; runs of blank lines collapse to one. */
    page_fresh_turn(&page);
    CHECK(page_run(&page,
        "alert('\\n\\n a\\n\\n\\n\\nb'+'\\nline'.repeat(40));"
        "pocSummary='OK'"));
    shown = seam.requests[2].message;
    size_t lines = 1;
    for (const char *at = shown; *at; at++) lines += *at == '\n';
    CHECK(strncmp(shown, "a\n\nb\nline", 9) == 0
          && lines == PAGE_DIALOG_MESSAGE_LINES
          && strcmp(shown + strlen(shown) - 3u, "\xe2\x80\xa6") == 0);
    /* A prompt default is a single line. */
    page_fresh_turn(&page);
    CHECK(page_run(&page,
        "prompt('q','one\\ntwo\\u202e');pocSummary='OK'"));
    CHECK(strcmp(seam.requests[3].default_value, "one two") == 0);
    /* A long default is shown cut with an ellipsis; accepted unchanged, the
       page gets the kept text back without it. */
    page_fresh_turn(&page);
    CHECK(page_run(&page,
        "const v=prompt('q','y'.repeat(1000));"
        "pocSummary=v.length>200&&v.length<256&&/^y+$/.test(v)?'OK':"
        "'BAD '+v.length+' '+v.slice(-4)"));
    shown = seam.requests[4].default_value;
    CHECK(strcmp(shown + strlen(shown) - 3u, "\xe2\x80\xa6") == 0);
    /* A default goes back exactly as the page wrote it, spaces, breaks and
       controls included. The dialog is only shown a safe line of it. */
    page_fresh_turn(&page);
    CHECK(page_run(&page,
        "const s=[prompt('q','  two  spaces  '),prompt('q','   '),"
        "prompt('q','a\\tb\\nc')];"
        "pocSummary=s[0]==='  two  spaces  '&&s[1]==='   '&&s[2]==='a\\tb\\nc'"
        "?'OK':'BAD '+JSON.stringify(s)"));
    page_fresh_turn(&page);
    CHECK(page_run(&page,
        "const v=prompt('q','x\\u202ey\\u0007z');"
        "pocSummary=v==='x\\u202ey\\u0007z'?'OK':'BAD '+JSON.stringify(v)"));
    CHECK(strcmp(seam.requests[5].default_value, "two spaces") == 0
          && strcmp(seam.requests[6].default_value, "") == 0
          && strcmp(seam.requests[7].default_value, "a b c") == 0
          && strcmp(seam.requests[8].default_value, "xyz") == 0);
    /* A long default keeps the size bound and never splits a character. */
    page_fresh_turn(&page);
    CHECK(page_run(&page,
        "const v=prompt('q',' '+'\\u65e5'.repeat(200));"
        "pocSummary=v[0]===' '&&v.length===85&&/^ [\\u65e5]+$/.test(v)?'OK':"
        "'BAD '+v.length"));
    /* NUL cannot cross the frontend's terminated string. */
    page_fresh_turn(&page);
    CHECK(page_run(&page,
        "const v=prompt('q','a\\0b');pocSummary=v==='a\\ufffdb'?'OK':"
        "'BAD '+JSON.stringify(v)"));
    CHECK(page_close(&page));

    /* An opaque or hostless document gets no host in its title. */
    char host[PAGE_DIALOG_HOST_BYTES];
    CHECK(!page_dialog_title_host("data:text/html,hi", host, sizeof(host))
          && host[0] == '\0');
    CHECK(page_dialog_title_host("HTTPS://Example.COM/path", host,
                                 sizeof(host))
          && strcmp(host, "example.com") == 0);
    /* A long host is never left out: the dialog shortens it on screen
       from the left, so its registrable end shows. */
    static const char long_host[] =
        "accounts.google.com.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        ".evil.example";
    char long_url[sizeof(long_host) + 32];
    snprintf(long_url, sizeof(long_url), "https://%s:8443/login", long_host);
    CHECK(page_dialog_title_host(long_url, host, sizeof(host))
          && strncmp(host, long_host, sizeof(long_host) - 1u) == 0
          && strcmp(host + sizeof(long_host) - 1u, ":8443") == 0);
    return 0;
}

static int test_answer_text_bounds(void)
{
    puts("test: an answer is copied exactly, within its bound");
    char out[8];
    CHECK(page_dialog_answer_text(" a\tb\n ", 6, out, sizeof(out)) == 6u
          && strcmp(out, " a\tb\n ") == 0);
    /* Seven bytes and the terminator; a character that does not fit stays out. */
    CHECK(page_dialog_answer_text("abcdefghij", 10, out, sizeof(out)) == 7u
          && strcmp(out, "abcdefg") == 0);
    CHECK(page_dialog_answer_text("\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5", 9,
                                  out, sizeof(out)) == 6u
          && strcmp(out, "\xe6\x97\xa5\xe6\x97\xa5") == 0);
    CHECK(page_dialog_answer_text(NULL, 4, out, sizeof(out)) == 0u
          && out[0] == '\0');
    CHECK(page_dialog_answer_text("abc", 3, out, 1u) == 0u && out[0] == '\0');
    /* The keyboard's copy keeps spaces and leaves nothing that could hide or
       break the line. */
    CHECK(page_dialog_editable_text("  a\r\nb\a\xe2\x80\xae ", 11, out,
                                    sizeof(out)) == 6u
          && strcmp(out, "  a b ") == 0);
    return 0;
}

static int test_sanitizer_bounds(void)
{
    puts("test: the sanitizer never overruns its output");
    char out[16];
    bool truncated = false;
    size_t length = page_dialog_sanitize_text(
        "abcdefghijklmnopqrstuvwxyz", 26, false, 8, out, sizeof(out),
        &truncated);
    /* Twelve bytes of text, then the ellipsis. */
    CHECK(truncated && length == 15u && length == strlen(out)
          && strncmp(out, "abcdefghijkl", 12) == 0
          && strcmp(out + 12, "\xe2\x80\xa6") == 0);
    /* A multibyte character is never split. */
    length = page_dialog_sanitize_text(
        "\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5", 15,
        false, 8, out, sizeof(out), &truncated);
    CHECK(truncated && length == 15u
          && memcmp(out + 9, "\xe6\x97\xa5\xe2\x80\xa6", 6) == 0);
    /* Malformed input becomes replacement characters: one for a bad
       byte, one for a whole overlong or surrogate sequence. */
    length = page_dialog_sanitize_text("\xff\xc0\x80\xed\xa0\x80" "a", 7,
                                       false, 8, out, sizeof(out),
                                       &truncated);
    CHECK(!truncated
          && strcmp(out, "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd" "a") == 0);
    CHECK(page_dialog_sanitize_text("abc", 3, false, 8, out, 4, &truncated)
          == 0 && out[0] == '\0');
    CHECK(page_dialog_sanitize_text(NULL, 9, false, 8, out, sizeof(out),
                                    &truncated) == 0 && !truncated);
    return 0;
}

static int test_frames_policy(void)
{
    puts("test: cross-origin and sandboxed frames get default answers");
    /* Chrome's rule: a frame may open dialogs only through a same-origin
       chain to the top-level document, and a sandbox needs allow-modals. */
    uint32_t no_sandbox = tilefinch_frame_sandbox_parse(NULL, 0, false);
    uint32_t scripts = tilefinch_frame_sandbox_parse(
        "allow-scripts allow-same-origin", 31, true);
    uint32_t modals = tilefinch_frame_sandbox_parse(
        "allow-scripts allow-same-origin allow-modals", 44, true);
    CHECK(page_dialog_frame_allowed(true, no_sandbox, true));
    CHECK(!page_dialog_frame_allowed(false, no_sandbox, true));
    CHECK(!page_dialog_frame_allowed(true, scripts, true));
    CHECK(page_dialog_frame_allowed(true, modals, true));
    CHECK(!page_dialog_frame_allowed(true, modals, false));

    return 0;
}

/* configure_messaging requires a callback; a recorder that drops. */
static bool drop_message(void *opaque, ScriptRuntime *source,
                         long target_frame_handle, const char *json,
                         const char *target_origin, const char *task_kind,
                         uint64_t task_sequence)
{
    (void) opaque; (void) source; (void) target_frame_handle; (void) json;
    (void) target_origin; (void) task_kind; (void) task_sequence;
    return true;
}

static int test_child_realms(void)
{
    puts("test: a frame realm shares its admitted parent's policy");
    seam_install(true);
    DialogPage top, child;
    CHECK(page_open(&top, "https://dialogs.test/"));
    CHECK(page_open(&child, "https://other.test/frame"));
    CHECK(script_runtime_configure_messaging(
        child.runtime, true, false, false, true, drop_message, NULL));
    CHECK(!script_runtime_dialogs_allowed(child.runtime));
    CHECK(page_run(&child,
        "const a=alert('x'),c=confirm('x'),p=prompt('x','d');"
        "pocSummary=a===undefined&&c===false&&p===null?'OK':'BAD'"));
    CHECK(seam.request_count == 0);
    PageDialogPolicy policy;
    CHECK(!script_runtime_page_dialog_policy(child.runtime, &policy)
          && policy.denied[PAGE_DIALOG_DENIED_FRAME] == 3u);

    script_runtime_set_dialog_parent(child.runtime, top.runtime);
    CHECK(script_runtime_dialogs_allowed(child.runtime));
    CHECK(page_run(&child, "pocSummary=confirm('child')?'OK':'BAD'"));
    CHECK(seam.request_count == 1
          && strcmp(seam.requests[0].host, "other.test") == 0);
    /* The count is the page's: the top realm's next dialog is its second
       and offers the escape, and a suppression in either realm holds for
       both. */
    const ScriptedAnswer suppress[] = {
        {true, PAGE_DIALOG_OUTCOME_SUPPRESS, NULL, 0}};
    seam_answers(suppress, 1);
    /* The pages share one budget and the child's leak check counts from
       its opening, so what the top realm grows by in its own run (an atom
       table or heap block, depending on the bootstrap's layout) is the
       top's, released when the top closes. */
    size_t top_before = page_budget.current;
    CHECK(page_run(&top, "pocSummary=confirm('top')===false?'OK':'BAD'"));
    child.baseline += page_budget.current - top_before;
    CHECK(seam.request_count == 2 && seam.requests[1].offer_suppress);
    page_fresh_turn(&top);
    CHECK(page_run(&child, "pocSummary=confirm('again')===false?'OK':'BAD'"));
    CHECK(seam.request_count == 2);
    policy = page_policy(&top);
    CHECK(policy.shown == 2 && policy.suppressed
          && policy.denied[PAGE_DIALOG_DENIED_SUPPRESSED] == 1u);
    script_runtime_set_dialog_parent(child.runtime, NULL);
    CHECK(page_close(&child));
    CHECK(page_close(&top));
    return 0;
}

static int test_hidden_page(void)
{
    puts("test: a hidden page, and its visibilitychange handler, get none");
    seam_install(true);
    DialogPage page;
    CHECK(page_open(&page, "https://dialogs.test/"));
    CHECK(page_run(&page,
        "globalThis.seen=[];document.addEventListener('visibilitychange',"
        "()=>{seen.push(document.visibilityState,confirm('stay?'),"
        "prompt('why?','x'),alert('bye'))});pocSummary='OK'"));
    CHECK(script_runtime_set_page_visibility(page.runtime, false));
    for (unsigned turn = 0; turn < 4u; turn++)
        CHECK(script_runtime_advance(page.runtime, 16u, 8u, &page.result));
    CHECK(page_run(&page,
        "pocSummary=JSON.stringify(seen)==='[\"hidden\",false,null,null]'"
        "&&confirm('later')===false?'OK':'BAD '+JSON.stringify(seen)"));
    CHECK(seam.request_count == 0);
    CHECK(page_policy(&page).denied[PAGE_DIALOG_DENIED_HIDDEN] == 4u);
    /* Visible again: dialogs return. */
    CHECK(script_runtime_set_page_visibility(page.runtime, true));
    CHECK(page_run(&page, "pocSummary=confirm('back')?'OK':'BAD'"));
    CHECK(seam.request_count == 1);
    CHECK(page_close(&page));
    return 0;
}

static int test_block_option(void)
{
    puts("test: the second dialog offers to stop the page's dialogs");
    seam_install(true);
    DialogPage page;
    CHECK(page_open(&page, "https://dialogs.test/"));
    const ScriptedAnswer answers[] = {
        {true, PAGE_DIALOG_OUTCOME_ACCEPTED, NULL, 0},
        {true, PAGE_DIALOG_OUTCOME_SUPPRESS, NULL, 0},
    };
    seam_answers(answers, 2);
    CHECK(page_run(&page,
        "const a=confirm('one'),b=confirm('two'),c=confirm('three'),"
        "d=prompt('four','x');alert('five');"
        "pocSummary=a===true&&b===false&&c===false&&d===null?'OK':'BAD'"));
    CHECK(seam.request_count == 2 && !seam.requests[0].offer_suppress
          && seam.requests[1].offer_suppress);
    /* Neither a gesture nor time lifts it for this page load. */
    page_fresh_turn(&page);
    CHECK(page_run(&page, "pocSummary=confirm('x')===false?'OK':'BAD'"));
    CHECK(seam.request_count == 2);
    PageDialogPolicy policy = page_policy(&page);
    CHECK(policy.suppressed
          && policy.denied[PAGE_DIALOG_DENIED_SUPPRESSED] == 4u);
    CHECK(page_close(&page));

    /* A new page load starts over. */
    CHECK(page_open(&page, "https://dialogs.test/"));
    CHECK(page_run(&page, "pocSummary=confirm('fresh')?'OK':'BAD'"));
    CHECK(seam.request_count == 3 && !seam.requests[2].offer_suppress);
    CHECK(page_close(&page));

    puts("test: leaving the page from a dialog also stops its dialogs");
    CHECK(page_open(&page, "https://dialogs.test/"));
    const ScriptedAnswer leave[] = {
        {true, PAGE_DIALOG_OUTCOME_LEAVE, NULL, 0}};
    seam_answers(leave, 1);
    CHECK(page_run(&page,
        "pocSummary=confirm('leave?')===false&&confirm('more')===false"
        "?'OK':'BAD'"));
    CHECK(seam.request_count == 4 && page_policy(&page).suppressed);
    CHECK(page_close(&page));
    return 0;
}

static int test_limits(void)
{
    puts("test: an alert loop ends after the gesture limit");
    seam_install(true);
    DialogPage page;
    CHECK(page_open(&page, "https://dialogs.test/"));
    /* Every dialog is dismissed at once, as a user mashing Circle would;
       the loop must still end, quickly, with no further dialogs. */
    seam.fallback = (ScriptedAnswer) {
        .shown = true, .outcome = PAGE_DIALOG_OUTCOME_DISMISSED
    };
    CHECK(page_run(&page,
        "let n=0;for(let i=0;i<100000;i++){alert(i);n++}"
        "while(confirm('again?'))n++;"
        "pocSummary=n===100000?'OK':'BAD '+n"));
    CHECK(seam.request_count == PAGE_DIALOG_GESTURE_LIMIT);
    CHECK(page_policy(&page).denied[PAGE_DIALOG_DENIED_GESTURE]
          == 100000u + 1u - PAGE_DIALOG_GESTURE_LIMIT);
    /* A timer chain gets no more, however long it runs. */
    CHECK(page_run(&page,
        "globalThis.ticks=0;const id=setInterval(()=>{ticks++;alert('t')},1);"
        "globalThis.stopTicks=()=>clearInterval(id);pocSummary='OK'"));
    for (unsigned turn = 0; turn < 64u; turn++) {
        seam_advance_ms(1000u);
        CHECK(script_runtime_advance(page.runtime, 16u, 8u, &page.result));
    }
    CHECK(seam.request_count == PAGE_DIALOG_GESTURE_LIMIT);
    CHECK(page_run(&page, "stopTicks();pocSummary=ticks>8?'OK':'BAD '+ticks"));

    /* A gesture restarts the count. */
    script_runtime_notify_user_activation(page.runtime);
    CHECK(page_run(&page, "alert('after a press');pocSummary='OK'"));
    CHECK(seam.request_count == PAGE_DIALOG_GESTURE_LIMIT + 1u);
    CHECK(page_close(&page));

    puts("test: at most five dialogs in thirty seconds, gestures or not");
    CHECK(page_open(&page, "https://dialogs.test/"));
    size_t before = seam.request_count;
    for (unsigned turn = 0; turn < 4u; turn++) {
        script_runtime_notify_user_activation(page.runtime);
        seam_advance_ms(1000u);
        CHECK(page_run(&page, "alert(1);alert(2);pocSummary='OK'"));
    }
    CHECK(seam.request_count - before == PAGE_DIALOG_RATE_LIMIT);
    CHECK(page_policy(&page).denied[PAGE_DIALOG_DENIED_RATE] == 3u);
    /* The window slides: thirty seconds after the first turn, its two
       openings have left it, and only those two. */
    seam_advance_ms(PAGE_DIALOG_RATE_WINDOW_MS - 3000u);
    script_runtime_notify_user_activation(page.runtime);
    CHECK(page_run(&page,
        "alert('later');alert('again');alert('refused');pocSummary='OK'"));
    CHECK(seam.request_count - before == PAGE_DIALOG_RATE_LIMIT + 2u);
    /* A clock that steps back refuses rather than opening the window. */
    seam.now_ns -= UINT64_C(500000000000);
    script_runtime_notify_user_activation(page.runtime);
    CHECK(page_run(&page, "alert('past');pocSummary='OK'"));
    CHECK(seam.request_count - before == PAGE_DIALOG_RATE_LIMIT + 2u);
    CHECK(page_close(&page));
    return 0;
}

static int test_frontend_refusal_and_watchdog(void)
{
    puts("test: a frontend that cannot show a dialog gives default answers");
    seam_install(true);
    DialogPage page;
    CHECK(page_open(&page, "https://dialogs.test/"));
    seam.fallback = (ScriptedAnswer) {.shown = false};
    CHECK(page_run(&page,
        "let ok=true;for(let i=0;i<10;i++)ok=ok&&alert()===undefined"
        "&&confirm()===false&&prompt('','x')===null;"
        "pocSummary=ok?'OK':'BAD'"));
    /* Refusals are not counted against the page. */
    PageDialogPolicy policy = page_policy(&page);
    CHECK(seam.request_count == 30u && policy.shown == 0
          && policy.since_gesture == 0
          && policy.denied[PAGE_DIALOG_DENIED_UNAVAILABLE] == 30u);
    seam.fallback = (ScriptedAnswer) {
        .shown = true, .outcome = PAGE_DIALOG_OUTCOME_ACCEPTED
    };
    CHECK(page_run(&page, "pocSummary=confirm('now')?'OK':'BAD'"));
    CHECK(page_policy(&page).shown == 1u);

    puts("test: time spent in a dialog is not script time");
    /* The realm's script timeout is three seconds; the user takes a
       minute. The script that asked keeps running afterwards. */
    page_fresh_turn(&page);
    const ScriptedAnswer slow[] = {
        {true, PAGE_DIALOG_OUTCOME_ACCEPTED, NULL, 60000u}};
    seam_answers(slow, 1);
    CHECK(page_run(&page,
        "const r=confirm('slow');let s=0;for(let i=0;i<20000;i++)s+=i;"
        "pocSummary=r&&s>0?'OK':'BAD'"));
    CHECK(page_close(&page));
    tilefinch_platform_set_services(NULL);

    puts("test: without a dialog service every call is refused");
    CHECK(page_open(&page, "https://dialogs.test/"));
    CHECK(page_run(&page,
        "pocSummary=alert('x')===undefined&&confirm('x')===false"
        "&&prompt('x','y')===null?'OK':'BAD'"));
    CHECK(page_close(&page));
    return 0;
}

static int test_modal_input(void)
{
    puts("test: the modal arms before it accepts, and refuses at once");
    PageDialogModal modal;
    /* X held when the dialog opened (a mashed button) does not accept. */
    page_dialog_modal_open(&modal, PAGE_DIALOG_CONFIRM, false,
                           PAGE_DIALOG_BUTTON_CROSS, 1000u);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_CROSS, 1000u
                                 + PAGE_DIALOG_ARM_MS * 4u)
          == PAGE_DIALOG_MODAL_WAIT);
    CHECK(page_dialog_modal_step(&modal, 0, 1000u + PAGE_DIALOG_ARM_MS * 4u)
          == PAGE_DIALOG_MODAL_WAIT);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_CROSS,
                                 1000u + PAGE_DIALOG_ARM_MS * 4u)
          == PAGE_DIALOG_MODAL_ACCEPT);
    /* A fresh X before the arming delay waits; Circle never does. */
    page_dialog_modal_open(&modal, PAGE_DIALOG_CONFIRM, false, 0, 1000u);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_CROSS, 1001u)
          == PAGE_DIALOG_MODAL_WAIT);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_CIRCLE, 1002u)
          == PAGE_DIALOG_MODAL_DISMISS);
    /* Several edges in one sample refuse. */
    page_dialog_modal_open(&modal, PAGE_DIALOG_CONFIRM, false, 0, 0);
    CHECK(page_dialog_modal_step(&modal, 0, PAGE_DIALOG_ARM_MS) ==
          PAGE_DIALOG_MODAL_WAIT);
    CHECK(page_dialog_modal_step(
              &modal, PAGE_DIALOG_BUTTON_CROSS | PAGE_DIALOG_BUTTON_CIRCLE,
              PAGE_DIALOG_ARM_MS + 1u) == PAGE_DIALOG_MODAL_DISMISS);
    /* Square is the escape only when offered; Triangle always leaves. */
    page_dialog_modal_open(&modal, PAGE_DIALOG_ALERT, false, 0, 0);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_SQUARE, 1u)
          == PAGE_DIALOG_MODAL_WAIT);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_TRIANGLE, 2u)
          == PAGE_DIALOG_MODAL_LEAVE);
    page_dialog_modal_open(&modal, PAGE_DIALOG_ALERT, true, 0, 0);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_SQUARE, 1u)
          == PAGE_DIALOG_MODAL_SUPPRESS);
    /* prompt: X edits, Start accepts. */
    page_dialog_modal_open(&modal, PAGE_DIALOG_PROMPT, false, 0, 0);
    CHECK(page_dialog_modal_step(&modal, 0, PAGE_DIALOG_ARM_MS)
          == PAGE_DIALOG_MODAL_WAIT);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_CROSS,
                                 PAGE_DIALOG_ARM_MS + 1u)
          == PAGE_DIALOG_MODAL_EDIT);
    /* Back from the keyboard with Start still held: not an accept. */
    page_dialog_modal_resume(&modal, PAGE_DIALOG_BUTTON_START);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_START,
                                 PAGE_DIALOG_ARM_MS + 2u)
          == PAGE_DIALOG_MODAL_WAIT);
    CHECK(page_dialog_modal_step(&modal, 0, PAGE_DIALOG_ARM_MS + 3u)
          == PAGE_DIALOG_MODAL_WAIT);
    CHECK(page_dialog_modal_step(&modal, PAGE_DIALOG_BUTTON_START,
                                 PAGE_DIALOG_ARM_MS + 4u)
          == PAGE_DIALOG_MODAL_ACCEPT);
    /* Unanswered, it is dismissed after the open limit. */
    page_dialog_modal_open(&modal, PAGE_DIALOG_CONFIRM, false, 0, 0);
    CHECK(page_dialog_modal_step(&modal, 0, PAGE_DIALOG_MAXIMUM_OPEN_MS)
          == PAGE_DIALOG_MODAL_DISMISS);
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
    } pages[] = {
        {"https://dialogs.test/top",
         "<!doctype html><title>Top</title><p>Top document.</p>"
         "<iframe src=\"https://other.test/child\"></iframe>"
         "<iframe src=\"https://dialogs.test/same\"></iframe>"
         "<iframe sandbox=\"allow-scripts allow-same-origin\" "
         "src=\"https://dialogs.test/sandboxed\"></iframe>"
         "<iframe sandbox=\"allow-scripts allow-same-origin allow-modals\" "
         "src=\"https://dialogs.test/modals\"></iframe>"},
        {"https://other.test/child", NULL},
        {"https://dialogs.test/same", NULL},
        {"https://dialogs.test/sandboxed", NULL},
        {"https://dialogs.test/modals", NULL},
    };
    if (mkdir(directory, 0755) != 0 && errno != EEXIST) return false;
    char path[512], meta[1024], body[512];
    size_t count = sizeof(pages) / sizeof(pages[0]);
    for (size_t at = 0; at < count; at++) {
        if (pages[at].body != NULL) {
            snprintf(body, sizeof(body), "%s", pages[at].body);
        } else {
            snprintf(body, sizeof(body),
                     "<!doctype html><title>Child</title><p>Child.</p>"
                     "<script>document.title=String(confirm('frame:'"
                     "+location.host+location.pathname))</script>");
        }
        snprintf(meta, sizeof(meta),
                 "psp-http-trace=1\nmethod=GET\nurl=%s\nsuccess=1\n"
                 "async-delay-pumps=1\nexternal-cancel=0\n"
                 "transport-timeout=0\nerror=\nstatus=200\nlength=%zu\n"
                 "effective-url=%s\n"
                 "content-type=text/html; charset=utf-8\nserver=fixture\n"
                 "set-cookie-count=0\nresponse-header-count=1\n"
                 "response-header-0=content-type: text/html; charset=utf-8\n",
                 pages[at].url, strlen(body), pages[at].url);
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

static int test_leaving_opens_no_dialog(void)
{
    puts("test: closing a page runs no handler or timer that could ask");
    seam_install(true);
    DialogPage page;
    CHECK(page_open(&page, "https://dialogs.test/"));
    CHECK(page_run(&page,
        "for (const name of ['pagehide','unload','beforeunload'])"
        "  addEventListener(name,()=>alert('leaving'));"
        "setTimeout(()=>alert('timer'),0);"
        "Promise.resolve().then(()=>{});"
        "pocSummary='OK'"));
    size_t before = seam.request_count;
    /* Teardown drains the callback queues; none of it may run page script. */
    CHECK(page_close(&page));
    CHECK(seam.request_count == before);
    return 0;
}

static int test_navigation_frames(void)
{
    puts("test: navigation admits only same-origin allow-modals frames");
    static const char directory[] = "page-dialog-frames-fixture";
    CHECK(write_fixture(directory));
    seam_install(false);
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
        engine, "https://dialogs.test/top", 1u * MIB, 5000, true));
    BrowserNavigationJobStatus status =
        browser_engine_navigation_status(engine);
    for (unsigned pump = 0;
         pump < 512 && status == BROWSER_NAVIGATION_JOB_PENDING; pump++)
        status = browser_engine_pump_navigation(engine, NULL);
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    NavigationSession *navigation = browser_engine_navigation(engine);
    for (unsigned tick = 0; tick < 32u; tick++)
        (void) browser_engine_advance_runtime(engine, 16, 4, NULL);
    CHECK(navigation->page.frame_count == 4u);
    size_t loaded = 0;
    for (size_t at = 0; at < navigation->page.frame_count; at++) {
        NavigationFrame *frame = &navigation->page.frames[at];
        if (!frame->loaded || frame->runtime == NULL) continue;
        loaded++;
        PageDialogPolicy policy;
        bool shared = script_runtime_page_dialog_policy(frame->runtime,
                                                        &policy);
        bool expected = strstr(frame->url, "/same") != NULL
            || strstr(frame->url, "/modals") != NULL;
        if (shared != expected
            || (!expected && policy.denied[PAGE_DIALOG_DENIED_FRAME] != 1u)) {
            fprintf(stderr, "frame %s: shared=%d denied=%u\n", frame->url,
                    shared, policy.denied[PAGE_DIALOG_DENIED_FRAME]);
            return 1;
        }
    }
    CHECK(loaded == 4u);
    CHECK(seam.request_count == 2u);
    bool same = false, modals = false;
    for (size_t at = 0; at < 2u; at++) {
        CHECK(strcmp(seam.requests[at].host, "dialogs.test") == 0);
        same |= strcmp(seam.requests[at].message, "frame:dialogs.test/same")
            == 0;
        modals |= strcmp(seam.requests[at].message,
                         "frame:dialogs.test/modals") == 0;
    }
    CHECK(same && modals);
    PageDialogPolicy top;
    CHECK(script_runtime_page_dialog_policy(navigation->page.runtime, &top)
          && top.shown == 2u);
    browser_engine_destroy(engine);
    fetch_trace_end();
    tilefinch_platform_set_services(NULL);
    return 0;
}

int main(void)
{
    budget_init(&page_budget, 64u * MIB);
    if (!budget_install_lexbor(&page_budget)) return 1;
    int failures = 0;
    failures += test_global_shape();
    failures += test_return_values();
    failures += test_stringification();
    failures += test_message_neutralised();
    failures += test_answer_text_bounds();
    failures += test_sanitizer_bounds();
    failures += test_frames_policy();
    failures += test_child_realms();
    failures += test_hidden_page();
    failures += test_block_option();
    failures += test_limits();
    failures += test_frontend_refusal_and_watchdog();
    failures += test_modal_input();
    failures += test_leaving_opens_no_dialog();
    /* The engine installs its own lexbor allocator. */
    (void) budget_uninstall_lexbor(&page_budget);
    failures += test_navigation_frames();
    tilefinch_platform_set_services(NULL);
    if (failures == 0) puts("page dialog tests passed");
    return failures == 0 ? 0 : 1;
}
