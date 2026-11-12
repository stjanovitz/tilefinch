/* One image changing on a large page costs work proportional to that image:
   no document walk, no table rebuild, no cascade reset, and one relayout for
   several changes arriving together. Each shortcut keeps its slow path: a box
   that can change is laid out, a revealed or newly styled image is found. */
#include "tilefinch/budget.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/layout.h"
#include "tilefinch/navigation.h"
#include "tilefinch/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define GIF "data:image/gif;base64," \
    "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw=="
/* The same pixels at another address. */
#define GIF_AT(n) "data:image/gif;v=" #n ";base64," \
    "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw=="
#define BROKEN "data:image/gif;base64,AAAA"
/* A document walk visits every element below; these pages have thousands. */
#define LOCAL_VISITS 200u

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return false;                                                      \
    }                                                                      \
} while (0)

static Budget budget;
static NavigationSession navigation;
static char html[1024 * 1024];

static bool open_page(const char *head, const char *body, size_t filler,
                      const char *tail)
{
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:0}b{display:block;height:1px}"
        "%s</style><body>%s", head, body);
    for (size_t i = 0; i < filler && used + 16u < sizeof(html); i++)
        used += (size_t) snprintf(html + used, sizeof(html) - used, "<b></b> ");
    used += (size_t) snprintf(html + used, sizeof(html) - used, "%s", tail);
    if (used + 1u >= sizeof(html) || !navigation_init(&navigation, &budget, 2))
        return false;
    /* These count relayouts; how long one took must not change their kind. */
    navigation.relayout_preview_threshold_us = UINT64_MAX;
    navigation_enable_scripts(&navigation, 3 * MIB, 1000);
    navigation_enable_external_resources(
        &navigation, 2, 32 * 1024, 32 * 1024,
        64, 64 * 1024, 32 * 1024, 64 * 1024, 1000);
    uint64_t generation = navigation_begin(&navigation);
    return navigation_commit_html(
               &navigation, generation, "https://img-relayout.test/", html,
               used, 480, NULL, NULL, true)
        && navigation_complete_layout_sync(&navigation);
}

static bool evaluate(const char *script)
{
    return script_runtime_evaluate_diagnostic(
        navigation.page.runtime, script, "<img-relayout>",
        &navigation.page.script_result);
}

/* Runs the script and the relayout it asks for. */
static bool step(const char *script)
{
    return evaluate(script)
        && script_runtime_consume_relayout(navigation.page.runtime)
        && navigation_relayout(&navigation)
        && navigation_complete_layout_sync(&navigation);
}

static size_t image_visits(void)
{
    const ExternalImageStats *stats = &navigation.page.images.stats;
    return stats->node_style_cache_hits + stats->node_style_cache_misses;
}

static lxb_dom_node_t *find_id_below(lxb_dom_node_t *node, const char *id)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->next) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            const lxb_char_t *value = lxb_dom_element_id(
                lxb_dom_interface_element(at), &length);
            if (value != NULL && strlen(id) == length
                && memcmp(value, id, length) == 0) return at;
        }
        lxb_dom_node_t *found = find_id_below(at->first_child, id);
        if (found != NULL) return found;
    }
    return NULL;
}

static lxb_dom_node_t *by_id(const char *id)
{
    return find_id_below(
        lxb_dom_interface_node(navigation.page.document.html), id);
}

static bool close_page(void)
{
    navigation_destroy(&navigation);
    return budget.current == 0;
}

/* An image with no entry yet is examined by itself. */
static bool test_new_source_examines_one_image(void)
{
    CHECK(open_page("", "<img id=late width=8 height=8>", 3000, ""));
    size_t visits = image_visits();
    CHECK(step("document.getElementById('late').src='" GIF "'"));
    CHECK(images_find_node(&navigation.page.images, by_id("late")) != NULL);
    CHECK(image_visits() - visits < LOCAL_VISITS);
    return close_page();
}

/* The refresh still follows an ancestor's change to a revealed image. */
static bool test_revealed_image_is_found(void)
{
    CHECK(open_page(".off{display:none}",
                    "<div id=box class=off><img id=hid src='" GIF "'></div>",
                    2000, ""));
    CHECK(images_find_node(&navigation.page.images, by_id("hid")) == NULL);
    size_t visits = image_visits();
    CHECK(step("document.getElementById('box').className=''"));
    CHECK(images_find_node(&navigation.page.images, by_id("hid")) != NULL);
    CHECK(image_visits() - visits < LOCAL_VISITS);
    return close_page();
}

/* One that failed stays failed until its source changes. */
static bool test_failed_image_waits_for_new_source(void)
{
    CHECK(open_page(".on{display:block}",
                    "<div id=box><img id=bad src='" BROKEN "'></div>",
                    2000, ""));
    lxb_dom_node_t *bad = by_id("bad");
    CHECK(images_find_node(&navigation.page.images, bad) == NULL);
    CHECK(images_node_failed(&navigation.page.images, bad));
    size_t visits = image_visits();
    CHECK(step("document.getElementById('box').className='on'"));
    CHECK(image_visits() == visits);
    CHECK(step("document.getElementById('bad').src='" GIF "'"));
    CHECK(images_find_node(&navigation.page.images, bad) != NULL);
    CHECK(image_visits() - visits < LOCAL_VISITS);
    return close_page();
}

/* Insertions are walked by themselves, CSS images in them included, and a
   hidden parent still hides what is put in it. */
static bool test_inserted_subtrees_are_walked(void)
{
    CHECK(open_page(".art{height:9px;background-image:url('" GIF "')}"
                    "#hide{display:none}",
                    "<div id=host></div><div id=hide></div>", 3000, ""));
    size_t visits = image_visits();
    CHECK(step("(()=>{const d=document.createElement('div');"
               "d.innerHTML='<p id=art class=art></p><img id=put src=\"" GIF
               "\">';document.getElementById('host').append(d);"
               "const h=document.createElement('img');h.id='no';h.src='" GIF
               "';document.getElementById('hide').append(h)})()"));
    CHECK(images_find_background_node(&navigation.page.images, by_id("art"))
          != NULL);
    CHECK(images_find_node(&navigation.page.images, by_id("put")) != NULL);
    CHECK(images_find_node(&navigation.page.images, by_id("no")) == NULL);
    CHECK(image_visits() - visits < LOCAL_VISITS);
    return close_page();
}

/* An insertion also moves what selectors see outside the inserted subtree: the
   siblings around it, and through :has() the elements above it. */
static bool test_insertion_styles_relatives(void)
{
    CHECK(open_page(".a+.b,.a~.far{height:9px;background-image:url('" GIF "')}"
                    "u:not(:last-child){display:block;height:9px;"
                    "background-image:url('" GIF_AT(2) "')}",
                    "<div id=host><p id=b class=b></p><p id=f class=far></p>"
                    "</div><div id=list><u id=u></u></div>", 3000, ""));
    CHECK(images_find_background_node(&navigation.page.images, by_id("b"))
          == NULL);
    size_t visits = image_visits();
    CHECK(step("document.getElementById('b').insertAdjacentElement("
               "'beforebegin',(()=>{const d=document.createElement('div');"
               "d.className='a';d.innerHTML='<img src=\"" GIF_AT(3) "\">';"
               "return d})())"));
    CHECK(images_find_background_node(&navigation.page.images, by_id("b"))
          != NULL);
    CHECK(images_find_background_node(&navigation.page.images, by_id("f"))
          != NULL);
    CHECK(step("document.getElementById('list').append((()=>{"
               "const d=document.createElement('div');"
               "d.innerHTML='<img src=\"" GIF_AT(4) "\">';return d})())"));
    CHECK(images_find_background_node(&navigation.page.images, by_id("u"))
          != NULL);
    CHECK(image_visits() - visits < LOCAL_VISITS);
    CHECK(close_page());
    CHECK(open_page(".w:has(.x){height:9px;background-image:url('" GIF "')}",
                    "<div id=w class=w></div>", 10, ""));
    CHECK(step("document.getElementById('w').append((()=>{"
               "const d=document.createElement('div');d.className='x';"
               "d.innerHTML='<img src=\"" GIF_AT(3) "\">';return d})())"));
    CHECK(images_find_background_node(&navigation.page.images, by_id("w"))
          != NULL);
    return close_page();
}

