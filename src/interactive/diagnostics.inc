static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--fixture FILE | --url URL] [--fetch-scripts] "
            "[--ticks N] [--tick-ms N] [--focus-next N] [--type TEXT] "
            "[--media-file MP4 | --media-url URL] "
            "[--pace-real-time] "
            "[--focus-id ID] [--click-selector CSS] [--interaction-ticks N] "
            "[--post-click-selector CSS] [--post-click-from-top] "
            "[--activate] [--follow-action] [--reload N] [--output FILE] "
            "[--user-css FILE] "
            "[--scroll-y N | --scroll-bottom] "
            "[--limit-mb N] [--psp-profile strict|realistic] "
            "[--low-memory-navigation] "
            "[--adaptive-resources | --no-adaptive-resources] "
            "[--forced-dark] "
            "[--max-download-kb N] "
            "[--no-external-resources] "
            "[--no-progressive-first-paint] "
            "[--wpt-test-rendered] [--hide-cookie-banners] "
            "[--script-timeout-ms N] [--probe-script FILE] "
            "[--script-heap-mb N] [--script-total-mb N] "
            "[--script-file-kb N] [--script-count N] "
            "[--visual-state-marker TEXT] [--visual-evidence] "
            "[--viewport-css-width N] [--viewport-css-height N] "
            "[--block-origin HOST]... "
            "[--content-blocker off|basic|custom] "
            "[--content-blocker-list FILE] "
            "[--trace-frames] [--trace-page] "
            "[--capture-http DIR | --capture-http-from-top-level N DIR | "
            "--replay-http DIR | --replay-http-response-keyed DIR] "
            "[--deterministic-replay-seed N] "
            "[--diagnostic-frame-safari] "
            "[--experimental-compressed-sections] [--experimental-section N] "
            "[--commands FILE | --interactive] [--loop-output-dir DIR] "
            "[--dump-text-metrics FILE] "
            "[--no-loop-capture] [--platform-sim]\n",
            program);
}

static void print_http_replay_ledger(void)
{
    FetchTraceReplayStats stats;
    if (!fetch_trace_replay_stats(&stats)) return;
    printf("http-replay-ledger mode=%s records=%zu claimed=%zu "
           "requests=%zu matched=%zu served=%zu rejected=%zu unmatched=%zu "
           "conflicts=%zu invalid=%zu shape-mismatches=%zu claimed-routes=",
           stats.response_keyed ? "response-keyed" : "strict",
           stats.record_count, stats.claimed_record_count,
           stats.request_count, stats.matched_request_count,
           stats.served_request_count, stats.rejected_request_count,
           stats.unmatched_request_count,
           stats.conflicting_request_count,
           stats.invalid_route_request_count,
           stats.request_shape_mismatch_count);
    bool any = false;
    for (size_t start = 0; start < stats.record_count;) {
        if (!fetch_trace_replay_record_was_claimed(start)) {
            start++;
            continue;
        }
        size_t end = start;
        while (end + 1 < stats.record_count
               && fetch_trace_replay_record_was_claimed(end + 1)) end++;
        printf("%s%zu", any ? "," : "", start);
        if (end != start) printf("-%zu", end);
        any = true;
        start = end + 1;
    }
    if (!any) printf("none");
    putchar('\n');
}

static size_t visual_evidence_decimal_digits(size_t value)
{
    size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        digits++;
    }
    return digits;
}

static void visual_evidence_print_hex(const char *value, size_t length)
{
    static const char hexadecimal[] = "0123456789abcdef";
    if (length == 0) {
        putchar('-');
        return;
    }
    const unsigned char *bytes = (const unsigned char *) value;
    for (size_t index = 0; index < length; index++) {
        putchar(hexadecimal[bytes[index] >> 4]);
        putchar(hexadecimal[bytes[index] & 15]);
    }
}

