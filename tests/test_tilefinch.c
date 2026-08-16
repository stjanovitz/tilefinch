#include "tilefinch_test_common.h"
#include "../src/psp_network_policy.h"

static bool test_cache_put_stylesheet(BrowserSession *session,
                                      const char *url,
                                      const unsigned char *data,
                                      size_t length, const char *etag)
{
    if (session == NULL || url == NULL
        || data == NULL || length == 0) return false;
    unsigned char *copy = budget_malloc(session->budget, length);
    if (copy == NULL) return false;
    memcpy(copy, data, length);
    BrowserSharedBody *body = browser_shared_body_take(
        session->budget, copy, length);
    if (body == NULL) {
        budget_free(session->budget, copy);
        return false;
    }
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = url,
        .top_level_url = url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    TilefinchResourceGrant grant = {
        .destination = TILEFINCH_DESTINATION_STYLE,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true,
        .final_same_site = true,
        .cors_validated = true
    };
    bool stored = browser_session_cache_put_http_shared_resource(
            session, url, body, etag, NULL, "text/css", "immutable", NULL,
            0, &context, &grant)
        && browser_session_cache_set_resource_response_provenance(
            session, url, &context, url, "");
    browser_shared_body_release(body);
    return stored;
}

static bool test_cache_put_classic_script(
    BrowserSession *session, const char *document_url, const char *script_url,
    const unsigned char *source, size_t source_length)
{
    if (session == NULL || document_url == NULL || script_url == NULL
        || source == NULL || source_length == 0) return false;
    unsigned char *copy = budget_malloc(session->budget, source_length + 1u);
    if (copy == NULL) return false;
    memcpy(copy, source, source_length);
    copy[source_length] = 0;
    BrowserSharedBody *body = browser_shared_body_take(
        session->budget, copy, source_length);
    if (body == NULL) {
        budget_free(session->budget, copy);
        return false;
    }
    TilefinchRequestContext context = {
        .target_url = script_url, .initiator_url = document_url,
        .top_level_url = document_url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    TilefinchResourceGrant grant = {
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true
    };
    bool stored = browser_session_cache_put_http_shared_classic_script(
        session, script_url, body, NULL, NULL, "text/javascript",
        "immutable", NULL, 0, &context, &grant);
    browser_shared_body_release(body);
    return stored;
}

static bool deterministic_entropy_probe(
    PocDocument *document, Budget *budget, const ViewportContext *viewport,
    uint64_t seed, const char *url, ScriptDocumentScope scope,
    char *summary, size_t summary_size)
{
    static const char source[] =
        "(()=>{const failures=[],invalid=[new Float32Array(1),"
        "new DataView(new ArrayBuffer(8)),new Uint8Array(65537)];"
        "for(const value of invalid)try{crypto.getRandomValues(value);"
        "failures.push('none')}catch(error){failures.push(error.name)}"
        "const unit=9007199254740992,first=Math.random()*unit,"
        "bytes=new Uint8Array(13),same=crypto.getRandomValues(bytes)===bytes;"
        "const uuid=crypto.randomUUID(),tail=new Uint8Array(1);"
        "crypto.getRandomValues(tail);const last=Math.random()*unit,"
        "hex=array=>Array.from(array,value=>value.toString(16)"
        ".padStart(2,'0')).join('');globalThis.pocSummary="
        "same&&failures.join(',')==='TypeMismatchError,TypeMismatchError,QuotaExceededError'"
        "?first.toString(16)+'|'+hex(bytes)+'|'+uuid+'|'"
        "+hex(tail)+'|'+last.toString(16):'CRYPTO-VALIDATION-FAILED:'"
        "+failures.join(',')})()";
    ScriptRuntimeOptions options = {
        .viewport = *viewport,
        .defer_document_scripts = true,
        .document_scope = scope
    };
    ScriptResult result;
    script_runtime_configure_deterministic_replay(true, seed);
    ScriptRuntime *runtime = script_runtime_create_configured(
        document, budget, 4u * MIB, 1000, url, &options, &result);
    bool ok = runtime != NULL
        && script_runtime_evaluate_diagnostic(
            runtime, source, "<deterministic-entropy-vector>", &result)
        && result.success && result.summary[0] != '\0';
    if (ok && summary != NULL && summary_size != 0) {
        snprintf(summary, summary_size, "%s", result.summary);
    }
    script_runtime_destroy(runtime);
    return ok;
}

typedef struct {
    char *data;
    size_t length;
} TestSerialization;

static bool test_cache_put_module(
    BrowserSession *session, const char *url, const unsigned char *data,
    size_t length, const char *initiator_origin)
{
    BrowserModuleCacheProvenance provenance = {
        .effective_url = url,
        .initiator_origin = initiator_origin,
        .top_level_url = initiator_origin,
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .cors_validated = true,
        .javascript_mime_validated = true
    };
    return browser_session_cache_put_http_module(
        session, url, data, length, NULL, NULL, "text/javascript",
        "immutable", NULL, 0, &provenance);
}

static void test_store_le32(unsigned char *destination, uint32_t value)
{
    destination[0] = (unsigned char) value;
    destination[1] = (unsigned char) (value >> 8);
    destination[2] = (unsigned char) (value >> 16);
    destination[3] = (unsigned char) (value >> 24);
}

typedef struct {
    size_t calls;
    size_t final_calls;
    size_t ready_calls;
    size_t cancel_at;
    size_t last_indexed_bytes;
    bool monotonic;
} TestSectionProgress;

static bool test_section_progress(
    void *opaque, const CompressedSectionStore *store,
    size_t indexed_source_bytes, bool final)
{
    TestSectionProgress *progress = opaque;
    progress->calls++;
    if (indexed_source_bytes < progress->last_indexed_bytes)
        progress->monotonic = false;
    progress->last_indexed_bytes = indexed_source_bytes;
    if (store->section_count != 0) progress->ready_calls++;
    if (final) progress->final_calls++;
    return progress->cancel_at == 0
        || progress->calls < progress->cancel_at;
}

typedef struct {
    size_t calls;
    char last_selector[64];
} TestRemoteSelectorProbe;

static bool test_remote_selector_lookup(
    void *opaque, const char *selector, size_t length,
    size_t *section_index, char tag_name[32], char identifier[129],
    char stable_key[96])
{
    TestRemoteSelectorProbe *probe = opaque;
    probe->calls++;
    size_t copied = length < sizeof(probe->last_selector) - 1
        ? length : sizeof(probe->last_selector) - 1;
    memcpy(probe->last_selector, selector, copied);
    probe->last_selector[copied] = '\0';
    (void) section_index;
    (void) tag_name;
    (void) identifier;
    (void) stable_key;
    return false;
}

static bool test_bound_remote_selector_lookup(
    void *opaque, const char *selector, size_t length,
    size_t *section_index, char tag_name[32], char identifier[129],
    char stable_key[96])
{
    TestRemoteSelectorProbe *probe = opaque;
    probe->calls++;
    if (length != 5 || memcmp(selector, "aside", 5) != 0) return false;
    *section_index = 1;
    snprintf(tag_name, 32, "aside");
    snprintf(identifier, 129, "remote-aside");
    snprintf(stable_key, 96, "id:remote-aside");
    return true;
}


static lxb_status_t test_serialize_receive(const lxb_char_t *data,
                                           size_t length, void *opaque)
{
    TestSerialization *serialized = opaque;
    if (length > SIZE_MAX - serialized->length - 1) return LXB_STATUS_ERROR;
    char *expanded = realloc(serialized->data,
                             serialized->length + length + 1);
    if (expanded == NULL) return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    serialized->data = expanded;
    memcpy(serialized->data + serialized->length, data, length);
    serialized->length += length;
    serialized->data[serialized->length] = '\0';
    return LXB_STATUS_OK;
}

static bool test_document_equivalent(PocDocument *left,
                                     PocDocument *right)
{
    TestSerialization left_html = {0}, right_html = {0};
    const char *left_body = document_body_text(left);
    const char *right_body = document_body_text(right);
    bool ok = left->node_count == right->node_count
        && left->element_count == right->element_count
        && left->text_bytes == right->text_bytes
        && strcmp(left->title, right->title) == 0
        && left_body != NULL && right_body != NULL
        && strcmp(left_body, right_body) == 0
        && lxb_html_serialize_tree_cb(lxb_dom_interface_node(left->html),
                                      test_serialize_receive, &left_html)
               == LXB_STATUS_OK
        && lxb_html_serialize_tree_cb(lxb_dom_interface_node(right->html),
                                      test_serialize_receive, &right_html)
               == LXB_STATUS_OK
        && left_html.length == right_html.length
        && memcmp(left_html.data, right_html.data, left_html.length) == 0;
    free(left_html.data);
    free(right_html.data);
    return ok;
}

typedef struct {
    size_t calls;
} ParserCooperateProbe;

static bool test_parser_cancel_cooperate(void *context, const char *phase,
                                         size_t completed_work_units)
{
    ParserCooperateProbe *probe = context;
    if (strcmp(phase, "document-parse") != 0
        || completed_work_units == 0) return true;
    probe->calls++;
    return false;
}

static bool test_large_parser_feed_is_cooperative(Budget *budget)
{
    enum { LARGE_FEED_BYTES = 24 * 1024 };
    char *html = malloc(LARGE_FEED_BYTES);
    if (html == NULL) return false;
    memset(html, 'x', LARGE_FEED_BYTES);
    memcpy(html, "<!doctype html><body>", 21);
    DocumentParser parser = {0};
    ParserCooperateProbe probe = {0};
    TilefinchPlatformServices services = {
        .context = &probe,
        .cooperate = test_parser_cancel_cooperate
    };
    bool began = document_parser_begin(&parser, budget);
    tilefinch_platform_set_services(&services);
    bool fed = began && document_parser_feed(
        &parser, html, LARGE_FEED_BYTES);
    tilefinch_platform_set_services(NULL);
    bool ok = began && !fed && probe.calls == 1
        && parser.failed && parser.bytes_fed == 8u * 1024u;
    document_parser_abort(&parser);
    free(html);
    return ok;
}

static bool test_incremental_parser_boundaries(Budget *budget)
{
    static const char *fixtures[] = {
        "<!doctype html><!--x--><title>T &amp; &#x1f642;</title>"
        "<body><p data-x='a&amp;b' disabled>caf\xc3\xa9 \xe2\x98\x83</p>",
        "<style>.x:before{content:'<>&'}</style><body><script>"
        "if (1 < 2) globalThis.x='</not-script>';/* &amp; */</script>tail",
        "<title>R &amp; D</title><body><textarea>a&amp;b<z>\r\nq</textarea>"
        "<p title=unquoted>x<!--broken",
        "<table>text<tr><td>A<td>B</table><template><div data-v='1'>"
        "inside</template><select><option>one<option>two</select>",
        "<!DOCTYPE html PUBLIC \"x\"><body><b><i>misnested</b> tail</i>"
        "<svg viewBox='0 0 1 1'><foreignObject><p>x</p></foreignObject></svg>"
    };
    for (size_t fixture = 0;
         fixture < sizeof(fixtures) / sizeof(fixtures[0]); fixture++) {
        const char *html = fixtures[fixture];
        size_t length = strlen(html);
        PocDocument baseline = {0};
        if (!document_parse(&baseline, budget, html, length, length)) {
            return false;
        }
        for (size_t split = 1; split < length; split++) {
            DocumentParser parser;
            PocDocument candidate = {0};
            bool ok = document_parser_begin(&parser, budget)
                && document_parser_feed(&parser, html, split)
                && document_parser_feed(&parser, html + split,
                                        length - split)
                && document_parser_finish(&parser, &candidate)
                && test_document_equivalent(&baseline, &candidate);
            document_parser_abort(&parser);
            document_destroy(&candidate);
            if (!ok) {
                fprintf(stderr,
                        "incremental parser mismatch fixture=%zu split=%zu\n",
                        fixture, split);
                document_destroy(&baseline);
                return false;
            }
        }
        PocDocument bytewise = {0};
        if (!document_parse(&bytewise, budget, html, length, 1)
            || !test_document_equivalent(&baseline, &bytewise)) {
            document_destroy(&bytewise);
            document_destroy(&baseline);
            return false;
        }
        document_destroy(&bytewise);
        document_destroy(&baseline);
    }
    return true;
}

static bool test_parser_scripting_noscript_model(Budget *budget)
{
    static const char html[] =
        "<!doctype html><head><noscript><style id=fallback-style>"
        ".fallback{display:none}</style></noscript></head><body>"
        "<noscript><p id=fallback-body>fallback</p></noscript>"
        "<p id=content>content</p></body>";
    DocumentParser disabled_parser = {0}, enabled_parser = {0};
    PocDocument disabled = {0}, enabled = {0};
    bool ok = document_parser_begin(&disabled_parser, budget)
        && document_parser_set_scripting(&disabled_parser, false)
        && document_parser_feed(&disabled_parser, html, sizeof(html) - 1)
        && !document_parser_set_scripting(&disabled_parser, true)
        && document_parser_finish(&disabled_parser, &disabled)
        && find_id(lxb_dom_interface_node(disabled.html),
                   "fallback-style") != NULL
        && find_id(lxb_dom_interface_node(disabled.html),
                   "fallback-body") != NULL
        && document_parser_begin(&enabled_parser, budget)
        && document_parser_set_scripting(&enabled_parser, true)
        && document_parser_feed(&enabled_parser, html, sizeof(html) - 1)
        && document_parser_finish(&enabled_parser, &enabled)
        && find_id(lxb_dom_interface_node(enabled.html),
                   "fallback-style") == NULL
        && find_id(lxb_dom_interface_node(enabled.html),
                   "fallback-body") == NULL
        && find_id(lxb_dom_interface_node(enabled.html), "content") != NULL;
    document_parser_abort(&disabled_parser);
    document_parser_abort(&enabled_parser);
    document_destroy(&disabled);
    document_destroy(&enabled);
    return ok;
}

typedef struct {
    lxb_dom_node_t *script;
    bool before_visible;
    bool after_hidden;
    size_t closed_elements;
} TestParserLifecycle;

static bool test_parser_element_closed(void *opaque, PocDocument *document,
                                       lxb_dom_node_t *element)
{
    TestParserLifecycle *lifecycle = opaque;
    lifecycle->closed_elements++;
    size_t name_length = 0;
    const char *name = document_element_name(element, &name_length);
    if (name != NULL && name_length == 6
        && memcmp(name, "script", 6) == 0) {
        lifecycle->script = element;
        lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
        lifecycle->before_visible = find_id(root, "before") != NULL;
        lifecycle->after_hidden = find_id(root, "after") == NULL;
    }
    return true;
}

static bool test_parser_lifecycle_hook(Budget *budget)
{
    static const char html[] =
        "<!doctype html><body><p id=before>before</p><script id=run>"
        "globalThis.order=['run']</script><p id=after>after</p></body>";
    DocumentParser parser;
    TestParserLifecycle lifecycle = {0};
    PocDocument document = {0};
    if (!document_parser_begin(&parser, budget)) return false;
    document_parser_set_element_closed_callback(
        &parser, test_parser_element_closed, &lifecycle);
    for (size_t i = 0; i < sizeof(html) - 1; i++) {
        if (!document_parser_feed(&parser, html + i, 1)) {
            document_parser_abort(&parser);
            return false;
        }
    }
    bool ok = document_parser_finish(&parser, &document)
        && lifecycle.script != NULL && lifecycle.before_visible
        && lifecycle.after_hidden && lifecycle.closed_elements >= 4
        && find_id(lxb_dom_interface_node(document.html), "after") != NULL;
    document_parser_abort(&parser);
    document_destroy(&document);
    return ok;
}

typedef struct {
    Budget *budget;
    ScriptRuntime *runtime;
    ScriptResult result;
    bool mutated;
    bool parser_preserved;
} TestParserInnerHtmlMutation;

static bool test_parser_inner_html_mutation(void *opaque,
                                            PocDocument *document,
                                            lxb_dom_node_t *element)
{
    TestParserInnerHtmlMutation *probe = opaque;
    size_t name_length = 0;
    const char *name = document_element_name(element, &name_length);
    if (name == NULL || name_length != 6
        || memcmp(name, "script", 6) != 0) return true;
    lxb_html_parser_t *parser = document->html->dom_document.parser;
    probe->runtime = script_runtime_create(
        document, probe->budget, 2 * MIB, 1000,
        "https://parser-inner-html.test/", &probe->result);
    probe->mutated = probe->runtime != NULL
        && script_runtime_evaluate_diagnostic(
            probe->runtime,
            "document.getElementById('target').innerHTML="
            "'<svg><text id=nested>nested</text></svg>';"
            "globalThis.pocSummary=document.getElementById('nested')"
            "?'nested:ok':'nested:missing'",
            "<parser-inner-html>", &probe->result)
        && strcmp(probe->result.summary, "nested:ok") == 0;
    probe->parser_preserved =
        document->html->dom_document.parser == parser
        && parser != NULL
        && lxb_html_parser_state(parser) == LXB_HTML_PARSER_STATE_PROCESS;
    return probe->mutated && probe->parser_preserved;
}

static bool test_parser_reentrant_inner_html(Budget *budget)
{
    static const char html[] =
        "<!doctype html><body><div id=target>old</div>"
        "<script>replace()</script>"
        "<svg><text id=after>after</text></svg></body>";
    DocumentParser parser;
    PocDocument document = {0};
    TestParserInnerHtmlMutation probe = {.budget = budget};
    if (!document_parser_begin(&parser, budget)) return false;
    document_parser_set_element_closed_callback(
        &parser, test_parser_inner_html_mutation, &probe);
    bool parsed = document_parser_feed(&parser, html, sizeof(html) - 1)
        && document_parser_finish(&parser, &document);
    lxb_dom_node_t *root = parsed
        ? lxb_dom_interface_node(document.html) : NULL;
    bool ok = parsed && probe.mutated && probe.parser_preserved
        && find_id(root, "nested") != NULL
        && find_id(root, "after") != NULL;
    script_runtime_destroy(probe.runtime);
    document_parser_abort(&parser);
    document_destroy(&document);
    return ok;
}

typedef struct {
    PocDocument *incumbent;
    ScriptRuntime *runtime;
    ScriptResult result;
    bool mutated;
} TestReentrantParserMutation;

static bool test_reentrant_parser_mutation(void *opaque,
                                           PocDocument *candidate,
                                           lxb_dom_node_t *element)
{
    (void) element;
    TestReentrantParserMutation *probe = opaque;
    if (probe == NULL || probe->mutated) return false;
    static const lxb_char_t incumbent_tag[] = "aside";
    static const lxb_char_t incumbent_text[] = "reentrant incumbent";
    BudgetAllocationOwner previous = document_allocation_owner_enter(
        probe->incumbent);
    lxb_dom_element_t *aside = lxb_dom_document_create_element(
        &probe->incumbent->html->dom_document, incumbent_tag,
        sizeof(incumbent_tag) - 1, NULL);
    lxb_dom_text_t *text = lxb_dom_document_create_text_node(
        &probe->incumbent->html->dom_document, incumbent_text,
        sizeof(incumbent_text) - 1);
    lxb_dom_node_t *body = document_body_node(probe->incumbent);
    bool incumbent_ok = aside != NULL && text != NULL && body != NULL
        && lxb_dom_node_append_child(
               lxb_dom_interface_node(aside),
               lxb_dom_interface_node(text)) == LXB_DOM_EXCEPTION_OK
        && lxb_dom_node_append_child(
               body, lxb_dom_interface_node(aside))
               == LXB_DOM_EXCEPTION_OK;
    document_allocation_owner_leave(probe->incumbent, previous);

    /* The callback resumes in the candidate's owner. This node must be
       reclaimed when returning false aborts the parser transaction. */
    static const lxb_char_t candidate_tag[] = "mark";
    lxb_dom_element_t *candidate_node = lxb_dom_document_create_element(
        &candidate->html->dom_document, candidate_tag,
        sizeof(candidate_tag) - 1, NULL);
    static const char runtime_mutation[] =
        "const reentrant=document.createElement('aside');"
        "reentrant.textContent='runtime reentrant';"
        "document.body.appendChild(reentrant);";
    bool runtime_ok = probe->runtime != NULL
        && script_runtime_evaluate_diagnostic(
            probe->runtime, runtime_mutation, "<reentrant-incumbent>",
            &probe->result);
    probe->mutated = incumbent_ok && candidate_node != NULL
        && document_refresh(probe->incumbent) && runtime_ok;
    return false;
}

static bool test_reentrant_incumbent_owner_isolation(Budget *budget)
{
    static const char incumbent_html[] =
        "<!doctype html><title>Incumbent</title><body>stable";
    static const char candidate_html[] =
        "<!doctype html><body><p>candidate</p><div>abort</div>";
    PocDocument incumbent = {0};
    if (!document_parse(&incumbent, budget, incumbent_html,
                        sizeof(incumbent_html) - 1, 32)) return false;
    TestReentrantParserMutation probe = {.incumbent = &incumbent};
    probe.runtime = script_runtime_create(
        &incumbent, budget, 2 * MIB, 1000,
        "https://incumbent.test/", &probe.result);
    DocumentParser parser;
    bool began = document_parser_begin(&parser, budget);
    if (began) {
        document_parser_set_element_closed_callback(
            &parser, test_reentrant_parser_mutation, &probe);
    }
    bool rejected = began && !document_parser_feed(
        &parser, candidate_html, sizeof(candidate_html) - 1);
    document_parser_abort(&parser);
    const char *body_text = document_body_text(&incumbent);
    bool ok = rejected && probe.mutated
        && strcmp(incumbent.title, "Incumbent") == 0
        && body_text != NULL
        && strstr(body_text, "reentrant incumbent") != NULL
        && strstr(body_text, "runtime reentrant") != NULL;
    script_runtime_destroy(probe.runtime);
    document_destroy(&incumbent);
    return ok && budget->current == 0
        && budget->active_allocation_owner == BUDGET_ALLOCATION_OWNER_NONE;
}

typedef struct {
    DocumentParser parser;
    size_t header_callbacks;
    size_t stall_callbacks;
} TestStreamConsumer;

static bool test_stream_headers(void *opaque, const FetchResult *metadata)
{
    TestStreamConsumer *consumer = opaque;
    consumer->header_callbacks++;
    return metadata->status_code == 200
        && strcmp(metadata->content_type, "text/html; charset=utf-8") == 0;
}

static bool test_stream_body(void *opaque, const unsigned char *data,
                             size_t length)
{
    TestStreamConsumer *consumer = opaque;
    return document_parser_feed(&consumer->parser,
                                (const char *) data, length);
}

static bool test_section_store_stream_body(void *opaque,
                                           const unsigned char *data,
                                           size_t length)
{
    return section_store_stream_append(opaque, data, length);
}

static bool test_section_store_transport_fault(size_t cancel_after,
                                               size_t truncate_after)
{
    Budget budget;
    budget_init(&budget, 4 * MIB);
    CompressedSectionStore store = {0};
    SectionStoreStreamBuilder builder = {0};
    char error[256] = {0};
    bool trace_ready = fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream", error,
        sizeof(error));
    FetchResult result = {.budget = &budget};
    FetchStreamMetrics metrics = {0};
    FetchStreamOptions stream = {
        .on_body = test_section_store_stream_body,
        .opaque = &builder,
        .chunk_bytes = 7,
        .cancel_after_bytes = cancel_after,
        .truncate_after_bytes = truncate_after
    };
    FetchRequest request = {
        .method = "GET", .send_low_client_hints = true,
        .sec_fetch_user = true, .upgrade_insecure_requests = true
    };
    bool began = trace_ready && section_store_stream_begin(
        &builder, &store, &budget, 4096, 8192);
    bool fetched = began && fetch_request_stream_cancelable(
        &budget, "https://stream.test/document", &request, 4096, 1000,
        NULL, NULL, &stream, &metrics, &result);
    bool signaled = cancel_after != 0 ? metrics.cancelled
                                     : metrics.truncated;
    section_store_stream_abort(&builder);
    section_store_destroy(&store);
    fetch_result_destroy(&result);
    fetch_trace_end();
    return began && !fetched && signaled && budget.current == 0;
}

