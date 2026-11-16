/* A live NodeList: Node.childNodes. `__tilefinchLiveNodeList(owner, source,
   prototype, wrap)` makes one over a native node's children (source: the node's
   handle, or 0 for the realm's document) or over what a bootstrap function
   returns each time (a script-side tree: source is called with no
   arguments and returns an array). The list is an exotic object, so an
   index or `length` read sees the tree as it is now, without a Proxy and
   without copying the children; it holds `owner` (the node's wrapper),
   which keeps a native node's handle valid. `wrap` is the bootstrap's
   handle-to-wrapper function (the host's copy is captured only after the
   bootstrap, and compat.js reads document.childNodes before that).

   A native list remembers the last child it reached, while the DOM version
   stands, and steps to an index from whichever of the first child, that
   cursor and the last child (once the length is counted) is nearest, so
   walking it by index is linear in either direction. */

static JSClassID live_node_list_class_id;

typedef struct {
    JSValue owner;
    JSValue read;       /* a function, or undefined for a native list */
    JSValue wrap;
    int64_t handle;     /* 0: the realm's document */
    JSAtom length_atom;
    /* The cache: valid while `parent` is still the resolved parent and the
       DOM version is `version`. `length` is UINT32_MAX until counted, and
       stands only while `last` is still the last child: the parser appends
       children between scripts without moving the version. */
    const lxb_dom_node_t *parent;
    const lxb_dom_node_t *last;
    lxb_dom_node_t *node;
    uint64_t version;
    size_t version_nodes;
    uint32_t index;
    uint32_t length;
} LiveNodeList;

static void live_node_list_finalizer(JSRuntime *runtime, JSValue value)
{
    LiveNodeList *list = JS_GetOpaque(value, live_node_list_class_id);
    if (list == NULL) return;
    JS_FreeValueRT(runtime, list->owner);
    JS_FreeValueRT(runtime, list->read);
    JS_FreeValueRT(runtime, list->wrap);
    JS_FreeAtomRT(runtime, list->length_atom);
    js_free_rt(runtime, list);
}

static void live_node_list_mark(JSRuntime *runtime, JSValueConst value,
                                JS_MarkFunc *mark)
{
    LiveNodeList *list = JS_GetOpaque(value, live_node_list_class_id);
    if (list == NULL) return;
    JS_MarkValue(runtime, list->owner, mark);
    JS_MarkValue(runtime, list->read, mark);
    JS_MarkValue(runtime, list->wrap, mark);
}

/* The native parent, with the cache dropped if it no longer describes it.
   NULL: the node is gone (an empty list). */
static lxb_dom_node_t *live_node_list_parent(JSContext *context,
                                             LiveNodeList *list)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->document == NULL) return NULL;
    lxb_dom_node_t *parent = NULL;
    size_t slot = 0;
    if (list->handle == 0) {
        if (bridge->document->html != NULL)
            parent = lxb_dom_interface_node(bridge->document->html);
    } else if (js_rt_bridge_node_slot_for_handle(bridge, list->handle,
                                                 &slot)) {
        parent = bridge->nodes[slot];
    }
    js_rt_bridge_note_parser_writes(bridge);
    size_t nodes = bridge->document->node_count;
    if (parent != list->parent || bridge->dom_version != list->version
        || nodes != list->version_nodes) {
        list->parent = parent;
        list->version = bridge->dom_version;
        list->version_nodes = nodes;
        list->node = NULL;
        list->index = 0;
        list->length = UINT32_MAX;
    }
    if (parent != NULL && parent->last_child != list->last) {
        list->last = parent->last_child;
        list->length = UINT32_MAX;
    }
    return parent;
}

/* What a script-side list's function returns now; the caller frees it. */
static JSValue live_node_list_read(JSContext *context,
                                   const LiveNodeList *list,
                                   uint32_t *length)
{
    *length = 0;
    JSValue values = JS_Call(context, list->read, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(values) || !JS_IsObject(values)) return values;
    JSValue count = JS_GetProperty(context, values, list->length_atom);
    if (JS_IsException(count)
        || JS_ToUint32(context, length, count) < 0
        || !collection_length_valid(context, *length)) {
        JS_FreeValue(context, count);
        JS_FreeValue(context, values);
        return JS_EXCEPTION;
    }
    JS_FreeValue(context, count);
    return values;
}

