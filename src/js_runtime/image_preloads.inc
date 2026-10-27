/* Detached HTMLImageElements are active resource clients, not layout nodes.
   Keep their small asynchronous lane separate from the connected image table
   so stylesheet/resource replacement cannot invalidate an in-flight job.
   All storage, encoded bytes and decoder scratch share the page Budget. */
/* The lane's image table: fetch attempts and encoded/decoded bytes. */
#define SCRIPT_IMAGE_PRELOAD_LIMIT 128u
#define SCRIPT_IMAGE_PRELOAD_BYTES (1024u * 1024u)
/* Images queued for or holding a load. Slots grow on demand up to this
   bound; an image past it (or past a refused growth) is queued for an
   error event instead, as a browser reports a fetch it could not start.
   Each queued error holds a distinct live node handle, so that queue never
   outgrows the handle table. */
#define SCRIPT_IMAGE_PRELOAD_SLOT_LIMIT 512u
#define SCRIPT_IMAGE_PRELOAD_SLOT_INITIAL 16u
#define SCRIPT_IMAGE_PRELOAD_ERROR_LIMIT DOM_BRIDGE_NODE_LIMIT_MAX
typedef struct {
    long handle;
    lxb_dom_node_t *node;
    JSValue owner;
    bool queued;
    bool event_pending;
    bool failed;
    bool finished;
    /* When the current run of refused job starts began (0: none yet). A
       queued slot waits behind earlier ones; that wait is not its timeout. */
    uint64_t start_attempt_ns;
} ScriptImagePreload;

typedef struct ScriptImagePreloads {
    ImageResources images;
    ImagePriorityLoadJob *job;
    size_t active;
    /* Entries detached from their node (set to NULL) while a job was open,
       or when the node was released; compacted once no job is open. */
    bool orphans;
    size_t slot_capacity;
    ScriptImagePreload *slots;
    /* Images awaiting their error event, in order: refused a slot, or
       `broken`: with nothing to select, or failed in the page's image
       table. Each names the source hash it is for, which a newer source
       supersedes. A connection hands an image refused a slot to the page's
       loader; a broken request's error fires connected or not. */
    struct {
        long handle;
        JSValue owner;
        uint64_t source_hash;
        bool broken;
    } *errors;
    size_t error_head;
    size_t error_count;
    size_t error_capacity;
    /* Images whose queued error was dispatched: broken, so complete, until
       their source changes. Room for every queued error is reserved when
       it is queued, so dispatching one never allocates. */
    long *broken;
    size_t broken_count;
    size_t broken_capacity;
} ScriptImagePreloads;

static void image_preload_cancel_job(ScriptRuntime *runtime)
{
    ScriptImagePreloads *loads = runtime->image_preloads;
    if (loads == NULL || loads->job == NULL) return;
    images_priority_load_destroy(loads->job);
    loads->job = NULL;
    loads->slots[loads->active].queued = true;
}

void script_runtime_pause_image_preloads(ScriptRuntime *runtime)
{
    if (runtime != NULL) image_preload_cancel_job(runtime);
}

/* Compacts the entries of slots cleared while a job was open. */
static void image_preloads_compact(ScriptImagePreloads *loads)
{
    if (loads->job != NULL || !loads->orphans) return;
    lxb_dom_node_t *orphan = NULL;
    images_discard_nodes(&loads->images, &orphan, 1u);
    loads->orphans = false;
}

static void image_preload_clear(ScriptRuntime *runtime, size_t at)
{
    ScriptImagePreloads *loads = runtime->image_preloads;
    ScriptImagePreload *slot = &loads->slots[at];
    /* Only this slot's own load ends here. The loader's rollback snapshot
       covers the table's count and stats, so while another slot's job is
       open this slot's older entries are only detached from their node (no
       lookup finds them) and compacted after that job ends. */
    if (loads->job != NULL && loads->active == at)
        image_preload_cancel_job(runtime);
    lxb_dom_node_t *node = slot->node;
    if (node != NULL && loads->job == NULL) {
        images_discard_nodes(&loads->images, &node, 1u);
    } else if (node != NULL) {
        for (size_t i = 0; i < loads->images.count; i++) {
            if (loads->images.items[i].node != node) continue;
            loads->images.items[i].node = NULL;
            loads->orphans = true;
        }
    }
    JSValue owner = slot->owner;
    *slot = (ScriptImagePreload) {.owner = JS_UNDEFINED};
    JS_FreeValue(runtime->context, owner);
}

