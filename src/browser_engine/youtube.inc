/* YouTube provider policy owned by the engine facade: the feed, like,
   account and sign-in state machines, their native controls and notices,
   and the idle pumps that publish provider pages. browser_engine.c includes
   this file once, ahead of browser_engine_run_idle_work; the hooks the
   facade calls before that point are declared near the top of
   browser_engine.c. The facade's generic paths reach this state only
   through the hooks at the end of the file and the idle steps. */

static void browser_engine_clear_youtube_work(BrowserEngine *engine)
{
#ifndef __PSP__
    engine->youtube.history_metadata = 0u;
#endif
    youtube_lite_load_destroy(engine->youtube.topics_job);
    engine->youtube.topics_job = NULL;
    youtube_lite_feed_destroy(engine->youtube.feed_job);
    engine->youtube.feed_job = NULL;
    youtube_lite_feed_destroy(engine->youtube.like_job);
    engine->youtube.like_job = NULL;
    youtube_lite_feed_destroy(engine->youtube.account_job);
    engine->youtube.account_job = NULL;
    if (engine->youtube.account_state == BROWSER_YOUTUBE_FEED_LOADING)
        engine->youtube.account_state = BROWSER_YOUTUBE_FEED_FAILED;
    if (engine->youtube.like_state == BROWSER_YOUTUBE_FEED_LOADING)
        engine->youtube.like_state = BROWSER_YOUTUBE_FEED_FAILED;
    youtube_lite_document_destroy(&engine->youtube.feed_pending);
    engine->youtube.feed_pending_identity = 0;
    if (engine->youtube.feed_state == BROWSER_YOUTUBE_FEED_LOADING
        || engine->youtube.feed_state == BROWSER_YOUTUBE_FEED_READY)
        engine->youtube.feed_state = BROWSER_YOUTUBE_FEED_FAILED;
}

static void browser_engine_youtube_browsing(BrowserEngine *engine)
{
    if (youtube_lite_route(engine->navigation.page.document_url) != YOUTUBE_LITE_ROUTE_HOME)
        return;
    engine->youtube.browsing_generation = engine->navigation.generation;
    engine->youtube.browsing = true;
}

static bool browser_engine_youtube_result_focused(BrowserEngine *engine)
{
    if (youtube_lite_route(engine->navigation.page.document_url) != YOUTUBE_LITE_ROUTE_HOME)
        return false;
    /* Header and refresh order may change; a numeric link index cannot
       distinguish browsing a result from focusing a header action. */
    lxb_dom_node_t *node = controller_focused_node(&engine->controller);
    for (unsigned depth = 0; node != NULL && depth < 8u; depth++, node = node->parent) {
        size_t length = 0;
        const char *name = document_attribute(node, "class", &length);
        if (name != NULL && ((length == 4u && memcmp(name, "card", 4u) == 0)
            || (length == 10u && memcmp(name, "result-row", 10u) == 0))) return true;
        if (name != NULL && length == 12u && memcmp(name, "account-menu", 12u) == 0
            && lxb_dom_element_has_attribute(lxb_dom_interface_element(node),
                (const lxb_char_t *) "open", 4u)) return true;
    }
    return false;
}

BrowserYoutubeFeedState browser_engine_youtube_feed_state(const BrowserEngine *engine)
{
    return engine == NULL ? BROWSER_YOUTUBE_FEED_IDLE : engine->youtube.feed_state;
}

BrowserYoutubeFeedState browser_engine_youtube_like_state(const BrowserEngine *engine)
{
    return engine == NULL ? BROWSER_YOUTUBE_FEED_IDLE : engine->youtube.like_state;
}

bool browser_engine_youtube_like_selected(const BrowserEngine *engine)
{
    return engine != NULL && engine->youtube.like_selected;
}

BrowserYoutubeFeedState browser_engine_youtube_account_state(const BrowserEngine *engine)
{
    return engine == NULL ? BROWSER_YOUTUBE_FEED_IDLE : engine->youtube.account_state;
}

#ifndef __PSP__
unsigned browser_engine_youtube_history_metadata(const BrowserEngine *engine)
{
    return engine == NULL ? 0u : engine->youtube.history_metadata;
}

bool browser_engine_youtube_rating_diagnostics(const BrowserEngine *engine,
    YoutubeLiteRatingDiagnostics *result)
{
    if (engine == NULL || result == NULL) return false;
    *result = engine->youtube.rating_diagnostics;
    return true;
}

bool browser_engine_youtube_account_diagnostics(const BrowserEngine *engine,
    YoutubeLiteAccountDiagnostics *result)
{
    if (engine == NULL || result == NULL) return false;
    *result = engine->youtube.account_diagnostics;
    return true;
}
#endif

/* Bind account mutations only to a control we committed from the native
   provider, never to an author-supplied fragment or public DOM marker. */
static lxb_dom_node_t *browser_engine_history_walk_next(
    lxb_dom_node_t *root, lxb_dom_node_t *node);

static void browser_engine_bind_youtube_controls(BrowserEngine *engine)
{
    engine->youtube.like_node = NULL;
    engine->youtube.unlike_node = NULL;
    engine->youtube.signout_node = NULL;
    engine->youtube.account_node_count = 0;
    engine->youtube.like_node_generation = engine->navigation.generation;
    if (youtube_lite_route(engine->navigation.page.document_url) == YOUTUBE_LITE_ROUTE_NONE)
        return;
    /* Bind the committed provider tree, including controls in disclosures.
       Opening them later changes visibility, not provenance. */
    lxb_dom_node_t *root = lxb_dom_interface_node(engine->navigation.page.document.html);
    lxb_dom_node_t *node = root;
    for (unsigned visited = 0u; node != NULL && visited < BROWSER_ENGINE_DOM_WALK_LIMIT;
         visited++, node = browser_engine_history_walk_next(root, node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->local_name != LXB_TAG_A) continue;
        size_t action_length = 0;
        const char *account_action = document_attribute(node,
            "data-yt-action", &action_length);
        if (account_action != NULL && action_length == 1u
            && account_action[0] >= '1' && account_action[0] <= '4'
            && engine->youtube.account_node_count < 16u)
            engine->youtube.account_nodes[engine->youtube.account_node_count++] = node;
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && length == sizeof("yt-like-action") - 1u
            && memcmp(id, "yt-like-action", length) == 0) {
            engine->youtube.like_node = node;
        } else if (id != NULL && length == sizeof("yt-unlike-action") - 1u
            && memcmp(id, "yt-unlike-action", length) == 0) {
            engine->youtube.unlike_node = node;
        } else if (id != NULL && length == sizeof("yt-signout-action") - 1u
            && memcmp(id, "yt-signout-action", length) == 0) {
            engine->youtube.signout_node = node;
        }
    }
}

