static uint64_t navigation_elapsed_us(uint64_t started, uint64_t finished)
{
    return finished >= started ? finished - started : 0;
}

static void navigation_stream_capture_server_body(
    NavigationDocumentStream *stream, PocDocument *document)
{
    if (stream == NULL || document == NULL) return;
    if (stream->server_body_snapshot.markup == NULL) {
        /* Before the first useful prefix, capture returns before serialization
           when visible text is below its bound. Keep checking so head scripts
           cannot permanently prevent a later server body from being retained. */
        if (document_body_snapshot_capture(
                document, &stream->server_body_snapshot)) {
            stream->server_body_snapshot_node_count = document->node_count;
            stream->server_body_snapshot_text_bytes = document->text_bytes;
        }
        return;
    }

    /* Permit at most two transactional refresh attempts. Cheap parser census
       deltas prevent serializing an unchanged prefix; the semantic comparison
       below accepts only a genuinely newer useful body. */
    if (stream->server_body_snapshot_refresh_attempts >= 2u) return;
    bool node_growth = document->node_count
            >= stream->server_body_snapshot_node_count
                && document->node_count
                       - stream->server_body_snapshot_node_count >= 8u;
    bool text_growth = document->text_bytes
            >= stream->server_body_snapshot_text_bytes
                && document->text_bytes
                       - stream->server_body_snapshot_text_bytes >= 256u;
    /* A useful late GET form or link can add only a couple of nodes and bytes.
       On raw growth below both serialization thresholds, pay one bounded,
       allocation-free action census so that compact server controls are not
       stranded behind the coarse churn gate. */
    bool compact_action_growth = false;
    if (!node_growth && !text_growth
        && document->node_count > stream->server_body_snapshot_node_count) {
        compact_action_growth = document_body_action_count(document)
            > stream->server_body_snapshot.source_action_count;
    }
    if (!node_growth && !text_growth && !compact_action_growth) return;

    stream->server_body_snapshot_refresh_attempts++;
    stream->server_body_snapshot_node_count = document->node_count;
    stream->server_body_snapshot_text_bytes = document->text_bytes;
    DocumentBodySnapshot candidate = {0};
    if (!document_body_snapshot_capture(document, &candidate)) return;
    const DocumentBodySnapshot *current = &stream->server_body_snapshot;
    bool keeps_text = candidate.source_text_bytes >= current->source_text_bytes;
    bool adds_action = candidate.source_action_count
        > current->source_action_count;
    bool adds_text = candidate.source_text_bytes
            >= current->source_text_bytes
        && candidate.source_text_bytes - current->source_text_bytes >= 64u;
    if (keeps_text && (adds_action || adds_text)) {
        document_body_snapshot_destroy(&stream->server_body_snapshot);
        stream->server_body_snapshot = candidate;
        return;
    }
    document_body_snapshot_destroy(&candidate);
}

static void navigation_record_blocking_script_sample(
    NavigationSession *session,
    const NavigationBlockingScriptSample *sample)
{
    if (session == NULL || sample == NULL) return;
    NavigationPerformance *performance = &session->performance;
    performance->parser_script_compile_us += sample->compile_us;
    performance->parser_script_execute_us += sample->execute_us;
    performance->parser_script_rescan_us +=
        sample->metadata_us + sample->stylesheet_fingerprint_us;
    if (sample->dom_mutations
        > SIZE_MAX - performance->parser_script_mutations) {
        performance->parser_script_mutations = SIZE_MAX;
    } else {
        performance->parser_script_mutations += sample->dom_mutations;
    }
    if (performance->blocking_script_sample_count
        < NAVIGATION_BLOCKING_SCRIPT_SAMPLE_LIMIT) {
        performance->blocking_script_samples[
            performance->blocking_script_sample_count++] = *sample;
    } else {
        performance->blocking_script_samples_dropped++;
    }
}

#if !defined(__PSP__)
static NavigationParserCheckpointTestFault
    navigation_parser_checkpoint_test_fault;

void navigation_test_refuse_next_parser_checkpoint(
    NavigationParserCheckpointTestFault fault)
{
    navigation_parser_checkpoint_test_fault = fault;
}

static bool navigation_test_consume_parser_checkpoint_fault(
    NavigationParserCheckpointTestFault fault)
{
    if (fault == NAVIGATION_TEST_PARSER_CHECKPOINT_NONE
        || navigation_parser_checkpoint_test_fault != fault) return false;
    navigation_parser_checkpoint_test_fault =
        NAVIGATION_TEST_PARSER_CHECKPOINT_NONE;
    return true;
}
#else
static bool navigation_test_consume_parser_checkpoint_fault(unsigned fault)
{
    (void) fault;
    return false;
}
#endif

static const char *navigation_parser_refusal_summary(
    NavigationParserRefusalReason reason)
{
    switch (reason) {
    case NAVIGATION_PARSER_REFUSAL_METADATA:
        return "JavaScript stopped after bounded page metadata";
    case NAVIGATION_PARSER_REFUSAL_FINGERPRINT:
        return "JavaScript stopped after bounded stylesheet discovery";
    case NAVIGATION_PARSER_REFUSAL_STYLESHEET:
        return "JavaScript stopped after bounded stylesheet preparation";
    case NAVIGATION_PARSER_REFUSAL_MUTATION:
        return "JavaScript stopped after bounded DOM mutation work";
    case NAVIGATION_PARSER_REFUSAL_CSSOM:
        return "JavaScript stopped after bounded style/layout work";
    case NAVIGATION_PARSER_REFUSAL_SCRIPT:
        return "JavaScript stopped; page retained";
    case NAVIGATION_PARSER_REFUSAL_NONE:
    default:
        return "Optional parser work stopped; page retained";
    }
}

static void navigation_record_parser_refusal(
    NavigationSession *session, NavigationParserRefusalReason reason)
{
    if (session == NULL) return;
    NavigationPerformance *performance = &session->performance;
    performance->parser_checkpoint_soft_refusals++;
    switch (reason) {
    case NAVIGATION_PARSER_REFUSAL_METADATA:
        performance->parser_checkpoint_metadata_refusals++;
        break;
    case NAVIGATION_PARSER_REFUSAL_FINGERPRINT:
        performance->parser_checkpoint_fingerprint_refusals++;
        break;
    case NAVIGATION_PARSER_REFUSAL_STYLESHEET:
        performance->parser_checkpoint_stylesheet_refusals++;
        break;
    case NAVIGATION_PARSER_REFUSAL_MUTATION:
        performance->parser_checkpoint_mutation_refusals++;
        break;
    case NAVIGATION_PARSER_REFUSAL_CSSOM:
        performance->parser_checkpoint_cssom_refusals++;
        break;
    case NAVIGATION_PARSER_REFUSAL_SCRIPT:
        performance->parser_checkpoint_script_refusals++;
        break;
    case NAVIGATION_PARSER_REFUSAL_NONE:
    default:
        break;
    }
}

