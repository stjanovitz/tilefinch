/* The streaming preview's layout cache serves styles across preview
   builds while the parser keeps appending. Each case streams a synthetic
   page in 512-byte deliveries and checks every preview against what the
   tree holds at that moment: a style the cache kept from an earlier
   preview must not survive the content that arrived since (sibling tests
   counting from the end, :has() answers and a ::after the cache remembered
   as generating nothing, a stylesheet that arrived, :empty, attributes
   merged into <body>, the measurements of content that grew), wherever the
   parser put it (foster parenting, the adoption agency, after </body>,
   below the frontier's depth bound). The committed page, which adopts the
   preview's cache, is checked the same way. */

#define STREAM_GROWTH_HOST "https://stream-growth.test"

/* Whether `name` is in the comma-separated `only` (every case when empty). */
static bool stream_growth_selected(const char *only, const char *name)
{
    size_t length = strlen(name);
    if (*only == '\0') return true;
    for (const char *at = only; *at != '\0';) {
        const char *end = strchr(at, ',');
        size_t span = end == NULL ? strlen(at) : (size_t) (end - at);
        if (span == length && memcmp(at, name, length) == 0) return true;
        if (end == NULL) break;
        at = end + 1;
    }
    return false;
}

typedef bool (*StreamGrowthCheck)(const LayoutDocument *layout,
                                  size_t *checked, char *why, size_t size);

typedef struct {
    StreamGrowthCheck check;
    size_t previews;
    size_t checked;
    size_t stale;
    char why[160];
} StreamGrowthProbe;

static bool stream_growth_paint(void *opaque, NavigationSession *candidate,
                                const LayoutDocument *layout)
{
    StreamGrowthProbe *probe = opaque;
    if (probe == NULL || candidate == NULL) return false;
    if (layout == NULL) return true;
    probe->previews++;
    char why[160] = {0};
    if (!probe->check(layout, &probe->checked, why, sizeof(why))) {
        if (probe->stale++ == 0)
            snprintf(probe->why, sizeof(probe->why), "preview %zu: %s",
                     probe->previews, why);
    }
    return layout->count != 0;
}

static const char *stream_growth_tag(const lxb_dom_node_t *node,
                                     size_t *length)
{
    return document_element_name((lxb_dom_node_t *) node, length);
}

static bool stream_growth_tag_is(const lxb_dom_node_t *node,
                                 const char *tag)
{
    size_t length = 0;
    const char *name = node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        ? NULL : stream_growth_tag(node, &length);
    return name != NULL && length == strlen(tag)
        && memcmp(name, tag, length) == 0;
}

static bool stream_growth_id_is(const lxb_dom_node_t *node, const char *id)
{
    size_t length = 0;
    const char *value = node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        ? NULL : document_attribute((lxb_dom_node_t *) node, "id", &length);
    return value != NULL && length == strlen(id)
        && memcmp(value, id, length) == 0;
}

static const lxb_dom_node_t *stream_growth_element_sibling(
    const lxb_dom_node_t *node, bool next)
{
    for (node = next ? node->next : node->prev; node != NULL;
         node = next ? node->next : node->prev)
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) return node;
    return NULL;
}

/* Whether the document holds an element with id `id`, as parsed so far. */
static bool stream_growth_has_id(const LayoutDocument *layout,
                                 const char *id)
{
    if (layout->node_box_count == 0) return false;
    const lxb_dom_node_t *root = layout->node_boxes[0].node;
    while (root != NULL && root->parent != NULL) root = root->parent;
    size_t visited = 0;
    for (const lxb_dom_node_t *at = root; at != NULL && visited < 65536;
         visited++) {
        if (stream_growth_id_is(at, id)) return true;
        if (at->first_child != NULL) {
            at = at->first_child;
            continue;
        }
        while (at != NULL && at->next == NULL) at = at->parent;
        if (at != NULL) at = at->next;
    }
    return false;
}

static const LayoutNodeBox *stream_growth_box(const LayoutDocument *layout,
                                              const char *id)
{
    for (size_t i = 0; i < layout->node_box_count; i++)
        if (stream_growth_id_is(layout->node_boxes[i].node, id))
            return &layout->node_boxes[i];
    return NULL;
}

static bool stream_growth_draws_text(const LayoutDocument *layout,
                                     const char *text)
{
    size_t length = strlen(text);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT && command->text != NULL
            && command->text_length == length
            && memcmp(command->text, text, length) == 0) return true;
    }
    return false;
}

/* Streams `html` (one document record) in 512-byte deliveries; true when
   the load committed, with every preview checked by `probe->check`. */