static void image_preloads_destroy(ScriptRuntime *runtime)
{
    ScriptImagePreloads *loads = runtime->image_preloads;
    if (loads == NULL) return;
    image_preload_cancel_job(runtime);
    for (size_t i = 0; i < loads->slot_capacity; i++)
        image_preload_clear(runtime, i);
    for (size_t i = 0; i < loads->error_count; i++)
        JS_FreeValue(runtime->context,
                     loads->errors[loads->error_head + i].owner);
    images_destroy(&loads->images);
    budget_free(runtime->budget, loads->broken);
    budget_free(runtime->budget, loads->errors);
    budget_free(runtime->budget, loads->slots);
    budget_free(runtime->budget, loads);
    runtime->image_preloads = NULL;
}

const ImageResource *js_rt_image_preload_find(DomBridge *bridge,
                                             const lxb_dom_node_t *node)
{
    if (js_rt_image_preload_status(bridge, node) != 1) return NULL;
    return images_find_node(&bridge->host->image_preloads->images, node);
}

ImageResources *js_rt_image_preload_resources(DomBridge *bridge)
{
    return bridge == NULL || bridge->host == NULL
        || bridge->host->image_preloads == NULL ? NULL
        : &bridge->host->image_preloads->images;
}

void js_rt_image_preload_prepare_pin(DomBridge *bridge, size_t index)
{
    ScriptImagePreloads *loads = bridge == NULL || bridge->host == NULL
        ? NULL : bridge->host->image_preloads;
    /* Pinning only marks the surface unless it must be decoded again. Only
       that restore changes the stats the open job's rollback snapshot
       covers, so only it ends the job (which then restarts). Reading a
       pending or an already full-size image leaves the job running. */
    if (loads != NULL && loads->job != NULL
        && images_retarget_pin_full_restores(&loads->images, index))
        image_preload_cancel_job(bridge->host);
}

int js_rt_image_preload_status(DomBridge *bridge, const lxb_dom_node_t *node)
{
    if (bridge == NULL || bridge->host == NULL
        || bridge->host->image_preloads == NULL) return -1;
    ScriptRuntime *runtime = bridge->host;
    for (size_t i = 0; i < runtime->image_preloads->slot_capacity; i++) {
        ScriptImagePreload *slot = &runtime->image_preloads->slots[i];
        if (slot->handle != 0
            && script_runtime_node_handle_resolve(runtime, slot->handle) == node)
            return slot->finished ? (slot->failed ? 2 : 1) : 0;
    }
    return -1;
}

bool js_rt_image_preload_broken(DomBridge *bridge, const lxb_dom_node_t *node)
{
    if (bridge == NULL || bridge->host == NULL || node == NULL
        || bridge->host->image_preloads == NULL) return false;
    const ScriptImagePreloads *loads = bridge->host->image_preloads;
    for (size_t i = 0; i < loads->broken_count; i++)
        if (script_runtime_node_handle_resolve(bridge->host, loads->broken[i])
            == node) return true;
    return false;
}

/* Drops the broken marks of `node` (NULL: of released nodes). */
static void image_preload_forget_broken(ScriptRuntime *runtime,
                                        const lxb_dom_node_t *node)
{
    ScriptImagePreloads *loads = runtime->image_preloads;
    size_t kept = 0;
    for (size_t i = 0; i < loads->broken_count; i++) {
        lxb_dom_node_t *marked = script_runtime_node_handle_resolve(
            runtime, loads->broken[i]);
        if (marked != NULL && marked != node)
            loads->broken[kept++] = loads->broken[i];
    }
    loads->broken_count = kept;
}

