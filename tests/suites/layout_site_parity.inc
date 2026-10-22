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

static int test_grid_intrinsic_height_and_margin_alignment(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}.row{display:flex;justify-content:center}"
        "#logo{display:grid;height:18px}svg{width:auto;height:100%;max-width:248px}"
        "#grid{display:grid;justify-items:center;width:480px}"
        "#item{width:448px;height:20px;margin:0 16px}"
        "#limit{width:448px}#fit{display:grid;width:100%;min-width:fit-content;"
        "max-width:calc(100% - 32px)}#wide{width:480px;height:10px}"
        "</style><div class=row><div id=logo>"
        "<svg id=art width=208 height=54 viewBox='0 0 208 54'>"
        "<rect width=208 height=54 fill=red/></svg></div></div>"
        "<div id=grid><div id=item></div></div>"
        "<div id=limit><div id=fit><div id=wide></div></div></div>";
    ParityPage page;CHECK(parity_page_load(&page,html));
    unsigned char pixels[208u*54u*4u]={0};
    ImageResource image={.node=find_id(lxb_dom_interface_node(page.document.html),"art"),
                         .pixels=pixels,.width=208,.height=54};
    ImageResources images={.items=&image,.count=1};
    layout_destroy(&page.layout);
    CHECK(layout_build(&page.layout,&page.budget,&page.document,&page.sheet,
                       NULL,&images,480));
    const LayoutNodeBox *logo=parity_box(&page,"logo");
    const LayoutNodeBox *item=parity_box(&page,"item");
    CHECK(logo && logo->width>=69 && logo->width<=70 && logo->x>=205 && logo->x<=206);
    CHECK(item && item->x==16 && item->width==448);
    const LayoutNodeBox *fit=parity_box(&page,"fit");
    CHECK(fit && fit->width==448);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_generated_flex_intrinsic_separator(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}.bar{display:flex;width:100px;overflow:hidden}"
        ".crumb{display:flex}a{display:block;width:35px;height:24px;flex-shrink:0}"
        ".crumb::after{content:'';display:block;width:20px;height:20px;"
        "margin:0 4px;background:red}"
        "#button{appearance:none;display:block;padding:11px;border:0;min-width:0}"
        "#button::before{content:'';display:block;width:24px;height:24px;background:blue}"
        "#nav{display:grid;grid-template-columns:1fr 80px min-content;gap:4px;width:480px}"
        "#menu{appearance:none;display:block;padding:11px;border:0}"
        "#menu::before{content:'';display:block;width:24px;height:24px;background:blue}"
        "</style><div class=bar><div id=first class=crumb><a></a></div>"
        "<div id=second class=crumb><a></a></div></div>"
        "<div class=bar><button id=button></button></div>"
        "<nav id=nav><div></div><div></div><button id=menu></button></nav>";
    ParityPage page;CHECK(parity_page_load(&page,html));
    const LayoutNodeBox *first=parity_box(&page,"first");
    const LayoutNodeBox *second=parity_box(&page,"second");
    CHECK(first && second && first->width==63 && second->x==63);
    const LayoutNodeBox *button=parity_box(&page,"button");
    CHECK(button && button->width==46);
    const LayoutNodeBox *menu=parity_box(&page,"menu");
    CHECK(menu && menu->width==46 && menu->x==434);
    const DrawCommand *red=NULL;
    for(size_t i=0;i<page.layout.count;i++)
        if(page.layout.commands[i].type==DRAW_FILL_RECT
            && page.layout.commands[i].color==0xff0000u){red=&page.layout.commands[i];break;}
    CHECK(red && red->width==20);
    CHECK(parity_page_destroy(&page));return 0;
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
static int test_inline_replaced_percentage_height(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}"
        ".row{height:24px}img,svg{width:60px;height:100%;vertical-align:top}"
        "#border{box-sizing:border-box;padding:2px;border:1px solid}"
        "#auto{height:auto}#auto img{height:100%;width:60px}"
        "</style><body><div class=row><img id=plain width=60 height=90>"
        "<svg id=vector width=60 height=90></svg>"
        "<img id=border width=60 height=90></div>"
        "<div class=row><div id=auto><img id=indefinite width=60 height=90>"
        "</div></div></body>"));
    const LayoutNodeBox *plain = parity_box(&page, "plain");
    const LayoutNodeBox *vector = parity_box(&page, "vector");
    const LayoutNodeBox *border = parity_box(&page, "border");
    const LayoutNodeBox *indefinite = parity_box(&page, "indefinite");
    CHECK(plain && plain->width == 60 && plain->height == 24);
    CHECK(vector && vector->width == 60 && vector->height == 24);
    CHECK(border && border->width == 60 && border->height == 24);
    /* An auto-height intermediate block does not inherit the outer
       ancestor's definite percentage basis. */
    CHECK(indefinite == NULL || indefinite->height != 24);
    CHECK(parity_page_destroy(&page));
    return 0;
}

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

static int test_generated_text_flex_order(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.row{display:flex;gap:5px;"
        "align-items:center;width:200px;height:30px}.row::before{content:'['}"
        ".row::after{content:']'}.reverse{flex-direction:row-reverse}"
        ".ordered::before{order:2}.ordered::after{order:-1}"
        "</style><body><div class=row><span id=normal>X</span></div>"
        "<div class='row reverse'><span id=reverse>Y</span></div>"
        "<div class='row ordered'><span id=ordered>Z</span></div></body>"));
    const DrawCommand *left = text_command(&page.layout, "[");
    const DrawCommand *right = text_command(&page.layout, "]");
    const DrawCommand *x = text_command(&page.layout, "X");
    CHECK(left && right && x);
    CHECK(left->y == x->y && right->y == x->y);
    CHECK(x->x == left->x + left->width + 5);
    CHECK(right->x == x->x + x->width + 5);
    const LayoutNodeBox *reverse = parity_box(&page, "reverse");
    const LayoutNodeBox *ordered = parity_box(&page, "ordered");
    CHECK(reverse && reverse->x > 100);
    CHECK(ordered && ordered->x > 0 && ordered->x < 100);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_block_text_control_auto_height(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}input{font-size:14px;"
        "padding:7px;border:1px solid;box-sizing:border-box;width:140px}"
        "#block,#plain,#fixed{display:block}#plain{appearance:none}"
        "#fixed{height:20px}.flex{display:flex;align-items:center}"
        "</style><body><input id=inline type=search placeholder=Search>"
        "<input id=block type=search placeholder=Search>"
        "<input id=plain type=text><input id=fixed type=text>"
        "<div class=flex><input id=flex type=search></div></body>"));
    const LayoutNodeBox *inline_box = parity_box(&page, "inline");
    const LayoutNodeBox *block = parity_box(&page, "block");
    const LayoutNodeBox *plain = parity_box(&page, "plain");
    const LayoutNodeBox *fixed = parity_box(&page, "fixed");
    const LayoutNodeBox *flex = parity_box(&page, "flex");
    CHECK(inline_box && block && plain && fixed && flex);
    CHECK(block->height == inline_box->height && block->height >= 30);
    CHECK(plain->height == block->height);
    CHECK(flex->height == block->height);
    CHECK(fixed->height == 20);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_generated_flex_percentage_edges_and_wrap(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.row{display:flex;gap:5px}"
        "#percent{width:200px}#percent::before{content:'P';padding:10%;"
        "background:red}#percent span{width:10px;flex-shrink:0}"
        "#wrap{width:60px;flex-wrap:wrap}#wrap::before{content:'AA BB';"
        "width:40px;flex-shrink:0}#wrap span{width:30px;flex-shrink:0}"
        "</style><body><div class=row id=percent><span id=q>Q</span></div>"
        "<div class=row id=wrap><span id=r>R</span></div></body>"));
    const DrawCommand *p = text_command(&page.layout, "P");
    const DrawCommand *fill = parity_fill(&page.layout, 0xff0000u, 0);
    const DrawCommand *aa = text_command(&page.layout, "AA");
    const LayoutNodeBox *q = parity_box(&page, "q");
    const LayoutNodeBox *r = parity_box(&page, "r");
    CHECK(p && fill && aa && q && r);
    CHECK(p->x == fill->x + 20);
    CHECK(fill->width == p->width + 40);
    CHECK(q->x == fill->x + fill->width + 5);
    CHECK(r->y > aa->y && r->x == 0);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_empty_inline_positioned_border_pseudo(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.icon{position:relative;"
        "float:right;border-radius:100%;background:#fff;height:42px;"
        "min-width:42px}.icon i::before{position:absolute;top:6px;"
        "bottom:0;left:0;right:0;margin:auto;border:2px solid #052962;"
        "border-bottom:0;border-right:0;content:'';height:12px;width:12px;"
        "transform:rotate(45deg)}"
        "</style><body><span class=icon id=icon><i></i></span></body>"));
    const LayoutNodeBox *icon = parity_box(&page, "icon");
    CHECK(icon != NULL && icon->width == 42 && icon->height == 42);
    TileCache cache = {0};
    uint16_t pixels[480u * 48u];
    CHECK(tile_cache_init(&cache, &page.budget, &page.layout, 8));
    CHECK(tile_cache_set_frame(&cache, pixels, 480u * 48u));
    CHECK(tile_cache_render_frame(&cache, 0, 480, 48, NULL));
    /* The rotated top/left border forms a centered upward chevron, not a
       square corner at the containing block's origin. */
    CHECK(pixels[(icon->y + 15) * 480 + icon->x + 20] != test_rgb565(0xffffff));
    CHECK(pixels[(icon->y + 22) * 480 + icon->x + 13] != test_rgb565(0xffffff));
    CHECK(pixels[(icon->y + 22) * 480 + icon->x + 27] != test_rgb565(0xffffff));
    CHECK(pixels[(icon->y + 4) * 480 + icon->x + 1] == test_rgb565(0xffffff));
    tile_cache_destroy(&cache);
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

