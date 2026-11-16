/* Legacy Window and Document surface installed natively after the bootstrap:
   document.all (HTMLAllCollection), window.print(), the Window members a
   PSP window answers with constants (screenX, closed, moveTo...), window.name
   and status, and document.characterSet. Native so no
   bootstrap JavaScript stays resident for them; the collection's class and
   prototype are created on the first document.all read.

   window.orientation is deliberately absent, as in desktop browsers:
   screen.orientation already reports the PSP's landscape-primary at angle
   0, while pages read the legacy property as "this is a phone" (amazon's
   capability test and Leaflet's mobile flag), and amazon then loads touch
   code that takes it to the 32 MiB ceiling. */

static JSClassID legacy_all_class_id;

static bool collection_length_valid(JSContext *context, uint64_t count)
{
    if (count <= SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX + 1u) return true;
    JS_ThrowRangeError(context, "Collection exceeds bounded size");
    return false;
}

typedef struct {
    JSValue document;
    /* js_rt_document_scope: 0 or a document handle walks the native tree
       (holding the document keeps that handle valid); -1 is a script-side
       document (createHTMLDocument, DOMParser), whose own querySelectorAll
       is the only view of its tree. */
    int64_t scope;
} LegacyAllCollection;

static void legacy_all_finalizer(JSRuntime *runtime, JSValue value)
{
    LegacyAllCollection *collection =
        JS_GetOpaque(value, legacy_all_class_id);
    if (collection == NULL) return;
    JS_FreeValueRT(runtime, collection->document);
    js_free_rt(runtime, collection);
}

static void legacy_all_mark(JSRuntime *runtime, JSValueConst value,
                            JS_MarkFunc *mark)
{
    LegacyAllCollection *collection =
        JS_GetOpaque(value, legacy_all_class_id);
    if (collection != NULL) JS_MarkValue(runtime, collection->document, mark);
}

static LegacyAllCollection *legacy_all_this(JSContext *context,
                                            JSValueConst value)
{
    LegacyAllCollection *collection =
        JS_GetOpaque(value, legacy_all_class_id);
    if (collection == NULL) JS_ThrowTypeError(context, "Illegal invocation");
    return collection;
}

/* One pass over the collection's elements in tree order. The collection is
   live, so every read walks again: natively, registering a handle only for
   an element it returns, or, for a script-side document, through the
   static list its querySelectorAll("*") builds. */
typedef struct {
    DomElementWalk walk;
    JSValue list;
    uint32_t at, length;
} LegacyAllCursor;

/* Always pair with legacy_all_close(), whatever this returns. */
static bool legacy_all_open(JSContext *context,
                            const LegacyAllCollection *collection,
                            LegacyAllCursor *cursor)
{
    cursor->list = JS_UNDEFINED;
    cursor->at = cursor->length = 0;
    if (collection->scope >= 0) {
        if (js_rt_element_walk_init(&cursor->walk,
                                    JS_GetContextOpaque(context),
                                    collection->scope)) return true;
        JS_ThrowInternalError(context, "document.all is unavailable");
        return false;
    }
    JSValue select = JS_GetPropertyStr(context, collection->document,
                                       "querySelectorAll");
    JSValue argument = JS_NewString(context, "*");
    cursor->list = JS_IsException(select) || JS_IsException(argument)
        ? JS_EXCEPTION
        : JS_Call(context, select, collection->document, 1, &argument);
    JS_FreeValue(context, argument);
    JS_FreeValue(context, select);
    JSValue length = JS_IsException(cursor->list) ? JS_EXCEPTION
        : JS_GetPropertyStr(context, cursor->list, "length");
    bool ok = !JS_IsException(length)
        && JS_ToUint32(context, &cursor->length, length) == 0
        && collection_length_valid(context, cursor->length);
    JS_FreeValue(context, length);
    return ok;
}

static void legacy_all_close(JSContext *context, LegacyAllCursor *cursor)
{
    JS_FreeValue(context, cursor->list);
    cursor->list = JS_UNDEFINED;
}

/* The next element: a native node, or a script-side element the caller
   then owns. 1, 0 at the end, or -1 with an exception pending. */
static int legacy_all_next(JSContext *context, LegacyAllCursor *cursor,
                           lxb_dom_node_t **node, JSValue *element)
{
    *node = NULL;
    *element = JS_UNDEFINED;
    if (JS_IsUndefined(cursor->list)) {
        *node = js_rt_element_walk_next(&cursor->walk);
        return *node != NULL;
    }
    if (cursor->at >= cursor->length) return 0;
    *element = JS_GetPropertyUint32(context, cursor->list, cursor->at++);
    return JS_IsException(*element) ? -1 : 1;
}

/* What script sees for an element legacy_all_next() returned; takes
   ownership of `element`. */
