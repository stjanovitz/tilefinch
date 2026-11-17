#ifndef TILEFINCH_PSP_HEAVY_NOTICE_H
#define TILEFINCH_PSP_HEAVY_NOTICE_H

#include <stdbool.h>

#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"

/*
 * Which heavy-page notice the PSP frontend shows next
 * (src/psp_app/psp_app_heavy.c, docs/USER_GUIDE.md "Heavy pages"), kept
 * free of firmware state so the order is a host test. The poll weighs the
 * page (ScriptHeavyState, the top-level realm's) and its frames
 * (NavigationHeavyFrames) and passes what it has already said for this
 * page; the caller does what the answer names and records it.
 */
typedef struct {
    /* The page's own notice (offer, OVER explanation or Heavy pages
       status) has been shown for this page. */
    bool offer_shown;
    /* The page-scripts status, and its low-memory follow-up. */
    bool status_shown;
    bool low_memory_shown;
    /* The frame offer has been shown for this page. */
    bool frame_offer_shown;
    /* The screen: the page, or the heavy offer's own screen (open for the
       page, or for its frames). Any other screen gets no notice. */
    bool on_page;
    bool on_offer;
    bool offer_is_frames;
    /* The site's own heavy policy is Run (session or Always allowance). */
    bool site_runs;
    /* An app shell's grace period is over. */
    bool shell_settled;
    /* The scripts-off offer is standing. */
    bool scripts_off_offer_open;
} PspHeavyNoticeInput;

typedef enum {
    PSP_HEAVY_NOTICE_NONE = 0,
    /* Answer the page's held scripts Run without asking (site allowed). */
    PSP_HEAVY_NOTICE_RUN_SILENTLY,
    /* Open the page's offer. */
    PSP_HEAVY_NOTICE_OFFER,
    /* Answer the page's held scripts No and explain why they cannot fit;
       REPLACE_OFFER does so over the page's open offer. */
    PSP_HEAVY_NOTICE_OVER_REFUSE,
    PSP_HEAVY_NOTICE_OVER_REPLACES_OFFER,
    /* Explain the page's refused oversized scripts (nothing held). */
    PSP_HEAVY_NOTICE_OVER,
    /* Heavy pages = Basic view refused the page's big scripts. */
    PSP_HEAVY_NOTICE_SETTING_REFUSED,
    /* Open the frame offer: a frame on the page holds a large script. */
    PSP_HEAVY_NOTICE_FRAME_OFFER,
    /* The page-scripts status, and its low-memory follow-up. */
    PSP_HEAVY_NOTICE_SCRIPTS_STATUS,
    PSP_HEAVY_NOTICE_LOW_MEMORY
} PspHeavyNotice;

/* Nothing is left to say for this page, so the poll need not weigh it:
   the page's notice and the frame offer have both been shown, and the
   status either was not or its follow-up was too. */
bool psp_heavy_notice_done(const PspHeavyNoticeInput *input);

/* The next notice. `frames` may be NULL (no frame is asked about). The
   page's own notice comes first; the frame offer follows once the page is
   showing again, so a page whose own scripts were refused (OVER) still
   offers its held frames. */
PspHeavyNotice psp_heavy_notice_next(const PspHeavyNoticeInput *input,
                                     const ScriptHeavyState *page,
                                     const NavigationHeavyFrames *frames);

#endif