static int test_percentage_image_auto_height(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.card{display:flex;flex-direction:column;width:448px}"
        "figure{margin:0;min-height:137px;position:relative;overflow:hidden}"
        "img{display:block;width:100%;height:100%;object-fit:cover}"
        "</style><div class=card><figure id=figure><picture><img id=photo src=x></picture>"
        "</figure><p>Caption</p></div>"));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(page.document.html), "photo");
    static unsigned char pixels[4] = {255,255,255,255};
    ImageResource image = {.node=node,.pixels=pixels,.width=343,.height=137};
    ImageResources images = {.items=&image,.count=1};
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout,&page.budget,&page.document,
        &page.sheet,NULL,&images,480);
    CHECK(page.layout_ready);
    const LayoutNodeBox *photo=parity_box(&page,"photo"),*figure=parity_box(&page,"figure");
    CHECK(photo && figure);
    CHECK(photo->width==448 && photo->height>=178 && photo->height<=179);
    CHECK(figure->height==photo->height);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_floating_generated_list_marker(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}ul{list-style:none;padding:0;margin:0}"
        "li{font:14px/19px sans-serif;overflow:hidden}a{display:block;padding:7px 0}"
        "li::before{content:'*';display:block;float:left;font-size:10px;padding:7px 3px 7px 0}"
        "</style><ul><li id=one><a>First item</a></li><li id=two><a>Second item</a></li></ul>"));
    const LayoutNodeBox *one=parity_box(&page,"one"),*two=parity_box(&page,"two");
    CHECK(one && two);
    CHECK(one->height==33 && two->y-one->y==33);
    const DrawCommand *first = text_command(&page.layout,"First");
    CHECK(first && first->y-one->y<15);
    CHECK(parity_page_destroy(&page));
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}#box{width:200px;overflow:hidden}"
        "#box::after{content:'';float:right;width:25%;height:9px;"
        "padding:2px;border:1px solid red;margin:3px;background:#123456}"
        "</style><div id=box></div>"));
    const DrawCommand *marker = parity_fill(&page.layout, 0x123456u, 0);
    const LayoutNodeBox *box = parity_box(&page, "box");
    CHECK(marker && box && marker->width == 56 && marker->height == 15
        && marker->x == 141 && marker->y == 3 && box->height == 21);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_compact_background_geometry(void)
{
    char html[32768];
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:0}");
    for (unsigned i = 0; i < 280; i++) {
        int n = snprintf(html + used, sizeof(html) - used,
            ".p%u{background-position:%upx %upx}", i, i, i + 1);
        CHECK(n > 0 && (size_t) n < sizeof(html) - used);
        used += (size_t) n;
    }
    CHECK((size_t) snprintf(html + used, sizeof(html) - used,
        "#a{background-position:7px 9px}"
        "#a,#b{background-image:linear-gradient(red,blue)}"
        "#b{background-position:11px 13px}"
        "#c{background-position:right 3px bottom 4px;"
        "background-image:linear-gradient(red,blue)}"
        "#d{background-position:2px 3px,4px 5px;"
        "background-image:linear-gradient(red,blue),linear-gradient(blue,red)}"
        "#e{background-position:5px 8px;background-position-x:20%%;"
        "background-position-y:30%%;background-image:linear-gradient(red,blue)}"
        "#f{background-image:linear-gradient(red,blue);background-position:0%% 0%%}"
        "div{height:20px}</style><div id=a></div><div id=b></div>"
        "<div id=c></div><div id=d></div><div id=e></div><div id=f></div>"
        "<div id=late style='background:linear-gradient(red,blue)'></div>")
        < sizeof(html) - used);
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    CHECK(page.layout.gradient_count > 0);
    const char *ids[] = {"a", "b", "c", "d", "late", "e", "f"};
    for (unsigned i = 0; i < 7; i++) {
        lxb_dom_node_t *node = find_id(
            lxb_dom_interface_node(page.document.html), ids[i]);
        ComputedStyle style = style_for_node(&page.sheet, node, NULL);
        CHECK(style.background_image_kind == STYLE_BACKGROUND_IMAGE_GRADIENT);
        const StylePaintStack *paint = stylesheet_paint_stack(
            &page.sheet, computed_style_paint_stack_id(&style));
        CHECK(paint && paint->background_count > 0);
        if (i < 2) {
            CHECK(paint->backgrounds[0].position_x == (i == 0 ? 7 : 11));
            CHECK(paint->backgrounds[0].position_y == (i == 0 ? 9 : 13));
        } else if (i == 2) {
            CHECK(paint->backgrounds[0].position_x == 3
                && paint->backgrounds[0].position_y == 4
                && (paint->backgrounds[0].position_edges & 3u) == 3u);
        } else if (i == 3) {
            CHECK(paint->background_count == 2
                && paint->backgrounds[1].position_x == 4
                && paint->backgrounds[1].position_y == 5);
        } else if (i >= 5) {
            CHECK(paint->backgrounds[0].position_x == (i == 5 ? 20 : 0)
                && paint->backgrounds[0].position_y == (i == 5 ? 30 : 0)
                && paint->backgrounds[0].position_edges == 0);
        }
    }
    CHECK(page.sheet.paint_storage->count < 32);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_intrinsic_float_rows(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.group{display:inline-block}"
        ".item{width:10px;height:10px}.top{clear:both;margin:auto}"
        ".left{float:left;margin-right:4px}.right{float:right}"
        "</style><div class=group id=group><div class='item top'></div>"
        "<div class='item left' id=left></div>"
        "<div class='item right' id=right></div></div>"));
    const LayoutNodeBox *group = parity_box(&page, "group");
    const LayoutNodeBox *left = parity_box(&page, "left");
    const LayoutNodeBox *right = parity_box(&page, "right");
    CHECK(group && left && right);
    CHECK(group->width == 24 && group->height == 20);
    CHECK(left->y == right->y && right->x - left->x == 14);
    CHECK(parity_page_destroy(&page));
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.group{display:inline-block;"
        "font-size:0;line-height:0}.item{display:inline-block;width:40px;"
        "height:20px;vertical-align:top}.right{float:right;height:50px}"
        ".taller{height:30px}</style><div class=group id=group>"
        "<i class=item id=before></i><b class='item right' id=float></b>"
        "<i class='item taller' id=after></i></div>"));
    group = parity_box(&page, "group");
    const LayoutNodeBox *before = parity_box(&page, "before");
    const LayoutNodeBox *floating = parity_box(&page, "float");
    const LayoutNodeBox *after = parity_box(&page, "after");
    CHECK(group && before && floating && after);
    CHECK(group->width == 120 && group->height == 50
          && before->y == floating->y && after->y == before->y
          && before->x < after->x && after->x < floating->x);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_border_color_triangles(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;background:white}"
        "#diamond{position:relative;width:0;height:0;border:10px solid;"
        "border-color:transparent transparent rgb(255, 0, 0) transparent}"
        "#diamond::after{content:'';position:absolute;left:-10px;top:10px;"
        "width:0;height:0;border:10px solid transparent;border-top-color:red}"
        "#two{border-color:red blue}#three{border-color:red blue green}"
        "#invalid{border-color:red;border-color:blue green red black white}"
        "</style><div id=diamond></div><div id=two></div>"
        "<div id=three></div><div id=invalid></div>"));
    const char *ids[] = {"diamond", "two", "three", "invalid"};
    for (unsigned i = 0; i < 4; i++) {
        ComputedStyle style = style_for_node(&page.sheet,
            find_id(lxb_dom_interface_node(page.document.html), ids[i]), NULL);
        for (unsigned side = 0; side < 4; side++) {
            uint8_t alpha;
            uint32_t color = stylesheet_border_color(&page.sheet, &style,
                (StyleBorderSide) side, &alpha);
            if (i == 0) CHECK(alpha == (side == 2 ? 255 : 0));
            else if (i == 3) CHECK(color == 0xff0000u && alpha == 255);
            else CHECK(color == ((side & 1u) ? 0x0000ffu
                : (i == 2 && side == 2 ? 0x008000u : 0xff0000u)));
        }
    }
    uint16_t pixels[480 * 40];
    TileCache cache = {0};
    CHECK(tile_cache_init(&cache, &page.budget, &page.layout, 8));
    CHECK(tile_cache_set_frame(&cache, pixels, 480u * 40u));
    CHECK(tile_cache_render_frame(&cache, 0, 480, 40, NULL));
    CHECK(pixels[15 * 480 + 10] != pixels[15 * 480]);
    CHECK(pixels[25 * 480 + 10] != pixels[25 * 480]);
    CHECK(pixels[11 * 480 + 1] == pixels[11 * 480 + 30]);
    CHECK(pixels[28 * 480 + 1] == pixels[28 * 480 + 30]);
    tile_cache_destroy(&cache);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_variable_gradient_token_boundaries(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>:root{--light:initial;--dark: ;"
        "--navigation-controls-gradient-fade-start-color-property:"
        "var(--light,#fff)var(--dark,#111);"
        "--navigation-controls-gradient-fade-middle-color-property:"
        "var(--light,#ffffffb3)var(--dark,#111111b3)}"
        "body{margin:0}#fade{width:56px;height:23px;"
        "background:linear-gradient(270deg,"
        "var(--navigation-controls-gradient-fade-start-color-property)52.47%,"
        "var(--navigation-controls-gradient-fade-middle-color-property)76.79%,"
        "transparent 87.29%)}"
        "</style><div id=fade></div>"));
    CHECK(page.layout.gradient_count == 1);
    const StyleGradient *gradient = &page.layout.gradients[0];
    CHECK(gradient->stop_count == 3 && gradient->angle == 270
        && gradient->stop_argb[0] == 0xffffffffu
        && gradient->stop_argb[1] == 0xb3ffffffu
        && gradient->stop_position[0] == 134);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_positioned_percentage_height_after_flow(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}#host{position:absolute;top:55px;"
        "bottom:0;width:100%;display:flex;flex-direction:column}#surface{"
        "position:relative;flex-basis:50%;flex-grow:1}#tools{position:absolute;"
        "top:0;right:0;height:100%;display:flex;flex-direction:column;"
        "flex-wrap:wrap-reverse;column-gap:10px}#tools>div{width:40px;height:80px}"
        "</style><div id=host><div id=surface><div><div id=tools>"
        "<div id=first></div><div></div><div id=third></div>"
        "</div></div></div></div>"));
    const LayoutNodeBox *surface = parity_box(&page, "surface");
    const LayoutNodeBox *tools = parity_box(&page, "tools");
    const LayoutNodeBox *first = parity_box(&page, "first");
    const LayoutNodeBox *third = parity_box(&page, "third");
    CHECK(surface && tools && first && third && surface->height == 217);
    CHECK(tools->height == surface->height && third->y == first->y
          && third->x == first->x - 50);
    CHECK(parity_page_destroy(&page));
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;font:14px/14px sans-serif}"
        ".outer{position:relative;width:448px}"
        ".line{display:flex;height:23px}.next{display:flex;position:absolute;"
        "top:0;right:0;width:56px;height:100%}"
        "</style><div class=outer><div><div class=line>Row</div>"
        "<div class=next id=next></div></div></div>"));
    const LayoutNodeBox *next = parity_box(&page, "next");
    CHECK(next && next->x == 392 && next->y == 0 && next->height == 23);
    CHECK(parity_page_destroy(&page));
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.host{position:relative;"
        "width:200px;padding:10px 0;border:2px solid;min-height:20px}"
        ".content{height:339px}.arrow{position:absolute;top:50%;right:0;"
        "width:16px;height:20px}.low{top:auto;bottom:25%}"
        "</style><div id=host class=host><div class=content></div>"
        "<div><a href=x id=arrow class=arrow>Go</a>"
        "<a href=y id=low class='arrow low'>Low</a></div></div>"));
    const LayoutNodeBox *host = parity_box(&page, "host");
    const LayoutNodeBox *arrow = parity_box(&page, "arrow");
    const LayoutNodeBox *low = parity_box(&page, "low");
    CHECK(host && arrow && low && host->height == 363);
    CHECK(arrow->y == host->y + 2 + 359 / 2
          && low->y == host->y + 2 + 359 - 359 / 4 - 20);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_page_font_normal_leading(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    FontSet fonts = {0};
    CHECK(font_set_load_selected(&fonts, &budget, TILEFINCH_TEST_SANS_FONT,
        NULL, NULL, NULL, NULL, NULL, NULL, 2u * MIB, FONT_SET_FACE_SANS));
    FontFace web = {0};
    CHECK(font_face_load_encoded(&web, &budget, fonts.sans.data,
        fonts.sans.data_length, 2u * MIB));
    int metric = font_metric_height_fixed_at_size(&web, 922);
    CHECK(metric > 0 && metric < (922 * 6 + 2) / 5
          && font_line_height_fixed_at_size(&web, 922)
                 == ((metric + 32) / 64) * 64
          && font_line_height_fixed_at_size(&fonts.sans, 922) > metric);
    font_face_destroy(&web);
    font_set_destroy(&fonts);
    CHECK(budget.current == 0);
    return 0;
}

