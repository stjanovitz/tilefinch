#ifndef TILEFINCH_UPDATE_HISTORY_H
#define TILEFINCH_UPDATE_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

#define TILEFINCH_UPDATE_HISTORY_LIMIT 8u
#define TILEFINCH_UPDATE_HISTORY_VERSION_CAPACITY 16u

typedef enum {
    TILEFINCH_UPDATE_HISTORY_IDLE = 0,
    TILEFINCH_UPDATE_HISTORY_LOADING,
    TILEFINCH_UPDATE_HISTORY_READY,
    TILEFINCH_UPDATE_HISTORY_ERROR
} TilefinchUpdateHistoryPhase;

typedef struct TilefinchUpdateHistory TilefinchUpdateHistory;

typedef struct {
    TilefinchUpdateHistoryPhase phase;
    size_t count;
    char versions[TILEFINCH_UPDATE_HISTORY_LIMIT]
                 [TILEFINCH_UPDATE_HISTORY_VERSION_CAPACITY];
    char message[64];
} TilefinchUpdateHistorySnapshot;

/* The catalog exists only while the user explicitly opens Previous versions.
   It queries GitHub into a bounded RAM response and never persists results. */
TilefinchUpdateHistory *tilefinch_update_history_create(
    Budget *budget, const char *repository_owner,
    const char *repository_name, const char *current_version);
void tilefinch_update_history_destroy(TilefinchUpdateHistory *history);
bool tilefinch_update_history_begin(TilefinchUpdateHistory *history);
bool tilefinch_update_history_pump(
    TilefinchUpdateHistory *history, uint64_t maximum_time_us);
bool tilefinch_update_history_cancel(TilefinchUpdateHistory *history);
bool tilefinch_update_history_snapshot(
    const TilefinchUpdateHistory *history,
    TilefinchUpdateHistorySnapshot *snapshot);
bool tilefinch_update_history_url(
    const char *repository_owner, const char *repository_name,
    char *output, size_t capacity);

/* Pure bounded parser used by host fixtures and the live query. Releases are
   accepted only when they are older stable semantic versions and advertise
   the signed browser metadata asset. */
bool tilefinch_update_history_parse(
    const unsigned char *json, size_t length, const char *current_version,
    TilefinchUpdateHistorySnapshot *snapshot);
bool tilefinch_update_history_tag(
    const TilefinchUpdateHistorySnapshot *snapshot, size_t index,
    char *output, size_t capacity);

#endif
