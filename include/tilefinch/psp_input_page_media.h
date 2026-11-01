#ifndef TILEFINCH_PSP_INPUT_PAGE_MEDIA_H
#define TILEFINCH_PSP_INPUT_PAGE_MEDIA_H

#include "tilefinch/browser_engine.h"

/* Validation input scripts only (focus-page-media): never linked into a
   shipping EBOOT. Moves d-pad focus to what Play would act on
   (browser_engine_page_media_target), so a following Cross plays it
   through the user path. NONE leaves focus where it was. */
BrowserPageMediaTarget psp_input_focus_page_media(BrowserEngine *engine);
const char *psp_input_page_media_target_name(BrowserPageMediaTarget target);

#endif