static bool visual_evidence_record_frame(VisualEvidenceFrames *frames,
                                         const NavigationSession *navigation)
{
    if (frames == NULL) return true;
    if (frames->frame_count >= VISUAL_EVIDENCE_MAX_FRAMES) {
        frames->overflow = true;
        return false;
    }
    const NavigationEntry *entry = navigation_current(navigation);
    frames->scroll_y[frames->frame_count++] = entry == NULL ? 0 : entry->scroll_y;
    return true;
}

static bool visual_evidence_clock_valid(
    const ScriptDeterministicReplayDiagnostics *replay)
{
    if (replay == NULL
        || replay->host_elapsed_ms
               > SCRIPT_DETERMINISTIC_CLOCK_MAX_HOST_ELAPSED_MS
        || replay->wall_elapsed_ms
               > SCRIPT_DETERMINISTIC_CLOCK_MAX_DOMAIN_ELAPSED_MS
        || replay->monotonic_elapsed_ms
               > SCRIPT_DETERMINISTIC_CLOCK_MAX_DOMAIN_ELAPSED_MS) {
        return false;
    }
    const uint64_t sources[] = {
        replay->sources.date_now,
        replay->sources.date_function,
        replay->sources.date_constructor,
        replay->sources.performance_now,
        replay->sources.performance_mark,
        replay->sources.performance_measure,
        replay->sources.animation_timeline,
        replay->sources.idle_deadline_time_remaining,
        replay->sources.animation_frame,
        replay->sources.event_timestamp,
        replay->sources.intersection_observer,
        replay->sources.idle_callback_start
    };
    uint64_t total = 0;
    for (size_t index = 0; index < sizeof(sources) / sizeof(sources[0]);
         index++) {
        if (sources[index]
                > SCRIPT_DETERMINISTIC_CLOCK_MAX_SOURCE_EVENTS
            || total
                 > SCRIPT_DETERMINISTIC_CLOCK_MAX_SOURCE_EVENTS
                       - sources[index]) {
            return false;
        }
        total += sources[index];
    }
    uint64_t wall = sources[0] + sources[1] + sources[2];
    uint64_t monotonic = sources[3] + sources[4] + sources[5]
                       + sources[6] + sources[7];
    uint64_t samples = sources[8] + sources[9] + sources[10] + sources[11];
    return replay->wall_observations == wall
        && replay->monotonic_observations == monotonic
        && replay->monotonic_samples == samples
        && replay->wall_elapsed_ms == replay->host_elapsed_ms + wall
        && replay->monotonic_elapsed_ms
               == replay->host_elapsed_ms + monotonic;
}

/* Emit one fixed-order ASCII record for machine qualification. Page-derived
   strings are hexadecimal UTF-8, so document newlines can remain useful in the
   human diagnostics without ever becoming forged evidence fields. The Python
   consumer requires exactly one complete v2 record and rejects all duplicates,
   reordered keys, unknown keys, or values outside these shared bounds. */