static JSValue legacy_all_value(JSContext *context,
                                const LegacyAllCursor *cursor,
                                lxb_dom_node_t *node, JSValue element)
{
    return node == NULL ? element
        : js_rt_bridge_wrap_node(context, cursor->walk.bridge, node);
}

static int64_t legacy_all_length(JSContext *context,
                                 const LegacyAllCollection *collection)
{
    LegacyAllCursor cursor;
    int64_t length = -1;
    if (legacy_all_open(context, collection, &cursor)) {
        length = cursor.length;
        if (JS_IsUndefined(cursor.list))
            while (js_rt_element_walk_next(&cursor.walk) != NULL) length++;
    }
    legacy_all_close(context, &cursor);
    return length;
}

/* An array index atom as a number; -1 for any other string, -2 for a
   symbol. */
static int64_t legacy_all_atom_index(JSContext *context, JSAtom atom)
{
    JSValue key = JS_AtomToValue(context, atom);
    if (JS_IsSymbol(key) || JS_IsException(key)) {
        JS_FreeValue(context, key);
        return -2;
    }
    const char *text = JS_ToCString(context, key);
    JS_FreeValue(context, key);
    if (text == NULL) return -2;
    int64_t index = text[0] == '\0' || (text[0] == '0' && text[1] != '\0')
        ? -1 : 0;
    for (const char *c = text; index >= 0 && *c != '\0'; c++) {
        index = *c >= '0' && *c <= '9'
            ? index * 10 + (*c - '0') : -1;
        if (index >= (int64_t) UINT32_MAX) index = -1;
    }
    JS_FreeCString(context, text);
    return index;
}

static JSValue legacy_all_item_at(JSContext *context,
                                  const LegacyAllCollection *collection,
                                  int64_t index)
{
    LegacyAllCursor cursor;
    JSValue item = JS_EXCEPTION;
    if (legacy_all_open(context, collection, &cursor)) {
        lxb_dom_node_t *node = NULL;
        JSValue element = JS_UNDEFINED;
        int next = 0;
        if (!JS_IsUndefined(cursor.list)) cursor.at = (uint32_t) index;
        else for (int64_t skip = 0; skip < index
                  && js_rt_element_walk_next(&cursor.walk) != NULL; skip++) {}
        next = index < UINT32_MAX
            ? legacy_all_next(context, &cursor, &node, &element) : 0;
        item = next < 0 ? JS_EXCEPTION
            : next == 0 ? JS_NULL
            : legacy_all_value(context, &cursor, node, element);
    }
    legacy_all_close(context, &cursor);
    return item;
}

/* The HTML elements HTMLAllCollection also names by their name attribute,
   as their tagName reads. */
static const char *const legacy_all_named_tags[] = {
    "A", "BUTTON", "EMBED", "FORM", "FRAME", "FRAMESET", "IFRAME",
    "IMG", "INPUT", "MAP", "META", "OBJECT", "SELECT", "TEXTAREA"
};

/* A native element's attribute equals `name` (`length` bytes). */
static bool legacy_all_attribute_is(lxb_dom_node_t *node,
                                    const char *attribute,
                                    const char *name, size_t length)
{
    size_t value_length = 0;
    const char *value = document_attribute(node, attribute, &value_length);
    return value != NULL && value_length == length
        && memcmp(value, name, length) == 0;
}

static bool legacy_all_native_named(lxb_dom_node_t *node,
                                    const char *name, size_t length)
{
    if (legacy_all_attribute_is(node, "id", name, length)) return true;
    size_t tag_length = 0;
    const char *tag = node->ns == LXB_NS_HTML
        ? document_element_name(node, &tag_length) : NULL;
    bool legacy = false;
    for (size_t n = 0; tag != NULL && !legacy
                       && n < sizeof(legacy_all_named_tags)
                              / sizeof(legacy_all_named_tags[0]); n++)
        legacy = strlen(legacy_all_named_tags[n]) == tag_length
            && strncasecmp(tag, legacy_all_named_tags[n], tag_length) == 0;
    return legacy && legacy_all_attribute_is(node, "name", name, length);
}

/* A script-side element answers through its own members. 1, 0, or -1 with
   an exception pending. */
