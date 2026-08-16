#ifndef TILEFINCH_SECTION_PAGER_H
#define TILEFINCH_SECTION_PAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/section_store.h"

typedef struct {
    CompressedSectionStore *store;
    char *current_html;
    size_t current_length;
    size_t current_section;
    char *prefetched_html;
    size_t prefetched_length;
    size_t prefetched_section;
    int *known_heights;
    char **focus_ids;
    /* Allocation bound for every per-section state array.  The attached store
       may be transactionally reindexed before this pager is rebuilt, so its
       live section_count is not a safe allocation bound. */
    size_t state_capacity;
    uint64_t store_revision;
    uint64_t state_revision;
    size_t section_loads;
    size_t prefetches;
    size_t prefetch_hits;
    size_t prefetch_evictions;
    uint64_t last_decode_us;
    uint64_t last_prefetch_us;
    uint64_t decode_total_us;
    uint64_t decode_max_us;
    uint64_t prefetch_total_us;
    uint64_t prefetch_max_us;
    size_t decode_samples;
    size_t prefetch_samples;
    int pending_prefetch_direction;
    int last_move_direction;
    size_t sequential_moves;
    bool initialized;
    bool has_prefetch;
    bool last_select_prefetch_hit;
} SectionPager;

/*
 * A section selection is materialized before it is made visible.  Preparing
 * never changes the pager's current section and never releases its current or
 * prefetched fragment.  The caller may therefore parse/build the candidate
 * and either commit it after the page succeeds or abort it on cancellation.
 *
 * Treat this structure as opaque state: initialize it only through prepare,
 * then finish it exactly once through commit or abort.
 */
typedef struct {
    SectionPager *pager;
    Budget *budget;
    char *html;
    size_t length;
    size_t section;
    size_t base_section;
    char *base_html;
    char *base_prefetched_html;
    uint64_t base_revision;
    int prefetch_direction;
    uint64_t decode_us;
    bool active;
    bool same_section;
    bool uses_prefetch;
    bool owns_html;
} SectionPagerSelection;

bool section_pager_init(SectionPager *pager, CompressedSectionStore *store,
                        size_t initial_section, int prefetch_direction);
bool section_pager_valid(const SectionPager *pager);
bool section_pager_prepare(SectionPager *pager, size_t section,
                           int prefetch_direction,
                           SectionPagerSelection *selection);
bool section_pager_commit(SectionPagerSelection *selection);
void section_pager_abort(SectionPagerSelection *selection);
bool section_pager_select(SectionPager *pager, size_t section,
                          int prefetch_direction);
bool section_pager_move(SectionPager *pager, int direction);
/* Materializes a predicted neighbor only after sustained movement. Call from
   an idle or post-paint boundary so speculative HTML never delays a tile. */
bool section_pager_prefetch_pending(SectionPager *pager);
void section_pager_record_height(SectionPager *pager, int height);
bool section_pager_record_section_height(SectionPager *pager, size_t section,
                                         int height);
bool section_pager_known_height(const SectionPager *pager, size_t section,
                                int *height);
bool section_pager_known_logical_offset(const SectionPager *pager,
                                        size_t section, size_t *offset);
bool section_pager_record_focus(SectionPager *pager, const char *identifier,
                                size_t length);
const char *section_pager_focus(const SectionPager *pager, size_t section);
void section_pager_destroy(SectionPager *pager);

#endif
