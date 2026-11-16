/* Reflected content attributes (HTML "reflect") that the bootstrap modules
   do not define: img.alt, iframe.allowFullscreen, td.colSpan and the rest of
   reflected_attributes[]. Each is a native accessor pair on its interface
   prototype, so a member costs two native function objects and no closure
   or bootstrap source. A member a bootstrap module already defines is
   kept.

   The accessors read and write through the Element.prototype getAttribute,
   setAttribute and removeAttribute captured after the bootstrap, as the
   bootstrap's own reflectString does, so mutation observers and custom
   element callbacks see the write. URL members resolve against the
   document base URL with the captured URL constructor.

   An accessor pair costs about 700 bytes of page heap on the desktop build
   (measured 2026-11-15: 43 members, 29.7 KiB), paid by every realm that
   installs it, so the table holds the members pages and fingerprint scripts
   read, not every obsolete presentational attribute (body.bgColor,
   font.face, table.cellPadding and the like stay undefined). The ARIA
   members, most of that cost, are installed only on the sites
   site_identity_aria_reflection_site() names, with dom.js's
   element-reference attributes (reflect_install_aria_element_references),
   except role, ariaAtomic and ariaLive, which every site has had since
   before the ARIA set was added (v0.1.50). */

enum {
    REFLECT_STRING,
    REFLECT_STRING_NULL_EMPTY, /* [LegacyNullToEmptyString] DOMString */
    REFLECT_NULLABLE,          /* DOMString?: null is no attribute */
    REFLECT_URL,
    REFLECT_URL_OR_DOCUMENT,   /* formAction: missing or empty is the document */
    REFLECT_BOOLEAN,
    REFLECT_LONG,              /* default in value */
    REFLECT_UNSIGNED,          /* default in value */
    REFLECT_POSITIVE,          /* limited to only positive numbers */
    REFLECT_CLAMPED,           /* clamp range in keywords, default in value */
    REFLECT_ENUMERATED,        /* keyword set in keywords */
    REFLECT_TEXT_CONTENT       /* a.text */
};

enum {
    REFLECT_HOOK_GET, REFLECT_HOOK_SET, REFLECT_HOOK_REMOVE, REFLECT_HOOK_URL,
    REFLECT_HOOK_DOM_EXCEPTION, REFLECT_HOOK_COUNT
};

typedef struct ReflectKeywords {
    const char *const *keywords; /* canonical, lowercase; NULL ends */
    const char *missing;         /* NULL: the attribute is nullable */
    const char *invalid;
} ReflectKeywords;

static const char *const reflect_cross_origin_words[] = {
    "anonymous", "use-credentials", NULL
};
static const char *const reflect_referrer_policy_words[] = {
    "", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin",
    "strict-origin", "origin-when-cross-origin",
    "strict-origin-when-cross-origin", "unsafe-url", NULL
};
static const char *const reflect_decoding_words[] = {
    "sync", "async", "auto", NULL
};
static const char *const reflect_fetch_priority_words[] = {
    "high", "low", "auto", NULL
};
static const char *const reflect_on_off_words[] = { "on", "off", NULL };
static const char *const reflect_enctype_words[] = {
    "application/x-www-form-urlencoded", "multipart/form-data",
    "text/plain", NULL
};
static const char *const reflect_method_words[] = {
    "get", "post", "dialog", NULL
};
static const char *const reflect_scope_words[] = {
    "row", "col", "rowgroup", "colgroup", NULL
};
static const char *const reflect_enter_key_hint_words[] = {
    "enter", "done", "go", "next", "previous", "search", "send", NULL
};
static const char *const reflect_track_kind_words[] = {
    "subtitles", "captions", "descriptions", "chapters", "metadata", NULL
};

