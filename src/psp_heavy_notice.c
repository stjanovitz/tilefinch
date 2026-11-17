#include "tilefinch/psp_heavy_notice.h"

#include <stddef.h>

bool psp_heavy_notice_done(const PspHeavyNoticeInput *input)
{
    return input != NULL && input->on_page && input->offer_shown
        && input->frame_offer_shown
        && (!input->status_shown || input->low_memory_shown);
}

PspHeavyNotice psp_heavy_notice_next(const PspHeavyNoticeInput *input,
                                     const ScriptHeavyState *page,
                                     const NavigationHeavyFrames *frames)
{
    if (input == NULL || page == NULL) return PSP_HEAVY_NOTICE_NONE;
    /* The page's offer is open and a script that arrived since proves the
       app cannot fit (m.vk.ru: the offer opens on its 1.1 MB vendor
       chunk, then the 3.0 MiB application chunk arrives): replace the
       offer with the explanation instead of offering to start what cannot
       run. */
    if (input->on_offer && !input->offer_is_frames && !page->answered
        && page->page_class == SCRIPT_HEAVY_CLASS_OVER)
        return PSP_HEAVY_NOTICE_OVER_REPLACES_OFFER;
    if (!input->on_page) return PSP_HEAVY_NOTICE_NONE;
    if (!input->offer_shown) {
        if (page->waiting) {
            if (input->site_runs) return PSP_HEAVY_NOTICE_RUN_SILENTLY;
            return page->page_class == SCRIPT_HEAVY_CLASS_OVER
                ? PSP_HEAVY_NOTICE_OVER_REFUSE : PSP_HEAVY_NOTICE_OFFER;
        }
        if (page->page_class == SCRIPT_HEAVY_CLASS_OVER
            && page->oversized_scripts != 0)
            return PSP_HEAVY_NOTICE_OVER;
        if (page->refused_scripts != 0)
            return PSP_HEAVY_NOTICE_SETTING_REFUSED;
    }
    /* The scripts-off offer stands until it is answered or ends. */
    if (input->scripts_off_offer_open) return PSP_HEAVY_NOTICE_NONE;
    /* A frame holding a large script is asked about apart from the page,
       whatever the page's own notice was. */
    if (!input->frame_offer_shown && frames != NULL && frames->asks
        && frames->waiting_scripts != 0)
        return PSP_HEAVY_NOTICE_FRAME_OFFER;
    /* (a), or an app shell whose scripts were already running when it
       became heavy: say what it costs once, and offer the stop. */
    if ((page->page_class == SCRIPT_HEAVY_CLASS_CONTENT
         || (page->page_class == SCRIPT_HEAVY_CLASS_SHELL
             && input->shell_settled))
        && !input->status_shown && !page->answered && !input->offer_shown)
        return PSP_HEAVY_NOTICE_SCRIPTS_STATUS;
    if (page->page_class == SCRIPT_HEAVY_CLASS_CONTENT && input->status_shown
        && !input->low_memory_shown
        && script_heavy_state_low_on_memory(page))
        return PSP_HEAVY_NOTICE_LOW_MEMORY;
    return PSP_HEAVY_NOTICE_NONE;
}