static bool test_stream_stall(void *opaque)
{
    TestStreamConsumer *consumer = opaque;
    consumer->stall_callbacks++;
    return true;
}

static bool test_replay_stream_schedule(
    Budget *budget, size_t chunk_bytes, uint64_t irregular_seed,
    size_t irregular_max, size_t stall_every, size_t cancel_after,
    size_t truncate_after, PocDocument *document,
    FetchStreamMetrics *metrics, bool expected_success)
{
    char error[256] = {0};
    if (!fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
            error, sizeof(error))) return false;
    TestStreamConsumer consumer = {0};
    FetchResult result = {.budget = budget};
    bool began = document_parser_begin(&consumer.parser, budget);
    FetchStreamOptions stream = {
        .on_headers = test_stream_headers,
        .on_body = test_stream_body,
        .on_stall = test_stream_stall,
        .opaque = &consumer,
        .chunk_bytes = chunk_bytes,
        .irregular_seed = irregular_seed,
        .irregular_max_chunk_bytes = irregular_max,
        .stall_every_chunks = stall_every,
        .cancel_after_bytes = cancel_after,
        .truncate_after_bytes = truncate_after
    };
    FetchRequest request = {
        .method = "GET", .send_low_client_hints = true,
        .sec_fetch_user = true, .upgrade_insecure_requests = true
    };
    bool fetched = began && fetch_request_stream_cancelable(
        budget, "https://stream.test/document", &request, 4096, 1000,
        NULL, NULL, &stream, metrics, &result);
    bool finished = fetched
        && document_parser_finish(&consumer.parser, document);
    bool ok = expected_success
        ? finished && result.length == 383 && metrics->bytes_received == 383
          && metrics->peak_buffered_bytes <= 65536
          && metrics->headers_delivered && consumer.header_callbacks == 1
          && consumer.stall_callbacks == metrics->replay_stalls
        : !fetched && !finished && result.length == metrics->bytes_received;
    document_parser_abort(&consumer.parser);
    fetch_result_destroy(&result);
    fetch_trace_end();
    return ok;
}

static bool test_streaming_replay(Budget *budget)
{
    static const size_t chunks[] = {1, 7, 64, 1024, 16384};
    PocDocument baseline = {0};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        PocDocument candidate = {0};
        FetchStreamMetrics metrics;
        if (!test_replay_stream_schedule(
                budget, chunks[i], 0, 0, i == 1 ? 3 : 0, 0, 0,
                &candidate, &metrics, true)
            || (i != 0
                && !test_document_equivalent(&baseline, &candidate))) {
            document_destroy(&candidate);
            document_destroy(&baseline);
            return false;
        }
        if (i == 0) {
            baseline = candidate;
        } else {
            document_destroy(&candidate);
        }
    }
    PocDocument irregular = {0};
    FetchStreamMetrics irregular_metrics;
    bool ok = test_replay_stream_schedule(
        budget, 0, UINT64_C(0x123456789abcdef), 37, 2, 0, 0,
        &irregular, &irregular_metrics, true)
        && test_document_equivalent(&baseline, &irregular)
        && irregular_metrics.chunks_received > 4
        && irregular_metrics.replay_stalls > 0;
    document_destroy(&irregular);
    FetchStreamMetrics cancelled, truncated;
    PocDocument rejected = {0};
    ok = ok && test_replay_stream_schedule(
        budget, 7, 0, 0, 0, 23, 0, &rejected, &cancelled, false)
        && cancelled.cancelled && cancelled.bytes_received == 23;
    document_destroy(&rejected);
    ok = ok && test_replay_stream_schedule(
        budget, 7, 0, 0, 0, 0, 29, &rejected, &truncated, false)
        && truncated.truncated && truncated.bytes_received == 29;
    document_destroy(&rejected);
    document_destroy(&baseline);
    return ok;
}