/* Record a refusal from inside an author callback without destroying the
   active QuickJS realm underneath that callback. The outer parser-script
   boundary consumes the pending reason after evaluation unwinds. */
static void navigation_stream_mark_parser_refusal(
    NavigationDocumentStream *stream, NavigationParserRefusalReason reason)
{
    if (stream == NULL) return;
    stream->parser_checkpoint_outcome =
        NAVIGATION_PARSER_CHECKPOINT_SOFT_REFUSAL;
    if (stream->pending_parser_refusal == NAVIGATION_PARSER_REFUSAL_NONE) {
        stream->pending_parser_refusal = reason;
    }
}

static bool navigation_stream_suppress_optional_parser_work(
    NavigationDocumentStream *stream, NavigationParserRefusalReason reason)
{
    if (stream == NULL || stream->session == NULL
        || stream->session->cancelled) {
        return false;
    }
    if (stream->pending_parser_refusal != NAVIGATION_PARSER_REFUSAL_NONE) {
        reason = stream->pending_parser_refusal;
    }
    stream->pending_parser_refusal = NAVIGATION_PARSER_REFUSAL_NONE;
    stream->parser_checkpoint_outcome =
        NAVIGATION_PARSER_CHECKPOINT_SOFT_REFUSAL;
    if (stream->optional_work_shed) return true;

    navigation_record_parser_refusal(stream->session, reason);
    if (stream->cssom_layout_ready) {
        if (stream->runtime != NULL) {
            script_runtime_set_layout(stream->runtime, NULL, 0);
        }
        layout_destroy(&stream->cssom_layout);
        memset(&stream->cssom_layout, 0, sizeof(stream->cssom_layout));
        stream->cssom_layout_ready = false;
    }
    /* A checkpoint refusal makes the temporary sheet ineligible for final
       adoption. EOF will build one authoritative sheet from the complete
       DOM after the parser and transport pressure have cleared. */
    navigation_stream_release_blocking_stylesheet(stream);
    if (reason == NAVIGATION_PARSER_REFUSAL_METADATA
        && strncmp(stream->session->last_error, "stream ", 7) == 0) {
        stream->session->last_error[0] = '\0';
    }
    navigation_stream_shed_optional_work(
        stream, navigation_parser_refusal_summary(reason), false);
    return true;
}

/* A large realm for later async application work does not authorize the
   parser itself to monopolize that allowance. This is cumulative source/work
   admission, not a second heap or per-file quota. */
#define NAVIGATION_PARSER_SCRIPT_STAGE_MAX_WORK (1024u * 1024u)
#define NAVIGATION_PARSER_SCRIPT_STAGE_MAX_US 8000000u
#define NAVIGATION_PARSER_SCRIPT_FAILURE_LIMIT 3u

typedef enum {
    NAVIGATION_PARSER_SCRIPT_BREAKER_FAILURES = 0,
    NAVIGATION_PARSER_SCRIPT_BREAKER_TIME,
    NAVIGATION_PARSER_SCRIPT_BREAKER_WORK
} NavigationParserScriptBreakerReason;

#if !defined(__PSP__)
static size_t navigation_parser_script_stage_test_work_limit;
static uint64_t navigation_parser_script_stage_test_time_limit_us;
static uint64_t navigation_parser_script_stage_test_elapsed_us;

/* Kept private to the navigation test executable rather than expanding the
   public API for a deterministic work-bound seam. Zero restores production. */
void navigation_test_set_parser_script_stage_work_limit(size_t limit);
void navigation_test_set_parser_script_stage_work_limit(size_t limit)
{
    navigation_parser_script_stage_test_work_limit = limit;
}

void navigation_test_set_parser_script_stage_elapsed_us(uint64_t elapsed_us);
void navigation_test_set_parser_script_stage_elapsed_us(uint64_t elapsed_us)
{
    navigation_parser_script_stage_test_elapsed_us = elapsed_us;
}

void navigation_test_set_parser_script_stage_time_limit_us(uint64_t limit_us);
void navigation_test_set_parser_script_stage_time_limit_us(uint64_t limit_us)
{
    navigation_parser_script_stage_test_time_limit_us = limit_us;
}
#endif

static size_t navigation_parser_script_stage_work_limit(
    const NavigationDocumentStream *stream)
{
    (void) stream;
#if !defined(__PSP__)
    if (navigation_parser_script_stage_test_work_limit != 0u) {
        return navigation_parser_script_stage_test_work_limit;
    }
#endif
    return NAVIGATION_PARSER_SCRIPT_STAGE_MAX_WORK;
}

static uint64_t navigation_parser_script_stage_time_limit_us(void)
{
#if !defined(__PSP__)
    if (navigation_parser_script_stage_test_time_limit_us != 0u) {
        return navigation_parser_script_stage_test_time_limit_us;
    }
#endif
    return NAVIGATION_PARSER_SCRIPT_STAGE_MAX_US;
}

static bool navigation_stream_open_parser_script_circuit(
    NavigationDocumentStream *stream, const char *summary,
    NavigationParserScriptBreakerReason reason)
{
    if (stream == NULL) return false;
    stream->parser_script_circuit_open = true;
    if (!navigation_stream_suppress_optional_parser_work(
            stream, NAVIGATION_PARSER_REFUSAL_SCRIPT)) {
        stream->parser_script_circuit_open = false;
        return false;
    }
    snprintf(stream->script_result.summary,
             sizeof(stream->script_result.summary), "%s", summary);
    NavigationPerformance *performance = &stream->session->performance;
    if (performance->parser_script_stage_breakers != SIZE_MAX) {
        performance->parser_script_stage_breakers++;
    }
    size_t *reason_count = reason == NAVIGATION_PARSER_SCRIPT_BREAKER_FAILURES
        ? &performance->parser_script_stage_failure_breakers
        : reason == NAVIGATION_PARSER_SCRIPT_BREAKER_TIME
            ? &performance->parser_script_stage_time_breakers
            : &performance->parser_script_stage_work_breakers;
    if (*reason_count != SIZE_MAX) (*reason_count)++;
    return true;
}