static int legacy_all_script_named(JSContext *context, JSValueConst element,
                                   const char *name)
{
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            JSValue tag = JS_GetPropertyStr(context, element, "tagName");
            const char *tag_text = JS_IsException(tag) ? NULL
                : JS_ToCString(context, tag);
            JS_FreeValue(context, tag);
            if (tag_text == NULL) return -1;
            bool legacy = false;
            for (size_t n = 0; !legacy && n < sizeof(legacy_all_named_tags)
                                   / sizeof(legacy_all_named_tags[0]); n++)
                legacy = strcmp(tag_text, legacy_all_named_tags[n]) == 0;
            JS_FreeCString(context, tag_text);
            if (!legacy) return 0;
        }
        JSValue get = JS_GetPropertyStr(context, element, "getAttribute");
        JSValue attribute = JS_NewString(context, pass == 0 ? "id" : "name");
        JSValue value = JS_IsException(get) || JS_IsException(attribute)
            ? JS_EXCEPTION
            : JS_Call(context, get, element, 1, &attribute);
        JS_FreeValue(context, attribute);
        JS_FreeValue(context, get);
        if (JS_IsException(value)) return -1;
        const char *value_text = JS_IsString(value)
            ? JS_ToCString(context, value) : NULL;
        bool match = value_text != NULL && strcmp(value_text, name) == 0;
        JS_FreeCString(context, value_text);
        JS_FreeValue(context, value);
        if (match) return 1;
    }
    return 0;
}

/* namedItem(): the elements whose id, or (for the legacy named elements)
   name, equals `name`: null, the one element, or an HTMLCollection-like
   array of them. */
static JSValue legacy_all_named(JSContext *context,
                                const LegacyAllCollection *collection,
                                JSValueConst name)
{
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, name);
    if (text == NULL) return JS_EXCEPTION;
    LegacyAllCursor cursor;
    JSValue matches = length > 0 && legacy_all_open(context, collection, &cursor)
        ? JS_NewArray(context) : length > 0 ? JS_EXCEPTION : JS_NULL;
    uint32_t found = 0;
    while (JS_IsObject(matches)) {
        lxb_dom_node_t *node = NULL;
        JSValue element = JS_UNDEFINED;
        int next = legacy_all_next(context, &cursor, &node, &element);
        int named = next <= 0 ? next
            : node != NULL ? legacy_all_native_named(node, text, length)
            : legacy_all_script_named(context, element, text);
        if (next == 0) break;
        if (named == 1) {
            JSValue value = legacy_all_value(context, &cursor, node, element);
            if (!JS_IsException(value)
                && JS_DefinePropertyValueUint32(
                       context, matches, found++, value,
                       JS_PROP_C_W_E) >= 0) continue;
            named = -1;
        } else JS_FreeValue(context, element);
        if (named < 0) {
            JS_FreeValue(context, matches);
            matches = JS_EXCEPTION;
        }
    }
    if (length > 0) legacy_all_close(context, &cursor);
    JS_FreeCString(context, text);
    if (!JS_IsObject(matches) || found > 1) return matches;
    JSValue result = found == 1
        ? JS_GetPropertyUint32(context, matches, 0) : JS_NULL;
    JS_FreeValue(context, matches);
    return result;
}

/* item() and the legacy caller: an array index selects by position, any
   other value by name. */
static JSValue legacy_all_item_or_named(JSContext *context,
                                        const LegacyAllCollection *collection,
                                        int argc, JSValueConst *argv)
{
    if (argc < 1 || JS_IsUndefined(argv[0])) return JS_NULL;
    JSAtom atom = JS_ValueToAtom(context, argv[0]);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
    int64_t index = legacy_all_atom_index(context, atom);
    JS_FreeAtom(context, atom);
    return index >= 0 ? legacy_all_item_at(context, collection, index)
                      : legacy_all_named(context, collection, argv[0]);
}

static JSValue legacy_all_call(JSContext *context, JSValueConst function,
                               JSValueConst this_value, int argc,
                               JSValueConst *argv, int flags)
{
    (void) this_value;
    (void) flags;
    LegacyAllCollection *collection = legacy_all_this(context, function);
    return collection == NULL ? JS_EXCEPTION
        : legacy_all_item_or_named(context, collection, argc, argv);
}

static JSValue legacy_all_method(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv, int magic)
{
    LegacyAllCollection *collection = legacy_all_this(context, this_value);
    if (collection == NULL) return JS_EXCEPTION;
    if (magic == 1)
        return legacy_all_item_or_named(context, collection, argc, argv);
    return argc < 1 ? JS_NULL
        : legacy_all_named(context, collection, argv[0]);
}

static JSValue legacy_all_length_get(JSContext *context,
                                     JSValueConst this_value, int magic)
{
    (void) magic;
    LegacyAllCollection *collection = legacy_all_this(context, this_value);
    if (collection == NULL) return JS_EXCEPTION;
    int64_t length = legacy_all_length(context, collection);
    return length < 0 ? JS_EXCEPTION : JS_NewInt64(context, length);
}