static bool print_visual_evidence_record(
    const NavigationSession *navigation, const char *marker,
    bool marker_found, const char *body_preview, size_t body_preview_length,
    const ExternalScriptMetrics *script_metrics,
    const ScriptDeterministicReplayDiagnostics *replay,
    const VisualEvidenceFrames *frames)
{
    if (navigation == NULL || marker == NULL || body_preview == NULL
        || script_metrics == NULL || replay == NULL || frames == NULL
        || frames->overflow || frames->frame_count == 0
        || replay->seed != replay->clock_origin_ms
        || !visual_evidence_clock_valid(replay)) {
        return false;
    }
    FetchTraceReplayStats stats;
    if (!fetch_trace_replay_stats(&stats) || !stats.response_keyed
        || stats.record_count > 4096
        || stats.claimed_record_count > stats.record_count) {
        return false;
    }
    const NavigationEntry *entry = navigation_current(navigation);
    const char *final_url = entry == NULL ? "" : entry->url;
    const char *title = navigation->page.document.title;
    if (title == NULL) title = "";
    size_t title_length = strlen(title);
    size_t url_length = strlen(final_url);
    size_t marker_length = strlen(marker);
    if (title_length > VISUAL_EVIDENCE_TEXT_LIMIT
        || url_length >= NAVIGATION_URL_LIMIT
        || marker_length > VISUAL_EVIDENCE_TEXT_LIMIT
        || body_preview_length > 512) {
        return false;
    }

    size_t route_characters = 0;
    bool any_route = false;
    for (size_t start = 0; start < stats.record_count;) {
        if (!fetch_trace_replay_record_was_claimed(start)) {
            start++;
            continue;
        }
        size_t end = start;
        while (end + 1 < stats.record_count
               && fetch_trace_replay_record_was_claimed(end + 1)) end++;
        route_characters += (any_route ? 1 : 0)
                          + visual_evidence_decimal_digits(start);
        if (end != start) {
            route_characters += 1 + visual_evidence_decimal_digits(end);
        }
        any_route = true;
        start = end + 1;
    }
    if (!any_route) route_characters = 4; /* none */
    size_t scroll_characters = 0;
    for (size_t index = 0; index < frames->frame_count; index++) {
        if (frames->scroll_y[index] < 0) return false;
        scroll_characters += (index == 0 ? 0 : 1)
                           + visual_evidence_decimal_digits(
                                 (size_t) frames->scroll_y[index]);
    }
    size_t encoded_characters = (title_length == 0 ? 1 : title_length * 2)
                              + (url_length == 0 ? 1 : url_length * 2)
                              + (body_preview_length == 0
                                   ? 1 : body_preview_length * 2)
                              + (marker_length == 0 ? 1 : marker_length * 2);
    if (encoded_characters + route_characters + scroll_characters + 4096
        > VISUAL_EVIDENCE_LINE_LIMIT) {
        return false;
    }

    fputs("tilefinch-visual-evidence-v2 schema=2 title-hex=", stdout);
    visual_evidence_print_hex(title, title_length);
    fputs(" url-hex=", stdout);
    visual_evidence_print_hex(final_url, url_length);
    fputs(" body-preview-hex=", stdout);
    visual_evidence_print_hex(body_preview, body_preview_length);
    printf(" http-status=%ld marker-hex=", navigation->last_http_status);
    visual_evidence_print_hex(marker, marker_length);
    printf(" marker-found=%s marker-bytes=%zu stylesheets-loaded=%zu "
           "images-loaded=%zu scripts-loaded=%zu network-completions=%zu "
           "active-native=%zu pending-logical=%zu seed=%llu "
           "seed-source=%s rng=%s clock=%s clock-scope=%s "
           "host-elapsed-ms=%llu wall-elapsed-ms=%llu "
           "monotonic-elapsed-ms=%llu wall-observations=%llu "
           "monotonic-observations=%llu monotonic-samples=%llu "
           "clock-date-now=%llu clock-date-function=%llu "
           "clock-date-constructor=%llu clock-performance-now=%llu "
           "clock-performance-mark=%llu clock-performance-measure=%llu "
           "clock-animation-timeline=%llu "
           "clock-idle-deadline-time-remaining=%llu "
           "clock-animation-frame=%llu clock-event-timestamp=%llu "
           "clock-intersection-observer=%llu "
           "clock-idle-callback-start=%llu performance-entries=%s "
           "intl=%s frames=%zu scrolls=",
           marker_found ? "yes" : "no", marker_length,
           navigation->page.external_stylesheets.loaded,
           navigation->page.images.stats.loaded, script_metrics->loaded,
           navigation->page.script_result.async_network_logical_completed,
           navigation->page.script_result.async_network_active_native,
           navigation->page.script_result.async_network_pending_logical,
           (unsigned long long) replay->seed,
           SCRIPT_DETERMINISTIC_REPLAY_SEED_SOURCE,
           SCRIPT_DETERMINISTIC_ENTROPY_CONTRACT,
           SCRIPT_DETERMINISTIC_CLOCK_CONTRACT,
           SCRIPT_DETERMINISTIC_CLOCK_EVIDENCE_SCOPE,
           (unsigned long long) replay->host_elapsed_ms,
           (unsigned long long) replay->wall_elapsed_ms,
           (unsigned long long) replay->monotonic_elapsed_ms,
           (unsigned long long) replay->wall_observations,
           (unsigned long long) replay->monotonic_observations,
           (unsigned long long) replay->monotonic_samples,
           (unsigned long long) replay->sources.date_now,
           (unsigned long long) replay->sources.date_function,
           (unsigned long long) replay->sources.date_constructor,
           (unsigned long long) replay->sources.performance_now,
           (unsigned long long) replay->sources.performance_mark,
           (unsigned long long) replay->sources.performance_measure,
           (unsigned long long) replay->sources.animation_timeline,
           (unsigned long long)
               replay->sources.idle_deadline_time_remaining,
           (unsigned long long) replay->sources.animation_frame,
           (unsigned long long) replay->sources.event_timestamp,
           (unsigned long long) replay->sources.intersection_observer,
           (unsigned long long) replay->sources.idle_callback_start,
           SCRIPT_DETERMINISTIC_PERFORMANCE_ENTRIES,
           SCRIPT_DETERMINISTIC_INTL_SURFACE, frames->frame_count);
    for (size_t index = 0; index < frames->frame_count; index++) {
        printf("%s%d", index == 0 ? "" : ",", frames->scroll_y[index]);
    }
    printf(" replay-mode=response-keyed route-selection=%s "
           "occurrence-claims=%zu reusable-claims=%zu occurrence-exhausted=%zu "
           "records=%zu claimed=%zu requests=%zu "
           "matched=%zu served=%zu rejected=%zu unmatched=%zu conflicts=%zu "
           "invalid=%zu shape-mismatches=%zu claimed-routes=",
           FETCH_TRACE_RESPONSE_KEY_ROUTE_SELECTION_VERSION,
           stats.occurrence_claim_count, stats.reusable_claim_count,
           stats.occurrence_exhausted_count,
           stats.record_count, stats.claimed_record_count, stats.request_count,
           stats.matched_request_count, stats.served_request_count,
           stats.rejected_request_count, stats.unmatched_request_count,
           stats.conflicting_request_count, stats.invalid_route_request_count,
           stats.request_shape_mismatch_count);
    any_route = false;
    for (size_t start = 0; start < stats.record_count;) {
        if (!fetch_trace_replay_record_was_claimed(start)) {
            start++;
            continue;
        }
        size_t end = start;
        while (end + 1 < stats.record_count
               && fetch_trace_replay_record_was_claimed(end + 1)) end++;
        printf("%s%zu", any_route ? "," : "", start);
        if (end != start) printf("-%zu", end);
        any_route = true;
        start = end + 1;
    }
    if (!any_route) fputs("none", stdout);
    putchar('\n');
    return !ferror(stdout);
}

