/* Short on-off-on blinks of localized screen regions, for
   scripts/analyze-game-video.py.

   Reads RGB24 frames of WIDTH x HEIGHT from stdin. A pixel blinks over a
   run of 1..MAX_RUN frames when the frames just before and just after the
   run agree (every channel within AGREE) and every frame of the run differs
   from both of them (some channel by at least JUMP): something that
   appears for a few frames and is gone again, or that vanishes and comes
   back. Smooth motion (a tank driving, the camera panning) does not return
   to the same pixels, so it is not a blink; a camera that moves every
   frame hides blinks in the scene, though not in the screen-fixed HUD.

   Blinking pixels are counted per BLOCK x BLOCK cell; a cell with at least
   MIN_PIXELS of them is on, and each 8-connected group of on cells is one
   blink. For every blink this prints one line:

     blink <first> <run> <x0> <y0> <x1> <y1> <cells> <pixels> <brighter>
           <run r g b> <around r g b> <context>

   first is the index of the run's first frame, the box is in this stream's
   pixels (inclusive-exclusive), pixels counts blinking pixels, brighter is
   1 when the run is brighter than its surroundings (something appeared) and
   0 when darker (something vanished), the two colours are the mean of
   the blinking pixels in the run's first frame and in the frame before it,
   and context counts the pixels around the blink (its box grown by
   CONTEXT) where the frames before and after the run disagree: near zero
   for an isolated blink, large when the blink is a piece of something
   that moved (a shell, a shaking camera) rather than something that
   blinked.

   A run is reported at its exact length only: every frame of a longer run
   must differ from the surroundings, so a one-frame blink is not also a
   two-frame one (its second frame already matches them).

   Usage: tilefinch-video-blink WIDTH HEIGHT [JUMP AGREE MIN_PIXELS] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_RUN = 4, BLOCK = 4, RING = MAX_RUN + 2, CONTEXT = 16 };

static int channel_distance(const unsigned char *a, const unsigned char *b)
{
    int best = 0;
    for (int c = 0; c < 3; c++) {
        const int d = a[c] > b[c] ? a[c] - b[c] : b[c] - a[c];
        if (d > best)
            best = d;
    }
    return best;
}

static int luma(const unsigned char *p)
{
    return p[0] * 77 + p[1] * 150 + p[2] * 29;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 6) {
        fprintf(stderr, "usage: %s WIDTH HEIGHT [JUMP AGREE MIN_PIXELS]\n", argv[0]);
        return 2;
    }
    const int width = atoi(argv[1]), height = atoi(argv[2]);
    const int jump = argc == 6 ? atoi(argv[3]) : 48;
    const int agree = argc == 6 ? atoi(argv[4]) : 12;
    const int min_pixels = argc == 6 ? atoi(argv[5]) : 4;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        fprintf(stderr, "bad frame size\n");
        return 2;
    }
    const size_t frame_bytes = (size_t) width * (size_t) height * 3u;
    const int columns = (width + BLOCK - 1) / BLOCK, rows = (height + BLOCK - 1) / BLOCK;
    const size_t cells = (size_t) columns * (size_t) rows;
    unsigned char *ring[RING];
    for (int i = 0; i < RING; i++) {
        ring[i] = malloc(frame_bytes);
        if (!ring[i])
            return 1;
    }
    unsigned char *mask = malloc((size_t) width * (size_t) height);
    int *count = malloc(cells * sizeof *count);
    int *label = malloc(cells * sizeof *label);
    int *stack = malloc(cells * sizeof *stack);
    /* Summed-area table of pixels where the frames around a run disagree. */
    const size_t table_width = (size_t) width + 1u;
    int *changed = malloc(table_width * ((size_t) height + 1u) * sizeof *changed);
    if (!mask || !count || !label || !stack || !changed)
        return 1;
    long index = 0;
    for (;;) {
        unsigned char *current = ring[index % RING];
        if (fread(current, 1, frame_bytes, stdin) != frame_bytes)
            break;
        /* current is the frame after a run of `run` frames ending at
           index - 1; the frame before the run is index - run - 1. */
        for (int run = 1; run <= MAX_RUN && index - run - 1 >= 0; run++) {
            const unsigned char *before = ring[(index - run - 1) % RING];
            memset(count, 0, cells * sizeof *count);
            int any = 0;
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    const size_t at = ((size_t) y * (size_t) width + (size_t) x) * 3u;
                    unsigned char on = 0;
                    if (channel_distance(before + at, current + at) <= agree) {
                        on = 1;
                        for (int k = 1; k <= run && on; k++) {
                            const unsigned char *middle = ring[(index - k) % RING] + at;
                            if (channel_distance(middle, before + at) < jump
                                    || channel_distance(middle, current + at) < jump)
                                on = 0;
                        }
                    }
                    mask[(size_t) y * (size_t) width + (size_t) x] = on;
                    if (on) {
                        count[(size_t) (y / BLOCK) * (size_t) columns + (size_t) (x / BLOCK)]++;
                        any = 1;
                    }
                }
            }
            if (!any)
                continue;
            memset(changed, 0, table_width * sizeof *changed);
            for (int y = 0; y < height; y++) {
                int line = 0;
                int *row = changed + (size_t) (y + 1) * table_width;
                const int *above = row - table_width;
                row[0] = 0;
                for (int x = 0; x < width; x++) {
                    const size_t at = ((size_t) y * (size_t) width + (size_t) x) * 3u;
                    line += channel_distance(before + at, current + at) >= jump;
                    row[x + 1] = above[x + 1] + line;
                }
            }
            for (size_t c = 0; c < cells; c++)
                label[c] = count[c] >= min_pixels ? 0 : -1;
            int next = 0;
            for (size_t seed = 0; seed < cells; seed++) {
                if (label[seed] != 0)
                    continue;
                next++;
                int top = 0;
                stack[top++] = (int) seed;
                label[seed] = next;
                int x0 = columns, y0 = rows, x1 = -1, y1 = -1, members = 0;
                while (top) {
                    const int at = stack[--top];
                    const int cx = at % columns, cy = at / columns;
                    members++;
                    if (cx < x0) x0 = cx;
                    if (cy < y0) y0 = cy;
                    if (cx > x1) x1 = cx;
                    if (cy > y1) y1 = cy;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const int nx = cx + dx, ny = cy + dy;
                            if (nx < 0 || ny < 0 || nx >= columns || ny >= rows)
                                continue;
                            const int n = ny * columns + nx;
                            if (label[n] == 0) {
                                label[n] = next;
                                stack[top++] = n;
                            }
                        }
                    }
                }
                /* Colours of the blinking pixels inside the group's box. */
                const unsigned char *first = ring[(index - run) % RING];
                long sum_run[3] = {0, 0, 0}, sum_before[3] = {0, 0, 0}, pixels = 0, brighter = 0;
                const int px0 = x0 * BLOCK, py0 = y0 * BLOCK;
                int px1 = (x1 + 1) * BLOCK, py1 = (y1 + 1) * BLOCK;
                if (px1 > width) px1 = width;
                if (py1 > height) py1 = height;
                for (int y = py0; y < py1; y++) {
                    for (int x = px0; x < px1; x++) {
                        const size_t p = (size_t) y * (size_t) width + (size_t) x;
                        if (!mask[p] || label[(size_t) (y / BLOCK) * (size_t) columns
                                                + (size_t) (x / BLOCK)] != next)
                            continue;
                        pixels++;
                        for (int c = 0; c < 3; c++) {
                            sum_run[c] += first[p * 3u + (size_t) c];
                            sum_before[c] += before[p * 3u + (size_t) c];
                        }
                        brighter += luma(first + p * 3u) > luma(before + p * 3u);
                    }
                }
                if (!pixels)
                    continue;
                /* Context: pixels around the blink (its box grown by
                   CONTEXT) where the frames before and after the run
                   disagree. A blink is isolated when this is small; a
                   moving object or a shaking camera changes its
                   surroundings too. */
                const int cx0 = px0 - CONTEXT < 0 ? 0 : px0 - CONTEXT;
                const int cy0 = py0 - CONTEXT < 0 ? 0 : py0 - CONTEXT;
                const int cx1 = px1 + CONTEXT > width ? width : px1 + CONTEXT;
                const int cy1 = py1 + CONTEXT > height ? height : py1 + CONTEXT;
                const long context = (long) changed[(size_t) cy1 * table_width + (size_t) cx1]
                    - changed[(size_t) cy0 * table_width + (size_t) cx1]
                    - changed[(size_t) cy1 * table_width + (size_t) cx0]
                    + changed[(size_t) cy0 * table_width + (size_t) cx0];
                printf("blink %ld %d %d %d %d %d %d %ld %d %ld %ld %ld %ld %ld %ld %ld\n",
                       index - run, run, px0, py0, px1, py1, members, pixels,
                       brighter * 2 > pixels,
                       sum_run[0] / pixels, sum_run[1] / pixels, sum_run[2] / pixels,
                       sum_before[0] / pixels, sum_before[1] / pixels, sum_before[2] / pixels,
                       context);
            }
        }
        printf("frame %ld\n", index);
        fflush(stdout);
        index++;
    }
    for (int i = 0; i < RING; i++)
        free(ring[i]);
    free(mask);
    free(count);
    free(label);
    free(stack);
    free(changed);
    return 0;
}