static int legacy_all_get_own_property(JSContext *context,
                                       JSPropertyDescriptor *descriptor,
                                       JSValueConst object, JSAtom atom)
{
    LegacyAllCollection *collection =
        JS_GetOpaque(object, legacy_all_class_id);
    if (collection == NULL) return 0;
    int64_t index = legacy_all_atom_index(context, atom);
    JSValue value;
    if (index >= 0) {
        value = legacy_all_item_at(context, collection, index);
    } else {
        /* Symbols are never named properties, and named properties never
           shadow the interface's own members. */
        if (index == -2) return 0;
        JSValue prototype = JS_GetPrototype(context, object);
        if (JS_IsException(prototype)) return -1;
        int inherited = JS_IsObject(prototype)
            ? JS_HasProperty(context, prototype, atom) : 0;
        JS_FreeValue(context, prototype);
        if (inherited != 0) return inherited < 0 ? -1 : 0;
        JSValue key = JS_AtomToString(context, atom);
        value = JS_IsException(key) ? key
            : legacy_all_named(context, collection, key);
        JS_FreeValue(context, key);
    }
    if (JS_IsException(value)) return -1;
    if (JS_IsNull(value)) return 0;
    if (descriptor != NULL) {
        descriptor->flags = JS_PROP_CONFIGURABLE
            | (index >= 0 ? JS_PROP_ENUMERABLE : 0);
        descriptor->value = value;
        descriptor->getter = JS_UNDEFINED;
        descriptor->setter = JS_UNDEFINED;
    } else {
        JS_FreeValue(context, value);
    }
    return 1;
}

/* Both exotic collection enumerators use this boundary. Script-side trees
   may report arbitrary lengths; a Budget refusal is not an overflow guard
   on the PSP. Reject those counts before size arithmetic or native loops. */
static JSPropertyEnum *collection_key_table(JSContext *context,
                                            int64_t count, size_t extra)
{
    size_t maximum = SIZE_MAX / sizeof(JSPropertyEnum);
    if (count < 0 || !collection_length_valid(context, (uint64_t) count))
        return NULL;
    if (extra > maximum || (uint64_t) count > maximum - extra) {
        JS_ThrowRangeError(context, "Collection enumeration exceeds bounded size");
        return NULL;
    }
    return js_malloc(context, sizeof(JSPropertyEnum) * ((size_t) count + extra));
}

static int legacy_all_get_own_property_names(JSContext *context,
                                             JSPropertyEnum **table,
                                             uint32_t *length,
                                             JSValueConst object)
{
    *table = NULL;
    *length = 0;
    LegacyAllCollection *collection =
        JS_GetOpaque(object, legacy_all_class_id);
    if (collection == NULL) return 0;
    int64_t count = legacy_all_length(context, collection);
    if (count < 0) return -1;
    if (count == 0) return 0;
    JSPropertyEnum *entries = collection_key_table(context, count, 0);
    if (entries == NULL) return -1;
    for (int64_t i = 0; i < count; i++) {
        entries[i].is_enumerable = 1;
        entries[i].atom = JS_NewAtomUInt32(context, (uint32_t) i);
    }
    *table = entries;
    *length = (uint32_t) count;
    return 0;
}

static int legacy_all_define_own_property(JSContext *context,
                                          JSValueConst object, JSAtom atom,
                                          JSValueConst value,
                                          JSValueConst getter,
                                          JSValueConst setter, int flags)
{
    (void) object;
    (void) atom;
    (void) value;
    (void) getter;
    (void) setter;
    /* No setters for indexed or named properties: like WebIDL legacy
       platform objects, a define fails (and throws in strict code). */
    if (flags & JS_PROP_THROW)
        JS_ThrowTypeError(context, "HTMLAllCollection is read-only");
    return (flags & JS_PROP_THROW) ? -1 : 0;
}

static JSClassExoticMethods legacy_all_exotic = {
    .get_own_property = legacy_all_get_own_property,
    .get_own_property_names = legacy_all_get_own_property_names,
    .define_own_property = legacy_all_define_own_property,
};

static JSClassDef legacy_all_class = {
    .class_name = "HTMLAllCollection",
    .finalizer = legacy_all_finalizer,
    .gc_mark = legacy_all_mark,
    .call = legacy_all_call,
    .exotic = &legacy_all_exotic,
};