static bool browser_engine_youtube_notice_paint(BrowserEngine *engine,
                                               const char *name, const char *label)
{
    LayoutDocument *layout = &engine->navigation.page.layout;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        LayoutNodeBox *box = &layout->node_boxes[i];
        size_t length = 0;
        const char *id = document_attribute(box->node, "id", &length);
        if (id == NULL || length != strlen(name) || memcmp(id, name, length) != 0)
            continue;
        /* Replacing logical runs with a pre-shaped notice invalidates their
           old bidi offsets. Keep unrelated sidecars and their glyph storage. */
        size_t retained = 0;
        for (size_t at = 0; at < layout->bidi_command_count; at++) {
            LayoutBidiCommand value = layout->bidi_commands[at];
            if (value.command_index >= box->command_start
                && value.command_index < box->command_end) continue;
            layout->bidi_commands[retained++] = value;
        }
        bool changed = retained != layout->bidi_command_count;
        layout->bidi_command_count = retained;
        bool first_text = true;
        for (size_t at = box->command_start; at < box->command_end && at < layout->count; at++) {
            DrawCommand *command = &layout->commands[at];
            if (command->type != DRAW_TEXT) continue;
            /* Inline layout emits a run per word. Publish the notice once,
               not once at every old word position. The reserved line keeps
               result geometry and the user's focus stable. */
            const char *text = first_text ? label : "";
            uint32_t text_length = first_text ? (uint32_t) strlen(label) : 0u;
            int width = first_text ? box->width : 0;
            int x = box->x;
            if (first_text && tilefinch_ui_translation_bound_language() == TILEFINCH_UI_LANGUAGE_ARABIC) {
                const FontFace *face = font_context_face(layout->fonts,
                    layout->web_fonts, draw_command_font_family(command));
                int fixed = font_text_width_at_size_fixed(face, text, text_length,
                    draw_command_text_font_size_fixed(command), false);
                width = (fixed + 63) / 64;
                if (width > box->width) width = box->width;
                x += box->width - width;
            }
            if (command->text != text || command->text_length != text_length
                || command->width != width || command->x != x) {
                command->text = text;
                command->text_length = text_length;
                command->width = width;
                command->radius &= ~LAYOUT_TEXT_RTL;
                draw_command_set_text_x_fixed(command, x * 64);
                changed = true;
            }
            first_text = false;
        }
        if (changed) tile_cache_invalidate_rect(&engine->render,
            box->x, box->y, box->x + box->width, box->y + box->height);
        return changed;
    }
    return false;
}

static bool browser_engine_youtube_status_paint(BrowserEngine *engine)
{
    if (!engine->render_ready || youtube_lite_route(engine->navigation.page.document_url)
            != YOUTUBE_LITE_ROUTE_HOME) return false;
    if (engine->youtube.status_painted_state == engine->youtube.feed_state
        && engine->youtube.status_layout_generation == engine->navigation.incremental_relayouts
        && engine->youtube.status_shell == engine->render_shell_serial) return false;
    static const char *const labels[] = {
        "Search above or select Refresh.", "Reloading suggestions...",
        "New suggestions ready - select Refresh.", "Suggestions up to date.",
        "Suggestions unavailable - try Refresh.", "Sign in to view your subscriptions."
    };
    bool changed = browser_engine_youtube_notice_paint(engine, "yt-feed-status",
        tilefinch_ui_text(labels[engine->youtube.feed_state]));
    engine->youtube.status_painted_state = engine->youtube.feed_state;
    engine->youtube.status_layout_generation = engine->navigation.incremental_relayouts;
    engine->youtube.status_shell = engine->render_shell_serial;
    return changed;
}

bool browser_engine_set_youtube_compact_results(
    BrowserEngine *engine, bool compact)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE) {
        return false;
    }
    engine->youtube.compact_results = compact;
    clear_error(engine);
    return true;
}

bool browser_engine_youtube_compact_results(const BrowserEngine *engine)
{
    return engine != NULL && engine->youtube.compact_results;
}

bool browser_engine_youtube_topics(const BrowserEngine *engine)
{
    return engine == NULL || engine->youtube.topics;
}

bool browser_engine_set_youtube_topics(BrowserEngine *engine, bool enabled)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE) return false;
    if (engine->youtube.topics == enabled) return true;
    engine->youtube.topics = enabled;
    if (!enabled) {
        if (engine->youtube.feed_job == NULL
            && !youtube_lite_session_signed_in(&engine->session))
            browser_engine_clear_youtube_work(engine);
    } else {
        engine->youtube.topics_attempted = false;
    }
    return true;
}

unsigned browser_engine_take_youtube_login_events(BrowserEngine *engine)
{
    if (engine == NULL) return 0;
    unsigned events = engine->youtube.login_events;
    engine->youtube.login_events = 0;
    return events;
}

bool browser_engine_youtube_login_verified(const BrowserEngine *engine)
{
    if (engine == NULL || engine->youtube.login_verified == 0) return false;
    return engine->youtube.login_verified == youtube_lite_session_login_identity(
        (BrowserSession *) &engine->session);
}

uint64_t browser_engine_youtube_login_identity(const BrowserEngine *engine)
{
    return browser_engine_youtube_login_verified(engine) ? engine->youtube.login_verified : 0;
}

static bool browser_engine_youtube_signin_pending(const BrowserEngine *engine)
{
    return engine->youtube.signin_state >= YOUTUBE_SIGNIN_ACCOUNT
        && engine->youtube.signin_state <= YOUTUBE_SIGNIN_VERIFYING;
}

void browser_engine_set_youtube_signin_diagnostics(BrowserEngine *engine, bool enabled)
{
    if (engine == NULL) return;
    engine->youtube.signin_diagnostics = enabled;
    engine->youtube.signin_feed = (YoutubeLiteFeedDiagnostics) {0};
    engine->youtube.handoff_http_status = 0;
    engine->youtube.handoff_service_origin = false;
    engine->youtube.feed_build_outcome = engine->youtube.feed_publication_outcome = 0;
    engine->session.cookie_diagnostics = (BrowserCookieDiagnostics) {.enabled = enabled};
}

