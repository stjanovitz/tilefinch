bool js_rt_bridge_flush_synchronous_layout(DomBridge *bridge)
{
    if (bridge == NULL || bridge->synchronous_layout == NULL) return false;
    return bridge->synchronous_layout(bridge->synchronous_layout_opaque);
}

static FetchScheduler *bridge_fetch_scheduler_create(
    DomBridge *bridge, size_t maximum_reserved_bytes)
{
    if (bridge == NULL || bridge->budget == NULL) return NULL;
    FetchSchedulerDomain *domain = bridge->fetch_scheduler_domain;
    bool created_domain = domain == NULL;
    if (created_domain) {
        domain = fetch_scheduler_domain_create(
            bridge->budget, SCRIPT_RUNTIME_FETCH_CONCURRENCY);
        if (domain == NULL) return NULL;
    }
    FetchScheduler *scheduler = fetch_scheduler_create_in_domain(
        domain, SCRIPT_RUNTIME_FETCH_CONCURRENCY,
        maximum_reserved_bytes);
    if (scheduler == NULL && created_domain) {
        fetch_scheduler_domain_release(domain);
        return NULL;
    }
    if (created_domain) bridge->fetch_scheduler_domain = domain;
    /* The bridge still owns CORS, credentials, redirect policy and promise /
       script delivery. On PSP only each resulting immutable wire hop is
       produced by the bounded curl worker, preserving the shared domain's
       concurrency cap and all browser-thread observable ordering. */
    (void) fetch_scheduler_enable_background_transport(scheduler, true);
    return scheduler;
}

uintptr_t js_rt_node_owner_identity(const lxb_dom_node_t *node)
{
    return node == NULL ? 0 : (uintptr_t) node->owner_document;
}

static uintptr_t document_owner_identity(
    const PocDocument *document)
{
    return document == NULL || document->html == NULL
        ? 0 : (uintptr_t) &document->html->dom_document;
}

static ElementScrollIntent *bridge_scroll_intent_find(
    DomBridge *bridge, const lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < bridge->scroll_intent_count; i++) {
        if (bridge->scroll_intents[i].node == node) {
            return &bridge->scroll_intents[i];
        }
    }
    return NULL;
}

static ElementScrollIntent *bridge_scroll_intent_set(
    DomBridge *bridge, lxb_dom_node_t *node, int scroll_x, int scroll_y)
{
    if (bridge == NULL || node == NULL) return NULL;
    ElementScrollIntent *intent = bridge_scroll_intent_find(bridge, node);
    if (intent == NULL
        && bridge->scroll_intent_count < DOM_SCROLL_INTENT_LIMIT) {
        intent = &bridge->scroll_intents[bridge->scroll_intent_count++];
    } else if (intent == NULL) {
        size_t oldest = 0;
        for (size_t i = 1; i < bridge->scroll_intent_count; i++) {
            if (bridge->scroll_intents[i].sequence
                < bridge->scroll_intents[oldest].sequence) oldest = i;
        }
        intent = &bridge->scroll_intents[oldest];
    }
    uint32_t sequence = ++bridge->scroll_intent_sequence;
    if (sequence == 0) sequence = ++bridge->scroll_intent_sequence;
    *intent = (ElementScrollIntent) {
        .node = node,
        .owner_document_identity = js_rt_node_owner_identity(node),
        .sequence = sequence,
        .scroll_x = scroll_x < 0 ? 0 : scroll_x,
        .scroll_y = scroll_y < 0 ? 0 : scroll_y
    };
    return intent;
}

static void bridge_scroll_intents_detach_document(
    DomBridge *bridge, uintptr_t owner)
{
    if (bridge == NULL || owner == 0) return;
    size_t output = 0;
    for (size_t i = 0; i < bridge->scroll_intent_count; i++) {
        if (bridge->scroll_intents[i].owner_document_identity == owner) {
            continue;
        }
        if (output != i) {
            bridge->scroll_intents[output] = bridge->scroll_intents[i];
        }
        output++;
    }
    memset(bridge->scroll_intents + output, 0,
           (bridge->scroll_intent_count - output)
               * sizeof(bridge->scroll_intents[0]));
    bridge->scroll_intent_count = output;
}

ScriptElementState *js_rt_script_element_state_find(
    DomBridge *bridge, const lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < bridge->script_element_count; i++) {
        if (bridge->script_elements[i].node == node) {
            return &bridge->script_elements[i];
        }
    }
    return NULL;
}