/* The class and its prototype for this context, on first use. */
static bool legacy_all_ensure_class(JSContext *context)
{
    JSRuntime *runtime = JS_GetRuntime(context);
    if (legacy_all_class_id == 0) JS_NewClassID(&legacy_all_class_id);
    if (!JS_IsRegisteredClass(runtime, legacy_all_class_id)
        && JS_NewClass(runtime, legacy_all_class_id, &legacy_all_class) < 0)
        return false;
    JSValue existing = JS_GetClassProto(context, legacy_all_class_id);
    bool ready = JS_IsObject(existing);
    JS_FreeValue(context, existing);
    if (ready) return true;
    JSValue prototype = JS_NewObject(context);
    if (JS_IsException(prototype)) return false;
    static const JSCFunctionListEntry members[] = {
        JS_CGETSET_MAGIC_DEF("length", legacy_all_length_get, NULL, 0),
        JS_CFUNC_MAGIC_DEF("item", 0, legacy_all_method, 1),
        JS_CFUNC_MAGIC_DEF("namedItem", 1, legacy_all_method, 2),
        JS_PROP_STRING_DEF("[Symbol.toStringTag]", "HTMLAllCollection",
                           JS_PROP_CONFIGURABLE),
    };
    bool ok = JS_SetPropertyFunctionList(
                  context, prototype, members,
                  sizeof(members) / sizeof(members[0])) >= 0;
    /* Array iteration walks length and indices, as WebIDL's
       [LegacyIterable]-style collections do. */
    JSValue global = JS_GetGlobalObject(context);
    JSValue array = ok ? JS_GetPropertyStr(context, global, "Array")
                       : JS_UNDEFINED;
    JSValue array_prototype = JS_IsObject(array)
        ? JS_GetPropertyStr(context, array, "prototype") : JS_UNDEFINED;
    JSValue values = JS_IsObject(array_prototype)
        ? JS_GetPropertyStr(context, array_prototype, "values")
        : JS_UNDEFINED;
    JSValue symbol = JS_GetPropertyStr(context, global, "Symbol");
    JSValue iterator = JS_IsObject(symbol)
        ? JS_GetPropertyStr(context, symbol, "iterator") : JS_UNDEFINED;
    JSAtom iterator_atom = JS_IsSymbol(iterator)
        ? JS_ValueToAtom(context, iterator) : JS_ATOM_NULL;
    ok = ok && JS_IsFunction(context, values) && iterator_atom != JS_ATOM_NULL
        && JS_DefinePropertyValue(
               context, prototype, iterator_atom,
               JS_DupValue(context, values),
               JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) >= 0;
    if (iterator_atom != JS_ATOM_NULL) JS_FreeAtom(context, iterator_atom);
    JS_FreeValue(context, iterator);
    JS_FreeValue(context, symbol);
    JS_FreeValue(context, values);
    JS_FreeValue(context, array_prototype);
    JS_FreeValue(context, array);
    JS_FreeValue(context, global);
    if (!ok) {
        JS_FreeValue(context, prototype);
        return false;
    }
    JS_SetClassProto(context, legacy_all_class_id, prototype);
    return true;
}

/* Whether `value` inherits from `prototype` (the Document.prototype
   captured at install, not the global a page can replace). 1, 0, or -1 with
   an exception pending. */
static int legacy_all_inherits(JSContext *context, JSValueConst value,
                               JSValueConst prototype)
{
    JSValue at = JS_GetPrototype(context, value);
    for (int depth = 0; depth < 64 && JS_IsObject(at); depth++) {
        if (JS_VALUE_GET_PTR(at) == JS_VALUE_GET_PTR(prototype)) {
            JS_FreeValue(context, at);
            return 1;
        }
        JSValue next = JS_GetPrototype(context, at);
        JS_FreeValue(context, at);
        at = next;
    }
    int result = JS_IsException(at) ? -1 : 0;
    JS_FreeValue(context, at);
    return result;
}

/* get Document.prototype.all: one collection per Document ([SameObject]),
   remembered in a private WeakMap created on the first read. Its
   [[IsHTMLDDA]] slot makes it typeof "undefined", falsy and loosely equal
   to null and undefined. data[0] is Document.prototype. */