static lxb_dom_node_t *find_element_id(lxb_dom_node_t *node,
                                       const char *wanted)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && strlen(wanted) == length
            && memcmp(id, wanted, length) == 0) return node;
        lxb_dom_node_t *found = find_element_id(node->first_child, wanted);
        if (found != NULL) return found;
    }
    return NULL;
}

static bool experimental_element_ordinal_walk(lxb_dom_node_t *node,
                                              lxb_dom_node_t *target,
                                              size_t *ordinal,
                                              size_t *target_ordinal)
{
    for (; node != NULL; node = node->next) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (node == target) {
                *target_ordinal = *ordinal;
                return true;
            }
            (*ordinal)++;
        }
        if (experimental_element_ordinal_walk(node->first_child, target,
                                              ordinal, target_ordinal)) {
            return true;
        }
    }
    return false;
}

static lxb_dom_node_t *experimental_element_at_ordinal(lxb_dom_node_t *node,
                                                       size_t wanted,
                                                       size_t *ordinal)
{
    for (; node != NULL; node = node->next) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (*ordinal == wanted) return node;
            (*ordinal)++;
        }
        lxb_dom_node_t *found = experimental_element_at_ordinal(
            node->first_child, wanted, ordinal);
        if (found != NULL) return found;
    }
    return NULL;
}

