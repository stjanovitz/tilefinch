#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "tilefinch/psp_boot_handoff.h"
#include "tilefinch/psp_boot_cache.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

/* Inject a source replacement at the promotion boundary, without a
   production-only test hook or a timing-dependent racing thread. */
static const char *replace_source_after_rename;
static int cache_test_rename(const char *from, const char *to)
{
    int result = rename(from, to);
    if (result == 0 && replace_source_after_rename != NULL) {
        FILE *file = fopen(replace_source_after_rename, "r+b");
        struct stat info;
        if (file == NULL || fseek(file, 64, SEEK_SET) != 0
            || fputc(7, file) == EOF || fclose(file) != 0
            || stat(replace_source_after_rename, &info) != 0) abort();
        struct utimbuf changed = {info.st_atime, info.st_mtime + 10};
        if (utime(replace_source_after_rename, &changed) != 0) abort();
        replace_source_after_rename = NULL;
    }
    return result;
}
#ifdef TILEFINCH_BOOT_CACHE_TEST_STDIO
#define fopen traced_open
#define fclose traced_close
#define setvbuf traced_setup
#define fread traced_read
#endif
#define rename cache_test_rename
#include "../src/psp_boot_cache.c"
#undef rename
#ifdef TILEFINCH_BOOT_CACHE_TEST_STDIO
#undef fopen
#undef fclose
#undef setvbuf
#undef fread
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static int test_arguments(void)
{
    const PspBootModuleArgument input = {1, 2, 3, 0x7fffffff};
    char text[PSP_BOOT_MODULE_ARGUMENT_BYTES + 1u];
    memset(text, 'X', sizeof(text));
    CHECK(!psp_boot_module_encode(text, PSP_BOOT_MODULE_ARGUMENT_BYTES - 1u, &input));
    CHECK(text[0] == 'X');
    CHECK(psp_boot_module_encode(text, PSP_BOOT_MODULE_ARGUMENT_BYTES, &input));
    CHECK(strlen(text) + 1u == PSP_BOOT_MODULE_ARGUMENT_BYTES);
    CHECK(text[PSP_BOOT_MODULE_ARGUMENT_BYTES] == 'X');
    PspBootModuleArgument output = {0};
    CHECK(psp_boot_module_decode(text, &output));
    CHECK(memcmp(&input, &output, sizeof(input)) == 0);
    for (size_t at = 0; at < strlen(text); at++) {
        char bad[sizeof(text)];
        memcpy(bad, text, sizeof(bad));
        bad[at] = '\0';
        PspBootModuleArgument before = output;
        CHECK(!psp_boot_module_decode(bad, &output));
        CHECK(memcmp(&before, &output, sizeof(before)) == 0);
        bad[at] = '!';
        CHECK(!psp_boot_module_decode(bad, &output));
    }
    text[sizeof(text) - 2u] = 'x'; text[sizeof(text) - 1u] = '\0';
    CHECK(!psp_boot_module_decode(text, &output));
    CHECK(!psp_boot_module_decode(NULL, &output));
    CHECK(!psp_boot_module_encode(NULL, sizeof(text), &input));
    const PspBootModuleArgument invalid[] = {
        {0, 2, 3, 4}, {-1, 2, 3, 4}, {1, 1, 3, 4}, {1, 2, 3, 3},
        {1, 0, 3, 4}, {1, 2, 0, 4}, {1, 2, 3, 0}
    };
    for (size_t at = 0; at < sizeof(invalid) / sizeof(invalid[0]); at++)
        CHECK(!psp_boot_module_encode(text, sizeof(text), &invalid[at]));
    CHECK(!psp_boot_module_decode(PSP_BOOT_MODULE_PREFIX "80000000,00000002,00000003,00000004", &output));
    return 0;
}

