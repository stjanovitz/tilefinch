#ifndef TILEFINCH_FRAME_LITE_H
#define TILEFINCH_FRAME_LITE_H

/* Lighter child-frame realms: the lazy-realm switch (validation only,
   default off) and the frame memory counters the memory report prints.
   Compiled only into host builds and PSP validation builds; a shipping PSP
   image defines none of this, so every guarded call site compiles to the
   shipping code there. With the switch at its default the behaviour equals
   the shipping build's.

   Host lab: environment variable TILEFINCH_FRAME_LITE_LAZY_REALM, read once
   on first use. PSP validation: boot.cfg key frame_lite_lazy_realm, parsed
   by psp_boot_config.c outside PspBootConfig.

     lazy_realm    1 = a fetched child document with nothing that can run
                   script or take a press (no executable script, no frame
                   element, no on* handler, no javascript: URL, no link or
                   form control) commits with no realm, like a sandboxed
                   frame without allow-scripts, and its snapshot is drawn
                   without one. Presses into such a frame are not
                   delivered, which is why it stays off. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define TILEFINCH_FRAME_LITE 1

typedef struct {
    bool lazy_realm;
} TilefinchFrameLiteSwitches;

typedef struct {
    /* Child documents committed without a realm by lazy_realm. */
    size_t realms_skipped;
    /* Child realms created (any switch state), and the time spent in
       runtime creation (bootstrap included) for them. */
    size_t realms_created;
    uint64_t realm_create_us;
    /* Child-realm pool trims (script_runtime_trim_pool) and the bytes they
       returned. */
    size_t pool_trims;
    size_t pool_trim_bytes;
    /* DOM handle table doublings in every realm, their total and longest
       time. */
    size_t handle_growths;
    uint64_t handle_growth_us;
    uint64_t handle_growth_max_us;
    /* Snapshots not drawn because the frame's element has no box, and
       mutation journals allocated (each realm allocates its own on its
       first connected mutation). */
    size_t hidden_snapshots_skipped;
    size_t mutation_journals;
} TilefinchFrameLiteCounters;

const TilefinchFrameLiteSwitches *tilefinch_frame_lite(void);
/* Sets one switch by its key without the frame_lite_ prefix; returns false
   for an unknown key. */
bool tilefinch_frame_lite_set(const char *key, const char *value);
void tilefinch_frame_lite_reset(void);
void tilefinch_frame_lite_restore(const TilefinchFrameLiteSwitches *saved);
extern TilefinchFrameLiteCounters tilefinch_frame_lite_counters;

typedef int (*TilefinchFrameLitePrint)(const char *format, ...);
void tilefinch_frame_lite_print(TilefinchFrameLitePrint print,
                                const char *phase);
#endif

#endif