static bool experimental_focus_key(const PocDocument *document,
                                   size_t section, lxb_dom_node_t *node,
                                   char key[256], size_t *length)
{
    *length = 0;
    if (node == NULL) return true;
    size_t id_length = 0;
    const char *id = document_attribute(node, "id", &id_length);
    if (id != NULL && id_length != 0 && id_length <= 253) {
        key[0] = 'i';
        key[1] = ':';
        memcpy(key + 2, id, id_length);
        key[id_length + 2] = '\0';
        *length = id_length + 2;
        return true;
    }
    size_t ordinal = 0, target_ordinal = 0, name_length = 0;
    const char *name = document_element_name(node, &name_length);
    if (document == NULL || document->html == NULL || name == NULL
        || name_length == 0 || name_length >= 32
        || !experimental_element_ordinal_walk(
               lxb_dom_interface_node(document->html), node, &ordinal,
               &target_ordinal)) return false;
    int written = snprintf(key, 256, "s:%zu:%zu:%.*s", section,
                           target_ordinal, (int) name_length, name);
    if (written <= 0 || written >= 256) return false;
    *length = (size_t) written;
    return true;
}

static lxb_dom_node_t *experimental_find_focus_key(
    const PocDocument *document, size_t section, const char *key)
{
    if (document == NULL || document->html == NULL || key == NULL)
        return NULL;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    if (key[0] == 'i' && key[1] == ':' && key[2] != '\0')
        return find_element_id(root, key + 2);
    size_t key_section = 0, wanted = 0;
    char tag_name[32] = {0};
    int consumed = 0;
    if (sscanf(key, "s:%zu:%zu:%31s%n", &key_section, &wanted,
               tag_name, &consumed) != 3
        || consumed < 0 || key[(size_t) consumed] != '\0'
        || key_section != section) return NULL;
    size_t ordinal = 0, name_length = 0;
    lxb_dom_node_t *node = experimental_element_at_ordinal(
        root, wanted, &ordinal);
    const char *name = document_element_name(node, &name_length);
    if (name == NULL || strlen(tag_name) != name_length
        || strncasecmp(name, tag_name, name_length) != 0) return NULL;
    return node;
}

static void collect_node_text(lxb_dom_node_t *node, char *output,
                              size_t capacity, size_t *used)
{
    for (; node != NULL && *used + 1 < capacity; node = node->next) {
        size_t length = 0;
        const char *text = document_text_data(node, &length);
        if (text != NULL) {
            size_t room = capacity - *used - 1;
            size_t copy = length < room ? length : room;
            memcpy(output + *used, text, copy);
            *used += copy;
        } else if (node->first_child != NULL) {
            collect_node_text(node->first_child, output, capacity, used);
        }
    }
    output[*used] = '\0';
}

static lxb_dom_node_t *find_external_script(lxb_dom_node_t *node)
{
    for (; node != NULL; node = node->next) {
        size_t name_length = 0, source_length = 0;
        const char *name = document_element_name(node, &name_length);
        const char *source = name != NULL && name_length == 6
                             && memcmp(name, "script", 6) == 0
            ? document_attribute(node, "src", &source_length) : NULL;
        if (source != NULL && source_length != 0) return node;
        lxb_dom_node_t *found = find_external_script(node->first_child);
        if (found != NULL) return found;
    }
    return NULL;
}