typedef struct { int fail_at, count, invalid, absent; char order[9]; } Fake;
static int record(Fake *f, char step, int id, int expected)
{
    if (id != expected || f->count >= 8) { f->invalid++; return -1; }
    f->order[f->count] = step;
    return f->count++ == f->fail_at ? -7 : 0;
}
static int signal_ready(void *p, int32_t id) { return record(p, 'R', id, 2); }
static int wait_gate(void *p, int32_t id, uint32_t us)
{ Fake *f = p; if (us != PSP_BOOT_MODULE_WAIT_US) f->invalid++; return record(f, 'G', id, 1); }
static int join_thread(void *p, int32_t id, uint32_t us)
{ Fake *f = p; if (us != PSP_BOOT_MODULE_JOIN_US) f->invalid++; return record(f, 'J', id, 4); }
static int delete_thread(void *p, int32_t id) { return record(p, 'T', id, 4); }
static int delete_gate(void *p, int32_t id) { return record(p, 'S', id, 1); }
static int stop_module(void *p, int32_t id) { return record(p, 'P', id, 3); }
static int unload_module(void *p, int32_t id)
{ int result = record(p, 'U', id, 3); return result < 0 ? result : 77; }
static int absent(void *p, int32_t id)
{ Fake *f = p; int result = record(f, 'A', id, 3); return result < 0 ? result : f->absent; }

static int test_retirement(void)
{
    const PspBootModuleArgument argument = {1, 2, 3, 4};
    Fake fake = {.fail_at = -1, .absent = 1};
    PspBootModuleOps ops = {&fake, signal_ready, wait_gate, join_thread,
        delete_thread, delete_gate, stop_module, unload_module, absent};
    CHECK(psp_boot_module_receive(&argument, &ops) == PSP_BOOT_MODULE_COMPLETE);
    CHECK(fake.count == 8 && fake.invalid == 0);
    CHECK(memcmp(fake.order, "RGJTSPUA", 8) == 0);
    const PspBootModuleResult failures[] = {
        PSP_BOOT_MODULE_READY_FAILED, PSP_BOOT_MODULE_GATE_FAILED,
        PSP_BOOT_MODULE_JOIN_FAILED, PSP_BOOT_MODULE_THREAD_FAILED,
        PSP_BOOT_MODULE_SEMAPHORE_FAILED, PSP_BOOT_MODULE_STOP_FAILED,
        PSP_BOOT_MODULE_UNLOAD_FAILED, PSP_BOOT_MODULE_STILL_PRESENT
    };
    for (int at = 0; at < 8; at++) {
        fake = (Fake){.fail_at = at, .absent = 1};
        CHECK(psp_boot_module_receive(&argument, &ops) == failures[at]);
        CHECK(psp_boot_module_receiver_owns_fallback(failures[at]) == (at >= 2));
        CHECK(fake.count == at + 1 && fake.invalid == 0);
        CHECK(memcmp(fake.order, "RGJTSPUA", (size_t)fake.count) == 0);
    }
    fake = (Fake){.fail_at = -1, .absent = 0};
    CHECK(psp_boot_module_receive(&argument, &ops) == PSP_BOOT_MODULE_STILL_PRESENT);
    fake = (Fake){.fail_at = -1, .absent = 1};
    ops.join_thread = NULL;
    CHECK(psp_boot_module_receive(&argument, &ops) == PSP_BOOT_MODULE_INVALID);
    CHECK(fake.count == 0);
    CHECK(psp_boot_module_receive(NULL, &ops) == PSP_BOOT_MODULE_INVALID);
    CHECK(psp_boot_module_receive(&argument, NULL) == PSP_BOOT_MODULE_INVALID);
    CHECK(!psp_boot_module_receiver_owns_fallback(PSP_BOOT_MODULE_INVALID));
    CHECK(!psp_boot_module_receiver_owns_fallback(PSP_BOOT_MODULE_COMPLETE));
    return 0;
}