bool browser_engine_youtube_signin_report(const BrowserEngine *engine, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) return false;
    out[0] = '\0';
    if (engine == NULL || !engine->youtube.signin_diagnostics) return false;
    const YoutubeLiteFeedDiagnostics *feed = &engine->youtube.signin_feed;
    const BrowserCookieDiagnostics *cookies = &engine->session.cookie_diagnostics;
    uint64_t identity = youtube_lite_session_login_identity((BrowserSession *) &engine->session);
    /* Only enum/boolean/count fields, never generic errors or identity hashes. */
    int n = snprintf(out, capacity,
        "Tilefinch YouTube sign-in diagnostics v2\n"
        "stage=%u recovery-attempts=%u scripts-enabled=%u runtime-present=%u degraded=%u\n"
        "handoff-http-status=%ld handoff-service-origin=%u\n"
        "dynamic-transport-failures=%zu dynamic-authority-failures=%zu dynamic-source-failures=%zu allocation-refusals=%zu\n"
        "session-cookie-present=%u server-verified=%u feed-state=%u\n"
        "parsed-results=%zu pending-publication=%u\n"
        "build-outcome=%u publication-outcome=%u continue-available=%u\n"
        "bootstrap-status=%ld api-status=%ld history-status=%ld failure-kind=%u\n"
        "configuration-valid=%u clock-available=%u api-request-admitted=%u history-outcome=%u\n"
        "feed-phase=%u http-status=%ld transported=%u same-origin=%u bootstrap-signed-in=%u auth-index-known=%u request-signed=%u api-authenticated=%u\n"
        "transport-code=%ld transport-timed-out=%u tls-result-available=%u tls-result=%ld tls-failed=%u\n"
        "cookie-refusals=%zu byte-refusals=%zu slot-refusals=%zu allocation-refusals=%zu policy-ignored=%zu evictions=%zu\n",
        engine->youtube.signin_state, engine->youtube.signin_retries,
        engine->navigation.scripts_enabled, engine->navigation.page.runtime != NULL,
        engine->navigation.page.script_degradation_observed,
        engine->youtube.handoff_http_status >= 100 && engine->youtube.handoff_http_status <= 599
            ? engine->youtube.handoff_http_status : 0L, engine->youtube.handoff_service_origin,
        engine->navigation.page.script_result.dynamic_script_transport_failures,
        engine->navigation.page.script_result.dynamic_script_authority_failures,
        engine->navigation.page.script_result.dynamic_script_source_failures,
        engine->budget.failure_count,
        identity != 0, identity != 0 && engine->youtube.login_verified == identity,
        engine->youtube.feed_state,
        feed->result_count, engine->youtube.feed_pending.html != NULL,
        engine->youtube.feed_build_outcome, engine->youtube.feed_publication_outcome,
        browser_engine_youtube_signin_continue_available(engine),
        feed->bootstrap_status >= 100 && feed->bootstrap_status <= 599 ? feed->bootstrap_status : 0L,
        feed->api_status >= 100 && feed->api_status <= 599 ? feed->api_status : 0L,
        feed->history_status >= 100 && feed->history_status <= 599 ? feed->history_status : 0L,
        feed->failure,
        feed->configuration_valid, feed->clock_available, feed->request_admitted, feed->history_outcome,
        feed->phase, feed->http_status >= 100 && feed->http_status <= 599 ? feed->http_status : 0L,
        feed->transported, feed->same_origin, feed->bootstrap_signed_in,
        feed->auth_user_known, feed->request_signed, feed->api_authenticated,
        feed->transport_code, feed->transport_timed_out,
        feed->tls_verify_result_available, feed->tls_verify_result, feed->tls_verification_failed,
        cookies->refused, cookies->byte_refusals, cookies->slot_refusals,
        cookies->allocation_refusals, cookies->policy_ignored, cookies->evictions);
    if (n < 0 || (size_t) n >= capacity) { out[0] = '\0'; return false; }
    return true;
}

static const char youtube_signin_entry[] = YOUTUBE_LITE_SIGNIN_URL;

static bool browser_engine_youtube_auth_origin(const char *url)
{
    return tilefinch_url_same_origin(url, "https://accounts.google.com/")
        || tilefinch_url_same_origin(url, "https://support.google.com/")
        || tilefinch_url_same_origin(url, "https://accounts.youtube.com/");
}

bool browser_engine_youtube_signin_continue_available(const BrowserEngine *engine)
{
    return engine != NULL && engine->navigation.page.loaded
        && !browser_engine_navigation_pending(engine)
        && (browser_engine_youtube_signin_pending(engine)
            || engine->youtube.signin_state == YOUTUBE_SIGNIN_FAILED
            || engine->youtube.signin_state == YOUTUBE_SIGNIN_MANUAL)
        && browser_engine_youtube_auth_origin(engine->navigation.page.document_url);
}

const char *browser_engine_youtube_signin_continue_target(const BrowserEngine *engine)
{
    if (!browser_engine_youtube_signin_continue_available(engine)) return NULL;
    /* Existing service cookies may belong to an earlier account or a revoked
       session. Always finish the current account flow through its stable GET. */
    return youtube_signin_entry;
}

static void browser_engine_note_youtube_signin_navigation(BrowserEngine *engine, const char *url)
{
    if (strcmp(url, "https://www.youtube.com/?tilefinch_signin=1") == 0
        && browser_engine_youtube_signin_continue_available(engine)) {
        engine->youtube.signin_state = YOUTUBE_SIGNIN_HANDOFF;
    } else if (strcmp(url, youtube_signin_entry) == 0
        || strcmp(url, YOUTUBE_LITE_ACCOUNT_CHOOSER_URL) == 0) {
        engine->youtube.signin_retries = 0;
        engine->youtube.signin_state = YOUTUBE_SIGNIN_ACCOUNT;
        engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_STARTED;
        engine->youtube.handoff_http_status = 0;
        engine->youtube.handoff_service_origin = false;
    } else if (browser_engine_youtube_signin_pending(engine)) {
        if (youtube_lite_signin_handoff_url(url)) engine->youtube.signin_state = YOUTUBE_SIGNIN_HANDOFF;
        else if (!browser_engine_youtube_auth_origin(url)
            && youtube_lite_route(url) == YOUTUBE_LITE_ROUTE_NONE)
            engine->youtube.signin_state = YOUTUBE_SIGNIN_IDLE;
    }
}

/* Native control activations. Each provider control links to a
   same-document fragment from youtube_lite.h; activating it runs the
   provider command instead of a fragment scroll. A command whose own
   conditions do not hold leaves the activation to the generic path. */
