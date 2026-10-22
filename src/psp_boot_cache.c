#include "tilefinch/psp_boot_cache.h"
#include "tilefinch/psp_boot_handoff.h"
#include "tilefinch/install_paths.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* This is a reuse hint, not an integrity/authenticity record. In particular,
   same-size corruption with preserved metadata requires safe-mode recovery. */
typedef struct {
    uint64_t size, modified, changed, inode, device;
} CacheFileIdentity;

typedef struct {
    uint32_t version, bytes;
    CacheFileIdentity source, cache;
    char source_path[TILEFINCH_INSTALL_PATH_LIMIT];
} CacheReceipt;

_Static_assert(sizeof(CacheReceipt) < 1024u, "startup reuse receipt stays bounded");

static bool file_identity(const char *path, CacheFileIdentity *identity)
{
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_size <= 0 || (uint64_t)info.st_size > PSP_BOOT_IMAGE_MAX_BYTES)
        return false;
    *identity = (CacheFileIdentity) {
        (uint64_t)info.st_size, (uint64_t)info.st_mtime,
        (uint64_t)info.st_ctime, (uint64_t)info.st_ino, (uint64_t)info.st_dev
    };
    return true;
}

static bool receipt_identity(const char *source, const char *cache, CacheReceipt *receipt)
{
    size_t length = 0;
    while (length < sizeof(receipt->source_path) && source[length]) length++;
    if (length == sizeof(receipt->source_path)) return false;
    memset(receipt, 0, sizeof(*receipt));
    receipt->version = 1;
    receipt->bytes = sizeof(*receipt);
    memcpy(receipt->source_path, source, length + 1u);
    return file_identity(source, &receipt->source) && file_identity(cache, &receipt->cache);
}

static uint32_t little32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8u
        | (uint32_t)p[2] << 16u | (uint32_t)p[3] << 24u;
}

static FILE *open_read(const char *path)
{
    FILE *file = fopen(path, "rb");
#ifdef __PSP__
    /* Newlib's buffered fread refills its small FILE buffer even for these
       large caller-owned blocks. Its unbuffered path reads directly into
       our bounded scratch instead, avoiding thousands of Memory Stick
       operations and an extra copy without skipping any verification. */
    if (file != NULL && setvbuf(file, NULL, _IONBF, 0) != 0) {
        (void)fclose(file);
        return NULL;
    }
#endif
    return file;
}

bool psp_boot_cache_paths(const char *directory, char *cache,
                          char *temporary, size_t size)
{
    if (directory == NULL || cache == NULL || temporary == NULL) return false;
    int cache_length = snprintf(cache, size, "%s/%s", directory,
                                PSP_BOOT_CACHE_MODULE_NAME);
    int temporary_length = snprintf(temporary, size, "%s/%s", directory,
                                    PSP_BOOT_CACHE_TEMPORARY_NAME);
    return cache_length > 0 && (size_t)cache_length < size
        && temporary_length > 0 && (size_t)temporary_length < size;
}

void psp_boot_cache_discard(const char *directory)
{
    if (directory == NULL || directory[0] == '\0') return;
    static const char *const names[] = {
        PSP_BOOT_CACHE_MODULE_NAME, PSP_BOOT_CACHE_TEMPORARY_NAME,
        PSP_BOOT_CACHE_MODULE_NAME ".receipt"
    };
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    for (unsigned at = 0; at < sizeof(names) / sizeof(names[0]); at++) {
        int count = snprintf(path, sizeof(path), "%s/%s", directory, names[at]);
        if (count > 0 && (size_t)count < sizeof(path)) (void)remove(path);
    }
}

bool psp_boot_pbp_payload(const uint8_t *h, size_t n, uint32_t file_size,
                          uint32_t *offset, uint32_t *size)
{
    if (h == NULL || n < 40u || offset == NULL || size == NULL
        || file_size < 60u || file_size > PSP_BOOT_IMAGE_MAX_BYTES
        || memcmp(h, "\0PBP\0\0\1\0", 8u) != 0) return false;
    uint32_t previous = 40u;
    for (unsigned at = 8u; at < 40u; at += 4u) {
        uint32_t next = little32(h + at);
        if (next < previous || next > file_size) return false;
        previous = next;
    }
    uint32_t begin = little32(h + 32u), end = little32(h + 36u);
    if (end - begin < 20u) return false;
    *offset = begin;
    *size = end - begin;
    return true;
}

/* Positions source at its relocatable DATA.PSP module, given the PBP's
   size; false for a legacy static EBOOT or malformed package. */