static int test_generated_inline_padding(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;font:16px/20px monospace}"
        "p{margin:0}.label{padding:0 7px;border-left:1px solid;"
        "margin-left:3px;white-space:nowrap;background:#ddd}"
        "#generated::before{content:'I'}"
        "</style><p><a href=x id=generated class=label> Label</a></p>"
        "<p><a href=y class=label>I Label</a></p>"));
    const DrawCommand *runs[2] = {0};
    size_t count = 0;
    for (size_t i = 0; i < page.layout.count && count < 2; i++) {
        const DrawCommand *command = &page.layout.commands[i];
        if (command->type == DRAW_TEXT && command->text_length == 1
            && command->text[0] == 'I') runs[count++] = command;
    }
    CHECK(count == 2 && runs[0]->x == 11 && runs[1]->x == 11);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_button_content_centering(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}button{display:block;box-sizing:border-box;"
        "height:30px;width:80px;padding:0;border:1px solid black;font-size:14px;line-height:20px}"
        "#flex{display:flex;align-items:flex-start}#auto{height:auto;min-height:30px}"
        "#fixed{position:absolute;top:2px;height:2px;width:2px}"
        "</style><button id=plain>Plain</button><button id=nested><span>Nested</span></button>"
        "<button id=flex>Start</button><button id=positioned style='position:relative'>"
        "Positioned<i id=fixed></i></button><button id=auto>Auto</button>"));
    const DrawCommand *plain = text_command(&page.layout, "Plain");
    const DrawCommand *nested = text_command(&page.layout, "Nested");
    const DrawCommand *start = text_command(&page.layout, "Start");
    CHECK(plain && nested && start);
    CHECK(plain->y - parity_box(&page, "plain")->y == 5
        && nested->y - parity_box(&page, "nested")->y == 5
        && start->y - parity_box(&page, "flex")->y == 1);
    CHECK(parity_box(&page, "fixed")->y - parity_box(&page, "positioned")->y == 3);
    CHECK(text_command(&page.layout, "Auto")->y - parity_box(&page, "auto")->y == 1);
    CHECK(parity_page_destroy(&page));
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}button{display:block;"
        "height:50px;width:100px;border:0;padding:0}"
        "#inner{display:block;position:relative;height:30px}"
        "#icon{position:absolute;top:5px;width:5px;height:5px}"
        "</style><button><b id=inner>Label<i id=icon></i></b></button>"));
    CHECK(parity_box(&page, "inner")->y == 10
          && parity_box(&page, "icon")->y == 15);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_nested_toolbar_intrinsics(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.row{display:flex}"
        "table{border-spacing:2px}td{padding:0}button{display:flex;"
        "padding:3px;border:1px solid;height:36px;box-sizing:border-box}"
        "button span{display:block;width:60px;height:12px}"
        "</style><div class=row><div id=picker><table><tr><td>"
        "<button><span></span></button></td><td><button><span></span>"
        "</button></td></tr></table></div></div>"));
    CHECK(parity_box(&page, "picker")->width == 142);
    CHECK(parity_page_destroy(&page));
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.row{display:flex;justify-content:end}"
        "</style><div class=row><div id=count><span>1 - 45 </span>"
        "<span> of 45</span></div></div>"));
    const DrawCommand *last = text_command(&page.layout, "45");
    CHECK(last && parity_box(&page, "count")->height < 30);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_legacy_grid_gaps(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}#grid{display:grid;"
        "width:100px;grid-template-columns:20px 20px;"
        "grid-template-rows:10px 10px;grid-column-gap:11px;grid-row-gap:2px}"
        "</style><div id=grid><i></i><i id=second></i><i id=third></i></div>"));
    CHECK(parity_box(&page, "second")->x == 31
          && parity_box(&page, "third")->y == 12);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_nested_button_duration(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}button{height:30px;"
        "padding:0 10px;border:0;font-size:14px}"
        ".inner{display:flex;align-items:center;height:100%}"
        ".icon{display:block;width:10px;height:10px;background:red}"
        ".label{display:inline-block;margin-left:6px;line-height:1}"
        "button{text-indent:-999em}.wrapper{display:block;text-indent:0}"
        ".wrapper button{text-indent:inherit}"
        ".hide{display:none}</style><div class=wrapper><button><b class=inner><b class=icon></b>"
        "<b class=label><b class=hide>Listen</b><b id=duration>"
        "<span class=hide>separator</span> 4:22</b></b></b></button></div>"));
    CHECK(text_command(&page.layout, "4:22") != NULL
          && text_command(&page.layout, "4:22")->x >= 0
          && parity_box(&page, "duration")->width > 0);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_auto_fractional_grid_rows(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}#grid{display:grid;width:200px;"
        "grid-template-columns:auto 1fr;grid-template-rows:1fr 1fr;gap:2px 11px}"
        "#icon{grid-column:1/2;grid-row:1/3;width:35px;height:35px}"
        "#label{grid-column:2/2;grid-row:1/2;align-self:end;font-size:12px;line-height:12px}"
        "</style><div id=grid><b id=icon></b><b id=label>Radio</b></div>"));
    CHECK(parity_box(&page, "grid")->height == 35
          && parity_box(&page, "label")->x == 46
          && parity_box(&page, "label")->y >= 4
          && parity_box(&page, "label")->y <= 5);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_css_replacement_image(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}"
        ".icon{content:url('data:image/svg+xml,<svg width=\"16\" height=\"16\"><path d=\"M0 0h16v16H0Z\"/></svg>')}"
        "#block{display:block;background-image:url('data:image/svg+xml,<svg width=\"8\" height=\"8\"><path d=\"M0 0h8v8H0Z\"/></svg>')}"
        "#ordinary{content:none}</style>"
        "<span id=icon class=icon>Hidden</span><span>1:23</span>"
        "<div id=block class=icon>AlsoHidden</div>"
        "<span id=ordinary class=icon>Visible</span>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    lxb_dom_node_t *root = lxb_dom_interface_node(page.document.html);
    lxb_dom_node_t *icon = find_id(root, "icon");
    ComputedStyle style = style_for_node(&page.sheet, icon, NULL);
    CHECK(style.generated_image && style.generated_text != NULL);
    CHECK(!style_for_node(&page.sheet, find_id(root, "ordinary"), NULL).generated_image);
    ImageResources images = {0};
    CHECK(images_load_external(&page.document, &page.sheet, &images,
        &page.budget, "https://replacement.test/", "https://replacement.test/",
        NULL, 8, 64 * 1024, 32 * 1024, 64 * 1024, 1000, NULL, NULL));
    const ImageResource *image = images_find_background_source(
        &images, icon, style.generated_text, PSEUDO_NONE);
    CHECK(image_resource_available(image) && image->width == 16 && image->height == 16);
    lxb_dom_node_t *block = find_id(root, "block");
    ComputedStyle block_style = style_for_node(&page.sheet, block, NULL);
    const ImageResource *background = images_find_background_source(
        &images, block, block_style.background_image, PSEUDO_NONE);
    CHECK(image_resource_available(background)
        && background->width == 8 && background->height == 8);
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget,
        &page.document, &page.sheet, NULL, &images, 480);
    CHECK(page.layout_ready);
    CHECK(parity_box(&page, "icon")->width == 16
        && parity_box(&page, "icon")->height == 16
        && parity_box(&page, "block")->width == 16
        && parity_box(&page, "block")->height == 16);
    CHECK(!text_command(&page.layout, "Hidden")
        && !text_command(&page.layout, "AlsoHidden")
        && text_command(&page.layout, "Visible"));
    bool painted = false;
    for (size_t i = 0; i < page.layout.count; i++)
        if (page.layout.commands[i].type == DRAW_IMAGE
            && page.layout.commands[i].image == image) painted = true;
    CHECK(painted);
    layout_destroy(&page.layout); page.layout_ready = false;
    images_destroy(&images);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_unknown_media_dimensions(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.column{width:448px;display:flex;flex-direction:column}"
        "video,iframe,canvas{display:block;width:100%;height:100%;vertical-align:top}"
        "#ratio{aspect-ratio:16/9}#tall{width:auto;height:200px}</style>"
        "<div class=column><div><video id=video></video></div></div>"
        "<div class=column><div><iframe id=frame></iframe></div></div>"
        "<div class=column><div><canvas id=canvas></canvas></div></div>"
        "<div class=column><div><video id=ratio></video></div></div>"
        "<video id=tall></video>"));
    CHECK(parity_box(&page, "video")->width == 448
        && parity_box(&page, "video")->height == 150);
    CHECK(parity_box(&page, "frame")->width == 448
        && parity_box(&page, "frame")->height == 150);
    CHECK(parity_box(&page, "canvas")->width == 448
        && parity_box(&page, "canvas")->height == 224);
    CHECK(parity_box(&page, "ratio")->height == 252);
    CHECK(parity_box(&page, "tall")->width == 300
        && parity_box(&page, "tall")->height == 200);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_nested_flex_baseline(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;font:16px sans-serif}"
        ".atomic{display:inline-block;width:200px}.flex{display:flex;flex-direction:column}"
        ".label{font-size:12px;line-height:18px}.title{font-size:20px;line-height:26px}"
        ".marker{display:inline-block;width:1px;height:1px}</style>"
        "<div id=outer><span id=atomic class=atomic><span class=flex>"
        "<span class=label>Label</span><span class=title>First line<br>Second line</span>"
        "</span></span><span id=baseline class=marker></span></div>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    FontSet fonts = {0};
    CHECK(font_set_load_selected(&fonts, &page.budget, TILEFINCH_TEST_SANS_FONT,
        NULL, NULL, NULL, NULL, NULL, NULL, 2 * MIB, FONT_SET_FACE_SANS));
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget,
        &page.document, &page.sheet, &fonts, NULL, 480);
    CHECK(page.layout_ready);
    const LayoutNodeBox *atomic = parity_box(&page, "atomic");
    const LayoutNodeBox *baseline = parity_box(&page, "baseline");
    const DrawCommand *label = text_command(&page.layout, "Label");
    CHECK(atomic && baseline && label);
    int label_baseline = label->y + font_line_baseline_at_size(
        &fonts.sans, 12 * 64, label->height);
    if (baseline->y + baseline->height != label_baseline) fprintf(stderr,
        "nested baseline: marker=%d/%d label=%d atomic-y=%d\n",
        baseline->y, baseline->height, label_baseline, atomic->y);
    CHECK(baseline->y + baseline->height == label_baseline && atomic->height == 70);
    font_set_destroy(&fonts);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* align-items:baseline aligns the items' text baselines, which include
   the half-leading of each item's line-height (CSS Flexbox 8.5,
   CSS 2.1 10.8.1): two items whose line-heights differ must still share
   one baseline. */