static bool test_response_keyed_visual_replay(Budget *budget)
{
    size_t baseline = budget->current;
    char error[256] = {0};
    FetchRequest changed_shape = {
        .method = "GET",
        /* fixtures/http-stream retained the historical value true. */
        .send_low_client_hints = false,
        .sec_fetch_user = true,
        .upgrade_insecure_requests = true
    };
    FetchRequest retained_shape = {
        .method = "GET",
        .send_low_client_hints = true
    };
    FetchResult result = {.budget = budget};
    bool strict_ready = fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
        error, sizeof(error));
    bool strict_rejected = strict_ready
        && !fetch_request_cancelable(
               budget, "https://stream.test/document", &changed_shape,
               4096, 1000, NULL, NULL, &result)
        && strstr(result.error, "does not match") != NULL;
    fetch_result_destroy(&result);
    fetch_trace_end();

    memset(error, 0, sizeof(error));
    bool keyed_ready = fetch_trace_replay_begin_response_keyed(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
        error, sizeof(error));
    bool repeated = keyed_ready;
    for (size_t attempt = 0; repeated && attempt < 2; attempt++) {
        result = (FetchResult) {.budget = budget};
        repeated = fetch_request_cancelable(
            budget, "https://stream.test/document", &changed_shape,
            4096, 1000, NULL, NULL, &result)
            && result.status_code == 200 && result.length == 383;
        fetch_result_destroy(&result);
    }
    FetchTraceReplayStats stats = {0};
    bool ledger = repeated && fetch_trace_replay_stats(&stats)
        && stats.response_keyed && stats.record_count == 1
        && stats.claimed_record_count == 1
        && stats.request_count == 2 && stats.matched_request_count == 2
        && stats.served_request_count == 2
        && stats.rejected_request_count == 0
        && stats.unmatched_request_count == 0
        && stats.conflicting_request_count == 0
        && stats.invalid_route_request_count == 0
        && stats.request_shape_mismatch_count == 2
        && stats.occurrence_claim_count == 0
        && stats.reusable_claim_count == 2
        && stats.occurrence_exhausted_count == 0
        && fetch_trace_replay_record_was_claimed(0)
        && !fetch_trace_replay_record_was_claimed(1);
    fetch_trace_end();

    memset(error, 0, sizeof(error));
    bool occurrence_ready = fetch_trace_replay_begin_response_keyed(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-response-key-occurrence",
        error, sizeof(error));
    static const char *occurrence_bodies[] = {
        "alpha\n", "alpha\n", "bravo\n"
    };
    static const char *occurrence_headers[] = {"A", "A", "B"};
    static const char *occurrence_servers[] = {
        "fixture-a", "fixture-a", "fixture-b"
    };
    static const char *occurrence_cookies[] = {
        "sequence=xxxxx; Path=/",
        "sequence=xxxxx; Path=/",
        "sequence=xxxxxxx; Path=/"
    };
    static const size_t occurrence_delays[] = {1, 1, 3};
    bool occurrence_order = occurrence_ready;
    for (size_t index = 0;
         occurrence_order
         && index < sizeof(occurrence_bodies) / sizeof(occurrence_bodies[0]);
         index++) {
        char header[32] = {0};
        result = (FetchResult) {.budget = budget};
        occurrence_order = fetch_request_cancelable(
                budget, "https://visual-occurrence.test/resource",
                &changed_shape, 4096, 1000, NULL, NULL, &result)
            && result.status_code == 200
            && strcmp(result.data, occurrence_bodies[index]) == 0
            && strcmp(result.server, occurrence_servers[index]) == 0
            && result.trace_delay_pumps == occurrence_delays[index]
            && fetch_response_header_value(
                   &result, "x-occurrence", header, sizeof(header))
            && strcmp(header, occurrence_headers[index]) == 0
            && result.set_cookie_count == 1
            && strcmp(
                   result.set_cookies[0], occurrence_cookies[index]) == 0;
        fetch_result_destroy(&result);
    }
    result = (FetchResult) {.budget = budget};
    bool exhausted = occurrence_order
        && !fetch_request_cancelable(
               budget, "https://visual-occurrence.test/resource",
               &changed_shape, 4096, 1000, NULL, NULL, &result)
        && strstr(result.error, "occurrence sequence exhausted") != NULL
        && result.data == NULL && result.set_cookie_count == 0;
    fetch_result_destroy(&result);

    result = (FetchResult) {.budget = budget};
    bool selected_failure = exhausted
        && !fetch_request_cancelable(
               budget, "https://visual-occurrence.test/reusable",
               &retained_shape, 4, 1000, NULL, NULL, &result)
        && strstr(result.error, "exceeds request quota") != NULL
        && result.data == NULL && result.set_cookie_count == 0;
    FetchTraceReplayStats selected_failure_stats = {0};
    selected_failure = selected_failure
        && fetch_trace_replay_stats(&selected_failure_stats)
        && selected_failure_stats.request_count == 5
        && selected_failure_stats.matched_request_count == 3
        && selected_failure_stats.served_request_count == 3
        && selected_failure_stats.conflicting_request_count == 1
        && selected_failure_stats.invalid_route_request_count == 1
        && selected_failure_stats.claimed_record_count == 3
        && !fetch_trace_replay_record_was_claimed(5);
    fetch_result_destroy(&result);

    bool reusable = selected_failure;
    for (size_t attempt = 0; reusable && attempt < 2; attempt++) {
        char header[32] = {0};
        result = (FetchResult) {.budget = budget};
        reusable = fetch_request_cancelable(
                budget, "https://visual-occurrence.test/reusable",
                &changed_shape, 4096, 1000, NULL, NULL, &result)
            && result.status_code == 200
            && strcmp(result.data, "single\n") == 0
            && strcmp(result.server, "fixture-singleton") == 0
            && result.trace_delay_pumps == 7
            && fetch_response_header_value(
                   &result, "x-occurrence", header, sizeof(header))
            && strcmp(header, "singleton") == 0
            && result.set_cookie_count == 1
            && strcmp(result.set_cookies[0],
                      "mode=xxxxxxxxx; Path=/") == 0;
        fetch_result_destroy(&result);
    }
    bool retained_failure_rank = reusable;
    for (size_t attempt = 0;
         retained_failure_rank && attempt < 2; attempt++) {
        result = (FetchResult) {.budget = budget};
        retained_failure_rank = !fetch_request_cancelable(
                budget, "https://visual-occurrence.test/captured",
                &changed_shape, 4096, 1000, NULL, NULL, &result)
            && strcmp(result.error, "captured HTTP failure") == 0
            && result.status_code == 503
            && strcmp(result.server, "fixture-captured") == 0
            && result.trace_delay_pumps == 9
            && result.data == NULL && result.set_cookie_count == 0;
        fetch_result_destroy(&result);
    }
    result = (FetchResult) {.budget = budget};
    bool retained_status_zero = retained_failure_rank
        && !fetch_request_cancelable(
               budget, "https://visual-occurrence.test/captured-zero",
               &changed_shape, 4096, 1000, NULL, NULL, &result)
        && strcmp(result.error, "standalone status-zero failure") == 0
        && result.status_code == 0
        && strcmp(result.server, "fixture-zero") == 0
        && result.trace_delay_pumps == 11
        && result.data == NULL && result.set_cookie_count == 0;
    fetch_result_destroy(&result);
    result = (FetchResult) {.budget = budget};
    bool bounded_delay = retained_status_zero
        && !fetch_request_cancelable(
               budget, "https://visual-occurrence.test/delay-bound",
               &changed_shape, 4096, 1000, NULL, NULL, &result)
        && strcmp(result.error, "maximum bounded replay delay") == 0
        && result.trace_delay_pumps == 1000000
        && result.data == NULL && result.set_cookie_count == 0;
    fetch_result_destroy(&result);
    result = (FetchResult) {.budget = budget};
    bool rejected_delay_overflow = bounded_delay
        && !fetch_request_cancelable(
               budget, "https://visual-occurrence.test/delay-overflow",
               &changed_shape, 4096, 1000, NULL, NULL, &result)
        && strstr(result.error, "metadata is corrupt") != NULL
        && result.data == NULL && result.set_cookie_count == 0;
    fetch_result_destroy(&result);
    FetchTraceReplayStats occurrence_stats = {0};
    bool ranked_occurrence = rejected_delay_overflow
        && fetch_trace_replay_stats(&occurrence_stats)
        && occurrence_stats.response_keyed
        && occurrence_stats.record_count == 11
        && occurrence_stats.claimed_record_count == 7
        && occurrence_stats.request_count == 12
        && occurrence_stats.matched_request_count == 9
        && occurrence_stats.served_request_count == 5
        && occurrence_stats.rejected_request_count == 4
        && occurrence_stats.unmatched_request_count == 0
        && occurrence_stats.conflicting_request_count == 1
        && occurrence_stats.invalid_route_request_count == 2
        && occurrence_stats.request_shape_mismatch_count == 9
        && occurrence_stats.occurrence_claim_count == 3
        && occurrence_stats.reusable_claim_count == 6
        && occurrence_stats.occurrence_exhausted_count == 1
        && occurrence_stats.request_count
             == occurrence_stats.matched_request_count
                  + occurrence_stats.unmatched_request_count
                  + occurrence_stats.conflicting_request_count
                  + occurrence_stats.invalid_route_request_count
        && fetch_trace_replay_record_was_claimed(0)
        && fetch_trace_replay_record_was_claimed(1)
        && fetch_trace_replay_record_was_claimed(2)
        && !fetch_trace_replay_record_was_claimed(3)
        && !fetch_trace_replay_record_was_claimed(4)
        && fetch_trace_replay_record_was_claimed(5)
        && fetch_trace_replay_record_was_claimed(6)
        && !fetch_trace_replay_record_was_claimed(7)
        && fetch_trace_replay_record_was_claimed(8)
        && fetch_trace_replay_record_was_claimed(9)
        && !fetch_trace_replay_record_was_claimed(10);
    fetch_trace_end();

    memset(error, 0, sizeof(error));
    bool failure_ready = fetch_trace_replay_begin_response_keyed(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-response-key-failure",
        error, sizeof(error));
    result = (FetchResult) {.budget = budget};
    bool retained_failure = failure_ready
        && !fetch_request_cancelable(
               budget, "https://visual-failure.test/resource", NULL,
               4096, 1000, NULL, NULL, &result)
        && strcmp(result.error, "fixture retained failure") == 0
        && result.status_code == 503
        && strcmp(result.server, "fixture") == 0
        && result.trace_delay_pumps == 3 && result.data == NULL
        && result.set_cookie_count == 0;
    FetchTraceReplayStats failure_stats = {0};
    retained_failure = retained_failure
        && fetch_trace_replay_stats(&failure_stats)
        && failure_stats.response_keyed
        && failure_stats.request_count == 1
        && failure_stats.matched_request_count == 1
        && failure_stats.served_request_count == 0
        && failure_stats.rejected_request_count == 1
        && failure_stats.unmatched_request_count == 0
        && failure_stats.conflicting_request_count == 0
        && failure_stats.invalid_route_request_count == 0
        && failure_stats.claimed_record_count == 1
        && failure_stats.occurrence_claim_count == 0
        && failure_stats.reusable_claim_count == 1
        && failure_stats.occurrence_exhausted_count == 0;
    fetch_result_destroy(&result);
    fetch_trace_end();
    return strict_rejected && ledger && ranked_occurrence && retained_failure
        && budget->current == baseline;
}