enum {
    REFLECT_WORDS_CROSS_ORIGIN, REFLECT_WORDS_REFERRER_POLICY,
    REFLECT_WORDS_DECODING, REFLECT_WORDS_FETCH_PRIORITY,
    REFLECT_WORDS_AUTOCOMPLETE, REFLECT_WORDS_FORM_ENCTYPE,
    REFLECT_WORDS_FORM_METHOD, REFLECT_WORDS_SCOPE,
    REFLECT_WORDS_ENTER_KEY_HINT, REFLECT_WORDS_TRACK_KIND
};

/* crossOrigin's empty value is the Anonymous state, so "" reads as
   "anonymous" through the invalid-value default. */
static const ReflectKeywords reflect_keyword_sets[] = {
    [REFLECT_WORDS_CROSS_ORIGIN] =
        { reflect_cross_origin_words, NULL, "anonymous" },
    [REFLECT_WORDS_REFERRER_POLICY] =
        { reflect_referrer_policy_words, "", "" },
    [REFLECT_WORDS_DECODING] = { reflect_decoding_words, "auto", "auto" },
    [REFLECT_WORDS_FETCH_PRIORITY] =
        { reflect_fetch_priority_words, "auto", "auto" },
    [REFLECT_WORDS_AUTOCOMPLETE] = { reflect_on_off_words, "on", "on" },
    [REFLECT_WORDS_FORM_ENCTYPE] = { reflect_enctype_words, "",
                                     "application/x-www-form-urlencoded" },
    [REFLECT_WORDS_FORM_METHOD] = { reflect_method_words, "", "get" },
    [REFLECT_WORDS_SCOPE] = { reflect_scope_words, "", "" },
    [REFLECT_WORDS_ENTER_KEY_HINT] =
        { reflect_enter_key_hint_words, "", "" },
    [REFLECT_WORDS_TRACK_KIND] =
        { reflect_track_kind_words, "subtitles", "metadata" },
};

/* Clamp ranges for REFLECT_CLAMPED, by keywords index. */
static const int32_t reflect_clamp_ranges[][2] = {
    { 1, 1000 },  /* colSpan, col.span */
    { 0, 65534 }, /* rowSpan */
};

typedef struct ReflectedAttribute {
    const char *interface_name;
    const char *idl;
    const char *attribute;
    uint8_t kind;
    uint8_t keywords;
    bool aria_sites_only; /* site_identity_aria_reflection_site() realms */
    int32_t value;
} ReflectedAttribute;

#define R_(iface, idl, attr, kind) { iface, idl, attr, kind, 0, false, 0 }
#define RS(iface, idl, attr) R_(iface, idl, attr, REFLECT_STRING)
#define RN(iface, idl, attr) R_(iface, idl, attr, REFLECT_STRING_NULL_EMPTY)
#define RU(iface, idl, attr) R_(iface, idl, attr, REFLECT_URL)
#define RA(idl, attr) \
    { "Element", idl, attr, REFLECT_NULLABLE, 0, true, 0 }
#define RV(idl, attr) \
    { "Element", idl, attr, REFLECT_NULLABLE, 0, false, 0 }
