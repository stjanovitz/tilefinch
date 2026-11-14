#include "tilefinch_test_common.h"
#include "tilefinch/component_store.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/media_mp4.h"

#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Original out-of-line bodies, retained only for differential testing. */
static bool original_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
        && memcmp(name, wanted, length) == 0;
}

static uint64_t original_sample_time(const MediaMp4Sample *sample)
{
    return media_ticks_to_us(sample->dts, sample->timescale);
}

static int test_install_primitives(void)
{
    char directory[] = "/tmp/tilefinch-shared-helpers-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256], saved[256], parent[256];
    snprintf(path, sizeof(path), "%s/a/b/receipt", directory);
    snprintf(saved, sizeof(saved), "%s", path);
    CHECK(tilefinch_component_store_make_parents(path));
    CHECK(strcmp(path, saved) == 0);
    CHECK(tilefinch_component_store_make_parents(path));
    const unsigned char bytes[] = {0, 255, 17, 0, 3};
    tilefinch_memory_stick_sync_set_host_result(TILEFINCH_MEMORY_STICK_SYNC_REJECTED);
    CHECK(tilefinch_component_store_write_file(path, bytes, sizeof(bytes)));
    FILE *file = fopen(path, "rb");
    unsigned char actual[sizeof(bytes)];
    CHECK(file != NULL && fread(actual, 1, sizeof(actual), file) == sizeof(actual));
    CHECK(fgetc(file) == EOF && fclose(file) == 0);
    CHECK(memcmp(actual, bytes, sizeof(bytes)) == 0);
    /* A file in the middle of a path must fail without leaving it truncated. */
    snprintf(parent, sizeof(parent), "%s/a/b/receipt/c/entry", directory);
    snprintf(saved, sizeof(saved), "%s", parent);
    CHECK(!tilefinch_component_store_make_parents(parent));
    CHECK(strcmp(parent, saved) == 0);
    CHECK(!tilefinch_component_store_write_file(parent, bytes, sizeof(bytes)));
    CHECK(tilefinch_component_store_write_file(path, bytes, 0));
    file = fopen(path, "rb");
    CHECK(file != NULL && fgetc(file) == EOF && fclose(file) == 0);
    CHECK(unlink(path) == 0);
    snprintf(parent, sizeof(parent), "%s/a/b", directory);
    CHECK(rmdir(parent) == 0);
    snprintf(parent, sizeof(parent), "%s/a", directory);
    CHECK(rmdir(parent) == 0 && rmdir(directory) == 0);
    tilefinch_memory_stick_sync_set_host_result(TILEFINCH_MEMORY_STICK_SYNC_SKIPPED);
    return 0;
}

static uint64_t batch(bool (*name_is)(lxb_dom_node_t *, const char *),
                      uint64_t (*sample_time)(const MediaMp4Sample *),
                      lxb_dom_node_t *node, unsigned count)
{
    uint64_t sum = 0;
    bool (*volatile call_name)(lxb_dom_node_t *, const char *) = name_is;
    uint64_t (*volatile call_time)(const MediaMp4Sample *) = sample_time;
    MediaMp4Sample sample = {.timescale = 90000};
    for (unsigned i = 0; i < count; i++) {
        sample.dts = i;
        sum += call_time(&sample);
        sum += call_name(node, i & 1u ? "div" : "span");
    }
    return sum;
}

static double now_seconds(void)
{
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (double) now.tv_sec + (double) now.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    CHECK(test_install_primitives() == 0);
    const uint64_t ticks[] = {0, 1, 89999, 90000, UINT32_MAX,
                             UINT64_C(1000000000000), UINT64_MAX};
    const uint32_t scales[] = {0, 1, 1000, 44100, 90000, 1000000, UINT32_MAX};
    for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); i++) {
        for (size_t j = 0; j < sizeof(scales) / sizeof(scales[0]); j++) {
            MediaMp4Sample sample = {.dts = ticks[i], .timescale = scales[j],
                                     .pts = 7};
            CHECK(media_mp4_sample_dts_us(&sample) == original_sample_time(&sample));
        }
    }
    MediaMp4Sample sample = {.dts = 90000, .timescale = 90000, .pts = 0};
    CHECK(media_mp4_sample_dts_us(&sample) == 1000000);
    sample.timescale = 0;
    CHECK(media_mp4_sample_dts_us(&sample) == UINT64_MAX);
    Budget budget;
    budget_init(&budget, 4u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    const char html[] = "<div id=d>text<span></span><x-widget></x-widget>"
                        "<svg><linearGradient></linearGradient></svg></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    const char *names[] = {"", "div", "DIV", "span", "x-widget", "svg",
                          "linearGradient", "lineargradient", "not-present"};
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *node = root;
    unsigned visited = 0;
    while (node != NULL && visited++ < 128) {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            CHECK(document_element_name_is(node, names[i])
                  == original_name_is(node, names[i]));
        }
        if (node->first_child != NULL) node = node->first_child;
        else {
            while (node != root && node->next == NULL) node = node->parent;
            node = node == root ? NULL : node->next;
        }
    }
    CHECK(node == NULL && visited > 5);
    CHECK(!document_element_name_is(NULL, "div"));
    if (argc == 2 && strcmp(argv[1], "--bench") == 0) {
        node = find_id(root, "d");
        CHECK(node != NULL);
        CHECK(batch(original_name_is, original_sample_time, node, 10000)
              == batch(document_element_name_is, media_mp4_sample_dts_us, node, 10000));
        for (unsigned round = 0; round < 8; round++) {
            uint64_t sums[2];
            double elapsed[2];
            for (unsigned lane = 0; lane < 2; lane++) {
                unsigned choice = lane ^ (round & 1u);
                double begin = now_seconds();
                sums[choice] = choice
                    ? batch(document_element_name_is, media_mp4_sample_dts_us, node, 5000000)
                    : batch(original_name_is, original_sample_time, node, 5000000);
                elapsed[choice] = now_seconds() - begin;
            }
            CHECK(sums[0] == sums[1]);
            printf("round=%u local_ms=%.3f shared_ms=%.3f checksum=%llu\n",
                   round, elapsed[0] * 1000, elapsed[1] * 1000,
                   (unsigned long long) sums[0]);
        }
    }
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    puts("shared helpers: name, timestamp, and installation tests passed");
    return 0;
}