static int test_flex_row_baseline_line_height(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;font:16px sans-serif}"
        "#row{display:flex;align-items:baseline}"
        "#tight{line-height:16px}#loose{line-height:40px}</style>"
        "<div id=row><span id=tight>Tight</span><span id=loose>Loose</span></div>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    FontSet fonts = {0};
    CHECK(font_set_load_selected(&fonts, &page.budget, TILEFINCH_TEST_SANS_FONT,
        NULL, NULL, NULL, NULL, NULL, NULL, 2 * MIB, FONT_SET_FACE_SANS));
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget,
        &page.document, &page.sheet, &fonts, NULL, 480);
    CHECK(page.layout_ready);
    const DrawCommand *tight = text_command(&page.layout, "Tight");
    const DrawCommand *loose = text_command(&page.layout, "Loose");
    CHECK(tight && loose && tight->height != loose->height);
    int tight_baseline = tight->y + font_line_baseline_at_size(
        &fonts.sans, 16 * 64, tight->height);
    int loose_baseline = loose->y + font_line_baseline_at_size(
        &fonts.sans, 16 * 64, loose->height);
    if (tight_baseline != loose_baseline) fprintf(stderr,
        "flex baseline: tight=%d (y=%d h=%d) loose=%d (y=%d h=%d)\n",
        tight_baseline, tight->y, tight->height,
        loose_baseline, loose->y, loose->height);
    CHECK(tight_baseline == loose_baseline);
    font_set_destroy(&fonts);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* A naturally sized column flex container exports its first item's first
   baseline (CSS Flexbox 8.5). Relative positioning and z-index leave a box
   in flow (CSS Position 3, 3.4), so their text sets that baseline just as
   it does in a row; only out-of-flow (absolute) text is skipped. Each first
   item here is a row flex container, which exports no completed line
   baseline, so the column scans the item's text. */