static bool stream_growth_load(const char *label, const char *html,
                               size_t length, StreamGrowthProbe *probe,
                               size_t *style_hits)
{
    char directory[64] = "/tmp/tilefinch-stream-growth-XXXXXX";
    if (mkdtemp(directory) == NULL) return false;
    char file[128], error[256] = {0};
    snprintf(file, sizeof(file), "%s/0000.body", directory);
    FILE *out = fopen(file, "wb");
    bool ok = out != NULL && fwrite(html, 1, length, out) == length;
    if (out != NULL) ok = fclose(out) == 0 && ok;
    snprintf(file, sizeof(file), "%s/0000.meta", directory);
    out = ok ? fopen(file, "wb") : NULL;
    ok = out != NULL && fprintf(out,
        "psp-http-trace=1\nmethod=GET\nurl=" STREAM_GROWTH_HOST
        "/document\nsuccess=1\nasync-delay-pumps=0\nexternal-cancel=0\n"
        "transport-timeout=0\nerror=\nstatus=200\nlength=%zu\n"
        "effective-url=" STREAM_GROWTH_HOST "/document\n"
        "content-type=text/html; charset=utf-8\netag=\nlast-modified=\n"
        "cf-mitigated=\naccept-ch=\ncritical-ch=\nserver=fixture\n"
        "cf-ray=\nset-cookie-count=0\n", length) > 0;
    if (out != NULL) ok = fclose(out) == 0 && ok;
    snprintf(file, sizeof(file), "%s/trace.meta", directory);
    out = ok ? fopen(file, "wb") : NULL;
    ok = out != NULL && fprintf(out,
        "psp-http-trace-clock=1\norigin-ms=1000\ncapture-complete=yes\n"
        "record-count=1\n") > 0;
    if (out != NULL) ok = fclose(out) == 0 && ok;

    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = ok && installed
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, stream_growth_paint, probe)
        && fetch_trace_replay_begin(directory, error, sizeof(error));
    if (ready) {
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_stream_delivery(&navigation, 512, 0, 0, 0, 0, 0);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation, STREAM_GROWTH_HOST "/document",
        256 * 1024, 1000, 480, NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {
            .maximum_body_callbacks = 1,
            .maximum_body_bytes = 512,
            .maximum_time_us = 10000
        },
        .maximum_parser_body_bytes = 512,
        .maximum_parser_time_us = 10000
    };
    for (size_t i = 0; load != NULL && i < 4096; i++) {
        if (navigation_load_pump(load, &quota) != NAVIGATION_LOAD_PENDING)
            break;
    }
    bool loaded = false;
    for (size_t i = 0; load != NULL && !loaded && i < 4096; i++) {
        NavigationLoadStatus status = navigation_load_status(load);
        if (status != NAVIGATION_LOAD_READY_TO_FINISH
            && status != NAVIGATION_LOAD_FINALIZING) break;
        (void) navigation_load_finish(load, &quota);
        loaded = navigation_load_status(load) == NAVIGATION_LOAD_SUCCEEDED;
    }
    if (loaded) {
        char why[160] = {0};
        if (!probe->check(&navigation.page.layout, &probe->checked, why,
                          sizeof(why)) && probe->stale++ == 0)
            snprintf(probe->why, sizeof(probe->why), "committed page: %s",
                     why);
    }
    *style_hits = navigation.performance.layout_reuse_style_hits;
    if (!loaded)
        fprintf(stderr, "%s: status=%d previews=%zu error=\"%s\" "
                "trace=\"%s\"\n", label,
                load == NULL ? -1 : (int) navigation_load_status(load),
                probe->previews, navigation.last_error, error);
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    snprintf(file, sizeof(file), "%s/0000.body", directory);
    (void) unlink(file);
    snprintf(file, sizeof(file), "%s/0000.meta", directory);
    (void) unlink(file);
    snprintf(file, sizeof(file), "%s/trace.meta", directory);
    (void) unlink(file);
    (void) rmdir(directory);
    return loaded && clean;
}

/* `prefix`, then `count` repetitions of `item` with `late` inserted after
   `late_at` of them, then `suffix`. */
static char *stream_growth_repeat(const char *prefix, const char *item,
                                  size_t count, const char *late,
                                  size_t late_at, const char *suffix,
                                  size_t *length)
{
    size_t item_length = strlen(item);
    size_t capacity = strlen(prefix) + strlen(late) + strlen(suffix)
        + count * item_length + 1;
    char *html = malloc(capacity);
    if (html == NULL) return NULL;
    size_t at = strlen(prefix);
    memcpy(html, prefix, at);
    for (size_t i = 0; i < count; i++) {
        if (i == late_at) {
            memcpy(html + at, late, strlen(late));
            at += strlen(late);
        }
        memcpy(html + at, item, item_length);
        at += item_length;
    }
    memcpy(html + at, suffix, strlen(suffix));
    at += strlen(suffix);
    html[at] = '\0';
    *length = at;
    return html;
}