static bool test_streaming_navigation_lifecycle(Budget *budget)
{
    char error[256] = {0};
    if (!fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
            error, sizeof(error))) return false;
    NavigationSession navigation;
    BrowserSession browser;
    static const unsigned char css[] = "#before{display:grid}";
    static const unsigned char script[] =
        "globalThis.externalBlockingOk=document.getElementById('after')===null"
        "&&document.documentElement!==null&&document.head!==null"
        "&&document.documentElement.tagName==='HTML'"
        "&&document.head.tagName==='HEAD'"
        "&&getComputedStyle(document.getElementById('before')).display==='grid'";
    bool browser_ready = browser_session_init(&browser, budget, 64 * 1024)
        && test_cache_put_stylesheet(
            &browser, "https://stream.test/theme.css", css,
            sizeof(css) - 1, NULL);
    unsigned char *script_copy = browser_ready
        ? budget_malloc(budget, sizeof(script) - 1) : NULL;
    if (script_copy != NULL) memcpy(script_copy, script, sizeof(script) - 1);
    BrowserSharedBody *script_body = script_copy == NULL ? NULL
        : browser_shared_body_take(budget, script_copy, sizeof(script) - 1);
    TilefinchRequestContext script_context = {
        .target_url = "https://stream.test/block.js",
        .initiator_url = "https://stream.test/document",
        .top_level_url = "https://stream.test/document",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    TilefinchResourceGrant script_grant = {
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true
    };
    uint64_t script_cache_now = tilefinch_platform_monotonic_time_ns();
    browser_ready = browser_ready && script_body != NULL
        && browser_session_cache_put_http_shared_classic_script(
            &browser, script_context.target_url, script_body,
            NULL, NULL, "text/javascript", "max-age=60", NULL,
            script_cache_now,
            &script_context, &script_grant);
    browser_shared_body_release(script_body);
    const BrowserCacheEntry *seeded_script = NULL;
    browser_ready = browser_ready
        && browser_session_cache_match_classic_script(
               &browser, script_context.target_url, &script_context,
               script_cache_now + 1,
               &seeded_script) == BROWSER_CACHE_FRESH
        && seeded_script != NULL;
    bool initialized = browser_ready
        && navigation_init(&navigation, budget, 2);
    if (initialized) {
        navigation_attach_browser_session(&navigation, &browser);
        /* The standards bootstrap now crosses the 512 KiB admission reserve
           at this fixture's old 2 MiB ceiling. A measured 3 MiB test ceiling
           admits both blocking scripts and remains below the 4 MiB browser
           default; this does not alter a production profile. */
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
        navigation_enable_document_scripts(&navigation, 4, 32 * 1024,
                                           16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = initialized ? navigation_begin(&navigation) : 0;
    bool loaded = initialized && navigation_load_url(
        &navigation, generation, "https://stream.test/document", 4096,
        1000, 480, NULL, NULL, true);
    bool ok = loaded && navigation.page.loaded
        && navigation.last_stream.bytes_received == 383
        && navigation.last_stream.peak_buffered_bytes <= 16384
        && navigation.script_parser_blocking == 2
        && navigation.script_loaded == 1
        && navigation.performance.blocking_stylesheet_builds == 1
        && navigation.performance.blocking_stylesheet_reuses == 1
        && navigation.performance.blocking_stylesheet_adoptions == 1
        && navigation.performance.blocking_stylesheet_final_reuses == 1
        && navigation.page.external_stylesheets.loaded == 1
        && navigation.performance.first_dom_us > 0
        /* Final-layout readiness is now observable even when no progressive
           compositor is installed; paint remains absent in that case. */
        && navigation.performance.first_layout_us > 0
        && navigation.performance.first_paint_us == 0
        && navigation.performance.partial_layouts == 0
        && navigation.performance.partial_paints == 0
        && navigation.performance.max_slice_us > 0
        && navigation.performance.max_slice_phase < NAVIGATION_SLICE_COUNT
        && navigation.performance.slices[NAVIGATION_SLICE_NETWORK].slices >= 1
        && navigation.performance.slices[NAVIGATION_SLICE_PARSER].slices >= 1
        && navigation.performance.slices[NAVIGATION_SLICE_STYLE].slices >= 1
        && navigation.performance.slices[NAVIGATION_SLICE_LAYOUT].slices >= 1
        && navigation.performance.slices[NAVIGATION_SLICE_PAINT].slices == 0
        && navigation.page.script_result.dom_content_loaded_dispatched
        && strcmp(navigation.page.script_result.summary,
                  "stream-blocking:ok") == 0;
    ScriptResult final_style_probe = {0};
    ok = ok && script_runtime_evaluate_diagnostic(
            navigation.page.runtime,
            "globalThis.pocSummary=getComputedStyle("
            "document.getElementById('before')).display",
            "<final-stylesheet-ownership>", &final_style_probe)
        && strcmp(final_style_probe.summary, "grid") == 0;
    if (ok) {
        fetch_trace_end();
        ok = fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
            error, sizeof(error));
        fetch_inject_failure_once(FETCH_INJECT_TLS);
        size_t history_before = navigation.history_count;
        uint64_t failed_generation = navigation_begin(&navigation);
        bool failed_load = navigation_load_url(
            &navigation, failed_generation,
            "https://stream.test/document", 4096, 1000, 480,
            NULL, NULL, true);
        ok = ok && !failed_load
            && strstr(navigation.last_error, "TLS failure") != NULL
            && navigation.page.loaded
            && navigation.history_count == history_before
            && strcmp(navigation.page.document.title,
                      "Stream & split") == 0;
    }
    if (ok) {
        fetch_trace_end();
        ok = fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
            error, sizeof(error));
        navigation_set_stream_delivery(&navigation, 7, 0, 0, 0, 1, 0);
        size_t history_before = navigation.history_count;
        uint64_t cancelled_generation = navigation_begin(&navigation);
        bool cancelled_load = navigation_load_url(
            &navigation, cancelled_generation,
            "https://stream.test/document", 4096, 1000, 480,
            NULL, NULL, true);
        ok = ok && !cancelled_load && navigation.last_stream.cancelled
            && navigation.last_stream.headers_delivered
            && navigation.last_stream.bytes_received == 1
            && navigation.page.loaded
            && navigation.history_count == history_before
            && strcmp(navigation.page.document.title,
                      "Stream & split") == 0;
    }
    if (ok) {
        fetch_trace_end();
        ok = fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
            error, sizeof(error));
        navigation_set_stream_delivery(&navigation, 7, 0, 0, 2, 200, 0);
        size_t history_before = navigation.history_count;
        uint64_t cancelled_generation = navigation_begin(&navigation);
        bool cancelled_load = navigation_load_url(
            &navigation, cancelled_generation,
            "https://stream.test/document", 4096, 1000, 480,
            NULL, NULL, true);
        ok = ok && !cancelled_load && navigation.last_stream.cancelled
            && navigation.last_stream.bytes_received == 200
            && navigation.page.loaded
            && navigation.history_count == history_before
            && strcmp(navigation.page.document.title,
                      "Stream & split") == 0
            && strcmp(navigation.page.script_result.summary,
                      "stream-blocking:ok") == 0;
    }
    if (ok) {
        fetch_trace_end();
        ok = fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
            error, sizeof(error));
        navigation_set_stream_delivery(&navigation, 7, 0, 0, 0, 0, 211);
        size_t history_before = navigation.history_count;
        uint64_t truncated_generation = navigation_begin(&navigation);
        bool truncated_load = navigation_load_url(
            &navigation, truncated_generation,
            "https://stream.test/document", 4096, 1000, 480,
            NULL, NULL, true);
        ok = ok && !truncated_load && navigation.last_stream.truncated
            && navigation.last_stream.bytes_received == 211
            && navigation.page.loaded
            && navigation.history_count == history_before
            && strcmp(navigation.page.document.title,
                      "Stream & split") == 0;
    }
    if (ok) {
        fetch_trace_end();
        ok = fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
            error, sizeof(error));
        navigation_set_stream_delivery(&navigation, 64, 0, 0, 0, 0, 382);
        size_t history_before = navigation.history_count;
        uint64_t truncated_generation = navigation_begin(&navigation);
        bool truncated_load = navigation_load_url(
            &navigation, truncated_generation,
            "https://stream.test/document", 4096, 1000, 480,
            NULL, NULL, true);
        ok = ok && !truncated_load && navigation.last_stream.truncated
            && navigation.last_stream.bytes_received == 382
            && navigation.page.loaded
            && navigation.history_count == history_before
            && strcmp(navigation.page.document.title,
                      "Stream & split") == 0;
    }
    if (initialized) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    fetch_trace_end();
    return ok;
}

static bool test_streaming_stylesheet_checkpoint_reuse(Budget *budget)
{
    char error[256] = {0};
    if (!fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR
                "/fixtures/http-stream-style-mutation",
            error, sizeof(error))) return false;
    NavigationSession navigation;
    BrowserSession browser;
    bool browser_ready = browser_session_init(
        &browser, budget, 32u * 1024u);
    bool initialized = browser_ready
        && navigation_init(&navigation, budget, 2);
    if (initialized) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 2u * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 32u * 1024u, 16u * 1024u, 1000);
    }
    uint64_t generation = initialized ? navigation_begin(&navigation) : 0;
    bool loaded = initialized && navigation_load_url(
        &navigation, generation,
        "https://style-stream.test/document", 4096, 1000, 480,
        NULL, NULL, true);
    bool ok = loaded && navigation.page.loaded
        && navigation.script_parser_blocking == 3
        && navigation.performance.blocking_stylesheet_builds == 2
        && navigation.performance.blocking_stylesheet_reuses == 1
        && navigation.performance.blocking_stylesheet_adoptions == 0
        && navigation.performance.blocking_stylesheet_final_reuses == 0
        && strcmp(navigation.page.script_result.summary,
                  "style-checkpoint:ok") == 0;
    ScriptResult style_probe = {0};
    ok = ok && script_runtime_evaluate_diagnostic(
            navigation.page.runtime,
            "globalThis.pocSummary=getComputedStyle("
            "document.getElementById('probe')).display",
            "<style-checkpoint-probe>", &style_probe)
        && strcmp(style_probe.summary, "flex") == 0;
    if (!ok) {
        fprintf(stderr,
                "style checkpoint loaded=%d page=%d blocking=%zu "
                "builds=%zu reuses=%zu adoptions=%zu final-reuses=%zu "
                "summary=%s probe=%s probe-error=%s error=%s\n",
                loaded ? 1 : 0, navigation.page.loaded ? 1 : 0,
                navigation.script_parser_blocking,
                navigation.performance.blocking_stylesheet_builds,
                navigation.performance.blocking_stylesheet_reuses,
                navigation.performance.blocking_stylesheet_adoptions,
                navigation.performance.blocking_stylesheet_final_reuses,
                navigation.page.script_result.summary, style_probe.summary,
                style_probe.error,
                navigation.last_error);
    }
    if (initialized) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    fetch_trace_end();
    return ok;
}

static bool test_streaming_navigation_metadata(Budget *budget)
{
    char error[256] = {0};
    if (!fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream-metadata",
            error, sizeof(error))) return false;
    BrowserSession browser;
    NavigationSession navigation;
    static const unsigned char css[] = "#probe{display:grid;color:#123456}";
    bool browser_ready = browser_session_init(&browser, budget, 64 * 1024)
        && test_cache_put_stylesheet(
            &browser, "https://stream-meta.test/assets/theme.css", css,
            sizeof(css) - 1, NULL);
    bool initialized = browser_ready
        && navigation_init(&navigation, budget, 2);
    if (initialized) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_document_scripts(&navigation, 2, 32 * 1024,
                                           16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            1, 16 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_stream_delivery(&navigation, 7, 0, 0, 0, 0, 0);
    }
    uint64_t generation = initialized ? navigation_begin(&navigation) : 0;
    bool loaded = initialized && navigation_load_url(
        &navigation, generation, "https://stream-meta.test/document", 4096,
        1000, 480, NULL, NULL, true);
    lxb_dom_node_t *probe = loaded ? find_id(
        lxb_dom_interface_node(navigation.page.document.html), "probe") : NULL;
    ComputedStyle probe_style = loaded ? style_for_node(
        &navigation.page.stylesheet, probe, NULL) : (ComputedStyle) {0};
    const BrowserCacheEntry *preloaded = NULL;
    TilefinchRequestContext preload_context = {
        .target_url = "https://stream-meta.test/assets/pre.js",
        .initiator_url = "https://stream-meta.test/document",
        .top_level_url = "https://stream-meta.test/document",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    BrowserCacheStatus preload_status = loaded
        ? browser_session_cache_match_classic_script(
            &browser, "https://stream-meta.test/assets/pre.js",
            &preload_context, tilefinch_platform_monotonic_time_ns(),
            &preloaded)
        : BROWSER_CACHE_MISS;
    bool ok = loaded && navigation.last_stream.chunks_received > 1
        && navigation.page.external_stylesheets.loaded == 1
        && navigation.page.external_stylesheets.cache_hits == 1
        && strcmp(navigation.page.referrer_policy, "no-referrer") == 0
        && navigation.page.layout.viewport.declared
        && navigation.page.layout.viewport.device_width_declared
        && navigation.page.layout.viewport.css_width == 480
        && probe != NULL && probe_style.display == DISPLAY_GRID
        && probe_style.color == 0x123456
        && navigation.preloads_discovered == 1
        && navigation.preloads_launched == 1
        && navigation.preloads_completed == 1
        && navigation.preloads_failed == 0
        && preload_status == BROWSER_CACHE_FRESH
        && preloaded != NULL && preloaded->length == 27
        && preloaded->body != NULL
        && preloaded->data == preloaded->body->data
        && preloaded->body->references == 1
        && budget_usable_size(preloaded->data) > preloaded->length
        && preloaded->data[preloaded->length] == 0;
    if (!ok) {
        fprintf(stderr, "stream metadata loaded=%d chunks=%zu css=%zu/%zu "
                "referrer=%s viewport=%d/%d/%d probe=%p style=%d/%06x "
                "preload=%zu/%zu/%zu/%zu/%zu/%zu cache=%d/%zu error=%s\n",
                loaded ? 1 : 0,
                navigation.last_stream.chunks_received,
                navigation.page.external_stylesheets.loaded,
                navigation.page.external_stylesheets.cache_hits,
                navigation.page.referrer_policy,
                navigation.page.layout.viewport.declared ? 1 : 0,
                navigation.page.layout.viewport.device_width_declared ? 1 : 0,
                navigation.page.layout.viewport.css_width, (void *) probe,
                (int) probe_style.display, probe_style.color,
                navigation.preloads_discovered,
                navigation.preloads_launched,
                navigation.preloads_completed,
                navigation.preloads_cache_hits,
                navigation.preloads_failed,
                navigation.preloads_deferred, (int) preload_status,
                preloaded == NULL ? 0 : preloaded->length,
                navigation.last_error);
    }
    if (initialized) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    fetch_trace_end();
    return ok;
}

static bool test_streaming_navigation_allocation_sweep(Budget *main_budget)
{
    static const char prior[] =
        "<!doctype html><title>Prior</title><body><p>retained page</p>";
    size_t failed_loads = 0, completed_loads = 0;
    for (size_t countdown = 0; countdown < 256; countdown++) {
        Budget budget;
        budget_init(&budget, 12 * MIB);
        budget_install_lexbor(&budget);
        NavigationSession navigation;
        if (!navigation_init(&navigation, &budget, 2)) return false;
        uint64_t prior_generation = navigation_begin(&navigation);
        if (!navigation_commit_html(
                &navigation, prior_generation, "https://prior.test/", prior,
                sizeof(prior) - 1, 480, NULL, NULL, true)) return false;
        char error[256] = {0};
        if (!fetch_trace_replay_begin(
                TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
                error, sizeof(error))) return false;
        navigation_set_stream_delivery(&navigation, 7, 0, 0, 0, 0, 0);
        uint64_t generation = navigation_begin(&navigation);
        budget_inject_failure_after(&budget, countdown);
        bool loaded = navigation_load_url(
            &navigation, generation, "https://stream.test/document", 4096,
            1000, 480, NULL, NULL, true);
        budget_clear_failure_injection(&budget);
        bool valid = navigation.history_count >= 1
            && navigation.history_count <= 2
            && (!navigation.page.loaded
                || (navigation.page.document.html != NULL
                    && (strcmp(navigation.page.document.title, "Prior") == 0
                        || strcmp(navigation.page.document.title,
                                  "Stream & split") == 0)));
        if (loaded) completed_loads++;
        else failed_loads++;
        navigation_destroy(&navigation);
        fetch_trace_end();
        if (!valid || budget.current != 0) {
            fprintf(stderr,
                    "stream allocation sweep failed countdown=%zu loaded=%d "
                    "valid=%d current=%zu\n", countdown, loaded ? 1 : 0,
                    valid ? 1 : 0, budget.current);
            budget_install_lexbor(main_budget);
            return false;
        }
    }
    budget_install_lexbor(main_budget);
    return failed_loads != 0 && completed_loads != 0;
}

