#include "tilefinch/zip_install.h"
#include "tilefinch/file_download.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_offline_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return false; } } while (0)

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t length;
    unsigned method, flags, mode;
    bool bad_crc, descriptor;
    uint32_t declared_size;
} Input;

static void put16(FILE *file, unsigned n)
{
    fputc((int) (n & 255), file); fputc((int) ((n >> 8) & 255), file);
}
static void put32(FILE *file, uint32_t n)
{
    put16(file, n & 65535); put16(file, n >> 16);
}

/* Small, authored structural fixtures, not downloaded archives or reproducers. */
static bool archive_write(const char *path, const Input *input, size_t count)
{
    CHECK(count <= 8);
    uint32_t offsets[8], sizes[8], crcs[8];
    FILE *file = fopen(path, "wb"); CHECK(file);
    for (size_t i = 0; i < count; i++) {
        const Input *in = input + i;
        unsigned char compressed[65536];
        CHECK(in->length < sizeof(compressed));
        sizes[i] = (uint32_t) in->length;
        if (in->method == 8) {
            z_stream stream = {0};
            CHECK(deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
            stream.next_in = (Bytef *) in->data; stream.avail_in = (uInt) in->length;
            stream.next_out = compressed; stream.avail_out = sizeof(compressed);
            CHECK(deflate(&stream, Z_FINISH) == Z_STREAM_END);
            sizes[i] = (uint32_t) stream.total_out;
            CHECK(deflateEnd(&stream) == Z_OK);
        }
        crcs[i] = (uint32_t) crc32(0, in->data, (uInt) in->length) ^ (in->bad_crc ? 1u : 0u);
        offsets[i] = (uint32_t) ftell(file);
        put32(file, 0x04034b50); put16(file, 20);
        put16(file, in->flags | (in->descriptor ? 8u : 0u)); put16(file, in->method);
        put32(file, 0); put32(file, in->descriptor ? 0 : crcs[i]);
        put32(file, in->descriptor ? 0 : sizes[i]);
        put32(file, in->descriptor ? 0 : in->declared_size ? in->declared_size : (uint32_t) in->length);
        put16(file, (unsigned) strlen(in->name)); put16(file, 0);
        CHECK(fwrite(in->name, 1, strlen(in->name), file) == strlen(in->name));
        CHECK(fwrite(in->method == 8 ? compressed : in->data, 1, sizes[i], file) == sizes[i]);
        if (in->descriptor) {
            put32(file, 0x08074b50); put32(file, crcs[i]); put32(file, sizes[i]);
            put32(file, in->declared_size ? in->declared_size : (uint32_t) in->length);
        }
    }
    uint32_t central = (uint32_t) ftell(file);
    for (size_t i = 0; i < count; i++) {
        const Input *in = input + i;
        put32(file, 0x02014b50); put16(file, 3u << 8 | 20u); put16(file, 20);
        put16(file, in->flags | (in->descriptor ? 8u : 0u)); put16(file, in->method);
        put32(file, 0); put32(file, crcs[i]); put32(file, sizes[i]);
        put32(file, in->declared_size ? in->declared_size : (uint32_t) in->length);
        put16(file, (unsigned) strlen(in->name)); put16(file, 0); put16(file, 0);
        put16(file, 0); put16(file, 0); put32(file, in->mode << 16);
        put32(file, offsets[i]); fwrite(in->name, 1, strlen(in->name), file);
    }
    uint32_t bytes = (uint32_t) ftell(file) - central;
    put32(file, 0x06054b50); put16(file, 0); put16(file, 0);
    put16(file, (unsigned) count); put16(file, (unsigned) count);
    put32(file, bytes); put32(file, central); put16(file, 0);
    CHECK(!ferror(file) && fclose(file) == 0);
    return true;
}

static bool erase(const char *path, unsigned depth)
{
    CHECK(depth < 12);
    struct stat info;
    if (lstat(path, &info)) return true;
    if (!S_ISDIR(info.st_mode)) return unlink(path) == 0;
    DIR *dir = opendir(path); CHECK(dir);
    struct dirent *entry; size_t count = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        CHECK(++count < 1024);
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        CHECK(erase(child, depth + 1));
    }
    CHECK(!closedir(dir) && !rmdir(path));
    return true;
}