static bool browser_engine_youtube_account_command(BrowserEngine *engine,
                                                   ControllerAction *action)
{
    lxb_dom_node_t *node = controller_focused_node(&engine->controller);
    bool owned = false;
    for (size_t i = 0; i < engine->youtube.account_node_count; i++)
        owned |= engine->youtube.account_nodes[i] == node;
    if (!owned) return false;
    if (engine->youtube.account_job == NULL) {
#ifndef __PSP__
        memset(&engine->youtube.account_diagnostics, 0, sizeof(engine->youtube.account_diagnostics));
#endif
        char target[128] = {0}, video[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        size_t length = 0;
        const char *value = document_attribute(node, "data-yt-target", &length);
        bool valid = value != NULL && length < sizeof(target);
        if (valid) memcpy(target, value, length);
        value = document_attribute(node, "data-yt-video", &length);
        valid = valid && value != NULL && length < sizeof(video);
        if (valid) memcpy(video, value, length);
        value = document_attribute(node, "data-yt-action", &length);
        valid = valid && value != NULL && length == 1u && value[0] >= '1' && value[0] <= '4';
        engine->youtube.account_action = valid
            ? (YoutubeLiteAccountAction) (value[0] - '0') : YOUTUBE_LITE_ACTION_NONE;
        engine->youtube.account_generation = engine->navigation.generation;
        if (valid) engine->youtube.account_job = youtube_lite_account_action_begin(
            &engine->budget, &engine->session, engine->youtube.account_action,
            target, video, engine->config.navigation_timeout_ms);
        engine->youtube.account_state = engine->youtube.account_job != NULL
            ? BROWSER_YOUTUBE_FEED_LOADING : youtube_lite_session_signed_in(&engine->session)
            ? BROWSER_YOUTUBE_FEED_FAILED : BROWSER_YOUTUBE_FEED_SIGN_IN_REQUIRED;
    }
    action->type = CONTROLLER_ACTION_CONTROL; action->url[0] = '\0';
    return true;
}

/* Sign-out clears the provider cookies, then navigates home: the action
   stays a navigation, retargeted to the YouTube home page. */
static bool browser_engine_youtube_signout_command(BrowserEngine *engine,
                                                   ControllerAction *action)
{
    if (engine->youtube.signout_node == NULL
        || controller_focused_node(&engine->controller) != engine->youtube.signout_node)
        return false;
    browser_engine_clear_youtube_work(engine);
    (void) browser_session_cookie_clear_domain(&engine->session, "https://youtube.com/");
    engine->youtube.login_verified = false;
    engine->youtube.signin_state = YOUTUBE_SIGNIN_IDLE;
    engine->youtube.login_events = BROWSER_YOUTUBE_LOGIN_SIGNED_OUT;
    engine->youtube.feed_state = BROWSER_YOUTUBE_FEED_IDLE;
    engine->youtube.like_state = BROWSER_YOUTUBE_FEED_IDLE;
    snprintf(action->url, sizeof(action->url), "https://www.youtube.com/");
    return true;
}

static bool browser_engine_youtube_rating_command(BrowserEngine *engine,
                                                  ControllerAction *action,
                                                  lxb_dom_node_t *control)
{
    if (control == NULL
        || controller_focused_node(&engine->controller) != control) return false;
    bool liked = controller_focused_node(&engine->controller) == engine->youtube.like_node;
    if (engine->youtube.like_generation != engine->navigation.generation) {
        engine->youtube.like_generation = engine->navigation.generation;
        engine->youtube.like_state = BROWSER_YOUTUBE_FEED_IDLE;
    }
    if (engine->youtube.like_job == NULL
        && (engine->youtube.like_state != BROWSER_YOUTUBE_FEED_CURRENT
            || engine->youtube.like_selected != liked)) {
#ifndef __PSP__
        memset(&engine->youtube.rating_diagnostics, 0, sizeof(engine->youtube.rating_diagnostics));
#endif
        char id[YOUTUBE_VIDEO_ID_CAPACITY];
        bool signed_in = youtube_lite_session_signed_in(&engine->session);
        if (signed_in && youtube_watch_url_video_id(engine->navigation.page.document_url, id))
            engine->youtube.like_job = youtube_lite_rating_begin(&engine->budget,
                &engine->session, id, liked, engine->config.navigation_timeout_ms);
        engine->youtube.like_selected = liked;
        engine->youtube.like_state = engine->youtube.like_job != NULL
            ? BROWSER_YOUTUBE_FEED_LOADING : signed_in ? BROWSER_YOUTUBE_FEED_FAILED
            : BROWSER_YOUTUBE_FEED_SIGN_IN_REQUIRED;
    }
    action->type = CONTROLLER_ACTION_CONTROL;
    action->url[0] = '\0';
    return true;
}

static bool browser_engine_youtube_like_command(BrowserEngine *engine,
                                                ControllerAction *action)
{
    return browser_engine_youtube_rating_command(
        engine, action, engine->youtube.like_node);
}

static bool browser_engine_youtube_unlike_command(BrowserEngine *engine,
                                                  ControllerAction *action)
{
    return browser_engine_youtube_rating_command(
        engine, action, engine->youtube.unlike_node);
}

static bool browser_engine_youtube_refresh_command(BrowserEngine *engine,
                                                   ControllerAction *action)
{
    engine->youtube.refresh_confirmed = true;
    engine->youtube.header_attempts = 0;
    if (engine->youtube.feed_pending.html == NULL && engine->youtube.feed_job == NULL
        && engine->youtube.topics_job == NULL) engine->youtube.topics_attempted = false;
    action->type = CONTROLLER_ACTION_CONTROL;
    action->url[0] = '\0';
    return true;
}

typedef struct {
    const char *fragment;
    /* The page route the control lives on; YOUTUBE_LITE_ROUTE_NONE: any. */
    YoutubeLiteRoute route;
    /* Only on a script-free native provider page whose controls were bound
       in the current navigation generation. */
    bool native_controls;
    bool (*run)(BrowserEngine *engine, ControllerAction *action);
} BrowserEngineYoutubeCommand;

static const BrowserEngineYoutubeCommand browser_engine_youtube_commands[] = {
    {YOUTUBE_LITE_FRAGMENT_ACCOUNT_ACTION, YOUTUBE_LITE_ROUTE_NONE, true,
     browser_engine_youtube_account_command},
    {YOUTUBE_LITE_FRAGMENT_SIGNOUT, YOUTUBE_LITE_ROUTE_NONE, true,
     browser_engine_youtube_signout_command},
    {YOUTUBE_LITE_FRAGMENT_LIKE, YOUTUBE_LITE_ROUTE_WATCH, true,
     browser_engine_youtube_like_command},
    {YOUTUBE_LITE_FRAGMENT_UNLIKE, YOUTUBE_LITE_ROUTE_WATCH, true,
     browser_engine_youtube_unlike_command},
    {YOUTUBE_LITE_FRAGMENT_REFRESH, YOUTUBE_LITE_ROUTE_HOME, false,
     browser_engine_youtube_refresh_command},
};

/* Runs the provider command an activated same-document link names. Returns
   whether the provider handled the activation; then *finished is the
   facade's input result. */
static bool browser_engine_youtube_activate(BrowserEngine *engine,
                                            ControllerAction *action,
                                            bool activated, bool *finished)
{
    if (!activated || action->type != CONTROLLER_ACTION_NAVIGATE) return false;
    const char *fragment = strchr(action->url, '#');
    if (fragment == NULL) return false;
    const BrowserEngineYoutubeCommand *command = NULL;
    for (size_t i = 0; i < sizeof(browser_engine_youtube_commands)
                               / sizeof(browser_engine_youtube_commands[0]); i++) {
        if (strcmp(fragment, browser_engine_youtube_commands[i].fragment) == 0) {
            command = &browser_engine_youtube_commands[i];
            break;
        }
    }
    if (command == NULL
        || (command->route != YOUTUBE_LITE_ROUTE_NONE
            && youtube_lite_route(engine->navigation.page.document_url) != command->route)
        || (command->native_controls
            && (engine->navigation.page.runtime != NULL
                || engine->youtube.like_node_generation != engine->navigation.generation))
        || !navigation_url_is_same_document(&engine->navigation, action->url)
        || !command->run(engine, action)) return false;
    *finished = browser_engine_finish_input(engine, true);
    return true;
}

/* Header and feed publications share one transactional receiver. A header
   update retains the existing main body, entered search, scroll and focus. */
static bool browser_engine_publish_youtube_home(BrowserEngine *engine,
    YoutubeLiteDocument *document, bool preserve_focus)
{
    size_t search_length = 0;
    lxb_dom_node_t *search_node = NULL;
    for (size_t i = 0; i < engine->navigation.page.layout.control_count; i++) {
        lxb_dom_node_t *node = engine->navigation.page.layout.controls[i].node;
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && length == 9u && memcmp(id, "yt-search", 9u) == 0) {
            search_node = node; break;
        }
    }
    const char *search = search_node == NULL ? NULL : document_control_value(search_node, &search_length);
    if (search == NULL && search_node != NULL) search = document_attribute(search_node, "value", &search_length);
    if (!youtube_lite_home_set_search_value(document, search == NULL ? "" : search, search_length)) return false;
    char focus_id[64] = {0};
    if (preserve_focus) {
        size_t length = 0;
        const char *id = document_attribute(controller_focused_node(&engine->controller), "id", &length);
        if (id != NULL && length < sizeof(focus_id)) memcpy(focus_id, id, length);
    }
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll = preserve_focus && entry != NULL ? entry->scroll_y : 0;
    DocumentBacking previous = engine->backing;
    char url[NAVIGATION_URL_LIMIT];
    snprintf(url, sizeof(url), "%s", engine->navigation.page.document_url);
    engine->youtube.topics_publishing = true;
    bool hooked = navigation_set_candidate_commit_hooks(&engine->navigation,
        browser_engine_prepare_candidate_shell, browser_engine_abort_candidate_shell,
        browser_engine_commit_candidate_shell, engine);
    bool committed = hooked && navigation_commit_static_html(&engine->navigation,
        engine->navigation.generation, url, document->html, document->html_length,
        engine->config.device.navigation_viewport_width, engine->fonts_ready ? &engine->fonts : NULL,
        NULL, false);
    (void) navigation_set_candidate_commit_hooks(&engine->navigation, NULL, NULL, NULL, NULL);
    engine->youtube.topics_publishing = false;
    if (engine->candidate_shell_prepared || engine->candidate_render.budget != NULL)
        browser_engine_abort_candidate_shell(engine);
    if (!committed) {
        engine->backing = previous;
        clear_error(engine);
        return false;
    }
    browser_engine_bind_youtube_controls(engine);
    (void) browser_engine_bind_current_full_document(engine);
    /* This publication is the same feed operation, not a fresh navigation
       that should cancel its in-flight job or restart discovery. */
    engine->youtube.topics_generation = engine->navigation.generation;
    engine->youtube.browsing_generation = engine->navigation.generation;
    navigation_set_scroll(&engine->navigation, scroll);
    lxb_dom_node_t *root = lxb_dom_interface_node(engine->navigation.page.document.html), *node = root;
    for (unsigned visited = 0; focus_id[0] != '\0' && node != NULL && visited < BROWSER_ENGINE_DOM_WALK_LIMIT;
         visited++, node = browser_engine_history_walk_next(root, node)) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && length == strlen(focus_id) && memcmp(id, focus_id, length) == 0) {
            (void) browser_engine_focus_node(engine, node); break;
        }
    }
    return true;
}

