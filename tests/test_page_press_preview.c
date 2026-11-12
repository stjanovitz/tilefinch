/* A page press that arrives during author work or a relayout moves the picture
   at once, from resident tiles with a checkerboard, and leaves the page alone:
   its position, tiles and the next ordinary frame are as if no preview ran. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/controller.h"
#include "tilefinch/navigation.h"
#include "tilefinch/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

#define MIB (1024u * 1024u)
#define WIDTH 480u
#define HEIGHT 272u

static const char page[] =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<style>body{margin:0}div{height:119px}"
    "#a{background:#aa0000}#b{background:#00aa00}#c{background:#0000aa}"
    "#tail{height:3000px;background:#aaaa00}</style><body>"
    "<div id=a></div><div id=b></div><div id=c></div><div id=tail></div>"
    "<a href=/n style='position:absolute;left:2px;top:250px'>link</a></body>";

static uint16_t rgb565(unsigned rgb)
{
    return (uint16_t) (((rgb >> 16) & 0xf8u) << 8
                       | ((rgb >> 8) & 0xfcu) << 3 | (rgb & 0xffu) >> 3);
}

static bool row_is(const uint16_t *frame, unsigned y, uint16_t color)
{
    for (unsigned x = 0; x < WIDTH; x++)
        if (frame[y * WIDTH + x] != color) return false;
    return true;
}

int main(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error,
                                                  sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://press.test/", page,
                                     sizeof(page) - 1u, true));
    /* Nothing composed yet: nothing to move. */
    CHECK(!browser_engine_preview_page_presses(engine, 1));
    CHECK(!browser_engine_take_press_preview(engine));
    CHECK(browser_engine_render_frame(engine, NULL));
    size_t pixels = 0;
    uint16_t *frame = browser_engine_framebuffer(engine, &pixels);
    CHECK(frame != NULL && pixels == WIDTH * HEIGHT);
    static uint16_t shown[WIDTH * HEIGHT];
    memcpy(shown, frame, sizeof(shown));
    CHECK(row_is(frame, 0, rgb565(0xaa0000))
          && row_is(frame, 130, rgb565(0x00aa00)));

    NavigationSession *navigation = browser_engine_navigation(engine);
    const NavigationEntry *entry = navigation_current(navigation);
    CHECK(entry != NULL && entry->scroll_y == 0);
    const size_t relayouts = navigation->incremental_relayouts;
    const TileCache *cache = browser_engine_render_metrics_view(engine);
    const size_t misses = cache->misses;

    /* One page is 238 pixels: the top rows are resident (the third band),
       and the rows past what was painted are checkered, not rasterized. */
    CHECK(controller_page_press_preview_step(browser_engine_controller(engine))
          == 238);
    CHECK(browser_engine_preview_page_presses(engine, 1));
    CHECK(row_is(frame, 0, rgb565(0x0000aa)));
    size_t rows_checkered = 0;
    for (unsigned y = 0; y < HEIGHT; y++) {
        unsigned x = 0;
        while (x < WIDTH && frame[y * WIDTH + x] == frame[y * WIDTH]) x++;
        if (x < WIDTH) rows_checkered++;
    }
    CHECK(rows_checkered > HEIGHT / 2u);
    CHECK(entry->scroll_y == 0 && cache->misses == misses
          && navigation->incremental_relayouts == relayouts);

    /* Later presses in the same turn build on the page's own position, and
       the far end clamps to the end the last frame knew. */
    CHECK(browser_engine_preview_page_presses(engine, 2));
    CHECK(browser_engine_preview_page_presses(engine, 4000));
    CHECK(entry->scroll_y == 0);
    /* Pressing back to where the page is shows it, overlays aside. */
    CHECK(browser_engine_preview_page_presses(engine, -1));
    CHECK(row_is(frame, 0, rgb565(0xaa0000)));

    /* Whatever became of the presses, the owner learns to repaint, once, and
       the ordinary frame is the one a preview never touched. */
    CHECK(browser_engine_take_press_preview(engine));
    CHECK(!browser_engine_take_press_preview(engine));
    CHECK(browser_engine_render_frame(engine, NULL));
    CHECK(memcmp(frame, shown, sizeof(shown)) == 0);

    /* A focused element may take the press itself: no guess is shown. */
    CHECK(browser_engine_focus_at(engine, 6, 256));
    CHECK(controller_page_press_preview_step(
              browser_engine_controller(engine)) == 0);
    CHECK(!browser_engine_preview_page_presses(engine, 1));
    CHECK(!browser_engine_take_press_preview(engine));
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    puts("page-press-preview-tests status=PASS");
    return 0;
}