static JSValue js_document_all_get(JSContext *context,
                                   JSValueConst this_value, int argc,
                                   JSValueConst *argv, int magic,
                                   JSValue *data)
{
    (void) argc;
    (void) argv;
    (void) magic;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    if (runtime == NULL || !JS_IsObject(this_value))
        return JS_ThrowTypeError(context, "Illegal invocation");
    int is_document = legacy_all_inherits(context, this_value, data[0]);
    if (is_document < 0) return JS_EXCEPTION;
    if (!is_document) return JS_ThrowTypeError(context, "Illegal invocation");
    int64_t scope = js_rt_document_scope(context, this_value);
    if (scope < -1) return JS_EXCEPTION;
    if (JS_IsUndefined(runtime->document_all_cache)) {
        /* hardening.js published the factory non-writable and
           non-configurable, so author code cannot have replaced it. */
        JSValue global_object = JS_GetGlobalObject(context);
        JSValue factory = JS_GetPropertyStr(
            context, global_object, "__tilefinchCreatePrivateWeakMap");
        JS_FreeValue(context, global_object);
        JSValue created = JS_IsFunction(context, factory)
            ? JS_Call(context, factory, JS_UNDEFINED, 0, NULL)
            : JS_ThrowInternalError(context, "document.all is unavailable");
        JS_FreeValue(context, factory);
        if (JS_IsException(created)) return created;
        runtime->document_all_cache = created;
    }
    JSValue cache = runtime->document_all_cache;
    JSValue get = JS_GetPropertyStr(context, cache, "get");
    JSValue found = JS_IsFunction(context, get)
        ? JS_Call(context, get, cache, 1, &this_value) : JS_UNDEFINED;
    JS_FreeValue(context, get);
    if (JS_IsException(found) || !JS_IsUndefined(found)) return found;
    if (!legacy_all_ensure_class(context)) return JS_EXCEPTION;
    LegacyAllCollection *collection =
        js_mallocz(context, sizeof(*collection));
    if (collection == NULL) return JS_EXCEPTION;
    JSValue object = JS_NewObjectClass(context, (int) legacy_all_class_id);
    if (JS_IsException(object)) {
        js_free(context, collection);
        return object;
    }
    collection->document = JS_DupValue(context, this_value);
    collection->scope = scope;
    JS_SetOpaque(object, collection);
    JS_SetIsHTMLDDA(context, object);
    JSValue set = JS_GetPropertyStr(context, cache, "set");
    JSValue arguments[2] = { this_value, object };
    JSValue stored = JS_IsFunction(context, set)
        ? JS_Call(context, set, cache, 2, arguments) : JS_UNDEFINED;
    JS_FreeValue(context, set);
    if (JS_IsException(stored)) {
        JS_FreeValue(context, object);
        return stored;
    }
    JS_FreeValue(context, stored);
    return object;
}

static JSValue js_window_print(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) context;
    (void) this_value;
    (void) argc;
    (void) argv;
    /* Printing is optional in HTML; the PSP has no printer. */
    return JS_UNDEFINED;
}

/* Window operations a PSP window answers by doing nothing: it is never
   moved, resized, focused away from or blurred, and event capture is a
   no-op in HTML. stop() has no loading of its own to abort here either: the
   navigation layer owns the document's fetch. */
static JSValue js_window_noop(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) context;
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_UNDEFINED;
}

/* The browsing context's name (magic 0) and the legacy status text
   (magic 1). A top-level realm's name lives in its navigation's slot, so it
   survives the document; a frame's starts as its container's name. */
static JSValue js_window_text_get(JSContext *context, JSValueConst this_value,
                                  int magic)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    if (runtime == NULL) return JS_NewString(context, "");
    JSValue value = magic == 0 ? runtime->window_name : runtime->window_status;
    return JS_IsString(value) ? JS_DupValue(context, value)
                              : JS_NewString(context, "");
}

static JSValue js_window_text_set(JSContext *context, JSValueConst this_value,
                                  JSValueConst value, int magic)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    if (runtime == NULL) return JS_UNDEFINED;
    JSValue text = JS_ToString(context, value);
    if (JS_IsException(text)) return text;
    if (magic == 0 && runtime->window_name_slot != NULL) {
        size_t length = 0;
        const char *bytes = JS_ToCStringLen(context, &length, text);
        if (bytes == NULL) {
            JS_FreeValue(context, text);
            return JS_EXCEPTION;
        }
        /* A refusal (too long, or no memory) keeps the name with this
           document only: the next one starts without it. */
        if (!script_window_name_store(runtime->window_name_slot, bytes,
                                      length))
            script_window_name_store(runtime->window_name_slot, "", 0);
        JS_FreeCString(context, bytes);
    }
    JSValue *slot = magic == 0 ? &runtime->window_name
                               : &runtime->window_status;
    JS_FreeValue(context, *slot);
    *slot = text;
    return JS_UNDEFINED;
}

/* A Window whose browsing context exists is never closed: the realm ends
   with its document. */
static JSValue js_window_closed(JSContext *context, JSValueConst this_value)
{
    (void) context;
    (void) this_value;
    return JS_FALSE;
}