static int test_formats(void)
{
    uint8_t header[20] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    header[16] = 0xa0; header[17] = 0xff; header[18] = 8;
    CHECK(psp_boot_module_format(header, sizeof(header)));
    for (size_t at = 0; at < sizeof(header); at++)
        CHECK(!psp_boot_module_format(header, at));
    CHECK(!psp_boot_module_format(NULL, sizeof(header)));
    for (size_t at = 0; at < 20u; at++) {
        if (at >= 7u && at < 16u) continue;
        header[at] ^= 0xff;
        CHECK(!psp_boot_module_format(header, sizeof(header)));
        header[at] ^= 0xff;
    }
    /* Ordinary legacy EBOOTs and static ELF executables take LoadExec. */
    header[16] = 2; header[17] = 0;
    CHECK(!psp_boot_module_format(header, sizeof(header)));
    memcpy(header, "\0PBP", 4);
    CHECK(!psp_boot_module_format(header, sizeof(header)));
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{ for (unsigned at = 0; at < 4; at++) p[at] = (uint8_t)(v >> (8u * at)); }

typedef struct { uint64_t available; unsigned calls, notices; } Space;
static bool query_space(void *context, uint64_t *available)
{ Space *s = context; s->calls++; *available = s->available; return true; }
static void work_started(void *context)
{ ((Space *)context)->notices++; }
static PspBootCacheResult cache_with_space(
    const char *source, const char *cache, const char *temp, uint64_t available)
{
    uint8_t scratch[8192];
    Space space = {.available = available};
    return psp_boot_cache_prepare(source, cache, temp, scratch, sizeof(scratch), query_space, work_started, &space);
}

static int test_cache(void)
{
    uint8_t image[8500] = {0, 'P', 'B', 'P', 0, 0, 1, 0};
    for (unsigned at = 8; at < 36; at += 4) put32(image + at, 40u);
    put32(image + 36, sizeof(image));
    const uint8_t prx[20] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0xa0, 0xff, 8, 0};
    memcpy(image + 40, prx, sizeof(prx));
    for (size_t at = 60; at < sizeof(image); at++) image[at] = (uint8_t)(at * 37u);
    uint32_t begin = 99, size = 99;
    CHECK(psp_boot_pbp_payload(image, 40u, sizeof(image), &begin, &size));
    CHECK(begin == 40u && size == sizeof(image) - 40u);
    for (unsigned at = 0; at < 40; at++)
        CHECK(!psp_boot_pbp_payload(image, at, sizeof(image), &begin, &size));
    CHECK(!psp_boot_pbp_payload(image, 40, PSP_BOOT_IMAGE_MAX_BYTES + 1u, &begin, &size));
    put32(image + 32, sizeof(image) + 1u);
    CHECK(!psp_boot_pbp_payload(image, 40, sizeof(image), &begin, &size));
    put32(image + 32, 40u);
    put32(image + 12, 39u);
    CHECK(!psp_boot_pbp_payload(image, 40, sizeof(image), &begin, &size));
    put32(image + 12, 40u);
    char dir[] = "/tmp/tilefinch-boot-cache-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char source[160], cache[160], temp[160], invalid[180], receipt[180], other[180];
    snprintf(source, sizeof(source), "%s/EBOOT.PBP", dir);
    snprintf(cache, sizeof(cache), "%s/startup.prx", dir);
    snprintf(temp, sizeof(temp), "%s/startup.tmp", dir);
    snprintf(receipt, sizeof(receipt), "%s.receipt", cache);
    snprintf(other, sizeof(other), "%s/other.PBP", dir);
    snprintf(invalid, sizeof(invalid), "%s/missing/temp", dir);
    FILE *file = fopen(source, "wb"); CHECK(file != NULL);
    CHECK(fwrite(image, 1, sizeof(image), file) == sizeof(image)); CHECK(fclose(file) == 0);
    CHECK(cache_with_space(source, cache, temp, 0) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(access(temp, F_OK) != 0 && access(cache, F_OK) != 0);
    CHECK(cache_with_space(source, cache, invalid, UINT64_MAX) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(cache_with_space(source, cache, temp, UINT64_MAX) == PSP_BOOT_CACHE_CREATED);
    uint8_t scratch[131072];
    Space space = {0};
    CHECK(psp_boot_cache_prepare(source, cache, temp, scratch, sizeof(scratch), query_space, work_started, &space) == PSP_BOOT_CACHE_REUSED);
    CHECK(space.calls == 0); /* Full card / costly statvfs never delays a hit. */
    CHECK(space.notices == 0);
    CHECK(remove(receipt) == 0);
    CHECK(psp_boot_cache_prepare(source, cache, temp, scratch, sizeof(scratch), query_space, work_started, &space) == PSP_BOOT_CACHE_REUSED);
    CHECK(space.notices == 1 && space.calls == 0);
    CHECK(remove(cache) == 0);
    CHECK(psp_boot_cache_prepare(source, cache, temp, scratch, sizeof(scratch), query_space, work_started, &space) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(space.notices == 2 && space.calls == 1);
    space.available = UINT64_MAX;
    CHECK(psp_boot_cache_prepare(source, cache, temp, scratch, sizeof(scratch), query_space, work_started, &space) == PSP_BOOT_CACHE_CREATED);
    CHECK(space.notices == 3 && space.calls == 2);
    CHECK(psp_boot_cache_prepare(source, cache, temp, NULL, sizeof(scratch), query_space, work_started, &space) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(psp_boot_cache_prepare(source, cache, temp, scratch, 8191, query_space, work_started, &space) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(psp_boot_cache_prepare(source, cache, temp, scratch, sizeof(scratch)+1u, query_space, work_started, &space) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(space.notices == 3);
    CHECK(cache_with_space(receipt, cache, temp, UINT64_MAX) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(access(receipt, F_OK) == 0); /* An alias must not delete its input. */
    CHECK(cache_with_space(source, cache, receipt, UINT64_MAX) == PSP_BOOT_CACHE_UNAVAILABLE);
    struct stat before, after;
    CHECK(stat(cache, &before) == 0);
    CHECK(cache_with_space(source, cache, temp, 0) == PSP_BOOT_CACHE_REUSED);
    CHECK(stat(cache, &after) == 0 && before.st_ino == after.st_ino);
    CHECK(after.st_size == (off_t)(sizeof(image) - 40u));
    uint8_t actual[sizeof(image)];
    file = fopen(cache, "rb"); CHECK(file != NULL);
    CHECK(fread(actual, 1, sizeof(actual), file) == sizeof(image) - 40u);
    CHECK(memcmp(actual, image + 40, sizeof(image) - 40u) == 0); CHECK(fclose(file) == 0);
    /* The receipt intentionally trusts unchanged metadata after verification.
       Safe mode is the recovery path for undetected same-metadata corruption. */
    file = fopen(cache, "r+b"); CHECK(file != NULL);
    CHECK(fseek(file, 83, SEEK_SET) == 0 && fputc(0, file) == 0 && fclose(file) == 0);
    CHECK(stat(cache, &after) == 0);
    struct utimbuf times = {before.st_atime, before.st_mtime};
    CHECK(utime(cache, &times) == 0);
    /* ctime may tick during the test, so first renew the exact comparison. */
    CHECK(remove(receipt) == 0);
    CHECK(cache_with_space(source, cache, temp, UINT64_MAX) == PSP_BOOT_CACHE_CREATED);
    CHECK(stat(cache, &before) == 0);
    file = fopen(cache, "r+b"); CHECK(file != NULL);
    CHECK(fseek(file, 83, SEEK_SET) == 0 && fputc(0, file) == 0 && fclose(file) == 0);
    CHECK(stat(cache, &after) == 0);
    if (before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime)
        CHECK(cache_with_space(source, cache, temp, 0) == PSP_BOOT_CACHE_REUSED);
    /* A detectable cache change must not use the receipt, even on a full card. */
    times = (struct utimbuf){after.st_atime, after.st_mtime + 4};
    CHECK(utime(cache, &times) == 0);
    CHECK(cache_with_space(source, cache, temp, 0) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(cache_with_space(source, cache, temp, UINT64_MAX) == PSP_BOOT_CACHE_CREATED);
    /* A slot change forces comparison even when both packages share metadata. */
    CHECK(rename(source, other) == 0);
    CHECK(cache_with_space(other, cache, temp, 0) == PSP_BOOT_CACHE_REUSED);
    file = fopen(receipt, "rb"); CHECK(file != NULL);
    CHECK(fclose(file) == 0);
    CHECK(rename(other, source) == 0);
    /* A package update invalidates the hint and copies its new module. */
    file = fopen(source, "r+b"); CHECK(file != NULL);
    CHECK(fseek(file, 123, SEEK_SET) == 0 && fputc(7, file) == 7 && fclose(file) == 0);
    CHECK(stat(source, &after) == 0);
    times = (struct utimbuf){after.st_atime, after.st_mtime + 4};
    CHECK(utime(source, &times) == 0);
    CHECK(cache_with_space(source, cache, temp, 0) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(cache_with_space(source, cache, temp, UINT64_MAX) == PSP_BOOT_CACHE_CREATED);
    /* Missing/truncated/oversized receipts require full verification, not trust. */
    file = fopen(receipt, "wb"); CHECK(file != NULL);
    CHECK(fputc(1, file) == 1 && fclose(file) == 0);
    CHECK(cache_with_space(source, cache, temp, 0) == PSP_BOOT_CACHE_REUSED);
    file = fopen(receipt, "ab"); CHECK(file != NULL);
    CHECK(fputc(1, file) == 1 && fclose(file) == 0);
    CHECK(cache_with_space(source, cache, temp, 0) == PSP_BOOT_CACHE_REUSED);
    /* Interrupted temporary is ignored on a hit and overwritten on a miss. */
    file = fopen(temp, "wb"); CHECK(file != NULL); CHECK(fputc(1, file) == 1 && fclose(file) == 0);
    CHECK(cache_with_space(source, cache, temp, UINT64_MAX) == PSP_BOOT_CACHE_REUSED);
    CHECK(remove(cache) == 0);
    CHECK(cache_with_space(source, cache, temp, UINT64_MAX) == PSP_BOOT_CACHE_CREATED);
    CHECK(access(temp, F_OK) != 0);
    /* Older static EBOOT keeps LoadExec; never execute its stale cache. */
    file = fopen(source, "r+b"); CHECK(file != NULL);
    CHECK(fseek(file, 56, SEEK_SET) == 0 && fputc(2, file) == 2 && fclose(file) == 0);
    CHECK(stat(source, &after) == 0);
    times = (struct utimbuf){after.st_atime, after.st_mtime + 8};
    CHECK(utime(source, &times) == 0);
    CHECK(cache_with_space(source, cache, temp, UINT64_MAX) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(cache_with_space(source, source, temp, UINT64_MAX) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(remove(cache) == 0);
    CHECK(remove(receipt) != 0); /* Static source invalidated the old receipt. */
    snprintf(cache, sizeof(cache), "%s/startup-module.prx", dir);
    snprintf(temp, sizeof(temp), "%s/startup-module.tmp", dir);
    file = fopen(cache, "wb"); CHECK(file != NULL && fclose(file) == 0);
    file = fopen(temp, "wb"); CHECK(file != NULL && fclose(file) == 0);
    snprintf(receipt, sizeof(receipt), "%s.receipt", cache);
    file = fopen(receipt, "wb"); CHECK(file != NULL && fclose(file) == 0);
    psp_boot_cache_discard(NULL);
    psp_boot_cache_discard("");
    psp_boot_cache_discard(dir);
    CHECK(access(cache, F_OK) != 0 && access(temp, F_OK) != 0);
    CHECK(access(receipt, F_OK) != 0);
    CHECK(access(source, F_OK) == 0);
    psp_boot_cache_discard(dir); /* Repeated cleanup remains harmless. */
    CHECK(remove(source) == 0 && rmdir(dir) == 0);
    return 0;
}

static int test_prepackaged_and_incremental(void)
{
    char dir[] = "/tmp/tilefinch-prepackaged-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char source[160], module[160], temporary[160], receipt[180];
    snprintf(source, sizeof(source), "%s/EBOOT.PBP", dir);
    snprintf(module, sizeof(module), "%s/startup.prx", dir);
    snprintf(temporary, sizeof(temporary), "%s/startup.tmp", dir);
    snprintf(receipt, sizeof(receipt), "%s.receipt", module);
    uint8_t package[9200] = {0, 'P', 'B', 'P', 0, 0, 1, 0};
    uint8_t digest[32]; memset(digest, 0x57, sizeof(digest));
    for (unsigned at = 8; at < 36; at += 4) put32(package + at, 40);
    put32(package + 36, sizeof(package) - 40);
    memcpy(package + 40, "\x7f" "ELF\1\1\1", 7);
    package[56] = 0xa0; package[57] = 0xff; package[58] = 8;
    memcpy(package + sizeof(package) - 40, "TFBOOT01", 8);
    memcpy(package + sizeof(package) - 32, digest, sizeof(digest));
    FILE *file = fopen(source, "wb"); CHECK(file != NULL);
    CHECK(fwrite(package, 1, sizeof(package), file) == sizeof(package) && fclose(file) == 0);
    file = fopen(module, "wb"); CHECK(file != NULL);
    const size_t module_bytes = sizeof(package) - 80;
    CHECK(fwrite(package + 40, 1, module_bytes, file) == module_bytes && fclose(file) == 0);
    CHECK(psp_boot_prepackaged_module(source, module, sizeof(package), module_bytes, digest));
    CHECK(access(receipt, F_OK) != 0);
    digest[0] ^= 1;
    CHECK(!psp_boot_prepackaged_module(source, module, sizeof(package), module_bytes, digest));
    digest[0] ^= 1;
    CHECK(!psp_boot_prepackaged_module(source, module, sizeof(package)-1, module_bytes, digest));
    CHECK(!psp_boot_prepackaged_module(source, module, sizeof(package), module_bytes-1, digest));
    file = fopen(module, "r+b"); CHECK(file != NULL);
    CHECK(fputc(0, file) == 0 && fclose(file) == 0);
    CHECK(!psp_boot_prepackaged_module(source, module, sizeof(package), module_bytes, digest));
    PspBootCacheCopy copy;
    Space space = {.available = 0};
    CHECK(psp_boot_cache_copy_begin(&copy, source, module, temporary, query_space, &space)
          == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(access(temporary, F_OK) != 0);
    space.available = UINT64_MAX;
    CHECK(psp_boot_cache_copy_begin(&copy, source, module, temporary, query_space, &space)
          == PSP_BOOT_CACHE_PENDING);
    uint8_t scratch[8192];
    CHECK(psp_boot_cache_copy_step(&copy, scratch, sizeof(scratch), 0) == PSP_BOOT_CACHE_PENDING);
    CHECK(copy.remaining == module_bytes - sizeof(scratch)/2);
    psp_boot_cache_copy_close(&copy);
    CHECK(access(temporary, F_OK) != 0 && access(receipt, F_OK) != 0);
    CHECK(psp_boot_cache_copy_begin(&copy, source, module, temporary, query_space, &space)
          == PSP_BOOT_CACHE_PENDING);
    for (unsigned step = 0; step < 16 && copy.result == PSP_BOOT_CACHE_PENDING; step++)
        CHECK(psp_boot_cache_copy_step(&copy, scratch, sizeof(scratch), 0) != PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(copy.result == PSP_BOOT_CACHE_CREATED);
    CHECK(cache_with_space(source, module, temporary, 0) == PSP_BOOT_CACHE_REUSED);
    psp_boot_cache_copy_close(&copy);
    CHECK(psp_boot_prepackaged_module(source, module, sizeof(package), module_bytes, digest));
    /* A failed read-back cannot publish a new module or receipt. */
    CHECK(psp_boot_cache_copy_begin(&copy, source, module, temporary, query_space, &space)
          == PSP_BOOT_CACHE_PENDING);
    for (unsigned step = 0; step < 8 && copy.phase == 0; step++)
        CHECK(psp_boot_cache_copy_step(&copy, scratch, sizeof(scratch), 0) == PSP_BOOT_CACHE_PENDING);
    CHECK(copy.phase == 1);
    file = fopen(temporary, "r+b"); CHECK(file != NULL);
    CHECK(fputc(0, file) == 0 && fclose(file) == 0);
    CHECK(psp_boot_cache_copy_step(&copy, scratch, sizeof(scratch), 0) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(access(temporary, F_OK) != 0 && access(receipt, F_OK) != 0);
    CHECK(psp_boot_prepackaged_module(source, module, sizeof(package), module_bytes, digest));
    /* Respect the caller's smaller work unit, then reject a changed source. */
    CHECK(psp_boot_cache_copy_begin(&copy, source, module, temporary, query_space, &space)
          == PSP_BOOT_CACHE_PENDING);
    CHECK(psp_boot_cache_copy_step(&copy, scratch, sizeof(scratch), 7) == PSP_BOOT_CACHE_PENDING);
    CHECK(copy.remaining == module_bytes - 7);
    struct stat info; CHECK(stat(source, &info) == 0);
    struct utimbuf changed = {info.st_atime, info.st_mtime + 4};
    CHECK(utime(source, &changed) == 0);
    for (unsigned step = 0; step < 16 && copy.result == PSP_BOOT_CACHE_PENDING; step++)
        (void)psp_boot_cache_copy_step(&copy, scratch, sizeof(scratch), 0);
    CHECK(copy.result == PSP_BOOT_CACHE_UNAVAILABLE && access(temporary, F_OK) != 0);
    CHECK(psp_boot_prepackaged_module(source, module, sizeof(package), module_bytes, digest));
    /* The source can change after read-back but during promotion. Never
       authorize the old module with the replacement source's identity. */
    CHECK(psp_boot_cache_copy_begin(&copy, source, module, temporary, query_space, &space)
          == PSP_BOOT_CACHE_PENDING);
    replace_source_after_rename = source;
    for (unsigned step = 0; step < 16 && copy.result == PSP_BOOT_CACHE_PENDING; step++)
        (void)psp_boot_cache_copy_step(&copy, scratch, sizeof(scratch), 0);
    CHECK(replace_source_after_rename == NULL);
    CHECK(copy.result == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(access(receipt, F_OK) != 0 && access(module, F_OK) != 0);
    CHECK(cache_with_space(source, module, temporary, 0) == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(cache_with_space(source, module, temporary, UINT64_MAX) == PSP_BOOT_CACHE_CREATED);
    CHECK(psp_boot_cache_copy_begin(&copy, source, module, source, query_space, &space)
          == PSP_BOOT_CACHE_UNAVAILABLE);
    CHECK(psp_boot_cache_copy_begin(&copy, source, module, receipt, query_space, &space)
          == PSP_BOOT_CACHE_UNAVAILABLE);
    psp_boot_cache_copy_close(&copy);
    CHECK(remove(receipt) == 0);
    char named[256], hexadecimal[65];
    for (unsigned at = 0; at < sizeof(digest); at++)
        snprintf(hexadecimal + at * 2u, 3, "%02x", digest[at]);
    snprintf(named, sizeof(named), "%s/startup-%s.prx", dir, hexadecimal);
    CHECK(rename(module, named) == 0);
    /* Do not scan/delete lookalikes, legacy packages or malformed pairs. */
    file = fopen(module, "wb"); CHECK(file != NULL && fclose(file) == 0);
    file = fopen(source, "r+b"); CHECK(file != NULL);
    CHECK(fseek(file, sizeof(package) - 40, SEEK_SET) == 0);
    CHECK(fputc(0, file) == 0 && fclose(file) == 0);
    CHECK(!psp_boot_prepackaged_discard(dir) && access(named, F_OK) == 0);
    file = fopen(source, "r+b"); CHECK(file != NULL);
    CHECK(fseek(file, sizeof(package) - 40, SEEK_SET) == 0);
    CHECK(fputc('T', file) == 'T' && fclose(file) == 0);
    file = fopen(named, "r+b"); CHECK(file != NULL);
    CHECK(fputc(0, file) == 0 && fclose(file) == 0);
    CHECK(!psp_boot_prepackaged_discard(dir) && access(named, F_OK) == 0);
    file = fopen(named, "r+b"); CHECK(file != NULL);
    CHECK(fputc(0x7f, file) == 0x7f && fclose(file) == 0);
    CHECK(psp_boot_prepackaged_discard(dir) && access(named, F_OK) != 0);
    CHECK(access(module, F_OK) == 0 && access(source, F_OK) == 0);
    CHECK(!psp_boot_prepackaged_discard(dir));
    CHECK(!psp_boot_prepackaged_discard(NULL) && !psp_boot_prepackaged_discard(""));
    CHECK(remove(module) == 0 && remove(source) == 0 && rmdir(dir) == 0);
    return 0;
}

int main(void)
{
    if (test_arguments() || test_retirement() || test_formats() || test_cache()
        || test_prepackaged_and_incremental()) return 1;
    puts("PSP boot handoff checks passed");
    return 0;
}