static int test_column_flex_positioned_item_baseline(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;font:16px sans-serif}"
        ".atomic{display:inline-block;width:200px}"
        ".col{display:flex;flex-direction:column}.row{display:flex}"
        ".label{font-size:12px;line-height:18px}"
        ".tail{font-size:20px;line-height:26px}"
        ".badge{position:absolute;top:0;left:300px;font-size:30px;line-height:40px}"
        ".marker{display:inline-block;width:1px;height:1px}</style>"
        "<div><span class=atomic><span class=col><span class=row>"
        "<span class=label style=position:relative>Relative</span></span>"
        "<span class=tail>Tail</span></span></span>"
        "<span id=m1 class=marker></span></div>"
        "<div><span class=atomic><span class=col><span class=row>"
        "<span class=label style=z-index:1>Stacked</span></span>"
        "<span class=tail>Tail</span></span></span>"
        "<span id=m2 class=marker></span></div>"
        "<div><span class=atomic><span class=col>"
        "<span class=row style=position:relative><span class=label>Moved</span></span>"
        "<span class=tail>Tail</span></span></span>"
        "<span id=m3 class=marker></span></div>"
        "<div><span class=atomic><span class=col><span class=row>"
        "<span style=position:relative><span class=badge>Badge</span></span>"
        "<span class=label>Flowing</span></span>"
        "<span class=tail>Tail</span></span></span>"
        "<span id=m4 class=marker></span></div>";
    static const char *const labels[] = {
        "Relative", "Stacked", "Moved", "Flowing"
    };
    static const char *const markers[] = { "m1", "m2", "m3", "m4" };
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    FontSet fonts = {0};
    CHECK(font_set_load_selected(&fonts, &page.budget, TILEFINCH_TEST_SANS_FONT,
        NULL, NULL, NULL, NULL, NULL, NULL, 2 * MIB, FONT_SET_FACE_SANS));
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget,
        &page.document, &page.sheet, &fonts, NULL, 480);
    CHECK(page.layout_ready);
    int failures = 0;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        const LayoutNodeBox *marker = parity_box(&page, markers[i]);
        const DrawCommand *label = text_command(&page.layout, labels[i]);
        CHECK(marker && label);
        int label_baseline = label->y + font_line_baseline_at_size(
            &fonts.sans, 12 * 64, label->height);
        if (marker->y + marker->height != label_baseline) {
            fprintf(stderr, "column baseline %s: marker=%d label=%d\n",
                    labels[i], marker->y + marker->height, label_baseline);
            failures++;
        }
    }
    CHECK(failures == 0);
    font_set_destroy(&fonts);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* Row align-items:baseline uses the same first baseline: text in an
   absolutely positioned descendant is out of flow and must not set it,
   while relatively positioned or z-indexed text does (see above). */
static int test_flex_row_out_of_flow_baseline(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;font:16px sans-serif}"
        "#row{display:flex;align-items:baseline}.inner{display:flex}"
        ".label{font-size:12px;line-height:18px}"
        ".badge{position:absolute;top:0;left:300px;font-size:30px;line-height:40px}"
        "#outer{line-height:16px}</style>"
        "<div id=row><span class=inner>"
        "<span style=position:relative><span class=badge>Badge</span></span>"
        "<span class=label style=z-index:1>Inner</span></span>"
        "<span id=outer>Outer</span></div>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    FontSet fonts = {0};
    CHECK(font_set_load_selected(&fonts, &page.budget, TILEFINCH_TEST_SANS_FONT,
        NULL, NULL, NULL, NULL, NULL, NULL, 2 * MIB, FONT_SET_FACE_SANS));
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget,
        &page.document, &page.sheet, &fonts, NULL, 480);
    CHECK(page.layout_ready);
    const DrawCommand *inner = text_command(&page.layout, "Inner");
    const DrawCommand *outer = text_command(&page.layout, "Outer");
    CHECK(inner && outer);
    int inner_baseline = inner->y + font_line_baseline_at_size(
        &fonts.sans, 12 * 64, inner->height);
    int outer_baseline = outer->y + font_line_baseline_at_size(
        &fonts.sans, 16 * 64, outer->height);
    if (inner_baseline != outer_baseline) fprintf(stderr,
        "row out-of-flow baseline: inner=%d outer=%d\n",
        inner_baseline, outer_baseline);
    CHECK(inner_baseline == outer_baseline);
    font_set_destroy(&fonts);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* Out-of-flow text with no out-of-flow node box of its own must not set a
   flex first baseline either: an absolutely positioned ::before/::after
   (painted without a node box) and absolutely positioned form controls.
   Column cases compare a marker after the inline-block column with the
   first item's label; row cases compare the item's label with an outer
   item's text. */
static int test_flex_out_of_flow_generated_baseline(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;font:16px sans-serif}"
        ".atomic{display:inline-block;width:200px}"
        ".col{display:flex;flex-direction:column}.row{display:flex}"
        ".label{font-size:12px;line-height:18px}"
        ".tail{font-size:20px;line-height:26px}"
        ".badged::before{content:'NEW';position:absolute;top:0;left:300px;"
        "font-size:30px;line-height:40px}"
        ".abs{position:absolute;top:0;left:300px;font-size:30px;"
        "line-height:40px;height:40px}"
        ".marker{display:inline-block;width:1px;height:1px}"
        ".base{display:flex;align-items:baseline}.inner{display:flex}"
        ".outer{line-height:16px}</style>"
        "<div><span class=atomic><span class=col><span class=row>"
        "<span class='label badged'>Pseudo</span></span>"
        "<span class=tail>Tail</span></span></span>"
        "<span id=m1 class=marker></span></div>"
        "<div><span class=atomic><span class=col><span class=row>"
        "<input class=abs value=Badge><span class=label>Control</span></span>"
        "<span class=tail>Tail</span></span></span>"
        "<span id=m2 class=marker></span></div>"
        "<div><span class=atomic><span class=col><span class=row>"
        "<button class=abs>Badge</button><span class=label>Button</span></span>"
        "<span class=tail>Tail</span></span></span>"
        "<span id=m3 class=marker></span></div>"
        "<div class=base><span class=inner>"
        "<span class='label badged'>RowPseudo</span></span>"
        "<span class=outer>OuterA</span></div>"
        "<div class=base><span class=inner>"
        "<input class=abs value=Badge><span class=label>RowControl</span>"
        "</span><span class=outer>OuterB</span></div>";
    static const char *const labels[] = { "Pseudo", "Control", "Button" };
    static const char *const markers[] = { "m1", "m2", "m3" };
    static const char *const row_labels[] = { "RowPseudo", "RowControl" };
    static const char *const row_outers[] = { "OuterA", "OuterB" };
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    FontSet fonts = {0};
    CHECK(font_set_load_selected(&fonts, &page.budget, TILEFINCH_TEST_SANS_FONT,
        NULL, NULL, NULL, NULL, NULL, NULL, 2 * MIB, FONT_SET_FACE_SANS));
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget,
        &page.document, &page.sheet, &fonts, NULL, 480);
    CHECK(page.layout_ready);
    int failures = 0;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        const LayoutNodeBox *marker = parity_box(&page, markers[i]);
        const DrawCommand *label = text_command(&page.layout, labels[i]);
        CHECK(marker && label);
        int label_baseline = label->y + font_line_baseline_at_size(
            &fonts.sans, 12 * 64, label->height);
        if (marker->y + marker->height != label_baseline) {
            fprintf(stderr, "column out-of-flow baseline %s: marker=%d "
                    "label=%d\n", labels[i], marker->y + marker->height,
                    label_baseline);
            failures++;
        }
    }
    for (size_t i = 0; i < sizeof(row_labels) / sizeof(row_labels[0]); i++) {
        const DrawCommand *inner = text_command(&page.layout, row_labels[i]);
        const DrawCommand *outer = text_command(&page.layout, row_outers[i]);
        CHECK(inner && outer);
        int inner_baseline = inner->y + font_line_baseline_at_size(
            &fonts.sans, 12 * 64, inner->height);
        int outer_baseline = outer->y + font_line_baseline_at_size(
            &fonts.sans, 16 * 64, outer->height);
        if (inner_baseline != outer_baseline) {
            fprintf(stderr, "row out-of-flow baseline %s: inner=%d "
                    "outer=%d\n", row_labels[i], inner_baseline,
                    outer_baseline);
            failures++;
        }
    }
    CHECK(failures == 0);
    font_set_destroy(&fonts);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* Stacked slides: a baseline-aligned item whose in-flow label follows 100
   absolutely positioned slides, each in a relatively positioned wrapper
   (200 descendant boxes). The out-of-flow walk is one bounded pass (256
   boxes, a 32-entry range list); the run of slides must still skip as one
   range rather than overflow the list. The old walk restarted for every
   skipped slide and visited 20,100 boxes per baseline here. */
static int test_flex_row_many_out_of_flow_baseline(void)
{
    static const char head[] =
        "<!doctype html><style>body{margin:0;font:16px sans-serif}"
        "#row{display:flex;align-items:baseline}.inner{display:flex}"
        ".label{font-size:12px;line-height:18px}"
        ".slide{position:absolute;top:0;left:300px;font-size:30px;"
        "line-height:40px}#outer{line-height:16px}</style>"
        "<div id=row><span class=inner>";
    static const char slide[] =
        "<span style=position:relative><span class=slide>Slide</span></span>";
    static const char tail[] =
        "<span class=label>Inner</span></span>"
        "<span id=outer>Outer</span></div>";
    char html[sizeof(head) + 100 * (sizeof(slide) - 1) + sizeof(tail)];
    size_t length = 0;
    memcpy(html, head, sizeof(head) - 1);
    length += sizeof(head) - 1;
    for (int i = 0; i < 100; i++) {
        memcpy(html + length, slide, sizeof(slide) - 1);
        length += sizeof(slide) - 1;
    }
    memcpy(html + length, tail, sizeof(tail));
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    FontSet fonts = {0};
    CHECK(font_set_load_selected(&fonts, &page.budget, TILEFINCH_TEST_SANS_FONT,
        NULL, NULL, NULL, NULL, NULL, NULL, 2 * MIB, FONT_SET_FACE_SANS));
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget,
        &page.document, &page.sheet, &fonts, NULL, 480);
    CHECK(page.layout_ready);
    const DrawCommand *inner = text_command(&page.layout, "Inner");
    const DrawCommand *outer = text_command(&page.layout, "Outer");
    CHECK(inner && outer);
    int inner_baseline = inner->y + font_line_baseline_at_size(
        &fonts.sans, 12 * 64, inner->height);
    int outer_baseline = outer->y + font_line_baseline_at_size(
        &fonts.sans, 16 * 64, outer->height);
    if (inner_baseline != outer_baseline) fprintf(stderr,
        "row stacked slides baseline: inner=%d outer=%d\n",
        inner_baseline, outer_baseline);
    CHECK(inner_baseline == outer_baseline);
    font_set_destroy(&fonts);
    CHECK(parity_page_destroy(&page));
    return 0;
}