#define RB(iface, idl, attr) R_(iface, idl, attr, REFLECT_BOOLEAN)
#define RE(iface, idl, attr, words) \
    { iface, idl, attr, REFLECT_ENUMERATED, REFLECT_WORDS_##words, false, 0 }
#define RI(iface, idl, attr, kind, fallback) \
    { iface, idl, attr, kind, 0, false, fallback }
#define RC(iface, idl, attr, range, fallback) \
    { iface, idl, attr, REFLECT_CLAMPED, range, false, fallback }

static const ReflectedAttribute reflected_attributes[] = {
    RS("HTMLImageElement", "alt", "alt"),
    RE("HTMLImageElement", "crossOrigin", "crossorigin", CROSS_ORIGIN),
    RS("HTMLImageElement", "useMap", "usemap"),
    RB("HTMLImageElement", "isMap", "ismap"),
    RE("HTMLImageElement", "referrerPolicy", "referrerpolicy",
       REFERRER_POLICY),
    RE("HTMLImageElement", "decoding", "decoding", DECODING),
    RE("HTMLImageElement", "fetchPriority", "fetchpriority", FETCH_PRIORITY),
    RU("HTMLImageElement", "lowsrc", "lowsrc"),
    RS("HTMLImageElement", "align", "align"),
    RI("HTMLImageElement", "hspace", "hspace", REFLECT_UNSIGNED, 0),
    RI("HTMLImageElement", "vspace", "vspace", REFLECT_UNSIGNED, 0),
    RU("HTMLImageElement", "longDesc", "longdesc"),
    RN("HTMLImageElement", "border", "border"),

    RS("HTMLAnchorElement", "download", "download"),
    RS("HTMLAnchorElement", "ping", "ping"),
    RE("HTMLAnchorElement", "referrerPolicy", "referrerpolicy",
       REFERRER_POLICY),
    R_("HTMLAnchorElement", "text", "", REFLECT_TEXT_CONTENT),

    RS("HTMLAreaElement", "alt", "alt"),
    RS("HTMLAreaElement", "coords", "coords"),
    RS("HTMLAreaElement", "shape", "shape"),
    RS("HTMLAreaElement", "download", "download"),
    RS("HTMLAreaElement", "ping", "ping"),
    RE("HTMLAreaElement", "referrerPolicy", "referrerpolicy",
       REFERRER_POLICY),

    RS("HTMLIFrameElement", "allow", "allow"),
    RB("HTMLIFrameElement", "allowFullscreen", "allowfullscreen"),
    RS("HTMLIFrameElement", "scrolling", "scrolling"),
    RS("HTMLIFrameElement", "frameBorder", "frameborder"),

    RE("HTMLScriptElement", "referrerPolicy", "referrerpolicy",
       REFERRER_POLICY),
    RE("HTMLScriptElement", "fetchPriority", "fetchpriority", FETCH_PRIORITY),
    RS("HTMLScriptElement", "charset", "charset"),

    RS("HTMLLinkElement", "imageSrcset", "imagesrcset"),
    RS("HTMLLinkElement", "imageSizes", "imagesizes"),
    RE("HTMLLinkElement", "fetchPriority", "fetchpriority", FETCH_PRIORITY),

    RS("HTMLInputElement", "accept", "accept"),
    RS("HTMLInputElement", "alt", "alt"),
    RS("HTMLInputElement", "autocomplete", "autocomplete"),
    RS("HTMLInputElement", "dirName", "dirname"),
    R_("HTMLInputElement", "formAction", "formaction",
       REFLECT_URL_OR_DOCUMENT),
    RE("HTMLInputElement", "formEnctype", "formenctype", FORM_ENCTYPE),
    RE("HTMLInputElement", "formMethod", "formmethod", FORM_METHOD),
    RS("HTMLInputElement", "formTarget", "formtarget"),
    RB("HTMLInputElement", "multiple", "multiple"),
    RI("HTMLInputElement", "size", "size", REFLECT_POSITIVE, 20),

    R_("HTMLButtonElement", "formAction", "formaction",
       REFLECT_URL_OR_DOCUMENT),
    RE("HTMLButtonElement", "formEnctype", "formenctype", FORM_ENCTYPE),
    RE("HTMLButtonElement", "formMethod", "formmethod", FORM_METHOD),
    RS("HTMLButtonElement", "formTarget", "formtarget"),

    RS("HTMLFormElement", "acceptCharset", "accept-charset"),
    RE("HTMLFormElement", "autocomplete", "autocomplete", AUTOCOMPLETE),

    RS("HTMLSelectElement", "autocomplete", "autocomplete"),
    RI("HTMLSelectElement", "size", "size", REFLECT_UNSIGNED, 0),

    RS("HTMLTextAreaElement", "autocomplete", "autocomplete"),
    RI("HTMLTextAreaElement", "cols", "cols", REFLECT_POSITIVE, 20),
    RS("HTMLTextAreaElement", "dirName", "dirname"),
    RI("HTMLTextAreaElement", "rows", "rows", REFLECT_POSITIVE, 2),
    RS("HTMLTextAreaElement", "wrap", "wrap"),

    RC("HTMLTableCellElement", "colSpan", "colspan", 0, 1),
    RC("HTMLTableCellElement", "rowSpan", "rowspan", 1, 1),
    RS("HTMLTableCellElement", "headers", "headers"),
    RS("HTMLTableCellElement", "abbr", "abbr"),
    RE("HTMLTableCellElement", "scope", "scope", SCOPE),

    RC("HTMLTableColElement", "span", "span", 0, 1),

    RB("HTMLOListElement", "reversed", "reversed"),
    RI("HTMLOListElement", "start", "start", REFLECT_LONG, 1),
    RS("HTMLStyleElement", "media", "media"),

    RE("HTMLTrackElement", "kind", "kind", TRACK_KIND),
    RS("HTMLTrackElement", "srclang", "srclang"),
    RS("HTMLTrackElement", "label", "label"),
    RB("HTMLTrackElement", "default", "default"),

    RS("HTMLTimeElement", "dateTime", "datetime"),
    RU("HTMLQuoteElement", "cite", "cite"),
    RU("HTMLModElement", "cite", "cite"),
    RS("HTMLModElement", "dateTime", "datetime"),

    RE("HTMLElement", "enterKeyHint", "enterkeyhint", ENTER_KEY_HINT),

    /* ARIAMixin's string attributes (WAI-ARIA): the content attribute's
       value, or null without one. Its element-reference attributes are
       dom.js's. RV: on every site. */
    RV("role", "role"),
    RV("ariaAtomic", "aria-atomic"),
    RA("ariaAutoComplete", "aria-autocomplete"),
    RA("ariaBrailleLabel", "aria-braillelabel"),
    RA("ariaBrailleRoleDescription", "aria-brailleroledescription"),
    RA("ariaBusy", "aria-busy"),
    RA("ariaChecked", "aria-checked"),
    RA("ariaColCount", "aria-colcount"),
    RA("ariaColIndex", "aria-colindex"),
    RA("ariaColIndexText", "aria-colindextext"),
    RA("ariaColSpan", "aria-colspan"),
    RA("ariaCurrent", "aria-current"),
    RA("ariaDescription", "aria-description"),
    RA("ariaDisabled", "aria-disabled"),
    RA("ariaExpanded", "aria-expanded"),
    RA("ariaHasPopup", "aria-haspopup"),
    RA("ariaHidden", "aria-hidden"),
    RA("ariaInvalid", "aria-invalid"),
    RA("ariaKeyShortcuts", "aria-keyshortcuts"),
    RA("ariaLabel", "aria-label"),
    RA("ariaLevel", "aria-level"),
    RV("ariaLive", "aria-live"),
    RA("ariaModal", "aria-modal"),
    RA("ariaMultiLine", "aria-multiline"),
    RA("ariaMultiSelectable", "aria-multiselectable"),
    RA("ariaOrientation", "aria-orientation"),
    RA("ariaPlaceholder", "aria-placeholder"),
    RA("ariaPosInSet", "aria-posinset"),
    RA("ariaPressed", "aria-pressed"),
    RA("ariaReadOnly", "aria-readonly"),
    RA("ariaRelevant", "aria-relevant"),
    RA("ariaRequired", "aria-required"),
    RA("ariaRoleDescription", "aria-roledescription"),
    RA("ariaRowCount", "aria-rowcount"),
    RA("ariaRowIndex", "aria-rowindex"),
    RA("ariaRowIndexText", "aria-rowindextext"),
    RA("ariaRowSpan", "aria-rowspan"),
    RA("ariaSelected", "aria-selected"),
    RA("ariaSetSize", "aria-setsize"),
    RA("ariaSort", "aria-sort"),
    RA("ariaValueMax", "aria-valuemax"),
    RA("ariaValueMin", "aria-valuemin"),
    RA("ariaValueNow", "aria-valuenow"),
    RA("ariaValueText", "aria-valuetext"),
};

#undef R_
#undef RS
#undef RN
#undef RU
#undef RA
#undef RV
#undef RB
#undef RE
#undef RI
#undef RC

static ScriptRuntime *reflect_host(JSContext *context)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    return bridge == NULL ? NULL : bridge->host;
}