static bool navigation_stream_account_parser_script(
    NavigationDocumentStream *stream, uint64_t elapsed_us,
    size_t source_work, bool failed, bool completed_cleanly)
{
    if (stream == NULL) return false;
#if !defined(__PSP__)
    if (navigation_parser_script_stage_test_elapsed_us != 0u) {
        elapsed_us = navigation_parser_script_stage_test_elapsed_us;
    }
#endif
    if (elapsed_us > UINT64_MAX - stream->parser_script_stage_us) {
        stream->parser_script_stage_us = UINT64_MAX;
    } else {
        stream->parser_script_stage_us += elapsed_us;
    }
    if (source_work > SIZE_MAX - stream->parser_script_stage_work) {
        stream->parser_script_stage_work = SIZE_MAX;
    } else {
        stream->parser_script_stage_work += source_work;
    }
    NavigationPerformance *performance = &stream->session->performance;
    if (elapsed_us > UINT64_MAX - performance->parser_script_stage_us) {
        performance->parser_script_stage_us = UINT64_MAX;
    } else {
        performance->parser_script_stage_us += elapsed_us;
    }
    if (source_work > SIZE_MAX - performance->parser_script_stage_work) {
        performance->parser_script_stage_work = SIZE_MAX;
    } else {
        performance->parser_script_stage_work += source_work;
    }
    if (failed) {
        if (stream->parser_script_consecutive_failures != SIZE_MAX) {
            stream->parser_script_consecutive_failures++;
        }
    } else if (completed_cleanly) {
        stream->parser_script_consecutive_failures = 0u;
    }
    if (stream->optional_work_shed) return true;

    const char *summary = NULL;
    NavigationParserScriptBreakerReason reason =
        NAVIGATION_PARSER_SCRIPT_BREAKER_FAILURES;
    if (stream->parser_script_consecutive_failures
        >= NAVIGATION_PARSER_SCRIPT_FAILURE_LIMIT) {
        summary = "Limited page: JavaScript stopped after repeated failures";
    } else if (stream->parser_script_stage_us
               >= navigation_parser_script_stage_time_limit_us()) {
        summary = "Limited page: JavaScript stopped after its parser time limit";
        reason = NAVIGATION_PARSER_SCRIPT_BREAKER_TIME;
    } else if (stream->parser_script_stage_work
               >= navigation_parser_script_stage_work_limit(stream)) {
        summary = "Limited page: JavaScript stopped after bounded parser work";
        reason = NAVIGATION_PARSER_SCRIPT_BREAKER_WORK;
    }
    return summary == NULL
        || navigation_stream_open_parser_script_circuit(
               stream, summary, reason);
}

typedef struct {
    long timeout_ms;
    size_t maximum_file_bytes;
    uint64_t deadline_us;
    bool cumulative_work_limited;
} NavigationParserScriptAllowance;

typedef enum {
    NAVIGATION_PARSER_SCRIPT_ALLOWANCE_READY = 0,
    NAVIGATION_PARSER_SCRIPT_ALLOWANCE_SKIPPED,
    NAVIGATION_PARSER_SCRIPT_ALLOWANCE_FAILED
} NavigationParserScriptAllowanceResult;

static void navigation_stream_record_skipped_parser_script(
    NavigationDocumentStream *stream)
{
    if (stream == NULL || stream->session == NULL) return;
    ExternalScriptMetrics *metrics = &stream->scripts.early;
    if (metrics->discovered != SIZE_MAX) metrics->discovered++;
    if (metrics->parser_blocking != SIZE_MAX) metrics->parser_blocking++;
    if (metrics->discovered <= stream->session->maximum_scripts) {
        if (metrics->skipped_pressure != SIZE_MAX) {
            metrics->skipped_pressure++;
        }
        if (stream->session->performance.parser_script_stage_skipped
            != SIZE_MAX) {
            stream->session->performance.parser_script_stage_skipped++;
        }
    } else if (metrics->skipped_quota != SIZE_MAX) {
        metrics->skipped_quota++;
    }
}

/* Freeze the remaining cumulative allowance before entering a loader which
   may perform a blocking fetch and only then arm the QuickJS watchdog.  The
   runtime receives an absolute cap, so fetch time and evaluation time cannot
   each consume the full remainder independently. */
static NavigationParserScriptAllowanceResult
navigation_stream_prepare_parser_script_allowance(
    NavigationDocumentStream *stream, size_t known_inline_bytes,
    uint64_t current_callback_us,
    NavigationParserScriptAllowance *allowance)
{
    if (stream == NULL || stream->session == NULL || allowance == NULL)
        return NAVIGATION_PARSER_SCRIPT_ALLOWANCE_FAILED;
    uint64_t time_limit = navigation_parser_script_stage_time_limit_us();
    size_t work_limit = navigation_parser_script_stage_work_limit(stream);
    /* Parser callbacks also include metadata, stylesheet construction and
       transport settlement. Only author JavaScript consumes this breaker;
       the runtime deadline begins immediately before the author turn. */
    (void) current_callback_us;
    uint64_t stage_used_us = stream->parser_script_stage_us;
    if (stage_used_us >= time_limit) {
        navigation_stream_record_skipped_parser_script(stream);
        return navigation_stream_open_parser_script_circuit(
            stream,
            "Limited page: JavaScript stopped after its parser time limit",
            NAVIGATION_PARSER_SCRIPT_BREAKER_TIME)
            ? NAVIGATION_PARSER_SCRIPT_ALLOWANCE_SKIPPED
            : NAVIGATION_PARSER_SCRIPT_ALLOWANCE_FAILED;
    }
    if (stream->parser_script_stage_work >= work_limit
        || known_inline_bytes
               > work_limit - stream->parser_script_stage_work) {
        navigation_stream_record_skipped_parser_script(stream);
        return navigation_stream_open_parser_script_circuit(
            stream,
            "Limited page: JavaScript stopped after bounded parser work",
            NAVIGATION_PARSER_SCRIPT_BREAKER_WORK)
            ? NAVIGATION_PARSER_SCRIPT_ALLOWANCE_SKIPPED
            : NAVIGATION_PARSER_SCRIPT_ALLOWANCE_FAILED;
    }
    uint64_t remaining_us = time_limit - stage_used_us;
    uint64_t now_us = navigation_now_us();
    allowance->deadline_us = remaining_us > UINT64_MAX - now_us
        ? UINT64_MAX : now_us + remaining_us;
    uint64_t timeout_ms = remaining_us / UINT64_C(1000);
    if (remaining_us % UINT64_C(1000) != 0u) timeout_ms++;
    if (timeout_ms == 0u) timeout_ms = 1u;
    long configured_timeout = stream->session->script_timeout_ms;
    allowance->timeout_ms = timeout_ms > (uint64_t) LONG_MAX
        ? LONG_MAX : (long) timeout_ms;
    if (configured_timeout > 0
        && configured_timeout < allowance->timeout_ms) {
        allowance->timeout_ms = configured_timeout;
    }
    size_t remaining_work = work_limit - stream->parser_script_stage_work;
    allowance->maximum_file_bytes =
        stream->session->maximum_script_file_bytes;
    allowance->cumulative_work_limited =
        remaining_work < allowance->maximum_file_bytes;
    if (allowance->maximum_file_bytes > remaining_work) {
        allowance->maximum_file_bytes = remaining_work;
    }
    script_runtime_limit_execution_for_us(stream->runtime, remaining_us);
    return NAVIGATION_PARSER_SCRIPT_ALLOWANCE_READY;
}

