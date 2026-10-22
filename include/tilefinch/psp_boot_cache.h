#ifndef TILEFINCH_PSP_BOOT_CACHE_H
#define TILEFINCH_PSP_BOOT_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PSP_BOOT_IMAGE_MAX_BYTES (16u * 1024u * 1024u)

typedef enum {
    PSP_BOOT_CACHE_UNAVAILABLE,
    PSP_BOOT_CACHE_REUSED,
    PSP_BOOT_CACHE_CREATED,
    PSP_BOOT_CACHE_PENDING
} PspBootCacheResult;

/* Extract only a bounded relocatable DATA.PSP from a normal PBP. Static
   legacy EBOOTs decline. Outputs remain untouched on malformed input. */
bool psp_boot_pbp_payload(const uint8_t header[40], size_t bytes,
                          uint32_t file_size, uint32_t *offset, uint32_t *size);

/* The authoritative PBP remains unchanged. Creation/adoption compares all module
   bytes with DATA.PSP and saves a bounded .receipt reuse hint (after a new copy,
   only once a Memory Stick flush is accepted). Subsequent boots
   trust the selected path and unchanged file metadata, not a new byte/hash scan.
   Same-metadata corruption is an accepted safe-mode recovery case, not detected
   by this optimization. Partial temporaries are never executed. Caller-owned
   8–128 KiB scratch, no writes on a hit, failures permit LoadExec. */
typedef bool (*PspBootCacheSpaceQuery)(void *context, uint64_t *available);
/* Called once before potentially slow comparison/copy work, never on a
   matching receipt or an invalid/legacy package. Shares query_space context. */
typedef void (*PspBootCacheWorkStarted)(void *context);
PspBootCacheResult psp_boot_cache_prepare(
    const char *source, const char *cache, const char *temporary,
    void *scratch, size_t scratch_bytes,
    PspBootCacheSpaceQuery query_space, PspBootCacheWorkStarted work_started,
    void *context);

/* The one startup copy in the install's data directory: the module and its
   temporary sibling (the receipt is the module path plus ".receipt"). The
   launcher prepares it at boot and the updater after promotion. */
#define PSP_BOOT_CACHE_MODULE_NAME "startup-module.prx"
#define PSP_BOOT_CACHE_TEMPORARY_NAME "startup-module.tmp"
bool psp_boot_cache_paths(const char *data_directory, char *cache,
                          char *temporary, size_t size);

/* An explicitly requested application update reclaims this optional copy
   before its free-space check. Never touches slots, journals or user files. */
void psp_boot_cache_discard(const char *data_directory);

/* A fresh install trusts its packaged executables just as it trusts the root
   launcher. The launcher embeds the module's content-addressed filename and
   bundle marker; this checks pairing/format, not a per-launch content hash.
   Never use this shortcut for journalled slots or recovery. */
bool psp_boot_prepackaged_module(
    const char *source, const char *module, uint32_t package_bytes,
    uint32_t module_bytes, const uint8_t bundle_digest[32]);

/* Reclaim only the exact bundled duplicate paired with this slot's PBP.
   Called after update approval; leaves the authoritative PBP and other files. */
bool psp_boot_prepackaged_discard(const char *slot_directory);

/* Optional installer work, after the signed slot is promoted. Paths must stay
   alive until close. Each step copies or compares at most scratch_bytes / 2
   bytes, with 8–128 KiB caller-owned scratch. No partial file is executable;
   refusal must not invalidate the successfully installed update. */
typedef struct {
    const char *source_path, *cache_path, *temporary_path;
    FILE *source, *file;
    uint64_t source_identity[5];
    uint32_t begin, size, remaining;
    unsigned phase;
    PspBootCacheResult result;
} PspBootCacheCopy;
PspBootCacheResult psp_boot_cache_copy_begin(
    PspBootCacheCopy *copy, const char *source, const char *cache,
    const char *temporary, PspBootCacheSpaceQuery query_space, void *context);
PspBootCacheResult psp_boot_cache_copy_step(
    PspBootCacheCopy *copy, void *scratch, size_t scratch_bytes, size_t maximum_bytes);
void psp_boot_cache_copy_close(PspBootCacheCopy *copy);

#endif