/* A sheet that arrives can style anything, so that one still walks it all. */
static bool test_new_sheet_still_walks_document(void)
{
    CHECK(open_page("", "<p id=far class=far style='height:9px'></p>", 500,
                    ""));
    CHECK(step("(()=>{const s=document.createElement('style');s.textContent="
               "\".far{background-image:url('" GIF "')}\";"
               "document.head.append(s)})()"));
    CHECK(images_find_background_node(&navigation.page.images, by_id("far"))
          != NULL);
    return close_page();
}

/* Moving or sizing a box above many images leaves the image table alone. A
   style that hid or shows them is not geometry, in either direction. */
static bool test_geometry_write_keeps_table(void)
{
    enum { IMAGES = 300 };
    static char body[64 * 1024];
    size_t used = (size_t) snprintf(body, sizeof(body),
                                    "<div id=top><img id=in src='" GIF "'>");
    for (unsigned i = 0; i < IMAGES; i++)
        used += (size_t) snprintf(body + used, sizeof(body) - used,
            "<svg width=4 height=4><path d='M0 0h4v4H0z'/></svg>");
    snprintf(body + used, sizeof(body) - used, "</div>");
    CHECK(open_page("", body, 10, ""));
    size_t rebuilds = navigation.performance.mutation_image_rebuilds;
    size_t scans = navigation.performance.mutation_image_resource_scans;
    CHECK(step("document.getElementById('top').setAttribute('style',"
               "'height:900px;margin-top:2px;left:0')"));
    CHECK(navigation.performance.mutation_image_rebuilds == rebuilds);
    CHECK(navigation.performance.mutation_image_resource_scans == scans);
    CHECK(step("document.getElementById('top').style.minHeight='901px'"));
    CHECK(navigation.performance.mutation_image_rebuilds == rebuilds);
    CHECK(navigation.performance.mutation_image_resource_scans == scans);
    /* Hide, and show again through a geometry-only value: the old text is
       what makes this one count. */
    CHECK(step("document.getElementById('top').setAttribute('style',"
               "'display:none')"));
    CHECK(step("document.getElementById('in').src='" GIF_AT(2) "'"));
    CHECK(step("document.getElementById('top').setAttribute('style',"
               "'height:900px')"));
    CHECK(navigation.performance.mutation_image_rebuilds
              + navigation.performance.mutation_image_resource_scans
          > rebuilds + scans);
    CHECK(images_find_node(&navigation.page.images, by_id("in")) != NULL);
    return close_page();
}

/* A sheet adopted by one more root restyles that root, not the page, even
   past the old order-walk limit (16384 nodes refused style append). */
static bool test_adoption_keeps_other_styles(void)
{
    static char comments[8 * 12000 + 1];
    for (size_t i = 0; i < 12000; i++) memcpy(comments + i * 8, "<!-- -->", 8);
    CHECK(open_page("", "<x-host id=one></x-host><x-host id=two></x-host>",
                    3000, comments));
    CHECK(navigation.page.document.node_count > 16384u);
    CHECK(step("(()=>{const sheet=new CSSStyleSheet();"
               "sheet.replaceSync('.shared{color:#0000ff}');"
               "globalThis.sharedSheet=sheet;"
               "const r=document.getElementById('one')"
               ".attachShadow({mode:'open'});"
               "r.innerHTML='<p class=shared>one</p>';"
               "r.adoptedStyleSheets=[sheet]})()"));
    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(navigation.page.layout_reuse, &before);
    CHECK(step("(()=>{const r=document.getElementById('two')"
               ".attachShadow({mode:'open'});"
               "r.innerHTML='<p class=shared>two</p>';"
               "r.adoptedStyleSheets=[sharedSheet]})()"));
    layout_reuse_cache_stats(navigation.page.layout_reuse, &after);
    CHECK(after.full_resets == before.full_resets);
    CHECK(after.style_misses - before.style_misses < 1000u);
    return close_page();
}

/* A layout script already forced is adopted when the refresh had nothing to
   change: not built a second time. */
static bool test_forced_layout_adopted_when_table_unchanged(void)
{
    CHECK(open_page("",
                    "<div id=box><img id=bad src='" BROKEN "'></div>", 400, ""));
    LayoutReuseStats before = {0}, after = {0};
    CHECK(evaluate("(()=>{const b=document.getElementById('box');"
                   "b.style.display='block';return b.offsetHeight})()"));
    CHECK(script_runtime_consume_relayout(navigation.page.runtime));
    layout_reuse_cache_stats(navigation.page.layout_reuse, &before);
    CHECK(navigation_relayout(&navigation));
    layout_reuse_cache_stats(navigation.page.layout_reuse, &after);
    CHECK(after.style_hits - before.style_hits < 100u);
    return close_page();
}

/* Text changing in one of many siblings restyles none of them unless the
   sheet has a sibling or position test, which keeps restyling them. */
static bool test_long_sibling_list_keeps_styles(void)
{
    CHECK(open_page("", "<div id=first>a</div>", 1000, ""));
    CHECK(step("document.getElementById('first').textContent='b'"));
    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(navigation.page.layout_reuse, &before);
    CHECK(step("document.getElementById('first').textContent='c'"));
    layout_reuse_cache_stats(navigation.page.layout_reuse, &after);
    CHECK(after.style_misses - before.style_misses < 20u);
    CHECK(close_page());

    CHECK(open_page("div:first-child+p{height:50px}",
                    "<div id=first>a</div><p id=next></p>", 1000, ""));
    CHECK(step("document.getElementById('first').textContent='b'"));
    const LayoutNodeBox *box =
        layout_box_for_node(&navigation.page.layout, by_id("next"));
    CHECK(box != NULL && box->height == 50);
    CHECK(step("document.getElementById('first').before("
               "document.createElement('i'))"));
    box = layout_box_for_node(&navigation.page.layout, by_id("next"));
    CHECK(box == NULL || box->height != 50);
    return close_page();
}

/* A script's first frees reach the layout cache as they happen: the cache
   must not keep a freed node until the first relayout binds anything. */
static bool test_first_free_leaves_layout_cache(void)
{
    /* The sibling test sends the relayout's invalidation through every entry
       the cache holds, which reads each entry's node. */
    CHECK(open_page("p:first-child+p{height:50px}",
                    "<div id=first>a</div><p></p>", 1000, ""));
    CHECK(navigation.page.layout_reuse != NULL);
    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(navigation.page.layout_reuse, &before);
    CHECK(evaluate("document.getElementById('first').textContent='b'"));
    layout_reuse_cache_stats(navigation.page.layout_reuse, &after);
    CHECK(after.retired_subtrees > before.retired_subtrees);
    CHECK(script_runtime_consume_relayout(navigation.page.runtime)
          && navigation_relayout(&navigation)
          && navigation_complete_layout_sync(&navigation));
    return close_page();
}

/* A shell too small for a cache at commit, then filled by script: its first
   frees after the cache appears are reported like any others. */
static bool test_grown_page_free_leaves_layout_cache(void)
{
    CHECK(open_page("", "<div id=first>a</div><div id=box></div>", 0, ""));
    CHECK(navigation.page.layout_reuse == NULL);
    CHECK(step("document.getElementById('box').innerHTML="
               "'<p>row</p>'.repeat(600)"));
    CHECK(navigation.page.layout_reuse != NULL);
    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(navigation.page.layout_reuse, &before);
    CHECK(evaluate("document.getElementById('first').textContent='b'"));
    layout_reuse_cache_stats(navigation.page.layout_reuse, &after);
    CHECK(after.retired_subtrees > before.retired_subtrees);
    return close_page();
}