/* A delivered slot (not queued, not awaiting its event, not loading)
   whose wrapper has been collected; only it may be cleared early. */
static bool image_preload_slot_dead(ScriptRuntime *runtime, size_t at)
{
    ScriptImagePreloads *loads = runtime->image_preloads;
    ScriptImagePreload *slot = &loads->slots[at];
    DomBridge *bridge = &runtime->bridge;
    if (slot->handle == 0 || slot->queued || slot->event_pending
        || (loads->job != NULL && loads->active == at)) return false;
    size_t native_slot = 0;
    JSValue owner = JS_UNDEFINED;
    if (js_rt_bridge_node_slot_for_handle(bridge, slot->handle, &native_slot)
        && bridge->wrapper_refs != NULL)
        owner = JS_WeakRefDeref(runtime->context, bridge->wrapper_refs[native_slot]);
    bool live = JS_IsObject(owner);
    JS_FreeValue(runtime->context, owner);
    return !live;
}

/* Queues the error event of an image for its source `source_hash`;
   takes `owner`. An image already queued keeps its one event, now for that
   source. False on a refused growth. */
static bool image_preload_queue_error(ScriptRuntime *runtime, long handle,
                                      JSValue owner, uint64_t source_hash,
                                      bool broken)
{
    ScriptImagePreloads *loads = runtime->image_preloads;
    for (size_t i = 0; i < loads->error_count; i++) {
        if (loads->errors[loads->error_head + i].handle != handle) continue;
        loads->errors[loads->error_head + i].source_hash = source_hash;
        loads->errors[loads->error_head + i].broken = broken;
        if (JS_IsObject(owner)
            && !JS_IsObject(loads->errors[loads->error_head + i].owner)) {
            loads->errors[loads->error_head + i].owner = owner;
            owner = JS_UNDEFINED;
        }
        JS_FreeValue(runtime->context, owner);
        return true;
    }
    if (loads->error_head + loads->error_count == loads->error_capacity
        && loads->error_head != 0) {
        memmove(loads->errors, loads->errors + loads->error_head,
                loads->error_count * sizeof(*loads->errors));
        loads->error_head = 0;
    }
    /* Reserve the broken mark this error leaves once dispatched. Each
       mark and queued error holds a distinct live handle; the marks of
       released nodes are dropped only when that room is needed. */
    if (loads->broken_count + loads->error_count == loads->broken_capacity)
        image_preload_forget_broken(runtime, NULL);
    if (loads->broken_count + loads->error_count == loads->broken_capacity) {
        size_t capacity = loads->broken_capacity == 0
            ? SCRIPT_IMAGE_PRELOAD_SLOT_INITIAL : loads->broken_capacity * 2u;
        if (capacity > SCRIPT_IMAGE_PRELOAD_ERROR_LIMIT)
            capacity = SCRIPT_IMAGE_PRELOAD_ERROR_LIMIT;
        long *broken = capacity <= loads->broken_capacity ? NULL
            : budget_realloc_category(runtime->budget,
                  BUDGET_CATEGORY_JAVASCRIPT, loads->broken,
                  capacity * sizeof(*loads->broken));
        if (broken == NULL) {
            JS_FreeValue(runtime->context, owner);
            return false;
        }
        loads->broken = broken;
        loads->broken_capacity = capacity;
    }
    if (loads->error_count == loads->error_capacity) {
        size_t capacity = loads->error_capacity == 0
            ? SCRIPT_IMAGE_PRELOAD_SLOT_INITIAL : loads->error_capacity * 2u;
        if (capacity > SCRIPT_IMAGE_PRELOAD_ERROR_LIMIT)
            capacity = SCRIPT_IMAGE_PRELOAD_ERROR_LIMIT;
        void *errors = capacity <= loads->error_capacity ? NULL
            : budget_realloc_category(runtime->budget,
                  BUDGET_CATEGORY_JAVASCRIPT, loads->errors,
                  capacity * sizeof(*loads->errors));
        if (errors == NULL) {
            JS_FreeValue(runtime->context, owner);
            return false;
        }
        loads->errors = errors;
        loads->error_capacity = capacity;
    }
    size_t at = loads->error_head + loads->error_count++;
    loads->errors[at].handle = handle;
    loads->errors[at].owner = owner;
    loads->errors[at].source_hash = source_hash;
    loads->errors[at].broken = broken;
    return true;
}