static void navigation_stream_finish_parser_script_allowance(
    ScriptRuntime *runtime)
{
    script_runtime_clear_execution_limit(runtime);
}

typedef struct {
    bool parser_blocking;
    bool external;
    size_t inline_source_bytes;
} NavigationParserScriptShape;

/* Inspect a parser-owned script only while no author callback can run.  In
   particular, do not retain attribute/text pointers across a mutation
   checkpoint: an observer may detach or completely rewrite this element. */
static NavigationParserScriptShape navigation_parser_script_shape(
    lxb_dom_node_t *element)
{
    NavigationParserScriptShape shape = {0};
    shape.parser_blocking = document_script_is_parser_blocking(element);
    if (!shape.parser_blocking) return shape;

    size_t source_length = 0;
    const char *source = document_attribute(element, "src", &source_length);
    if ((source == NULL || source_length == 0)
        && element->ns == LXB_NS_SVG) {
        source = document_attribute(element, "href", &source_length);
    }
    shape.external = source != NULL && source_length != 0;
    if (shape.external) return shape;

    for (lxb_dom_node_t *child = element->first_child;
         child != NULL; child = child->next) {
        size_t length = 0;
        const char *text = document_text_data(child, &length);
        if (text == NULL) continue;
        if (length > SIZE_MAX - shape.inline_source_bytes) {
            shape.inline_source_bytes = SIZE_MAX;
            break;
        }
        shape.inline_source_bytes += length;
    }
    return shape;
}

static bool navigation_parser_script_copy_source_reference(
    lxb_dom_node_t *element, char *destination, size_t capacity)
{
    if (element == NULL || destination == NULL || capacity == 0) return false;
    size_t source_length = 0;
    const char *source = document_attribute(element, "src", &source_length);
    if ((source == NULL || source_length == 0)
        && element->ns == LXB_NS_SVG) {
        source = document_attribute(element, "href", &source_length);
    }
    if (source == NULL || source_length == 0 || source_length >= capacity) {
        return false;
    }
    memcpy(destination, source, source_length);
    destination[source_length] = '\0';
    return true;
}

/* A mutation checkpoint can consume the entire parser-stage slice without an
   actual script evaluation (for example, a hostile MutationObserver). Charge
   only that author turn: the surrounding element-close callback may contain
   blocking stylesheet or transport work which is not JavaScript time. */
static bool navigation_stream_finish_parser_mutation_checkpoint(
    NavigationDocumentStream *stream, uint64_t mutation_started,
    uint64_t deadline_us, bool failed)
{
    if (stream == NULL || stream->session == NULL) return false;
    uint64_t finished = navigation_now_us();
    uint64_t elapsed = navigation_elapsed_us(mutation_started, finished);
    if (elapsed > UINT64_MAX
                    - stream->session->performance.parser_callback_us) {
        stream->session->performance.parser_callback_us = UINT64_MAX;
    } else {
        stream->session->performance.parser_callback_us += elapsed;
    }
    if (!navigation_stream_account_parser_script(
            stream, elapsed, 0u, failed, false)) {
        return false;
    }
    if (!stream->parser_script_circuit_open && deadline_us != 0u
        && finished >= deadline_us
        && !navigation_stream_open_parser_script_circuit(
               stream,
               "Limited page: JavaScript stopped after its parser time limit",
               NAVIGATION_PARSER_SCRIPT_BREAKER_TIME)) {
        return false;
    }
    if (stream->parser_script_circuit_open) return true;
    return !failed || navigation_stream_suppress_optional_parser_work(
                          stream, NAVIGATION_PARSER_REFUSAL_MUTATION);
}

static NavigationParserCheckpointOutcome
navigation_stream_metadata_checkpoint(NavigationDocumentStream *stream)
{
    if (stream == NULL) return NAVIGATION_PARSER_CHECKPOINT_HARD_FAILURE;
    if (stream->optional_work_shed) return NAVIGATION_PARSER_CHECKPOINT_OK;
    bool refused = navigation_test_consume_parser_checkpoint_fault(
        NAVIGATION_TEST_PARSER_CHECKPOINT_METADATA);
    if (!refused) refused = !navigation_stream_scan_metadata(stream);
    if (!refused) return NAVIGATION_PARSER_CHECKPOINT_OK;
    return navigation_stream_suppress_optional_parser_work(
               stream, NAVIGATION_PARSER_REFUSAL_METADATA)
        ? NAVIGATION_PARSER_CHECKPOINT_SOFT_REFUSAL
        : NAVIGATION_PARSER_CHECKPOINT_HARD_FAILURE;
}