/* The next runtime turn's relayout, with a source written far below. These
   tests count the wait in `elapsed_ms`, so this turn's real time must not end
   it early or lengthen it: the wait's real-time limit starts over, and it
   scales as if the last relayout were fast. Under a loaded machine, or after a
   shadow root has made this process's relayouts slower, it would otherwise
   outlast the elapsed time these tests give it. */
static bool advance(unsigned elapsed_ms)
{
    navigation.image_hold_relayout_us = 0;
    navigation.image_hold_started_us = tilefinch_platform_monotonic_time_us();
    return navigation_advance_runtime(&navigation, elapsed_ms, 8);
}

static bool test_far_image_changes_gather(void)
{
    /* The filler puts #far well past the viewport; intrinsic sizes keep this
       a change that can move what follows. */
    CHECK(open_page("img{display:block;width:10px;min-height:2px}",
                    "<img id=near width=8 height=8>", 3000,
                    "<img id=far><img id=far2><p id=end>end</p>"));
    lxb_dom_node_t *far = by_id("far"), *far2 = by_id("far2");
    CHECK(layout_box_for_node(&navigation.page.layout, far) != NULL);
    size_t relayouts = navigation.incremental_relayouts;

    /* Nothing else is on its way: nothing to gather with, so no wait. */
    navigation.page.image_continuation_pending = false;
    CHECK(evaluate("document.getElementById('far').src='" GIF "'"));
    CHECK(advance(16));
    CHECK(navigation.incremental_relayouts == relayouts + 1u);
    CHECK(images_find_node(&navigation.page.images, far) != NULL);
    relayouts = navigation.incremental_relayouts;

    /* More is coming: changes gather until the bound. */
    navigation.page.image_continuation_pending = true;
    CHECK(evaluate("document.getElementById('far2').src='" GIF "'"));
    CHECK(advance(400) && advance(400));
    CHECK(navigation.incremental_relayouts == relayouts);
    CHECK(images_find_node(&navigation.page.images, far2) == NULL);
    CHECK(script_runtime_has_pending_mutations(navigation.page.runtime));
    CHECK(evaluate("document.getElementById('far').src='" GIF_AT(2) "'"));
    CHECK(advance(100));
    CHECK(navigation.incremental_relayouts == relayouts);
    CHECK(advance(200));
    CHECK(navigation.incremental_relayouts == relayouts + 1u);
    CHECK(images_find_node(&navigation.page.images, far2) != NULL);
    CHECK(!script_runtime_has_pending_mutations(navigation.page.runtime));
    relayouts = navigation.incremental_relayouts;

    /* Reaching it ends the longer wait at the next turn. */
    CHECK(evaluate("document.getElementById('far2').src='" GIF_AT(3) "'"));
    CHECK(advance(100));
    CHECK(navigation.incremental_relayouts == relayouts);
    navigation_set_scroll(&navigation, navigation.page.layout.height);
    CHECK(advance(200));
    CHECK(navigation.incremental_relayouts == relayouts + 1u);
    relayouts = navigation.incremental_relayouts;
    navigation_set_scroll(&navigation, 0);

    /* Anything but an image source does not wait. */
    CHECK(evaluate("document.getElementById('far').src='" GIF_AT(4) "';"
                   "document.getElementById('end').textContent='changed'"));
    CHECK(advance(16));
    CHECK(navigation.incremental_relayouts == relayouts + 1u);
    relayouts = navigation.incremental_relayouts;

    /* Near the viewport the wait is short. */
    CHECK(evaluate("document.getElementById('near').src='" GIF "'"));
    CHECK(advance(100));
    CHECK(navigation.incremental_relayouts == relayouts);
    CHECK(advance(150));
    CHECK(navigation.incremental_relayouts == relayouts + 1u);
    CHECK(images_find_node(&navigation.page.images, by_id("near")) != NULL);
    relayouts = navigation.incremental_relayouts;

    /* Too many together are laid out rather than walked for. */
    CHECK(evaluate("(()=>{const f=document.getElementById('end');"
                   "for(let i=0;i<20;i++){const m=document.createElement('img');"
                   "m.id='m'+i;f.append(m)}})()"));
    navigation.page.image_continuation_pending = false;
    CHECK(advance(16));
    relayouts = navigation.incremental_relayouts;
    navigation.page.image_continuation_pending = true;
    CHECK(evaluate("for(let i=0;i<20;i++)"
                   "document.getElementById('m'+i).src="
                   "'data:image/gif;v='+i+';base64,"
                   "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw=='"));
    CHECK(advance(16));
    CHECK(navigation.incremental_relayouts == relayouts + 1u);
    navigation.page.image_continuation_pending = false;
    return close_page();
}

/* What the wait must not lose: a box the image changes is laid out when it
   ends, and everything after it moves. */
static bool test_held_image_still_moves_layout(void)
{
    CHECK(open_page("#far{width:10px;min-height:2px;display:block}", "", 3000,
                    "<img id=far><p id=end>end</p>"));
    lxb_dom_node_t *end = by_id("end");
    const LayoutNodeBox *box = layout_box_for_node(&navigation.page.layout, end);
    CHECK(box != NULL);
    int end_y = box->y;
    navigation.page.image_continuation_pending = true;
    CHECK(evaluate("document.getElementById('far').src='" GIF "'"));
    CHECK(advance(500));
    box = layout_box_for_node(&navigation.page.layout, end);
    CHECK(box != NULL && box->y == end_y);
    CHECK(advance(500));
    navigation.page.image_continuation_pending = false;
    box = layout_box_for_node(&navigation.page.layout, end);
    CHECK(box != NULL && box->y > end_y);
    return close_page();
}

/* Past the table, the count and the byte quotas: what the reader reaches is
   still admitted, and what makes way for it is far from the reader. None of
   it walks the document or rebuilds the table. */

#define ICON "<svg style='display:block;width:4px;height:4px'>" \
    "<path d='M0 0h4v4H0z'/></svg>"
/* A 3000 px page of 1 px <b> filler below the icons. */
enum { TABLE_FILLER = 3000 };

/* Many icons sharing one raster, the way a result grid's tiles do. */
static bool open_table_full_page(const char *head, const char *top)
{
    static char body[64 * 1024];
    size_t used = (size_t) snprintf(body, sizeof(body), "%s", top);
    for (unsigned i = 0; i < IMAGE_TRACKED_NODE_LIMIT + 8u; i++)
        used += (size_t) snprintf(body + used, sizeof(body) - used, ICON);
    if (!open_page(head, body, TABLE_FILLER, "<div id=low></div>")) return false;
    return navigation.page.images.count == IMAGE_TRACKED_NODE_LIMIT;
}

/* Idle turns until the background work settles; bounded, so a loop that never
   settles fails the test. */
static bool settle_images(void)
{
    for (unsigned turn = 0; turn < 4000u; turn++) {
        if (!navigation_background_resources_pending(&navigation)) return true;
        if (navigation_run_background_resources(&navigation)
                == NAVIGATION_BACKGROUND_WORK_HARD_FAILURE) return false;
    }
    return false;
}

static const DrawCommand *image_command_for(const lxb_dom_node_t *node)
{
    const LayoutDocument *layout = &navigation.page.layout;
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_IMAGE && command->image != NULL
            && command->image->node == node) return command;
    }
    return NULL;
}

/* Every display command still names the entry of its own element. */
static bool slots_match_table(void)
{
    const LayoutDocument *layout = &navigation.page.layout;
    const ImageResources *images = &navigation.page.images;
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type != DRAW_IMAGE || command->image == NULL) continue;
        if (command->image < images->items
            || command->image >= images->items + images->count) return false;
        if (command->text != NULL
            && command->text != (const char *) command->image->node)
            return false;
    }
    return true;
}