/* A generated flex item's box lives in the flex order plan, not in
   layout->node_boxes, and has no descendant boxes. Its relatively
   positioned (and z-indexed) text is in the positioned paint phase, still
   in flow, so it sets the item's baseline. The old out-of-flow walk did
   pointer arithmetic between the plan's box and node_boxes and read
   records outside the array (an ASan report in the sanitize preset). */
static int test_flex_generated_positioned_item_baseline(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;font:16px sans-serif}"
        ".row{display:flex;align-items:baseline;line-height:16px}"
        ".rel::before{content:'$';position:relative;font-size:28px;"
        "line-height:34px}"
        ".stack::before{content:'#';position:relative;z-index:2;"
        "font-size:28px;"
        "line-height:34px}</style>"
        "<div class='row rel'><span id=a>Rel</span></div>"
        "<div class='row stack'><span id=b>Stack</span></div>";
    static const char *const labels[] = { "Rel", "Stack" };
    static const char *const marks[] = { "$", "#" };
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    FontSet fonts = {0};
    CHECK(font_set_load_selected(&fonts, &page.budget, TILEFINCH_TEST_SANS_FONT,
        NULL, NULL, NULL, NULL, NULL, NULL, 2 * MIB, FONT_SET_FACE_SANS));
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget,
        &page.document, &page.sheet, &fonts, NULL, 480);
    CHECK(page.layout_ready);
    int failures = 0;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        const DrawCommand *label = text_command(&page.layout, labels[i]);
        const DrawCommand *mark = text_command(&page.layout, marks[i]);
        CHECK(label && mark);
        if (!draw_command_has_positioned_phase(mark)) {
            fprintf(stderr, "generated item %s: not positioned phase\n",
                    marks[i]);
            failures++;
        }
        int label_baseline = label->y + font_line_baseline_at_size(
            &fonts.sans, 16 * 64, label->height);
        int mark_baseline = mark->y + font_line_baseline_at_size(
            &fonts.sans, 28 * 64, mark->height);
        if (label_baseline != mark_baseline) {
            fprintf(stderr, "generated item baseline %s: label=%d mark=%d\n",
                    labels[i], label_baseline, mark_baseline);
            failures++;
        }
    }
    CHECK(failures == 0);
    font_set_destroy(&fonts);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_fractional_tracking(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0;font-size:16px;line-height:20px}"
        ".half{letter-spacing:.5px}.negative{letter-spacing:calc(-.25px - .25px)}"
        "#wrap{width:50px;word-break:break-all}"
        "#split,#negative{white-space:nowrap}</style>"
        "<div id=wrap class=half>ABCD</div>"
        "<div id=split class=half>R<span>S</span>T</div>"
        "<div id=negative class=negative>U<span>V</span>W</div>"
        "<div class=half><span>A</span><span>B</span><span>C</span><span>D</span></div>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *wrap = parity_box(&page, "wrap");
    CHECK(wrap && wrap->height == 20);
    const DrawCommand *r = text_command(&page.layout, "R");
    const DrawCommand *s = text_command(&page.layout, "S");
    const DrawCommand *t = text_command(&page.layout, "T");
    const DrawCommand *u = text_command(&page.layout, "U");
    const DrawCommand *v = text_command(&page.layout, "V");
    const DrawCommand *w = text_command(&page.layout, "W");
    CHECK(r && s && t && u && v && w);
    CHECK(draw_command_text_letter_spacing_fixed(r) == 32
        && draw_command_text_scale(r) == 2
        && draw_command_text_x_fixed(s) - draw_command_text_x_fixed(r) == 12 * 64 + 32
        && draw_command_text_x_fixed(t) - draw_command_text_x_fixed(s) == 12 * 64 + 32);
    CHECK(draw_command_text_letter_spacing_fixed(u) == -32
        && draw_command_text_x_fixed(v) - draw_command_text_x_fixed(u) == 12 * 64 - 32
        && draw_command_text_x_fixed(w) - draw_command_text_x_fixed(v) == 12 * 64 - 32);
    DrawCommand packed = *r;
    for (int fixed = -128 * 64; fixed <= 127 * 64 + 63; fixed += 17) {
        draw_command_set_text_letter_spacing_fixed(&packed, fixed);
        CHECK(draw_command_text_letter_spacing_fixed(&packed) == fixed
            && draw_command_text_scale(&packed) == 2
            && packed.radius == r->radius && packed.image_fit == r->image_fit);
    }
    TileCache cache;
    uint16_t *frame = budget_malloc(&page.budget, 480u * 80u * sizeof(*frame));
    CHECK(frame && tile_cache_init(&cache, &page.budget, &page.layout, 4)
        && tile_cache_set_frame(&cache, frame, 480u * 80u)
        && tile_cache_render_frame(&cache, 0, 480, 80, NULL));
    /* Independent split commands paint the same half-pixel glyph positions. */
    CHECK(memcmp(frame, frame + 480u * 60u, 480u * 20u * sizeof(*frame)) == 0);
    tile_cache_destroy(&cache);
    budget_free(&page.budget, frame);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_flex_thumbnail_text_column(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}.row{display:flex;width:448px}"
        ".row>a{display:flex;width:100%}.row>a:first-child{flex:1}"
        ".media{display:flex;flex-direction:column;min-width:95px;align-self:flex-start}"
        ".picture{width:95px;height:95px}.text{display:inline-block;width:100%;"
        "flex:75%;align-items:flex-start;margin-inline-start:12px}"
        ".title{display:block}</style><div class=row>"
        "<a><div class=media><div class=picture></div></div></a>"
        "<a><div id=text class=text><span id=title class=title>"
        "A long descriptive title for the adjacent story card</span></div></a></div>";
    ParityPage page;
    CHECK(parity_page_load(&page, html));
    const LayoutNodeBox *text = parity_box(&page, "text");
    const LayoutNodeBox *title = parity_box(&page, "title");
    CHECK(text && title && text->x == 107 && text->width == 341
        && title->x == 107 && title->width == 341);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_column_generated_ratio_spacer(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.media{display:flex;"
        "flex-direction:column;position:relative;overflow:hidden;width:448px}"
        ".media::after{content:'';display:block;padding-bottom:56.25%}"
        "picture{width:100%}img{position:absolute;inset:0;width:100%;height:100%}"
        "h2{margin:0}</style><div class=media id=media><picture>"
        "<img id=image width=448 height=252></picture></div><h2 id=title>Title</h2>"));
    const LayoutNodeBox *media = parity_box(&page, "media");
    const LayoutNodeBox *image = parity_box(&page, "image");
    const LayoutNodeBox *title = parity_box(&page, "title");
    CHECK(media && image && title && media->height == 252
        && image->height == 252 && title->y >= 252);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_positioned_flex_pseudo_static_alignment(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}.row{position:relative;"
        "display:flex;align-items:center;width:56px;height:23px}"
        ".row::before{content:'';position:absolute;right:-12px;"
        "width:32px;height:32px;background:#123456}"
        ".row::after{content:'';display:inline-block;width:12px;height:12px}"
        "</style><div class=row></div>"));
    const DrawCommand *circle = parity_fill(&page.layout, 0x123456u, 0);
    CHECK(circle && circle->x == 36 && circle->y == -4
        && circle->width == 32 && circle->height == 32);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_negative_margin_reaches_enclosing_floats(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;width:480px}"
        "header{height:44px}header i{float:left;width:176px;height:44px}"
        "header b{float:right;width:80px;height:44px}"
        "section{position:relative}section::before{content:'';display:table}"
        "#strip{width:100%;height:64px;overflow:hidden;margin-top:-5px}"
        "</style><header><i></i><b></b></header>"
        "<section id=parent><div id=strip></div></section>"));
    const LayoutNodeBox *parent = parity_box(&page, "parent");
    const LayoutNodeBox *strip = parity_box(&page, "strip");
    CHECK(parent && strip && parent->y == 44
          && strip->y == 44 && strip->width == 480);
    CHECK(parity_page_destroy(&page));
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;width:480px}"
        "header{height:44px}header i{float:left;width:60px;height:44px}"
        "#box::after{content:'';display:table;clear:both}"
        "#box b{float:left;width:200px;height:20px}"
        "</style><header><i></i><i></i><i></i><i></i>"
        "<i></i><i></i><i></i><i></i></header>"
        "<section><div id=box><b></b><b></b><b></b>"
        "<b></b><b></b><b></b></div></section>"));
    const LayoutNodeBox *box = parity_box(&page, "box");
    CHECK(box && box->y == 44 && box->height == 60);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_system_font_family_identity(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>#a{font-family:-apple-system,Arial}"
        "#b{font-family:system-ui}#c{font-family:BlinkMacSystemFont}"
        "#d{font-family:Arial}</style><div id=a>A</div>"
        "<div id=b>B</div><div id=c>C</div><div id=d>D</div>"));
    lxb_dom_node_t *root = lxb_dom_interface_node(page.document.html);
    const char *ids[] = {"a", "b", "c", "d"};
    for (size_t i = 0; i < 4; i++) {
        ComputedStyle style = style_for_node(
            &page.sheet, find_id(root, ids[i]), NULL);
        CHECK(style.font_family == (i == 3
              ? FONT_METRIC_SANS : FONT_SYSTEM_SANS));
    }
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_clamp_requires_vertical_box(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;font:10px/12px monospace}"
        ".text{width:42px;overflow:hidden;text-overflow:ellipsis;"
        "-webkit-line-clamp:1;-webkit-box-orient:vertical}"
        "#plain{display:block}#clamped{display:-webkit-box}"
        "</style><div id=plain class=text>alpha beta gamma</div>"
        "<div id=clamped class=text>alpha beta gamma</div>"));
    const LayoutNodeBox *plain = parity_box(&page, "plain");
    const LayoutNodeBox *clamped = parity_box(&page, "clamped");
    CHECK(plain && clamped && plain->height == 36
          && clamped->height == 12);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_generated_icon_intrinsic_space(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;font:16px/20px monospace}"
        ".row{height:40px}.float{float:right}a{display:inline-block;"
        "padding:3px 12px;border:3px solid;margin-left:8px}"
        "i{font-style:normal}i::before{content:'X'}"
        "#own::before{content:'X'}"
        "</style><div class=row><div class=float><a id=icon><i></i> Alpha</a></div></div>"
        "<div class=row><div class=float><a id=literal>X Alpha</a></div></div>"
        "<div class=row><div class=float><a id=own> Alpha</a></div></div>"));
    const LayoutNodeBox *icon = parity_box(&page, "icon");
    const LayoutNodeBox *literal = parity_box(&page, "literal");
    const LayoutNodeBox *own = parity_box(&page, "own");
    CHECK(icon && literal && own);
    if (icon->width != literal->width || icon->height != literal->height
        || own->width != literal->width || own->height != literal->height)
        fprintf(stderr, "generated spacing icon=%dx%d own=%dx%d literal=%dx%d\n",
            icon->width, icon->height, own->width, own->height,
            literal->width, literal->height);
    CHECK(icon->width == literal->width && icon->height == literal->height
          && own->width == literal->width && own->height == literal->height);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_adjacent_nowrap_inline_boundaries(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;font:16px/20px monospace}"
        ".row{width:130px}.tag{white-space:nowrap;padding:0 7px}"
        "#joined{overflow:hidden;text-overflow:ellipsis}"
        "#multi{overflow:hidden;text-overflow:ellipsis}"
        "#atomic .tag{display:inline-block}"
        "</style><div id=joined class=row><span class=tag>First tag</span>"
        "<span class=tag>Second tag</span><span class=tag>Third tag</span></div>"
        "<div id=visible class=row><span class=tag>First tag</span>"
        "<span class=tag>Second tag</span></div>"
        "<div id=spaced class=row><span class=tag>First tag</span> "
        "<span class=tag>Second tag</span></div>"
        "<div id=atomic class=row><span class=tag>First tag</span>"
        "<span class=tag>Second tag</span></div>"
        "<div id=hyphen class=row>abcdefghij-<span class=tag>Second tag</span></div>"
        "<div id=cjk class=row>日本語日本語<span class=tag>Second tag</span></div>"
        "<div id=multi class=row><span class=tag>Veryverylonglabel</span> "
        "<span class=tag>Tail</span></div>"));
    const LayoutNodeBox *joined = parity_box(&page, "joined");
    const LayoutNodeBox *visible = parity_box(&page, "visible");
    const LayoutNodeBox *spaced = parity_box(&page, "spaced");
    const LayoutNodeBox *atomic = parity_box(&page, "atomic");
    CHECK(joined && visible && spaced && atomic);
    if (joined->height != 20 || visible->height != 20)
        fprintf(stderr, "nowrap boundaries joined=%d visible=%d spaced=%d atomic=%d\n",
            joined->height, visible->height, spaced->height, atomic->height);
    CHECK(joined->height == 20 && visible->height == 20
          && spaced->height > joined->height && atomic->height > joined->height);
    const LayoutNodeBox *hyphen = parity_box(&page, "hyphen");
    const LayoutNodeBox *cjk = parity_box(&page, "cjk");
    const LayoutNodeBox *multi = parity_box(&page, "multi");
    CHECK(hyphen && cjk && multi && hyphen->height == 40
          && cjk->height == 40 && multi->height == 40);
    size_t markers = 0;
    for (size_t i = joined->command_start; i < joined->command_end; i++) {
        const DrawCommand *command = &page.layout.commands[i];
        if (command->type == DRAW_TEXT && command->text_length == 3
            && memcmp(command->text, "\xe2\x80\xa6", 3) == 0) markers++;
    }
    CHECK(markers == 1 && text_command(&page.layout, "Tail") != NULL);
    CHECK(parity_page_destroy(&page));
    /* Refusing the optional marker must leave the original text and hit
       regions available to ordinary overflow clipping. */
    Budget budget;
    budget_init(&budget, MIB);
    DrawCommand *command = budget_calloc(&budget, 1, sizeof(*command));
    CHECK(command != NULL);
    *command = (DrawCommand) {.type = DRAW_TEXT, .text = "Verylonglabel",
        .text_length = 13, .width = 100, .height = 20,
        .font_size = 16, .scale = 1, .opacity_scale = 255};
    LinkRegion link = {.width = 100, .height = 20};
    LayoutDocument layout = {.budget = &budget, .commands = command,
        .count = 1, .capacity = 1, .links = &link, .link_count = 1};
    LineState line = {.layout = &layout, .x = 100, .right = 30,
        .base_right = 30, .line_height = 20, .has_text_character = true,
        .inline_overflow_ellipsis = true};
    budget.limit = budget.current;
    layout_flush_line(&line);
    CHECK(layout.count == 1 && command->text_length == 13
          && command->width == 100 && link.width == 100);
    budget_free(&budget, command);
    CHECK(budget.current == 0);
    return 0;
}

