#include "tilefinch/psp_basic_fallback.h"
#include "tilefinch/psp_heavy_notice.h"
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

/* The heavy-page notices in order (psp_app_heavy_poll). A page whose own
   scripts are refused as too large still offers the large script its
   frame holds, once the page shows again; the frame offer never reuses the
   page's numbers, and a poll is done only once both have been said. */
static int test_heavy_notice_order(void)
{
    ScriptHeavyState over = {0};
    over.page_class = SCRIPT_HEAVY_CLASS_OVER;
    over.oversized_scripts = 1;
    over.oversized_bytes = 4u * 1024u * 1024u;
    NavigationHeavyFrames frames = {0};
    frames.waiting_scripts = 1;
    frames.waiting_bytes = 270018;
    frames.asks = true;
    PspHeavyNoticeInput input = {.on_page = true};
    CHECK(!psp_heavy_notice_done(&input));
    CHECK(psp_heavy_notice_next(&input, &over, &frames)
          == PSP_HEAVY_NOTICE_OVER);
    input.offer_shown = true;
    /* The recovery sheet is up: nothing over it. */
    input.on_page = false;
    CHECK(psp_heavy_notice_next(&input, &over, &frames)
          == PSP_HEAVY_NOTICE_NONE);
    input.on_page = true;
    CHECK(!psp_heavy_notice_done(&input));
    CHECK(psp_heavy_notice_next(&input, &over, &frames)
          == PSP_HEAVY_NOTICE_FRAME_OFFER);
    input.frame_offer_shown = true;
    CHECK(psp_heavy_notice_done(&input));
    CHECK(psp_heavy_notice_next(&input, &over, &frames)
          == PSP_HEAVY_NOTICE_NONE);

    /* A held page asks first; its frame waits for the page to show. */
    ScriptHeavyState shell = {0};
    shell.page_class = SCRIPT_HEAVY_CLASS_SHELL;
    shell.waiting = true;
    shell.waiting_scripts = 1;
    PspHeavyNoticeInput fresh = {.on_page = true};
    CHECK(psp_heavy_notice_next(&fresh, &shell, &frames)
          == PSP_HEAVY_NOTICE_OFFER);
    fresh.offer_shown = true;
    fresh.on_page = false;
    fresh.on_offer = true;
    CHECK(psp_heavy_notice_next(&fresh, &shell, &frames)
          == PSP_HEAVY_NOTICE_NONE);
    /* The frame offer open: a page turning OVER does not replace it. */
    ScriptHeavyState shell_over = shell;
    shell_over.page_class = SCRIPT_HEAVY_CLASS_OVER;
    fresh.offer_is_frames = true;
    CHECK(psp_heavy_notice_next(&fresh, &shell_over, &frames)
          == PSP_HEAVY_NOTICE_NONE);
    fresh.offer_is_frames = false;
    CHECK(psp_heavy_notice_next(&fresh, &shell_over, &frames)
          == PSP_HEAVY_NOTICE_OVER_REPLACES_OFFER);
    fresh.on_offer = false;
    fresh.on_page = true;
    shell.answered = true;
    shell.allowed = true;
    shell.waiting = false;
    CHECK(psp_heavy_notice_next(&fresh, &shell, &frames)
          == PSP_HEAVY_NOTICE_FRAME_OFFER);
    /* The scripts-off offer stands first. */
    fresh.scripts_off_offer_open = true;
    CHECK(psp_heavy_notice_next(&fresh, &shell, &frames)
          == PSP_HEAVY_NOTICE_NONE);
    fresh.scripts_off_offer_open = false;

    /* Frames that do not ask (answered, or the page's policy is not Ask)
       are not offered, and an allowed site runs its page silently. */
    frames.asks = false;
    CHECK(psp_heavy_notice_next(&fresh, &shell, &frames)
          == PSP_HEAVY_NOTICE_NONE);
    CHECK(psp_heavy_notice_next(&fresh, &shell, NULL)
          == PSP_HEAVY_NOTICE_NONE);
    PspHeavyNoticeInput allowed = {.on_page = true, .site_runs = true};
    shell.answered = false;
    shell.waiting = true;
    CHECK(psp_heavy_notice_next(&allowed, &shell, NULL)
          == PSP_HEAVY_NOTICE_RUN_SILENTLY);
    return 0;
}

int main(void)
{
    CHECK(test_presentation_sheets_fit_frontend_buffer() == 0);
    CHECK(test_decisions_per_setting() == 0);
    CHECK(test_post_commit_failure_recheck() == 0);
    CHECK(test_recovery_choices() == 0);
    CHECK(test_heavy_notice_order() == 0);
    CHECK(psp_basic_fallback_boot_page_examined(true, false, false)
          && !psp_basic_fallback_boot_page_examined(false, false, false)
          && !psp_basic_fallback_boot_page_examined(true, true, false)
          && !psp_basic_fallback_boot_page_examined(true, false, true));
    puts("psp-basic-fallback-tests: ok");
    return EXIT_SUCCESS;
}