static bool scroll_to_node(lxb_dom_node_t *node)
{
    const LayoutNodeBox *box = layout_box_for_node(&navigation.page.layout, node);
    return box != NULL && navigation_set_scroll(&navigation, box->y)
        && settle_images();
}

#define TILES_SCRIPT(style) \
    "(()=>{const low=document.getElementById('low');" \
    "for(let i=0;i<8;i++){const m=document.createElement('img');m.id='t'+i;" \
    "m.style.cssText='" style "';" \
    "m.src='data:image/gif;v='+i+';base64," \
    "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==';low.append(m);" \
    "const n=document.createElementNS('http://www.w3.org/2000/svg','svg');" \
    "n.id='n'+i;n.setAttribute('style','display:block;width:5px;height:5px');" \
    "n.innerHTML='<path d=\"M0 0h'+(i+1)+'v5H0z\"/>';low.append(n)}})()"

/* The defect: tiles mounted once the table is full of off-screen icons. */
static bool test_viewport_admits_past_full_table(void)
{
    CHECK(open_table_full_page("", ""));
    CHECK(navigation.viewport.css_height > 0);
    size_t rebuilds = navigation.performance.mutation_image_rebuilds;
    CHECK(step(TILES_SCRIPT("display:block;width:8px;height:8px")));
    CHECK(navigation.page.images.count == IMAGE_TRACKED_NODE_LIMIT);
    CHECK(images_find_node(&navigation.page.images, by_id("t0")) == NULL);
    size_t visits = image_visits();
    CHECK(scroll_to_node(by_id("low")));
    static const char *const nodes[] = {"t0", "t1", "t2", "t3", "t4", "t5",
                                        "t6", "t7", "n0", "n7"};
    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        lxb_dom_node_t *node = by_id(nodes[i]);
        CHECK(image_resource_available(
            images_find_node(&navigation.page.images, node)));
        CHECK(image_command_for(node) != NULL);
    }
    CHECK(slots_match_table());
    CHECK(navigation.page.images.count <= IMAGE_TRACKED_NODE_LIMIT);
    CHECK(navigation.performance.viewport_image_evictions != 0);
    CHECK(navigation.performance.mutation_image_rebuilds == rebuilds);
    CHECK(image_visits() - visits < LOCAL_VISITS);
    /* An evicted icon comes back when the reader does. */
    CHECK(navigation_set_scroll(&navigation, 0));
    CHECK(settle_images());
    lxb_dom_node_t *icon = document_body_node(&navigation.page.document)
        ->first_child;
    while (icon != NULL && icon->local_name != LXB_TAG_SVG) icon = icon->next;
    CHECK(icon != NULL);
    CHECK(image_resource_available(
        images_find_node(&navigation.page.images, icon)));
    CHECK(image_command_for(icon) != NULL);
    CHECK(slots_match_table());
    CHECK(navigation.page.images.count <= IMAGE_TRACKED_NODE_LIMIT);
    return close_page();
}

/* A static page has no mutation to wake anything: scrolling alone admits, past
   the image count, and the count still limits what the page starts by itself. */
static bool test_scroll_admits_past_count_on_static_page(void)
{
    enum { IMAGES = 24, PADDING = 4200, COUNT = 4 };
    static char body[256 * 1024];
    static char padding[PADDING + 1];
    memset(padding, 'x', PADDING);
    size_t used = 0;
    for (unsigned i = 0; i < IMAGES; i++)
        used += (size_t) snprintf(body + used, sizeof(body) - used,
            "<img id=i%u style='display:block;width:8px;height:8px;"
            "margin-bottom:400px' src='data:image/gif;v=%u;p=%s;base64,"
            "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw=='>",
            i, i, padding);
    CHECK(navigation_init(&navigation, &budget, 2));
    navigation_enable_external_resources(
        &navigation, 2, 32 * 1024, 32 * 1024,
        COUNT, 256 * 1024, 32 * 1024, 64 * 1024, 1000);
    size_t length = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:0}</style><body>%s", body);
    uint64_t generation = navigation_begin(&navigation);
    CHECK(navigation_commit_static_html(
        &navigation, generation, "https://img-relayout.test/", html, length,
        480, NULL, NULL, true));
    CHECK(navigation_complete_layout_sync(&navigation));
    CHECK(navigation.page.runtime == NULL);
    CHECK(settle_images());
    char id[8];
    snprintf(id, sizeof(id), "i%u", IMAGES - 1u);
    lxb_dom_node_t *last = by_id(id);
    CHECK(!image_resource_available(
        images_find_node(&navigation.page.images, last)));
    CHECK(scroll_to_node(last));
    CHECK(image_resource_available(
        images_find_node(&navigation.page.images, last)));
    CHECK(image_command_for(last) != NULL);
    CHECK(slots_match_table());
    return close_page();
}

/* Images whose box does not prove itself fixed each need a flow to appear, and
   those around one position share it. */
static bool test_viewport_batch_shares_one_flow(void)
{
    CHECK(open_table_full_page(
        ".tile{height:20px}.tile img{width:100%;height:100%}", ""));
    CHECK(step("(()=>{const low=document.getElementById('low');"
               "for(let i=0;i<8;i++){const d=document.createElement('div');"
               "d.className='tile';const m=document.createElement('img');"
               "m.id='t'+i;m.src='data:image/gif;v='+i+';base64,"
               "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==';"
               "d.append(m);low.append(d)}})()"));
    size_t relayouts = navigation.performance.fast_relayouts;
    CHECK(scroll_to_node(by_id("low")));
    CHECK(navigation.performance.fast_relayouts - relayouts <= 2u);
    for (int i = 0; i < 8; i++) {
        char id[8];
        snprintf(id, sizeof(id), "t%d", i);
        CHECK(image_command_for(by_id(id)) != NULL);
    }
    return close_page();
}

/* Evicting cannot move the page or blank what paints at any scroll: an image
   sized by its resource stays, and so does a fixed layer's. */
static bool test_eviction_keeps_page_geometry(void)
{
    CHECK(open_table_full_page(
        "#keep{display:block;width:30px}"
        "#pin{position:fixed;top:0;left:0;width:6px;height:6px}",
        "<img id=keep src='" GIF "'><svg id=pin><path d='M0 0h6v6H0z'/></svg>"));
    lxb_dom_node_t *keep = by_id("keep"), *pin = by_id("pin");
    CHECK(images_find_node(&navigation.page.images, keep) != NULL);
    CHECK(images_find_node(&navigation.page.images, pin) != NULL);
    int height = navigation.page.layout.height;
    CHECK(step(TILES_SCRIPT("display:block;width:8px;height:8px")));
    int grown = navigation.page.layout.height;
    CHECK(grown > height);
    CHECK(scroll_to_node(by_id("low")));
    CHECK(navigation.performance.viewport_image_evictions != 0);
    CHECK(images_find_node(&navigation.page.images, keep) != NULL);
    CHECK(images_find_node(&navigation.page.images, pin) != NULL);
    CHECK(step("document.getElementById('low').setAttribute('title','x')"));
    CHECK(navigation_relayout(&navigation));
    CHECK(navigation_complete_layout_sync(&navigation));
    CHECK(navigation.page.layout.height == grown);
    CHECK(slots_match_table());
    return close_page();
}

