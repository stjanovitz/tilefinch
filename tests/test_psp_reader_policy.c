/* Who engaged Reader on the PSP frontend (psp_reader_policy.h): Auto Reader
   belongs to one page and says so; the user's Reader is sticky. The page
   analyses come from the real classifier on synthetic pages. */
#include "tilefinch/psp_reader_policy.h"
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/reader_mode.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define PROSE \
    "<p>This paragraph of a long report carries its own sentences, more " \
    "than one hundred and twenty bytes of ordinary prose with only a " \
    "single <a href='/related'>link</a> in it.</p>"

static const char article_page[] =
    "<!doctype html><body><nav><a href='/'>Home</a></nav><main>"
    "<h1>A clear report</h1>" PROSE PROSE PROSE PROSE PROSE PROSE
    "</main></body>";

#define TEASER(n) \
    "<section><h3><a href='/story/" #n "'>Headline " #n "</a></h3>" PROSE \
    "</section>"
static const char front_page[] =
    "<!doctype html><body><main><h1>News</h1>"
    TEASER(1) TEASER(2) TEASER(3) TEASER(4) TEASER(5) TEASER(6)
    "</main></body>";

static int analyze(const char *html, ReaderDocumentAnalysis *analysis)
{
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    *analysis = (ReaderDocumentAnalysis) {0};
    CHECK(document_parse(&document, &budget, html, strlen(html), 113u)
          && reader_document_analyze_with_stylesheet(
                 &document, NULL, analysis));
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u);
    return 0;
}

/* The frontend's commit step (psp_apply_reader_after_navigation): an
   explicit request (carried user Reader) keeps Reader; otherwise Auto Reader
   engages only on an admitted page. Returns whether Reader is shown. */
static bool commit_page(PspUiState *ui, bool carried,
                        const ReaderDocumentAnalysis *analysis)
{
    if (carried) {
        psp_reader_mark_engaged(ui, false);
        return true;
    }
    if (psp_reader_automatic_admits(analysis)) {
        psp_reader_mark_engaged(ui, true);
        return true;
    }
    return false;
}

/* The link step: a carrying Reader stays on (psp_reader_navigation_prepare);
   anything else leaves it (psp_leave_reader_for_navigation). */
static bool follow_link(PspUiState *ui)
{
    bool carried = psp_reader_carries_to_navigation(ui);
    if (!carried) psp_reader_mark_left(ui);
    return carried;
}

static int test_auto_reader_is_per_page(void)
{
    ReaderDocumentAnalysis article = {0}, front = {0};
    CHECK(analyze(article_page, &article) == 0
          && analyze(front_page, &front) == 0);
    CHECK(psp_reader_automatic_admits(&article)
          && !psp_reader_automatic_admits(&front));
    PspUiState ui;
    psp_ui_init(&ui);
    CHECK(commit_page(&ui, follow_link(&ui), &article) && ui.reader_mode);
    /* A link from the automatically engaged article to a front page shows
       the front page raw. */
    CHECK(!commit_page(&ui, follow_link(&ui), &front) && !ui.reader_mode);
    /* The next article qualifies on its own. */
    CHECK(commit_page(&ui, follow_link(&ui), &article) && ui.reader_mode);
    return 0;
}

static int test_user_reader_stays_sticky(void)
{
    ReaderDocumentAnalysis front = {0};
    CHECK(analyze(front_page, &front) == 0);
    PspUiState ui;
    psp_ui_init(&ui);
    /* Page tools -> Reader mode. */
    psp_reader_mark_engaged(&ui, false);
    CHECK(commit_page(&ui, follow_link(&ui), &front) && ui.reader_mode);
    CHECK(commit_page(&ui, follow_link(&ui), &front) && ui.reader_mode);
    /* Turning an automatic Reader off and on again makes it the user's. */
    psp_reader_mark_engaged(&ui, true);
    psp_reader_mark_left(&ui);
    psp_reader_mark_engaged(&ui, false);
    CHECK(psp_reader_carries_to_navigation(&ui));
    psp_reader_mark_left(&ui);
    CHECK(!psp_reader_carries_to_navigation(&ui) && !ui.reader_mode);
    return 0;
}

static int test_note_on_automatic_engagement_only(void)
{
    PspUiState ui;
    psp_ui_init(&ui);
    psp_reader_mark_engaged(&ui, false);
    CHECK(strcmp(ui.status, PSP_READER_AUTOMATIC_NOTE) != 0);
    psp_reader_mark_left(&ui);
    psp_reader_mark_engaged(&ui, true);
    CHECK(strcmp(ui.status, PSP_READER_AUTOMATIC_NOTE) == 0
          && ui.toast_frames == PSP_READER_AUTOMATIC_NOTE_FRAMES);
    /* Two lines within the multiline toast bound, already mixed case so the
       status renderer shows them as written. */
    const char *second = strchr(PSP_READER_AUTOMATIC_NOTE, '\n');
    CHECK(second != NULL
          && (size_t) (second - PSP_READER_AUTOMATIC_NOTE)
                 <= PSP_UI_MULTILINE_TOAST_CHARACTER_LIMIT
          && strlen(second + 1) <= PSP_UI_MULTILINE_TOAST_CHARACTER_LIMIT
          && strlen(PSP_READER_AUTOMATIC_NOTE) < PSP_UI_STATUS_CAPACITY);
    char presented[PSP_UI_STATUS_CAPACITY];
    snprintf(presented, sizeof(presented), "%s", PSP_READER_AUTOMATIC_NOTE);
    psp_ui_status_sentence_case(presented);
    CHECK(strcmp(presented, PSP_READER_AUTOMATIC_NOTE) == 0);
    return 0;
}

int main(void)
{
    if (test_auto_reader_is_per_page() != 0
        || test_user_reader_stays_sticky() != 0
        || test_note_on_automatic_engagement_only() != 0) return 1;
    puts("psp-reader-policy-tests: ok");
    return 0;
}
