#ifndef TILEFINCH_PSP_YOUTUBE_LOGIN_CONSENT_H
#define TILEFINCH_PSP_YOUTUBE_LOGIN_CONSENT_H

#include <stdbool.h>
#include <stdint.h>

#include "tilefinch/psp_ui.h"
#include "tilefinch/youtube_login.h"

/*
 * The PSP frontend's consent rules for saving a YouTube login to the Memory
 * Stick (psp_app_youtube.c applies them). Kept free of engine, session and
 * firmware state so they are a host test.
 */

/* What an answer to the consent offer asks for. */
typedef struct {
    /* YOUTUBE_LOGIN_* to remember (see psp_youtube_login_answer_policy). */
    unsigned policy;
    /* Write the login the offer was bound to, now. */
    bool save;
} PspYoutubeLoginAnswer;

/* OFFER_BOUND is whether the offer is bound to a verified login: Always
   chosen from Settings while signed out has nothing to write yet, so it is
   remembered and the next verified sign-in saves under it. */
PspYoutubeLoginAnswer psp_youtube_login_answer(PspUiAction action,
                                               bool offer_bound);

/* A save writes only the login the offer was bound to, while it is still
   the verified, signed-in login, and only when site data may persist. */
bool psp_youtube_login_save_allowed(bool persistent_site_data,
                                    uint64_t offer_identity,
                                    uint64_t verified_identity,
                                    bool signed_in);

/* The policy remembered once the save (if any) is known: an Always whose
   credential write failed is never remembered. */
unsigned psp_youtube_login_answer_policy(PspYoutubeLoginAnswer answer,
                                         bool saved);

/* The Settings row. Always still needs the warning and a deliberate
   confirmation: it stores Ask and opens the consent offer. Never removes
   any saved login. */
typedef struct {
    unsigned stored_policy;
    bool open_offer;
    bool clear_saved;
} PspYoutubeLoginSetting;

PspYoutubeLoginSetting psp_youtube_login_setting(unsigned policy);

#endif