/* The decoded quota gives way the same way, and returns its bytes. */
static bool test_viewport_admits_past_byte_quota(void)
{
    enum { QUOTA = 100 * 1024 };
    static const char big[] =
        "<svg id=%s width=100 height=100 style='display:block;width:100px;"
        "height:100px'><path d='M0 0h%uv100H0z'/></svg>";
    static char top[1024], tail[512];
    size_t used = 0;
    for (unsigned i = 0; i < 3u; i++) {
        char id[8];
        snprintf(id, sizeof(id), "b%u", i);
        used += (size_t) snprintf(top + used, sizeof(top) - used, big, id, 50u + i);
    }
    snprintf(tail, sizeof(tail), big, "last", 90u);
    size_t length = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:0}b{display:block;height:1px}</style>"
        "<body>%s", top);
    for (unsigned i = 0; i < TABLE_FILLER; i++)
        length += (size_t) snprintf(html + length, sizeof(html) - length,
                                    "<b></b> ");
    length += (size_t) snprintf(html + length, sizeof(html) - length, "%s", tail);
    CHECK(navigation_init(&navigation, &budget, 2));
    navigation_enable_scripts(&navigation, 3 * MIB, 1000);
    navigation_enable_external_resources(
        &navigation, 2, 32 * 1024, 32 * 1024, 64, 64 * 1024, 32 * 1024, QUOTA,
        1000);
    uint64_t generation = navigation_begin(&navigation);
    CHECK(navigation_commit_html(
        &navigation, generation, "https://img-relayout.test/", html, length,
        480, NULL, NULL, true));
    CHECK(navigation_complete_layout_sync(&navigation));
    CHECK(settle_images());
    lxb_dom_node_t *last = by_id("last");
    CHECK(images_find_node(&navigation.page.images, last) == NULL);
    CHECK(scroll_to_node(last));
    CHECK(image_command_for(last) != NULL);
    CHECK(navigation.performance.viewport_image_evictions != 0);
    CHECK(images_find_node(&navigation.page.images, by_id("b0")) == NULL);
    CHECK(slots_match_table());
    return close_page();
}

/* An <img> of 100 x 100 CSS pixels whose raster costs 40 KB of the quota. */
#define BIG_IMG_SRC(fill) \
    "data:image/svg+xml,%%3Csvg xmlns='http://www.w3.org/2000/svg' width='100' " \
    "height='100'%%3E%%3Cpath d='M0 0h" fill "v100H0z'/%%3E%%3C/svg%%3E"

/* Three <img> filling the decoded quota, counted by a load listener, and a
   picture far below that makes the farthest make way. */
static bool open_evictable_image_page(void)
{
    enum { QUOTA = 100 * 1024 };
    size_t length = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:0}b{display:block;height:1px}"
        "img{display:block;width:100px;height:100px}</style><body>"
        "<script>globalThis.loads={};globalThis.note=e=>{"
        "loads[e.target.id]=(loads[e.target.id]||0)+1};"
        "document.addEventListener('load',note,true)</script>");
    for (unsigned i = 0; i < 3u; i++)
        length += (size_t) snprintf(html + length, sizeof(html) - length,
            i == 0u ? "<img id=b0 src=\"" BIG_IMG_SRC("50") "\">"
            : i == 1u ? "<img id=b1 src=\"" BIG_IMG_SRC("51") "\">"
                      : "<img id=b2 src=\"" BIG_IMG_SRC("52") "\">");
    for (unsigned i = 0; i < TABLE_FILLER; i++)
        length += (size_t) snprintf(html + length, sizeof(html) - length,
                                    "<b></b> ");
    /* Each takes a smaller raster of what remains, until none fits. */
    for (unsigned i = 0; i < 6u; i++)
        length += (size_t) snprintf(html + length, sizeof(html) - length,
            "<svg width=100 height=100 style='display:block;width:100px;"
            "height:100px'><path d='M0 0h%uv100H0z'/></svg>", 70u + i);
    length += (size_t) snprintf(html + length, sizeof(html) - length,
        "<svg id=last width=100 height=100 style='display:block;width:100px;"
        "height:100px'><path d='M0 0h90v100H0z'/></svg>");
    if (!navigation_init(&navigation, &budget, 2)) return false;
    navigation_enable_scripts(&navigation, 3 * MIB, 1000);
    navigation_enable_external_resources(
        &navigation, 2, 32 * 1024, 32 * 1024, 64, 64 * 1024, 32 * 1024, QUOTA,
        1000);
    uint64_t generation = navigation_begin(&navigation);
    return navigation_commit_html(
               &navigation, generation, "https://img-relayout.test/", html,
               length, 480, NULL, NULL, true)
        && navigation_complete_layout_sync(&navigation)
        && settle_images() && advance(16) && advance(16);
}

/* Pairs of <img> with one source share a surface, and fill the decoded quota
   between them. Neither alias frees anything alone, so eviction has to see the
   pair: the picture far below paints, and the quota has its bytes back. */
static bool test_viewport_evicts_shared_surfaces(void)
{
    enum { QUOTA = 100 * 1024 };
    size_t length = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:0}b{display:block;height:1px}"
        "img{display:block;width:100px;height:100px}</style><body>");
    for (unsigned i = 0; i < 8u; i++)
        length += (size_t) snprintf(html + length, sizeof(html) - length,
            "<img id=a%u src=\"" BIG_IMG_SRC("%u") "\">"
            "<img id=c%u src=\"" BIG_IMG_SRC("%u") "\">",
            i, 50u + i, i, 50u + i);
    for (unsigned i = 0; i < TABLE_FILLER; i++)
        length += (size_t) snprintf(html + length, sizeof(html) - length,
                                    "<b></b> ");
    length += (size_t) snprintf(html + length, sizeof(html) - length, "<i id=end></i>");
    CHECK(navigation_init(&navigation, &budget, 2));
    navigation_enable_scripts(&navigation, 3 * MIB, 1000);
    navigation_enable_external_resources(
        &navigation, 2, 32 * 1024, 32 * 1024, 64, 64 * 1024, 32 * 1024, QUOTA,
        1000);
    uint64_t generation = navigation_begin(&navigation);
    CHECK(navigation_commit_html(
        &navigation, generation, "https://img-relayout.test/", html, length,
        480, NULL, NULL, true));
    CHECK(navigation_complete_layout_sync(&navigation));
    CHECK(settle_images());
    ImageResources *images = &navigation.page.images;
    const ImageResource *first = images_find_node(images, by_id("a0"));
    const ImageResource *second = images_find_node(images, by_id("c0"));
    CHECK(image_resource_available(first) && image_resource_available(second));
    CHECK(first->pixels == second->pixels && first->owns_pixels != second->owns_pixels);
    /* A picture the page adds once the quota is spent. */
    CHECK(step("document.getElementById('end').insertAdjacentHTML('afterend',"
               "'<svg id=last width=100 height=100 style=\"display:block;"
               "width:100px;height:100px\"><path d=\"M0 0h90v100H0z\"/></svg>')"));
    CHECK(settle_images());
    lxb_dom_node_t *last = by_id("last");
    CHECK(scroll_to_node(last));
    CHECK(images_find_node(images, last) != NULL);
    CHECK(navigation.performance.viewport_image_evictions != 0);
    CHECK(images_find_node(images, by_id("a0")) == NULL);
    CHECK(images_find_node(images, by_id("c0")) == NULL);
    /* Its full raster fits only because the pair gave its bytes back. */
    CHECK(images->stats.decoded_bytes <= QUOTA);
    CHECK(image_command_for(last) != NULL);
    CHECK(image_resource_available(images_find_node(images, last)));
    CHECK(images_find_node(images, last)->width == 100);
    CHECK(slots_match_table());
    /* Back at the top, the pair returns. */
    CHECK(navigation_set_scroll(&navigation, 0));
    CHECK(settle_images());
    CHECK(image_resource_available(images_find_node(images, by_id("a0"))));
    CHECK(image_resource_available(images_find_node(images, by_id("c0"))));
    CHECK(slots_match_table());
    return close_page();
}

/* Whether a JavaScript expression holds. */
static bool script_holds(const char *expression)
{
    char source[512];
    snprintf(source, sizeof(source), "if(!(%s))throw new Error('no')", expression);
    return evaluate(source);
}

/* An inline <svg> is only rasterized once it has a table entry, and a frame-
   work builds the whole subtree before connecting its root: the one record
   names a wrapper. Mastodon's banner icon arrives like this. */
