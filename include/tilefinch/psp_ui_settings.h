#ifndef TILEFINCH_PSP_UI_SETTINGS_H
#define TILEFINCH_PSP_UI_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/psp_ui.h"

/*
 * A plain on/off setting: the Options toggle stores it in the profile, and
 * in the engine when it has an engine half, marks the profile dirty and
 * shows one of two toasts. Settings whose apply path does more than this
 * keep a handler of their own in src/psp_app/psp_app_settings.c.
 */
typedef struct {
    PspUiSettingId id;
    bool (*profile_get)(const BrowserProfile *profile);
    void (*profile_set)(BrowserProfile *profile, bool enabled);
    /* Writes the chrome's mirror of the setting; NULL when it keeps none. */
    void (*ui_set)(PspUiState *ui, bool enabled);
    /* Optional engine half, applied after the profile accepted the value.
       False means the engine refused and kept its value. */
    bool (*engine_set)(BrowserEngine *engine, bool enabled);
    /* The engine's value; set exactly when engine_set is. Boot reads it
       when the engine refuses the profile's value. */
    bool (*engine_get)(const BrowserEngine *engine);
    const char *on_status;
    const char *off_status;
    uint16_t status_frames;
} PspBoolSetting;

/* The descriptor for `id`, or NULL when that setting is not a plain
   on/off setting. */
const PspBoolSetting *psp_bool_setting_find(PspUiSettingId id);
/* Walk the table, e.g. from tests. */
const PspBoolSetting *psp_bool_settings(size_t *count);

/*
 * The failure policy for a setting with a profile half and an engine half:
 *
 * - The profile is written first. It is plain memory, so it can be put
 *   back, and a value it refuses never reaches the engine.
 * - The engine is written second. An engine that refuses keeps its value,
 *   and the profile is put back to match, so a refused value is never
 *   persisted.
 * - The chrome's copy is then written from the profile, so the menu shows
 *   the value in effect rather than the one the user stepped to.
 * - The toast names the value in effect: the new one on success, the kept
 *   one on failure. These settings have no failure toast in the published
 *   interface catalog, so a failure reads as the unchanged value.
 *
 * The caller marks the profile dirty only for PSP_SETTING_APPLIED.
 *
 * Boot and Night mode's clock refresh follow the same policy without a
 * toast. At boot the value the engine kept is the engine's own, so a
 * refused setting is brought back to it in the profile and the chrome.
 */
typedef enum {
    PSP_SETTING_APPLIED = 0,
    PSP_SETTING_PROFILE_REFUSED,
    PSP_SETTING_ENGINE_REFUSED,
    /* The value asked for was already in effect; nothing was written. */
    PSP_SETTING_UNCHANGED
} PspSettingResult;

/* Apply a plain on/off setting chosen in Options under the policy above.
   Its profile setter returns nothing, so a refusal is a value that does not
   read back. */
PspSettingResult psp_bool_setting_apply(
    const PspBoolSetting *setting, BrowserProfile *profile,
    BrowserEngine *engine, PspUiState *ui, bool enabled);
/* Website fonts: On, Off or Smart, in the profile and the engine. */
PspSettingResult psp_web_font_setting_apply(
    BrowserProfile *profile, BrowserEngine *engine, PspUiState *ui,
    WebFontMode mode);
/* Night mode: the profile keeps the mode, the engine its result. The caller
   has already set ui->page_dark for `mode` (it depends on the clock);
   `previous_dark` is the value it replaced, which a refusal restores along
   with ui->color_mode. */
PspSettingResult psp_color_mode_setting_apply(
    BrowserProfile *profile, BrowserEngine *engine, PspUiState *ui,
    BrowserColorMode mode, bool previous_dark);

/* Night mode's clock refresh in the frame loop. The caller has recomputed
   ui->page_dark for ui->color_mode; `was_dark` is the value it replaced.
   Tells the engine when the result changed, without a toast, and returns
   true when the page changed appearance. An engine that refuses keeps the
   old appearance, so ui->page_dark goes back to `was_dark` and the next
   refresh tries again. Costs one comparison when nothing changed. */
bool psp_color_mode_refresh_apply(
    BrowserEngine *engine, PspUiState *ui, bool was_dark);
/* Boot: hand the profile's Website fonts mode to the engine. Called before
   the chrome is synced from the profile; an engine that refuses leaves the
   profile on the engine's mode. */
PspSettingResult psp_web_font_setting_boot(
    BrowserProfile *profile, BrowserEngine *engine);