/* A page: `head` styles, a visible heading, `top` markup, then `count`
   repetitions of `item` inside <div id=list>, with `late` markup inserted
   after `late_at` of them. */
static char *stream_growth_page(const char *head, const char *top,
                                const char *item, size_t count,
                                const char *late, size_t late_at,
                                size_t *length)
{
    size_t capacity = 256 + strlen(head) + strlen(top);
    char *prefix = malloc(capacity);
    if (prefix == NULL) return NULL;
    snprintf(prefix, capacity,
        "<!doctype html><title>Stream growth</title><style>%s</style></head>"
        "<body><h1>Streaming page</h1>%s<div id=list>", head, top);
    char *html = stream_growth_repeat(prefix, item, count, late, late_at,
                                      "</div></body>", length);
    free(prefix);
    return html;
}

static bool stream_growth_run(const char *label, const char *html,
                              size_t length, StreamGrowthCheck check)
{
    StreamGrowthProbe probe = {.check = check};
    size_t style_hits = 0;
    bool loaded = stream_growth_load(label, html, length, &probe,
                                     &style_hits);
    /* Several previews, and the later ones took styles from the cache:
       otherwise nothing here was exercised. */
    bool ok = loaded && probe.previews >= 3 && probe.checked != 0
        && style_hits != 0 && probe.stale == 0;
    if (!ok)
        fprintf(stderr, "%s: loaded=%d previews=%zu checked=%zu hits=%zu "
                "stale=%zu %s\n", label, loaded, probe.previews,
                probe.checked, style_hits, probe.stale, probe.why);
    return ok;
}

/* #list holds b, u and s blocks: the last child is tall, the u right after
   the last b is wide, an s after the last u wider. */
static bool stream_growth_check_siblings(const LayoutDocument *layout,
                                         size_t *checked, char *why,
                                         size_t size)
{
    const lxb_dom_node_t *list = NULL, *last_b = NULL, *last_u = NULL;
    const LayoutNodeBox *list_box = stream_growth_box(layout, "list");
    if (list_box == NULL) return true;
    list = list_box->node;
    for (const lxb_dom_node_t *child = list->last_child; child != NULL;
         child = child->prev) {
        if (last_b == NULL && stream_growth_tag_is(child, "b")) last_b = child;
        if (last_u == NULL && stream_growth_tag_is(child, "u")) last_u = child;
    }
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        const lxb_dom_node_t *node = box->node;
        if (node == NULL || node->parent != list
            || node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        (*checked)++;
        bool last = stream_growth_element_sibling(node, true) == NULL;
        if ((box->height > 0) != last) {
            snprintf(why, size, ":last-child height=%d last=%d",
                     box->height, last);
            return false;
        }
        int wide = 100;
        if (stream_growth_tag_is(node, "u")
            && stream_growth_element_sibling(node, false) == last_b
            && last_b != NULL) wide = 200;
        if (stream_growth_tag_is(node, "s") && last_u != NULL) {
            for (const lxb_dom_node_t *at = node->prev; at != NULL;
                 at = at->prev)
                if (at == last_u) {
                    wide = 300;
                    break;
                }
        }
        if (box->width != wide) {
            snprintf(why, size, "%s width=%d want=%d",
                     stream_growth_tag_is(node, "u") ? "b:last-of-type+u"
                                                     : "u:last-of-type~s",
                     box->width, wide);
            return false;
        }
    }
    return true;
}

/* A sibling test counting from the end, and through + and ~, on cached
   siblings when later ones arrive. */
static bool test_streaming_preview_restyles_trailing_siblings(void)
{
    size_t length = 0;
    char *html = stream_growth_page(
        "#list>*{display:block;height:0;width:100px;margin:0}"
        "#list>:last-child{height:4px}"
        "#list>b:last-of-type+u{width:200px}"
        "#list>u:last-of-type~s{width:300px}",
        "", "<b></b><u></u><s></s>", 2200, "", SIZE_MAX, &length);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-siblings", html, length,
                             stream_growth_check_siblings);
    free(html);
    return ok;
}

/* #list holds 40 plain <p> blocks, then p and div.t pairs: the <em> of the
   last child, when a .t, is narrow. */
