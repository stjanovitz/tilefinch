/* MDN and Guardian parity repros. Each test reduces one engine gap found by
   comparing the fidelity replays of developer.mozilla.org and
   theguardian.com/international against Chrome to a few elements, and
   checks the layout a standards browser produces for it. */

typedef struct {
    Budget budget;
    PocDocument document;
    Stylesheet sheet;
    LayoutDocument layout;
    bool document_ready;
    bool sheet_ready;
    bool layout_ready;
} ParityPage;

static bool parity_page_load(ParityPage *page, const char *html)
{
    *page = (ParityPage) {0};
    budget_init(&page->budget, 8u * MIB);
    budget_install_lexbor(&page->budget);
    page->document_ready = document_parse(&page->document, &page->budget,
                                          html, strlen(html), 51);
    page->sheet_ready = page->document_ready
        && stylesheet_build(&page->sheet, &page->budget, &page->document,
                            480);
    page->layout_ready = page->sheet_ready
        && layout_build(&page->layout, &page->budget, &page->document,
                        &page->sheet, NULL, NULL, 480);
    return page->layout_ready;
}

static bool parity_page_destroy(ParityPage *page)
{
    if (page->layout_ready) layout_destroy(&page->layout);
    if (page->sheet_ready) stylesheet_destroy(&page->sheet);
    if (page->document_ready) document_destroy(&page->document);
    return page->budget.current == 0;
}

static const LayoutNodeBox *parity_box(ParityPage *page, const char *id)
{
    lxb_dom_node_t *node = find_id(
        lxb_dom_interface_node(page->document.html), id);
    return node == NULL ? NULL : layout_box_for_node(&page->layout, node);
}

/* The first solid fill of `color`, or NULL. */
static const DrawCommand *parity_fill(const LayoutDocument *layout,
                                      uint32_t color, size_t nth)
{
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_FILL_RECT && command->color == color
            && command->width > 0 && command->opacity_scale != 0
            && nth-- == 0) return command;
    }
    return NULL;
}

/* Guardian's pillar navigation: each link's `::after` underline is
   absolutely positioned in the positioned list item (the link itself is
   not positioned), sits below the 28 px item through margin-bottom:-5px
   and a margin-left inset, and is lifted into view only on the current
   pillar by translateY(-5px); the others are clipped by the item's
   overflow:hidden. Each item's separator is a bordered empty ::after. */
static int test_parity_positioned_pseudo_containing_block(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}"
        "ul{display:flex;margin:0;padding:0 10px}"
        "li{display:flex;position:relative;overflow:hidden;height:28px;"
        "list-style:none}"
        "li::after{content:'';border-left:1px solid #00ff00;display:flex;"
        "position:absolute;right:0;top:0;bottom:0;height:calc(100% - 4px);"
        "margin-top:4px}"
        "a{font-size:17px;flex-grow:1;padding-left:6px;padding-right:4px}"
        "a::after{height:5px;background:#ff0000;content:'';display:block;"
        "position:absolute;bottom:0;left:0;right:0;width:calc(100% - 7px);"
        "margin-left:6px;margin-bottom:-5px}"
        "a.current::after{transform:translateY(-5px)}"
        "#inline-cb{position:relative;height:30px}"
        "#inline-cb span::after{content:'';position:absolute;bottom:0;"
        "left:0;width:20px;height:3px;background:#0000ff}"
        "</style><body><ul><li id=first><a class=current>News</a></li>"
        "<li id=second><a>Opinion</a></li></ul>"
        "<div id=inline-cb><span>inline</span></div></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *first = parity_box(&page, "first");
    const LayoutNodeBox *second = parity_box(&page, "second");
    const LayoutNodeBox *inline_cb = parity_box(&page, "inline-cb");
    const DrawCommand *bar = parity_fill(&page.layout, 0xff0000u, 0);
    const DrawCommand *hidden_bar = parity_fill(&page.layout, 0xff0000u, 1);
    const DrawCommand *rule = parity_fill(&page.layout, 0x00ff00u, 0);
    const DrawCommand *inline_bar = parity_fill(&page.layout, 0x0000ffu, 0);
    bool ok = first != NULL && second != NULL && inline_cb != NULL
        && bar != NULL && hidden_bar != NULL && rule != NULL
        && inline_bar != NULL
        /* Bottom edge of the item, inset by the margin. */
        && bar->x == first->x + 6 && bar->y == first->y + 28 - 5
        && bar->height == 5 && bar->width == first->width - 7
        /* The other pillar's bar lies just below its item. */
        && hidden_bar->y == second->y + 28
        /* A 1 px separator at the item's right edge, 4 px from its top. */
        && rule->width == 1 && rule->x == first->x + first->width - 1
        && rule->y == first->y + 4 && rule->height == 24
        /* An inline element's positioned pseudo is painted, anchored to
           the positioned block. */
        && inline_bar->y == inline_cb->y + 30 - 3
        && inline_bar->x == inline_cb->x;
    if (!ok)
        fprintf(stderr,
                "positioned pseudo: item=%d,%d %dx%d bar=%d,%d %dx%d "
                "hidden=%d rule=%d,%d %dx%d inline=%d,%d\n",
                first ? first->x : -1, first ? first->y : -1,
                first ? first->width : -1, first ? first->height : -1,
                bar ? bar->x : -1, bar ? bar->y : -1,
                bar ? bar->width : -1, bar ? bar->height : -1,
                hidden_bar ? hidden_bar->y : -1,
                rule ? rule->x : -1, rule ? rule->y : -1,
                rule ? rule->width : -1, rule ? rule->height : -1,
                inline_bar ? inline_bar->x : -1,
                inline_bar ? inline_bar->y : -1);
    CHECK(ok);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* CSS Text 4 `text-wrap: nowrap` / `text-wrap-mode: nowrap` (MDN's
   breadcrumb bar), `@media not (<condition>)` (MDN's desktop-only heading
   anchors) and a visibility:hidden generated box. */
