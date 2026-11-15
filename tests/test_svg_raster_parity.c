/* SVG raster parity: the decoder's pixels for a set of SVG documents at
   several raster sizes, against recorded goldens.

   With no arguments it checks the repository-owned fixtures in
   tests/fixtures/svg-raster against golden.tsv there (an FNV-1a hash of
   every RGBA byte, so any pixel change fails). The fixtures are written
   the way exporters write real artwork (8-9 significant digits, exponent
   forms, evenodd counters, strokes, arcs, gradients over curves,
   transforms, clips and nested viewports) without borrowing anyone's
   logo.

   Real-site artwork stays out of the repository; check it from a local
   corpus instead, as a pixel diff:

     tilefinch-svg-raster-parity-tests --record DIR FILE.svg...
     tilefinch-svg-raster-parity-tests --compare DIR [--tolerance N] FILE.svg...

   --record writes each raster to DIR (raw RGBA); --compare decodes again
   and reports, per raster, the pixels that differ and the largest channel
   difference, failing above the tolerance (default 0: identical).
   --print-golden prints golden.tsv for the repository fixtures, and
   --time FILE.svg... reports where decode time goes on this host. */
#include "tilefinch_test_common.h"
#include "../src/image_svg_decode_internal.h"

#include <errno.h>
#include <sys/stat.h>

#define FIXTURE_DIR TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/svg-raster"

static const char *const repository_fixtures[] = {
    FIXTURE_DIR "/wordmark.svg",
    FIXTURE_DIR "/strokes.svg",
    FIXTURE_DIR "/gradients.svg",
    FIXTURE_DIR "/transforms.svg",
    FIXTURE_DIR "/no-viewbox.svg",
    FIXTURE_DIR "/class-styles.svg",
    TILEFINCH_TEST_SOURCE_DIR "/fixtures/unified-mark.svg",
    TILEFINCH_TEST_SOURCE_DIR "/examples/treadline-arena/icon.svg"
};

/* Raster sizes asked for: the document's own size, then boxes that scale
   it down, up and out of proportion. */
static const int raster_sizes[][2] = {{0, 0}, {17, 13}, {64, 64}, {150, 45}};
#define RASTER_SIZE_COUNT (sizeof(raster_sizes) / sizeof(raster_sizes[0]))

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    char *data = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        if (size >= 0 && fseek(file, 0, SEEK_SET) == 0) {
            data = malloc((size_t) size + 1u);
            if (data != NULL
                && fread(data, 1, (size_t) size, file) == (size_t) size) {
                data[size] = '\0';
                *length = (size_t) size;
            } else {
                free(data);
                data = NULL;
            }
        }
    }
    fclose(file);
    return data;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static uint64_t fnv1a(const unsigned char *bytes, size_t length)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

typedef struct {
    unsigned char *pixels;
    int width, height;
} Raster;

/* Decodes `path` at raster size `size`; pixels NULL when it does not. */
static int decode(Budget *budget, const char *path, size_t size,
                  Raster *raster)
{
    size_t length = 0;
    char *data = read_file(path, &length);
    if (data == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    raster->width = raster_sizes[size][0];
    raster->height = raster_sizes[size][1];
    raster->pixels = image_svg_decode(data, length, budget, 4u * MIB,
                                      &raster->width, &raster->height);
    free(data);
    if (raster->pixels == NULL) raster->width = raster->height = 0;
    return 0;
}

static size_t raster_bytes(const Raster *raster)
{
    return (size_t) raster->width * (size_t) raster->height * 4u;
}

static int check_repository_fixtures(Budget *budget, bool print)
{
    size_t golden_length = 0;
    char *golden = print ? NULL
        : read_file(FIXTURE_DIR "/golden.tsv", &golden_length);
    CHECK(print || golden != NULL);
    int failures = 0;
    for (size_t i = 0; i < sizeof(repository_fixtures)
                                / sizeof(repository_fixtures[0]); i++) {
        for (size_t size = 0; size < RASTER_SIZE_COUNT; size++) {
            Raster raster;
            CHECK(decode(budget, repository_fixtures[i], size, &raster) == 0);
            CHECK(raster.pixels != NULL);
            char line[192];
            snprintf(line, sizeof(line), "%s\t%dx%d\t%dx%d\t%016llx\n",
                     base_name(repository_fixtures[i]),
                     raster_sizes[size][0], raster_sizes[size][1],
                     raster.width, raster.height,
                     (unsigned long long) fnv1a(raster.pixels,
                                                raster_bytes(&raster)));
            budget_free(budget, raster.pixels);
            CHECK(budget->current == 0u);
            if (print) {
                fputs(line, stdout);
            } else if (strstr(golden, line) == NULL) {
                fprintf(stderr, "raster differs from golden.tsv: %s", line);
                failures++;
            }
        }
    }
    free(golden);
    CHECK(failures == 0);
    return 0;
}

static void raster_path(char *path, size_t capacity, const char *directory,
                        const char *file, size_t size)
{
    snprintf(path, capacity, "%s/%s.%dx%d.rgba", directory, base_name(file),
             raster_sizes[size][0], raster_sizes[size][1]);
}

static int record(Budget *budget, const char *directory, char **files,
                  int count)
{
    if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s\n", directory);
        return 1;
    }
    for (int i = 0; i < count; i++) {
        for (size_t size = 0; size < RASTER_SIZE_COUNT; size++) {
            Raster raster;
            CHECK(decode(budget, files[i], size, &raster) == 0);
            char path[1024];
            raster_path(path, sizeof(path), directory, files[i], size);
            FILE *out = fopen(path, "wb");
            CHECK(out != NULL);
            int header[2] = {raster.width, raster.height};
            CHECK(fwrite(header, sizeof(header), 1, out) == 1);
            CHECK(raster_bytes(&raster) == 0
                  || fwrite(raster.pixels, raster_bytes(&raster), 1, out)
                         == 1);
            fclose(out);
            budget_free(budget, raster.pixels);
            CHECK(budget->current == 0u);
        }
    }
    printf("recorded %d documents x %zu sizes in %s\n", count,
           RASTER_SIZE_COUNT, directory);
    return 0;
}

