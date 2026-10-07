/* Declarative refresh: <meta http-equiv="refresh"> and the `Refresh`
   response header (HTML Standard, "shared declarative refresh steps").

   The committed document owns one NavigationRefresh (page state). The
   response header is adopted when the document is created; the DOM is
   examined on the first runtime advance after commit, which is after the
   load event, and the timer starts there. Author script inserting a
   refresh meta later makes the runtime ask for another look. The timer
   runs on the page's runtime clock (navigation_advance_runtime's elapsed
   time), so it pauses whenever page work does. A refresh that comes due
   is followed like a page-script navigation: synchronously when the owner
   lets navigation load in place, otherwise handed to the frontend through
   navigation_take_refresh_navigation. It replaces the current history
   entry. See docs/ARCHITECTURE.md, "Declarative refresh". */

static bool navigation_urls_share_document(const char *left,
                                            const char *right);

static uint64_t navigation_refresh_url_hash(const char *url)
{
    return fingerprint_bytes(UINT64_C(1469598103934665603), url,
                             url == NULL ? 0 : strlen(url));
}

/* A URL with a scheme (an absolute URL in the URL Standard's sense):
   "javascript:x", "data:,x", "file:///x". The resolver understands only
   HTTP(S), so this tells a valid but non-navigable target (refused) from a
   relative reference that failed to parse (no refresh at all). */
static bool navigation_refresh_reference_has_scheme(const char *reference)
{
    if (reference == NULL || !isalpha((unsigned char) reference[0]))
        return false;
    for (size_t at = 1; reference[at] != '\0'; at++) {
        unsigned char byte = (unsigned char) reference[at];
        if (byte == ':') return true;
        if (!isalnum(byte) && byte != '+' && byte != '-' && byte != '.')
            return false;
    }
    return false;
}

/* The URL parser's input clean-up: strip leading and trailing C0 controls
   and spaces, remove every ASCII tab and newline. */
static bool navigation_refresh_clean_url(const char *input, size_t length,
                                         char *output, size_t capacity)
{
    while (length != 0 && (unsigned char) input[0] <= 0x20) {
        input++;
        length--;
    }
    while (length != 0 && (unsigned char) input[length - 1] <= 0x20)
        length--;
    size_t used = 0;
    for (size_t at = 0; at < length; at++) {
        char character = input[at];
        if (character == '\t' || character == '\n' || character == '\r')
            continue;
        if (used + 1 >= capacity) return false;
        output[used++] = character;
    }
    output[used] = '\0';
    return true;
}

/* Runs the shared declarative refresh steps' parse and URL steps for the
   committed document. True when the document now "will declaratively
   refresh" (including a refused target); false when the steps returned
   early and a later declaration may still count. */
static bool navigation_refresh_declare(NavigationSession *session,
                                       const char *input, size_t length,
                                       const char *document_url,
                                       const char *base_url,
                                       bool from_header)
{
    NavigationRefresh *refresh = &session->page.refresh;
    if (refresh->state != NAVIGATION_REFRESH_NONE || document_url == NULL
        || document_url[0] == '\0') return false;
    DeclarativeRefresh parsed;
    if (!declarative_refresh_parse(input, length, &parsed)) return false;
    if (base_url == NULL || base_url[0] == '\0') base_url = document_url;
    NavigationRefreshState state = NAVIGATION_REFRESH_PENDING;
    char reference[NAVIGATION_URL_LIMIT];
    if (!parsed.has_url) {
        /* "Let urlRecord be document's URL." */
        if (strlen(document_url) >= sizeof(refresh->url)) return false;
        snprintf(refresh->url, sizeof(refresh->url), "%s", document_url);
    } else if (!navigation_refresh_clean_url(
                   input + parsed.url_offset, parsed.url_length,
                   reference, sizeof(reference))) {
        return false;
    } else if (!tilefinch_url_resolve(base_url, reference, refresh->url,
                                      sizeof(refresh->url))) {
        refresh->url[0] = '\0';
        /* A parsable URL that is not HTTP(S) still makes this the
           document's refresh; it is just never followed. */
        if (!navigation_refresh_reference_has_scheme(reference)) return false;
        state = NAVIGATION_REFRESH_REFUSED;
    }
    refresh->state = state;
    refresh->from_header = from_header;
    refresh->delay_seconds = parsed.seconds;
    refresh->elapsed_ms = 0;
    refresh->reload = state == NAVIGATION_REFRESH_PENDING
        && navigation_urls_share_document(refresh->url, document_url);
    return true;
}

/* The Refresh header is processed when the document is created, before
   any meta, against the document's URL (there is no <base> yet). */