static int test_parity_text_wrap_media_not(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}"
        "#a,#b{width:100px}#a{text-wrap:nowrap}#b{text-wrap-mode:nowrap}"
        "#c{width:100px;white-space:pre;text-wrap:wrap}"
        "@media not (width <= 769px){#d{color:#ff0000}}"
        "@media not (width >= 769px){#e{color:#00ff00}}"
        "#f::after{content:'';display:block;height:4px;background:#0000ff;"
        "visibility:hidden}"
        "</style><body><div id=a>Standard built-in objects</div>"
        "<div id=b>Standard built-in objects</div>"
        "<div id=c>Standard built-in objects</div>"
        "<p id=d>DESKTOP</p><p id=e>HANDHELD</p><div id=f>F</div></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *a = parity_box(&page, "a");
    const LayoutNodeBox *b = parity_box(&page, "b");
    const LayoutNodeBox *c = parity_box(&page, "c");
    const DrawCommand *desktop = text_command(&page.layout, "DESKTOP");
    const DrawCommand *handheld = text_command(&page.layout, "HANDHELD");
    bool ok = a != NULL && b != NULL && c != NULL && desktop != NULL
        && handheld != NULL
        && a->height < 30 && b->height < 30 && c->height > 30
        && desktop->color == 0x000000u && handheld->color == 0x00ff00u
        && parity_fill(&page.layout, 0x0000ffu, 0) == NULL;
    if (!ok)
        fprintf(stderr,
                "text-wrap/media: heights=%d/%d/%d desktop=%06x "
                "handheld=%06x\n",
                a ? a->height : -1, b ? b->height : -1, c ? c->height : -1,
                desktop ? (unsigned) desktop->color : 0u,
                handheld ? (unsigned) handheld->color : 0u);
    CHECK(ok);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_balanced_text_ignores_collapsible_indentation(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}"
        "a{display:inline-flex;font:700 16px sans-serif;line-height:1;"
        "padding:8px 24px;border:1px solid;text-wrap:balance}"
        ".pretty{text-wrap:pretty}.preserved{white-space:pre}"
        "div{margin-bottom:4px}</style><body>"
        "<div><a id=plain>See more examples</a></div>"
        "<div><a id=indented>\n        See more examples\n      </a></div>"
        "<div><a id=pretty class=pretty>\n     See   more\t examples\n</a></div>"
        "<div><a id=preserved class=preserved>See\nmore examples</a></div>"
        "</body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    FontSet fonts = {0};
    CHECK(font_set_load_selected(&fonts, &page.budget,
          TILEFINCH_TEST_METRIC_SANS_FONT, NULL, NULL,
          TILEFINCH_TEST_METRIC_SANS_BOLD_FONT, NULL, NULL, NULL, 4 * MIB,
          FONT_SET_FACE_SANS | FONT_SET_FACE_SANS_BOLD));
    layout_destroy(&page.layout);
    CHECK(layout_build(&page.layout, &page.budget, &page.document,
          &page.sheet, &fonts, NULL, 480));
    const LayoutNodeBox *plain = parity_box(&page, "plain");
    const LayoutNodeBox *indented = parity_box(&page, "indented");
    const LayoutNodeBox *pretty = parity_box(&page, "pretty");
    const LayoutNodeBox *preserved = parity_box(&page, "preserved");
    if (plain != NULL && indented != NULL
        && (plain->width != indented->width
            || plain->height != indented->height))
        fprintf(stderr, "balanced indentation: plain=%dx%d indented=%dx%d\n",
                plain->width, plain->height, indented->width, indented->height);
    CHECK(plain != NULL && indented != NULL && pretty != NULL
          && preserved != NULL
          && plain->width == indented->width
          && plain->height == indented->height
          && plain->width == pretty->width
          && plain->height == pretty->height
          && preserved->height > plain->height);
    font_set_destroy(&fonts);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* Guardian's front sections and masthead are named-line grids: lines carry
   up to four names (`[content-end title-end hide-end]`), templates run past
   190 characters, `grid-column: title` means title-start / title-end, and
   the menu button spans the logo and pillar rows aligned to their end. */
static int test_parity_named_grid_lines(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}"
        "section{display:grid;grid-template-rows:[headline-start "
        "controls-start] 30px [controls-end headline-end "
        "content-toggleable-start content-start] 30px [content-end "
        "content-toggleable-end bottom-content-start] 30px "
        "[bottom-content-end];grid-template-columns:[decoration-start] 0px "
        "[content-start title-start] repeat(3,minmax(0,1fr)) [hide-start] "
        "minmax(0,1fr) [content-end title-end hide-end] 0px "
        "[decoration-end];column-gap:20px;padding:0 20px}"
        "#title{grid-row:headline;grid-column:title}"
        "#hide{grid-row:controls;grid-column:hide}"
        "#content{grid-row:content-toggleable;grid-column:content}"
        "nav{display:grid;grid-template-columns:[content-start] 1fr "
        "[content-end];grid-template-rows:auto auto}"
        "#logo{grid-column:content;grid-row:1;height:60px}"
        "#pillars{grid-column:content;grid-row:2;height:30px}"
        "#menu{grid-column:content;grid-row:1/3;align-self:end;"
        "justify-self:end;width:40px;height:40px}"
        "</style><body><section><div id=title>T</div><div id=hide>H</div>"
        "<div id=content>C</div></section><nav><div id=logo></div>"
        "<div id=pillars></div><div id=menu></div></nav></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *title = parity_box(&page, "title");
    const LayoutNodeBox *hide = parity_box(&page, "hide");
    const LayoutNodeBox *content = parity_box(&page, "content");
    const LayoutNodeBox *pillars = parity_box(&page, "pillars");
    const LayoutNodeBox *menu = parity_box(&page, "menu");
    /* 440 px of tracks: four 85 px columns between 0 px edge tracks. */
    bool ok = title != NULL && hide != NULL && content != NULL
        && pillars != NULL && menu != NULL
        && title->x == 40 && title->width == 400 && title->y == 0
        && hide->x == 355 && hide->width == 85 && hide->y == 0
        && content->x == 40 && content->width == 400 && content->y == 30
        && menu->y == pillars->y + pillars->height - 40;
    if (!ok)
        fprintf(stderr,
                "named grid: title=%d,%d w%d hide=%d,%d w%d content=%d,%d "
                "w%d menu=%d pillars=%d+%d\n",
                title ? title->x : -1, title ? title->y : -1,
                title ? title->width : -1, hide ? hide->x : -1,
                hide ? hide->y : -1, hide ? hide->width : -1,
                content ? content->x : -1, content ? content->y : -1,
                content ? content->width : -1, menu ? menu->y : -1,
                pillars ? pillars->y : -1, pillars ? pillars->height : -1);
    CHECK(ok);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* A flex item's percentages resolve against the flex container (Guardian's
   `max-width: 74%` section titles wrapped "Opinion" against their own
   basis); MDN's narrow layout unsets its sidebar's `max-height` cap; a
   grid item's percentage height resolves against its grid area; a hard
   one-axis box-shadow paints only the strip outside the box (MDN's
   table-of-contents rule). */