static bool browser_engine_refresh_youtube_header(BrowserEngine *engine, bool *visual_changed)
{
    if (!engine->youtube.header_dirty || engine->controller.pointer_down_active
        || engine->youtube.header_attempts >= 2) return false;
    bool signed_in = browser_engine_youtube_login_verified(engine);
    if ((engine->youtube.signout_node != NULL) == signed_in) {
        engine->youtube.header_dirty = false; return false;
    }
    engine->youtube.header_attempts++;
    DocumentBodySnapshot body = {0};
    YoutubeLiteDocument document = {0};
    size_t class_length = 0;
    const char *body_class = document_attribute(document_body_node(&engine->navigation.page.document), "class", &class_length);
    bool compact = body_class != NULL && class_length == 15u && memcmp(body_class, "compact-results", 15u) == 0;
    bool published = document_body_snapshot_capture(&engine->navigation.page.document, &body)
        && youtube_lite_home_rebuild_header(&engine->budget, signed_in, compact, body.markup, body.length, &document)
        && browser_engine_publish_youtube_home(engine, &document, true);
    youtube_lite_document_destroy(&document);
    document_body_snapshot_destroy(&body);
    if (published) {
        engine->youtube.header_dirty = false;
        if (visual_changed != NULL) *visual_changed = true;
    }
    return true;
}