/* getAttribute(attribute) through the captured method: a string, or null. */
static JSValue reflect_read(JSContext *context, ScriptRuntime *host,
                            JSValueConst element,
                            const ReflectedAttribute *entry)
{
    JSValue name = JS_NewString(context, entry->attribute);
    if (JS_IsException(name)) return name;
    JSValue value = JS_Call(context, host->reflect_hooks[REFLECT_HOOK_GET],
                            element, 1, &name);
    JS_FreeValue(context, name);
    return value;
}

static JSValue reflect_write(JSContext *context, ScriptRuntime *host,
                             JSValueConst element,
                             const ReflectedAttribute *entry, JSValue text)
{
    if (JS_IsException(text)) return text;
    JSValue arguments[2] = { JS_NewString(context, entry->attribute), text };
    JSValue result = JS_IsException(arguments[0]) ? JS_EXCEPTION
        : JS_Call(context, host->reflect_hooks[REFLECT_HOOK_SET], element, 2,
                  arguments);
    JS_FreeValue(context, arguments[0]);
    JS_FreeValue(context, text);
    if (JS_IsException(result)) return result;
    JS_FreeValue(context, result);
    return JS_UNDEFINED;
}

static JSValue reflect_remove(JSContext *context, ScriptRuntime *host,
                              JSValueConst element,
                              const ReflectedAttribute *entry)
{
    JSValue name = JS_NewString(context, entry->attribute);
    if (JS_IsException(name)) return name;
    JSValue result = JS_Call(context,
                             host->reflect_hooks[REFLECT_HOOK_REMOVE],
                             element, 1, &name);
    JS_FreeValue(context, name);
    if (JS_IsException(result)) return result;
    JS_FreeValue(context, result);
    return JS_UNDEFINED;
}