/* Boot: hand Night mode's result (ui->page_dark, already computed) and the
   on/off rows with an engine half to the engine, after the chrome was
   synced from the profile. A refused on/off row takes the engine's value
   in the profile and the chrome. A refused Night mode puts ui->page_dark
   on the engine's appearance; a Light or Dark mode that contradicts it
   becomes the other one, while Auto stays Auto (its result depends on the
   clock, and the next refresh tries again). Shows no toast; the caller
   does not mark the profile dirty. Returns how many settings the engine
   refused. */
unsigned psp_engine_settings_boot(
    BrowserProfile *profile, BrowserEngine *engine, PspUiState *ui);
/* This site's JavaScript switch, for the page at ui->url. On
   PSP_SETTING_APPLIED the caller marks the profile dirty and reloads; any
   other result has already shown its toast. A refusal by either half shows
   the unavailable toast; a value already in effect shows that value. A
   pending load is cancelled only once the profile took a change (the
   engine takes no policy mid-load), so an unchanged value or a profile
   refusal leaves it running. */
PspSettingResult psp_site_javascript_setting_apply(
    BrowserProfile *profile, BrowserEngine *engine, PspUiState *ui,
    bool enabled);
/* After a page's scripts ran out of memory (R5): the next visit to its
   site offers to load it without scripts, never by itself.
   psp_scripts_off_note_stop() remembers the stop for ui->url's site while
   its scripts are on (true when the profile changed: mark it dirty).
   psp_scripts_off_offer_due() says whether ui->url's page should offer:
   its site was noted and JavaScript is on, globally and for the site.
   psp_scripts_off_offer_answer() takes the reader's answer and forgets the
   note either way (another stop notes it again). Accepting turns the
   site's JavaScript off through psp_site_javascript_setting_apply(); on
   PSP_SETTING_APPLIED the caller marks the profile dirty and reloads.
   Declining returns PSP_SETTING_APPLIED when the note was forgotten (mark
   dirty, no reload), PSP_SETTING_UNCHANGED otherwise. */
bool psp_scripts_off_note_stop(BrowserProfile *profile, const PspUiState *ui);
bool psp_scripts_off_offer_due(const BrowserProfile *profile,
                               const PspUiState *ui);
PspSettingResult psp_scripts_off_offer_answer(
    BrowserProfile *profile, BrowserEngine *engine, PspUiState *ui,
    bool accept);
/* The offer within one browser session (user decision 2026-11-14): an
   offer that ends without an answer (it runs out, or the reader goes on to
   another page) was ignored, and its site does not offer again until the
   browser restarts. psp_scripts_off_session_due() is
   psp_scripts_off_offer_due() for a site not ignored this session;
   psp_scripts_off_session_shown() records the offer just shown for ui->url;
   psp_scripts_off_session_settle() runs every poll (page_changed when a new
   page committed) and notes an offer that ended unanswered;
   psp_scripts_off_session_answered() takes an answer. An answer forgets the
   site's note either way, so an offer noted as ignored after its answer
   changes nothing. */
#define PSP_SCRIPTS_OFF_IGNORED_SITE_LIMIT 8u
typedef struct {
    char pending[CONTENT_BLOCKER_HOST_LIMIT];
    size_t ignored_count;
    char ignored[PSP_SCRIPTS_OFF_IGNORED_SITE_LIMIT][CONTENT_BLOCKER_HOST_LIMIT];
} PspScriptsOffSession;
bool psp_scripts_off_session_due(const PspScriptsOffSession *session,
                                 const BrowserProfile *profile,
                                 const PspUiState *ui);
void psp_scripts_off_session_shown(PspScriptsOffSession *session,
                                   const PspUiState *ui);
void psp_scripts_off_session_settle(PspScriptsOffSession *session,
                                    const PspUiState *ui, bool page_changed);
void psp_scripts_off_session_answered(PspScriptsOffSession *session);
/* Basic view fallback: Automatic, Ask or Off, in the profile only. The
   toast names the mode in effect. */
PspSettingResult psp_basic_fallback_setting_apply(
    BrowserProfile *profile, PspUiState *ui, BrowserBasicFallbackMode mode);

/* Copy every chrome setting the profile alone determines into `ui`. Boot
   calls it once before the first frame; settings that also depend on the
   start page, the boot configuration or a live session are set by the
   caller. */
void psp_app_ui_sync_from_profile(
    PspUiState *ui, const BrowserProfile *profile);

#endif