static bool test_image_node_cap_ownership(Budget *budget)
{
    char html[8192];
    size_t used = (size_t) snprintf(html, sizeof(html),
                                   "<!doctype html><body>");
    for (size_t i = 0; i < 128 && used < sizeof(html); i++) {
        int written = snprintf(html + used, sizeof(html) - used,
                               "<img src=/a.svg>");
        if (written < 0 || (size_t) written >= sizeof(html) - used) {
            return false;
        }
        used += (size_t) written;
    }
    int written = snprintf(html + used, sizeof(html) - used,
                           "<img src=/b.svg>");
    if (written < 0 || (size_t) written >= sizeof(html) - used) return false;
    used += (size_t) written;

    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    char error[256] = {0};
    bool ok = document_parse(&document, budget, html, used, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-image-cap",
            error, sizeof(error))
        && images_load_external(
            &document, &stylesheet, &images, budget,
            "https://image-cap.test/", "https://image-cap.test/", NULL,
            2, 4096, 2048, 1024,
            1000, NULL, NULL)
        && images.count == 128 && images.stats.loaded == 1
        && images.stats.skipped_limit >= 1;
    fetch_trace_end();
    images_destroy(&images);
    /* A request for more images than the bounded node table can track must
       degrade to a soft clamp, not escalate into a navigation failure: a
       reference-scoring profile raises the count to fit a busy page (e.g.
       old.reddit) and every over-ceiling request should still load what
       fits.  Before the fix, any maximum_count > 64 returned false here. */
    ImageResources over_budget = {0};
    ok = ok
        && fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-image-cap",
            error, sizeof(error))
        && images_load_external(
            &document, &stylesheet, &over_budget, budget,
            "https://image-cap.test/", "https://image-cap.test/", NULL,
            200, 4096, 2048, 1024,
            1000, NULL, NULL)
        && over_budget.count == 128 && over_budget.stats.loaded == 1
        && over_budget.stats.skipped_limit >= 1;
    fetch_trace_end();
    images_destroy(&over_budget);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static bool test_image_refresh_transfers_shared_ownership(Budget *budget)
{
    static const char html[] =
        "<!doctype html><body>"
        "<img id=refresh src='data:image/png;base64,AA=='>"
        "<img id=alias></body>";
    size_t baseline = budget->current;
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    bool ok = document_parse(
            &document, budget, html, sizeof(html) - 1, 19)
        && stylesheet_build(&stylesheet, budget, &document, 480);
    lxb_dom_node_t *root = ok
        ? lxb_dom_interface_node(document.html) : NULL;
    lxb_dom_node_t *refresh = find_id(root, "refresh");
    lxb_dom_node_t *alias = find_id(root, "alias");
    images.budget = budget;
    images.items = budget_malloc(
        budget, 2 * sizeof(*images.items));
    if (images.items != NULL) {
        images.capacity = 2;
        images.count = 2;
    }
    unsigned char *encoded = budget_malloc(budget, 16);
    bool encoded_adopted = false;
    if (images.items != NULL) {
        memset(images.items, 0, 2 * sizeof(*images.items));
    }
    if (encoded != NULL) memset(encoded, 0x5a, 16);
    if (!ok || refresh == NULL || alias == NULL
        || images.items == NULL || encoded == NULL) {
        ok = false;
    } else {
        images.items[0] = (ImageResource) {
            .node = refresh, .encoded = encoded, .encoded_length = 16,
            .owns_encoded = true
        };
        encoded_adopted = true;
        images.items[1] = (ImageResource) {
            .node = alias, .encoded = encoded, .encoded_length = 16
        };
        images.stats.encoded_bytes = 16;
        lxb_dom_node_t *nodes[] = {refresh};
        ok = images_refresh_external_nodes(
                &document, &stylesheet, &images, nodes, 1, budget,
                "https://example.test/", "https://example.test/", NULL,
                8, 1024, 512, 1024, 1000, NULL, NULL)
            && images.count == 1 && images.items[0].node == alias
            && images.items[0].encoded == encoded
            && images.items[0].owns_encoded
            && images.stats.encoded_bytes == 16
            && images.stats.unsupported >= 1;
    }
    if (!encoded_adopted) budget_free(budget, encoded);
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == baseline;
}

static bool test_decoded_media_surface_ownership(Budget *budget)
{
    static const char html[] =
        "<!doctype html><body><video id=player width=4 height=3></video>";
    size_t baseline = budget->current;
    PocDocument document = {0};
    ImageResources images = {0};
    bool ok = document_parse(
        &document, budget, html, sizeof(html) - 1, 17);
    lxb_dom_node_t *video = ok ? find_id(
        lxb_dom_interface_node(document.html), "player") : NULL;
    unsigned char *surface = ok ? budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 4u * 3u * 4u) : NULL;
    if (surface != NULL) memset(surface, 0x5a, 4u * 3u * 4u);
    ok = ok && video != NULL && surface != NULL
        && images_adopt_decoded_surface(
            &images, budget, video, surface, 4, 3);
    const ImageResource *resource = ok
        ? images_find_node(&images, video) : NULL;
    ok = ok && resource != NULL && resource->pixels == surface
        && resource->width == 4 && resource->height == 3
        && resource->owns_pixels && images.stats.loaded == 1
        && images.stats.decoded_bytes == 4u * 3u * 4u;
    unsigned char *duplicate = ok ? budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 4u * 3u * 4u) : NULL;
    ok = ok && duplicate != NULL
        && !images_adopt_decoded_surface(
            &images, budget, video, duplicate, 4, 3)
        && images.count == 1;
    budget_free(budget, duplicate);
    images_destroy(&images);
    document_destroy(&document);
    return ok && budget->current == baseline;
}

static bool test_decoded_media_surface_replaces_poster(Budget *budget)
{
    static const char html[] =
        "<!doctype html><body><video id=player poster=x.png></video>"
        "<img id=alias src=x.png>";
    size_t baseline = budget->current;
    PocDocument document = {0};
    ImageResources images = {.budget = budget};
    bool ok = document_parse(
        &document, budget, html, sizeof(html) - 1, 17);
    lxb_dom_node_t *root = ok
        ? lxb_dom_interface_node(document.html) : NULL;
    lxb_dom_node_t *video = ok ? find_id(root, "player") : NULL;
    lxb_dom_node_t *alias_node = ok ? find_id(root, "alias") : NULL;
    unsigned char *poster = ok ? budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 2u * 2u * 4u) : NULL;
    unsigned char *surface = ok ? budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 4u * 3u * 4u) : NULL;
    if (poster != NULL) memset(poster, 0x33, 2u * 2u * 4u);
    if (surface != NULL) memset(surface, 0x77, 4u * 3u * 4u);
    if (ok && poster != NULL && surface != NULL) {
        images.items = budget_calloc(budget, 2, sizeof(*images.items));
        ok = images.items != NULL;
    }
    if (ok) {
        images.capacity = images.count = 2;
        images.items[0] = (ImageResource) {
            .node = video, .pixels = poster, .width = 2, .height = 2,
            .source_width = 2, .source_height = 2, .owns_pixels = true
        };
        images.items[1] = images.items[0];
        images.items[1].node = alias_node;
        images.items[1].owns_pixels = false;
        images.stats.loaded = 2;
        images.stats.decoded_bytes = 2u * 2u * 4u;
        ok = images_replace_with_decoded_surface(
            &images, budget, video, surface, 4, 3);
    }
    const ImageResource *video_image =
        ok ? images_find_node(&images, video) : NULL;
    const ImageResource *alias_image =
        ok ? images_find_node(&images, alias_node) : NULL;
    ok = ok && video_image != NULL && alias_image != NULL
        && video_image->pixels == surface && video_image->owns_pixels
        && video_image->width == 4 && video_image->height == 3
        && alias_image->pixels == poster && alias_image->owns_pixels
        && images.stats.decoded_bytes == 2u * 2u * 4u + 4u * 3u * 4u;
    if (!ok && surface != NULL
        && (video_image == NULL || video_image->pixels != surface)) {
        budget_free(budget, surface);
    }
    images_destroy(&images);
    document_destroy(&document);
    return ok && budget->current == baseline;
}

static bool test_mutable_surface_cache_invalidation(Budget *budget)
{
    size_t baseline = budget->current;
    int target_identity = 1;
    int other_identity = 2;
    TileCache cache = {.budget = budget};
    unsigned char *decoded_target = budget_malloc(budget, 16);
    unsigned char *decoded_other = budget_malloc(budget, 16);
    unsigned char *scaled_target = budget_malloc(budget, 16);
    unsigned char *scaled_other = budget_malloc(budget, 16);
    bool ok = decoded_target != NULL && decoded_other != NULL
        && scaled_target != NULL && scaled_other != NULL;
    if (ok) {
        cache.decoded_images[0] = (DecodedImageCacheEntry) {
            .identity = &target_identity, .pixels = decoded_target,
            .bytes = budget_usable_size(decoded_target), .valid = true
        };
        cache.decoded_images[1] = (DecodedImageCacheEntry) {
            .identity = &other_identity, .pixels = decoded_other,
            .bytes = budget_usable_size(decoded_other), .valid = true
        };
        cache.scaled_images[0] = (ScaledImageCacheEntry) {
            .identity = &target_identity, .pixels = scaled_target,
            .bytes = budget_usable_size(scaled_target), .valid = true
        };
        cache.scaled_images[1] = (ScaledImageCacheEntry) {
            .identity = &other_identity, .pixels = scaled_other,
            .bytes = budget_usable_size(scaled_other), .valid = true
        };
        cache.decoded_image_cache_bytes =
            cache.decoded_images[0].bytes + cache.decoded_images[1].bytes;
        cache.scaled_image_cache_bytes =
            cache.scaled_images[0].bytes + cache.scaled_images[1].bytes;
        tile_cache_invalidate_image_identity(&cache, &target_identity);
        ok = !cache.decoded_images[0].valid
            && cache.decoded_images[1].valid
            && !cache.scaled_images[0].valid
            && cache.scaled_images[1].valid
            && cache.decoded_image_evictions == 1
            && cache.scaled_image_evictions == 1;
    } else {
        budget_free(budget, decoded_target);
        budget_free(budget, decoded_other);
        budget_free(budget, scaled_target);
        budget_free(budget, scaled_other);
        memset(&cache, 0, sizeof(cache));
    }
    tile_cache_destroy(&cache);
    return ok && budget->current == baseline;
}

static bool test_picture_source_media_selection(Budget *budget)
{
    static const char picture_html[] =
        "<!doctype html><body><picture>"
        "<source media='(min-width: 500px)' srcset='/b.svg'>"
        "<img id=picture src='/a.svg' width=25 height=25>"
        "</picture></body>";
    static const char sizes_html[] =
        "<!doctype html><body><picture>"
        "<source srcset='/a.svg 320w, /b.svg 640w' "
        "sizes='(max-width: 500px) 200px, 600px'>"
        "<img id=picture src='/b.svg' width=25 height=25>"
        "</picture></body>";
    static const char density_html[] =
        "<!doctype html><body><img id=picture src='/b.svg' "
        "srcset='/a.svg 1x, /b.svg 2x' width=25 height=25></body>";
    static const char implicit_density_html[] =
        "<!doctype html><body><img id=picture src='/a.svg' "
        "srcset='/b.svg 2x' width=25 height=25></body>";
    static const struct {
        const char *html;
        size_t length;
        int width;
        unsigned char red;
        const char *selected;
    } cases[] = {
        {picture_html, sizeof(picture_html) - 1, 480, 0x12, "/a.svg"},
        {picture_html, sizeof(picture_html) - 1, 640, 0x65, "/b.svg"},
        {sizes_html, sizeof(sizes_html) - 1, 480, 0x12, "/a.svg"},
        {sizes_html, sizeof(sizes_html) - 1, 640, 0x65, "/b.svg"},
        {density_html, sizeof(density_html) - 1, 480, 0x12, "/a.svg"},
        {implicit_density_html, sizeof(implicit_density_html) - 1, 480,
         0x12, "/a.svg"}
    };
    char error[256] = {0};
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(cases) / sizeof(cases[0]); i++) {
        PocDocument document = {0};
        Stylesheet stylesheet = {0};
        ImageResources images = {0};
        ok = document_parse(
                &document, budget, cases[i].html, cases[i].length, 17)
            && stylesheet_build(
                &stylesheet, budget, &document, cases[i].width)
            && fetch_trace_replay_begin(
                TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-image-cap",
                error, sizeof(error))
            && images_load_external(
                &document, &stylesheet, &images, budget,
                "https://image-cap.test/", "https://image-cap.test/", NULL,
                2, 4096, 2048, 4096, 1000, NULL, NULL);
        lxb_dom_node_t *picture = ok ? find_id(
            lxb_dom_interface_node(document.html), "picture") : NULL;
        const ImageResource *resource = ok
            ? images_find_node(&images, picture) : NULL;
        size_t selected_length = 0;
        const char *selected = ok ? image_select_source(
            &stylesheet, picture, &selected_length) : NULL;
        bool case_ok = ok && resource != NULL
            && image_resource_available(resource)
            && resource->pixels != NULL
            && resource->pixels[0] == cases[i].red
            && resource->width == 1 && resource->height == 1
            && images.stats.loaded == 1 && selected != NULL
            && selected_length == strlen(cases[i].selected)
            && memcmp(selected, cases[i].selected, selected_length) == 0;
        if (!case_ok) {
            fprintf(stderr,
                    "responsive image case %zu width=%d selected=%.*s "
                    "expected=%s loaded=%zu pixel=%u error=%s\n",
                    i, cases[i].width, (int) selected_length,
                    selected == NULL ? "" : selected, cases[i].selected,
                    images.stats.loaded,
                    resource != NULL && resource->pixels != NULL
                        ? resource->pixels[0] : 0, error);
        }
        ok = case_ok;
        fetch_trace_end();
        images_destroy(&images);
        stylesheet_destroy(&stylesheet);
        document_destroy(&document);
        ok = ok && budget->current == 0;
    }
    static const char dynamic_html[] =
        "<!doctype html><body><picture><source id=source "
        "srcset='data:,a'><img id=dynamic src='data:,b' "
        "srcset='/small.svg 100w, /large.svg 400w' "
        "sizes='calc(25vw + 10px)'></picture></body>";
    PocDocument dynamic_document = {0};
    Stylesheet dynamic_sheet = {0};
    ok = ok && document_parse(
        &dynamic_document, budget, dynamic_html,
        sizeof(dynamic_html) - 1, 18)
        && stylesheet_build(
            &dynamic_sheet, budget, &dynamic_document, 480);
    lxb_dom_node_t *dynamic_image = ok ? find_id(
        lxb_dom_interface_node(dynamic_document.html), "dynamic") : NULL;
    lxb_dom_node_t *dynamic_source = ok ? find_id(
        lxb_dom_interface_node(dynamic_document.html), "source") : NULL;
    size_t selected_length = 0;
    const char *selected = ok ? image_select_source(
        &dynamic_sheet, dynamic_image, &selected_length) : NULL;
    ok = ok && selected != NULL && selected_length == 7
        && memcmp(selected, "data:,a", 7) == 0
        && lxb_dom_element_set_attribute(
            lxb_dom_interface_element(dynamic_source),
            (const lxb_char_t *) "media", 5,
            (const lxb_char_t *) "not all", 7) != NULL;
    selected = ok ? image_select_source(
        &dynamic_sheet, dynamic_image, &selected_length) : NULL;
    /* calc(25vw + 10px) is 130 CSS px at 480px, so 400w is the first
       density at or above the PSP's 1x device scale. */
    ok = ok && selected != NULL && selected_length == 10
        && memcmp(selected, "/large.svg", 10) == 0;
    stylesheet_destroy(&dynamic_sheet);
    document_destroy(&dynamic_document);
    ok = ok && budget->current == 0;
    return ok;
}