static const JSCFunctionListEntry window_legacy_functions[] = {
#define WINDOW_NOOP(name, length) \
    { name, JS_PROP_C_W_E, JS_DEF_CFUNC, 0, \
      .u = { .func = { length, JS_CFUNC_generic, \
                       { .generic = js_window_noop } } } }
    /* close() too: no Tilefinch window was opened by a script, so none is
       script-closable, and the call does nothing. */
    WINDOW_NOOP("stop", 0), WINDOW_NOOP("focus", 0), WINDOW_NOOP("blur", 0),
    WINDOW_NOOP("close", 0),
    WINDOW_NOOP("moveTo", 2), WINDOW_NOOP("moveBy", 2),
    WINDOW_NOOP("resizeTo", 2), WINDOW_NOOP("resizeBy", 2),
    WINDOW_NOOP("captureEvents", 0), WINDOW_NOOP("releaseEvents", 0),
#undef WINDOW_NOOP
    /* The window fills the screen at its origin. [Replaceable] attributes,
       so data properties a page may overwrite, as innerWidth is. */
    JS_PROP_INT32_DEF("screenX", 0, JS_PROP_C_W_E),
    JS_PROP_INT32_DEF("screenY", 0, JS_PROP_C_W_E),
    JS_PROP_INT32_DEF("screenLeft", 0, JS_PROP_C_W_E),
    JS_PROP_INT32_DEF("screenTop", 0, JS_PROP_C_W_E),
    /* The entries above are also each frame realm's own; frames.js defines
       a frame's name, and its WindowProxy answers closed. */
    { "name", JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE,
      JS_DEF_CGETSET_MAGIC, 0,
      .u = { .getset = { .get = { .getter_magic = js_window_text_get },
                         .set = { .setter_magic = js_window_text_set } } } },
    { "status", JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE,
      JS_DEF_CGETSET_MAGIC, 1,
      .u = { .getset = { .get = { .getter_magic = js_window_text_get },
                         .set = { .setter_magic = js_window_text_set } } } },
    { "closed", JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, JS_DEF_CGETSET, 0,
      .u = { .getset = { .get = { .getter = js_window_closed } } } },
};

#define WINDOW_FRAME_MEMBER_COUNT 14

/* The entries the global does not already own: a function list aborts on
   a name that exists, and a session page's bootstrap defines some of these
   itself (that definition is kept). */
static bool legacy_define_missing_members(JSContext *context,
                                          JSValueConst global, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        JSAtom atom = JS_NewAtom(context, window_legacy_functions[i].name);
        int present = atom == JS_ATOM_NULL ? -1
            : JS_GetOwnProperty(context, NULL, global, atom);
        if (atom != JS_ATOM_NULL) JS_FreeAtom(context, atom);
        if (present < 0
            || (present == 0
                && JS_SetPropertyFunctionList(
                       context, global, &window_legacy_functions[i], 1) < 0))
            return false;
    }
    return true;
}

static bool legacy_define_frame_window_members(JSContext *realm,
                                               JSValueConst global)
{
    return legacy_define_missing_members(realm, global,
                                         WINDOW_FRAME_MEMBER_COUNT);
}

/* Document.characterSet and its legacy aliases charset and inputEncoding
   (magic picks the name). data[0] is the realm's document, whose encoding
   the parser decided; data[1] is Document.prototype. Every other document
   here (createHTMLDocument, DOMParser, new Document(), frames) was built
   from a string, so it is UTF-8. */
static JSValue js_document_character_set(JSContext *context,
                                         JSValueConst this_value, int argc,
                                         JSValueConst *argv, int magic,
                                         JSValue *data)
{
    (void) argc;
    (void) argv;
    (void) magic;
    if (!JS_IsObject(this_value))
        return JS_ThrowTypeError(context, "Illegal invocation");
    if (JS_VALUE_GET_PTR(this_value) != JS_VALUE_GET_PTR(data[0])) {
        int is_document = legacy_all_inherits(context, this_value, data[1]);
        if (is_document < 0) return JS_EXCEPTION;
        if (!is_document)
            return JS_ThrowTypeError(context, "Illegal invocation");
        return JS_NewString(context, "UTF-8");
    }
    DomBridge *bridge = JS_GetContextOpaque(context);
    const char *name = bridge == NULL || bridge->document == NULL ? NULL
        : tilefinch_encoding_name(
              (TilefinchEncoding) bridge->document->encoding);
    return JS_NewString(context, name == NULL ? "UTF-8" : name);
}

static bool legacy_define_character_set(JSContext *context,
                                        JSValueConst global,
                                        JSValueConst prototype)
{
    JSValue data[2] = {
        JS_GetPropertyStr(context, global, "document"),
        JS_DupValue(context, prototype)
    };
    static const char *const names[] = {
        "characterSet", "charset", "inputEncoding"
    };
    bool ok = JS_IsObject(data[0]);
    for (int i = 0; ok && i < 3; i++) {
        char label[32];
        snprintf(label, sizeof(label), "get %s", names[i]);
        JSValue getter = JS_NewCFunctionData(
            context, js_document_character_set, 0, i, 2, data);
        ok = !JS_IsException(getter)
            && JS_DefinePropertyValueStr(context, getter, "name",
                                         JS_NewString(context, label),
                                         JS_PROP_CONFIGURABLE) >= 0;
        JSAtom atom = ok ? JS_NewAtom(context, names[i]) : JS_ATOM_NULL;
        if (ok && atom != JS_ATOM_NULL) {
            ok = JS_DefinePropertyGetSet(
                     context, prototype, atom, getter, JS_UNDEFINED,
                     JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) >= 0;
            getter = JS_UNDEFINED;
        } else {
            ok = false;
        }
        JS_FreeValue(context, getter);
        if (atom != JS_ATOM_NULL) JS_FreeAtom(context, atom);
    }
    JS_FreeValue(context, data[0]);
    JS_FreeValue(context, data[1]);
    return ok;
}