static void navigation_refresh_adopt_header(NavigationSession *session,
                                            const char *document_url)
{
    if (session->pending_refresh_header[0] != '\0') {
        (void) navigation_refresh_declare(
            session, session->pending_refresh_header,
            strlen(session->pending_refresh_header), document_url,
            document_url, true);
    }
    session->pending_refresh_header[0] = '\0';
}

/* Every connected <meta http-equiv=refresh>, in tree order, until one
   declares the refresh: the order the parser inserted them in. A <meta>
   inside <noscript> counts only when the document was parsed with
   scripting disabled (it is an element then, as in a no-script browser);
   template contents are never in the tree. */
static void navigation_refresh_examine_document(NavigationSession *session)
{
    PocDocument *document = &session->page.document;
    if (session->page.refresh.state != NAVIGATION_REFRESH_NONE
        || document->html == NULL) return;
    const char *document_url = navigation_active_document_url(session);
    if (document_url == NULL) return;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    char base[NAVIGATION_URL_LIMIT];
    bool base_ready = false;
    lxb_dom_node_t *node = root;
    for (size_t visited = 0;
         node != NULL && visited < NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT;
         visited++) {
        bool descend = node->first_child != NULL;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && node->local_name == LXB_TAG_NOSCRIPT
            && !document->noscript_rendered) descend = false;
        if (document_is_refresh_meta(node)) {
            size_t length = 0;
            const char *content = document_attribute(
                node, "content", &length);
            if (content != NULL && length != 0) {
                if (!base_ready) {
                    /* The document base when the meta is processed; an
                       empty base falls back to the document's URL. */
                    if (!document_base_url(document, document_url, base,
                                           sizeof(base)))
                        base[0] = '\0';
                    base_ready = true;
                }
                if (navigation_refresh_declare(session, content, length,
                                               document_url, base, false))
                    return;
            }
        }
        if (descend) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        if (node == NULL || node == root) return;
        node = node->next;
    }
}

/* The load-complete point (the header was adopted at commit), or a meta
   inserted after load: look for the document's refresh; its time counts
   from now. Counted here, on the live session, rather than on a
   candidate's copy of it. */
static void navigation_refresh_examine(NavigationSession *session)
{
    NavigationRefresh *refresh = &session->page.refresh;
    refresh->examined = true;
    if (!session->page.loaded) return;
    navigation_refresh_examine_document(session);
    refresh->elapsed_ms = 0;
    if (NAVIGATION_TEST_COUNTERS && refresh->state != NAVIGATION_REFRESH_NONE
        && !refresh->counted) {
        refresh->counted = true;
        session->refresh.declared++;
        if (refresh->state == NAVIGATION_REFRESH_REFUSED)
            session->refresh.refused++;
    }
}

/* One runtime tick of the refresh timer. True when the refresh is due and
   may be followed now. */
static bool navigation_refresh_advance(NavigationSession *session,
                                       unsigned elapsed_ms)
{
    NavigationRefresh *refresh = &session->page.refresh;
    bool mutated = session->page.runtime != NULL
        && script_runtime_take_refresh_meta_mutation(session->page.runtime);
    if (!refresh->examined
        || (mutated && refresh->state == NAVIGATION_REFRESH_NONE)) {
        navigation_refresh_examine(session);
        elapsed_ms = 0;
    }
    if (refresh->state == NAVIGATION_REFRESH_DUE) return true;
    if (refresh->state != NAVIGATION_REFRESH_PENDING) return false;
    refresh->elapsed_ms = refresh->elapsed_ms > UINT64_MAX - elapsed_ms
        ? UINT64_MAX : refresh->elapsed_ms + elapsed_ms;
    if (refresh->elapsed_ms < (uint64_t) refresh->delay_seconds * 1000u)
        return false;
    /* An auto-reload of a page the reader has started using would throw
       the page (and a full PSP load) away under them: offer it instead.
       A refresh to another URL is a redirect and is followed. */
    if (refresh->reload && refresh->user_touched) {
        refresh->state = NAVIGATION_REFRESH_OFFERED;
        refresh->offer_notice = true;
        if (NAVIGATION_TEST_COUNTERS) session->refresh.offered++;
        return false;
    }
    if (session->refresh.chain >= NAVIGATION_REFRESH_CHAIN_LIMIT) {
        refresh->state = NAVIGATION_REFRESH_STOPPED;
        refresh->stop_notice = true;
        if (NAVIGATION_TEST_COUNTERS) session->refresh.stopped++;
        return false;
    }
    refresh->state = NAVIGATION_REFRESH_DUE;
    return true;
}

/* The next load of url replaces the current history entry (historyHandling
   "replace"): a followed refresh, or location.replace() to another
   document. A reload or fragment navigation keeps its entry, so callers
   arm only cross-document targets. */
static void navigation_arm_history_replace(NavigationSession *session,
                                           const char *url)
{
    session->history_replace_url_hash = navigation_refresh_url_hash(url);
    session->history_replace_armed = true;
}