/* The lane, made on first use; NULL when the Budget refuses it. */
static ScriptImagePreloads *image_preloads_open(ScriptRuntime *runtime)
{
    if (runtime->image_preloads == NULL)
        runtime->image_preloads = budget_calloc_category(
            runtime->budget, BUDGET_CATEGORY_JAVASCRIPT, 1u,
            sizeof(*runtime->image_preloads));
    return runtime->image_preloads;
}

/* The live wrapper of `handle`, held until its task (undefined without
   one). Never reconstruct author state from a handle. */
static JSValue image_preload_owner(ScriptRuntime *runtime, long handle)
{
    DomBridge *bridge = &runtime->bridge;
    size_t native_slot = 0;
    if (!js_rt_bridge_node_slot_for_handle(bridge, handle, &native_slot)
        || bridge->wrapper_refs == NULL) return JS_UNDEFINED;
    JSValue owner = JS_WeakRefDeref(runtime->context,
                                    bridge->wrapper_refs[native_slot]);
    if (JS_IsObject(owner)) return owner;
    JS_FreeValue(runtime->context, owner);
    return JS_UNDEFINED;
}

/* An image with nothing to select fires error when it has a src or srcset
   to select from (an empty src); one with neither (its src removed) fires
   nothing. */
static bool image_preload_empty_source_errs(lxb_dom_node_t *node)
{
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    return lxb_dom_element_has_attribute(
               element, (const lxb_char_t *) "src", 3)
        || lxb_dom_element_has_attribute(
               element, (const lxb_char_t *) "srcset", 6);
}

/* Queues the error events of the page image table's failed <img>
   elements whose source is still the one that failed, once per request:
   an image already broken or queued for it is not queued again. */
static void image_queue_page_failures(ScriptRuntime *runtime,
                                      const ImageResources *images)
{
    DomBridge *bridge = &runtime->bridge;
    for (size_t i = 0; i < images->failure_count; i++) {
        lxb_dom_node_t *node = images->failures[i].node;
        uint64_t hash = images->failures[i].source_hash;
        if (node == NULL) continue;
        size_t length = 0;
        const char *source = image_select_source(
            bridge->stylesheet, node, &length);
        if (image_source_hash(source, length) != hash
            || ((source == NULL || length == 0)
                && !image_preload_empty_source_errs(node))
            || image_resource_available(images_find_node(images, node))
            || js_rt_image_preload_broken(bridge, node)) continue;
        long handle = (long) js_rt_bridge_find_node_handle(bridge, node);
        if (handle == 0) handle = script_runtime_node_weak_handle(runtime, node);
        if (handle == 0 || image_preloads_open(runtime) == NULL) continue;
        ScriptImagePreloads *loads = runtime->image_preloads;
        bool queued = false;
        for (size_t at = 0; at < loads->error_count && !queued; at++)
            queued = loads->errors[loads->error_head + at].handle == handle
                && loads->errors[loads->error_head + at].source_hash == hash;
        if (queued) continue;
        (void) image_preload_queue_error(
            runtime, handle, image_preload_owner(runtime, handle), hash, true);
    }
}

/* A free slot index, reclaiming a dead delivered one or growing the lane;
   SIZE_MAX when the lane is at its bound or growth is refused. */
