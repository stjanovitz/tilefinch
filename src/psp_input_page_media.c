/*
 * focus-page-media for validation input scripts. Compiled into the PSP
 * validation EBOOT and host tests only, never into a shipping build. The
 * choice itself is the engine's (browser_engine_page_media_target), shared
 * with the Cross precedence over page notices.
 */
#include "tilefinch/psp_input_page_media.h"

BrowserPageMediaTarget psp_input_focus_page_media(BrowserEngine *engine)
{
    lxb_dom_node_t *node = NULL;
    BrowserPageMediaTarget target =
        browser_engine_page_media_target(engine, &node);
    if (target == BROWSER_PAGE_MEDIA_NONE || node == NULL
        || !browser_engine_focus_node(engine, node))
        return BROWSER_PAGE_MEDIA_NONE;
    return target;
}

const char *psp_input_page_media_target_name(BrowserPageMediaTarget target)
{
    switch (target) {
    case BROWSER_PAGE_MEDIA_CARD: return "card";
    case BROWSER_PAGE_MEDIA_AUTHOR: return "author";
    case BROWSER_PAGE_MEDIA_AUTHOR_PLAYER: return "author-player";
    case BROWSER_PAGE_MEDIA_ELEMENT: return "element";
    case BROWSER_PAGE_MEDIA_ELEMENT_PLAYER: return "element-player";
    case BROWSER_PAGE_MEDIA_NONE:
    default: return "none";
    }
}