static bool stream_growth_check_keyed_trailing(const LayoutDocument *layout,
                                               size_t *checked, char *why,
                                               size_t size)
{
    const LayoutNodeBox *list_box = stream_growth_box(layout, "list");
    if (list_box == NULL) return true;
    const lxb_dom_node_t *list = list_box->node;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        const lxb_dom_node_t *node = box->node;
        if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || node->parent == NULL) continue;
        bool em = node->parent->parent == list
            && stream_growth_tag_is(node, "em");
        if (node->parent != list && !em) continue;
        (*checked)++;
        const lxb_dom_node_t *child = em ? node->parent : node;
        bool last = stream_growth_element_sibling(child, true) == NULL
            && stream_growth_tag_is(child, "div");
        int want = em ? (last ? 50 : 100) : 0;
        int got = em ? box->width : box->height;
        if (got != want || (!em && box->width != 100)) {
            snprintf(why, size, "%s %s=%d want=%d",
                     em ? ".t:last-child>em" : "#list>*",
                     em ? "width" : "height", got, want);
            return false;
        }
    }
    return true;
}

#define STREAM_GROWTH_PAD \
    "pppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp"

/* A test counting from the end that only the siblings' own keys place:
   the siblings before the first one carrying a key keep their styles, the
   rest restyle with their subtrees (here a child combinator reads the
   test while the sibling's own style stays the same). */
static bool test_streaming_preview_restyles_keyed_trailing_siblings(void)
{
    char top[64 * 8];
    size_t at = 0;
    for (size_t i = 0; i < 40; i++)
        at += (size_t) snprintf(top + at, sizeof(top) - at, "<p></p>");
    char prefix[1024];
    snprintf(prefix, sizeof(prefix),
        "<!doctype html><title>Stream growth</title><style>"
        "#list>*{display:block;height:0;width:100px;margin:0}"
        "#list em{display:block;height:0}"
        "#list>.t:last-child>em{width:50px}"
        "</style></head><body><h1>Streaming page</h1><div id=list>%s", top);
    size_t length = 0;
    /* Few enough children for the sibling walk's bound, padded for bytes
       enough to stream several previews. */
    char *html = stream_growth_repeat(prefix,
        "<p data-pad=" STREAM_GROWTH_PAD "></p><div class=t data-pad="
        STREAM_GROWTH_PAD "><em></em></div>", 230, "", SIZE_MAX,
        "</div></body>", &length);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-keyed-siblings", html, length,
                             stream_growth_check_keyed_trailing);
    free(html);
    return ok;
}

/* #flag is tall until an element with id `late` arrives; then body::after
   generates "zq". */
static bool stream_growth_check_has(const LayoutDocument *layout,
                                    size_t *checked, char *why, size_t size)
{
    const LayoutNodeBox *flag = stream_growth_box(layout, "flag");
    if (flag == NULL) return true;
    (*checked)++;
    bool late = stream_growth_has_id(layout, "late");
    bool tall = flag->height > 0;
    bool after = stream_growth_draws_text(layout, "zq");
    if (tall == late || after != late) {
        snprintf(why, size, "late=%d: :has() subject height=%d, "
                 "body:has()::after drawn=%d", late, flag->height, after);
        return false;
    }
    return true;
}

/* A :has() answer the arriving content moves: a cached descendant's style
   and an ancestor's ::after the cache remembered as generating nothing.
   #late sits within the :has() walk's visit bound
   (STYLE_RELATIVE_SELECTOR_VISIT_LIMIT); past it the walk answers no
   match, fresh or cached alike. */
static bool test_streaming_preview_restyles_has_answers(void)
{
    size_t length = 0;
    char *html = stream_growth_page(
        "#list>p{display:block;height:0;margin:0}"
        "#flag{display:block;height:0}"
        "body:not(:has(#late)) #flag{height:6px}"
        "body:has(#late)::after{content:\"zq\"}",
        "<div id=flag></div>",
        "<p title=0123456789012345678901234567890123456789></p>", 2000,
        "<p id=late></p>", 900, &length);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-has", html, length,
                             stream_growth_check_has);
    free(html);
    return ok;
}

/* #sized is tall and #marked's ::before draws "qz" once the <style>
   holding id `late-style` has arrived. */
static bool stream_growth_check_sheet(const LayoutDocument *layout,
                                      size_t *checked, char *why,
                                      size_t size)
{
    const LayoutNodeBox *sized = stream_growth_box(layout, "sized");
    if (sized == NULL) return true;
    (*checked)++;
    bool late = stream_growth_has_id(layout, "late-style");
    if ((sized->height > 0) != late) {
        snprintf(why, size, "late rule height=%d arrived=%d", sized->height,
                 late);
        return false;
    }
    if (stream_growth_draws_text(layout, "qz") != late) {
        snprintf(why, size, "late ::before rule arrived=%d", late);
        return false;
    }
    return true;
}

