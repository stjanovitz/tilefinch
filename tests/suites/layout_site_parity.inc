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

static int test_site_parity(void)
{
    puts("test: positioned pseudos use their containing block");
    CHECK(test_parity_positioned_pseudo_containing_block() == 0);
    puts("test: text-wrap nowrap, media not, hidden pseudos");
    CHECK(test_parity_text_wrap_media_not() == 0);
    puts("test: named grid lines, long templates, spanning alignment");
    CHECK(test_parity_named_grid_lines() == 0);
    puts("test: flex/grid percentage bases, unset caps, shadow strips");
    CHECK(test_parity_percentages_and_strips() == 0);
    puts("test: row flex containers lay out generated items");
    CHECK(test_parity_generated_flex_items() == 0);
    puts("test: bottom insets follow an auto-height containing block");
    CHECK(test_parity_bottom_inset_auto_height() == 0);
    puts("test: inline boxes paint their backgrounds");
    CHECK(test_parity_inline_backgrounds() == 0);
    return 0;
}