/* A navigation this document starts (a page script, or its refresh): the
   document is the referer and the activation is the script's; with
   `replace` the navigation replaces the current history entry. */
static void navigation_note_document_initiator(NavigationSession *session,
                                               const char *url,
                                               bool user_activated,
                                               bool replace)
{
    const char *current_url = navigation_active_document_url(session);
    if (current_url != NULL) {
        snprintf(session->pending_navigation_referer,
                 sizeof(session->pending_navigation_referer), "%s",
                 current_url);
        session->pending_navigation_same_origin =
            tilefinch_url_same_origin(current_url, url);
    }
    session->pending_navigation_user_activated = user_activated;
    if (replace) navigation_arm_history_replace(session, url);
}

/* A page-script navigation the runtime marked "replace" (location.replace,
   location.reload) replaces the entry when it leaves the document. */
static void navigation_note_script_initiator(NavigationSession *session,
                                             const char *url,
                                             bool user_activated,
                                             bool replace)
{
    navigation_note_document_initiator(
        session, url, user_activated,
        replace && !navigation_urls_share_document(
                       navigation_active_document_url(session), url));
}

/* Marks the due refresh followed and records it like a page-script
   navigation from this document: the document is the referer, there is no
   user activation, and the next load of this URL replaces the current
   history entry. */
static void navigation_refresh_follow(NavigationSession *session,
                                      char *url, size_t capacity)
{
    NavigationRefresh *refresh = &session->page.refresh;
    refresh->state = NAVIGATION_REFRESH_FOLLOWED;
    snprintf(url, capacity, "%s", refresh->url);
    if (NAVIGATION_TEST_COUNTERS) session->refresh.followed++;
    session->refresh.chain++;
    navigation_note_document_initiator(session, url, false, true);
}

/* navigation_begin: any navigation of this browsing context ends a refresh
   that has not been followed, including one the document declares but no
   runtime turn has examined yet. */
static void navigation_refresh_supersede(NavigationSession *session)
{
    NavigationRefresh *refresh = &session->page.refresh;
    if (!refresh->examined) navigation_refresh_examine(session);
    if (refresh->state == NAVIGATION_REFRESH_PENDING
        || refresh->state == NAVIGATION_REFRESH_DUE) {
        refresh->state = NAVIGATION_REFRESH_CANCELLED;
        if (NAVIGATION_TEST_COUNTERS) session->refresh.cancelled++;
    }
}

/* navigation_begin: the arming belongs to the navigation starting now. */
static void navigation_hand_history_replace(NavigationSession *session)
{
    session->history_replace_generation = session->history_replace_armed
        ? session->generation : 0;
    session->history_replace_armed = false;
}

/* Load start or direct commit of `generation`: whether it is the armed
   navigation, which replaces the current history entry. A navigation that
   records its own entry never replaces. Consumes the arming either way. */
static bool navigation_take_history_replace(NavigationSession *session,
                                            uint64_t generation,
                                            const char *url,
                                            bool record_history)
{
    bool replace = !record_history
        && session->history_replace_generation != 0
        && session->history_replace_generation == generation
        && session->history_replace_url_hash
               == navigation_refresh_url_hash(url);
    session->history_replace_generation = 0;
    return replace;
}

NavigationRefreshTake navigation_take_refresh_navigation(
    NavigationSession *session, char *url, size_t capacity)
{
    if (session == NULL || url == NULL || capacity == 0
        || !session->defer_script_navigation || !session->page.loaded)
        return NAVIGATION_REFRESH_TAKE_NONE;
    NavigationRefresh *refresh = &session->page.refresh;
    if (refresh->stop_notice) {
        refresh->stop_notice = false;
        return NAVIGATION_REFRESH_TAKE_STOPPED;
    }
    if (refresh->offer_notice) {
        refresh->offer_notice = false;
        return NAVIGATION_REFRESH_TAKE_OFFERED;
    }
    if (refresh->state != NAVIGATION_REFRESH_DUE
        || strlen(refresh->url) >= capacity
        || (session->page.runtime != NULL
            && script_runtime_has_pending_navigation(session->page.runtime)))
        return NAVIGATION_REFRESH_TAKE_NONE;
    navigation_refresh_follow(session, url, capacity);
    return NAVIGATION_REFRESH_TAKE_NAVIGATE;
}

void navigation_note_user_input(NavigationSession *session)
{
    if (session == NULL) return;
    session->refresh.chain = 0;
    if (session->page.loaded) session->page.refresh.user_touched = true;
}

bool navigation_cancel_refresh(NavigationSession *session)
{
    if (session == NULL) return false;
    NavigationRefreshState before = session->page.refresh.state;
    navigation_refresh_supersede(session);
    return before != session->page.refresh.state;
}