/* A stylesheet arriving mid-stream restyles what the cache holds,
   ::before included. */
static bool test_streaming_preview_follows_arriving_stylesheet(void)
{
    size_t length = 0;
    char *html = stream_growth_page(
        "#list>p{display:block;height:0;margin:0}"
        "#sized{display:block;height:0}",
        "<div id=sized></div><div id=marked>m</div>", "<p></p>", 9000,
        "<style id=late-style>#sized{height:5px}"
        "#marked::before{content:\"qz\"}</style>", 5000, &length);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-sheet", html, length,
                             stream_growth_check_sheet);
    free(html);
    return ok;
}

#define STREAM_GROWTH_HEAD(css) \
    "<!doctype html><title>Stream growth</title><style>" css \
    "</style></head><body><h1>Streaming page</h1>"

/* The <body> of the document `layout` was built from. */
static const lxb_dom_node_t *stream_growth_body(const LayoutDocument *layout)
{
    if (layout->node_box_count == 0) return NULL;
    const lxb_dom_node_t *at = layout->node_boxes[0].node;
    while (at != NULL && at->parent != NULL) at = at->parent;
    for (at = at == NULL ? NULL : at->first_child; at != NULL; at = at->next)
        if (stream_growth_tag_is(at, "html")) break;
    for (at = at == NULL ? NULL : at->first_child; at != NULL; at = at->next)
        if (stream_growth_tag_is(at, "body")) return at;
    return NULL;
}

/* Every element box below `scope` (its children only, unless `deep`) is
   tall exactly when it is its parent's last element child. */
static bool stream_growth_last_children(const LayoutDocument *layout,
                                        const lxb_dom_node_t *scope,
                                        bool deep, size_t *checked,
                                        char *why, size_t size)
{
    for (size_t i = 0; scope != NULL && i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        const lxb_dom_node_t *node = box->node;
        if (node == NULL || node == scope
            || node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !(deep ? document_node_within(node, scope)
                      : node->parent == scope)) continue;
        (*checked)++;
        bool last = stream_growth_element_sibling(node, true) == NULL;
        if ((box->height > 0) != last) {
            size_t length = 0;
            const char *tag = stream_growth_tag(node, &length);
            snprintf(why, size, ":last-child <%.*s> height=%d last=%d",
                     (int) length, tag == NULL ? "" : tag, box->height,
                     last);
            return false;
        }
    }
    return true;
}

static bool stream_growth_check_list_last(const LayoutDocument *layout,
                                          size_t *checked, char *why,
                                          size_t size)
{
    const LayoutNodeBox *list = stream_growth_box(layout, "list");
    return list == NULL
        || stream_growth_last_children(layout, list->node, false, checked,
                                       why, size);
}

/* #list's children tall when last; body::after once #late arrived. */
static bool stream_growth_check_foster(const LayoutDocument *layout,
                                       size_t *checked, char *why,
                                       size_t size)
{
    if (!stream_growth_check_list_last(layout, checked, why, size))
        return false;
    bool late = stream_growth_has_id(layout, "late");
    if (stream_growth_draws_text(layout, "zq") != late) {
        snprintf(why, size, "body:has()::after drawn=%d late=%d", !late,
                 late);
        return false;
    }
    return true;
}

#define STREAM_GROWTH_LAST_RULES \
    "#list>*{display:block;height:0;margin:0}" \
    "#list>:last-child{height:4px}"

/* An element foster-parented out of a table is where the parser keeps
   inserting, though it sits before the table, off the document's last
   child chain. */
static bool test_streaming_preview_follows_fostered_content(void)
{
    size_t length = 0;
    char *html = stream_growth_page(
        STREAM_GROWTH_LAST_RULES "body:has(#late)::after{content:\"zq\"}",
        "<table><tr>",
        "<p title=0123456789012345678901234567890123456789></p>", 2000,
        "<p id=late></p>", 900, &length);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-foster", html, length,
                             stream_growth_check_foster);
    free(html);
    return ok;
}

/* #float holds the text fostered out of its table: a word once "bbb..."
   has merged into the "aaa" before the table, as wide as it then. */
static bool stream_growth_check_fostered_text(const LayoutDocument *layout,
                                              size_t *checked, char *why,
                                              size_t size)
{
    const LayoutNodeBox *box = stream_growth_box(layout, "float");
    if (box == NULL || box->node->first_child == NULL) return true;
    (*checked)++;
    size_t length = 0;
    (void) document_text_data(box->node->first_child, &length);
    bool merged = length > 3;
    if (merged ? box->width < 200 : box->width > 100) {
        snprintf(why, size, "fostered text width=%d length=%zu", box->width,
                 length);
        return false;
    }
    return true;
}

