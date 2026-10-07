#ifndef TILEFINCH_BROWSER_ENGINE_TEST_SUPPORT_H
#define TILEFINCH_BROWSER_ENGINE_TEST_SUPPORT_H

#include "tilefinch/script_loader.h"
#include "tilefinch/browser_engine.h"
#include "tilefinch/platform.h"
#include "tilefinch_test_clocks.h"
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
int test_script_focus_adoption(void);
int test_late_script_text_focus_adoption(void);
int test_boot_window_check_backs_off(void);
int test_idle_page_drains_wrapper_cleanups(void);
int test_large_incumbent_realm_retired_for_navigation(void);
int test_deferred_script_navigation(void);
int test_textarea_rows_geometry(void);
int test_scroll_into_view_resolves_margin_math(void);
int test_grid_fr_rows_share_after_content(void);
int test_grid_auto_row_stretch_and_focus_clip(void);
int test_inset_pseudo_disc(void);
int test_night_mode_prefers_dark(void);
int test_declared_var_and_script_form_submission(void);
int test_module_dependency_prefetch(void);
int test_script_free_browsing_journey(void);
int test_background_interruption_journey(void);
int test_deferred_startup_journey(void);
int test_deferred_image_publication_survives_rebuild(void);
int test_deferred_images_skip_oversized_pages(void);

#endif
