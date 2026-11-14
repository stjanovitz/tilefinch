/* The lazy-realm switch and frame memory counters
   (include/tilefinch/frame_lite.h).
   Host builds and PSP validation builds only; a shipping PSP image compiles
   this translation unit to nothing. */

#include "tilefinch/frame_lite.h"

#if defined(TILEFINCH_FRAME_LITE)

#include <stdlib.h>
#include <string.h>

#define FRAME_LITE_DEFAULTS { false }

static TilefinchFrameLiteSwitches frame_lite_switches = FRAME_LITE_DEFAULTS;
TilefinchFrameLiteCounters tilefinch_frame_lite_counters;

#if !defined(__PSP__)
static bool frame_lite_environment_read;

static void frame_lite_read_environment(void)
{
    static const struct { const char *variable, *key; } keys[] = {
        {"TILEFINCH_FRAME_LITE_LAZY_REALM", "lazy_realm"},
    };
    frame_lite_environment_read = true;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *value = getenv(keys[i].variable);
        if (value != NULL && value[0] != '\0')
            (void) tilefinch_frame_lite_set(keys[i].key, value);
    }
}
#endif

const TilefinchFrameLiteSwitches *tilefinch_frame_lite(void)
{
#if !defined(__PSP__)
    if (!frame_lite_environment_read) frame_lite_read_environment();
#endif
    return &frame_lite_switches;
}

bool tilefinch_frame_lite_set(const char *key, const char *value)
{
    if (key == NULL || value == NULL) return false;
    TilefinchFrameLiteSwitches *s = &frame_lite_switches;
    if (strcmp(key, "lazy_realm") != 0) return false;
    s->lazy_realm = strtol(value, NULL, 10) > 0;
    return true;
}

void tilefinch_frame_lite_reset(void)
{
    static const TilefinchFrameLiteSwitches defaults = FRAME_LITE_DEFAULTS;
    frame_lite_switches = defaults;
}

void tilefinch_frame_lite_restore(const TilefinchFrameLiteSwitches *saved)
{
    if (saved != NULL) frame_lite_switches = *saved;
}

void tilefinch_frame_lite_print(TilefinchFrameLitePrint print,
                                const char *phase)
{
    if (print == NULL) return;
    const TilefinchFrameLiteSwitches *s = tilefinch_frame_lite();
    const TilefinchFrameLiteCounters *c = &tilefinch_frame_lite_counters;
    print("tilefinch-frame-lite: phase=%s lazy_realm=%d "
          "realms-skipped=%zu realms-created=%zu realm-create-us=%llu "
          "pool-trims=%zu/%zu handle-growths=%zu/%llu/%llu "
          "hidden-snapshots-skipped=%zu mutation-journals=%zu\n",
          phase == NULL ? "unknown" : phase, s->lazy_realm ? 1 : 0,
          c->realms_skipped, c->realms_created,
          (unsigned long long) c->realm_create_us, c->pool_trims,
          c->pool_trim_bytes, c->handle_growths,
          (unsigned long long) c->handle_growth_us,
          (unsigned long long) c->handle_growth_max_us,
          c->hidden_snapshots_skipped, c->mutation_journals);
}

#else
typedef int tilefinch_frame_lite_unused;
#endif