static void trace_interaction_state(NavigationSession *navigation,
                                    const char *phase)
{
    if (getenv("TILEFINCH_TRACE_INTERACTION") == NULL
        || navigation == NULL || navigation->page.runtime == NULL) return;
    static const char probe[] =
        "(()=>{const p=document.querySelector('[data-mobile-composer-prompt]'),"
        "b=document.querySelector('[data-composer-submit]'),"
        "f=document.querySelector('[data-mobile-composer]'),"
        "v=document.querySelector('.wm-app-threadViewport'),"
        "d=globalThis.__webMobileDeclarativePartialUpdates?.getPerformanceSnapshot?.(),"
        "ab=document.querySelectorAll('[data-assistant-stream-block]');"
        "if(f&&!globalThis.__tilefinchInteractionSubmitInstalled){"
        "globalThis.__tilefinchInteractionSubmitInstalled=true;"
        "globalThis.__tilefinchInteractionSubmitEvents=0;"
        "globalThis.__tilefinchConversationControlEvents=[];"
        "f.addEventListener('submit',()=>globalThis.__tilefinchInteractionSubmitEvents++);"
        "document.addEventListener('web-mobile-conversation-control',event=>{const kind=event?.detail?.kind;if(kind&&globalThis.__tilefinchConversationControlEvents.length<32)globalThis.__tilefinchConversationControlEvents.push(String(kind));});}"
        "globalThis.pocSummary='promptLength='+(p?String(p.value.length):'missing')+"
        "',submitDisabled='+(b?String(b.disabled):'missing')+"
        "',submitType='+(b?String(b.type):'missing')+"
        "',submitEvents='+String(globalThis.__tilefinchInteractionSubmitEvents||0)+"
        "',clickDefault='+String(globalThis.__tilefinchLastClickDefault||'none')+"
        "',sentinel='+String(globalThis.__tilefinchSentinelStage||'none')+"
        "',dpu='+String(globalThis.__tilefinchDpuStage||'none')+"
        "',dpuApply='+String(d?.applyCalls ?? -1)+"
        "',dpuTemplates='+String(d?.templatesApplied ?? -1)+"
        "',dpuStale='+String(d?.presentationStaleTargetCount ?? -1)+"
        "',dpuQueued='+String(d?.presentationUnitQueuedCount ?? -1)+"
        "',dpuAdmitted='+String(d?.presentationUnitAdmittedCount ?? -1)+"
        "',answerBlocks='+String(ab.length)+"
        "',fragmentInserts='+String(globalThis.__tilefinchFragmentInsertCount||0)+"
        "',fragmentText='+String(globalThis.__tilefinchFragmentInsertText||'').slice(0,80)+"
        "',viewportScroll='+(v?String(v.scrollTop)+'/'+String(v.scrollHeight)+'/'+String(v.clientHeight):'missing')+"
        "',windowScroll='+String(globalThis.scrollY||0)+"
        "',controls='+String(globalThis.__tilefinchConversationControlEvents||[]);})()";
    if (script_runtime_evaluate_diagnostic(
            navigation->page.runtime, probe, "<interaction-state>",
            &navigation->page.script_result)) {
        fprintf(stderr, "interaction-state phase=%s %s\n", phase,
                navigation->page.script_result.summary);
    }
}

static void dump_probe_dom(lxb_dom_node_t *node, unsigned depth,
                           size_t *remaining)
{
    for (; node != NULL && *remaining != 0; node = node->next) {
        size_t name_length = 0, id_length = 0, class_length = 0;
        const char *name = document_element_name(node, &name_length);
        if (name != NULL) {
            const char *id = document_attribute(node, "id", &id_length);
            const char *class_name = document_attribute(node, "class",
                                                        &class_length);
            printf("probe-dom depth=%u tag=%.*s id=\"%.*s\" class=\"%.*s\"\n",
                   depth, (int) name_length, name,
                   (int) id_length, id == NULL ? "" : id,
                   (int) class_length,
                   class_name == NULL ? "" : class_name);
            (*remaining)--;
        } else {
            size_t text_length = 0;
            const char *text_value = document_text_data(node, &text_length);
            size_t parent_name_length = 0;
            const char *parent_name = node->parent == NULL ? NULL
                : document_element_name(node->parent, &parent_name_length);
            bool sensitive_source = parent_name != NULL
                && ((parent_name_length == 6
                     && memcmp(parent_name, "script", 6) == 0)
                    || (parent_name_length == 5
                        && memcmp(parent_name, "style", 5) == 0));
            if (!sensitive_source && text_value != NULL && text_length != 0) {
                size_t shown = text_length < 160 ? text_length : 160;
                printf("probe-dom depth=%u text=\"%.*s\"\n", depth,
                       (int) shown, text_value);
                (*remaining)--;
            }
        }
        dump_probe_dom(node->first_child, depth + 1, remaining);
    }
}