ScriptElementState *js_rt_script_element_state_register(
    DomBridge *bridge, lxb_dom_node_t *node, bool html)
{
    ScriptElementState *state = js_rt_script_element_state_find(bridge, node);
    if (state != NULL) return state;
    if (bridge == NULL || node == NULL
        || bridge->script_element_count >= SCRIPT_DYNAMIC_NODE_LIMIT) {
        return NULL;
    }
    state = &bridge->script_elements[bridge->script_element_count++];
    uint32_t identity = ++bridge->next_script_element_identity;
    if (identity == 0) identity = ++bridge->next_script_element_identity;
    *state = (ScriptElementState) {
        .node = node,
        .owner_document_identity = js_rt_node_owner_identity(node),
        .identity = identity,
        .programmatic = true,
        .html = html,
        .force_async = html
    };
    return state;
}

bool js_rt_node_is_strict_descendant(const lxb_dom_node_t *node,
                                     const lxb_dom_node_t *ancestor)
{
    if (node == NULL || ancestor == NULL || node == ancestor) return false;
    size_t climbed = 0;
    for (const lxb_dom_node_t *at = node->parent;
         at != NULL && climbed++ < DOM_TRAVERSAL_VISIT_LIMIT;
         at = at->parent) {
        if (at == ancestor) return true;
    }
    return false;
}

void js_rt_script_element_states_mark_descendants(
    const DomBridge *bridge, const lxb_dom_node_t *ancestor,
    unsigned char marked[SCRIPT_DYNAMIC_NODE_LIMIT])
{
    memset(marked, 0, SCRIPT_DYNAMIC_NODE_LIMIT);
    if (bridge == NULL || ancestor == NULL) return;
    for (size_t i = 0; i < bridge->script_element_count; i++) {
        marked[i] = js_rt_node_is_strict_descendant(
            bridge->script_elements[i].node, ancestor);
    }
}

void js_rt_script_element_states_purge_marked(
    DomBridge *bridge,
    const unsigned char marked[SCRIPT_DYNAMIC_NODE_LIMIT])
{
    if (bridge == NULL || marked == NULL) return;
    size_t output = 0;
    for (size_t i = 0; i < bridge->script_element_count; i++) {
        if (marked[i]) continue;
        if (output != i) bridge->script_elements[output] =
            bridge->script_elements[i];
        output++;
    }
    memset(bridge->script_elements + output, 0,
           (bridge->script_element_count - output)
               * sizeof(bridge->script_elements[0]));
    bridge->script_element_count = output;
}

static void script_element_states_detach_document(
    DomBridge *bridge, const PocDocument *document)
{
    uintptr_t owner = document_owner_identity(document);
    if (bridge == NULL || owner == 0) return;
    size_t output = 0;
    for (size_t i = 0; i < bridge->script_element_count; i++) {
        if (bridge->script_elements[i].owner_document_identity == owner) {
            continue;
        }
        if (output != i) {
            bridge->script_elements[output] = bridge->script_elements[i];
        }
        output++;
    }
    memset(bridge->script_elements + output, 0,
           (bridge->script_element_count - output)
               * sizeof(bridge->script_elements[0]));
    bridge->script_element_count = output;
}

ScriptQuotaReserveResult js_rt_bridge_script_quota_reserve_bounded(
DomBridge *bridge, ScriptQuotaCountMode count_mode,
size_t requested_max_bytes, size_t reservation_ceiling,
ScriptQuotaReservation *reservation)
{
    if (bridge == NULL || reservation == NULL || reservation->active
        || (count_mode != SCRIPT_QUOTA_NEW_EXECUTABLE
            && count_mode != SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE)
        || requested_max_bytes == SIZE_MAX || reservation_ceiling == 0
        || reservation_ceiling == SIZE_MAX) {
        return SCRIPT_QUOTA_RESERVE_REJECTED;
    }
    if (count_mode == SCRIPT_QUOTA_NEW_EXECUTABLE
        && bridge->script_quota_count >= bridge->maximum_scripts) {
        js_rt_trace_script_quota_rejection(bridge, "count");
        return SCRIPT_QUOTA_RESERVE_REJECTED;
    }

    size_t desired = requested_max_bytes;
    if (desired > reservation_ceiling) desired = reservation_ceiling;
    if (desired != 0) {
        if (bridge->script_quota_bytes >= bridge->maximum_script_bytes) {
            js_rt_trace_script_quota_rejection(bridge, "source-bytes");
            return SCRIPT_QUOTA_RESERVE_REJECTED;
        }
        size_t logical_remaining = bridge->maximum_script_bytes
            - bridge->script_quota_bytes;
        if (desired > logical_remaining) desired = logical_remaining;
        if (bridge->script_quota_reserved_bytes > logical_remaining) {
            js_rt_trace_script_quota_rejection(
                bridge, "reservation-accounting");
            return SCRIPT_QUOTA_RESERVE_REJECTED;
        }
        size_t available = logical_remaining
            - bridge->script_quota_reserved_bytes;
        /* Reservations are temporary, while logical_remaining reflects
           source bytes already committed to the realm.  Starting an unknown
           network response with a limit reduced only by a peer reservation
           can reject a body that would fit as soon as that peer commits its
           smaller actual length.  Wait instead; after peer settlement the
           logical clamp above determines any genuinely permanent reduction. */
        if (desired > available) return SCRIPT_QUOTA_RESERVE_DEFERRED;
    }

    if (count_mode == SCRIPT_QUOTA_NEW_EXECUTABLE) {
        bridge->script_quota_count++;
    }
    bridge->script_quota_reserved_bytes += desired;
    reservation->reserved_bytes = desired;
    reservation->active = true;
    return SCRIPT_QUOTA_RESERVE_GRANTED;
}