/* HTML's rules for parsing integers. false: no number. A value beyond the
   int32 range is reported as INT64_MAX or INT64_MIN. */
static bool reflect_parse_integer(const char *text, size_t length,
                                  int64_t *out)
{
    size_t at = 0;
    while (at < length && (text[at] == ' ' || text[at] == '\t'
                           || text[at] == '\n' || text[at] == '\f'
                           || text[at] == '\r')) at++;
    bool negative = false;
    if (at < length && (text[at] == '-' || text[at] == '+')) {
        negative = text[at] == '-';
        at++;
    }
    if (at >= length || text[at] < '0' || text[at] > '9') return false;
    int64_t value = 0;
    bool overflow = false;
    for (; at < length && text[at] >= '0' && text[at] <= '9'; at++) {
        if (value > INT64_C(0x100000000)) overflow = true;
        else value = value * 10 + (text[at] - '0');
    }
    *out = overflow ? (negative ? INT64_MIN : INT64_MAX)
                    : (negative ? -value : value);
    return true;
}

static bool reflect_integer_value(JSContext *context, JSValueConst value,
                                  int64_t *out)
{
    if (!JS_IsString(value)) return false;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, value);
    if (text == NULL) {
        JS_FreeValue(context, JS_GetException(context));
        return false;
    }
    bool parsed = reflect_parse_integer(text, length, out);
    JS_FreeCString(context, text);
    return parsed;
}

static JSValue reflect_keyword(JSContext *context, JSValueConst value,
                               const ReflectKeywords *set)
{
    if (!JS_IsString(value))
        return set->missing == NULL ? JS_NULL
                                    : JS_NewString(context, set->missing);
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, value);
    if (text == NULL) return JS_EXCEPTION;
    const char *match = set->invalid;
    for (size_t i = 0; set->keywords[i] != NULL; i++) {
        const char *keyword = set->keywords[i];
        if (strlen(keyword) == length
            && (length == 0 || strncasecmp(keyword, text, length) == 0)) {
            match = keyword;
            break;
        }
    }
    JS_FreeCString(context, text);
    return JS_NewString(context, match);
}