static bool profile;
static uint64_t pump_times[10000];
static int compare_time(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static ZipInstallStatus finish(ZipInstall *install)
{
    ZipInstallStatus status = ZIP_INSTALL_PENDING;
    size_t count = 0;
    for (; count < 10000 && status == ZIP_INSTALL_PENDING; count++) {
        uint64_t start = profile ? tilefinch_platform_monotonic_time_us() : 0;
        status = zip_install_pump(install);
        if (profile) pump_times[count] = tilefinch_platform_monotonic_time_us() - start;
    }
    if (profile && count) {
        qsort(pump_times, count, sizeof(*pump_times), compare_time);
        printf("ZIP owner pump n=%zu median=%lluus p95=%lluus max=%lluus\n", count,
            (unsigned long long)pump_times[count/2], (unsigned long long)pump_times[(count-1)*95/100],
            (unsigned long long)pump_times[count-1]);
    }
    return status;
}

static unsigned char pbp[80];

static bool cancel_install_checkpoint(void *context, const char *phase, size_t work)
{
    (void) context; (void) work;
    return strcmp(phase, "zip-install") != 0;
}

static uint64_t frozen_clock(void *context)
{
    (void) context;
    return 1;
}

static bool native_journey(const char *root, const char *archive, const char *games)
{
    BrowserConfig config; browser_config_init(&config, NULL);
    config.javascript.enabled = false; config.resources.enabled = false;
    char error[128]; BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine);
    PspOfflineStore store;
    psp_offline_store_init(&store, browser_engine_budget(engine), browser_engine_session(engine), root);
    store.game_directory = games;
    uint32_t id = 0;
    CHECK(file_download_enqueue(&store.library, "https://downloads.example.test/get?id=1",
        "download.bin", &id, error, sizeof(error)));
    char path[512]; CHECK(file_download_path(&store.library, id, path, sizeof(path)));
    FILE *source = fopen(archive, "rb"), *copy = fopen(path, "wb"); CHECK(source && copy);
    unsigned char buffer[4096]; size_t count, total = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), source))) {
        CHECK(fwrite(buffer, 1, count, copy) == count); total += count;
    }
    CHECK(!ferror(source) && !fclose(source) && !fclose(copy));
    store.library.items[0].state = OFFLINE_ITEM_READY;
    store.library.items[0].content_bytes = total;
    CHECK(offline_library_save(&store.library));
    char *html = NULL; size_t length = 0;
    CHECK(offline_library_build_file_page(&store.library, browser_engine_budget(engine), id, &html, &length));
    CHECK(strstr(html, "Install homebrew") != NULL); /* Signature, not filename extension. */
    budget_free(browser_engine_budget(engine), html);
    char url[128]; snprintf(url, sizeof(url), "https://tilefinch.local/offline/extract?id=%u", (unsigned)id);
    CHECK(psp_offline_store_handle_url(&store, engine, NULL, url, NULL, true) == PSP_OFFLINE_ROUTE_ZIP_PREVIEW);
    CHECK(store.zip_install && !strcmp(zip_install_preview(store.zip_install)->name, "Test"));
    psp_offline_store_discard_zip(&store); CHECK(!store.zip_install);
    CHECK(psp_offline_store_handle_url(&store, engine, NULL, url, NULL, true) == PSP_OFFLINE_ROUTE_ZIP_PREVIEW);
    TilefinchPlatformServices services = {.cooperate = cancel_install_checkpoint,
        .monotonic_time_us = frozen_clock};
    tilefinch_platform_set_services(&services);
    bool installed = psp_offline_store_install_prepared_zip(&store);
    tilefinch_platform_set_services(NULL);
    CHECK(installed && !store.zip_install && !access(path, F_OK));
    CHECK(offline_library_remove(&store.library, id));
    CHECK(access(path, F_OK)); /* Delete ZIP independently of the installation. */
    char names[4][64]; size_t installed_count = 0;
    CHECK(zip_install_list(games, names, 4, &installed_count) && installed_count == 1 && !strcmp(names[0], "Test"));
    CHECK(psp_offline_store_handle_url(&store, engine, NULL,
        "https://tilefinch.local/offline/homebrew", NULL, true) == PSP_OFFLINE_ROUTE_PAGE);
    CHECK(psp_offline_store_handle_url(&store, engine, NULL,
        "https://tilefinch.local/offline/remove-homebrew?name=54657374", NULL, true) == PSP_OFFLINE_ROUTE_ZIP_PREVIEW);
    CHECK(zip_install_preview(store.zip_install)->removing);
    psp_offline_store_discard_zip(&store); /* Cancel is read-only. */
    char boot[768]; snprintf(boot, sizeof(boot), "%s/Test/EBOOT.PBP", games);
    CHECK(!access(boot, F_OK));
    char link[768]; snprintf(link, sizeof(link), "%s/Test/unsafe", games);
    CHECK(!symlink(root, link));
    CHECK(!zip_install_prepare_removal(browser_engine_budget(engine), games, "Test", error, sizeof(error)));
    CHECK(!access(boot, F_OK) && !unlink(link));
    size_t retained = browser_engine_budget(engine)->current;
    for (size_t failure = 0; failure < 4; failure++) {
        budget_inject_failure_after(browser_engine_budget(engine), failure);
        ZipInstall *probe = zip_install_prepare_removal(browser_engine_budget(engine), games, "Test", error, sizeof(error));
        budget_clear_failure_injection(browser_engine_budget(engine));
        zip_install_destroy(probe);
        CHECK(browser_engine_budget(engine)->current == retained && !access(boot, F_OK));
    }
    char receipt_path[768]; snprintf(receipt_path, sizeof(receipt_path), "%s/Test/.tilefinch-homebrew", games);
    FILE *marker = fopen(receipt_path, "r+b"); CHECK(marker);
    CHECK(fputc('X', marker) != EOF && !fclose(marker));
    CHECK(!zip_install_prepare_removal(browser_engine_budget(engine), games, "Test", error, sizeof(error)));
    marker = fopen(receipt_path, "r+b"); CHECK(marker);
    CHECK(fputc('T', marker) != EOF && !fclose(marker));
    for (unsigned i = 0; i < 20; i++) {
        char save[768]; snprintf(save, sizeof(save), "%s/Test/user%u.sav", games, i);
        FILE *file = fopen(save, "wb"); CHECK(file && fputs("saved data", file) >= 0 && !fclose(file));
    }
    ZipInstall *partial = zip_install_prepare_removal(browser_engine_budget(engine), games, "Test", error, sizeof(error));
    CHECK(partial && zip_install_begin(partial) && zip_install_pump(partial) == ZIP_INSTALL_PENDING);
    zip_install_cancel(partial); zip_install_destroy(partial);
    CHECK(!access(receipt_path, F_OK)); /* A stopped removal remains discoverable. */
    CHECK(psp_offline_store_handle_url(&store, engine, NULL,
        "https://tilefinch.local/offline/remove-homebrew?name=54657374", NULL, true) == PSP_OFFLINE_ROUTE_ZIP_PREVIEW);
    CHECK(psp_offline_store_install_prepared_zip(&store));
    CHECK(access(boot, F_OK) && zip_install_list(games, names, 4, &installed_count) && installed_count == 0);
    CHECK(!zip_install_prepare_removal(browser_engine_budget(engine), games, "../Test", error, sizeof(error)));
    psp_offline_store_destroy(&store); browser_engine_destroy(engine);
    return true;
}

