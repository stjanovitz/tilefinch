/* What a Cross or Enter press on the page ends in. Compiled into
 * psp_app_activation.c, and into tests/test_psp_page_press.c with a real
 * engine and PspUiState, so the order of these outcomes and the status a
 * refused form shows run on the host. It may use only the app fields that
 * test declares.
 */
__attribute__((noinline))
void psp_app_dispatch_activation(
    PspApp *app, PspAppFrameState *frame, bool enter_only,
    const uint16_t *engine_frame)
{
    BrowserEngine *engine = app->browser->engine;
    PspPageActivation activation;
    if (!psp_app_activate_page(
            app, frame, enter_only, engine_frame, &activation)) return;
    if (activation.activated) {
        psp_app_route_controller_action(
            app, frame, engine_frame, &activation.action);
        if (activation.action.page_scripts_stopped)
            psp_ui_show_status(&app->process->presentation.ui,
                               "PAGE SCRIPTS STOPPED - RELOAD TO RETRY", 180);
    } else if (!activation.preview
               && app->views->controller->activations
                      == activation.activations_before) {
        /* Resolution can fail only after the retained target truly
           leaves the current layout. Do not silently consume Cross:
           move to a live target and paint its focus in this frame.
           A failure after a handler began is deliberately excluded
           so no page-side action is followed by an unrelated move. */
        bool recovered = browser_engine_restore_autofocus(engine)
            || browser_engine_focus_move(engine, true);
        if (recovered) {
            frame->page_dirty = true;
        } else {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "PAGE ITEM IS NO LONGER AVAILABLE", 120);
        }
    } else if (activation.action.form_not_sent) {
        psp_ui_show_status(&app->process->presentation.ui,
                           "FORM COULD NOT BE SENT", 180);
    }
}