ScriptQuotaReserveResult js_rt_bridge_script_quota_reserve(
DomBridge *bridge, ScriptQuotaCountMode count_mode,
size_t requested_max_bytes, ScriptQuotaReservation *reservation)
{
    return js_rt_bridge_script_quota_reserve_bounded(
        bridge, count_mode, requested_max_bytes,
        bridge == NULL ? 0 : bridge->maximum_script_file_bytes, reservation);
}

ScriptQuotaReserveResult js_rt_bridge_script_quota_reserve_known(
DomBridge *bridge, ScriptQuotaCountMode count_mode,
size_t exact_source_bytes, ScriptQuotaReservation *reservation)
{
    if (bridge == NULL || reservation == NULL || reservation->active
        || (count_mode != SCRIPT_QUOTA_NEW_EXECUTABLE
            && count_mode != SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE)
        || exact_source_bytes == SIZE_MAX
        || exact_source_bytes > bridge->maximum_script_bytes
        || (count_mode == SCRIPT_QUOTA_NEW_EXECUTABLE
            && bridge->script_quota_count >= bridge->maximum_scripts)
        || bridge->script_quota_bytes
               > bridge->maximum_script_bytes - exact_source_bytes
        || bridge->script_quota_reserved_bytes
               > SIZE_MAX - exact_source_bytes) {
        return SCRIPT_QUOTA_RESERVE_REJECTED;
    }
    /* A fresh cache hit has an exact, already-owned length and consumes no
       scheduler response reservation. Let it race an unknown network peer:
       whichever commits first wins the logical byte quota, while the loser
       fails closed at commit. This preserves as-ready classic-script order
       without permitting the sum of committed source bytes to exceed the
       realm limit. */
    if (count_mode == SCRIPT_QUOTA_NEW_EXECUTABLE) {
        bridge->script_quota_count++;
    }
    bridge->script_quota_reserved_bytes += exact_source_bytes;
    reservation->reserved_bytes = exact_source_bytes;
    reservation->active = true;
    return SCRIPT_QUOTA_RESERVE_GRANTED;
}

bool js_rt_bridge_script_quota_expand(
DomBridge *bridge, ScriptQuotaReservation *reservation,
size_t exact_source_bytes)
{
    if (bridge == NULL || reservation == NULL || !reservation->active
        || exact_source_bytes < reservation->reserved_bytes
        || exact_source_bytes > bridge->maximum_script_bytes
        || bridge->script_quota_bytes > bridge->maximum_script_bytes
        || bridge->script_quota_reserved_bytes
               > bridge->maximum_script_bytes - bridge->script_quota_bytes) {
        return false;
    }
    size_t additional = exact_source_bytes - reservation->reserved_bytes;
    size_t available = bridge->maximum_script_bytes
        - bridge->script_quota_bytes
        - bridge->script_quota_reserved_bytes;
    if (additional > available) return false;
    bridge->script_quota_reserved_bytes += additional;
    reservation->reserved_bytes = exact_source_bytes;
    return true;
}

bool js_rt_bridge_script_quota_commit(
DomBridge *bridge, ScriptQuotaReservation *reservation,
size_t actual_source_bytes)
{
    if (bridge == NULL || reservation == NULL || !reservation->active
        || actual_source_bytes > reservation->reserved_bytes
        || reservation->reserved_bytes > bridge->script_quota_reserved_bytes
        || actual_source_bytes > bridge->maximum_script_bytes
                              - bridge->script_quota_bytes) {
        return false;
    }
    bridge->script_quota_reserved_bytes -= reservation->reserved_bytes;
    bridge->script_quota_bytes += actual_source_bytes;
    memset(reservation, 0, sizeof(*reservation));
    return true;
}

void js_rt_bridge_script_quota_abort(
DomBridge *bridge, ScriptQuotaReservation *reservation)
{
    if (bridge == NULL || reservation == NULL || !reservation->active) return;
    if (reservation->reserved_bytes <= bridge->script_quota_reserved_bytes) {
        bridge->script_quota_reserved_bytes -= reservation->reserved_bytes;
    } else {
        bridge->script_quota_reserved_bytes = 0;
    }
    memset(reservation, 0, sizeof(*reservation));
}