static bool test_streaming_preview_measures_fostered_text(void)
{
    /* The rows come first; then only ignored end tags pad the stream
       around the merge, so the table grows no more. */
    size_t length = 0;
    char *rows = stream_growth_repeat(
        STREAM_GROWTH_HEAD("#float{float:left}")
        "<div id=float><table>aaa<tr></tr>",
        "<tr></tr>", 600, "", SIZE_MAX, "", &length);
    char *html = rows == NULL ? NULL : stream_growth_repeat(
        rows, "</zz>", 12000, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 6000,
        "</table></div></body>", &length);
    free(rows);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-foster-text", html, length,
                             stream_growth_check_fostered_text);
    free(html);
    return ok;
}

static bool stream_growth_check_body_last(const LayoutDocument *layout,
                                          size_t *checked, char *why,
                                          size_t size)
{
    return stream_growth_last_children(layout, stream_growth_body(layout),
                                       false, checked, why, size);
}

/* Comments after </body> and </html> follow <body> and the root element,
   then the parser goes back to inserting into <body>: div.b arrives after
   them, and div.a is no longer body's last child. */
static bool test_streaming_preview_follows_content_after_body(void)
{
    size_t length = 0;
    char *before = stream_growth_repeat(
        STREAM_GROWTH_HEAD("body>*{display:block;height:0;margin:0}"
                           "body>:last-child{height:4px}")
        "<div class=a>",
        "<p></p>", 300, "", SIZE_MAX, "</div></body>", &length);
    char *comments = before == NULL ? NULL : stream_growth_repeat(
        before, "<!-- 0123456789012345678901234567 -->", 600, "</html>",
        300, "<div class=b>", &length);
    char *html = comments == NULL ? NULL : stream_growth_repeat(
        comments, "<p></p>", 4000, "", SIZE_MAX, "</div>", &length);
    free(before);
    free(comments);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-after-body", html, length,
                             stream_growth_check_body_last);
    free(html);
    return ok;
}

/* #box is tall while it holds only comments. */
static bool stream_growth_check_empty(const LayoutDocument *layout,
                                      size_t *checked, char *why,
                                      size_t size)
{
    const LayoutNodeBox *box = stream_growth_box(layout, "box");
    if (box == NULL) return true;
    (*checked)++;
    bool content = false;
    for (const lxb_dom_node_t *child = box->node->first_child;
         child != NULL && !content; child = child->next) {
        size_t length = 0;
        content = child->type == LXB_DOM_NODE_TYPE_ELEMENT
            || (child->type == LXB_DOM_NODE_TYPE_TEXT
                && document_text_data((lxb_dom_node_t *) child, &length)
                       != NULL && length != 0);
    }
    if ((box->height > 0) == content) {
        snprintf(why, size, ":empty height=%d content=%d", box->height,
                 content);
        return false;
    }
    return true;
}

static bool test_streaming_preview_restyles_empty_parent(void)
{
    size_t length = 0;
    char *prefix = stream_growth_repeat(
        STREAM_GROWTH_HEAD("#box{display:block;height:0}"
                           "#box:empty{height:5px}"),
        "<p></p>", 400, "", SIZE_MAX, "<div id=box>", &length);
    char *html = prefix == NULL ? NULL : stream_growth_repeat(
        prefix, "<!-- 0123456789012345678901234567890123456789 -->", 2000,
        "<i>x</i>", 1000, "</div></body>", &length);
    free(prefix);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-empty", html, length,
                             stream_growth_check_empty);
    free(html);
    return ok;
}

/* #grow floats around one unbreakable word the parser keeps extending:
   each "a" arrives as its own character token (the ignored </zz> end tags
   between them pad the stream), merged into the same text node. */
static bool stream_growth_check_word(const LayoutDocument *layout,
                                     size_t *checked, char *why, size_t size)
{
    const LayoutNodeBox *box = stream_growth_box(layout, "grow");
    if (box == NULL) return true;
    const lxb_dom_node_t *text = box->node;
    for (size_t depth = 0; text != NULL && depth < 512
         && text->type != LXB_DOM_NODE_TYPE_TEXT; depth++)
        text = text->last_child;
    if (text == NULL) return true;
    (*checked)++;
    size_t length = 0;
    (void) document_text_data((lxb_dom_node_t *) text, &length);
    /* About 12 px a letter, up to the width the float has. */
    size_t wanted = length * 10u < 960u ? length * 10u : 960u;
    if ((size_t) box->width < wanted) {
        snprintf(why, size, "growing word width=%d length=%zu", box->width,
                 length);
        return false;
    }
    return true;
}