static int test_generated_atomic_nowrap(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;font:16px/22px sans-serif}"
        ".item{width:60px;white-space:nowrap}.pre{white-space:pre}"
        ".item span{display:inline-block;width:60px}"
        ".item::after{content:'|';display:inline-block;padding:0 8px}"
        "</style><div class=item><span>First</span></div>"
        "<div class='item pre'><span>Second</span></div>"));
    const DrawCommand *first = text_command(&page.layout, "First");
    const DrawCommand *second = text_command(&page.layout, "Second");
    const DrawCommand *bars[2] = {0};
    size_t count = 0;
    for (size_t i = 0; i < page.layout.count && count < 2; i++) {
        const DrawCommand *command = &page.layout.commands[i];
        if (command->type == DRAW_TEXT && command->text_length == 1
            && command->text[0] == '|') bars[count++] = command;
    }
    CHECK(first && second && count == 2);
    /* The font-free fallback places atomic text six pixels below the
       sibling ink. Both still occupy the same 22-pixel line, not a second
       line introduced at the generated item's overflow boundary. */
    CHECK(bars[0]->y == first->y + 6 && bars[1]->y == second->y + 6);
    CHECK(second->y == first->y + 22);
    CHECK(bars[0]->x >= 60 && bars[1]->x >= 60);
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