static JSValue reflect_url(JSContext *context, ScriptRuntime *host,
                           JSValueConst value)
{
    const char *base = js_rt_bridge_calculated_base_url(&host->bridge);
    JSValue arguments[2] = {
        JS_DupValue(context, value),
        JS_NewString(context, base == NULL ? "about:blank" : base)
    };
    JSValue url = JS_IsException(arguments[1]) ? JS_EXCEPTION
        : JS_CallConstructor(context, host->reflect_hooks[REFLECT_HOOK_URL],
                             2, arguments);
    JS_FreeValue(context, arguments[0]);
    JS_FreeValue(context, arguments[1]);
    if (JS_IsException(url)) {
        /* An unparseable URL reads as the attribute's own value. */
        JS_FreeValue(context, JS_GetException(context));
        return JS_DupValue(context, value);
    }
    JSValue href = JS_GetPropertyStr(context, url, "href");
    JS_FreeValue(context, url);
    return href;
}

static JSValue js_reflect_get(JSContext *context, JSValueConst this_value,
                              int magic)
{
    const ReflectedAttribute *entry = &reflected_attributes[magic];
    ScriptRuntime *host = reflect_host(context);
    if (host == NULL) return JS_ThrowTypeError(context, "Illegal invocation");
    if (entry->kind == REFLECT_TEXT_CONTENT)
        return JS_GetPropertyStr(context, this_value, "textContent");
    JSValue value = reflect_read(context, host, this_value, entry);
    if (JS_IsException(value)) return value;
    JSValue result;
    int64_t number = 0;
    bool parsed;
    switch (entry->kind) {
    case REFLECT_BOOLEAN:
        result = JS_NewBool(context, !JS_IsNull(value));
        break;
    case REFLECT_URL:
    case REFLECT_URL_OR_DOCUMENT: {
        bool empty = !JS_IsString(value);
        if (!empty) {
            JSValue length = JS_GetPropertyStr(context, value, "length");
            int32_t characters = 0;
            empty = JS_ToInt32(context, &characters, length) == 0
                && characters == 0;
            JS_FreeValue(context, length);
        }
        if (empty && entry->kind == REFLECT_URL_OR_DOCUMENT) {
            const char *document_url = host->bridge.document_url;
            result = JS_NewString(context, document_url == NULL
                                               ? "about:blank" : document_url);
        } else if (!JS_IsString(value)) {
            result = JS_NewString(context, "");
        } else {
            result = reflect_url(context, host, value);
        }
        break;
    }
    case REFLECT_LONG:
        parsed = reflect_integer_value(context, value, &number);
        result = JS_NewInt32(context,
            parsed && number >= INT32_MIN && number <= INT32_MAX
                ? (int32_t) number : entry->value);
        break;
    case REFLECT_UNSIGNED:
    case REFLECT_POSITIVE:
        parsed = reflect_integer_value(context, value, &number);
        if (!parsed || number < (entry->kind == REFLECT_POSITIVE ? 1 : 0)
            || number > INT32_MAX) number = entry->value;
        result = JS_NewUint32(context, (uint32_t) number);
        break;
    case REFLECT_CLAMPED: {
        const int32_t *range = reflect_clamp_ranges[entry->keywords];
        parsed = reflect_integer_value(context, value, &number);
        if (!parsed) number = entry->value;
        else if (number < range[0]) number = range[0];
        else if (number > range[1]) number = range[1];
        result = JS_NewUint32(context, (uint32_t) number);
        break;
    }
    case REFLECT_ENUMERATED:
        result = reflect_keyword(context, value,
                                 &reflect_keyword_sets[entry->keywords]);
        break;
    case REFLECT_NULLABLE:
        result = JS_IsString(value) ? JS_DupValue(context, value) : JS_NULL;
        break;
    default:
        result = JS_IsString(value) ? JS_DupValue(context, value)
                                    : JS_NewString(context, "");
        break;
    }
    JS_FreeValue(context, value);
    return result;
}