static int test_parity_percentages_and_strips(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}"
        ".row{display:flex;width:400px}#t{max-width:74%}"
        "h2{margin:0;font-size:24px;overflow-wrap:break-word}"
        "#p{padding-left:10%;background:#ff0000}"
        ".cap{max-height:40px;overflow:hidden}.wide .cap{max-height:unset}"
        ".g{display:grid;height:200px;grid-template-rows:50px 1fr}"
        ".g>div{min-height:0}#inner{height:100%}"
        "#s{display:inline-block;box-shadow:-2px 0 0 #00ff00;"
        "margin-left:10px;padding:4px}"
        "</style><body><div class=row><div id=t><h2>Opinion</h2></div></div>"
        "<div class=row><div id=p>X</div></div>"
        "<div class=wide><div class=cap id=cap>A<br>B<br>C<br>D</div></div>"
        "<div class=g><div></div><div id=area><div id=inner></div></div>"
        "</div><div><span id=s>Try it</span></div></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *title = parity_box(&page, "t");
    const LayoutNodeBox *padded = parity_box(&page, "p");
    const LayoutNodeBox *cap = parity_box(&page, "cap");
    const LayoutNodeBox *area = parity_box(&page, "area");
    const LayoutNodeBox *inner = parity_box(&page, "inner");
    const LayoutNodeBox *strip_box = parity_box(&page, "s");
    const DrawCommand *strip = parity_fill(&page.layout, 0x00ff00u, 0);
    const DrawCommand *x = text_command(&page.layout, "X");
    bool ok = title != NULL && padded != NULL && cap != NULL
        && area != NULL && inner != NULL && strip_box != NULL
        && strip != NULL && x != NULL
        && text_command(&page.layout, "Opinion") != NULL
        && x->x == padded->x + 40 && padded->width >= 40 + x->width
        && cap->height > 40
        && inner->height == area->height && area->height == 150
        && strip->width == 2 && strip->x == strip_box->x - 2
        && strip->y == strip_box->y && strip->height == strip_box->height;
    if (!ok)
        fprintf(stderr,
                "percentages: opinion=%d x=%d padded=%d/%d cap=%d "
                "area=%d inner=%d strip=%d,%d %dx%d box=%d,%d h%d\n",
                text_command(&page.layout, "Opinion") != NULL,
                x ? x->x : -1, padded ? padded->x : -1,
                padded ? padded->width : -1, cap ? cap->height : -1,
                area ? area->height : -1, inner ? inner->height : -1,
                strip ? strip->x : -1, strip ? strip->y : -1,
                strip ? strip->width : -1, strip ? strip->height : -1,
                strip_box ? strip_box->x : -1, strip_box ? strip_box->y : -1,
                strip_box ? strip_box->height : -1);
    CHECK(ok);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* A row flex container's sized, empty ::before/::after are flex items:
   MDN's breadcrumb chevrons follow each crumb and widen it, and its search
   button's caret leads the line while space-between pushes the icon to
   the end. */
static int test_parity_generated_flex_items(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}"
        "ol{display:flex;list-style:none;margin:0;padding:0}"
        "li{display:flex;place-items:center}"
        "li:not(:last-child)::after{content:'';background:#ff0000;"
        "width:20px;height:20px;margin-inline:4px}"
        "button{display:flex;justify-content:space-between;width:80px;"
        "padding:0;border:0;align-items:center}"
        "button::before{content:'';width:15px;height:18px;"
        "border-bottom:2px solid #0000ff}"
        "</style><body><ol><li id=one>Web</li><li id=two>JavaScript</li>"
        "</ol><button id=b><b id=q>Q</b></button></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *one = parity_box(&page, "one");
    const LayoutNodeBox *two = parity_box(&page, "two");
    const LayoutNodeBox *button = parity_box(&page, "b");
    const LayoutNodeBox *q = parity_box(&page, "q");
    const DrawCommand *web = text_command(&page.layout, "Web");
    const DrawCommand *chevron = parity_fill(&page.layout, 0xff0000u, 0);
    const DrawCommand *caret = parity_fill(&page.layout, 0x0000ffu, 0);
    bool ok = one != NULL && two != NULL && button != NULL && q != NULL
        && web != NULL && chevron != NULL && caret != NULL
        && chevron->x == web->x + web->width + 4
        && one->width == web->width + 28
        && two->x == one->x + one->width
        && caret->x == button->x && caret->width == 15
        && q->x + q->width == button->x + 80;
    if (!ok)
        fprintf(stderr,
                "flex pseudo: web=%d+%d chevron=%d one=%d+%d two=%d "
                "caret=%d w%d q=%d+%d button=%d\n",
                web ? web->x : -1, web ? web->width : -1,
                chevron ? chevron->x : -1, one ? one->x : -1,
                one ? one->width : -1, two ? two->x : -1,
                caret ? caret->x : -1, caret ? caret->width : -1,
                q ? q->x : -1, q ? q->width : -1, button ? button->x : -1);
    CHECK(ok);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* `bottom` on an absolutely positioned box refers to its containing
   block's final padding box, even when that block's height is auto and
   only known after the box is placed: Guardian's footer "Back to top"
   tab hangs 21 px below the footer. */