#define STREAM_GROWTH_WORD_PAD "</zz></zz></zz></zz></zz></zz></zz></zz>"
#define STREAM_GROWTH_WORD_ITEM \
    "a" STREAM_GROWTH_WORD_PAD STREAM_GROWTH_WORD_PAD STREAM_GROWTH_WORD_PAD \
    STREAM_GROWTH_WORD_PAD STREAM_GROWTH_WORD_PAD

/* `wrap` between #grow and the word. */
static char *stream_growth_word_page(const char *wrap, const char *unwrap,
                                     size_t *length)
{
    char *prefix = stream_growth_repeat(
        STREAM_GROWTH_HEAD("#grow{float:left}"), "<p></p>", 300, "",
        SIZE_MAX, "<div id=grow>", length);
    size_t capacity = prefix == NULL ? 0 : strlen(prefix) + strlen(wrap) + 1;
    char *opened = capacity == 0 ? NULL : malloc(capacity);
    if (opened != NULL) snprintf(opened, capacity, "%s%s", prefix, wrap);
    free(prefix);
    capacity = strlen(unwrap) + 16;
    char *closed = opened == NULL ? NULL : malloc(capacity);
    if (closed != NULL) snprintf(closed, capacity, "%s</div></body>", unwrap);
    char *html = closed == NULL ? NULL : stream_growth_repeat(
        opened, STREAM_GROWTH_WORD_ITEM, 400, "", SIZE_MAX, closed, length);
    free(opened);
    free(closed);
    return html;
}

static bool test_streaming_preview_measures_growing_text(void)
{
    size_t length = 0;
    char *html = stream_growth_word_page("", "", &length);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-text", html, length,
                             stream_growth_check_word);
    free(html);
    return ok;
}

/* #flag is tall once a second <body> tag merged class=late into body. */
static bool stream_growth_check_body_attribute(const LayoutDocument *layout,
                                               size_t *checked, char *why,
                                               size_t size)
{
    const LayoutNodeBox *flag = stream_growth_box(layout, "flag");
    const lxb_dom_node_t *body = stream_growth_body(layout);
    if (flag == NULL || body == NULL) return true;
    (*checked)++;
    size_t length = 0;
    bool late = document_attribute((lxb_dom_node_t *) body, "class",
                                   &length) != NULL;
    if ((flag->height > 0) != late) {
        snprintf(why, size, "body.late height=%d merged=%d", flag->height,
                 late);
        return false;
    }
    return true;
}

static bool test_streaming_preview_follows_merged_body_attributes(void)
{
    size_t length = 0;
    char *html = stream_growth_page(
        "#flag{display:block;height:0}body.late #flag{height:6px}",
        "<div id=flag></div>",
        "<p title=0123456789012345678901234567890123456789></p>", 2000,
        "<body class=late>", 900, &length);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-body-attr", html, length,
                             stream_growth_check_body_attribute);
    free(html);
    return ok;
}

/* The growing word sits below more open elements than the frontier
   records; #grow's width still follows it. */
static bool test_streaming_preview_restyles_past_depth_bound(void)
{
    char wrap[160 * 5 + 1], unwrap[160 * 6 + 1];
    for (size_t i = 0; i < 160; i++) {
        memcpy(wrap + i * 5, "<div>", 5);
        memcpy(unwrap + i * 6, "</div>", 6);
    }
    wrap[160 * 5] = '\0';
    unwrap[160 * 6] = '\0';
    size_t length = 0;
    char *html = stream_growth_word_page(wrap, unwrap, &length);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-deep", html, length,
                             stream_growth_check_word);
    free(html);
    return ok;
}

/* A misnested </b> runs the adoption agency: <p id=moved> leaves
   <b id=fmt> (now empty) for #list, and its spans move into a new <b>
   inside it (a copy of #fmt, id included). #fmt is tall while empty; a
   span is tall while inside #list's #fmt. The <p> and the new <b> compute
   alike, so a moved span keeps its cache key. */
static bool stream_growth_check_adoption(const LayoutDocument *layout,
                                         size_t *checked, char *why,
                                         size_t size)
{
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        const lxb_dom_node_t *node = box->node;
        bool tall;
        if (stream_growth_id_is(node, "fmt")
            && stream_growth_id_is(node->parent, "list")) {
            tall = node->first_child == NULL;
        } else if (stream_growth_tag_is(node, "span")) {
            /* Inside the original #fmt, #list's child (the new <b> is a
               copy, id included). */
            tall = false;
            for (const lxb_dom_node_t *up = node->parent; up != NULL;
                 up = up->parent)
                tall = tall || (stream_growth_id_is(up, "fmt")
                                && stream_growth_id_is(up->parent, "list"));
        } else continue;
        (*checked)++;
        if ((box->height > 0) != tall) {
            snprintf(why, size, "%s height=%d want tall=%d",
                     stream_growth_tag_is(node, "span") ? "#list>#fmt span"
                                                        : "emptied #fmt",
                     box->height, tall);
            return false;
        }
    }
    return true;
}

