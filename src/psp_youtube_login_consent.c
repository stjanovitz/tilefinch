#include "tilefinch/psp_youtube_login_consent.h"

PspYoutubeLoginAnswer psp_youtube_login_answer(PspUiAction action,
                                               bool offer_bound)
{
    PspYoutubeLoginAnswer answer = { .policy = YOUTUBE_LOGIN_ASK };
    switch (action) {
        case PSP_UI_ACTION_YOUTUBE_LOGIN_SAVE: answer.save = true; break;
        case PSP_UI_ACTION_YOUTUBE_LOGIN_ALWAYS:
            answer.policy = YOUTUBE_LOGIN_ALWAYS;
            answer.save = offer_bound;
            break;
        case PSP_UI_ACTION_YOUTUBE_LOGIN_NEVER:
            answer.policy = YOUTUBE_LOGIN_NEVER;
            break;
        default: break;
    }
    return answer;
}

bool psp_youtube_login_save_allowed(bool persistent_site_data,
                                    uint64_t offer_identity,
                                    uint64_t verified_identity,
                                    bool signed_in)
{
    return persistent_site_data && offer_identity != 0
        && offer_identity == verified_identity && signed_in;
}

unsigned psp_youtube_login_answer_policy(PspYoutubeLoginAnswer answer,
                                         bool saved)
{
    if (answer.policy == YOUTUBE_LOGIN_ALWAYS && !saved)
        return YOUTUBE_LOGIN_ASK;
    return answer.policy;
}

PspYoutubeLoginSetting psp_youtube_login_setting(unsigned policy)
{
    PspYoutubeLoginSetting setting = { .stored_policy = policy };
    if (policy == YOUTUBE_LOGIN_ALWAYS) {
        setting.stored_policy = YOUTUBE_LOGIN_ASK;
        setting.open_offer = true;
    } else if (policy == YOUTUBE_LOGIN_NEVER) {
        setting.clear_saved = true;
    }
    return setting;
}