static bool test_deep_document_resource_walkers(Budget *budget)
{
    static const char html[] =
        "<!doctype html><title>Deep traversal</title><body></body>";
    static const lxb_char_t div_tag[] = "div";
    static const lxb_char_t meta_tag[] = "meta";
    static const lxb_char_t text[] = "deep body sentinel";
    static const lxb_char_t name_attr[] = "name";
    static const lxb_char_t viewport_value[] = "viewport";
    static const lxb_char_t content_attr[] = "content";
    static const lxb_char_t device_width_value[] = "width=device-width";
    enum { DEEP_WALK_DEPTH = 2048 };

    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    size_t baseline = budget->current;
    bool ok = document_parse(
        &document, budget, html, sizeof(html) - 1, 17);
    BudgetAllocationOwner previous = {0};
    if (ok) previous = document_allocation_owner_enter(&document);
    lxb_dom_node_t *cursor = ok ? document_body_node(&document) : NULL;
    for (size_t depth = 0; ok && depth < DEEP_WALK_DEPTH; depth++) {
        lxb_dom_element_t *element = lxb_dom_document_create_element(
            &document.html->dom_document, div_tag,
            sizeof(div_tag) - 1, NULL);
        ok = element != NULL
            && lxb_dom_node_append_child(
                   cursor, lxb_dom_interface_node(element))
                   == LXB_DOM_EXCEPTION_OK;
        /* A side branch every four levels forces the image pass to retain a
           few hundred inherited-style continuation records while the main
           chain remains deep enough to expose a recursive implementation. */
        if (ok && depth % 4u == 0) {
            lxb_dom_element_t *side = lxb_dom_document_create_element(
                &document.html->dom_document, div_tag,
                sizeof(div_tag) - 1, NULL);
            ok = side != NULL
                && lxb_dom_node_append_child(
                       cursor, lxb_dom_interface_node(side))
                       == LXB_DOM_EXCEPTION_OK;
        }
        if (ok) cursor = lxb_dom_interface_node(element);
    }
    lxb_dom_text_t *text_node = ok
        ? lxb_dom_document_create_text_node(
              &document.html->dom_document, text, sizeof(text) - 1)
        : NULL;
    lxb_dom_element_t *meta = ok
        ? lxb_dom_document_create_element(
              &document.html->dom_document, meta_tag,
              sizeof(meta_tag) - 1, NULL)
        : NULL;
    ok = ok && text_node != NULL && meta != NULL
        && lxb_dom_node_append_child(
               cursor, lxb_dom_interface_node(text_node))
               == LXB_DOM_EXCEPTION_OK
        && lxb_dom_element_set_attribute(
               meta, name_attr, sizeof(name_attr) - 1,
               viewport_value, sizeof(viewport_value) - 1) != NULL
        && lxb_dom_element_set_attribute(
               meta, content_attr, sizeof(content_attr) - 1,
               device_width_value, sizeof(device_width_value) - 1) != NULL
        && lxb_dom_node_append_child(
               cursor, lxb_dom_interface_node(meta))
               == LXB_DOM_EXCEPTION_OK;
    if (document.html != NULL) {
        document_allocation_owner_leave(&document, previous);
    }

    MobileViewport viewport = {0};
    ok = ok && document_refresh(&document)
        && document.node_count > DEEP_WALK_DEPTH
        && document_body_text(&document) != NULL
        && strstr(document_body_text(&document),
                  (const char *) text) != NULL
        && document_mobile_viewport(&document, 480, 980, &viewport)
        && viewport.declared && viewport.device_width
        && viewport.layout_width == 480
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && images_load_external(
            &document, &stylesheet, &images, budget,
            "https://deep-walk.test/", "https://deep-walk.test/", NULL,
            1, 4096, 4096, 4096, 1000, NULL, NULL)
        && images.stats.discovered == 0;
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == baseline;
}

static bool test_inline_svg_resource(Budget *budget)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;padding:0}"
        "#icon{color:#ff0000}</style><body>"
        "<svg id=icon width=16 height=16 viewBox='0 0 16 16' "
        "color='#ff0000' xmlns='http://www.w3.org/2000/svg'>"
        "<path fill='currentColor' d='M0 0h16v16H0z'/></svg></body>";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    LayoutDocument layout = {0};
    bool ok = document_parse(&document, budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && images_load_external(
            &document, &stylesheet, &images, budget,
            "https://inline-svg.test/", "https://inline-svg.test/", NULL,
            8, 64 * 1024, 32 * 1024, 64 * 1024, 1000, NULL, NULL);
    lxb_dom_node_t *icon = ok ? find_id(
        lxb_dom_interface_node(document.html), "icon") : NULL;
    const ImageResource *resource = ok ? images_find_node(&images, icon) : NULL;
    ok = ok && icon != NULL && resource != NULL
        && image_resource_available(resource)
        && resource->width == 16 && resource->height == 16
        && resource->pixels != NULL && images.stats.loaded == 1
        && images.stats.decoded_bytes == 16u * 16u * 4u
        && resource->pixels[0] > 240 && resource->pixels[1] < 16
        && resource->pixels[2] < 16 && resource->pixels[3] > 240
        && layout_build(&layout, budget, &document, &stylesheet,
                        NULL, &images, 480);
    bool draw_seen = false;
    if (ok) {
        for (size_t i = 0; i < layout.count; i++) {
            if (layout.commands[i].type == DRAW_IMAGE
                && layout.commands[i].image == resource
                && layout.commands[i].width == 16
                && layout.commands[i].height == 16) {
                draw_seen = true;
                break;
            }
        }
    }
    layout_destroy(&layout);
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && draw_seen && budget->current == 0;
}