static bool test_streaming_preview_follows_adoption_agency(void)
{
    size_t length = 0;
    char *prefix = stream_growth_repeat(
        STREAM_GROWTH_HEAD(
            "#list p,#list b{display:block;margin:0;font-weight:400}"
            "#list>#fmt{height:0}#list>#fmt:empty{height:9px}"
            "#list span{display:block;height:0}"
            "#list>#fmt span{height:3px}")
        "<div>", "<p></p>", 300, "", SIZE_MAX,
        "</div><div id=list><b id=fmt><p id=moved>", &length);
    char *html = prefix == NULL ? NULL : stream_growth_repeat(
        prefix,
        "<span></span>" STREAM_GROWTH_WORD_PAD STREAM_GROWTH_WORD_PAD
        STREAM_GROWTH_WORD_PAD STREAM_GROWTH_WORD_PAD STREAM_GROWTH_WORD_PAD,
        200, "</b>", 100, "</p></div></body>", &length);
    free(prefix);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-adoption", html, length,
                             stream_growth_check_adoption);
    free(html);
    return ok;
}

/* #grow floats around 10px inline blocks the parser keeps appending
   (ignored end tags pad the stream between them); it widens with them. */
static bool stream_growth_check_children(const LayoutDocument *layout,
                                         size_t *checked, char *why,
                                         size_t size)
{
    const LayoutNodeBox *box = stream_growth_box(layout, "grow");
    if (box == NULL) return true;
    (*checked)++;
    size_t children = 0;
    for (const lxb_dom_node_t *child = box->node->first_child;
         child != NULL; child = child->next)
        children += child->type == LXB_DOM_NODE_TYPE_ELEMENT;
    size_t wanted = children * 10u < 960u ? children * 10u : 960u;
    if ((size_t) box->width < wanted) {
        snprintf(why, size, "growing float width=%d children=%zu",
                 box->width, children);
        return false;
    }
    return true;
}

static bool test_streaming_preview_measures_appended_children(void)
{
    size_t length = 0;
    char *prefix = stream_growth_repeat(
        STREAM_GROWTH_HEAD("#grow{float:left;white-space:nowrap}"
                           "#grow>i{display:inline-block;width:10px;"
                           "height:1px}"),
        "<p></p>", 300, "", SIZE_MAX, "<div id=grow>", &length);
    char *html = prefix == NULL ? NULL : stream_growth_repeat(
        prefix, "<i></i>" STREAM_GROWTH_WORD_PAD STREAM_GROWTH_WORD_PAD
                STREAM_GROWTH_WORD_PAD STREAM_GROWTH_WORD_PAD
                STREAM_GROWTH_WORD_PAD, 400, "", SIZE_MAX,
        "</div></body>", &length);
    free(prefix);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-children", html, length,
                             stream_growth_check_children);
    free(html);
    return ok;
}

/* After </div> closes the fostered #list, the parser fosters more content
   before the table, after #list: off both the path and the table's last
   child chain. body::after once #late, one of those, arrived. */
static bool stream_growth_check_fostered_after(const LayoutDocument *layout,
                                               size_t *checked, char *why,
                                               size_t size)
{
    const lxb_dom_node_t *body = stream_growth_body(layout);
    if (body == NULL) return true;
    (*checked)++;
    bool late = stream_growth_has_id(layout, "late");
    if (stream_growth_draws_text(layout, "zq") != late) {
        snprintf(why, size, "body:has(>#late)::after drawn=%d late=%d",
                 !late, late);
        return false;
    }
    return true;
}

static bool test_streaming_preview_follows_content_fostered_after(void)
{
    size_t length = 0;
    char *html = stream_growth_page(
        "#list>p{display:block;height:0;margin:0}"
        "body:has(>#late)::after{content:\"zq\"}",
        "<table><tr>",
        "<p title=0123456789012345678901234567890123456789></p>", 2000,
        "</div><p id=late></p><div>", 900, &length);
    bool ok = html != NULL
        && stream_growth_run("stream-growth-foster-after", html, length,
                             stream_growth_check_fostered_after);
    free(html);
    return ok;
}
