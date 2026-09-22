/* Frontend half of the declared frame-pump policy: sample the facts the
   table names, then let include/tilefinch/frame_pumps.h decide. Kept out of
   the resident loop so an admission costs that function one call. */

#include "psp_app_internal.h"

/* Sample one fact only when this pump's policy names it, and stop at the
   first one that refuses: an idle frame must not pay for media, network and
   storage predicates no declared relation depends on. The early exit still
   goes through frame_pumps_admit() so order accounting sees every attempt. */
#define REFUSE_IF(fact, condition) \
    do { \
        if ((needed & (fact)) != 0 && (condition)) \
            return frame_pumps_admit(pumps, id, (fact), 0); \
    } while (0)
#define YIELD_IF_ACTIVE(pump, condition) \
    do { \
        if ((yields & FRAME_PUMP_BIT(pump)) != 0 && (condition)) \
            return frame_pumps_admit(pumps, id, 0, FRAME_PUMP_BIT(pump)); \
    } while (0)

bool psp_app_frame_pump_admit(PspApp *app, FramePumpId id,
                              uint32_t local_facts)
{
    const FramePumpPolicy *policy = frame_pump_policy(id);
    if (app == NULL || app->browser == NULL || app->process == NULL
        || app->interactive == NULL || policy == NULL) return false;
    PspBrowserResources *browser = app->browser;
    FramePumpFrame *pumps = &app->interactive->frame_pumps;
    uint32_t needed = policy->blocked_by;
    uint16_t yields = policy->yields_to_active;

    /* The caller already holds these; they cost nothing to test first. */
    if ((local_facts & needed) != 0)
        return frame_pumps_admit(pumps, id, local_facts, 0);

    REFUSE_IF(FRAME_FACT_PAGE_WORK_PAUSED,
              psp_ui_page_work_paused(&app->process->presentation.ui));
    REFUSE_IF(FRAME_FACT_MEDIA_VISIBLE, browser->media.ui.visible);
    REFUSE_IF(FRAME_FACT_MEDIA_PLAYBACK, browser->media.playback != NULL);
    REFUSE_IF(FRAME_FACT_NAVIGATION_PENDING,
              browser_engine_navigation_pending(browser->engine));
    REFUSE_IF(FRAME_FACT_MEDIA_OPEN,
              psp_media_open_work_pending(&browser->media));
    REFUSE_IF(FRAME_FACT_MEDIA_DECODE,
              psp_media_decode_work_pending(&browser->media));
    REFUSE_IF(FRAME_FACT_COOPERATE_ACTIVE, psp_navigation_cooperate_active());
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /* Association is latency-sensitive firmware work; once it has finished,
       the selected-site handshake outranks an optional font read. */
    REFUSE_IF(FRAME_FACT_NETWORK_WARMING,
              psp_network_lifecycle_started(app->network_lifecycle)
                  && !psp_network_lifecycle_ready(app->network_lifecycle));
    REFUSE_IF(FRAME_FACT_PRECONNECT_CONTENDS,
              !psp_network_lifecycle_warming(app->network_lifecycle)
                  && fetch_preconnect_in_flight());
#endif

    /* Pending work is read from its owner at this moment, never inferred
       from an earlier sample: a writer may start and finish between two
       admissions, and its own site records the slice it consumed. */
    YIELD_IF_ACTIVE(FRAME_PUMP_UPDATE_SESSION,
                    psp_update_session_active(&browser->update_session));
    YIELD_IF_ACTIVE(FRAME_PUMP_SCREENSHOT,
                    app->interactive->screenshot.writer.status
                        == SCREENSHOT_PNG_PENDING);
    uint32_t active_download = 0;
    YIELD_IF_ACTIVE(FRAME_PUMP_OFFLINE_DOWNLOAD,
                    offline_download_manager_active(
                        &browser->offline_store.download, &active_download));
    YIELD_IF_ACTIVE(FRAME_PUMP_SITE_RESTORE_STORAGE,
                    app->interactive->site_data_restore.phase
                        == PSP_SITE_DATA_RESTORE_LOCAL_STORAGE);
    YIELD_IF_ACTIVE(FRAME_PUMP_BASELINE_FONTS,
                    !browser_engine_baseline_fonts_ready(browser->engine));
    return frame_pumps_admit(pumps, id, 0, 0);
}

void psp_app_frame_pump_ran(PspApp *app, FramePumpId id)
{
    if (app != NULL && app->interactive != NULL)
        frame_pumps_mark_ran(&app->interactive->frame_pumps, id);
}

/* For a pump whose slice follows admission unconditionally: one call in the
   resident loop both asks and records. */
bool psp_app_frame_pump_run(PspApp *app, FramePumpId id)
{
    if (!psp_app_frame_pump_admit(app, id, 0u)) return false;
    psp_app_frame_pump_ran(app, id);
    return true;
}
