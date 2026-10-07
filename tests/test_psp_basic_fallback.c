#include "tilefinch/psp_basic_fallback.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/site_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* Each setting value against each engine answer: Automatic switches, Ask
   offers, Off leaves the page alone; deferral and unavailability are the
   same for both active modes. */
static int test_decisions_per_setting(void)
{
    static const BrowserBasicViewRecovery answers[] = {
        BROWSER_BASIC_VIEW_RECOVERY_NONE,
        BROWSER_BASIC_VIEW_RECOVERY_AVAILABLE,
        BROWSER_BASIC_VIEW_RECOVERY_UNAVAILABLE,
        BROWSER_BASIC_VIEW_RECOVERY_DEFERRED
    };
    static const PspBasicFallbackDecision automatic[] = {
        PSP_BASIC_FALLBACK_KEEP_PAGE, PSP_BASIC_FALLBACK_SWITCH,
        PSP_BASIC_FALLBACK_UNAVAILABLE, PSP_BASIC_FALLBACK_DEFER
    };
    static const PspBasicFallbackDecision ask[] = {
        PSP_BASIC_FALLBACK_KEEP_PAGE, PSP_BASIC_FALLBACK_ASK,
        PSP_BASIC_FALLBACK_UNAVAILABLE, PSP_BASIC_FALLBACK_DEFER
    };
    for (size_t i = 0; i < 4u; i++) {
        CHECK(psp_basic_fallback_decide(
                  BROWSER_BASIC_FALLBACK_AUTOMATIC, answers[i])
              == automatic[i]);
        CHECK(psp_basic_fallback_decide(BROWSER_BASIC_FALLBACK_ASK,
                                        answers[i]) == ask[i]);
        CHECK(psp_basic_fallback_decide(BROWSER_BASIC_FALLBACK_OFF,
                                        answers[i])
              == PSP_BASIC_FALLBACK_KEEP_PAGE);
    }
    /* Off never prepares a tree; an explicit Reader request owns the slot. */
    CHECK(psp_basic_fallback_consulted(
        BROWSER_BASIC_FALLBACK_AUTOMATIC, false));
    CHECK(psp_basic_fallback_consulted(BROWSER_BASIC_FALLBACK_ASK, false));
    CHECK(!psp_basic_fallback_consulted(BROWSER_BASIC_FALLBACK_OFF, false));
    CHECK(!psp_basic_fallback_consulted(
        BROWSER_BASIC_FALLBACK_AUTOMATIC, true));
    return 0;
}

/* Trigger (b): scripts that fail after commit (a failed turn, a retired
   realm) are examined once per page; nothing is re-examined under Off, in an
   extracted view, or while a recovery is already pending. */
static int test_post_commit_failure_recheck(void)
{
    const BrowserBasicFallbackMode on[] = {
        BROWSER_BASIC_FALLBACK_AUTOMATIC, BROWSER_BASIC_FALLBACK_ASK
    };
    for (size_t i = 0; i < 2u; i++) {
        CHECK(psp_basic_fallback_recheck(on[i], false, false, false, false,
                                         false));
        CHECK(psp_basic_fallback_recheck(on[i], false, false, true, true,
                                         false));
        CHECK(!psp_basic_fallback_recheck(on[i], false, false, true, false,
                                          false));
        CHECK(!psp_basic_fallback_recheck(on[i], false, false, false, true,
                                          true));
        CHECK(!psp_basic_fallback_recheck(on[i], true, false, false, true,
                                          false));
        CHECK(!psp_basic_fallback_recheck(on[i], false, true, false, true,
                                          false));
    }
    CHECK(!psp_basic_fallback_recheck(BROWSER_BASIC_FALLBACK_OFF, false,
                                      false, false, true, false));
    return 0;
}

/* Triggers (c) and (d): the recovery prompt offers the failed page's own
   Basic view while its DOM exists, otherwise a script-free reload when the
   document arrived; Off offers neither. */
static int test_recovery_choices(void)
{
    const BrowserBasicFallbackMode on[] = {
        BROWSER_BASIC_FALLBACK_AUTOMATIC, BROWSER_BASIC_FALLBACK_ASK
    };
    for (size_t i = 0; i < 2u; i++) {
        CHECK(psp_basic_fallback_failure_actions(on[i], true, false, false)
              == PSP_UI_FAILURE_BASIC_VIEW);
        CHECK(psp_basic_fallback_failure_actions(on[i], true, false, true)
              == PSP_UI_FAILURE_BASIC_VIEW);
        CHECK(psp_basic_fallback_failure_actions(on[i], false, false, true)
              == PSP_UI_FAILURE_RELOAD_BASIC);
        CHECK(psp_basic_fallback_failure_actions(on[i], false, false, false)
              == 0u);
        CHECK(psp_basic_fallback_failure_actions(on[i], true, true, true)
              == 0u);
    }
    CHECK(psp_basic_fallback_failure_actions(
              BROWSER_BASIC_FALLBACK_OFF, true, false, true) == 0u);
    /* Only a document that arrived can be retried script-free. */
    CHECK(psp_basic_fallback_navigation_reloadable(200, false));
    CHECK(!psp_basic_fallback_navigation_reloadable(0, false));
    CHECK(!psp_basic_fallback_navigation_reloadable(404, false));
    CHECK(!psp_basic_fallback_navigation_reloadable(503, false));
    CHECK(!psp_basic_fallback_navigation_reloadable(200, true));
    return 0;
}

/* psp_set_presentation_css appends the cosmetic and cookie-notice sheets to
   the Reader/Basic sheet. The generic content-shape sheet alone nearly fills
   SITE_ADAPTER_READER_CSS_LIMIT, so the frontend buffer is sized for all
   three (an 8 KiB buffer refused every extracted view on the PSP's default
   content-blocker settings). */
static int test_presentation_sheets_fit_frontend_buffer(void)
{
    char reader[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    char cosmetic[CONTENT_BLOCKER_COSMETIC_CSS_LIMIT];
    char cookie[CONTENT_BLOCKER_COOKIE_CSS_LIMIT];
    size_t cosmetic_length = 0, cookie_length = 0;
    static const unsigned percents[] = {80u, 100u, 125u, 150u};
    CHECK(content_blocker_cosmetic_css(
              cosmetic, sizeof(cosmetic), &cosmetic_length)
          && content_blocker_cookie_banner_css(
                 cookie, sizeof(cookie), &cookie_length));
    for (size_t i = 0; i < 4u; i++) {
        for (int serif = 0; serif < 2; serif++) {
            CHECK(site_adapter_reader_css(
                "https://news.example.test/",
                serif ? SITE_ADAPTER_READER_FONT_SERIF
                      : SITE_ADAPTER_READER_FONT_SANS,
                percents[i], reader, sizeof(reader), adapter,
                sizeof(adapter)));
            size_t total = strlen(reader) + 1u + cosmetic_length + 1u
                + cookie_length;
            CHECK(total < SITE_ADAPTER_READER_CSS_LIMIT
                              + CONTENT_BLOCKER_COSMETIC_CSS_LIMIT
                              + CONTENT_BLOCKER_COOKIE_CSS_LIMIT + 2u);
        }
    }
    return 0;
}

int main(void)
{
    CHECK(test_presentation_sheets_fit_frontend_buffer() == 0);
    CHECK(test_decisions_per_setting() == 0);
    CHECK(test_post_commit_failure_recheck() == 0);
    CHECK(test_recovery_choices() == 0);
    puts("psp-basic-fallback-tests: ok");
    return EXIT_SUCCESS;
}
