/* Global camera motion between consecutive video frames, for
   scripts/analyze-game-video.py.

   Reads 8-bit grayscale frames of WIDTH x HEIGHT from stdin and prints one
   line per frame:

     <index> <log zoom> <dx> <dy> <cost> <identity cost>

   The model maps the previous frame onto the current one by a zoom about
   the screen centre plus a translation: x = c + (x' - c) * s + t. A game
   camera looks at a target at the screen centre, so a dolly (the camera
   retracting toward the player) is a zoom, and a pitch or bob is a vertical
   shift. Only the rows [BAND_Y0, BAND_Y1) of the current frame are compared,
   which keeps HUD text out of the estimate. Costs are the mean absolute
   luma difference over the compared pixels: the identity cost (s = 1,
   t = 0) is how much the frame changed, the cost how much the model leaves.

   Search: every zoom from 0.5x to 2x (a retraction is a large zoom) with a
   coarse translation grid on a quarter-size image, the best few candidates
   refined on the half-size image, and the winner at full size. A small C helper
   because the same search in pure Python takes seconds per frame. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int width, height;
    float *pixels;
} Plane;

typedef struct {
    double cost, scale, tx, ty;  /* translation in this level's pixels */
} Candidate;

enum { LEVELS = 3, KEEP = 3 };

static void downsample(const Plane *from, Plane *to)
{
    for (int y = 0; y < to->height; y++) {
        for (int x = 0; x < to->width; x++) {
            const float *a = from->pixels + (size_t) (y * 2) * (size_t) from->width + (size_t) x * 2u;
            const float *b = a + from->width;
            to->pixels[(size_t) y * (size_t) to->width + (size_t) x] = (a[0] + a[1] + b[0] + b[1]) * .25f;
        }
    }
}

/* Mean |current - warped previous| over the band, or HUGE_VAL when too
   little of the band maps inside the previous frame. */
static double warp_cost(const Plane *current, const Plane *previous, int band0, int band1,
                        double scale, double tx, double ty, int stride)
{
    const double cx = (current->width - 1) * .5, cy = (current->height - 1) * .5;
    const double inverse = 1.0 / scale;
    double total = 0.0;
    long count = 0, considered = 0;
    for (int y = band0; y < band1; y += stride) {
        const double sy = cy + (y - cy - ty) * inverse;
        const float *row = current->pixels + (size_t) y * (size_t) current->width;
        for (int x = 0; x < current->width; x += stride) {
            considered++;
            const double sx = cx + (x - cx - tx) * inverse;
            if (sx < 0 || sy < 0 || sx >= previous->width - 1 || sy >= previous->height - 1) continue;
            const int ix = (int) sx, iy = (int) sy;
            const double fx = sx - ix, fy = sy - iy;
            const float *p = previous->pixels + (size_t) iy * (size_t) previous->width + (size_t) ix;
            const double top = p[0] + (p[1] - p[0]) * fx;
            const double bottom = p[previous->width] + (p[previous->width + 1] - p[previous->width]) * fx;
            total += fabs(row[x] - (top + (bottom - top) * fy));
            count++;
        }
    }
    if (count == 0 || count * 10 < considered * 4) return HUGE_VAL;
    return total / (double) count;
}

static void keep_best(Candidate *best, Candidate candidate)
{
    for (int at = 0; at < KEEP; at++) {
        /* Neighbours of a kept candidate refine to the same answer. */
        if (fabs(log(best[at].scale / candidate.scale)) < .06
            && fabs(best[at].tx - candidate.tx) < 2.5 && fabs(best[at].ty - candidate.ty) < 2.5) {
            if (candidate.cost < best[at].cost) best[at] = candidate;
            return;
        }
    }
    for (int at = 0; at < KEEP; at++) {
        if (candidate.cost < best[at].cost) {
            memmove(best + at + 1, best + at, sizeof(*best) * (size_t) (KEEP - 1 - at));
            best[at] = candidate;
            return;
        }
    }
}