static int compare(Budget *budget, const char *directory, int tolerance,
                   char **files, int count)
{
    size_t rasters = 0, identical = 0, failed = 0;
    for (int i = 0; i < count; i++) {
        for (size_t size = 0; size < RASTER_SIZE_COUNT; size++) {
            char path[1024];
            raster_path(path, sizeof(path), directory, files[i], size);
            size_t length = 0;
            char *recorded = read_file(path, &length);
            if (recorded == NULL || length < 2u * sizeof(int)) {
                fprintf(stderr, "no recorded raster %s\n", path);
                free(recorded);
                return 1;
            }
            Raster raster;
            CHECK(decode(budget, files[i], size, &raster) == 0);
            int header[2];
            memcpy(header, recorded, sizeof(header));
            const unsigned char *before =
                (const unsigned char *) recorded + sizeof(header);
            size_t differing = 0;
            int largest = 0;
            bool same_size = header[0] == raster.width
                && header[1] == raster.height
                && length == sizeof(header) + raster_bytes(&raster);
            if (same_size) {
                for (size_t p = 0; p < raster_bytes(&raster); p += 4u) {
                    int pixel_largest = 0;
                    for (size_t c = 0; c < 4u; c++) {
                        int delta = abs((int) before[p + c]
                                        - (int) raster.pixels[p + c]);
                        if (delta > pixel_largest) pixel_largest = delta;
                    }
                    if (pixel_largest != 0) differing++;
                    if (pixel_largest > largest) largest = pixel_largest;
                }
            }
            rasters++;
            if (same_size && differing == 0) identical++;
            if (!same_size || largest > tolerance) failed++;
            if (!same_size || differing != 0) {
                printf("%s %dx%d: %s differing=%zu max-channel=%d\n",
                       base_name(files[i]), raster_sizes[size][0],
                       raster_sizes[size][1],
                       same_size ? "pixels" : "SIZE CHANGED", differing,
                       largest);
            }
            budget_free(budget, raster.pixels);
            free(recorded);
            CHECK(budget->current == 0u);
        }
    }
    printf("compared %zu rasters: %zu identical, %zu over tolerance %d\n",
           rasters, identical, failed, tolerance);
    return failed == 0 ? 0 : 1;
}

static int time_decodes(Budget *budget, char **files, int count)
{
    ImageSvgDecodeProfile before, after;
    image_svg_decode_profile(&before);
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < count; i++) {
            Raster raster;
            CHECK(decode(budget, files[i], 0, &raster) == 0);
            budget_free(budget, raster.pixels);
        }
    }
    image_svg_decode_profile(&after);
    printf("decodes=%zu parse=%lluus raster=%lluus\n",
           after.rasters - before.rasters,
           (unsigned long long) (after.parse_us - before.parse_us),
           (unsigned long long) (after.raster_us - before.raster_us));
    return 0;
}

int main(int argc, char **argv)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    if (argc >= 2 && strcmp(argv[1], "--print-golden") == 0)
        return check_repository_fixtures(&budget, true);
    if (argc >= 3 && strcmp(argv[1], "--record") == 0)
        return record(&budget, argv[2], argv + 3, argc - 3);
    if (argc >= 3 && strcmp(argv[1], "--compare") == 0) {
        int first = 3, tolerance = 0;
        if (argc >= 5 && strcmp(argv[3], "--tolerance") == 0) {
            tolerance = atoi(argv[4]);
            first = 5;
        }
        return compare(&budget, argv[2], tolerance, argv + first,
                       argc - first);
    }
    if (argc >= 2 && strcmp(argv[1], "--time") == 0)
        return time_decodes(&budget, argv + 2, argc - 2);
    CHECK(argc == 1);
    CHECK(check_repository_fixtures(&budget, false) == 0);
    puts("SVG raster parity passed");
    return 0;
}