static int test_parity_bottom_inset_auto_height(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}p{margin:0;height:20px}"
        "footer{position:relative}"
        "#tab{position:absolute;bottom:-21px;right:20px;width:30px;"
        "height:10px}</style><body><footer id=f><p>one</p><p>two</p>"
        "<div id=tab></div></footer><p>after</p></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *footer = parity_box(&page, "f");
    const LayoutNodeBox *tab = parity_box(&page, "tab");
    bool ok = footer != NULL && tab != NULL && footer->height == 40
        && tab->y + tab->height == footer->y + 40 + 21
        && tab->x == 480 - 20 - 30;
    if (!ok)
        fprintf(stderr, "bottom inset: footer=%d+%d tab=%d,%d+%d\n",
                footer ? footer->y : -1, footer ? footer->height : -1,
                tab ? tab->x : -1, tab ? tab->y : -1,
                tab ? tab->height : -1);
    CHECK(ok);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_parity_bottom_inset_min_height(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;position:relative;min-height:272px}"
        "#content{height:600px}#wrap{height:80px}"
        "#badge{position:absolute;bottom:0;width:100%;height:40px;"
        "background:#00ff00}</style><body><div id=content>content</div>"
        "<div id=wrap><div id=badge>bottom</div></div></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *badge = parity_box(&page, "badge");
    const DrawCommand *text = text_command(&page.layout, "bottom");
    const DrawCommand *fill = parity_fill(&page.layout, 0x00ff00u, 0);
    CHECK(badge != NULL && badge->y == 640 && badge->height == 40);
    CHECK(text != NULL && text->y >= 640 && text->y < 680);
    CHECK(fill != NULL && fill->y == 640);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_parity_flexible_grid_minimum(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}.strip{display:grid;width:432px;"
        "grid-template-columns:repeat(4,1fr);gap:24px;overflow-x:auto}"
        ".strip>div{min-width:204px;height:40px}"
        "#shrink{grid-template-columns:repeat(4,minmax(0,1fr))}"
        "</style><body><div class=strip><div id=a>A</div><div id=b>B</div>"
        "<div>C</div><div>D</div></div><div class=strip id=shrink>"
        "<div id=c>E</div><div id=d>F</div><div>G</div><div>H</div></div></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *a = parity_box(&page, "a");
    const LayoutNodeBox *b = parity_box(&page, "b");
    const LayoutNodeBox *c = parity_box(&page, "c");
    const LayoutNodeBox *d = parity_box(&page, "d");
    CHECK(a != NULL && b != NULL && c != NULL && d != NULL);
    CHECK(a->width == 204 && b->x == a->x + 228);
    /* An explicit zero track minimum still permits the item to overflow
       its 90px track; it must not silently become an automatic minimum. */
    CHECK(c->width == 204 && d->x == c->x + 114);
    lxb_dom_node_t *shrink = find_id(
        lxb_dom_interface_node(page.document.html), "shrink");
    ComputedStyle shrink_style = style_for_node(&page.sheet, shrink, NULL);
    char tracks[128];
    CHECK(stylesheet_serialize_grid_template_tracks(
        &page.sheet, &shrink_style, false, tracks, sizeof(tracks)));
    CHECK(strcmp(tracks,
        "minmax(0px,1fr) minmax(0px,1fr) minmax(0px,1fr) minmax(0px,1fr)") == 0);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* Non-replaced inline boxes paint their background behind each line
   fragment's content area plus padding: MDN's `code` spans and links. */
static int test_parity_inline_backgrounds(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}p{margin:0;width:120px}"
        "code{background:#00ff00;padding:2px 4px}"
        "span{background:#ff0000}</style><body>"
        "<p>Read <a href='#x'><code>map</code></a> now</p>"
        "<p>Go <span>red background that wraps</span></p></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const DrawCommand *map = text_command(&page.layout, "map");
    const DrawCommand *code_bg = parity_fill(&page.layout, 0x00ff00u, 0);
    const DrawCommand *span_first = parity_fill(&page.layout, 0xff0000u, 0);
    const DrawCommand *span_second = parity_fill(&page.layout, 0xff0000u, 1);
    size_t green_index = SIZE_MAX, text_index = SIZE_MAX;
    for (size_t i = 0; i < page.layout.count; i++) {
        if (&page.layout.commands[i] == code_bg) green_index = i;
        if (&page.layout.commands[i] == map) text_index = i;
    }
    bool ok = map != NULL && code_bg != NULL && span_first != NULL
        && span_second != NULL
        && code_bg->x == map->x - 4 && code_bg->width == map->width + 8
        && code_bg->y == map->y - 2 && code_bg->height == map->height + 4
        && green_index < text_index
        && span_second->y > span_first->y && span_second->x == 0;
    if (!ok)
        fprintf(stderr, "inline background: map=%d,%d %dx%d bg=%d,%d %dx%d "
                "span=%d,%d/%d,%d\n",
                map ? map->x : -1, map ? map->y : -1,
                map ? map->width : -1, map ? map->height : -1,
                code_bg ? code_bg->x : -1, code_bg ? code_bg->y : -1,
                code_bg ? code_bg->width : -1,
                code_bg ? code_bg->height : -1,
                span_first ? span_first->x : -1,
                span_first ? span_first->y : -1,
                span_second ? span_second->x : -1,
                span_second ? span_second->y : -1);
    CHECK(ok);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_parity_current_color_background(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;color:#123456}"
        "button{background:none;border:0;padding:4px;color:inherit}"
        "button span{display:block;width:22px;height:2px;margin:4px auto;"
        "background-color:currentColor}"
        "#later{background:currentColor;color:#00ff00;width:20px;height:10px}"
        "#override{background-color:currentColor;background-color:#ff0000;"
        "width:20px;height:10px}"
        "#transparent{background:currentColor;background:transparent;"
        "width:20px;height:10px}"
        "#inherit{background-color:inherit;color:#0000ff;width:20px;height:10px}"
        "#pseudo::after{content:'';display:block;width:20px;height:3px;"
        "background:var(--ink);color:#ffff00}#pseudo{--ink:currentColor}"
        "</style><body><button><span></span><span></span><span></span></button>"
        "<div id=later><div id=inherit></div></div><div id=override></div>"
        "<div id=transparent></div><div id=pseudo></div></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    for (size_t i = 0; i < 3; i++) {
        const DrawCommand *bar = parity_fill(&page.layout, 0x123456u, i);
        CHECK(bar != NULL && bar->width == 22 && bar->height == 2);
    }
    CHECK(parity_fill(&page.layout, 0x00ff00u, 0) != NULL);
    CHECK(parity_fill(&page.layout, 0x0000ffu, 0) != NULL);
    CHECK(parity_fill(&page.layout, 0xff0000u, 0) != NULL);
    CHECK(parity_fill(&page.layout, 0xffff00u, 0) != NULL);
    ComputedStyle body = style_for_node(&page.sheet,
        find_id(lxb_dom_interface_node(page.document.html), "later"), NULL);
    ComputedStyle transparent = style_for_node(&page.sheet,
        find_id(lxb_dom_interface_node(page.document.html), "transparent"), &body);
    CHECK(!transparent.has_background);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_parity_transformed_intrinsic_widths(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;font:16px Arial}"
        ".row{display:flex;gap:8px}.label{font-size:12px;font-weight:bold;"
        "padding:4px;background:#00ff00;align-self:center}"
        ".upper{text-transform:uppercase}.lower{text-transform:lowercase}"
        ".atomic{display:inline-block;padding:4px;font-kerning:none}"
        ".minimum{display:inline-block;width:min-content;overflow-wrap:anywhere}"
        "#capital{text-transform:capitalize}"
        "</style><body><div class=row><div id=upper class='label upper'>"
        "Widely available</div><div id=upper-literal class=label>"
        "WIDELY AVAILABLE</div></div>"
        "<div class=atomic id=lower><span class=lower>WIDE LABEL</span></div>"
        "<div class=atomic id=lower-literal>wide label</div>"
        "<div class=minimum id=capital>immmmm</div>"
        "<div class=minimum id=capital-literal>Immmmm</div></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    FontSet fonts;
    CHECK(font_set_load(&fonts, &page.budget, TILEFINCH_TEST_SANS_FONT,
        TILEFINCH_TEST_SERIF_FONT, TILEFINCH_TEST_SANS_ITALIC_FONT,
        TILEFINCH_TEST_SANS_BOLD_FONT, TILEFINCH_TEST_SERIF_BOLD_FONT,
        TILEFINCH_TEST_METRIC_SANS_FONT, TILEFINCH_TEST_METRIC_SANS_BOLD_FONT,
        2u * MIB));
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget,
        &page.document, &page.sheet, &fonts, NULL, 480);
    CHECK(page.layout_ready);
    const LayoutNodeBox *upper = parity_box(&page, "upper");
    const LayoutNodeBox *literal = parity_box(&page, "upper-literal");
    const LayoutNodeBox *lower = parity_box(&page, "lower");
    const LayoutNodeBox *lower_literal = parity_box(&page, "lower-literal");
    const LayoutNodeBox *capital = parity_box(&page, "capital");
    const LayoutNodeBox *capital_literal = parity_box(&page, "capital-literal");
    CHECK(upper != NULL && literal != NULL && lower != NULL && lower_literal != NULL);
    CHECK(capital != NULL && capital_literal != NULL);
    if (upper->width != literal->width || upper->height != literal->height
        || lower->width != lower_literal->width)
        fprintf(stderr, "transformed intrinsic: upper=%dx%d literal=%dx%d "
                "lower=%d literal=%d\n", upper->width, upper->height,
                literal->width, literal->height, lower->width, lower_literal->width);
    CHECK(upper->width == literal->width && upper->height == literal->height);
    CHECK(lower->width == lower_literal->width);
    CHECK(capital->width == capital_literal->width);
    font_set_destroy(&fonts);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_parity_inline_block_child_margins(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}.group{display:inline-block;"
        "padding:4px;border:0;background:none}.bar{display:block;width:22px;"
        "height:2px;margin:4px auto;background:#123456}</style><body>"
        "<button class=group id=button><span class=bar></span>"
        "<span class=bar></span><span class=bar></span></button>"
        "<div class=group id=box><div class=bar></div>"
        "<div class=bar></div><div class=bar></div></div></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    for (size_t group = 0; group < 2; group++) {
        const DrawCommand *first = parity_fill(&page.layout, 0x123456u, group * 3);
        const DrawCommand *second = parity_fill(&page.layout, 0x123456u, group * 3 + 1);
        const DrawCommand *third = parity_fill(&page.layout, 0x123456u, group * 3 + 2);
        const LayoutNodeBox *box = parity_box(&page, group == 0 ? "button" : "box");
        CHECK(first != NULL && second != NULL && third != NULL && box != NULL);
        CHECK(second->y - first->y == 6 && third->y - second->y == 6);
        CHECK(first->y - box->y == 8 && box->height == 30);
    }
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_parity_positioned_pseudo_static_margins(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}a{float:left;width:54px;"
        "height:44px;position:relative}span{margin:0 auto;position:relative;"
        "top:21px;color:transparent;font:0/0 a}"
        "span::before,span::after{content:'';position:absolute}"
        "span,span::before,span::after{display:block;width:20px;height:3px;"
        "background:#123456}span::before{margin-top:-7px}"
        "span::after{margin-top:7px}</style><body>"
        "<a><span id=bars>Menu</span></a></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const DrawCommand *middle = parity_fill(&page.layout, 0x123456u, 0);
    const DrawCommand *before = parity_fill(&page.layout, 0x123456u, 1);
    const DrawCommand *after = parity_fill(&page.layout, 0x123456u, 2);
    CHECK(middle != NULL && before != NULL && after != NULL);
    if (middle->x != 17 || middle->y != 21 || before->x != 17
        || before->y != 14 || after->x != 17 || after->y != 28)
        fprintf(stderr, "pseudo static margins: %d,%d %d,%d %d,%d\n",
            middle->x, middle->y, before->x, before->y, after->x, after->y);
    CHECK(middle->x == 17 && middle->y == 21);
    CHECK(before->x == 17 && before->y == 14);
    CHECK(after->x == 17 && after->y == 28);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_parity_generated_inline_spacing(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}"
        "header{display:flex;height:52px;width:240px}"
        "#wrap{display:flex;align-items:center;width:100%}"
        "h2{margin:0;font:14px/18px sans-serif;display:-webkit-box;"
        "-webkit-box-orient:vertical;-webkit-line-clamp:1}"
        "h2::before{content:'M';width:15px;margin-right:2px}"
        "#odds{font:10px/12px sans-serif}"
        "#odds::after{content:'';display:inline-block;width:3px;"
        "height:3px;margin:0 2px;background:#123456;"
        "position:relative;top:-2px}"
        "#bets{display:flex;justify-content:center;align-items:center;"
        "flex-wrap:wrap;gap:4px;width:240px;margin:0;padding:0;"
        "list-style:none;font:11px/13px sans-serif}"
        "#bets li{float:left;position:relative;top:-2px}"
        "#bets li:first-child::after{content:'';display:inline-block;"
        "width:3px;height:3px;margin:0 2px;background:#654321;"
        "position:relative;top:-2px}"
        "#text-atomic{font:10px/12px sans-serif}"
        "#text-atomic::before{content:'Z';display:inline-block;"
        "position:relative;top:-4px}"
        "#percent-atomic{width:100px;font:10px/12px sans-serif}"
        "#percent-atomic::before{content:'';display:inline-block;"
        "width:50%;height:3px;background:#abcdef}"
        "</style><body><header><div id=wrap><div id=heading>"
        "<h2>Headline</h2></div></div></header><div id=odds>Spread</div>"
        "<ul id=bets><li>\n<span>A</span>\n </li>"
        "<li><span>Second wager</span></li></ul>"
        "<div id=text-atomic>Body</div>"
        "<div id=percent-atomic>Tail</div></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *heading = parity_box(&page, "heading");
    const DrawCommand *icon = text_command(&page.layout, "M");
    const DrawCommand *title = text_command(&page.layout, "Headline");
    const DrawCommand *spread = text_command(&page.layout, "Spread");
    const DrawCommand *dot = parity_fill(&page.layout, 0x123456u, 0);
    const DrawCommand *first = text_command(&page.layout, "A");
    const DrawCommand *separator = parity_fill(&page.layout, 0x654321u, 0);
    CHECK(heading && icon && title && spread && dot);
    CHECK(title->x >= icon->x + icon->width + 2);
    CHECK(heading->y == (52 - heading->height) / 2);
    CHECK(dot->x == spread->x + spread->width + 2);
    CHECK(dot->y > spread->y && dot->y < spread->y + spread->height);
    CHECK(first && separator);
    CHECK(separator->y > first->y
          && separator->y < first->y + first->height);
    const DrawCommand *generated = text_command(&page.layout, "Z");
    const DrawCommand *body = text_command(&page.layout, "Body");
    const DrawCommand *percent = parity_fill(&page.layout, 0xabcdefu, 0);
    /* This font-free fixture uses the documented 3/4-height fallback
       baseline. Relative positioning moves ink, not the flow baseline. */
    CHECK(generated && body
          && body->y + body->height * 3 / 4
             == generated->y + generated->height * 3 / 4 + 4);
    CHECK(percent && percent->width == 50);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_parity_inherited_box_sizing(void)
{
    static const char html[] =
        "<!doctype html><style>html{box-sizing:border-box}body{margin:0}"
        "*,::before,::after{box-sizing:inherit}"
        "section{width:200px;padding:16px;border:2px solid}"
        "section div{width:100%;padding:10px;border:1px solid}"
        "#content{box-sizing:content-box}"
        "#initial{box-sizing:initial}#unset{box-sizing:unset}"
        "#last{box-sizing:inherit;box-sizing:content-box}"
        "#invalid{box-sizing:inherit;box-sizing:not-border-box}"
        "#var{--size:inherit;box-sizing:var(--size)}"
        "#pseudo::after{content:'';display:block;width:100%;height:10px;"
        "padding:4px;border:1px solid #123456}"
        "</style><body><section id=border><div id=child></div></section>"
        "<section id=content><div id=content-child></div></section>"
        "<section id=initial></section><section id=unset></section>"
        "<section id=last></section><section id=var></section>"
        "<section id=invalid></section>"
        "<section id=inline style='box-sizing:content-box;box-sizing:inherit'>"
        "</section><section id=pseudo></section></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *border = parity_box(&page, "border");
    const LayoutNodeBox *child = parity_box(&page, "child");
    const LayoutNodeBox *content = parity_box(&page, "content");
    const LayoutNodeBox *content_child = parity_box(&page, "content-child");
    CHECK(border && child && content && content_child);
    CHECK(border->width == 200 && child->width == 164);
    CHECK(content->width == 236 && content_child->width == 222);
    CHECK(parity_box(&page, "initial")->width == 236);
    CHECK(parity_box(&page, "unset")->width == 236);
    CHECK(parity_box(&page, "last")->width == 236);
    CHECK(parity_box(&page, "var")->width == 200);
    CHECK(parity_box(&page, "invalid")->width == 200);
    CHECK(parity_box(&page, "inline")->width == 200);
    const DrawCommand *edge = parity_fill(&page.layout, 0x123456u, 0);
    CHECK(edge && edge->width == 164);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_link_defaults_and_generated_gradient(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;color:#008000}"
        "#inherit{color:inherit}#explicit{color:#112233}"
        "#stripe{position:relative;height:4px;width:300px}"
        "#stripe::after{content:'';position:absolute;left:0;right:0;top:0;"
        "height:4px;background:linear-gradient(to right,#ff0000 0%,"
        "#ff0000 33.33%,#000000 33.33%,#000000 66.66%,#0000ff 66.66%)}"
        "</style><body><div id=stripe></div>"
        "<a href=''>DefaultLink</a><a>NoDestination</a>"
        "<a id=inherit href='#'>InheritedLink</a>"
        "<a id=explicit href='#'>ExplicitLink</a></body>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const DrawCommand *normal = text_command(&page.layout, "DefaultLink");
    const DrawCommand *plain = text_command(&page.layout, "NoDestination");
    const DrawCommand *inherited = text_command(&page.layout, "InheritedLink");
    const DrawCommand *explicit = text_command(&page.layout, "ExplicitLink");
    const DrawCommand *gradient = NULL;
    for (size_t i = 0; i < page.layout.count; i++) {
        const DrawCommand *command = &page.layout.commands[i];
        if (command->type == DRAW_FILL_RECT
            && draw_command_fill_gradient(command, NULL)) gradient = command;
    }
    bool links_ok = normal && plain && inherited && explicit
        && normal->color == 0x0000ff && plain->color == 0x008000
        && inherited->color == 0x008000 && explicit->color == 0x112233;
    bool gradient_ok = gradient && gradient->x == 0 && gradient->y == 0
        && gradient->width == 300 && gradient->height == 4
        && page.layout.gradient_count == 1;
    TileCache cache = {0};
    uint16_t pixels[480u * 8u];
    CHECK(tile_cache_init(&cache, &page.budget, &page.layout, 8));
    CHECK(tile_cache_set_frame(&cache, pixels, 480u * 8u));
    CHECK(tile_cache_render_frame(&cache, 0, 480, 8, NULL));
    gradient_ok = gradient_ok
        && pixels[480u + 50u] == test_rgb565(0xff0000)
        && pixels[480u + 150u] == test_rgb565(0x000000)
        && pixels[480u + 250u] == test_rgb565(0x0000ff);
    tile_cache_destroy(&cache);
    if (!links_ok || !gradient_ok)
        fprintf(stderr, "link defaults=%d gradient=%d default=%06x gradients=%zu\n",
                links_ok, gradient_ok, normal ? normal->color : 0u,
                page.layout.gradient_count);
    CHECK(parity_page_destroy(&page));
    CHECK(links_ok && gradient_ok);
    return 0;
}

static int test_site_parity(void)
{
    {
        /* Empty flex items have zero automatic main size. They still count
           for gaps; an intrinsic outer row must not give them a synthetic
           text-width floor that shrinks and wraps the neighbouring label. */
        ParityPage page;
        CHECK(parity_page_load(&page,
            "<!doctype html><style>body{margin:0;font:20px serif}"
            ".outer{display:flex}.row{display:flex}.label{margin-right:8px}"
            "#gap{gap:5px}#edges{padding:0 3px;border:1px solid}"
            "#zero{width:0}#hidden{display:none}"
            "</style><body>"
            "<div class=outer><div><div class=row id=plain>"
            "<span id=empty></span><a class=label id=label>"
            "<span>Small label words</span></a></div></div></div>"
            "<div class=outer><div><div class=row id=gap>"
            "<span id=gapempty></span><a class=label id=gaplabel>"
            "Small label words</a><span id=hidden>hidden label</span>"
            "</div></div></div>"
            "<div class=row id=fixed><span id=zero></span>"
            "<span id=edges></span><span id=tail>Tail</span></div></body>"));
        const LayoutNodeBox *empty = parity_box(&page, "empty");
        const LayoutNodeBox *plain = parity_box(&page, "plain");
        const LayoutNodeBox *label = parity_box(&page, "label");
        const LayoutNodeBox *gapempty = parity_box(&page, "gapempty");
        const LayoutNodeBox *gap = parity_box(&page, "gap");
        const LayoutNodeBox *gaplabel = parity_box(&page, "gaplabel");
        const LayoutNodeBox *zero = parity_box(&page, "zero");
        const LayoutNodeBox *edges = parity_box(&page, "edges");
        const LayoutNodeBox *tail = parity_box(&page, "tail");
        bool ok = empty && plain && label && gapempty && gap && gaplabel
            && zero && edges && tail && empty->width == 0
            && label->x == plain->x && gapempty->width == 0
            && gaplabel->x == gap->x + 5 && gap->width == plain->width + 5
            && zero->width == 0 && edges->width == 8
            && tail->x == edges->x + edges->width
            && plain->height == gap->height && plain->height < 30;
        if (!ok) fprintf(stderr,
            "empty flex: empty=%d label-x=%d plain=%d/%d gapempty=%d "
            "gaplabel-x=%d gap=%d/%d zero=%d edges=%d tail-x=%d\n",
            empty ? empty->width : -1, label ? label->x : -1,
            plain ? plain->width : -1, plain ? plain->height : -1,
            gapempty ? gapempty->width : -1, gaplabel ? gaplabel->x : -1,
            gap ? gap->width : -1, gap ? gap->height : -1,
            zero ? zero->width : -1, edges ? edges->width : -1,
            tail ? tail->x : -1);
        CHECK(ok);
        CHECK(parity_page_destroy(&page));
    }
    CHECK(test_parity_inherited_box_sizing() == 0);
    {
        /* Large functional lists retain their actual text, not a fixed-size
           rewrite buffer. Keep both alternatives and ancestor matching. */
        static const char selector[] =
            ".visual-theme .component:is(.feature-lead-package,"
            ".feature-lead-headlines,.feature-lead-images,.feature-vertical-strip,"
            ".feature-grid-two,.feature-grid-three,.feature-grid-four,"
            ".feature-hero-card,.feature-shelf-carousel,.feature-spotlight,"
            ".feature-side-by-side,.feature-streaming-showcase) "
            ".media-wrapper .label:is(.label-subscriber,.label-exclusive,"
            ".label-opinion,.label-analysis,.label-investigates,.label-review,"
            ".label-trending,.label-custom,.label-poll,.label-projection,"
            ".label-streaming,.label-fact-check,.label-quiz)";
        CHECK(sizeof(selector) > 256 && sizeof(selector) < 1024);
        char html[2048];
        int length = snprintf(html, sizeof(html),
            "<!doctype html><style>body{margin:0}.label{width:10px;height:20px}"
            "%s{width:70px;--badge-ink:#ff0000;background:var(--badge-ink)}"
            ".visual-theme .label{width:30px}"
            "</style><body class=visual-theme>"
            "<div class='component feature-spotlight'><div class=media-wrapper>"
            "<div id=matched class='label label-analysis'></div>"
            "<div id=other class='label label-unknown'></div></div></div>"
            "<div class='component feature-unknown'><div class=media-wrapper>"
            "<div id=outside class='label label-analysis'></div></div></div>"
            "</body>", selector);
        CHECK(length > 0 && (size_t) length < sizeof(html));
        ParityPage page;
        CHECK(parity_page_load(&page, html));
        if (parity_box(&page, "matched")->width != 70) {
            lxb_dom_node_t *node = find_id(
                lxb_dom_interface_node(page.document.html), "matched");
            fprintf(stderr, "long selector: length=%zu rules=%zu custom=%zu "
                    "drops=%llu/%llu width=%d matches=%d\n",
                    sizeof(selector) - 1u, page.sheet.count,
                    page.sheet.custom_rule_count,
                    (unsigned long long) page.sheet.diagnostic_selector_drops,
                    (unsigned long long) page.sheet.diagnostic_custom_property_drops,
                    parity_box(&page, "matched")->width,
                    style_selector_matches(node, selector,
                                           sizeof(selector) - 1u));
            for (size_t i = 0; i < page.sheet.count; i++) {
                const StyleRule *r = &page.sheet.rules[i];
                fprintf(stderr, "rule %zu length=%u specificity=%u match=%d key=%u+%u\n",
                        i, r->selector_length, r->specificity,
                        stylesheet_rule_index_matches(&page.sheet, i, node),
                        r->fast_key_offset, r->fast_key_length);
            }
        }
        CHECK(parity_box(&page, "matched")->width == 70);
        CHECK(parity_box(&page, "other")->width == 30);
        CHECK(parity_box(&page, "outside")->width == 30);
        CHECK(parity_fill(&page.layout, 0xff0000u, 0) != NULL);
        CHECK(parity_page_destroy(&page));
    }
    {
        ParityPage page;
        CHECK(parity_page_load(&page,
            "<!doctype html><style>body{margin:0}"
            ".primary-stack-items{padding:0 16px;--gutter:16px}"
            ".content-item{width:100%;height:20px;position:relative}"
            ".reader-layout-with-sidebar :is(.primary-content-zone,.primary-product-zone):first-child "
            ":is(.primary-zone-items,.primary-product-items,.primary-stack-items)"
            ">.content-container:first-child.featured-story .content-item:first-child{"
            "left:calc(-1*var(--gutter));"
            "min-width:calc(100% + (2*var(--gutter)));"
            "max-width:calc(100% + (2*var(--gutter)))}"
            "</style><body class=reader-layout-with-sidebar><div class=primary-product-zone>"
            "<div><div class=primary-stack-items><div class='content-container featured-story'>"
            "<div id=bleed class=content-item></div><div id=ordinary class=content-item></div>"
            "</div></div></div></div></body>"));
        const LayoutNodeBox *bleed = parity_box(&page, "bleed");
        const LayoutNodeBox *ordinary = parity_box(&page, "ordinary");
        CHECK(bleed && ordinary && bleed->x == 0 && bleed->width == 480);
        CHECK(ordinary->x == 16 && ordinary->width == 448);
        CHECK(parity_page_destroy(&page));
    }
    {
        ParityPage page;
        CHECK(parity_page_load(&page,
            "<!doctype html><style>body{margin:0}"
            "#row{display:flex;height:40px}"
            "#inner{display:flex;align-items:center;height:100%}"
            "#icon{display:block;width:46px;height:22px}"
            "</style><body><div id=row><span id=inner><svg id=icon "
            "viewBox='0 0 46 22'></svg></span></div></body>"));
        const LayoutNodeBox *inner = parity_box(&page, "inner");
        const LayoutNodeBox *icon = parity_box(&page, "icon");
        CHECK(inner && icon && inner->height == 40 && icon->y == 9);
        CHECK(parity_page_destroy(&page));
        CHECK(parity_page_load(&page,
            "<!doctype html><style>body{margin:0}"
            "#brand{display:flex;height:40px}"
            "a{padding:4px;border:1px solid;margin:2px 0}"
            "#inner{display:flex;align-items:center;height:100%}"
            "#icon{display:block;width:46px;height:22px}"
            "</style><body><div id=brand><a id=link href='#'>"
            "<span id=inner><svg id=icon viewBox='0 0 46 22'></svg>"
            "</span></a></div></body>"));
        inner = parity_box(&page, "inner");
        icon = parity_box(&page, "icon");
        CHECK(inner && icon && inner->height == 26 && icon->y == 9);
        CHECK(parity_box(&page, "link")->height == 36);
        CHECK(parity_page_destroy(&page));
        CHECK(parity_page_load(&page,
            "<!doctype html><style>body{margin:0}"
            "#brand{display:flex;align-items:center;height:40px}"
            "#inner{display:flex;align-items:center;height:100%}"
            "#icon{display:block;width:46px;height:22px}"
            "</style><body><div id=brand><a id=link href='#'>"
            "<span id=inner><svg id=icon viewBox='0 0 46 22'></svg>"
            "</span></a></div></body>"));
        inner = parity_box(&page, "inner");
        icon = parity_box(&page, "icon");
        CHECK(inner && icon && inner->height == 22 && icon->y == 9);
        CHECK(parity_page_destroy(&page));
        CHECK(parity_page_load(&page,
            "<!doctype html><style>body{margin:0}"
            "#row{display:flex;align-items:center;height:40px}"
            "#brand{display:flex;height:40px}"
            "#inner{display:flex;align-items:center;height:100%}"
            "#icon{display:block;width:46px;height:22px}"
            "</style><body><div id=row><div id=brand><a id=link href='#'>"
            "<span id=inner><svg id=icon viewBox='0 0 46 22'></svg>"
            "</span></a></div></div></body>"));
        inner = parity_box(&page, "inner");
        icon = parity_box(&page, "icon");
        CHECK(inner && icon && inner->height == 40 && icon->y == 9);
        CHECK(parity_page_destroy(&page));
    }
    puts("test: positioned pseudos use their containing block");
    CHECK(test_parity_positioned_pseudo_containing_block() == 0);
    puts("test: text-wrap nowrap, media not, hidden pseudos");
    CHECK(test_parity_text_wrap_media_not() == 0);
    CHECK(test_balanced_text_ignores_collapsible_indentation() == 0);
    puts("test: named grid lines, long templates, spanning alignment");
    CHECK(test_parity_named_grid_lines() == 0);
    puts("test: flex/grid percentage bases, unset caps, shadow strips");
    CHECK(test_parity_percentages_and_strips() == 0);
    puts("test: row flex containers lay out generated items");
    CHECK(test_parity_generated_flex_items() == 0);
    CHECK(test_parity_generated_inline_spacing() == 0);
    puts("test: bottom insets follow an auto-height containing block");
    CHECK(test_parity_bottom_inset_auto_height() == 0);
    puts("test: bottom insets follow growth beyond a minimum height");
    CHECK(test_parity_bottom_inset_min_height() == 0);
    puts("test: flexible grid tracks retain automatic minimums");
    CHECK(test_parity_flexible_grid_minimum() == 0);
    puts("test: inline boxes paint their backgrounds");
    CHECK(test_parity_inline_backgrounds() == 0);
    puts("test: currentColor backgrounds follow the final foreground");
    CHECK(test_parity_current_color_background() == 0);
    puts("test: intrinsic sizing measures transformed text");
    CHECK(test_parity_transformed_intrinsic_widths() == 0);
    puts("test: inline-block sibling margins collapse inside the container");
    CHECK(test_parity_inline_block_child_margins() == 0);
    puts("test: positioned empty pseudos retain margins at their static position");
    CHECK(test_parity_positioned_pseudo_static_margins() == 0);
    CHECK(test_link_defaults_and_generated_gradient() == 0);
    return 0;
}