static bool source_payload(FILE *source, uint32_t file_size,
                           uint32_t *begin, uint32_t *size)
{
    uint8_t header[40], module[20];
    return fseek(source, 0, SEEK_SET) == 0
        && fread(header, 1, sizeof(header), source) == sizeof(header)
        && psp_boot_pbp_payload(header, sizeof(header), file_size, begin, size)
        && fseek(source, (long)*begin, SEEK_SET) == 0
        && fread(module, 1, sizeof(module), source) == sizeof(module)
        && psp_boot_module_format(module, sizeof(module))
        && fseek(source, (long)*begin, SEEK_SET) == 0;
}

static bool module_equal(FILE *source, FILE *cache, uint32_t size,
                          uint8_t *a, uint8_t *b, size_t capacity)
{
    for (uint32_t remaining = size; remaining != 0u;) {
        size_t count = remaining < capacity ? remaining : capacity;
        if (fread(a, 1u, count, source) != count
            || fread(b, 1u, count, cache) != count
            || memcmp(a, b, count) != 0) return false;
        remaining -= (uint32_t)count;
    }
    return fgetc(cache) == EOF && !ferror(cache);
}

static bool receipt_matches(const char *path, const CacheReceipt *expected)
{
    FILE *file = open_read(path);
    if (file == NULL) return false;
    CacheReceipt actual;
    bool matches = fread(&actual, 1, sizeof(actual), file) == sizeof(actual)
        && fgetc(file) == EOF && !ferror(file)
        && memcmp(&actual, expected, sizeof(actual)) == 0;
    return fclose(file) == 0 && matches;
}

static void receipt_save(const char *path, const CacheReceipt *receipt)
{
    /* A torn record cannot match its fixed length and freshly obtained identity.
       Failure only loses the optimization; this boot already verified bytes. */
    FILE *file = fopen(path, "wb");
    if (file == NULL) return;
    bool written = fwrite(receipt, 1, sizeof(*receipt), file) == sizeof(*receipt);
    if (fclose(file) != 0) written = false;
    if (!written) (void)remove(path);
}

bool psp_boot_prepackaged_module(
    const char *source, const char *module, uint32_t package_bytes,
    uint32_t module_bytes, const uint8_t bundle_digest[32])
{
    CacheFileIdentity a, b;
    if (source == NULL || module == NULL || bundle_digest == NULL
        || strcmp(source, module) == 0 || !file_identity(source, &a)
        || !file_identity(module, &b) || a.size != package_bytes
        || b.size != module_bytes) return false;
    FILE *file = open_read(source);
    if (file == NULL) return false;
    uint8_t header[40], marker[40], source_header[20], module_header[20];
    uint32_t offset, size;
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header)
        && psp_boot_pbp_payload(header, sizeof(header), package_bytes, &offset, &size)
        && size == module_bytes && offset <= package_bytes
        && size <= package_bytes - offset
        && package_bytes - offset - size == sizeof(marker)
        && fseek(file, (long)offset, SEEK_SET) == 0
        && fread(source_header, 1, sizeof(source_header), file) == sizeof(source_header)
        && psp_boot_module_format(source_header, sizeof(source_header))
        && fseek(file, (long)(offset + size), SEEK_SET) == 0
        && fread(marker, 1, sizeof(marker), file) == sizeof(marker)
        && memcmp(marker, "TFBOOT01", 8) == 0
        && memcmp(marker + 8, bundle_digest, 32) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    file = open_read(module);
    if (file == NULL) return false;
    ok = fread(module_header, 1, sizeof(module_header), file) == sizeof(module_header)
        && memcmp(source_header, module_header, sizeof(module_header)) == 0;
    return fclose(file) == 0 && ok;
}

bool psp_boot_prepackaged_discard(const char *directory)
{
    if (directory == NULL || directory[0] == '\0') return false;
    char source[TILEFINCH_INSTALL_PATH_LIMIT], module[TILEFINCH_INSTALL_PATH_LIMIT];
    int n = snprintf(source, sizeof(source), "%s/EBOOT.PBP", directory);
    CacheFileIdentity identity;
    if (n <= 0 || (size_t)n >= sizeof(source) || !file_identity(source, &identity))
        return false;
    FILE *file = open_read(source);
    if (file == NULL) return false;
    uint8_t header[40], marker[40];
    uint32_t offset, size;
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header)
        && psp_boot_pbp_payload(header, sizeof(header), (uint32_t)identity.size, &offset, &size)
        && identity.size - offset - size == sizeof(marker)
        && fseek(file, (long)(offset + size), SEEK_SET) == 0
        && fread(marker, 1, sizeof(marker), file) == sizeof(marker)
        && memcmp(marker, "TFBOOT01", 8) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    n = snprintf(module, sizeof(module), "%s/startup-", directory);
    if (n <= 0 || (size_t)n > sizeof(module) - 69u) return false;
    static const char hex[] = "0123456789abcdef";
    for (unsigned at = 0; at < 32u; at++) {
        module[n + at * 2u] = hex[marker[8u + at] >> 4u];
        module[n + at * 2u + 1u] = hex[marker[8u + at] & 15u];
    }
    memcpy(module + n + 64u, ".prx", 5u);
    return psp_boot_prepackaged_module(source, module, (uint32_t)identity.size, size, marker + 8u)
        && remove(module) == 0;
}