static bool browser_engine_run_youtube_topics(BrowserEngine *engine,
                                             bool *visual_changed)
{
    if (browser_engine_navigation_pending(engine)
        || !engine->navigation.page.loaded
        || youtube_lite_route(engine->navigation.page.document_url)
               != YOUTUBE_LITE_ROUTE_HOME
        || engine->render.frames_rendered == 0) return false;
    uint64_t generation = engine->navigation.generation;
    if (engine->youtube.topics_generation != generation) {
        browser_engine_clear_youtube_work(engine);
        engine->youtube.topics_generation = generation;
        engine->youtube.topics_attempted = false;
        engine->youtube.refresh_confirmed = false;
        engine->youtube.feed_state = BROWSER_YOUTUBE_FEED_IDLE;
        const NavigationEntry *entry = navigation_current(&engine->navigation);
        engine->youtube.browsing = (engine->youtube.browsing_generation == generation
                                    && engine->youtube.browsing)
            || (entry != NULL && entry->scroll_y > 0)
            || browser_engine_youtube_result_focused(engine);
    }
    const char *host = strstr(engine->navigation.page.document_url, "://");
    const char *path = host == NULL ? NULL : strchr(host + 3, '/');
    bool subscriptions = path != NULL && strcspn(path, "?#") == 19u
        && memcmp(path, "/feed/subscriptions", 19u) == 0;
    if (!engine->youtube.topics_attempted) {
        bool authenticated = youtube_lite_session_signed_in(&engine->session);
        engine->youtube.topics_attempted = true;
        engine->youtube.feed_state = BROWSER_YOUTUBE_FEED_LOADING;
        if (authenticated || browser_engine_youtube_signin_pending(engine)) {
            engine->youtube.feed_build_outcome = 1;
            engine->youtube.feed_publication_outcome = 0;
            engine->youtube.signin_feed = (YoutubeLiteFeedDiagnostics) {0};
            if (browser_engine_youtube_signin_pending(engine)) engine->youtube.signin_state = YOUTUBE_SIGNIN_VERIFYING;
#ifndef __PSP__
            engine->youtube.history_metadata = 0u;
#endif
            engine->youtube.feed_job = youtube_lite_feed_begin(&engine->budget,
                &engine->session, subscriptions, engine->youtube.compact_results,
                engine->config.navigation_timeout_ms);
        } else if (!subscriptions && (engine->youtube.topics || engine->youtube.refresh_confirmed)) {
            char error[128] = {0};
            engine->youtube.topics_job = youtube_lite_load_begin_configured(
                &engine->budget, &engine->session,
                "https://www.youtube.com/?tilefinch_topics=1",
                engine->youtube.compact_results, YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES,
                engine->config.navigation_timeout_ms, error, sizeof(error));
        }
        if (engine->youtube.feed_job == NULL && engine->youtube.topics_job == NULL)
            engine->youtube.feed_state = subscriptions && !authenticated
                ? BROWSER_YOUTUBE_FEED_SIGN_IN_REQUIRED : authenticated
                || engine->youtube.topics || engine->youtube.refresh_confirmed
                ? BROWSER_YOUTUBE_FEED_FAILED : BROWSER_YOUTUBE_FEED_IDLE;
        if ((authenticated || browser_engine_youtube_signin_pending(engine)) && engine->youtube.feed_job == NULL) {
            engine->youtube.signin_feed.failure = YOUTUBE_FEED_FAILURE_ADMISSION;
            engine->youtube.feed_build_outcome = 3;
        }
        if (browser_engine_youtube_signin_pending(engine) && engine->youtube.feed_job == NULL) {
            engine->youtube.signin_state = YOUTUBE_SIGNIN_FAILED;
            engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_FAILED;
        }
    }
    FetchPumpQuota quota = {
        .maximum_body_callbacks = 1, .maximum_body_bytes = 16u * KIB,
        .maximum_time_us = engine->config.idle_work_budget_us
    };
    bool working = engine->youtube.feed_job != NULL || engine->youtube.topics_job != NULL;
    if (working) {
        YoutubeLiteLoadStatus status = engine->youtube.feed_job != NULL
            ? youtube_lite_feed_pump(engine->youtube.feed_job, &quota)
            : youtube_lite_load_pump(engine->youtube.topics_job, &quota);
        uint64_t authenticated_identity = youtube_lite_feed_authenticated_identity(engine->youtube.feed_job);
        uint64_t rejected_identity = youtube_lite_feed_rejected_identity(engine->youtube.feed_job);
        if (rejected_identity != 0 && engine->youtube.login_verified == rejected_identity) {
            engine->youtube.login_verified = 0;
            engine->youtube.signin_state = YOUTUBE_SIGNIN_FAILED;
            engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_FAILED;
            engine->youtube.header_dirty = true;
            engine->youtube.header_attempts = 0;
        }
        if (authenticated_identity != 0 && authenticated_identity
                == youtube_lite_session_login_identity(&engine->session)) {
            bool completing_attempt = browser_engine_youtube_signin_pending(engine);
            engine->youtube.signin_state = YOUTUBE_SIGNIN_VERIFIED;
            if (completing_attempt || engine->youtube.login_verified != authenticated_identity) {
                engine->youtube.login_verified = authenticated_identity;
                engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_VERIFIED;
                engine->youtube.header_dirty = true;
                engine->youtube.header_attempts = 0;
            }
        }
        if (engine->youtube.signin_diagnostics && engine->youtube.feed_job != NULL)
            (void) youtube_lite_feed_diagnostics(engine->youtube.feed_job, &engine->youtube.signin_feed);
        if (status == YOUTUBE_LITE_LOAD_PENDING) {
            (void) browser_engine_refresh_youtube_header(engine, visual_changed);
            if (browser_engine_youtube_status_paint(engine) && visual_changed != NULL)
                *visual_changed = true;
            return true;
        }
        bool received = status == YOUTUBE_LITE_LOAD_SUCCEEDED
            && (engine->youtube.feed_job != NULL
                ? youtube_lite_feed_take_document(engine->youtube.feed_job,
                                                   &engine->youtube.feed_pending)
                : youtube_lite_load_take_document(engine->youtube.topics_job,
                                                   &engine->youtube.feed_pending));
        if (engine->youtube.feed_job != NULL)
            engine->youtube.feed_build_outcome = received ? 2u : 3u;
        if (received) engine->youtube.feed_pending_identity = authenticated_identity;
#ifndef __PSP__
        engine->youtube.history_metadata = youtube_lite_feed_history_metadata(engine->youtube.feed_job);
#endif
        youtube_lite_feed_destroy(engine->youtube.feed_job);
        engine->youtube.feed_job = NULL;
        youtube_lite_load_destroy(engine->youtube.topics_job);
        engine->youtube.topics_job = NULL;
        engine->youtube.feed_state = received ? BROWSER_YOUTUBE_FEED_READY
                                              : BROWSER_YOUTUBE_FEED_FAILED;
        if (!received && browser_engine_youtube_signin_pending(engine)) {
            engine->youtube.signin_state = YOUTUBE_SIGNIN_FAILED;
            engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_FAILED;
        }
    }
    const NavigationEntry *current = navigation_current(&engine->navigation);
    if (engine->youtube.feed_pending_identity != 0 && engine->youtube.feed_pending_identity
        != youtube_lite_session_login_identity(&engine->session)) {
        youtube_lite_document_destroy(&engine->youtube.feed_pending);
        engine->youtube.feed_pending_identity = 0;
        engine->youtube.feed_state = BROWSER_YOUTUBE_FEED_FAILED;
        engine->youtube.feed_publication_outcome = 3;
        engine->youtube.signin_feed.failure = YOUTUBE_FEED_FAILURE_SESSION_CHANGED;
    }
    if (current != NULL && current->scroll_y > 0)
        browser_engine_youtube_browsing(engine);
    if (engine->youtube.feed_pending.html != NULL)
        engine->youtube.feed_publication_outcome = engine->youtube.browsing ? 4u : 1u;
    if (engine->youtube.feed_pending.html != NULL
        && !engine->controller.pointer_down_active
        && (!engine->youtube.browsing || engine->youtube.refresh_confirmed)) {
        YoutubeLiteDocument *document = &engine->youtube.feed_pending;
        bool committed = browser_engine_publish_youtube_home(engine, document, false);
        engine->youtube.feed_publication_outcome = committed ? 2u : 3u;
        engine->youtube.feed_state = committed ? BROWSER_YOUTUBE_FEED_CURRENT : BROWSER_YOUTUBE_FEED_FAILED;
        if (committed && visual_changed != NULL) *visual_changed = true;
        youtube_lite_document_destroy(document);
        engine->youtube.feed_pending_identity = 0;
        engine->youtube.refresh_confirmed = false;
    }
    if (browser_engine_youtube_status_paint(engine) && visual_changed != NULL)
        *visual_changed = true;
    return browser_engine_refresh_youtube_header(engine, visual_changed) || working;
}