static bool navigation_stream_element_closed(void *opaque,
                                             PocDocument *document,
                                             lxb_dom_node_t *element)
{
    NavigationDocumentStream *stream = opaque;
    if (stream->closed_element_count != SIZE_MAX) {
        stream->closed_element_count++;
    }
    size_t name_length = 0;
    const char *name = document_element_name(element, &name_length);
    if (name != NULL && name_length == 4
        && memcmp(name, "head", 4) == 0) {
        stream->head_closed = true;
        NavigationParserCheckpointOutcome metadata =
            navigation_stream_metadata_checkpoint(stream);
        if (metadata == NAVIGATION_PARSER_CHECKPOINT_HARD_FAILURE) {
            return false;
        }
        stream->preview_checkpoint_checks = 0;
        navigation_stream_try_pre_eof_preview(stream);
        return true;
    }
    if (name != NULL && name_length == 4
        && memcmp(name, "body", 4) == 0) {
        /* Body-close is the strongest streaming checkpoint, but ordinary
           browsers may already have painted after the head and a useful body
           prefix. Keep it as a final pre-EOF opportunity. */
        stream->body_closed = true;
        stream->preview_checkpoint_checks = 0;
        navigation_stream_try_pre_eof_preview(stream);
        return true;
    }
    if (name == NULL || name_length != 6
        || memcmp(name, "script", 6) != 0) return true;
    if (stream->session == NULL) return true;
    if (document_parser_script_was_truncated(stream->parser, element)) {
        navigation_stream_record_skipped_parser_script(stream);
        return true;
    }
    NavigationParserScriptShape script_shape =
        navigation_parser_script_shape(element);
    bool parser_blocking = script_shape.parser_blocking;
    if (stream->parser_script_circuit_open) {
        if (parser_blocking)
            navigation_stream_record_skipped_parser_script(stream);
        return true;
    }
    if (!stream->session->scripts_enabled
        || !stream->session->document_scripts_enabled
        || stream->script_disabled_after_failure) {
        return true;
    }
    uint64_t callback_started = navigation_now_us();
    bool external = script_shape.external;
    size_t inline_source_bytes = script_shape.inline_source_bytes;
    size_t script_ordinal = stream->scripts.early.parser_blocking + 1u;
    NavigationSession *session = stream->session;

    /* Once the realm's cumulative executable count is exhausted, this
       script cannot observe an intermediate stylesheet or metadata state.
       Still pass it through the loader so discovery/skipped metrics and the
       parser-executed set remain authoritative; EOF performs the final style
       build for the complete document. */
    if (parser_blocking && stream->runtime != NULL
        && !script_runtime_script_slot_available(stream->runtime)
        /* Before first paint, a later non-executing script can still be the
           first stable checkpoint after newly parsed CSS. Preserve that one
           opportunity; after paint, or without a compositor, its
           intermediate sheet is unobservable and may be skipped. */
        && (session->progressive_paint == NULL
            || stream->pre_eof_preview_painted
            || stream->pre_eof_preview_attempted)) {
        long element_handle = script_runtime_node_weak_handle(
            stream->runtime, element);
        if (element_handle == 0) {
            navigation_stream_record_skipped_parser_script(stream);
            return true;
        }
        NavigationParserScriptAllowance mutation_allowance = {0};
        NavigationParserScriptAllowanceResult mutation_allowance_result =
            navigation_stream_prepare_parser_script_allowance(
                stream, 0u,
                navigation_elapsed_us(callback_started, navigation_now_us()),
                &mutation_allowance);
        if (mutation_allowance_result
            != NAVIGATION_PARSER_SCRIPT_ALLOWANCE_READY) {
            return mutation_allowance_result
                == NAVIGATION_PARSER_SCRIPT_ALLOWANCE_SKIPPED;
        }
        ScriptRuntime *mutation_runtime = stream->runtime;
        uint64_t mutation_started = navigation_now_us();
        bool mutation_refused =
            navigation_test_consume_parser_checkpoint_fault(
                NAVIGATION_TEST_PARSER_CHECKPOINT_MUTATION)
            || !script_runtime_parser_mutation_checkpoint(mutation_runtime);
        navigation_stream_finish_parser_script_allowance(mutation_runtime);
        if (mutation_refused) {
            return navigation_stream_finish_parser_mutation_checkpoint(
                stream, mutation_started, mutation_allowance.deadline_us, true);
        }
        element = script_runtime_node_handle_resolve_connected(
            mutation_runtime, element_handle);
        if (element == NULL) {
            return navigation_stream_finish_parser_mutation_checkpoint(
                stream, mutation_started, mutation_allowance.deadline_us,
                false);
        }
        script_shape = navigation_parser_script_shape(element);
        if (!script_shape.parser_blocking) {
            return navigation_stream_finish_parser_mutation_checkpoint(
                stream, mutation_started, mutation_allowance.deadline_us,
                false);
        }
        parser_blocking = true;
        external = script_shape.external;
        inline_source_bytes = script_shape.inline_source_bytes;
        ScriptRuntimeTelemetry telemetry_before = {0};
        script_runtime_telemetry(stream->runtime, &telemetry_before);
        size_t source_bytes_before = stream->scripts.early.source_work_bytes;
        size_t failed_before = stream->scripts.early.failed;
        size_t execution_failures_before =
            stream->scripts.early.execution_failures;
        uint64_t execution_us_before = stream->scripts.early.execution_us;
        size_t skipped_quota_before = stream->scripts.early.skipped_quota;
        size_t skipped_pressure_before =
            stream->scripts.early.skipped_pressure;
        uint64_t script_started = navigation_now_us();
        NavigationParserScriptAllowance allowance = {0};
        NavigationParserScriptAllowanceResult allowance_result =
            navigation_stream_prepare_parser_script_allowance(
                stream, external ? 0u : inline_source_bytes,
                navigation_elapsed_us(callback_started, navigation_now_us()),
                &allowance);
        if (allowance_result != NAVIGATION_PARSER_SCRIPT_ALLOWANCE_READY) {
            return allowance_result
                       == NAVIGATION_PARSER_SCRIPT_ALLOWANCE_SKIPPED;
        }
        ScriptRuntime *limited_runtime = stream->runtime;
        navigation_stream_capture_server_body(stream, document);
        DocumentScriptProcessResult process_result =
            document_scripts_process_closed(
                limited_runtime, session->budget, session->browser_session,
                stream->resource_base_url, stream->document_url,
                stream->referrer_policy,
                &stream->parser->document.content_security_policy,
                session->maximum_scripts, session->maximum_script_bytes,
                allowance.maximum_file_bytes, allowance.timeout_ms,
                stream->scheduler, element, &stream->scripts);
        bool processed =
            process_result == DOCUMENT_SCRIPT_PROCESS_COMPLETE;
        navigation_stream_finish_parser_script_allowance(limited_runtime);
        if (process_result == DOCUMENT_SCRIPT_PROCESS_HARD_FAILURE) {
            return false;
        }
        bool parser_refusal_pending =
            stream->pending_parser_refusal != NAVIGATION_PARSER_REFUSAL_NONE;
        if (parser_refusal_pending) processed = false;
        uint64_t callback_finished = navigation_now_us();
        ScriptRuntimeTelemetry telemetry_after = {0};
        script_runtime_telemetry(stream->runtime, &telemetry_after);
        if (parser_refusal_pending) {
            (void) navigation_stream_suppress_optional_parser_work(
                stream, stream->pending_parser_refusal);
        }
        size_t source_bytes =
            stream->scripts.early.source_work_bytes >= source_bytes_before
            ? stream->scripts.early.source_work_bytes - source_bytes_before
            : 0;
        if (!external && source_bytes == 0) {
            source_bytes = inline_source_bytes;
        }
        size_t mutations =
            telemetry_after.dom_mutations >= telemetry_before.dom_mutations
            ? telemetry_after.dom_mutations
                - telemetry_before.dom_mutations : 0;
        uint64_t host_callback_us =
            telemetry_after.host_callback_total_us
                >= telemetry_before.host_callback_total_us
            ? telemetry_after.host_callback_total_us
                - telemetry_before.host_callback_total_us : 0;
        uint64_t process_us = navigation_elapsed_us(
            script_started, callback_finished);
        uint64_t execution_us = stream->scripts.early.execution_us
                >= execution_us_before
            ? stream->scripts.early.execution_us - execution_us_before : 0;
        NavigationBlockingScriptSample sample = {
            .ordinal = script_ordinal,
            .node_count = document == NULL ? 0 : document->node_count,
            .source_bytes = source_bytes,
            .dom_mutations = mutations,
            .total_us = navigation_elapsed_us(
                callback_started, callback_finished),
            .process_us = process_us,
            .host_callback_us = host_callback_us,
            .execute_us = execution_us,
            .external = external,
            .succeeded = processed
        };
        navigation_record_blocking_script_sample(session, &sample);
        session->performance.parser_script_us += process_us;
        session->performance.parser_callback_us += sample.total_us;
        size_t failure_count = stream->scripts.early.failed >= failed_before
            ? stream->scripts.early.failed - failed_before : 0u;
        bool script_failed = failure_count != 0u;
        bool execution_failed =
            stream->scripts.early.execution_failures
                > execution_failures_before;
        bool cumulative_work_exhausted =
            process_result == DOCUMENT_SCRIPT_PROCESS_SOURCE_LIMIT
            && allowance.cumulative_work_limited;
        bool individual_source_limit =
            process_result == DOCUMENT_SCRIPT_PROCESS_SOURCE_LIMIT
            && !allowance.cumulative_work_limited
            && !parser_refusal_pending;
        if (script_failed && stream->runtime != NULL) {
            script_runtime_preserve_first_error(
                stream->runtime, &stream->script_result);
        }
        bool completed_cleanly = processed && !script_failed
            && stream->scripts.early.skipped_quota == skipped_quota_before
            && stream->scripts.early.skipped_pressure
                   == skipped_pressure_before;
        if (!navigation_stream_account_parser_script(
                stream, execution_us, source_bytes, execution_failed,
                completed_cleanly)) {
            return false;
        }
        if (cumulative_work_exhausted
            && !stream->parser_script_circuit_open
            && !navigation_stream_open_parser_script_circuit(
                   stream,
                   "Limited page: JavaScript stopped after bounded parser work",
                   NAVIGATION_PARSER_SCRIPT_BREAKER_WORK)) {
            return false;
        }
        if (stream->parser_script_circuit_open) return true;
        if (processed) {
            navigation_stream_launch_preloads(stream);
            if (!navigation_stream_has_active_style_speculation(stream)) {
                navigation_stream_progress_preloads(stream, false);
            }
            stream->preview_checkpoint_checks = 0;
            navigation_stream_try_pre_eof_preview(stream);
        }
        return processed || individual_source_limit
            || navigation_stream_suppress_optional_parser_work(
                   stream, NAVIGATION_PARSER_REFUSAL_SCRIPT);
    }

    uint64_t metadata_started = callback_started;
    NavigationParserCheckpointOutcome metadata =
        navigation_stream_metadata_checkpoint(stream);
    if (metadata == NAVIGATION_PARSER_CHECKPOINT_HARD_FAILURE) return false;
    uint64_t metadata_finished = navigation_now_us();
    stream->session->performance.parser_metadata_us +=
        metadata_finished >= metadata_started
            ? metadata_finished - metadata_started : 0;
    /* Only a parser-blocking script can synchronously observe stylesheet
       state here. Rebuilding and refetching for defer/async/module elements
       is both unnecessary and harmful: a bounded stylesheet failure would
       otherwise be retried at every non-blocking closing tag while the
       top-level response's wall-clock deadline continues to run. */
    if (!parser_blocking) {
        navigation_stream_launch_preloads(stream);
        if (!navigation_stream_has_active_style_speculation(stream)) {
            navigation_stream_progress_preloads(stream, false);
        }
        uint64_t callback_finished = navigation_now_us();
        stream->session->performance.parser_callback_us +=
            callback_finished >= callback_started
                ? callback_finished - callback_started : 0;
        return true;
    }
    uint64_t runtime_started = metadata_finished;
    if (!navigation_stream_prepare_runtime(stream, true)) return false;
    uint64_t runtime_finished = navigation_now_us();
    stream->session->performance.parser_runtime_startup_us +=
        runtime_finished >= runtime_started
            ? runtime_finished - runtime_started : 0;
    if (stream->script_disabled_after_failure || stream->runtime == NULL) {
        uint64_t callback_finished = navigation_now_us();
        stream->session->performance.parser_callback_us +=
            callback_finished >= callback_started
                ? callback_finished - callback_started : 0;
        return true;
    }
    uint64_t stylesheet_started = runtime_finished;
    /* Snapshot parser-owned links before the first author callback.  Any
       stylesheet link inserted by that callback remains absent from this
       baseline and receives its eventual load/error event after commit. */
    navigation_stream_adopt_stylesheet_event_baseline(stream);
    uint64_t fingerprint_started = navigation_now_us();
    uint64_t fingerprint = 0;
    bool fingerprint_refused =
        navigation_test_consume_parser_checkpoint_fault(
            NAVIGATION_TEST_PARSER_CHECKPOINT_FINGERPRINT)
        || !navigation_stream_stylesheet_fingerprint(
               stream, document, &fingerprint);
    if (fingerprint_refused) {
        return navigation_stream_suppress_optional_parser_work(
            stream, NAVIGATION_PARSER_REFUSAL_FINGERPRINT);
    }
    uint64_t fingerprint_finished = navigation_now_us();
    session->performance.blocking_stylesheet_fingerprint_us +=
        fingerprint_finished >= fingerprint_started
            ? fingerprint_finished - fingerprint_started : 0;
    bool reuse = stream->blocking_stylesheet_ready
        && stream->blocking_stylesheet_fingerprint_valid
        && stream->blocking_stylesheet_fingerprint == fingerprint;
    if (reuse) {
        session->performance.blocking_stylesheet_reuses++;
    } else {
        bool continued = navigation_stream_try_continue_stylesheet(
            stream, document, fingerprint);
        bool stylesheet_refused = !continued
            && (navigation_test_consume_parser_checkpoint_fault(
                    NAVIGATION_TEST_PARSER_CHECKPOINT_STYLESHEET)
                || !navigation_stream_build_blocking_stylesheet(
                       stream, document, fingerprint));
        if (stylesheet_refused) {
            return navigation_stream_suppress_optional_parser_work(
                stream, NAVIGATION_PARSER_REFUSAL_STYLESHEET);
        }
    }
    script_runtime_set_stylesheet(stream->runtime,
                                  &stream->blocking_stylesheet);
    script_runtime_invalidate_document_base(stream->runtime);
    long element_handle = script_runtime_node_weak_handle(
        stream->runtime, element);
    if (element_handle == 0) {
        navigation_stream_record_skipped_parser_script(stream);
        return true;
    }
    NavigationParserScriptAllowance mutation_allowance = {0};
    NavigationParserScriptAllowanceResult mutation_allowance_result =
        navigation_stream_prepare_parser_script_allowance(
            stream, 0u,
            navigation_elapsed_us(callback_started, navigation_now_us()),
            &mutation_allowance);
    if (mutation_allowance_result
        != NAVIGATION_PARSER_SCRIPT_ALLOWANCE_READY) {
        return mutation_allowance_result
            == NAVIGATION_PARSER_SCRIPT_ALLOWANCE_SKIPPED;
    }
    ScriptRuntime *mutation_runtime = stream->runtime;
    uint64_t mutation_started = navigation_now_us();
    bool mutation_refused =
        navigation_test_consume_parser_checkpoint_fault(
            NAVIGATION_TEST_PARSER_CHECKPOINT_MUTATION)
        || !script_runtime_parser_mutation_checkpoint(mutation_runtime);
    navigation_stream_finish_parser_script_allowance(mutation_runtime);
    if (mutation_refused) {
        return navigation_stream_finish_parser_mutation_checkpoint(
            stream, mutation_started, mutation_allowance.deadline_us, true);
    }
    element = script_runtime_node_handle_resolve_connected(
        mutation_runtime, element_handle);
    if (element == NULL) {
        return navigation_stream_finish_parser_mutation_checkpoint(
            stream, mutation_started, mutation_allowance.deadline_us, false);
    }
    script_shape = navigation_parser_script_shape(element);
    if (!script_shape.parser_blocking) {
        return navigation_stream_finish_parser_mutation_checkpoint(
            stream, mutation_started, mutation_allowance.deadline_us, false);
    }
    parser_blocking = true;
    external = script_shape.external;
    inline_source_bytes = script_shape.inline_source_bytes;
    uint64_t stylesheet_finished = navigation_now_us();
    session->performance.parser_stylesheet_us +=
        stylesheet_finished >= stylesheet_started
            ? stylesheet_finished - stylesheet_started : 0;

    /* An external classic script may spend an appreciable interval waiting
       for its already-discovered response and then compiling/executing it.
       Once a closed-head body prefix and its stylesheet checkpoint are
       usable, present that immutable prefix before entering those blocking
       stages. Inline scripts have no network wait and usually execute more
       cheaply than a speculative layout, so painting before them would add
       work and author-visible flashing rather than improve first paint. */
    if (external && !stream->pre_eof_preview_attempted
        && !stream->pre_eof_preview_painted) {
        lxb_dom_node_t *body = document_body_node(document);
        if (body != NULL && body->first_child != NULL) {
            /* Entering the body insertion mode implicitly closes an omitted
               </head>. Record that parser fact so the ordinary preview gate
               need not special-case malformed-but-common document syntax. */
            stream->head_closed = true;
            session->performance.streaming_preview_pre_script_checks++;
            stream->preview_checkpoint_checks = 0;
            navigation_stream_try_pre_eof_preview(stream);
            if (stream->pre_eof_preview_painted) {
                session->performance.streaming_preview_pre_script_paints++;
            }
        }
    }
    ScriptRuntimeTelemetry telemetry_before = {0};
    script_runtime_telemetry(stream->runtime, &telemetry_before);
    size_t source_bytes_before = stream->scripts.early.source_work_bytes;
    size_t failed_before = stream->scripts.early.failed;
    size_t execution_failures_before =
        stream->scripts.early.execution_failures;
    uint64_t execution_us_before = stream->scripts.early.execution_us;
    size_t skipped_quota_before = stream->scripts.early.skipped_quota;
    size_t skipped_pressure_before = stream->scripts.early.skipped_pressure;
    uint64_t script_started = stylesheet_finished;
    NavigationParserScriptAllowance allowance = {0};
    NavigationParserScriptAllowanceResult allowance_result =
        navigation_stream_prepare_parser_script_allowance(
            stream, external ? 0u : inline_source_bytes,
            navigation_elapsed_us(callback_started, navigation_now_us()),
            &allowance);
    if (allowance_result != NAVIGATION_PARSER_SCRIPT_ALLOWANCE_READY) {
        return allowance_result == NAVIGATION_PARSER_SCRIPT_ALLOWANCE_SKIPPED;
    }
    if (external) {
        char source_reference[NAVIGATION_URL_LIMIT];
        if (navigation_parser_script_copy_source_reference(
                element, source_reference, sizeof(source_reference))) {
            navigation_stream_settle_script_preload(
                stream, source_reference, allowance.deadline_us);
            navigation_stream_finish_parser_script_allowance(stream->runtime);
            allowance_result =
                navigation_stream_prepare_parser_script_allowance(
                stream, external ? 0u : inline_source_bytes,
                navigation_elapsed_us(callback_started, navigation_now_us()),
                &allowance);
            if (allowance_result
                != NAVIGATION_PARSER_SCRIPT_ALLOWANCE_READY) {
                return allowance_result
                    == NAVIGATION_PARSER_SCRIPT_ALLOWANCE_SKIPPED;
            }
        }
    }
    ScriptRuntime *limited_runtime = stream->runtime;
    navigation_stream_capture_server_body(stream, document);
    DocumentScriptProcessResult process_result =
        document_scripts_process_closed(
            limited_runtime, session->budget, session->browser_session,
            stream->resource_base_url, stream->document_url,
            stream->referrer_policy,
            &stream->parser->document.content_security_policy,
            session->maximum_scripts, session->maximum_script_bytes,
            allowance.maximum_file_bytes, allowance.timeout_ms,
            stream->scheduler, element, &stream->scripts);
    bool processed = process_result == DOCUMENT_SCRIPT_PROCESS_COMPLETE;
    navigation_stream_finish_parser_script_allowance(limited_runtime);
    if (process_result == DOCUMENT_SCRIPT_PROCESS_HARD_FAILURE) return false;
    bool parser_refusal_pending =
        stream->pending_parser_refusal != NAVIGATION_PARSER_REFUSAL_NONE;
    if (parser_refusal_pending) processed = false;
    /* Parser-blocking work owns the scarce transport pool first. Once its
       checkpoint is satisfied, opportunistic preloads may use the remaining
       scheduler capacity without starving CSS or the blocking script. */
    if (processed) {
        navigation_stream_launch_preloads(stream);
        if (!navigation_stream_has_active_style_speculation(stream)) {
            navigation_stream_progress_preloads(stream, false);
        }
    }
    uint64_t callback_finished = navigation_now_us();
    ScriptRuntimeTelemetry telemetry_after = {0};
    script_runtime_telemetry(stream->runtime, &telemetry_after);
    if (parser_refusal_pending) {
        (void) navigation_stream_suppress_optional_parser_work(
            stream, stream->pending_parser_refusal);
    }
    uint64_t compile_us =
        telemetry_after.host_compile_total_us
            >= telemetry_before.host_compile_total_us
        ? telemetry_after.host_compile_total_us
            - telemetry_before.host_compile_total_us
        : 0;
    uint64_t process_us = navigation_elapsed_us(
        script_started, callback_finished);
    uint64_t execution_us = stream->scripts.early.execution_us
            >= execution_us_before
        ? stream->scripts.early.execution_us - execution_us_before : 0;
    size_t source_bytes =
        stream->scripts.early.source_work_bytes >= source_bytes_before
        ? stream->scripts.early.source_work_bytes - source_bytes_before : 0;
    if (!external && source_bytes == 0) {
        source_bytes = inline_source_bytes;
    }
    size_t mutations =
        telemetry_after.dom_mutations >= telemetry_before.dom_mutations
        ? telemetry_after.dom_mutations - telemetry_before.dom_mutations : 0;
    uint64_t host_callback_us =
        telemetry_after.host_callback_total_us
            >= telemetry_before.host_callback_total_us
        ? telemetry_after.host_callback_total_us
            - telemetry_before.host_callback_total_us : 0;
    NavigationBlockingScriptSample sample = {
        .ordinal = script_ordinal,
        .node_count = document == NULL ? 0 : document->node_count,
        .source_bytes = source_bytes,
        .dom_mutations = mutations,
        .total_us = navigation_elapsed_us(
            callback_started, callback_finished),
        .metadata_us = navigation_elapsed_us(
            metadata_started, metadata_finished),
        .runtime_startup_us = navigation_elapsed_us(
            runtime_started, runtime_finished),
        .stylesheet_us = navigation_elapsed_us(
            stylesheet_started, stylesheet_finished),
        .stylesheet_fingerprint_us = navigation_elapsed_us(
            fingerprint_started, fingerprint_finished),
        .process_us = process_us,
        .compile_us = compile_us,
        .host_callback_us = host_callback_us,
        .execute_us = execution_us,
        .external = external,
        .succeeded = processed
    };
    navigation_record_blocking_script_sample(session, &sample);
    session->performance.parser_script_us +=
        process_us;
    session->performance.parser_callback_us +=
        sample.total_us;
    size_t failure_count = stream->scripts.early.failed >= failed_before
        ? stream->scripts.early.failed - failed_before : 0u;
    bool script_failed = failure_count != 0u;
    bool execution_failed =
        stream->scripts.early.execution_failures > execution_failures_before;
    bool cumulative_work_exhausted =
        process_result == DOCUMENT_SCRIPT_PROCESS_SOURCE_LIMIT
        && allowance.cumulative_work_limited;
    bool individual_source_limit =
        process_result == DOCUMENT_SCRIPT_PROCESS_SOURCE_LIMIT
        && !allowance.cumulative_work_limited
        && !parser_refusal_pending;
    if (script_failed && stream->runtime != NULL) {
        script_runtime_preserve_first_error(
            stream->runtime, &stream->script_result);
    }
    bool completed_cleanly = processed && !script_failed
        && stream->scripts.early.skipped_quota == skipped_quota_before
        && stream->scripts.early.skipped_pressure == skipped_pressure_before;
    if (!navigation_stream_account_parser_script(
            stream, execution_us, source_bytes, execution_failed,
            completed_cleanly)) {
        return false;
    }
    if (cumulative_work_exhausted
        && !stream->parser_script_circuit_open
        && !navigation_stream_open_parser_script_circuit(
               stream,
               "Limited page: JavaScript stopped after bounded parser work",
               NAVIGATION_PARSER_SCRIPT_BREAKER_WORK)) {
        return false;
    }
    if (stream->parser_script_circuit_open) return true;
    if (processed) {
        /* This is the strongest parser checkpoint: the closing script has
           executed in order, and no later token has been consumed. Recheck
           the stylesheet fingerprint because the script itself may have
           changed a style/link element synchronously. */
        stream->preview_checkpoint_checks = 0;
        navigation_stream_try_pre_eof_preview(stream);
    }
    return processed || individual_source_limit
        || navigation_stream_suppress_optional_parser_work(
               stream, NAVIGATION_PARSER_REFUSAL_SCRIPT);
}