/* The list's length, or -1 with an exception pending. */
static int64_t live_node_list_length(JSContext *context, LiveNodeList *list)
{
    if (!JS_IsUndefined(list->read)) {
        uint32_t length = 0;
        JSValue values = live_node_list_read(context, list, &length);
        if (JS_IsException(values)) return -1;
        JS_FreeValue(context, values);
        return length;
    }
    const lxb_dom_node_t *parent = live_node_list_parent(context, list);
    if (parent == NULL) return 0;
    if (list->length == UINT32_MAX) {
        uint32_t count = 0, visited = 0;
        const lxb_dom_node_t *child = parent->first_child;
        for (; child != NULL
             && visited++ < SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX + 1u;
             child = child->next)
            if (!document_node_is_shadow_carrier(child)) count++;
        /* A truncated raw-sibling walk is not an exact visible count.
           Keep the counting-cap sentinel so indexing cannot start from
           the real last child using an incomplete light-DOM length. */
        list->length = child == NULL ? count
            : SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX + 1u;
    }
    return list->length;
}

/* A shadow root's native carrier is not a light-DOM child. The same
   filtered walk serves ordinary nodes and hosts, without JS array copies. */
static lxb_dom_node_t *live_node_list_visible(lxb_dom_node_t *child,
                                              bool forward)
{
    for (size_t skipped = 0; child != NULL
             && document_node_is_shadow_carrier(child); skipped++) {
        if (skipped == SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX + 1u) return NULL;
        child = forward ? child->next : child->prev;
    }
    return child;
}

/* The node at `index`: undefined past the end, or an exception. */
static JSValue live_node_list_item(JSContext *context, LiveNodeList *list,
                                   uint32_t index)
{
    if (!JS_IsUndefined(list->read)) {
        uint32_t length = 0;
        JSValue values = live_node_list_read(context, list, &length);
        if (JS_IsException(values)) return values;
        JSValue item = index < length
            ? JS_GetPropertyUint32(context, values, index) : JS_UNDEFINED;
        JS_FreeValue(context, values);
        return item;
    }
    lxb_dom_node_t *parent = live_node_list_parent(context, list);
    if (parent == NULL || index > SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX
        || (list->length != UINT32_MAX && index >= list->length))
        return JS_UNDEFINED;
    lxb_dom_node_t *child = parent->first_child;
    bool from_first = true;
    uint32_t at = 0, distance = index;
    if (list->node != NULL) {
        uint32_t from_cursor = index >= list->index
            ? index - list->index : list->index - index;
        if (from_cursor < distance) {
            child = list->node;
            from_first = false; /* The cached cursor is already visible. */
            at = list->index;
            distance = from_cursor;
        }
    }
    /* The length is exact below the counting cap. */
    if (list->length <= SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX
        && list->length - 1u - index < distance) {
        child = live_node_list_visible(parent->last_child, false);
        from_first = false;
        at = list->length - 1u;
    }
    /* Filtering examines attributes. Do it only at the chosen endpoint,
       never at an unused first child before taking the cached cursor. */
    if (from_first) child = live_node_list_visible(child, true);
    uint32_t steps = 0;
    for (; child != NULL && at < index; at++, steps++)
        child = live_node_list_visible(child->next, true);
    for (; child != NULL && at > index; at--, steps++)
        child = live_node_list_visible(child->prev, false);
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge != NULL && bridge->result != NULL)
        js_rt_saturating_add_size(&bridge->result->node_list_index_steps,
                                  steps);
    if (child == NULL) return JS_UNDEFINED;
    list->node = child;
    list->index = index;
    /* Wrapping may run script (a custom element's upgrade), which may
       change the tree; the cache is checked again on the next read. */
    int64_t handle = js_rt_bridge_register_node(
        JS_GetContextOpaque(context), child);
    if (handle == 0)
        return JS_ThrowRangeError(context, "DOM node handles are exhausted");
    JSValue argument = JS_NewInt64(context, handle);
    JSValue wrapped = JS_Call(context, list->wrap, JS_UNDEFINED, 1,
                              &argument);
    return wrapped;
}

/* An atom's array index, or a negative number. */
static int64_t live_node_list_atom_index(JSContext *context, JSAtom atom)
{
    return legacy_all_atom_index(context, atom);
}

static int live_node_list_get_own_property(JSContext *context,
                                           JSPropertyDescriptor *descriptor,
                                           JSValueConst object, JSAtom atom)
{
    LiveNodeList *list = JS_GetOpaque(object, live_node_list_class_id);
    if (list == NULL) return 0;
    JSValue value;
    int flags = JS_PROP_CONFIGURABLE;
    if (atom == list->length_atom) {
        int64_t length = live_node_list_length(context, list);
        if (length < 0) return -1;
        value = JS_NewInt64(context, length);
    } else {
        int64_t index = live_node_list_atom_index(context, atom);
        if (index < 0) return 0;
        value = live_node_list_item(context, list, (uint32_t) index);
        if (JS_IsException(value)) return -1;
        if (JS_IsUndefined(value)) return 0;
        flags |= JS_PROP_ENUMERABLE;
    }
    if (descriptor != NULL) {
        descriptor->flags = flags;
        descriptor->value = value;
        descriptor->getter = JS_UNDEFINED;
        descriptor->setter = JS_UNDEFINED;
    } else {
        JS_FreeValue(context, value);
    }
    return 1;
}