static size_t image_preload_slot_take(ScriptRuntime *runtime)
{
    ScriptImagePreloads *loads = runtime->image_preloads;
    for (size_t i = 0; i < loads->slot_capacity; i++)
        if (loads->slots[i].handle == 0) return i;
    /* A full lane first reclaims a delivered slot whose wrapper is gone,
       as the next pump would. */
    for (size_t i = 0; i < loads->slot_capacity; i++) {
        if (!image_preload_slot_dead(runtime, i)) continue;
        image_preload_clear(runtime, i);
        return i;
    }
    size_t capacity = loads->slot_capacity == 0
        ? SCRIPT_IMAGE_PRELOAD_SLOT_INITIAL : loads->slot_capacity * 2u;
    if (capacity > SCRIPT_IMAGE_PRELOAD_SLOT_LIMIT)
        capacity = SCRIPT_IMAGE_PRELOAD_SLOT_LIMIT;
    if (capacity <= loads->slot_capacity) return SIZE_MAX;
    ScriptImagePreload *slots = budget_realloc_category(
        runtime->budget, BUDGET_CATEGORY_JAVASCRIPT, loads->slots,
        capacity * sizeof(*slots));
    if (slots == NULL) return SIZE_MAX;
    size_t at = loads->slot_capacity;
    for (size_t i = at; i < capacity; i++)
        slots[i] = (ScriptImagePreload) {.owner = JS_UNDEFINED};
    loads->slots = slots;
    loads->slot_capacity = capacity;
    return at;
}

bool js_rt_image_preload_changed(DomBridge *bridge, lxb_dom_node_t *node)
{
    if (bridge == NULL || bridge->host == NULL || node == NULL
        || node->ns != LXB_NS_HTML || node->local_name != LXB_TAG_IMG) return true;
    ScriptRuntime *runtime = bridge->host;
    ScriptImagePreloads *loads = runtime->image_preloads;
    size_t at = SIZE_MAX;
    if (loads != NULL) {
        image_preload_forget_broken(runtime, node);
        for (size_t i = 0; i < loads->slot_capacity; i++) {
            if (script_runtime_node_handle_resolve(runtime, loads->slots[i].handle)
                == node) { at = i; break; }
        }
        if (at != SIZE_MAX) image_preload_clear(runtime, at);
    }
    size_t source_length = 0;
    const char *source = image_select_source(bridge->stylesheet, node, &source_length);
    bool empty = source == NULL || source_length == 0;
    /* A connected image with a source loads through the page's table. */
    if (empty ? !image_preload_empty_source_errs(node)
              : bridge_node_is_connected(node)) return true;
    /* Hold the actual wrapper until its completion task. A raw native pin
       would not retain its event handlers/expandos and can overlap another
       native owner's pin. */
    long handle = script_runtime_node_weak_handle(runtime, node);
    JSValue owner = handle == 0 ? JS_UNDEFINED
        : image_preload_owner(runtime, handle);
    if (!JS_IsObject(owner)) return true;
    if (image_preloads_open(runtime) == NULL) {
        JS_FreeValue(runtime->context, owner);
        JS_ThrowOutOfMemory(runtime->context);
        return false;
    }
    loads = runtime->image_preloads;
    uint64_t source_hash = image_current_source_hash(bridge->stylesheet, node);
    /* Nothing to fetch: the request is broken at once, its error queued. */
    if (!empty && at == SIZE_MAX) at = image_preload_slot_take(runtime);
    if (empty || at == SIZE_MAX) {
        if (image_preload_queue_error(runtime, handle, owner, source_hash,
                                      empty)) return true;
        JS_ThrowOutOfMemory(runtime->context);
        return false;
    }
    loads->slots[at] = (ScriptImagePreload) {
        .handle = handle, .node = node, .owner = owner, .queued = true};
    return true;
}