static bool browser_engine_run_youtube_account(BrowserEngine *engine, bool *visual_changed)
{
    if (browser_engine_navigation_pending(engine) || !engine->navigation.page.loaded
        || youtube_lite_route(engine->navigation.page.document_url) == YOUTUBE_LITE_ROUTE_NONE)
        return false;
    if (engine->youtube.account_generation != engine->navigation.generation) {
        engine->youtube.account_generation = engine->navigation.generation;
        engine->youtube.account_state = BROWSER_YOUTUBE_FEED_IDLE;
    }
    bool working = engine->youtube.account_job != NULL;
    if (working) {
        FetchPumpQuota quota = {.maximum_body_callbacks = 1, .maximum_body_bytes = 16u * KIB,
            .maximum_time_us = engine->config.idle_work_budget_us};
        YoutubeLiteLoadStatus status = youtube_lite_feed_pump(engine->youtube.account_job, &quota);
#ifndef __PSP__
        (void) youtube_lite_account_diagnostics(engine->youtube.account_job,
            &engine->youtube.account_diagnostics);
#endif
        if (status != YOUTUBE_LITE_LOAD_PENDING) {
            engine->youtube.account_state = status == YOUTUBE_LITE_LOAD_SUCCEEDED
                ? BROWSER_YOUTUBE_FEED_CURRENT : BROWSER_YOUTUBE_FEED_FAILED;
            youtube_lite_feed_destroy(engine->youtube.account_job);
            engine->youtube.account_job = NULL;
        }
    }
    if (engine->youtube.account_painted_state != engine->youtube.account_state
        || engine->youtube.account_layout_generation != engine->navigation.incremental_relayouts
        || engine->youtube.account_shell != engine->render_shell_serial) {
        static const char *const done[] = {"", "Added to playlist.", "Removed from playlist.", "Subscribed.", "Unsubscribed."};
        const char *label = engine->youtube.account_state == BROWSER_YOUTUBE_FEED_LOADING ? "Updating..."
            : engine->youtube.account_state == BROWSER_YOUTUBE_FEED_CURRENT ? done[engine->youtube.account_action]
            : engine->youtube.account_state == BROWSER_YOUTUBE_FEED_FAILED ? "Not confirmed. Select again to retry."
            : engine->youtube.account_state == BROWSER_YOUTUBE_FEED_SIGN_IN_REQUIRED ? "Sign in to use this action."
            : "";
        if (browser_engine_youtube_notice_paint(engine, "yt-library-status", tilefinch_ui_text(label))
            && visual_changed != NULL) *visual_changed = true;
        engine->youtube.account_painted_state = engine->youtube.account_state;
        engine->youtube.account_layout_generation = engine->navigation.incremental_relayouts;
        engine->youtube.account_shell = engine->render_shell_serial;
    }
    return working;
}

static bool browser_engine_run_youtube_like(BrowserEngine *engine, bool *visual_changed)
{
    if (browser_engine_navigation_pending(engine) || !engine->navigation.page.loaded
        || youtube_lite_route(engine->navigation.page.document_url) != YOUTUBE_LITE_ROUTE_WATCH)
        return false;
    if (engine->youtube.like_generation != engine->navigation.generation) {
        engine->youtube.like_generation = engine->navigation.generation;
        engine->youtube.like_state = BROWSER_YOUTUBE_FEED_IDLE;
    }
    bool working = engine->youtube.like_job != NULL;
    if (working) {
        FetchPumpQuota quota = {.maximum_body_callbacks = 1, .maximum_body_bytes = 16u * KIB,
            .maximum_time_us = engine->config.idle_work_budget_us};
        YoutubeLiteLoadStatus status = youtube_lite_feed_pump(engine->youtube.like_job, &quota);
        if (status != YOUTUBE_LITE_LOAD_PENDING) {
#ifndef __PSP__
            (void) youtube_lite_rating_diagnostics(engine->youtube.like_job,
                &engine->youtube.rating_diagnostics);
#endif
            engine->youtube.like_state = status == YOUTUBE_LITE_LOAD_SUCCEEDED
                ? BROWSER_YOUTUBE_FEED_CURRENT : BROWSER_YOUTUBE_FEED_FAILED;
            youtube_lite_feed_destroy(engine->youtube.like_job);
            engine->youtube.like_job = NULL;
        }
    }
    if (engine->youtube.like_painted_state != engine->youtube.like_state
        || engine->youtube.like_layout_generation != engine->navigation.incremental_relayouts
        || engine->youtube.like_shell != engine->render_shell_serial) {
        static const char *const labels[] = {"", "Saving like...",
            "", "Video liked.", "Like failed - select Like to retry.", "Sign in to like this video."};
        static const char *const unlike_labels[] = {"", "Removing like...",
            "", "Like removed.", "Unlike failed - select Unlike to retry.", "Sign in to unlike this video."};
        if (browser_engine_youtube_notice_paint(engine, "yt-like-status",
                tilefinch_ui_text((engine->youtube.like_selected ? labels : unlike_labels)[engine->youtube.like_state]))
            && visual_changed != NULL) *visual_changed = true;
        engine->youtube.like_painted_state = engine->youtube.like_state;
        engine->youtube.like_layout_generation = engine->navigation.incremental_relayouts;
        engine->youtube.like_shell = engine->render_shell_serial;
    }
    return working;
}