static bool test_inserted_wrapper_finds_inline_svg(void)
{
    CHECK(open_page("", "<div id=host></div>", 2000, ""));
    size_t rebuilds = navigation.performance.mutation_image_rebuilds;
    size_t visits = image_visits();
    CHECK(step("(()=>{const w=document.createElement('div');"
               "w.innerHTML='<p><svg id=parsed width=4 height=4 "
               "style=\"display:block\"><path d=\"M0 0h4v4H0z\"/></svg></p>';"
               "const n=document.createElementNS("
               "'http://www.w3.org/2000/svg','svg');n.id='built';"
               "n.setAttribute('style','display:block;width:4px;height:4px');"
               "n.innerHTML='<path d=\"M0 0h3v4H0z\"/>';"
               "const b=document.createElement('button');b.append(n);w.append(b);"
               "document.getElementById('host').append(w)})()"));
    CHECK(images_find_node(&navigation.page.images, by_id("parsed")) != NULL);
    CHECK(images_find_node(&navigation.page.images, by_id("built")) != NULL);
    /* Found in the wrapper, not by walking the document or rebuilding. */
    CHECK(navigation.performance.mutation_image_rebuilds == rebuilds);
    CHECK(image_visits() - visits < 50);
    return close_page();
}

/* To the page, an image that made way for others never stopped being loaded:
   it keeps its natural size and `complete`, and its return is not a new load. */
static bool test_evicted_image_stays_loaded_to_script(void)
{
    CHECK(open_evictable_image_page());
    lxb_dom_node_t *far = by_id("b0");
    CHECK(image_resource_available(images_find_node(&navigation.page.images, far)));
    CHECK(script_holds("loads.b0===1"));
    CHECK(script_holds("(()=>{const m=document.getElementById('b0');"
                       "return m.complete&&m.naturalWidth===100"
                       "&&m.naturalHeight===100})()"));
    CHECK(scroll_to_node(by_id("last")));
    CHECK(navigation.performance.viewport_image_evictions != 0);
    CHECK(images_find_node(&navigation.page.images, far) == NULL);
    /* Whatever the eviction's publication queued has run. */
    CHECK(advance(16) && advance(16));
    CHECK(script_holds("(()=>{const m=document.getElementById('b0');"
                       "return m.complete&&m.naturalWidth===100"
                       "&&m.naturalHeight===100})()"));
    CHECK(script_holds("loads.b0===1"));
    /* A relayout republishes the table while it is away. */
    CHECK(step("document.body.append(document.createElement('i'))"));
    CHECK(images_find_node(&navigation.page.images, far) == NULL);
    CHECK(navigation_set_scroll(&navigation, 0));
    CHECK(settle_images());
    CHECK(image_resource_available(images_find_node(&navigation.page.images, far)));
    CHECK(advance(16) && advance(16));
    CHECK(script_holds("loads.b0===1"));
    CHECK(script_holds("document.getElementById('b0').naturalWidth===100"));
    return close_page();
}

/* The memory covers one source. A new one is a new load, seen when it arrives,
   and until then the old size is not reported. */
static bool test_evicted_image_new_source_loads_again(void)
{
    CHECK(open_evictable_image_page());
    lxb_dom_node_t *far = by_id("b0");
    CHECK(scroll_to_node(by_id("last")));
    CHECK(images_find_node(&navigation.page.images, far) == NULL);
    CHECK(advance(16) && advance(16));
    CHECK(script_holds("document.getElementById('b0').naturalWidth===100"));
    CHECK(evaluate("document.getElementById('b0').setAttribute('src',"
                   "\"data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///"
                   "ywAAAAAAQABAAACAUwAOw==\")"));
    CHECK(script_holds("(()=>{const m=document.getElementById('b0');"
                       "return !m.complete&&m.naturalWidth===0})()"));
    CHECK(script_runtime_consume_relayout(navigation.page.runtime));
    CHECK(navigation_relayout(&navigation));
    CHECK(navigation_complete_layout_sync(&navigation));
    CHECK(navigation_set_scroll(&navigation, 0));
    CHECK(settle_images());
    CHECK(advance(16) && advance(16));
    CHECK(script_holds("(()=>{const m=document.getElementById('b0');"
                       "return loads.b0===2&&m.complete&&m.naturalWidth===1})()"));
    return close_page();
}

/* A change can reveal an image outside everything the journal names, through
   a selector or an ancestor: nothing but the next layout knows. These resolve
   in idle turns from what that layout found, with no document walk. */
#define REVEAL_SVG(id, style) "<svg id=" id " class='" style "' " \
    "style='width:4px;height:4px'><path d='M0 0h4v4H0z'/></svg>"
#define REVEAL_RULES \
    ".on2+.b2,.c7>i:first-child,.c11:empty,.on12+.b12{display:block;height:9px}" \
    ".on2+.b2{background-image:url('" GIF_AT(2) "')}" \
    ".c7>i:first-child{background-image:url('" GIF_AT(7) "')}" \
    ".c11:empty{background-image:url('" GIF_AT(11) "')}" \
    ".on12+.b12{background-image:url('" GIF_AT(12) "')}" \
    ".x9+.y9{display:none}.t10,.t13{display:none}" \
    "[data-on] .t10{display:block}[data-on] .t13{display:block}"

static bool test_revealed_later_images_resolve(void)
{
    CHECK(open_page(REVEAL_RULES,
        "<i id=a2></i><p id=b2 class=b2></p>"
        "<div id=c7 class=c7><b id=f7></b><i id=n7></i></div>"
        "<i id=x9 class=x9></i>" REVEAL_SVG("s9", "y9")
        "<div id=d10>" REVEAL_SVG("s10", "t10") "</div>"
        "<div id=c11 class=c11><span id=e11>t</span></div>"
        "<div id=g5 style='height:0;overflow:hidden'>"
        "<img id=i5 src=\"" GIF_AT(5) "\"></div>"
        /* Holds the deferred queue open, waiting for a box it may never get. */
        "<img src=\"" GIF_AT(6) "\" style='display:none'>"
        "<div id=d14><img id=i14 class=t10 src=\"" GIF_AT(14) "\"></div>",
        3000,
        "<i id=a12></i><p id=b12 class=b12></p>"
        "<div id=d13>" REVEAL_SVG("s13", "t13") "</div>"));
    CHECK(settle_images());
    size_t visits = image_visits();
    const ImageResources *images = &navigation.page.images;
    CHECK(images_find_background_node(images, by_id("b2")) == NULL);
    CHECK(step("document.getElementById('a2').classList.add('on2')"));
    CHECK(settle_images());
    CHECK(image_resource_available(
        images_find_background_node(images, by_id("b2"))));
    CHECK(image_command_for(by_id("b2")) != NULL);
    CHECK(step("document.getElementById('f7').remove()"));
    CHECK(settle_images());
    CHECK(image_resource_available(
        images_find_background_node(images, by_id("n7"))));
    CHECK(step("document.getElementById('e11').remove()"));
    CHECK(settle_images());
    CHECK(image_resource_available(
        images_find_background_node(images, by_id("c11"))));
    CHECK(images_find_node(images, by_id("s9")) == NULL);
    CHECK(step("document.getElementById('x9').remove()"));
    CHECK(settle_images());
    CHECK(image_resource_available(images_find_node(images, by_id("s9"))));
    CHECK(images_find_node(images, by_id("s10")) == NULL);
    CHECK(step("document.getElementById('d10').dataset.on=1"));
    CHECK(settle_images());
    CHECK(image_resource_available(images_find_node(images, by_id("s10"))));
    CHECK(step("document.getElementById('g5').style.height='40px'"));
    CHECK(settle_images());
    CHECK(image_resource_available(images_find_node(images, by_id("i5"))));
    CHECK(step("document.getElementById('d14').dataset.on=1"));
    CHECK(settle_images());
    CHECK(image_resource_available(images_find_node(images, by_id("i14"))));
    /* Beyond the band: nothing yet, but the reader finds it when arriving. */
    CHECK(step("document.getElementById('a12').classList.add('on12');"
               "document.getElementById('d13').dataset.on=1"));
    CHECK(settle_images());
    CHECK(images_find_background_node(images, by_id("b12")) == NULL);
    CHECK(images_find_node(images, by_id("s13")) == NULL);
    CHECK(image_visits() - visits < LOCAL_VISITS);
    CHECK(scroll_to_node(by_id("b12")));
    CHECK(image_resource_available(
        images_find_background_node(images, by_id("b12"))));
    CHECK(image_resource_available(images_find_node(images, by_id("s13"))));
    CHECK(image_visits() - visits < LOCAL_VISITS);
    return close_page();
}