static bool test_visible_inline_svg_priority_resource(Budget *budget)
{
    static const char html[] =
        "<!doctype html><style>"
        ".compact{display:none}@media(max-width:600px){"
        ".compact{display:flex}}"
        "</style><body><nav><span class=compact>"
        "<svg id=icon width=17 height=48 viewBox='0 0 17 48' "
        "xmlns='http://www.w3.org/2000/svg'>"
        "<path fill='#111111' d='M1 1h15v46H1z'/></svg>"
        "</span></nav></body>";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    bool ok = document_parse(&document, budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480);
    lxb_dom_node_t *icon = ok ? find_id(
        lxb_dom_interface_node(document.html), "icon") : NULL;
    ImagePriorityTarget target = {
        .node = icon,
        .kind = IMAGE_PRIORITY_KIND_DOCUMENT,
        .pseudo = (uint8_t) PSEUDO_NONE
    };
    ok = ok && icon != NULL
        && images_load_external_priority_targets(
            &document, &stylesheet, &images, &target, 1, budget,
            "https://visible-svg.test/", "https://visible-svg.test/", NULL,
            4, 64 * 1024, 32 * 1024, 128 * 1024, 1000, NULL, NULL);
    const ImageResource *resource = ok
        ? images_find_node(&images, icon) : NULL;
    ok = ok && resource != NULL && image_resource_available(resource)
        && resource->width == 17 && resource->height == 48
        && images.stats.loaded == 1;
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static bool test_bounded_large_data_svg_resource(Budget *budget)
{
    static const char prefix[] =
        "<!doctype html><style>#row{display:flex}#icon{height:24px}"
        "</style><body><div id=row><img id=icon src=\""
        "data:image/svg+xml,"
        "%3csvg%20xmlns='http://www.w3.org/2000/svg'%20width='16'"
        "%20height='16'%20viewBox='0%200%2016%2016'%3e"
        "%3cpath%20fill='%23d02040'%20d='M0%200h16v16H0z'/%3e"
        "%3c/svg%3e";
    static const char suffix[] = "\"></div></body>";
    enum { PADDING_TRIPLETS = 800 };
    size_t baseline = budget->current;
    size_t html_length = sizeof(prefix) - 1u
        + PADDING_TRIPLETS * 3u + sizeof(suffix) - 1u;
    char *html = budget_malloc(budget, html_length + 1u);
    if (html == NULL) return false;
    size_t offset = 0;
    memcpy(html + offset, prefix, sizeof(prefix) - 1u);
    offset += sizeof(prefix) - 1u;
    for (size_t i = 0; i < PADDING_TRIPLETS; i++) {
        memcpy(html + offset, "%20", 3);
        offset += 3;
    }
    memcpy(html + offset, suffix, sizeof(suffix));

    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    LayoutDocument layout = {0};
    bool ok = html_length > 2048
        && document_parse(&document, budget, html, html_length, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && images_load_external(
            &document, &stylesheet, &images, budget,
            "https://data-svg.test/", "https://data-svg.test/", NULL,
            2, 8 * 1024, 8 * 1024, 8 * 1024, 1000, NULL, NULL);
    lxb_dom_node_t *icon = ok ? find_id(
        lxb_dom_interface_node(document.html), "icon") : NULL;
    const ImageResource *resource = ok ? images_find_node(&images, icon) : NULL;
    ok = ok && layout_build(
        &layout, budget, &document, &stylesheet, NULL, &images, 480);
    const LayoutNodeBox *box = ok
        ? layout_box_for_node(&layout, icon) : NULL;
    ok = ok && resource != NULL && image_resource_available(resource)
        && resource->width == 16 && resource->height == 16
        && resource->pixels != NULL && resource->pixels[0] >= 0xcc
        && resource->pixels[0] <= 0xd4 && resource->pixels[1] >= 0x1c
        && resource->pixels[1] <= 0x24 && resource->pixels[2] >= 0x3c
        && resource->pixels[2] <= 0x44 && images.stats.loaded == 1
        && images.stats.failed == 0 && box != NULL
        && box->width == 24 && box->height == 24;
    layout_destroy(&layout);
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    budget_free(budget, html);
    return ok && budget->current == baseline;
}

static bool test_inline_svg_extreme_edge_resource(Budget *budget)
{
    /* A nearly horizontal edge produces a fixed-point slope far outside an
       int even though the SVG viewport and decoded output are tiny. The
       pinned NanoSVG guard must bound it before raster scan conversion. */
    static const char html[] =
        "<!doctype html><body>"
        "<svg id=edge width=16 height=16 viewBox='0 0 16 16' "
        "xmlns='http://www.w3.org/2000/svg'>"
        "<path fill='#123456' d='M0 0 L16 0.000000001 L0 16z'/>"
        "</svg></body>";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    bool ok = document_parse(&document, budget, html, sizeof(html) - 1, 19)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && images_load_external(
            &document, &stylesheet, &images, budget,
            "https://svg-edge.test/", "https://svg-edge.test/", NULL,
            2, 16 * 1024, 16 * 1024, 16 * 1024, 1000, NULL, NULL);
    lxb_dom_node_t *edge = ok ? find_id(
        lxb_dom_interface_node(document.html), "edge") : NULL;
    const ImageResource *resource = ok ? images_find_node(&images, edge) : NULL;
    ok = ok && edge != NULL && resource != NULL
        && image_resource_available(resource)
        && resource->width == 16 && resource->height == 16
        && images.stats.loaded == 1
        && images.stats.decoded_bytes == 16u * 16u * 4u;
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static bool test_inline_svg_presentation_resource(Budget *budget)
{
    static const char html[] =
        "<!doctype html><style>"
        "#container{color:#008000}.class>#use-rect{fill:#008000}"
        "@supports(fill:green){#supports{color:#008000}}"
        "</style><body>"
        "<div id=container><svg id=paint-svg "
        "xmlns='http://www.w3.org/2000/svg'>"
        "<rect id=paint-rect x=25 y=25 width=50 height=50 "
        "fill=currentColor stroke=currentColor stroke-width=50></rect>"
        "</svg></div>"
        "<svg id=use-svg width=100 height=100 "
        "xmlns='http://www.w3.org/2000/svg'>"
        "<use href='#tmpl'></use><defs><g id=tmpl class=class>"
        "<rect id=use-rect width=100 height=100 "
        "style=\"--label:'a;b:c'\"></rect>"
        "</g></defs></svg>"
        "<i id=supports></i></body>";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    bool ok = document_parse(
            &document, budget, html, sizeof(html) - 1, 23)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && images_load_external(
            &document, &stylesheet, &images, budget,
            "https://inline-svg-paint.test/",
            "https://inline-svg-paint.test/", NULL,
            4, 64 * 1024, 64 * 1024, 256 * 1024, 1000, NULL, NULL);
    lxb_dom_node_t *root = ok
        ? lxb_dom_interface_node(document.html) : NULL;
    lxb_dom_node_t *paint_svg = ok ? find_id(root, "paint-svg") : NULL;
    lxb_dom_node_t *paint_rect = ok ? find_id(root, "paint-rect") : NULL;
    lxb_dom_node_t *use_svg = ok ? find_id(root, "use-svg") : NULL;
    lxb_dom_node_t *use_rect = ok ? find_id(root, "use-rect") : NULL;
    lxb_dom_node_t *supports = ok ? find_id(root, "supports") : NULL;
    const ImageResource *paint = ok
        ? images_find_node(&images, paint_svg) : NULL;
    const ImageResource *use = ok
        ? images_find_node(&images, use_svg) : NULL;
    char fill[32] = {0};
    char label[32] = {0};
    StyleRetainedPresentationValues retained = {0};
    bool retained_batch = ok && style_retained_presentation_values(
        &stylesheet, use_rect, &retained);
    size_t fill_index = 0;
    while (fill_index < STYLE_RETAINED_PRESENTATION_COUNT
           && strcmp(style_retained_presentation_name(fill_index),
                     "fill") != 0) {
        fill_index++;
    }
    bool supports_query = stylesheet_supports_matches(
        &stylesheet, "(fill:green)", 12);
    bool rejects_invalid_fill = !stylesheet_supports_matches(
        &stylesheet, "(fill:not-a-paint)", 18);
    bool rejects_invalid_linecap = !stylesheet_supports_matches(
        &stylesheet, "(stroke-linecap:banana)", 23);
    ComputedStyle supports_style = ok
        ? style_for_node(&stylesheet, supports, NULL) : (ComputedStyle) {0};
    ok = ok && paint != NULL && use != NULL
        && image_resource_available(paint)
        && image_resource_available(use)
        && paint->width == 300 && paint->height == 150
        && use->width == 100 && use->height == 100
        && paint->pixels[0] < 8 && paint->pixels[1] >= 120
        && paint->pixels[1] <= 136 && paint->pixels[2] < 8
        && paint->pixels[3] > 240
        && use->pixels[0] < 8 && use->pixels[1] >= 120
        && use->pixels[1] <= 136 && use->pixels[2] < 8
        && use->pixels[3] > 240
        && !style_retained_presentation_value(
            &stylesheet, paint_rect, "stroke", 6, fill, sizeof(fill))
        && style_retained_presentation_value(
            &stylesheet, use_rect, "fill", 4, fill, sizeof(fill))
        && strcmp(fill, "#008000") == 0
        && style_custom_property_value(
            &stylesheet, use_rect, PSEUDO_NONE,
            "--label", 7, label, sizeof(label))
        && strcmp(label, "'a;b:c'") == 0
        && retained_batch
        && fill_index < STYLE_RETAINED_PRESENTATION_COUNT
        && (retained.present_mask & (UINT16_C(1) << fill_index)) != 0
        && strcmp(retained.values[fill_index], fill) == 0
        && supports_query && rejects_invalid_fill
        && rejects_invalid_linecap && supports_style.color == 0x008000
        && images.stats.loaded == 2;
    if (!ok) {
        fprintf(stderr,
                "svg-presentation paint=%p %dx%d use=%p %dx%d "
                "loaded=%zu unsupported=%zu fill=%s supports=%d/%06x\n",
                (void *) paint, paint == NULL ? 0 : paint->width,
                paint == NULL ? 0 : paint->height, (void *) use,
                use == NULL ? 0 : use->width,
                use == NULL ? 0 : use->height,
                images.stats.loaded, images.stats.unsupported, fill,
                supports_query,
                supports_style.color);
    }
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static bool test_inline_svg_symbol_use_resource(Budget *budget)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}#sprite{display:none}"
        "#href-icon{color:#12ab34}#xlink-icon{color:#3456cd}</style><body>"
        "<svg id=sprite xmlns='http://www.w3.org/2000/svg' "
        "xmlns:xlink='http://www.w3.org/1999/xlink'>"
        "<symbol id=solid viewBox='0 0 16 16'>"
        "<path fill='currentColor' d='M0 0h16v16H0z'/></symbol>"
        "<symbol id=nested viewBox='0 0 16 16'>"
        "<use href='#solid'/></symbol>"
        "<symbol id=cycle-a><use href='#cycle-b'/></symbol>"
        "<symbol id=cycle-b><use href='#cycle-a'/></symbol></svg>"
        "<svg id=href-icon width=16 height=16 "
        "xmlns='http://www.w3.org/2000/svg'><use href='#nested'/></svg>"
        "<svg id=xlink-icon width=16 height=16 "
        "xmlns='http://www.w3.org/2000/svg' "
        "xmlns:xlink='http://www.w3.org/1999/xlink'>"
        "<use xlink:href='#solid'/></svg>"
        "<svg id=missing-icon width=16 height=16><use href='#missing'/></svg>"
        "<svg id=cycle-icon width=16 height=16><use href='#cycle-a'/></svg>"
        "</body>";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    bool ok = document_parse(&document, budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && images_load_external(
            &document, &stylesheet, &images, budget,
            "https://inline-svg-use.test/",
            "https://inline-svg-use.test/", NULL,
            8, 64 * 1024, 32 * 1024, 128 * 1024, 1000, NULL, NULL);
    lxb_dom_node_t *root = ok
        ? lxb_dom_interface_node(document.html) : NULL;
    lxb_dom_node_t *href_icon = ok ? find_id(root, "href-icon") : NULL;
    lxb_dom_node_t *xlink_icon = ok ? find_id(root, "xlink-icon") : NULL;
    lxb_dom_node_t *missing_icon = ok ? find_id(root, "missing-icon") : NULL;
    lxb_dom_node_t *cycle_icon = ok ? find_id(root, "cycle-icon") : NULL;
    const ImageResource *href_resource = ok
        ? images_find_node(&images, href_icon) : NULL;
    const ImageResource *xlink_resource = ok
        ? images_find_node(&images, xlink_icon) : NULL;
    ok = ok && href_resource != NULL && xlink_resource != NULL
        && image_resource_available(href_resource)
        && image_resource_available(xlink_resource)
        && href_resource->width == 16 && href_resource->height == 16
        && xlink_resource->width == 16 && xlink_resource->height == 16
        && href_resource->pixels[0] >= 0x10
        && href_resource->pixels[0] <= 0x14
        && href_resource->pixels[1] >= 0xa8
        && href_resource->pixels[1] <= 0xae
        && href_resource->pixels[2] >= 0x31
        && href_resource->pixels[2] <= 0x37
        && href_resource->pixels[3] > 240
        && xlink_resource->pixels[0] >= 0x31
        && xlink_resource->pixels[0] <= 0x37
        && xlink_resource->pixels[1] >= 0x53
        && xlink_resource->pixels[1] <= 0x59
        && xlink_resource->pixels[2] >= 0xca
        && xlink_resource->pixels[2] <= 0xd0
        && xlink_resource->pixels[3] > 240
        && images_find_node(&images, missing_icon) == NULL
        && images_find_node(&images, cycle_icon) == NULL
        && images.stats.loaded == 2 && images.stats.unsupported >= 2
        && images.stats.decoded_bytes == 2u * 16u * 16u * 4u;
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static bool test_external_svg_symbol_use_resource(Budget *budget)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}#icon{color:#12ab34}</style>"
        "<body><svg id=icon width=16 height=16>"
        "<use href='icons.svg#solid'/></svg></body>";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    char error[256] = {0};
    bool ok = document_parse(&document, budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-svg-sprite",
            error, sizeof(error))
        && images_load_external(
            &document, &stylesheet, &images, budget,
            "https://svg-sprite.test/", "https://svg-sprite.test/", NULL,
            4, 64 * 1024, 32 * 1024, 128 * 1024, 1000, NULL, NULL);
    lxb_dom_node_t *icon = ok
        ? find_id(lxb_dom_interface_node(document.html), "icon") : NULL;
    const ImageResource *resource = ok
        ? images_find_node(&images, icon) : NULL;
    ok = ok && resource != NULL && image_resource_available(resource)
        && resource->width == 16 && resource->height == 16
        && resource->pixels[0] >= 0x10 && resource->pixels[0] <= 0x14
        && resource->pixels[1] >= 0xa8 && resource->pixels[1] <= 0xae
        && resource->pixels[2] >= 0x31 && resource->pixels[2] <= 0x37
        && resource->pixels[3] > 240 && images.stats.loaded == 1;
    fetch_trace_end();
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    if (!ok && error[0] != '\0') {
        fprintf(stderr, "external SVG sprite replay: %s\n", error);
    }
    return ok && budget->current == 0;
}

static bool test_stylesheet_shared_cache_body(Budget *budget)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href=https://fixture.test/layout.css>"
        "<link rel=stylesheet href=https://fixture.test/quota.css>"
        "<body>shared css";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    BrowserSession session = {0};
    ExternalStylesheetStats stats = {0};
    char error[256] = {0};
    bool ok = document_parse(
            &document, budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && browser_session_init(&session, budget, 64 * 1024)
        && fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-section-external-layout",
            error, sizeof(error))
        && stylesheets_load_external_tracked(
            &document, &stylesheet, budget, "https://fixture.test/page",
            1, 4096, 4096, 1000, NULL, &session, &resources, &stats)
        && stats.loaded == 1 && stats.terminal_failures == 1;
    TilefinchRequestContext cache_context = {
        .target_url = "https://fixture.test/layout.css",
        .initiator_url = "https://fixture.test/page",
        .top_level_url = "https://fixture.test/page",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    const BrowserCacheEntry *entry = NULL;
    BrowserCacheStatus cache_status = ok
        ? browser_session_cache_match_resource(
            &session, cache_context.target_url, &cache_context,
            tilefinch_platform_monotonic_time_ns(), &entry)
        : BROWSER_CACHE_MISS;
    ok = ok && cache_status != BROWSER_CACHE_MISS
        && entry != NULL && entry->body != NULL
        && entry->data == entry->body->data
        && entry->body->references == 2
        && budget_usable_size(entry->data) > entry->length
        && entry->data[entry->length] == 0;
    fetch_trace_end();
    browser_session_destroy(&session);
    stylesheet_destroy(&stylesheet);
    ok = ok && stylesheet_build(&stylesheet, budget, &document, 480)
        && stylesheets_load_external_tracked(
            &document, &stylesheet, budget, "https://fixture.test/page",
            1, 4096, 4096, 1000, NULL, NULL, &resources, &stats)
        && stats.loaded == 1 && stats.failed == 1
        && stats.retained_body_hits == 1
        && stats.retry_suppressed == 1
        && stats.terminal_failures == 1;
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static bool test_stylesheet_transient_retry_recovers(Budget *budget)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href=https://fixture.test/layout.css><body>retry css";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    BrowserSession session = {0};
    ExternalStylesheetStats stats = {0};
    char error[256] = {0};
    bool ok = document_parse(
            &document, budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && browser_session_init(&session, budget, 64 * 1024)
        && fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-section-external-layout",
            error, sizeof(error));
    if (ok) {
        fetch_inject_failure_once(FETCH_INJECT_TLS);
        ok = stylesheets_load_external_tracked(
                &document, &stylesheet, budget,
                "https://fixture.test/page", 1, 4096, 4096, 1000,
                NULL, &session, &resources, &stats)
            && stats.loaded == 0 && stats.failed == 1
            && stats.transient_failures == 1;
    }
    stylesheet_destroy(&stylesheet);
    if (ok) {
        ok = stylesheet_build(&stylesheet, budget, &document, 480)
            && stylesheets_load_external_tracked(
                &document, &stylesheet, budget,
                "https://fixture.test/page", 1, 4096, 4096, 1000,
                NULL, &session, &resources, &stats)
            && stats.loaded == 1 && stats.transient_retries == 1
            && stats.transient_failures == 1
            && stats.retry_suppressed == 0;
    }
    fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    browser_session_destroy(&session);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static bool test_stylesheet_transient_retry_cap(Budget *budget)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href=https://fixture.test/layout.css><body>retry cap";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    BrowserSession session = {0};
    ExternalStylesheetStats stats = {0};
    char error[256] = {0};
    bool ok = document_parse(
            &document, budget, html, sizeof(html) - 1, 17)
        && browser_session_init(&session, budget, 64 * 1024)
        && fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-section-external-layout",
            error, sizeof(error));
    for (size_t attempt = 0;
         ok && attempt < STYLESHEET_TRANSIENT_ATTEMPT_LIMIT; attempt++) {
        stylesheet_destroy(&stylesheet);
        ok = stylesheet_build(&stylesheet, budget, &document, 480);
        fetch_inject_failure_once(FETCH_INJECT_TLS);
        ok = ok && stylesheets_load_external_tracked(
                &document, &stylesheet, budget,
                "https://fixture.test/page", 1, 4096, 4096, 1000,
                NULL, &session, &resources, &stats)
            && stats.loaded == 0 && stats.failed == 1
            && stats.transient_failures == attempt + 1
            && stats.transient_retries == attempt;
    }
    stylesheet_destroy(&stylesheet);
    ok = ok && stylesheet_build(&stylesheet, budget, &document, 480)
        && stylesheets_load_external_tracked(
            &document, &stylesheet, budget,
            "https://fixture.test/page", 1, 4096, 4096, 1000,
            NULL, &session, &resources, &stats)
        && stats.loaded == 0 && stats.failed == 1
        && stats.transient_failures == STYLESHEET_TRANSIENT_ATTEMPT_LIMIT
        && stats.transient_retries
               == STYLESHEET_TRANSIENT_ATTEMPT_LIMIT - 1
        && stats.retry_suppressed == 1;
    stylesheet_document_resources_open_final_retry(&resources);
    stylesheet_destroy(&stylesheet);
    ok = ok && stylesheet_build(&stylesheet, budget, &document, 480)
        && stylesheets_load_external_tracked(
            &document, &stylesheet, budget,
            "https://fixture.test/page", 1, 4096, 4096, 1000,
            NULL, &session, &resources, &stats)
        && stats.loaded == 1 && stats.final_retry_grants == 1
        && stats.transient_retries == STYLESHEET_TRANSIENT_ATTEMPT_LIMIT
        && stats.retry_suppressed == 1;
    fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    browser_session_destroy(&session);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static bool test_script_shared_cache_lifetimes(Budget *budget)
{
    static const unsigned char classic[] =
        "globalThis.classicValue='classic'";
    static const unsigned char root_module[] =
        "import {dep} from './dep.js';globalThis.moduleValue=dep";
    static const unsigned char dependency[] =
        "export const dep='module'";
    static const char html[] =
        "<!doctype html><body><script src=/classic.js></script>"
        "<script>document.addEventListener('DOMContentLoaded',()=>"
        "globalThis.pocSummary=(globalThis.classicValue||'x')+':' +"
        "(globalThis.moduleValue||'x'))</script>"
        "<script type=module src=/root.js></script>";
    BrowserSession session = {0};
    NavigationSession navigation = {0};
    TilefinchRequestContext classic_context = {
        .target_url = "https://scripts.test/classic.js",
        .initiator_url = "https://scripts.test/",
        .top_level_url = "https://scripts.test/",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    TilefinchResourceGrant classic_grant = {
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true
    };
    unsigned char *classic_copy = budget_malloc(budget, sizeof(classic) - 1);
    if (classic_copy != NULL) {
        memcpy(classic_copy, classic, sizeof(classic) - 1);
    }
    BrowserSharedBody *seeded_classic_body = classic_copy == NULL ? NULL
        : browser_shared_body_take(
            budget, classic_copy, sizeof(classic) - 1);
    uint64_t cache_now = tilefinch_platform_monotonic_time_ns();
    bool session_ready = browser_session_init(&session, budget, 64 * 1024)
        && seeded_classic_body != NULL
        && browser_session_cache_put_http_shared_classic_script(
            &session, classic_context.target_url, seeded_classic_body,
            NULL, NULL, "text/javascript", "max-age=3600", NULL,
            cache_now, &classic_context, &classic_grant)
        && test_cache_put_module(
            &session, "https://scripts.test/root.js", root_module,
            sizeof(root_module) - 1, "https://scripts.test")
        && test_cache_put_module(
            &session, "https://scripts.test/dep.js", dependency,
            sizeof(dependency) - 1, "https://scripts.test");
    browser_shared_body_release(seeded_classic_body);
    const BrowserCacheEntry *classic_entry = NULL;
    if (session_ready) {
        (void) browser_session_cache_match_classic_script(
            &session, classic_context.target_url, &classic_context,
            cache_now + 1, &classic_entry);
    }
    const BrowserCacheEntry *root_entry = NULL;
    const BrowserCacheEntry *dep_entry = NULL;
    if (session_ready) {
        (void) browser_session_cache_match_module(
            &session, "https://scripts.test/root.js",
            "https://scripts.test", "https://scripts.test", false,
            TILEFINCH_CREDENTIALS_SAME_ORIGIN,
            cache_now + 1, &root_entry);
        (void) browser_session_cache_match_module(
            &session, "https://scripts.test/dep.js",
            "https://scripts.test", "https://scripts.test", false,
            TILEFINCH_CREDENTIALS_SAME_ORIGIN,
            cache_now + 1, &dep_entry);
    }
    BrowserSharedBody *classic_body = classic_entry == NULL
        ? NULL : classic_entry->body;
    BrowserSharedBody *root_body = root_entry == NULL ? NULL : root_entry->body;
    BrowserSharedBody *dep_body = dep_entry == NULL ? NULL : dep_entry->body;
    bool navigation_ready = session_ready
        && classic_body != NULL && root_body != NULL && dep_body != NULL
        && navigation_init(&navigation, budget, 2);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &session);
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 32 * 1024, 16 * 1024, 1000);
    }
    uint64_t generation = navigation_ready
        ? navigation_begin(&navigation) : 0;
    bool ok = navigation_ready && navigation_commit_html(
            &navigation, generation, "https://scripts.test/", html,
            sizeof(html) - 1, 480, NULL, NULL, true)
        && strcmp(navigation.page.script_result.summary,
                  "classic:module") == 0
        && navigation.script_cache_hits == 3
        && classic_body->references == 1
        && root_body->references == 1
        && dep_body->references == 1;
    if (navigation_ready) navigation_destroy(&navigation);
    if (session_ready) browser_session_destroy(&session);
    return ok && budget->current == 0;
}

