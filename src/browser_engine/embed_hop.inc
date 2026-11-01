/* The embed hop: Play on a page that has nothing playable itself but
   declares a player page (MediaDeclaredVideo.embed_url) navigates there
   (ControllerAction.embed_hop). The player page loads as an ordinary,
   fresh top-level page, after the original page is released (its realm at
   once, its document as the new one starts: low-memory replacement), with
   the original page as the referrer; only the content
   blocker's generic player-page list is added for requests that page
   makes. While the page runs, its own JSON/playlist responses are
   remembered as usual (media_json_response_remember, with the player
   page's address words as identity); the first identity-matched stream is
   handed to the frontend at once, since signed stream addresses expire.
   The frontend opens it with the ordinary page-video route (decoder and
   size checks, CSP media-src, content blocker, per-hop redirects) and
   goes Back when the player closes, or when nothing playable appears in
   BROWSER_EMBED_HOP_TIMEOUT_US after the player page commits. Any other
   navigation ends the hop. Polling costs a few loads and compares, and
   only while a hop is in progress. */

static void browser_engine_embed_hop_end(BrowserEngine *engine)
{
    if (engine->embed_hop.phase == BROWSER_EMBED_HOP_PHASE_IDLE) return;
    engine->embed_hop = (BrowserEngineEmbedHop) {0};
    engine->navigation.replacement_low_memory_next = false;
    engine->navigation.parser_script_stage_extended_next = false;
    content_blocker_set_player_page(engine->content_blocker, NULL);
}

/* Every navigation entry point passes through here: the hop's own
   navigation arms it, any other ends it, and each releases a held page
   network. */
static void browser_engine_embed_hop_navigation(
    BrowserEngine *engine, const ControllerAction *action)
{
    if (engine == NULL) return;
    /* A held page network (browser_engine_hold_page_network) belongs to
       the page being left; the next page may share its fetch domain. */
    browser_engine_release_page_network(engine);
    if (action == NULL || !action->embed_hop
        || action->type != CONTROLLER_ACTION_NAVIGATE) {
        browser_engine_embed_hop_end(engine);
        return;
    }
    engine->embed_hop = (BrowserEngineEmbedHop) {
        .phase = BROWSER_EMBED_HOP_PHASE_WAITING,
        .committed_before_us = engine->navigation.page.committed_us
    };
    content_blocker_set_player_page(engine->content_blocker, action->url);
    /* The player page needs the room the page being left holds: its
       script realm and the optional caches go now, and its document goes
       as the player page starts loading (its last frame stays on screen).
       Back reloads the page as any history move does. */
    if (engine->navigation_ready) {
        navigation_retire_current_page_scripts(&engine->navigation);
        engine->navigation.replacement_low_memory_next = true;
    }
    engine->navigation.parser_script_stage_extended_next = true;
    BrowserOptionalMemoryReclaim reclaim;
    (void) browser_engine_reclaim_optional_memory(engine, &reclaim);
}

bool browser_engine_embed_hop_active(const BrowserEngine *engine)
{
    return engine != NULL
        && engine->embed_hop.phase != BROWSER_EMBED_HOP_PHASE_IDLE;
}

BrowserEmbedHopStatus browser_engine_embed_hop_poll(
    BrowserEngine *engine, ControllerAction *action)
{
    if (!browser_engine_embed_hop_active(engine) || action == NULL)
        return BROWSER_EMBED_HOP_NONE;
    BrowserEngineEmbedHop *hop = &engine->embed_hop;
    const NavigationPage *page = &engine->navigation.page;
    if (hop->phase == BROWSER_EMBED_HOP_PHASE_PLAYING)
        return BROWSER_EMBED_HOP_PLAYING;
    if (!hop->committed) {
        if (browser_engine_navigation_pending(engine))
            return BROWSER_EMBED_HOP_WAITING;
        /* Refused, failed or cancelled: the page the user was on stays. */
        if (!page->loaded || page->committed_us == hop->committed_before_us
            || !engine->controller_ready) {
            browser_engine_embed_hop_end(engine);
            return BROWSER_EMBED_HOP_NONE;
        }
        hop->committed = true;
        hop->committed_us = page->committed_us;
        hop->started_us = browser_engine_recovery_now_us();
        hop->seen_generation = media_json_response_generation(
            &page->document) - 1u;
    }
    if (!page->loaded || page->committed_us != hop->committed_us) {
        browser_engine_embed_hop_end(engine);
        return BROWSER_EMBED_HOP_NONE;
    }
    uint32_t generation = media_json_response_generation(&page->document);
    if (generation != hop->seen_generation) {
        hop->seen_generation = generation;
        if (controller_build_embed_media_action(&engine->controller, action)) {
            hop->phase = BROWSER_EMBED_HOP_PHASE_PLAYING;
            return BROWSER_EMBED_HOP_READY;
        }
    }
    uint64_t now_us = browser_engine_recovery_now_us();
    if (now_us >= hop->started_us
        && now_us - hop->started_us >= BROWSER_EMBED_HOP_TIMEOUT_US) {
        browser_engine_embed_hop_end(engine);
        return BROWSER_EMBED_HOP_TIMED_OUT;
    }
    return BROWSER_EMBED_HOP_WAITING;
}

bool browser_engine_embed_hop_take_return(BrowserEngine *engine)
{
    if (!browser_engine_embed_hop_active(engine)
        || engine->embed_hop.phase != BROWSER_EMBED_HOP_PHASE_PLAYING)
        return false;
    bool on_player_page = engine->navigation.page.loaded
        && engine->navigation.page.committed_us
               == engine->embed_hop.committed_us;
    browser_engine_embed_hop_end(engine);
    return on_player_page;
}

bool browser_engine_embed_hop_take_unavailable(BrowserEngine *engine,
                                               const char *failure)
{
    if (failure == NULL || !browser_engine_embed_hop_active(engine)
        || engine->embed_hop.phase != BROWSER_EMBED_HOP_PHASE_PLAYING)
        return false;
    const char *headline =
        media_failure_headline(MEDIA_FAILURE_STREAM_UNAVAILABLE);
    MediaFailureMessage message;
    bool unavailable = strncmp(failure, headline, strlen(headline)) == 0
        || (media_failure_message(failure, &message)
            && message.kind == MEDIA_FAILURE_STREAM_UNAVAILABLE);
    return unavailable && browser_engine_embed_hop_take_return(engine);
}