void psp_boot_cache_copy_close(PspBootCacheCopy *copy)
{
    if (copy == NULL) return;
    if (copy->file != NULL) (void)fclose(copy->file);
    if (copy->source != NULL) (void)fclose(copy->source);
    if (copy->temporary_path != NULL) (void)remove(copy->temporary_path);
    memset(copy, 0, sizeof(*copy));
}

static PspBootCacheResult copy_failed(PspBootCacheCopy *copy)
{
    psp_boot_cache_copy_close(copy);
    return PSP_BOOT_CACHE_UNAVAILABLE;
}

PspBootCacheResult psp_boot_cache_copy_begin(
    PspBootCacheCopy *copy, const char *source, const char *cache,
    const char *temporary, PspBootCacheSpaceQuery query_space, void *context)
{
    if (copy == NULL) return PSP_BOOT_CACHE_UNAVAILABLE;
    memset(copy, 0, sizeof(*copy));
    char receipt_path[TILEFINCH_INSTALL_PATH_LIMIT];
    int n = cache == NULL ? -1 : snprintf(receipt_path, sizeof(receipt_path), "%s.receipt", cache);
    if (source == NULL || cache == NULL || temporary == NULL || query_space == NULL
        || n <= 0 || (size_t)n >= sizeof(receipt_path)
        || strcmp(source, cache) == 0 || strcmp(source, temporary) == 0
        || strcmp(cache, temporary) == 0 || strcmp(source, receipt_path) == 0
        || strcmp(temporary, receipt_path) == 0) return PSP_BOOT_CACHE_UNAVAILABLE;
    CacheFileIdentity identity;
    if (!file_identity(source, &identity)) return PSP_BOOT_CACHE_UNAVAILABLE;
    _Static_assert(sizeof(identity) == sizeof(copy->source_identity), "identity storage");
    memcpy(copy->source_identity, &identity, sizeof(identity));
    copy->source_path = source;
    copy->cache_path = cache;
    copy->temporary_path = temporary;
    copy->source = open_read(source);
    if (copy->source == NULL) return copy_failed(copy);
    uint64_t available = 0;
    if (!source_payload(copy->source, (uint32_t)identity.size,
                        &copy->begin, &copy->size)
        || !query_space(context, &available) || copy->size > available
        || 65536u > available - copy->size) return copy_failed(copy);
    copy->file = fopen(temporary, "wb");
    if (copy->file == NULL) return copy_failed(copy);
    /* Invalidate before any replacement. An interrupted copy never authorizes
       its old receipt, and the previously selected slot remains authoritative. */
    (void)remove(receipt_path);
    copy->remaining = copy->size;
    copy->result = PSP_BOOT_CACHE_PENDING;
    return copy->result;
}

PspBootCacheResult psp_boot_cache_copy_step(
    PspBootCacheCopy *copy, void *scratch, size_t scratch_bytes, size_t maximum_bytes)
{
    if (copy == NULL) return PSP_BOOT_CACHE_UNAVAILABLE;
    if (copy->result != PSP_BOOT_CACHE_PENDING) return copy->result;
    if (scratch == NULL || scratch_bytes < 8192u || scratch_bytes > 131072u)
        return copy_failed(copy);
    uint8_t *a = scratch, *b = a + scratch_bytes / 2u;
    size_t count = copy->remaining < scratch_bytes / 2u
        ? copy->remaining : scratch_bytes / 2u;
    if (maximum_bytes != 0 && count > maximum_bytes) count = maximum_bytes;
    if (count != 0) {
        if (fread(a, 1, count, copy->source) != count
            || (copy->phase == 0 ? fwrite(a, 1, count, copy->file) != count
                : fread(b, 1, count, copy->file) != count || memcmp(a, b, count) != 0))
            return copy_failed(copy);
        copy->remaining -= (uint32_t)count;
        return PSP_BOOT_CACHE_PENDING;
    }
    if (copy->phase == 0) {
        int closed = fclose(copy->file);
        copy->file = NULL;
        if (closed != 0 || fseek(copy->source, (long)copy->begin, SEEK_SET) != 0)
            return copy_failed(copy);
        copy->file = open_read(copy->temporary_path);
        if (copy->file == NULL) return copy_failed(copy);
        copy->phase = 1;
        copy->remaining = copy->size;
        return PSP_BOOT_CACHE_PENDING;
    }
    bool ok = fgetc(copy->file) == EOF && !ferror(copy->file);
    if (fclose(copy->file) != 0) ok = false;
    copy->file = NULL;
    if (fclose(copy->source) != 0) ok = false;
    copy->source = NULL;
    CacheFileIdentity identity;
    if (!ok || !file_identity(copy->source_path, &identity)
        || memcmp(copy->source_identity, &identity, sizeof(identity)) != 0)
        return copy_failed(copy);
    (void)remove(copy->cache_path);
    if (rename(copy->temporary_path, copy->cache_path) != 0) return copy_failed(copy);
    char receipt_path[TILEFINCH_INSTALL_PATH_LIMIT];
    CacheReceipt receipt;
    int n = snprintf(receipt_path, sizeof(receipt_path), "%s.receipt", copy->cache_path);
    if (n <= 0 || (size_t)n >= sizeof(receipt_path)
        || !receipt_identity(copy->source_path, copy->cache_path, &receipt)
        || memcmp(copy->source_identity, &receipt.source, sizeof(receipt.source)) != 0) {
        (void)remove(copy->cache_path);
        return copy_failed(copy);
    }
    /* The receipt lets later boots trust the copy by metadata alone, so the
       copy must reach the card first. Where the firmware rejects the flush,
       no receipt is written: the next boot's byte comparison adopts the
       copy instead (psp_boot_cache_prepare). */
    if (tilefinch_memory_stick_sync() != TILEFINCH_MEMORY_STICK_SYNC_REJECTED)
        receipt_save(receipt_path, &receipt);
    copy->result = PSP_BOOT_CACHE_CREATED;
    return copy->result;
}