static bool test_image_virtual_cancel_bound(Budget *budget)
{
    static const char html[] =
        "<!doctype html><body><img src=/cancel.svg>";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    char error[256] = {0};
    bool ok = document_parse(&document, budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-image-cancel",
            error, sizeof(error))
        && images_load_external(
            &document, &stylesheet, &images, budget,
            "https://image-cancel.test/", "https://image-cancel.test/", NULL,
            1, 4096, 4096, 4096,
            15000, NULL, NULL)
        && images.count == 0 && images.stats.failed == 1
        && images.stats.deadline_cancelled == 1
        /* 256 is the idle-poll ceiling; the extra one is the checkpoint
           each completed request now takes so a burst of completions cannot
           chain their decodes into one un-yielding stretch. */
        && images.stats.cooperative_yields <= 257
        && images.stats.work_units < 400;
    fetch_trace_end();
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static uint64_t test_image_progress_clock(void *opaque)
{
    uint64_t *now = opaque;
    *now += UINT64_C(250000000);
    return *now;
}

static bool test_image_request_no_progress_bound(Budget *budget)
{
    static const char html[] =
        "<!doctype html><body><img src=/cancel.svg>";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    char error[256] = {0};
    uint64_t clock = 0;
    TilefinchPlatformServices services = {
        .context = &clock,
        .monotonic_time_ns = test_image_progress_clock
    };
    bool ready = document_parse(
            &document, budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && fetch_trace_replay_begin(
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-image-cancel",
            error, sizeof(error));
    if (ready) tilefinch_platform_set_services(&services);
    bool loaded = ready && images_load_external(
        &document, &stylesheet, &images, budget,
        "https://image-cancel.test/", "https://image-cancel.test/", NULL,
        1, 4096, 4096, 4096, 15000, NULL, NULL);
    tilefinch_platform_set_services(NULL);
    bool ok = loaded && images.count == 0 && images.stats.failed == 1
        && images.stats.no_progress_cancelled == 1
        && images.stats.deadline_cancelled == 0
        && images.stats.maximum_no_progress_ms >= 3000
        && images.stats.cooperative_yields < 16;
    if (ready) fetch_trace_end();
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static bool test_image_request_policy_and_cookies(Budget *budget)
{
    static const char html[] =
        /* The authored HTTP image must be upgraded to the HTTPS replay. A
           failed HTTPS attempt must never retry this original URL. */
        "<!doctype html><body>"
        "<img src='http://assets-image.test/policy.svg'>";
    static const char document_url[] =
        "https://page-image.test/document";
    static const char asset_base_url[] =
        "https://assets-image.test/root/";
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    BrowserSession session = {0};
    char error[256] = {0};
    char cookies[4096] = {0};
    TilefinchRequestContext image_context = {
        .target_url = "https://assets-image.test/policy.svg",
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_IMAGE
    };
    TilefinchRequestContext cache_context = image_context;
    cache_context.target_url =
        "http://assets-image.test/policy.svg";
    bool ok = document_parse(
            &document, budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&stylesheet, budget, &document, 480)
        && browser_session_init(&session, budget, 64 * 1024)
        && browser_session_cookie_set_http_context(
            &session, &image_context,
            "asset_auth=1; Path=/; Secure; SameSite=None; Partitioned")
        && fetch_trace_replay_begin(
#if defined(TILEFINCH_DISABLE_GIF)
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-image-policy-no-gif",
#else
            TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-image-policy",
#endif
            error, sizeof(error))
        && images_load_external(
            &document, &stylesheet, &images, budget, asset_base_url,
            document_url, NULL, 1, 4096, 4096, 4096,
            1000, NULL, &session)
        && images.count == 1 && images.stats.loaded == 1
        && browser_session_cookie_header_context(
            &session, &image_context, cookies, sizeof(cookies))
        && strstr(cookies, "image_seen=x") != NULL;
    const BrowserCacheEntry *cached_image = NULL;
    ok = ok
        && browser_session_cache_lookup(
               &session, cache_context.target_url) == NULL
        && browser_session_cache_match_resource(
               &session, cache_context.target_url, &cache_context,
               tilefinch_platform_monotonic_time_ns(), &cached_image)
               == BROWSER_CACHE_FRESH
        && cached_image != NULL && cached_image->resource_grant_valid
        && cached_image->resource_grant.destination
               == TILEFINCH_DESTINATION_IMAGE
        && cached_image->body != NULL
        && cached_image->data == cached_image->body->data
        && cached_image->body->references == 1;
    if (!ok) {
        fprintf(stderr,
                "mixed image diagnostic replay='%s' images=%zu loaded=%zu "
                "failed=%zu cookies='%s' cache=%p\n",
                error, images.count, images.stats.loaded,
                images.stats.failed, cookies, (const void *) cached_image);
    }
    fetch_trace_end();
    images_destroy(&images);
    browser_session_destroy(&session);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    return ok && budget->current == 0;
}

static const char page[] =
    "<!doctype html><title>Test</title><style>"
    "body{background:#ffffff;color:#000000;padding:4px}"
    ".box{background:#eee8f8;padding:5px;margin:3px}h1{color:#253a67}"
    "</style><body><h1>Stage one</h1><div class=box>"
    "This malformed-ish page <b>still parses and wraps across a narrow device."
    "</div><p>Second <a href='/next'>link for scrolling</a>.</p>"
    "<script>const xs=[1,2,3].map(x=>x*2);globalThis.pocSummary="
    "`${document.title}:${xs.join('-')}:${document.nodeCount}`;</script>";

static bool attribute_equals(lxb_dom_node_t *node, const char *name,
                             const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    size_t wanted_length = strlen(wanted);
    return value != NULL && length == wanted_length
           && memcmp(value, wanted, length) == 0;
}

typedef struct {
    ScriptRuntime *target;
    ScriptResult *result;
} MessageTestTarget;

typedef struct {
    size_t calls;
} MessageOpaqueProbe;

static bool count_test_message(void *opaque, ScriptRuntime *source,
                               long target_frame_handle,
                               const char *json,
                               const char *target_origin)
{
    (void) source; (void) target_frame_handle;
    (void) json; (void) target_origin;
    MessageOpaqueProbe *probe = opaque;
    if (probe == NULL) return false;
    probe->calls++;
    return true;
}

static bool forward_test_message(void *opaque, ScriptRuntime *source,
                                 long target_frame_handle,
                                 const char *json,
                                 const char *target_origin)
{
    (void) source; (void) target_frame_handle; (void) target_origin;
    MessageTestTarget *target = opaque;
    return script_runtime_dispatch_message(target->target, json,
                                           "https://child.test", 7,
                                           target->result);
}

static uint64_t test_platform_wall(void *context)
{
    return *(const uint64_t *) context;
}

static uint64_t test_platform_monotonic(void *context)
{
    return *(const uint64_t *) context + 1;
}

static uint64_t test_platform_monotonic_us(void *context)
{
    return *(const uint64_t *) context + 2;
}

static bool test_platform_random(void *context, void *data, size_t length)
{
    (void) context;
    memset(data, 0xa5, length);
    return true;
}

static bool test_platform_asset(void *context, Budget *budget,
                                const char *path, size_t maximum_bytes,
                                unsigned char **data, size_t *length)
{
    (void) context;
    if (strcmp(path, "test.asset") != 0 || maximum_bytes < 3) return false;
    *data = budget_malloc(budget, 3);
    if (*data == NULL) return false;
    memcpy(*data, "PSP", 3);
    *length = 3;
    return true;
}

static bool test_platform_input(void *context, TilefinchPlatformInput *input)
{
    (void) context;
    *input = (TilefinchPlatformInput) {
        .buttons = 0x1234, .analog_x = -7, .analog_y = 9
    };
    return true;
}

static bool test_platform_present(void *context, const uint16_t *pixels,
                                  size_t width, size_t height, size_t stride)
{
    (void) context;
    return pixels[0] == 0xbeef && width == 1 && height == 1 && stride == 1;
}

static bool test_platform_cooperate(void *context, const char *phase,
                                    size_t completed_work_units)
{
    (void) context;
    return strcmp(phase, "test") == 0 && completed_work_units == 7;
}

typedef struct {
    size_t calls;
    size_t index_calls;
    size_t resource_calls;
    size_t script_calls;
    size_t style_calls;
    size_t cancel_at;
    size_t cancel_index_at;
    size_t cancel_resource_at;
    size_t cancel_script_at;
    size_t cancel_style_at;
} LayoutCooperateProbe;

static bool test_layout_cooperate(void *context, const char *phase,
                                  size_t completed_work_units)
{
    LayoutCooperateProbe *probe = context;
    bool index_phase = strcmp(phase, "layout-index") == 0;
    bool resource_phase = strcmp(phase, "resource") == 0;
    bool script_phase = strcmp(phase, "script") == 0;
    bool style_phase = strcmp(phase, "layout-style") == 0;
    if ((!index_phase && !resource_phase && !script_phase
         && !style_phase && strcmp(phase, "layout") != 0)
        || completed_work_units == 0) {
        return false;
    }
    probe->calls++;
    if (index_phase) probe->index_calls++;
    if (resource_phase) probe->resource_calls++;
    if (script_phase) probe->script_calls++;
    if (style_phase) probe->style_calls++;
    if (probe->cancel_at != 0 && probe->calls >= probe->cancel_at) {
        return false;
    }
    if (probe->cancel_index_at != 0 && index_phase
        && probe->index_calls >= probe->cancel_index_at) return false;
    if (probe->cancel_resource_at != 0 && resource_phase
        && probe->resource_calls >= probe->cancel_resource_at) return false;
    if (probe->cancel_style_at != 0 && style_phase
        && probe->style_calls >= probe->cancel_style_at) return false;
    return probe->cancel_script_at == 0 || !script_phase
           || probe->script_calls < probe->cancel_script_at;
}


/* Suite bodies stay in one translation unit so allocator and callback
   fixtures preserve their established process-local semantics. */
#include "suites/foundation_platform.inc"
#include "suites/foundation_document.inc"
#include "suites/web_runtime_render.inc"
#include "suites/web_runtime_navigation.inc"
#include "suites/web_runtime_forms.inc"
#include "suites/sections.inc"

    budget_install_lexbor(&tiny);
    PocDocument rejected;
    CHECK(!document_parse(&rejected, &tiny, page, strlen(page), 17));
    document_destroy(&rejected);
    CHECK(tiny.failure_count > 0 && tiny.current == 0);

    return 0;
}

typedef int (*TilefinchTestSuiteFunction)(void);

typedef struct {
    const char *name;
    const char *description;
    TilefinchTestSuiteFunction run;
} TilefinchTestSuite;

static const TilefinchTestSuite tilefinch_test_suites[] = {
    {
        "foundation",
        "allocator, parser, fetch, platform, navigation, and basic rendering",
        test_foundation
    },
    {
        "web-runtime",
        "images, DOM, JavaScript, controls, events, frames, and resources",
        test_web_runtime
    },
    {
        "sections",
        "compressed section store, streaming, paging, and adaptive routing",
        test_sections
    }
};

static void tilefinch_test_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--list] [--filter SUITE]\n"
            "       %s [SUITE]\n",
            program, program);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *filter = NULL;
    bool list = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list = true;
        } else if (strcmp(argv[i], "--filter") == 0) {
            if (++i >= argc || filter != NULL) {
                tilefinch_test_usage(argv[0]);
                return 2;
            }
            filter = argv[i];
        } else if (argv[i][0] != '-' && filter == NULL) {
            filter = argv[i];
        } else {
            tilefinch_test_usage(argv[0]);
            return 2;
        }
    }

    if (list) {
        puts("core\tfoundation and web-runtime aggregate");
        for (size_t i = 0;
             i < sizeof(tilefinch_test_suites) / sizeof(tilefinch_test_suites[0]);
             i++) {
            printf("%s\t%s\n", tilefinch_test_suites[i].name,
                   tilefinch_test_suites[i].description);
        }
        return 0;
    }

    size_t selected = 0;
    for (size_t i = 0;
         i < sizeof(tilefinch_test_suites) / sizeof(tilefinch_test_suites[0]);
         i++) {
        const TilefinchTestSuite *suite = &tilefinch_test_suites[i];
        bool core_alias = filter != NULL && strcmp(filter, "core") == 0;
        if (filter != NULL && strcmp(filter, suite->name) != 0
            && !(core_alias && strcmp(suite->name, "sections") != 0)) {
            continue;
        }
        selected++;
        printf("suite: %s\n", suite->name);
        int result = suite->run();
        if (result != 0) return result;
    }
    if (selected == 0) {
        fprintf(stderr, "unknown test suite: %s\n", filter);
        tilefinch_test_usage(argv[0]);
        return 2;
    }

    puts("tilefinch-tests: all checks passed");
    return 0;
}