static JSValue js_reflect_set(JSContext *context, JSValueConst this_value,
                              JSValueConst value, int magic)
{
    const ReflectedAttribute *entry = &reflected_attributes[magic];
    ScriptRuntime *host = reflect_host(context);
    if (host == NULL) return JS_ThrowTypeError(context, "Illegal invocation");
    switch (entry->kind) {
    case REFLECT_TEXT_CONTENT: {
        JSValue text = JS_ToString(context, value);
        if (JS_IsException(text)) return text;
        int set = JS_SetPropertyStr(context, this_value, "textContent", text);
        return set < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    case REFLECT_BOOLEAN:
        return JS_ToBool(context, value)
            ? reflect_write(context, host, this_value, entry,
                            JS_NewString(context, ""))
            : reflect_remove(context, host, this_value, entry);
    case REFLECT_NULLABLE:
        if (JS_IsNull(value) || JS_IsUndefined(value))
            return reflect_remove(context, host, this_value, entry);
        return reflect_write(context, host, this_value, entry,
                             JS_ToString(context, value));
    case REFLECT_STRING_NULL_EMPTY:
        return reflect_write(context, host, this_value, entry,
                             JS_IsNull(value) ? JS_NewString(context, "")
                                              : JS_ToString(context, value));
    case REFLECT_ENUMERATED:
        if (JS_IsNull(value)
            && reflect_keyword_sets[entry->keywords].missing == NULL)
            return reflect_remove(context, host, this_value, entry);
        return reflect_write(context, host, this_value, entry,
                             JS_ToString(context, value));
    case REFLECT_LONG: {
        int32_t number = 0;
        if (JS_ToInt32(context, &number, value) < 0) return JS_EXCEPTION;
        return reflect_write(context, host, this_value, entry,
                             JS_ToString(context, JS_NewInt32(context, number)));
    }
    case REFLECT_UNSIGNED:
    case REFLECT_POSITIVE:
    case REFLECT_CLAMPED: {
        uint32_t number = 0;
        if (JS_ToUint32(context, &number, value) < 0) return JS_EXCEPTION;
        if (entry->kind == REFLECT_POSITIVE && number == 0) {
            JSValue arguments[2] = {
                JS_NewString(context, "The value must be positive"),
                JS_NewString(context, "IndexSizeError")
            };
            JSValue error = JS_CallConstructor(
                context, host->reflect_hooks[REFLECT_HOOK_DOM_EXCEPTION], 2,
                arguments);
            JS_FreeValue(context, arguments[0]);
            JS_FreeValue(context, arguments[1]);
            return JS_IsException(error) ? error : JS_Throw(context, error);
        }
        if (number > INT32_MAX) number = (uint32_t) entry->value;
        return reflect_write(context, host, this_value, entry,
                             JS_ToString(context,
                                         JS_NewUint32(context, number)));
    }
    default:
        return reflect_write(context, host, this_value, entry,
                             JS_ToString(context, value));
    }
}

static bool reflect_capture_hooks(ScriptRuntime *host, JSValueConst global)
{
    JSContext *context = host->context;
    JSValue element = JS_GetPropertyStr(context, global, "Element");
    JSValue prototype = JS_IsObject(element)
        ? JS_GetPropertyStr(context, element, "prototype") : JS_UNDEFINED;
    JS_FreeValue(context, element);
    static const char *const methods[] = {
        "getAttribute", "setAttribute", "removeAttribute"
    };
    bool ok = JS_IsObject(prototype);
    for (int i = 0; ok && i < 3; i++) {
        host->reflect_hooks[i] = JS_GetPropertyStr(context, prototype,
                                                   methods[i]);
        ok = JS_IsFunction(context, host->reflect_hooks[i]);
    }
    JS_FreeValue(context, prototype);
    if (ok) {
        host->reflect_hooks[REFLECT_HOOK_URL] =
            JS_GetPropertyStr(context, global, "URL");
        host->reflect_hooks[REFLECT_HOOK_DOM_EXCEPTION] =
            JS_GetPropertyStr(context, global, "DOMException");
        ok = JS_IsConstructor(context, host->reflect_hooks[REFLECT_HOOK_URL])
            && JS_IsConstructor(context,
                                host->reflect_hooks[REFLECT_HOOK_DOM_EXCEPTION]);
    }
    return ok;
}

/* dom.js leaves the installer of ARIA's element-reference attributes on
   the host channel (compat.js moves it there); it runs only in a realm that
   gets the ARIA reflections, and the channel goes before author code. */
static bool reflect_install_aria_element_references(ScriptRuntime *host,
                                                    JSValueConst global)
{
    JSContext *context = host->context;
    JSValue channel = JS_GetPropertyStr(context, global,
                                        "__tilefinchHostChannel");
    JSValue install = JS_IsObject(channel)
        ? JS_GetPropertyStr(context, channel, "ariaElementReferences")
        : JS_UNDEFINED;
    JS_FreeValue(context, channel);
    bool ok = JS_IsFunction(context, install);
    if (ok) {
        JSValue result = JS_Call(context, install, JS_UNDEFINED, 0, NULL);
        ok = !JS_IsException(result);
        JS_FreeValue(context, result);
    }
    JS_FreeValue(context, install);
    return ok;
}

static bool runtime_install_reflected_attributes(ScriptRuntime *host,
                                                 JSValueConst global)
{
    JSContext *context = host->context;
    if (!reflect_capture_hooks(host, global)) return false;
    const bool aria = site_identity_aria_reflection_site(
        host->bridge.document_url);
    const char *interface_name = NULL;
    JSValue prototype = JS_UNDEFINED;
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(reflected_attributes)
                                     / sizeof(reflected_attributes[0]); i++) {
        const ReflectedAttribute *entry = &reflected_attributes[i];
        if (entry->aria_sites_only && !aria) continue;
        if (interface_name == NULL
            || strcmp(interface_name, entry->interface_name) != 0) {
            JS_FreeValue(context, prototype);
            interface_name = entry->interface_name;
            JSValue constructor = JS_GetPropertyStr(context, global,
                                                    interface_name);
            prototype = JS_IsObject(constructor)
                ? JS_GetPropertyStr(context, constructor, "prototype")
                : JS_UNDEFINED;
            JS_FreeValue(context, constructor);
            if (JS_IsException(prototype)) {
                prototype = JS_UNDEFINED;
                ok = false;
            }
        }
        if (!JS_IsObject(prototype)) continue;
        JSAtom atom = JS_NewAtom(context, entry->idl);
        int present = atom == JS_ATOM_NULL ? -1
            : JS_GetOwnProperty(context, NULL, prototype, atom);
        if (present == 0) {
            char name[48];
            JSCFunctionType get = { .getter_magic = js_reflect_get };
            JSCFunctionType set = { .setter_magic = js_reflect_set };
            snprintf(name, sizeof(name), "get %s", entry->idl);
            JSValue getter = JS_NewCFunction2(context, get.generic, name, 0,
                                              JS_CFUNC_getter_magic, (int) i);
            snprintf(name, sizeof(name), "set %s", entry->idl);
            JSValue setter = JS_IsException(getter) ? JS_EXCEPTION
                : JS_NewCFunction2(context, set.generic, name, 1,
                                   JS_CFUNC_setter_magic, (int) i);
            if (JS_IsException(setter)) {
                JS_FreeValue(context, getter);
                present = -1;
            } else {
                present = JS_DefinePropertyGetSet(
                    context, prototype, atom, getter, setter,
                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0 ? -1 : 1;
            }
        }
        if (atom != JS_ATOM_NULL) JS_FreeAtom(context, atom);
        ok = present >= 0;
    }
    JS_FreeValue(context, prototype);
    if (ok && aria) ok = reflect_install_aria_element_references(host, global);
    return ok;
}