/* Idle work the facade's step table runs first (browser_engine_idle_steps).
   Sign-in recovery never starts a navigation itself: it hands the facade a
   navigation intent and is told whether the facade started it. */
typedef struct {
    const char *url;
    bool record_history;
    bool signin_recovery;
} BrowserEngineNavigationIntent;

typedef enum {
    /* No recovery is due; the idle turn continues. */
    BROWSER_YOUTUBE_RECOVERY_NONE,
    /* Recovery was due but declined; the idle turn ends without work. */
    BROWSER_YOUTUBE_RECOVERY_DECLINED,
    /* Start the intent, then report with ..._recovery_started. */
    BROWSER_YOUTUBE_RECOVERY_NAVIGATE
} BrowserYoutubeRecovery;

static BrowserYoutubeRecovery browser_engine_youtube_idle_recovery(
    BrowserEngine *engine, BrowserEngineNavigationIntent *intent)
{
    if (!(browser_engine_youtube_signin_pending(engine)
        && !browser_engine_navigation_pending(engine) && engine->navigation.page.loaded
        && browser_engine_youtube_auth_origin(engine->navigation.page.document_url)
        && engine->navigation.scripts_enabled && engine->navigation.document_scripts_enabled
        && engine->navigation.page.runtime == NULL
        && engine->navigation.page.script_degradation_observed))
        return BROWSER_YOUTUBE_RECOVERY_NONE;
    if (engine->youtube.signin_retries != 0) {
        engine->youtube.signin_state = YOUTUBE_SIGNIN_FAILED;
        engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_FAILED;
        return BROWSER_YOUTUBE_RECOVERY_DECLINED;
    }
    /* Never discard an identifier/password the user has already entered.
       Inspect lengths only; no field contents enter diagnostics. */
    for (size_t i = 0; i < engine->navigation.page.layout.control_count; i++) {
        const ControlRegion *control = &engine->navigation.page.layout.controls[i];
        if (control->type != CONTROL_INPUT && control->type != CONTROL_TEXTAREA
            && control->type != CONTROL_EDITABLE) continue;
        size_t length = 0;
        if (document_control_value(control->node, &length) == NULL)
            (void) document_attribute(control->node, "value", &length);
        if (length != 0) {
            engine->youtube.signin_state = YOUTUBE_SIGNIN_MANUAL;
            engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_FAILED;
            return BROWSER_YOUTUBE_RECOVERY_DECLINED;
        }
    }
    /* Retry the stable GET entry once after the predecessor is gone.
       Never replay a password POST, single-use URL, or saved input. */
    engine->youtube.signin_retries++;
    engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_RECOVERING;
    engine->youtube.signin_state = YOUTUBE_SIGNIN_RECOVERING;
#ifndef __PSP__
    bool refused = tilefinch_test_faults()->refuse_youtube_signin_recovery;
    tilefinch_test_faults()->refuse_youtube_signin_recovery = false;
    if (refused) {
        engine->youtube.signin_state = YOUTUBE_SIGNIN_FAILED;
        engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_FAILED;
        return BROWSER_YOUTUBE_RECOVERY_DECLINED;
    }
#endif
    *intent = (BrowserEngineNavigationIntent) {
        .url = youtube_signin_entry,
        .record_history = false,
        .signin_recovery = true
    };
    return BROWSER_YOUTUBE_RECOVERY_NAVIGATE;
}

static void browser_engine_youtube_recovery_started(BrowserEngine *engine,
                                                    bool started)
{
    if (!started) {
        engine->youtube.signin_state = YOUTUBE_SIGNIN_FAILED;
        engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_FAILED;
    }
}

/* Discovery is optional: a slow response must not monopolize idle turns
   ahead of fonts, images or tile preparation. Keep pumping it once per
   turn, while still allowing the other bounded continuations to run. */
static bool browser_engine_youtube_idle_pumps(BrowserEngine *engine,
                                              bool *visual_changed)
{
    bool topics_work = browser_engine_run_youtube_topics(engine, visual_changed);
    topics_work |= browser_engine_run_youtube_like(engine, visual_changed);
    topics_work |= browser_engine_run_youtube_account(engine, visual_changed);
    return topics_work;
}

/* Facade hooks: the only provider touches in the generic navigation, commit,
   finish and cancel paths of browser_engine.c. */
static void browser_engine_youtube_on_navigation_begin(
    BrowserEngine *engine, const char *url, bool signin_recovery)
{
    /* Internal recovery preserves its one-retry budget. Explicit Continue or
       Sign in always starts a fresh attempt, even after a recovery page. */
    if (!signin_recovery) browser_engine_note_youtube_signin_navigation(engine, url);
}

/* The sign-in handoff commits as the YouTube home page it lands on. */
static const char *browser_engine_youtube_commit_url(const char *url)
{
    static const char youtube_home[] = "https://www.youtube.com/";
    return youtube_lite_signin_handoff_url(url) ? youtube_home : url;
}

static void browser_engine_youtube_on_commit(BrowserEngine *engine,
                                             bool committed)
{
    if (committed) browser_engine_bind_youtube_controls(engine);
}

static void browser_engine_youtube_on_finish(
    BrowserEngine *engine, BrowserNavigationJobStatus outcome,
    const SiteAdapterLoadMetrics *adapter)
{
    if (adapter != NULL && engine->youtube.signin_diagnostics
        && adapter->handoff_http_status != 0) {
        engine->youtube.handoff_http_status = adapter->handoff_http_status;
        engine->youtube.handoff_service_origin = adapter->handoff_service_origin;
    }
    if (!browser_engine_youtube_signin_pending(engine)) return;
    if (outcome == BROWSER_NAVIGATION_JOB_CANCELLED) {
        engine->youtube.signin_state = YOUTUBE_SIGNIN_CANCELLED;
    } else if (outcome == BROWSER_NAVIGATION_JOB_FAILED) {
        engine->youtube.signin_state = YOUTUBE_SIGNIN_FAILED;
        engine->youtube.login_events |= BROWSER_YOUTUBE_LOGIN_FAILED;
    }
}

/* Stops the provider's network jobs and returns how many were running. */
static size_t browser_engine_youtube_cancel(BrowserEngine *engine)
{
    size_t cancelled = (engine->youtube.topics_job != NULL ? 1u : 0u)
        + (engine->youtube.feed_job != NULL ? 1u : 0u)
        + (engine->youtube.like_job != NULL ? 1u : 0u)
        + (engine->youtube.account_job != NULL ? 1u : 0u);
    browser_engine_clear_youtube_work(engine);
    return cancelled;
}
