#include "tilefinch/section_pager.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "tilefinch/platform.h"

static uint64_t pager_now_us(void)
{
    return tilefinch_platform_monotonic_time_us();
}

static bool pager_state_current(const SectionPager *pager)
{
    return pager != NULL && pager->initialized && pager->store != NULL
        && pager->store->budget != NULL && pager->state_capacity != 0
        && pager->store->section_count == pager->state_capacity
        && pager->store_revision == pager->store->index_revision
        && pager->current_section < pager->state_capacity;
}

bool section_pager_valid(const SectionPager *pager)
{
    return pager_state_current(pager);
}

static void pager_bump_revision(SectionPager *pager)
{
    pager->state_revision++;
    if (pager->state_revision == 0) pager->state_revision = 1;
}

static void pager_record_decode(SectionPager *pager, uint64_t elapsed)
{
    pager->last_decode_us = elapsed;
    pager->decode_total_us += elapsed;
    if (elapsed > pager->decode_max_us) pager->decode_max_us = elapsed;
    pager->decode_samples++;
}

static void pager_record_prefetch(SectionPager *pager, uint64_t elapsed)
{
    pager->last_prefetch_us = elapsed;
    pager->prefetch_total_us += elapsed;
    if (elapsed > pager->prefetch_max_us) pager->prefetch_max_us = elapsed;
    pager->prefetch_samples++;
}

static void discard_prefetch(SectionPager *pager)
{
    if (!pager->has_prefetch) return;
    budget_free(pager->store->budget, pager->prefetched_html);
    pager->prefetched_html = NULL;
    pager->prefetched_length = 0;
    pager->has_prefetch = false;
    pager->prefetch_evictions++;
}

static bool prefetch_neighbor(SectionPager *pager, int direction)
{
    if (!pager_state_current(pager)) return false;
    pager_bump_revision(pager);
    discard_prefetch(pager);
    pager->last_prefetch_us = 0;
    if (direction == 0) return true;
    if ((direction < 0 && pager->current_section == 0)
        || (direction > 0
            && pager->current_section + 1 >= pager->store->section_count))
        return true;
    size_t section = direction < 0
                     ? pager->current_section - 1 : pager->current_section + 1;
    uint64_t started = pager_now_us();
    if (!section_store_extract_html(pager->store, section,
                                    &pager->prefetched_html,
                                    &pager->prefetched_length)) return false;
    pager_record_prefetch(pager, pager_now_us() - started);
    pager->prefetched_section = section;
    pager->has_prefetch = true;
    pager->prefetches++;
    return true;
}

bool section_pager_prefetch_pending(SectionPager *pager)
{
    if (!pager_state_current(pager)) return false;
    int direction = pager->pending_prefetch_direction;
    pager->pending_prefetch_direction = 0;
    if (direction != 0) pager_bump_revision(pager);
    /* One isolated movement is common on short documents and does not
       justify retaining another parser-ready fragment. Sustained movement is
       a useful generic signal that the neighbor is likely to be consumed. */
    if (direction == 0 || pager->sequential_moves < 2) return true;
    return prefetch_neighbor(pager, direction);
}

bool section_pager_init(SectionPager *pager, CompressedSectionStore *store,
                        size_t initial_section, int prefetch_direction)
{
    if (pager == NULL || store == NULL || store->budget == NULL
        || initial_section >= store->section_count) return false;
    memset(pager, 0, sizeof(*pager));
    pager->store = store;
    pager->state_capacity = store->section_count;
    pager->store_revision = store->index_revision;
    pager->state_revision = 1;
    pager->known_heights = budget_calloc_category(
        store->budget, BUDGET_CATEGORY_NAVIGATION, pager->state_capacity,
        sizeof(*pager->known_heights));
    pager->focus_ids = budget_calloc_category(
        store->budget, BUDGET_CATEGORY_NAVIGATION, pager->state_capacity,
        sizeof(*pager->focus_ids));
    uint64_t decode_started = pager_now_us();
    if (pager->known_heights == NULL || pager->focus_ids == NULL
        || !section_store_extract_html(store, initial_section,
                                       &pager->current_html,
                                       &pager->current_length)) {
        section_pager_destroy(pager);
        return false;
    }
    pager_record_decode(pager, pager_now_us() - decode_started);
    pager->current_section = initial_section;
    pager->section_loads = 1;
    pager->initialized = true;
    (void) prefetch_direction;
    return true;
}

bool section_pager_record_focus(SectionPager *pager, const char *identifier,
                                size_t length)
{
    if (!pager_state_current(pager) || pager->focus_ids == NULL || length > 255
        || (identifier == NULL && length != 0)) return false;
    Budget *budget = pager->store->budget;
    char *replacement = NULL;
    if (length != 0) {
        replacement = budget_malloc_category(
            budget, BUDGET_CATEGORY_NAVIGATION, length + 1);
        if (replacement == NULL) return false;
        memcpy(replacement, identifier, length);
        replacement[length] = '\0';
    }
    budget_free(budget, pager->focus_ids[pager->current_section]);
    pager->focus_ids[pager->current_section] = replacement;
    return true;
}

const char *section_pager_focus(const SectionPager *pager, size_t section)
{
    return !pager_state_current(pager) || pager->focus_ids == NULL
           || section >= pager->state_capacity
        ? NULL : pager->focus_ids[section];
}