static BudgetAllocationOwner bridge_document_owner_enter(
    const DomBridge *bridge)
{
    return bridge == NULL
        ? BUDGET_ALLOCATION_OWNER_NONE
        : document_allocation_owner_enter(bridge->document);
}

static void bridge_document_owner_leave(
    const DomBridge *bridge, BudgetAllocationOwner previous_owner)
{
    if (bridge == NULL) return;
    document_allocation_owner_leave(bridge->document, previous_owner);
}

typedef struct {
    bool enabled;
    uint64_t seed;
} DeterministicReplayConfig;

static DeterministicReplayConfig deterministic_replay;

bool script_execution_policy_for_profile(
    ScriptExecutionProfile profile, ScriptExecutionPolicy *policy)
{
    if (policy == NULL) return false;
    *policy = (ScriptExecutionPolicy) {
        .slow_compile_threshold_us = SCRIPT_DEFAULT_SLOW_DEPENDENCY_US,
        .slow_callback_threshold_us = SCRIPT_DEFAULT_SLOW_DEPENDENCY_US
    };
    switch (profile) {
    case SCRIPT_EXECUTION_PROFILE_LAB:
        return true;
    case SCRIPT_EXECUTION_PROFILE_PSP_STRICT:
    case SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC:
        /* The realistic profile has a measured 5 MiB realm ceiling while
           strict remains 4 MiB.  Keep the per-call compiler boundary
           identical: both must bound a non-preemptible compile to the same
           PSP-safe unit regardless of aggregate heap headroom. */
        policy->maximum_host_compile_source_bytes =
            SCRIPT_PSP_MAXIMUM_HOST_COMPILE_BYTES;
        return true;
    default:
        *policy = (ScriptExecutionPolicy) {0};
        return false;
    }
}

void script_runtime_configure_deterministic_replay(bool enabled,
                                                   unsigned long long seed)
{
    deterministic_replay.enabled = enabled;
    /* Zero is an explicit seed in the versioned derivation, not a sentinel. */
    deterministic_replay.seed = (uint64_t) seed;
}

static uint64_t replay_random_mix(uint64_t value)
{
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint64_t replay_entropy_hash_byte(uint64_t hash, uint8_t byte)
{
    return (hash ^ (uint64_t) byte) * UINT64_C(0x100000001b3);
}

static bool replay_document_url_exact(const char *url, size_t *length)
{
    if (length != NULL) *length = 0;
    if (url == NULL) return false;
    size_t size = 0;
    while (size < TILEFINCH_URL_SERIALIZED_LIMIT && url[size] != '\0') size++;
    if (size == 0 || size >= TILEFINCH_URL_SERIALIZED_LIMIT) return false;
    char normalized[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_normalize(url, normalized, sizeof(normalized))
        || strlen(normalized) != size || memcmp(normalized, url, size) != 0) {
        return false;
    }
    if (length != NULL) *length = size;
    return true;
}

uint64_t script_runtime_deterministic_entropy_state_v1(
    uint64_t seed, const char *serialized_document_url,
    ScriptDocumentScope scope)
{
    /* `splitmix64-url-scope-v1` hashes this exact byte stream with FNV-1a-64:
         ASCII contract label including its trailing NUL,
         seed as eight little-endian bytes,
         one scope byte (0 top-level, 1 child-frame),
         URL byte length as uint64 little-endian,
         serialized URL bytes excluding their trailing NUL.
       The standard SplitMix64 finalizer (without the stream gamma) avalanches
       the hash. This is allocation-free and independent of host endianness. */
    static const uint8_t contract[] = "splitmix64-url-scope-v1";
    size_t url_length = 0;
    if (scope != SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
        && scope != SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME) return 0;
    if (!replay_document_url_exact(
            serialized_document_url, &url_length)) return 0;
    const uint8_t *url = (const uint8_t *) serialized_document_url;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t index = 0; index < sizeof(contract); index++) {
        hash = replay_entropy_hash_byte(hash, contract[index]);
    }
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash = replay_entropy_hash_byte(hash, (uint8_t) (seed >> shift));
    }
    hash = replay_entropy_hash_byte(
        hash, scope == SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME ? 1u : 0u);
    uint64_t serialized_length = (uint64_t) url_length;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash = replay_entropy_hash_byte(
            hash, (uint8_t) (serialized_length >> shift));
    }
    for (size_t index = 0; index < url_length; index++) {
        hash = replay_entropy_hash_byte(hash, url[index]);
    }
    uint64_t state = replay_random_mix(hash);
    return state == 0 ? UINT64_C(1) : state;
}

static uint64_t replay_random_next(uint64_t *state)
{
    *state += UINT64_C(0x9e3779b97f4a7c15);
    return replay_random_mix(*state);
}

static uint64_t wall_time_ms(void);
static uint64_t monotonic_time_ms(void);