void js_rt_image_preload_forget_released(DomBridge *bridge)
{
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    ScriptImagePreloads *loads = runtime == NULL ? NULL : runtime->image_preloads;
    if (loads == NULL) return;
    /* A slot whose handle no longer resolves names a node that is gone or
       about to be freed, and its address may be reused by a new image
       before the next pump. Forget the pointer now, by identity only (it is
       never dereferenced): entries keyed by a reused address would answer
       for the new image, and a later clear of the stale slot would discard
       the new image's entries. */
    for (size_t i = 0; i < loads->slot_capacity; i++) {
        ScriptImagePreload *slot = &loads->slots[i];
        if (slot->node == NULL
            || js_rt_bridge_node_slot_for_handle(bridge, slot->handle, NULL))
            continue;
        if (loads->job != NULL && loads->active == i)
            image_preload_cancel_job(runtime);
        for (size_t at = 0; at < loads->images.count; at++) {
            if (loads->images.items[at].node != slot->node) continue;
            loads->images.items[at].node = NULL;
            loads->orphans = true;
        }
        slot->node = NULL;
    }
}

static size_t image_preloads_pending(const ScriptRuntime *runtime)
{
    const ScriptImagePreloads *loads = runtime->image_preloads;
    size_t pending = loads != NULL && loads->job != NULL;
    if (loads != NULL) pending += loads->error_count;
    if (loads != NULL) for (size_t i = 0; i < loads->slot_capacity; i++)
        pending += loads->slots[i].queued || loads->slots[i].event_pending;
    return pending;
}

static void image_preloads_pump(ScriptRuntime *runtime)
{
    ScriptImagePreloads *loads = runtime->image_preloads;
    DomBridge *bridge = &runtime->bridge;
    if (loads == NULL || bridge->document == NULL || bridge->stylesheet == NULL) return;
    bool sweep = true;
#ifndef __PSP__
    sweep = !tilefinch_test_faults()->defer_image_preload_sweep;
#endif
    for (size_t i = 0; sweep && i < loads->slot_capacity; i++) {
        if (image_preload_slot_dead(runtime, i)) image_preload_clear(runtime, i);
    }
    image_preloads_compact(loads);
    if (loads->job == NULL) {
        for (size_t i = 0; i < loads->slot_capacity; i++) {
            ScriptImagePreload *slot = &loads->slots[i];
            if (!slot->queued) continue;
            lxb_dom_node_t *node = script_runtime_node_handle_resolve(runtime, slot->handle);
            if (node == NULL) { image_preload_clear(runtime, i); continue; }
            if (bridge_node_is_connected(node)) {
                image_preload_clear(runtime, i);
                continue;
            }
            /* A busy decoder defers the start; it is not this load failing. */
            if (image_decode_busy()) return;
            ImagePriorityTarget target = {.node = node,
                .weak_handle = slot->handle, .display_width = 1u,
                .display_height = 1u, .kind = IMAGE_PRIORITY_KIND_PRELOAD};
            const char *base = js_rt_bridge_calculated_base_url(bridge);
            size_t refusals = runtime->budget->failure_count;
            FetchScheduler *scheduler = script_runtime_fetch_scheduler(runtime);
            loads->job = images_priority_load_begin(bridge->document,
                (Stylesheet *) bridge->stylesheet, &loads->images, &target,
                runtime->budget, base, bridge->document_url, bridge->referrer_policy,
                SCRIPT_IMAGE_PRELOAD_LIMIT, SCRIPT_IMAGE_PRELOAD_BYTES,
                SCRIPT_IMAGE_PRELOAD_BYTES, SCRIPT_IMAGE_PRELOAD_BYTES, runtime->timeout_ms,
                scheduler, bridge->session);
            if (loads->job == NULL) {
                /* The timeout runs from this slot's first refused start, not
                   from its enqueue behind earlier slots. */
                uint64_t now = js_rt_monotonic_time_ns();
                if (slot->start_attempt_ns == 0) slot->start_attempt_ns = now | 1u;
                if (runtime->budget->failure_count != refusals
                    || (now >= slot->start_attempt_ns
                        && now - slot->start_attempt_ns
                        >= (uint64_t) runtime->timeout_ms * UINT64_C(1000000))) {
                    slot->queued = false;
                    slot->failed = slot->finished = slot->event_pending = true;
                }
                return;
            }
            loads->active = i;
            slot->start_attempt_ns = 0;
            slot->queued = false;
            break;
        }
    }
    if (loads->job == NULL) return;
    lxb_dom_node_t *active_node = script_runtime_node_handle_resolve(
        runtime, loads->slots[loads->active].handle);
    if (active_node == NULL || bridge_node_is_connected(active_node)) {
        image_preload_clear(runtime, loads->active);
        return;
    }
    ImagePriorityLoadStatus status = images_priority_load_pump(loads->job);
    if (status == IMAGE_PRIORITY_LOAD_PENDING) return;
    size_t active = loads->active;
    ScriptImagePreload *slot = &loads->slots[active];
    lxb_dom_node_t *node = script_runtime_node_handle_resolve(runtime, slot->handle);
    bool loaded = status == IMAGE_PRIORITY_LOAD_COMPLETE
        && image_resource_available(images_find_node(&loads->images, node));
    images_priority_load_commit_progress(loads->job);
    images_priority_load_destroy(loads->job);
    loads->job = NULL;
    image_preloads_compact(loads);
    slot->failed = !loaded;
    slot->finished = true;
    slot->event_pending = true;
}