void script_window_name_release(ScriptWindowName *name)
{
    if (name == NULL) return;
    if (name->budget != NULL) budget_free(name->budget, name->value);
    name->value = NULL;
    name->length = 0;
}

bool script_window_name_store(ScriptWindowName *name, const char *value,
                              size_t length)
{
    if (name == NULL || (value == NULL && length != 0)) return false;
    if (length > SCRIPT_WINDOW_NAME_LIMIT || name->budget == NULL) {
        script_window_name_release(name);
        return length == 0;
    }
    if (length == 0) {
        script_window_name_release(name);
        return true;
    }
    char *copy = budget_realloc(name->budget, name->value, length);
    if (copy == NULL) {
        script_window_name_release(name);
        return false;
    }
    memcpy(copy, value, length);
    name->value = copy;
    name->length = length;
    return true;
}

/* HTML clears a top-level navigable's name when it navigates to another
   site, so a name a page set does not follow the user to an unrelated one
   (the "window.name" cross-site channel). Same-site documents keep it. */
void script_window_name_enter(ScriptWindowName *name, const char *url)
{
    if (name == NULL) return;
    char site[sizeof(name->site)];
    if (url == NULL || !tilefinch_url_site_key(url, site, sizeof(site)))
        site[0] = '\0';
    if (site[0] == '\0' || strcmp(site, name->site) != 0)
        script_window_name_release(name);
    memcpy(name->site, site, sizeof(site));
}

bool script_runtime_set_window_name(ScriptRuntime *runtime, const char *name,
                                    size_t length)
{
    if (runtime == NULL || runtime->context == NULL
        || (name == NULL && length != 0)) return false;
    JSValue value = JS_NewStringLen(runtime->context, name == NULL ? "" : name,
                                    length);
    if (JS_IsException(value)) {
        JS_FreeValue(runtime->context, JS_GetException(runtime->context));
        return false;
    }
    JS_FreeValue(runtime->context, runtime->window_name);
    runtime->window_name = value;
    return true;
}

static bool legacy_define_window_members(ScriptRuntime *host,
                                         JSValueConst global)
{
    const ScriptWindowName *slot = host->window_name_slot;
    if (slot != NULL && slot->length != 0
        && !script_runtime_set_window_name(host, slot->value, slot->length))
        return false;
    return legacy_define_missing_members(
        host->context, global,
        sizeof(window_legacy_functions) / sizeof(window_legacy_functions[0]));
}

/* Installed after hardening.js, which publishes the private WeakMap
   factory document.all's first read uses. */
static bool runtime_install_legacy_surface(ScriptRuntime *host,
                                           JSValueConst global)
{
    JSContext *context = host->context;
    JSValue constructor = JS_GetPropertyStr(context, global, "Document");
    JSValue prototype = JS_IsObject(constructor)
        ? JS_GetPropertyStr(context, constructor, "prototype")
        : JS_UNDEFINED;
    JS_FreeValue(context, constructor);
    JSAtom all_atom = JS_NewAtom(context, "all");
    JSValue getter = JS_IsObject(prototype)
        ? JS_NewCFunctionData(context, js_document_all_get, 0, 0, 1,
                              &prototype)
        : JS_EXCEPTION;
    bool ok = JS_IsObject(prototype) && all_atom != JS_ATOM_NULL
        && !JS_IsException(getter)
        && JS_DefinePropertyValueStr(context, getter, "name",
                                     JS_NewString(context, "get all"),
                                     JS_PROP_CONFIGURABLE) >= 0;
    if (ok) {
        ok = JS_DefinePropertyGetSet(
                 context, prototype, all_atom, getter, JS_UNDEFINED,
                 JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) >= 0;
    } else {
        JS_FreeValue(context, getter);
    }
    if (all_atom != JS_ATOM_NULL) JS_FreeAtom(context, all_atom);
    ok = ok && legacy_define_character_set(context, global, prototype)
        && legacy_define_window_members(host, global);
    JS_FreeValue(context, prototype);
    JSValue print = ok ? JS_NewCFunction(context, js_window_print, "print", 0)
                       : JS_EXCEPTION;
    return ok && !JS_IsException(print)
        && JS_DefinePropertyValueStr(context, global, "print", print,
                                     JS_PROP_C_W_E) >= 0;
}
