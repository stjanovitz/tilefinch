/* The PSP frontend's consent rules for saving a YouTube login
   (psp_youtube_login_consent.h), answer by answer: what is written, and
   what is remembered. */
#include "tilefinch/psp_youtube_login_consent.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* The consent offer answered with ACTION: the policy remembered, and
   whether a write happened. OFFERED is the verified login the offer was
   bound to (0 when opened from Settings while signed out); VERIFIED is the
   login verified when the answer arrives. */
static unsigned answer(PspUiAction action, uint64_t offered,
                       uint64_t verified, bool *wrote)
{
    PspYoutubeLoginAnswer chosen =
        psp_youtube_login_answer(action, offered != 0);
    *wrote = chosen.save
        && psp_youtube_login_save_allowed(true, offered, verified,
                                          verified != 0);
    bool saved = !chosen.save || *wrote;
    return psp_youtube_login_answer_policy(chosen, saved);
}

int main(void)
{
    bool wrote = false;
    /* Signed in: both save answers write the verified login; only Always
       is remembered. */
    CHECK(answer(PSP_UI_ACTION_YOUTUBE_LOGIN_ALWAYS, 7, 7, &wrote)
              == YOUTUBE_LOGIN_ALWAYS && wrote);
    CHECK(answer(PSP_UI_ACTION_YOUTUBE_LOGIN_SAVE, 7, 7, &wrote)
              == YOUTUBE_LOGIN_ASK && wrote);
    /* Signed out: nothing to write, but Always is remembered so the next
       verified sign-in saves without asking. */
    CHECK(answer(PSP_UI_ACTION_YOUTUBE_LOGIN_ALWAYS, 0, 0, &wrote)
              == YOUTUBE_LOGIN_ALWAYS && !wrote);
    /* A different login by the time of the answer is never saved, and the
       Always whose write failed is not remembered. */
    CHECK(answer(PSP_UI_ACTION_YOUTUBE_LOGIN_ALWAYS, 7, 8, &wrote)
              == YOUTUBE_LOGIN_ASK && !wrote);
    CHECK(answer(PSP_UI_ACTION_YOUTUBE_LOGIN_SAVE, 7, 8, &wrote)
              == YOUTUBE_LOGIN_ASK && !wrote);
    CHECK(answer(PSP_UI_ACTION_YOUTUBE_LOGIN_NOT_NOW, 7, 7, &wrote)
              == YOUTUBE_LOGIN_ASK && !wrote);
    CHECK(answer(PSP_UI_ACTION_YOUTUBE_LOGIN_NEVER, 7, 7, &wrote)
              == YOUTUBE_LOGIN_NEVER && !wrote);

    /* Nothing is written without persistent site data or a signed-in
       session, or for an offer bound to no login. */
    CHECK(psp_youtube_login_save_allowed(true, 7, 7, true));
    CHECK(!psp_youtube_login_save_allowed(false, 7, 7, true));
    CHECK(!psp_youtube_login_save_allowed(true, 7, 7, false));
    CHECK(!psp_youtube_login_save_allowed(true, 0, 0, true));

    /* Settings: Always waits for the warning's confirmation (stores Ask and
       opens the offer); Never removes any saved login; Ask just stores. */
    PspYoutubeLoginSetting setting =
        psp_youtube_login_setting(YOUTUBE_LOGIN_ALWAYS);
    CHECK(setting.stored_policy == YOUTUBE_LOGIN_ASK && setting.open_offer
          && !setting.clear_saved);
    setting = psp_youtube_login_setting(YOUTUBE_LOGIN_NEVER);
    CHECK(setting.stored_policy == YOUTUBE_LOGIN_NEVER && !setting.open_offer
          && setting.clear_saved);
    setting = psp_youtube_login_setting(YOUTUBE_LOGIN_ASK);
    CHECK(setting.stored_policy == YOUTUBE_LOGIN_ASK && !setting.open_offer
          && !setting.clear_saved);
    return 0;
}