/* Fires one image's load or error event; takes `owner`. */
static bool image_preload_fire(ScriptRuntime *runtime, long handle,
                               bool failed, JSValue owner)
{
    JSValue args[2] = {JS_NewInt64(runtime->context, handle),
        JS_NewString(runtime->context, failed ? "error" : "load")};
    bool ok = !JS_IsException(args[1]);
    if (ok) ok = runtime_call_captured(
        runtime, runtime->host_callbacks[SCRIPT_HOST_DISPATCH_HANDLE],
        "image preload", 2, args, NULL);
    else js_rt_record_exception(runtime->context, &runtime->result);
    JS_FreeValue(runtime->context, args[0]);
    JS_FreeValue(runtime->context, args[1]);
    JS_FreeValue(runtime->context, owner);
    return ok;
}

static bool image_preload_dispatch(ScriptRuntime *runtime, bool *dispatched)
{
    ScriptImagePreloads *loads = runtime->image_preloads;
    *dispatched = false;
    if (loads == NULL) return true;
    while (loads->error_count != 0) {
        long handle = loads->errors[loads->error_head].handle;
        JSValue owner = loads->errors[loads->error_head].owner;
        uint64_t source_hash = loads->errors[loads->error_head].source_hash;
        bool broken = loads->errors[loads->error_head].broken;
        loads->error_head++;
        if (--loads->error_count == 0) loads->error_head = 0;
        /* A newer source, a later assignment that got a slot, or (for an
           image with a source refused a slot) a connection owns the
           image's events now. */
        lxb_dom_node_t *node = script_runtime_node_handle_resolve(runtime, handle);
        if (node == NULL
            || image_current_source_hash(runtime->bridge.stylesheet, node)
                   != source_hash
            || (!broken && bridge_node_is_connected(node))
            || js_rt_image_preload_status(&runtime->bridge, node) != -1) {
            JS_FreeValue(runtime->context, owner);
            continue;
        }
        /* The request is broken from its error event on. */
        if (loads->broken_count < loads->broken_capacity)
            loads->broken[loads->broken_count++] = handle;
        *dispatched = true;
        return image_preload_fire(runtime, handle, true, owner);
    }
    for (size_t i = 0; i < loads->slot_capacity; i++) {
        ScriptImagePreload *slot = &loads->slots[i];
        if (!slot->event_pending) continue;
        slot->event_pending = false;
        JSValue owner = slot->owner;
        slot->owner = JS_UNDEFINED;
        *dispatched = true;
        return image_preload_fire(runtime, slot->handle, slot->failed, owner);
    }
    return true;
}