static int test_compact_controls_and_generated_intrinsics(unsigned checks)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0;font:16px Arial}"
        ".row{display:flex;width:480px;gap:12px}"
        "#label{flex:1}#compact{flex:0 0 44px}"
        "input{width:100%;box-sizing:border-box}input::placeholder{opacity:0}"
        "#heading{max-width:fit-content;letter-spacing:2px}"
        "#heading::after{content:'\\00a0|'}"
        "#normal{font-weight:normal}#visible,#entered{color:#102030}"
        "#visible::placeholder{color:#445566;opacity:.5}"
        "#entered::placeholder{color:red;opacity:0}</style><body>"
        "<div class=row><div id=label>Label</div><form id=compact>"
        "<input id=entry placeholder=Search></form></div>"
        "<div id=heading>VIDEOS</div>"
        "<table><tr><th id=header>Header</th><td id=cell>Cell</td>"
        "<th id=normal>Normal</th></tr></table>"
        "<input id=visible placeholder=Hint><input id=entered placeholder=Hint value=Value>"
        "</body>"));
    const LayoutNodeBox *compact = parity_box(&page, "compact");
    const LayoutNodeBox *heading = parity_box(&page, "heading");
    const DrawCommand *word = text_command(&page.layout, "VIDEOS");
    const DrawCommand *suffix = text_command(&page.layout, "\xc2\xa0|");
    if (checks & 1u) CHECK(compact && compact->width == 44);
    if ((checks & 2u) && !(heading && word && suffix && word->y == suffix->y)) {
        fprintf(stderr, "generated intrinsic: heading=%d word=%d,%d suffix=%d,%d\n",
            heading ? heading->width : -1, word ? word->x : -1,
            word ? word->y : -1, suffix ? suffix->x : -1, suffix ? suffix->y : -1);
        for (size_t i = 0; i < page.layout.count; i++) {
            const DrawCommand *c = &page.layout.commands[i];
            if (c->type == DRAW_TEXT) fprintf(stderr, "text %.*s %d,%d %dx%d\n",
                (int) c->text_length, c->text, c->x, c->y, c->width, c->height);
        }
    }
    if (checks & 2u) CHECK(heading && word && suffix && word->y == suffix->y);
    if (checks & 4u) {
        CHECK(text_command(&page.layout, "Search") == NULL);
        const DrawCommand *hint = text_command(&page.layout, "Hint");
        const DrawCommand *entered = text_command(&page.layout, "Value");
        CHECK(hint && hint->color == 0x445566
            && hint->opacity_scale == alpha_opacity_scale(128));
        CHECK(entered && entered->color == 0x102030
            && entered->opacity_scale == alpha_opacity_scale(255));
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(page.document.html);
    if (checks & 8u) CHECK(style_for_node(&page.sheet, find_id(root, "header"), NULL).font_weight == 700);
    CHECK(style_for_node(&page.sheet, find_id(root, "cell"), NULL).font_weight == 400);
    CHECK(style_for_node(&page.sheet, find_id(root, "normal"), NULL).font_weight == 400);
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_inline_badge_baseline_and_mask_size(unsigned checks)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}p{margin:0;font-size:20px;line-height:40px}"
        "#badge{font-size:10px;padding:0 4px 1px;border:1px solid #112233;"
        "border-radius:5px;background:#aabbcc}"
        "#icon{position:relative;width:30px;height:40px}"
        "#icon::before{content:'';position:absolute;width:15px;height:100%;"
        "background:#123456;mask:url(icon.svg) center/15px 15px no-repeat}"
        "</style><body><p>Large <span id=badge>TAG</span></p><div id=icon></div></body>"));
    const DrawCommand *text = text_command(&page.layout, "TAG");
    const DrawCommand *fill = parity_fill(&page.layout, 0xaabbcc, 0);
    const DrawCommand *border = NULL;
    for (size_t i = 0; i < page.layout.count; i++) {
        const DrawCommand *c = &page.layout.commands[i];
        if (c->type == DRAW_STROKE_RECT && c->color == 0x112233) border = c;
    }
    CHECK(text && fill && border);
    if (checks & 1u) {
        CHECK(fill->height == text->height + 3 && border->height == fill->height);
        CHECK(fill->y == text->y - 1 && border->y == fill->y);
        CHECK(fill->radius == border->radius);
        CHECK(parity_box(&page, "icon")->y == 40);
    }
    lxb_dom_node_t *icon = find_id(lxb_dom_interface_node(page.document.html), "icon");
    static unsigned char pixels[16] = {255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255};
    ImageResource mask = {.node=icon, .pixels=pixels, .width=2, .height=2,
        .is_mask=true, .pseudo=PSEUDO_BEFORE};
    ImageResources images = {.items=&mask, .count=1};
    layout_destroy(&page.layout);
    page.layout_ready = layout_build(&page.layout, &page.budget, &page.document,
        &page.sheet, NULL, &images, 480);
    CHECK(page.layout_ready);
    const DrawCommand *paint = NULL;
    for (size_t i = 0; i < page.layout.count; i++)
        if (page.layout.commands[i].type == DRAW_IMAGE
            && page.layout.commands[i].image == &mask) paint = &page.layout.commands[i];
    CHECK(paint && paint->width == 15 && paint->height == 40);
    if (checks & 2u) {
        CHECK(paint->image_fit == LAYOUT_IMAGE_FIT_SPRITE);
        CHECK(draw_command_image_sprite_width(paint) == 15
            && draw_command_image_sprite_height(paint) == 15);
        CHECK(draw_command_image_offset_y(paint) == 12);
    }
    CHECK(parity_page_destroy(&page));
    return 0;
}

static int test_generated_rounded_border_sides(void)
{
    ParityPage page;
    CHECK(parity_page_load(&page,
        "<!doctype html><style>body{margin:0}#host{position:relative;height:60px}"
        "#host::before{content:'';position:absolute;left:8px;top:8px;"
        "width:20px;height:20px;border:solid #112233;"
        "border-width:1px 2px 3px 4px;border-radius:8px}"
        "</style><body><div id=host></div></body>"));
    DrawCommand *border = NULL;
    for (size_t i = 0; i < page.layout.count; i++) {
        DrawCommand *c = &page.layout.commands[i];
        if (c->type == DRAW_STROKE_RECT && c->color == 0x112233) border = c;
    }
    CHECK(border && border->image_fit == LAYOUT_STROKE_SIDES);
    CHECK(border->radius != 0 && border->width > 16 && border->height > 16);
    for (unsigned i = 0; i < 4; i++) CHECK(draw_command_stroke_side(border, i) == i + 1);
    TileCache cache = {0};
    uint16_t pixels[480u * 60u];
    CHECK(tile_cache_init(&cache, &page.budget, &page.layout, 8));
    CHECK(tile_cache_set_frame(&cache, pixels, 480u * 60u));
    CHECK(tile_cache_render_frame(&cache, 0, 480, 60, NULL));
    CHECK(pixels[border->y * 480 + border->x] != test_rgb565(0x112233));
    CHECK(pixels[border->y * 480 + border->x + border->width / 2] == test_rgb565(0x112233));
    CHECK(pixels[(border->y + border->height / 2) * 480 + border->x] == test_rgb565(0x112233));
    CHECK(pixels[(border->y + border->height / 2) * 480 + border->x + border->width / 2] != test_rgb565(0x112233));
    tile_cache_destroy(&cache);
    size_t index = (size_t) (border - page.layout.commands);
    layout_scale_range(&page.layout, index, page.layout.link_count,
        page.layout.control_count, page.layout.node_box_count, 0, 0, 32768, NULL);
    for (unsigned i = 0; i < 4; i++) CHECK(draw_command_stroke_side(border, i) == (i + 1) * 2);
    layout_rotate_range_quadrants(&page.layout, index, page.layout.link_count,
        page.layout.control_count, page.layout.node_box_count, 0, 0, 1, NULL);
    const unsigned rotated[4] = {8, 2, 4, 6};
    for (unsigned i = 0; i < 4; i++) CHECK(draw_command_stroke_side(border, i) == rotated[i]);
    CHECK(parity_page_destroy(&page));
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
    CHECK(test_generated_text_flex_order() == 0);
    CHECK(test_block_text_control_auto_height() == 0);
    CHECK(test_generated_flex_percentage_edges_and_wrap() == 0);
    CHECK(test_empty_inline_positioned_border_pseudo() == 0);
    CHECK(test_inline_replaced_percentage_height() == 0);
    CHECK(test_parity_generated_inline_spacing() == 0);
    CHECK(test_generated_atomic_nowrap() == 0);
    CHECK(test_generated_icon_intrinsic_space() == 0);
    CHECK(test_adjacent_nowrap_inline_boundaries() == 0);
    CHECK(test_clamp_requires_vertical_box() == 0);
    CHECK(test_negative_margin_reaches_enclosing_floats() == 0);
    CHECK(test_system_font_family_identity() == 0);
    CHECK(test_variable_gradient_token_boundaries() == 0);
    CHECK(test_compact_background_geometry() == 0);
    CHECK(test_percentage_image_auto_height() == 0);
    CHECK(test_floating_generated_list_marker() == 0);
    CHECK(test_intrinsic_float_rows() == 0);
    CHECK(test_border_color_triangles() == 0);
    CHECK(test_positioned_percentage_height_after_flow() == 0);
    CHECK(test_page_font_normal_leading() == 0);
    CHECK(test_generated_inline_padding() == 0);
    CHECK(test_fractional_tracking() == 0);
    CHECK(test_nested_flex_baseline() == 0);
    CHECK(test_flex_row_baseline_line_height() == 0);
    CHECK(test_column_flex_positioned_item_baseline() == 0);
    CHECK(test_flex_row_out_of_flow_baseline() == 0);
    CHECK(test_flex_generated_positioned_item_baseline() == 0);
    CHECK(test_flex_out_of_flow_generated_baseline() == 0);
    CHECK(test_flex_row_many_out_of_flow_baseline() == 0);
    CHECK(test_css_replacement_image() == 0);
    CHECK(test_button_content_centering() == 0);
    CHECK(test_nested_toolbar_intrinsics() == 0);
    CHECK(test_legacy_grid_gaps() == 0);
    CHECK(test_nested_button_duration() == 0);
    CHECK(test_auto_fractional_grid_rows() == 0);
    CHECK(test_unknown_media_dimensions() == 0);
    CHECK(test_flex_thumbnail_text_column() == 0);
    CHECK(test_column_generated_ratio_spacer() == 0);
    CHECK(test_positioned_flex_pseudo_static_alignment() == 0);
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
    CHECK(test_compact_controls_and_generated_intrinsics(15) == 0);
    CHECK(test_inline_badge_baseline_and_mask_size(3) == 0);
    CHECK(test_generated_rounded_border_sides() == 0);
    return 0;
}