/* One look per change that could reveal something, and none for one that
   cannot. A full table admits nothing, however often the page asks. */
static bool test_reveal_look_stops_when_nothing_changed(void)
{
    CHECK(open_page(REVEAL_RULES,
        "<i id=a2></i><p id=b2 class=b2></p><p id=t>text</p>", 300, ""));
    CHECK(settle_images());
    size_t looks = navigation.performance.viewport_image_looks;
    /* Neither of these can show anything new. */
    CHECK(step("document.getElementById('t').firstChild.data='other';"
               "document.getElementById('t').setAttribute('aria-label','x')"));
    CHECK(settle_images());
    CHECK(navigation.performance.viewport_image_looks == looks);
    /* Nor can a class no rule mentions. */
    CHECK(step("document.getElementById('t').classList.add('unused')"));
    CHECK(settle_images());
    CHECK(navigation.performance.viewport_image_looks == looks);
    CHECK(step("document.getElementById('a2').classList.add('on2')"));
    CHECK(settle_images());
    CHECK(navigation.performance.viewport_image_looks == looks + 1u);
    for (unsigned turn = 0; turn < 8u; turn++) {
        CHECK(!navigation_background_resources_pending(&navigation));
        CHECK(advance(16));
    }
    CHECK(navigation.performance.viewport_image_looks == looks + 1u);
    CHECK(close_page());
    CHECK(open_table_full_page(REVEAL_RULES,
        "<i id=a2></i><p id=b2 class=b2></p>"));
    CHECK(settle_images());
    looks = navigation.performance.viewport_image_looks;
    size_t evictions = navigation.performance.viewport_image_evictions;
    /* This one can, once. */
    CHECK(step("document.getElementById('a2').classList.add('on2')"));
    CHECK(settle_images());
    size_t looked = navigation.performance.viewport_image_looks;
    CHECK(looked == looks + 1u);
    CHECK(navigation.page.images.count <= IMAGE_TRACKED_NODE_LIMIT);
    CHECK(slots_match_table());
    for (unsigned turn = 0; turn < 8u; turn++) {
        CHECK(!navigation_background_resources_pending(&navigation));
        CHECK(advance(16));
    }
    CHECK(navigation.performance.viewport_image_looks == looked);
    CHECK(navigation.performance.viewport_image_evictions - evictions
          <= 8u);
    return close_page();
}

/* What eviction gave up stays given up: a look only reaches around the reader,
   so a change there brings nothing far away back to churn the table. */
static bool test_reveal_look_leaves_evicted_images(void)
{
    CHECK(open_evictable_image_page());
    CHECK(scroll_to_node(by_id("last")));
    CHECK(images_find_node(&navigation.page.images, by_id("b0")) == NULL);
    CHECK(script_holds("loads.b0===1"));
    size_t evictions = navigation.performance.viewport_image_evictions;
    size_t looks = navigation.performance.viewport_image_looks;
    CHECK(step("document.getElementById('last').style.height='90px'"));
    CHECK(settle_images());
    CHECK(navigation.performance.viewport_image_looks == looks + 1u);
    CHECK(images_find_node(&navigation.page.images, by_id("b0")) == NULL);
    CHECK(navigation.performance.viewport_image_evictions == evictions);
    CHECK(script_holds("loads.b0===1"));
    return close_page();
}

/* The deferred image rank orders at most this many targets. */
enum { RANKED_IMAGE_LIMIT = 128 };

/* Idle turns while the queue works; bounded and not required to settle. */
static bool run_turns(unsigned turns)
{
    for (unsigned turn = 0; turn < turns; turn++) {
        if (!navigation_background_resources_pending(&navigation)) break;
        if (navigation_run_background_resources(&navigation)
                == NAVIGATION_BACKGROUND_WORK_HARD_FAILURE) return false;
        const NavigationPage *page = &navigation.page;
        if (page->deferred_image_count - page->deferred_image_cursor
                > RANKED_IMAGE_LIMIT) return false;
    }
    return true;
}

static bool queue_holds(const lxb_dom_node_t *node)
{
    const NavigationPage *page = &navigation.page;
    for (size_t i = page->deferred_image_cursor;
         i < page->deferred_image_count; i++)
        if (page->deferred_image_targets[i].node == node) return true;
    return false;
}

/* Infinite scroll with a full table: each look queues a batch of lazy images
   just below the reader, where they rest, so the resting queue grows by a
   batch a look. It once outgrew the rank's stack table, which overran it. */
static bool test_resting_queue_stays_within_rank(void)
{
    CHECK(open_table_full_page("img{display:block}",
        "<div style='height:600px'></div><div id=host></div>"));
    enum { ROUNDS = 20 };
    for (unsigned round = 0; round < ROUNDS; round++) {
        char script[512];
        snprintf(script, sizeof(script),
            "(()=>{const h=document.getElementById('host');"
            "for(let i=15;i>=0;i--){const m=document.createElement('img');"
            "m.id='r%ui'+i;m.loading='lazy';m.width=8;m.height=8;"
            "m.src='data:image/gif;v=%u_'+i+';base64,"
            "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==';h.prepend(m)}"
            "})()", round, round);
        CHECK(step(script));
        CHECK(run_turns(8));
    }
    const NavigationPage *page = &navigation.page;
    CHECK(page->deferred_image_count - page->deferred_image_cursor
          == RANKED_IMAGE_LIMIT);
    /* The nearest batch waits in the queue (or has its entry already). */
    char id[16];
    snprintf(id, sizeof(id), "r%ui0", ROUNDS - 1u);
    lxb_dom_node_t *nearest = by_id(id);
    CHECK(nearest != NULL);
    CHECK(queue_holds(nearest)
          || images_find_node(&navigation.page.images, nearest) != NULL);
    /* The farthest gave way, and the reader still finds it on arriving. */
    lxb_dom_node_t *farthest = by_id("r0i0");
    CHECK(farthest != NULL && !queue_holds(farthest));
    CHECK(images_find_node(&navigation.page.images, farthest) == NULL);
    const LayoutNodeBox *box = layout_box_for_node(&navigation.page.layout,
                                                   farthest);
    CHECK(box != NULL && navigation_set_scroll(&navigation, box->y));
    for (unsigned turn = 0; turn < 400u; turn++) {
        if (image_resource_available(
                images_find_node(&navigation.page.images, farthest))) break;
        CHECK(run_turns(1));
    }
    CHECK(image_resource_available(
        images_find_node(&navigation.page.images, farthest)));
    CHECK(slots_match_table());
    return close_page();
}

/* More lazy images just below the reader than a look's batch: the nearest
   batch rests in the queue, and every later look finds that same batch and
   nothing else. Each one walked every layout box and re-ranked the queue, on
   every idle turn, while the page stayed open. What the reader scrolls to
   is still found. */