static bool tests(const char *root)
{
    memcpy(pbp, "\0PBP", 4);
    for (size_t i = 8; i < 40; i += 4) pbp[i] = 40;
    char archive[512], games[512], final[600], stage[600], error[128];
    snprintf(archive, sizeof(archive), "%s/package.zip", root);
    snprintf(games, sizeof(games), "%s/GAME", root); CHECK(!mkdir(games, 0700));
    snprintf(final, sizeof(final), "%s/Test", games);
    snprintf(stage, sizeof(stage), "%s/.tilefinch-unpack-00000001", games);
    static const unsigned char data[] = "homebrew fixture\n";
    Input files[8] = {
        {.name="PSP/GAME/Test/EBOOT.PBP", .data=pbp, .length=sizeof(pbp), .method=8, .descriptor=true},
        {.name="PSP/GAME/Test/data/level.txt", .data=data, .length=sizeof(data)-1},
        {.name="README.txt", .data=data, .length=sizeof(data)-1},
        {.name="PSP/GAME/Test/empty/", .data=data},
        {.name="PSP/GAME/Test/.prefs", .data=data, .length=sizeof(data)-1},
        {.name="__MACOSX/Test/._data", .data=data, .length=sizeof(data)-1}
    };
    Budget budget; budget_init(&budget, 1024u * 1024u);
    CHECK(archive_write(archive, files, 6));
    ZipInstall *install = zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error));
    CHECK(install);
    const ZipInstallPreview *view = zip_install_preview(install);
    CHECK(view->files == 3 && view->skipped_files == 2 && !strcmp(view->destination, final));
    CHECK(access(stage, F_OK) && access(final, F_OK)); /* Preview never writes. */
    CHECK(zip_install_begin(install) && finish(install) == ZIP_INSTALL_DONE);
    zip_install_destroy(install); CHECK(!budget.current && access(stage, F_OK));
    char path[768]; snprintf(path, sizeof(path), "%s/EBOOT.PBP", final);
    FILE *file = fopen(path, "rb"); CHECK(file);
    unsigned char copied[sizeof(pbp)]; CHECK(fread(copied, 1, sizeof(copied), file) == sizeof(copied));
    CHECK(!memcmp(pbp, copied, sizeof(pbp)) && fgetc(file) == EOF && !fclose(file));
    snprintf(path, sizeof(path), "%s/empty", final);
    CHECK(!access(path, F_OK));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    CHECK(!budget.current && !access(path, F_OK));
    CHECK(erase(final, 0));
    CHECK(native_journey(root, archive, games));
    CHECK(erase(final, 0));

    /* Every allocation boundary, including inflater and directory bookkeeping. */
    size_t completed = 0, refused = 0;
    for (size_t failure = 0; failure < 40; failure++) {
        budget_inject_failure_after(&budget, failure);
        install = zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error));
        if (install && zip_install_begin(install) && finish(install) == ZIP_INSTALL_DONE) completed++;
        else refused++;
        budget_clear_failure_injection(&budget);
        zip_install_destroy(install);
        CHECK(!budget.current && access(stage, F_OK));
        CHECK(erase(final, 0));
    }
    CHECK(completed && refused);
    install = zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error));
    CHECK(install && zip_install_begin(install));
    zip_install_cancel(install); CHECK(zip_install_pump(install) == ZIP_INSTALL_CANCELLED);
    zip_install_destroy(install); CHECK(!budget.current && access(final, F_OK) && access(stage, F_OK));
    CHECK(!mkdir(stage, 0700));
    install = zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error));
    CHECK(install && !zip_install_begin(install));
    zip_install_destroy(install); CHECK(!budget.current && !access(stage, F_OK));
    CHECK(!rmdir(stage)); /* Never delete another run's staging folder. */
    CHECK(!symlink("missing-target", final));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    CHECK(!budget.current && !unlink(final));

    static const char *unsafe[] = {"../EBOOT.PBP", "/Test/EBOOT.PBP", "Test/../EBOOT.PBP",
        "C:/Test/EBOOT.PBP", "Test\\EBOOT.PBP", "Test/CON", "Test/evil. ", ".hidden/EBOOT.PBP"};
    for (size_t i = 0; i < sizeof(unsafe)/sizeof(unsafe[0]); i++) {
        files[0].name = unsafe[i]; CHECK(archive_write(archive, files, 1));
        CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)) && !budget.current);
    }
    files[0].name = "Test/EBOOT.PBP";
    const Input boot = files[0];
    files[0].flags = 1; CHECK(archive_write(archive, files, 1));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    files[0] = boot; files[0].mode = 0120777; CHECK(archive_write(archive, files, 1));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    files[0] = boot; files[1] = boot; files[1].name = "Other/EBOOT.PBP";
    CHECK(archive_write(archive, files, 2));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    files[1].name = "test/Eboot.pbp"; CHECK(archive_write(archive, files, 2));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    files[1].name = "test/data.bin"; CHECK(archive_write(archive, files, 2));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    files[1].name = "test/"; files[1].length = 0; files[1].method = 0;
    CHECK(archive_write(archive, files, 2));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    files[1] = boot;
    files[1].name = "Other/payload.prx"; CHECK(archive_write(archive, files, 2));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    files[1].name = "Test/.tilefinch-homebrew"; CHECK(archive_write(archive, files, 2));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    files[0].declared_size = ZIP_INSTALL_FILE_LIMIT + 1; CHECK(archive_write(archive, files, 1));
    CHECK(!zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error)));
    files[0] = boot; files[0].bad_crc = true;
    CHECK(archive_write(archive, files, 1));
    install = zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error));
    CHECK(install && zip_install_begin(install) && finish(install) == ZIP_INSTALL_FAILED);
    zip_install_destroy(install); CHECK(!budget.current && access(final, F_OK) && access(stage, F_OK));
    files[0] = boot; pbp[0] = 1; CHECK(archive_write(archive, files, 1)); pbp[0] = 0;
    install = zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error));
    CHECK(install && zip_install_begin(install) && finish(install) == ZIP_INSTALL_FAILED);
    zip_install_destroy(install); CHECK(!budget.current && access(final, F_OK));
    /* Cancellation after writing partial output rolls back all owned paths. */
    static unsigned char large[60000];
    files[0] = boot;
    files[1] = (Input) {.name="Test/data.bin", .data=large, .length=sizeof(large)};
    CHECK(archive_write(archive, files, 2));
    install = zip_install_prepare(&budget, archive, games, "Test.zip", 1, error, sizeof(error));
    CHECK(install && zip_install_begin(install) && zip_install_pump(install) == ZIP_INSTALL_PENDING);
    zip_install_cancel(install); zip_install_destroy(install);
    CHECK(!budget.current && access(final, F_OK) && access(stage, F_OK));
    /* Bare EBOOT archives get one explicit folder, not a write to GAME itself. */
    files[0] = boot; files[0].name = "EBOOT.PBP"; files[0].method = 0;
    CHECK(archive_write(archive, files, 1));
    install = zip_install_prepare(&budget, archive, games, "Test.v1.zip", 1, error, sizeof(error));
    CHECK(install && !strcmp(zip_install_preview(install)->name, "Test.v1"));
    zip_install_destroy(install); CHECK(!budget.current);
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 3 && (!strcmp(argv[1], "--qualify-archive") || !strcmp(argv[1], "--profile-archive"))) {
        char root[] = "/tmp/tilefinch-zip-qualification-XXXXXX", error[128] = {0};
        if (!mkdtemp(root)) return 1;
        Budget budget; budget_init(&budget, 1024u*1024u);
        profile = !strcmp(argv[1], "--profile-archive");
        uint64_t start = tilefinch_platform_monotonic_time_us();
        ZipInstall *install = zip_install_prepare(&budget, argv[2], root, "Homebrew.zip", 1, error, sizeof(error));
        if (profile) printf("ZIP prepare=%lluus\n", (unsigned long long)(tilefinch_platform_monotonic_time_us()-start));
        bool okay = install && zip_install_begin(install) && finish(install) == ZIP_INSTALL_DONE;
        if (okay) printf("Verified ZIP install: %u files, %u bytes; Budget peak %zu\n",
            zip_install_preview(install)->files, zip_install_preview(install)->unpacked_bytes, budget.peak);
        else fprintf(stderr, "%s\n", install ? zip_install_error(install) : error);
        zip_install_destroy(install); okay &= budget.current == 0;
        okay &= erase(root, 0); return okay ? 0 : 1;
    }
    char root[] = "/tmp/tilefinch-zip-tests-XXXXXX";
    if (argc != 1 || !mkdtemp(root)) return 1;
    bool okay = tests(root);
    okay &= erase(root, 0);
    return okay ? 0 : 1;
}