PspBootCacheResult psp_boot_cache_prepare(
    const char *source_path, const char *cache_path, const char *temporary_path,
    void *scratch, size_t scratch_bytes,
    PspBootCacheSpaceQuery query_space, PspBootCacheWorkStarted work_started,
    void *context)
{
    if (source_path == NULL || cache_path == NULL || temporary_path == NULL
        || scratch == NULL || scratch_bytes < 8192u || scratch_bytes > 131072u
        || query_space == NULL
        || strcmp(source_path, cache_path) == 0
        || strcmp(source_path, temporary_path) == 0
        || strcmp(cache_path, temporary_path) == 0) return PSP_BOOT_CACHE_UNAVAILABLE;
    char receipt_path[TILEFINCH_INSTALL_PATH_LIMIT];
    int receipt_length = snprintf(receipt_path, sizeof(receipt_path), "%s.receipt", cache_path);
    if (receipt_length <= 0 || (size_t)receipt_length >= sizeof(receipt_path)
        || strcmp(source_path, receipt_path) == 0
        || strcmp(temporary_path, receipt_path) == 0) return PSP_BOOT_CACHE_UNAVAILABLE;
    CacheReceipt identity;
    if (receipt_identity(source_path, cache_path, &identity)
        && receipt_matches(receipt_path, &identity)) return PSP_BOOT_CACHE_REUSED;
    /* Invalidate before replacement: a failed copy must never leave an old hint
       authorizing newly written data. The updater also discards this record. */
    (void)remove(receipt_path);
    FILE *source = open_read(source_path);
    if (source == NULL) return PSP_BOOT_CACHE_UNAVAILABLE;
    CacheFileIdentity original_source;
    uint32_t begin, size;
    bool valid = file_identity(source_path, &original_source)
        && source_payload(source, (uint32_t)original_source.size, &begin, &size);
    if (valid && work_started != NULL) work_started(context);
    /* A matching existing copy is adopted without a free-space query (FAT
       queries can be expensive) or a rewrite. */
    bool equal = false;
    FILE *cache = valid ? open_read(cache_path) : NULL;
    if (cache != NULL) {
        size_t capacity = scratch_bytes / 2u;
        uint8_t *bytes = scratch;
        equal = module_equal(source, cache, size, bytes, bytes + capacity, capacity);
        if (fclose(cache) != 0) equal = false;
    }
    if (fclose(source) != 0) return PSP_BOOT_CACHE_UNAVAILABLE;
    if (!valid) return PSP_BOOT_CACHE_UNAVAILABLE;
    if (equal) {
        if (receipt_identity(source_path, cache_path, &identity)
            && memcmp(&original_source, &identity.source, sizeof(original_source)) == 0)
            receipt_save(receipt_path, &identity);
        return PSP_BOOT_CACHE_REUSED;
    }
    /* Otherwise the same incremental copy the updater runs, to completion. */
    PspBootCacheCopy copy;
    if (psp_boot_cache_copy_begin(&copy, source_path, cache_path, temporary_path,
                                  query_space, context) != PSP_BOOT_CACHE_PENDING)
        return PSP_BOOT_CACHE_UNAVAILABLE;
    while (psp_boot_cache_copy_step(&copy, scratch, scratch_bytes, 0)
           == PSP_BOOT_CACHE_PENDING) {}
    return copy.result;
}