static bool test_resting_queue_look_settles(void)
{
    CHECK(open_table_full_page("img{display:block}",
        "<div style='height:600px'></div><div id=host></div>"));
    CHECK(step("(()=>{const h=document.getElementById('host');"
               "for(let i=39;i>=0;i--){const m=document.createElement('img');"
               "m.id='z'+i;m.loading='lazy';m.width=8;m.height=8;"
               "m.src='data:image/gif;v=z'+i+';base64,"
               "R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==';h.prepend(m)}"
               "})()"));
    size_t looks = navigation.performance.viewport_image_looks;
    for (unsigned turn = 0; turn < 64u; turn++)
        CHECK(navigation_run_background_resources(&navigation)
              != NAVIGATION_BACKGROUND_WORK_HARD_FAILURE);
    size_t idle_looks = navigation.performance.viewport_image_looks - looks;
    fprintf(stderr, "resting queue: %zu looks in 64 idle turns, pending %d\n",
            idle_looks, navigation_background_resources_pending(&navigation));
    CHECK(idle_looks <= 2u);
    CHECK(!navigation_background_resources_pending(&navigation));
    /* The farthest is past the first batch; the reader reaching it loads it. */
    lxb_dom_node_t *farthest = by_id("z39");
    CHECK(farthest != NULL && !queue_holds(farthest));
    const LayoutNodeBox *box = layout_box_for_node(&navigation.page.layout,
                                                   farthest);
    CHECK(box != NULL && navigation_set_scroll(&navigation, box->y));
    for (unsigned turn = 0; turn < 400u; turn++) {
        if (image_resource_available(
                images_find_node(&navigation.page.images, farthest))) break;
        CHECK(run_turns(1));
    }
    CHECK(image_resource_available(
        images_find_node(&navigation.page.images, farthest)));
    CHECK(slots_match_table());
    return close_page();
}

/* The slot a page gets after each presentation (the PSP loop's deferred-image
   pump, browser_engine_run_deferred_image_work). A page whose canvas commits
   a picture every frame stays dirty, so it never has a page-idle turn
   (navigation_run_background_resources): this slot is all it gets. */
static bool run_painting_frames(unsigned frames)
{
    for (unsigned frame = 0; frame < frames; frame++)
        if (!navigation_run_deferred_image_work(&navigation)) return false;
    return true;
}

/* A game starts: one change that could reveal an image (here a class a
   display rule depends on), then a canvas that paints every frame and no
   other change. The look that change asked for once waited for a page-idle
   turn that never came, so the page never settled: nothing that waits for a
   settled page ran for the whole match (the update check, retargeting,
   autofocus, a video preview, an input script's next step). */
static bool test_painting_page_settles_without_idle_turns(void)
{
    CHECK(open_page(".hud{display:none}.on .hud{display:block}",
        "<div id=shell><p class=hud>score</p></div><canvas id=game></canvas>",
        300, ""));
    CHECK(settle_images());
    size_t looks = navigation.performance.viewport_image_looks;
    CHECK(step("document.getElementById('shell').classList.add('on')"));
    CHECK(run_painting_frames(4));
    size_t painted_looks = navigation.performance.viewport_image_looks - looks;
    bool pending = navigation_background_resources_pending(&navigation);
    if (pending || painted_looks != 1u)
        fprintf(stderr, "painting page: %zu looks in 4 frames, pending %d\n",
                painted_looks, pending);
    CHECK(!pending);
    /* One look for the one change, not one a frame. */
    CHECK(painted_looks == 1u);
    CHECK(run_painting_frames(60));
    CHECK(navigation.performance.viewport_image_looks - looks == 1u);
    CHECK(!navigation_background_resources_pending(&navigation));
    return close_page();
}

/* What such a change does reveal also arrives without a page-idle turn: a
   background, an <img> and an inline <svg>, each drawn, and the page then
   settles. */
static bool test_painting_page_loads_revealed_images(void)
{
    CHECK(open_page(REVEAL_RULES,
        "<i id=a2></i><p id=b2 class=b2></p>"
        "<i id=x9 class=x9></i>" REVEAL_SVG("s9", "y9")
        "<div id=d14><img id=i14 class=t10 src=\"" GIF_AT(14) "\"></div>"
        "<canvas id=game></canvas>",
        300, ""));
    CHECK(settle_images());
    const ImageResources *images = &navigation.page.images;
    CHECK(images_find_background_node(images, by_id("b2")) == NULL);
    CHECK(images_find_node(images, by_id("s9")) == NULL);
    CHECK(images_find_node(images, by_id("i14")) == NULL);
    CHECK(step("document.getElementById('a2').classList.add('on2');"
               "document.getElementById('x9').remove();"
               "document.getElementById('d14').dataset.on=1"));
    unsigned frames = 0;
    while (frames < 64u && navigation_background_resources_pending(&navigation)) {
        CHECK(run_painting_frames(1));
        frames++;
    }
    bool b2 = image_resource_available(
        images_find_background_node(images, by_id("b2")));
    bool s9 = image_resource_available(images_find_node(images, by_id("s9")));
    bool i14 = image_resource_available(
        images_find_node(images, by_id("i14")));
    bool pending = navigation_background_resources_pending(&navigation);
    if (!b2 || !s9 || !i14 || pending)
        fprintf(stderr, "painting page reveal: %u frames background=%d svg=%d "
                "img=%d pending %d\n", frames, b2, s9, i14, pending);
    CHECK(b2 && s9 && i14 && !pending);
    CHECK(image_command_for(by_id("b2")) != NULL);
    CHECK(image_command_for(by_id("i14")) != NULL);
    CHECK(slots_match_table());
    return close_page();
}

int main(int argc, char **argv)
{
    budget_init(&budget, 48 * MIB);
    if (!budget_install_lexbor(&budget)) return 1;
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"new source", test_new_source_examines_one_image},
        {"reveal", test_revealed_image_is_found},
        {"failed image", test_failed_image_waits_for_new_source},
        {"inserted subtrees", test_inserted_subtrees_are_walked},
        {"inserted relatives", test_insertion_styles_relatives},
        {"revealed later images", test_revealed_later_images_resolve},
        {"reveal look stops", test_reveal_look_stops_when_nothing_changed},
        {"reveal look and eviction", test_reveal_look_leaves_evicted_images},
        {"new sheet", test_new_sheet_still_walks_document},
        {"geometry write", test_geometry_write_keeps_table},
        {"adoption", test_adoption_keeps_other_styles},
        {"forced layout", test_forced_layout_adopted_when_table_unchanged},
        {"sibling list", test_long_sibling_list_keeps_styles},
        {"first free", test_first_free_leaves_layout_cache},
        {"grown page free", test_grown_page_free_leaves_layout_cache},
        {"gathering", test_far_image_changes_gather},
        {"held box", test_held_image_still_moves_layout},
        {"full table", test_viewport_admits_past_full_table},
        {"resting queue rank", test_resting_queue_stays_within_rank},
        {"resting queue look settles", test_resting_queue_look_settles},
        {"painting page settles", test_painting_page_settles_without_idle_turns},
        {"painting page reveal", test_painting_page_loads_revealed_images},
        {"static count", test_scroll_admits_past_count_on_static_page},
        {"shared flow", test_viewport_batch_shares_one_flow},
        {"eviction geometry", test_eviction_keeps_page_geometry},
        {"byte quota", test_viewport_admits_past_byte_quota},
        {"shared surfaces under byte pressure", test_viewport_evicts_shared_surfaces},
        {"inserted wrapper svg", test_inserted_wrapper_finds_inline_svg},
        {"evicted stays loaded", test_evicted_image_stays_loaded_to_script},
        {"evicted new source", test_evicted_image_new_source_loads_again},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (argc > 1 && strcmp(argv[1], tests[i].name) != 0) continue;
        bool ok = tests[i].run();
        printf("%s: %s\n", tests[i].name, ok ? "ok" : "FAILED");
        if (!ok) failed = 1;
    }
    puts(failed ? "img-relayout-tests status=FAIL"
                : "img-relayout-tests status=PASS");
    return failed;
}
