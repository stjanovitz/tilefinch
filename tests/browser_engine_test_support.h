#ifndef TILEFINCH_BROWSER_ENGINE_TEST_SUPPORT_H
#define TILEFINCH_BROWSER_ENGINE_TEST_SUPPORT_H

#include "tilefinch/browser_engine.h"
#include "tilefinch/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "ENGINE CHECK failed at %s:%d: %s\n",                 \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

uint64_t frame_checksum(const uint16_t *pixels, size_t pixel_count);
lxb_dom_node_t *test_reader_find_id(lxb_dom_node_t *root, const char *wanted);
int test_loading_interaction_journey(void);
int test_script_free_browsing_journey(void);
int test_background_interruption_journey(void);
int test_deferred_startup_journey(void);
int test_deferred_image_publication_survives_rebuild(void);
int test_deferred_images_skip_oversized_pages(void);

#endif