static Candidate refine(const Plane *current, const Plane *previous, int band0, int band1,
                        Candidate start, double scale_step, double shift_step)
{
    Candidate best = start;
    best.cost = warp_cost(current, previous, band0, band1, best.scale, best.tx, best.ty, 1);
    for (int round = 0; round < 3; round++) {
        Candidate centre = best;
        for (int ds = -1; ds <= 1; ds++) {
            for (int dx = -1; dx <= 1; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                    if (!ds && !dx && !dy) continue;
                    Candidate trial = {0, centre.scale * exp(ds * scale_step),
                                       centre.tx + dx * shift_step, centre.ty + dy * shift_step};
                    trial.cost = warp_cost(current, previous, band0, band1,
                                           trial.scale, trial.tx, trial.ty, 1);
                    if (trial.cost < best.cost) best = trial;
                }
            }
        }
        scale_step *= .5;
        shift_step *= .5;
    }
    return best;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s WIDTH HEIGHT BAND_Y0 BAND_Y1 < gray frames\n", argv[0]);
        return 2;
    }
    const int width = atoi(argv[1]), height = atoi(argv[2]);
    const int band0 = atoi(argv[3]), band1 = atoi(argv[4]);
    if (width < 32 || height < 32 || width > 4096 || height > 4096 || (width & 3) || (height & 3)
        || band0 < 0 || band1 > height || band1 - band0 < 8) {
        fprintf(stderr, "bad geometry\n");
        return 2;
    }
    const size_t frame_bytes = (size_t) width * (size_t) height;
    unsigned char *bytes = malloc(frame_bytes);
    Plane planes[2][LEVELS];
    for (int which = 0; which < 2; which++) {
        for (int level = 0; level < LEVELS; level++) {
            planes[which][level].width = width >> level;
            planes[which][level].height = height >> level;
            planes[which][level].pixels = malloc(sizeof(float) * (frame_bytes >> (2 * level)));
            if (!planes[which][level].pixels) return 1;
        }
    }
    if (!bytes) return 1;
    int current = 0;
    for (long index = 0;; index++) {
        if (fread(bytes, 1, frame_bytes, stdin) != frame_bytes) break;
        Plane *now = planes[current], *before = planes[current ^ 1];
        for (size_t at = 0; at < frame_bytes; at++) now[0].pixels[at] = bytes[at];
        for (int level = 1; level < LEVELS; level++) downsample(&now[level - 1], &now[level]);
        if (index == 0) {
            printf("0 0 0 0 0 0\n");
        } else {
            const int coarse = LEVELS - 1, factor = 1 << coarse;
            const int c0 = band0 / factor, c1 = band1 / factor;
            Candidate best[KEEP];
            for (int at = 0; at < KEEP; at++) best[at] = (Candidate) {HUGE_VAL, 1, 0, 0};
            for (int k = -14; k <= 14; k++) {
                const double scale = exp(k * .05);
                for (int ty = -8; ty <= 8; ty += 2) {
                    for (int tx = -8; tx <= 8; tx += 2) {
                        Candidate trial = {warp_cost(&now[coarse], &before[coarse], c0, c1,
                                                     scale, tx, ty, 1), scale, tx, ty};
                        if (trial.cost < HUGE_VAL) keep_best(best, trial);
                    }
                }
            }
            /* The kept candidates compete on the quarter- and half-size
               images; only the winner is refined at full size. */
            Candidate winner = {HUGE_VAL, 1, 0, 0};
            for (int at = 0; at < KEEP; at++) {
                if (best[at].cost == HUGE_VAL) continue;
                Candidate candidate = best[at];
                double scale_step = .025;
                for (int level = coarse; level >= 1; level--) {
                    const int f = 1 << level;
                    candidate = refine(&now[level], &before[level], band0 / f, band1 / f,
                                       candidate, scale_step, 1.0);
                    scale_step *= .5;
                }
                if (candidate.cost < winner.cost) winner = candidate;
            }
            if (winner.cost < HUGE_VAL) {
                winner.tx *= 2;
                winner.ty *= 2;
                winner = refine(&now[0], &before[0], band0, band1, winner, .00625, 1.0);
            }
            const double identity = warp_cost(&now[0], &before[0], band0, band1, 1, 0, 0, 1);
            if (winner.cost == HUGE_VAL || identity <= winner.cost + 1e-9)
                winner = (Candidate) {identity, 1, 0, 0};
            printf("%ld %.5f %.3f %.3f %.3f %.3f\n", index, log(winner.scale),
                   winner.tx, winner.ty, winner.cost, identity);
        }
        fflush(stdout);
        current ^= 1;
    }
    for (int which = 0; which < 2; which++)
        for (int level = 0; level < LEVELS; level++) free(planes[which][level].pixels);
    free(bytes);
    return 0;
}