bool section_pager_prepare(SectionPager *pager, size_t section,
                           int prefetch_direction,
                           SectionPagerSelection *selection)
{
    if (selection == NULL) return false;
    memset(selection, 0, sizeof(*selection));
    if (!pager_state_current(pager)
        || section >= pager->state_capacity) return false;
    *selection = (SectionPagerSelection) {
        .pager = pager,
        .budget = pager->store->budget,
        .section = section,
        .base_section = pager->current_section,
        .base_html = pager->current_html,
        .base_prefetched_html = pager->prefetched_html,
        .base_revision = pager->state_revision,
        .prefetch_direction = prefetch_direction,
        .active = true,
        .same_section = section == pager->current_section
    };
    if (selection->same_section) {
        selection->html = pager->current_html;
        selection->length = pager->current_length;
        return true;
    }

    if (pager->has_prefetch && pager->prefetched_section == section) {
        selection->html = pager->prefetched_html;
        selection->length = pager->prefetched_length;
        selection->uses_prefetch = true;
        return true;
    }

    uint64_t started = pager_now_us();
    if (!section_store_extract_html(pager->store, section,
                                    &selection->html,
                                    &selection->length)) {
        memset(selection, 0, sizeof(*selection));
        return false;
    }
    selection->decode_us = pager_now_us() - started;
    selection->owns_html = true;
    return true;
}

static bool selection_base_unchanged(const SectionPagerSelection *selection)
{
    const SectionPager *pager = selection->pager;
    return pager_state_current(pager)
        && pager->state_revision == selection->base_revision
        && selection->section < pager->state_capacity
        && pager->current_section == selection->base_section
        && pager->current_html == selection->base_html
        && pager->prefetched_html == selection->base_prefetched_html;
}

bool section_pager_commit(SectionPagerSelection *selection)
{
    if (selection == NULL || !selection->active
        || !selection_base_unchanged(selection)) return false;
    SectionPager *pager = selection->pager;
    pager->last_decode_us = 0;
    pager->last_select_prefetch_hit = false;
    if (!selection->same_section) {
        if (selection->prefetch_direction != 0
            && selection->prefetch_direction == pager->last_move_direction) {
            pager->sequential_moves++;
        } else {
            pager->sequential_moves = selection->prefetch_direction == 0
                ? 0 : 1;
        }
        pager->last_move_direction = selection->prefetch_direction;
        if (selection->uses_prefetch) {
            pager->prefetched_html = NULL;
            pager->prefetched_length = 0;
            pager->has_prefetch = false;
            pager->prefetch_hits++;
            pager->last_select_prefetch_hit = true;
        } else {
            /* Stale speculation remains intact until this commit point. */
            discard_prefetch(pager);
            pager_record_decode(pager, selection->decode_us);
        }
        budget_free(pager->store->budget, pager->current_html);
        pager->current_html = selection->html;
        pager->current_length = selection->length;
        pager->current_section = selection->section;
        pager->section_loads++;
        selection->owns_html = false;
    }
    pager->pending_prefetch_direction = selection->prefetch_direction;
    pager_bump_revision(pager);
    memset(selection, 0, sizeof(*selection));
    return true;
}

void section_pager_abort(SectionPagerSelection *selection)
{
    if (selection == NULL) return;
    if (selection->active && selection->owns_html)
        budget_free(selection->budget, selection->html);
    memset(selection, 0, sizeof(*selection));
}

bool section_pager_select(SectionPager *pager, size_t section,
                          int prefetch_direction)
{
    SectionPagerSelection selection;
    if (!section_pager_prepare(pager, section, prefetch_direction,
                               &selection)) return false;
    if (section_pager_commit(&selection)) return true;
    section_pager_abort(&selection);
    return false;
}

bool section_pager_move(SectionPager *pager, int direction)
{
    if (!pager_state_current(pager) || direction == 0) return false;
    if ((direction < 0 && pager->current_section == 0)
        || (direction > 0
            && pager->current_section + 1 >= pager->store->section_count))
        return false;
    size_t target = direction < 0
                    ? pager->current_section - 1 : pager->current_section + 1;
    return section_pager_select(pager, target, direction);
}

void section_pager_record_height(SectionPager *pager, int height)
{
    if (!pager_state_current(pager)) return;
    (void) section_pager_record_section_height(
        pager, pager->current_section, height);
}

bool section_pager_record_section_height(SectionPager *pager, size_t section,
                                         int height)
{
    if (!pager_state_current(pager) || pager->known_heights == NULL
        || section >= pager->state_capacity || height <= 0) return false;
    pager->known_heights[section] = height;
    return true;
}

bool section_pager_known_height(const SectionPager *pager, size_t section,
                                int *height)
{
    if (!pager_state_current(pager) || pager->known_heights == NULL
        || height == NULL || section >= pager->state_capacity
        || pager->known_heights[section] <= 0) return false;
    *height = pager->known_heights[section];
    return true;
}

bool section_pager_known_logical_offset(const SectionPager *pager,
                                        size_t section, size_t *offset)
{
    if (!pager_state_current(pager) || pager->known_heights == NULL
        || offset == NULL || section > pager->state_capacity) return false;
    size_t total = 0;
    for (size_t i = 0; i < section; i++) {
        int height = pager->known_heights[i];
        if (height <= 0 || (size_t) height > SIZE_MAX - total) return false;
        total += (size_t) height;
    }
    *offset = total;
    return true;
}

void section_pager_destroy(SectionPager *pager)
{
    if (pager == NULL) return;
    Budget *budget = pager->store == NULL ? NULL : pager->store->budget;
    if (budget != NULL) {
        if (pager->focus_ids != NULL) {
            for (size_t i = 0; i < pager->state_capacity; i++)
                budget_free(budget, pager->focus_ids[i]);
        }
        budget_free(budget, pager->current_html);
        budget_free(budget, pager->prefetched_html);
        budget_free(budget, pager->known_heights);
        budget_free(budget, pager->focus_ids);
    }
    memset(pager, 0, sizeof(*pager));
}
