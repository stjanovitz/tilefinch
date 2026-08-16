#ifndef TILEFINCH_COMPILER_H
#define TILEFINCH_COMPILER_H

/*
 * Deliberate code-layout boundaries for the PSP's small instruction cache.
 *
 * COLD_PATH is reserved for once-per-process, failure, and diagnostic work.
 * OUT_OF_LINE is for mode-specific work that is common during a session but
 * not on every frame. `noinline` is intentional: GCC's -Os otherwise folds
 * single-caller helpers back into the ratcheted hot symbol.
 */
#if defined(__GNUC__)
#define TILEFINCH_COLD_PATH __attribute__((noinline, cold))
#define TILEFINCH_OUT_OF_LINE __attribute__((noinline))
/* A named hot boundary must survive interprocedural cloning so the final-ELF
   symbol ratchet keeps measuring the code the frame loop actually runs. */
#define TILEFINCH_HOT_BOUNDARY __attribute__((noinline, noclone))
#else
#define TILEFINCH_COLD_PATH
#define TILEFINCH_OUT_OF_LINE
#define TILEFINCH_HOT_BOUNDARY
#endif

#endif