static int live_node_list_get_own_property_names(JSContext *context,
                                                 JSPropertyEnum **table,
                                                 uint32_t *length,
                                                 JSValueConst object)
{
    *table = NULL;
    *length = 0;
    LiveNodeList *list = JS_GetOpaque(object, live_node_list_class_id);
    if (list == NULL) return 0;
    int64_t count = live_node_list_length(context, list);
    if (count < 0) return -1;
    JSPropertyEnum *entries = collection_key_table(context, count, 1);
    if (entries == NULL) return -1;
    for (int64_t i = 0; i < count; i++) {
        entries[i].is_enumerable = 1;
        entries[i].atom = JS_NewAtomUInt32(context, (uint32_t) i);
    }
    entries[count].is_enumerable = 0;
    entries[count].atom = JS_DupAtom(context, list->length_atom);
    *table = entries;
    *length = (uint32_t) count + 1;
    return 0;
}

/* The indices and length are read-only views of the tree: a write to one
   is ignored (a TypeError in strict mode); any other property is an
   ordinary expando. */
static int live_node_list_define_own_property(JSContext *context,
                                              JSValueConst object,
                                              JSAtom atom, JSValueConst value,
                                              JSValueConst getter,
                                              JSValueConst setter, int flags)
{
    LiveNodeList *list = JS_GetOpaque(object, live_node_list_class_id);
    if (list != NULL && atom != list->length_atom
        && live_node_list_atom_index(context, atom) < 0)
        return JS_DefineProperty(context, object, atom, value, getter, setter,
                                 flags | JS_PROP_NO_EXOTIC);
    if (flags & JS_PROP_THROW) {
        JS_ThrowTypeError(context, "A NodeList cannot be written to");
        return -1;
    }
    return 0;
}

static JSClassExoticMethods live_node_list_exotic = {
    .get_own_property = live_node_list_get_own_property,
    .get_own_property_names = live_node_list_get_own_property_names,
    .define_own_property = live_node_list_define_own_property,
};

static JSClassDef live_node_list_class = {
    .class_name = "NodeList",
    .finalizer = live_node_list_finalizer,
    .gc_mark = live_node_list_mark,
    .exotic = &live_node_list_exotic,
};

/* __tilefinchLiveNodeList(owner, source, prototype, wrap) */
static JSValue js_dom_live_node_list(JSContext *context,
                                     JSValueConst this_value, int argc,
                                     JSValueConst *argv)
{
    (void) this_value;
    if (argc < 4 || !JS_IsObject(argv[2])
        || !JS_IsFunction(context, argv[3]))
        return JS_ThrowTypeError(context, "Illegal invocation");
    JSRuntime *runtime = JS_GetRuntime(context);
    if (live_node_list_class_id == 0)
        JS_NewClassID(&live_node_list_class_id);
    if (!JS_IsRegisteredClass(runtime, live_node_list_class_id)
        && JS_NewClass(runtime, live_node_list_class_id,
                       &live_node_list_class) < 0)
        return JS_EXCEPTION;
    int64_t handle = 0;
    bool scripted = JS_IsFunction(context, argv[1]);
    if (!scripted && JS_ToInt64(context, &handle, argv[1]) < 0)
        return JS_EXCEPTION;
    LiveNodeList *list = js_mallocz(context, sizeof(*list));
    if (list == NULL) return JS_EXCEPTION;
    JSValue object = JS_NewObjectProtoClass(context, argv[2],
                                            live_node_list_class_id);
    list->length_atom = JS_NewAtom(context, "length");
    if (JS_IsException(object) || list->length_atom == JS_ATOM_NULL) {
        if (list->length_atom != JS_ATOM_NULL)
            JS_FreeAtom(context, list->length_atom);
        js_free(context, list);
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }
    list->owner = JS_DupValue(context, argv[0]);
    list->read = scripted ? JS_DupValue(context, argv[1]) : JS_UNDEFINED;
    list->wrap = JS_DupValue(context, argv[3]);
    list->handle = handle;
    list->length = UINT32_MAX;
    JS_SetOpaque(object, list);
    return object;
}
