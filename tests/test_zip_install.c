#include "tilefinch/zip_install.h"
#include "tilefinch/file_download.h"
#include "tilefinch/offline_library.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_offline_store.h"

#include "supervisor_view_probe.h"

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
    /* An Arabic name: the archive is stored under its percent-encoded
       name, and the install question still finds it. */
    CHECK(file_download_enqueue(&store.library, "https://downloads.example.test/get?id=1",
        "\xd9\x84\xd8\xb9\xd8\xa8\xd8\xa9.bin", &id, error, sizeof(error)));
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
    /* Supervision follows the store's own route parse: the ZIP previews and
       the homebrew list run under a cancellable scope, nothing else does. */
    CHECK(!strcmp(psp_offline_store_route_cancel_status(url), "STOPPING ZIP CHECK...")
          && !strcmp(psp_offline_store_route_cancel_status(
                 "https://tilefinch.local/offline/remove-homebrew?name=54657374"),
                 "STOPPING ZIP CHECK...")
          && !strcmp(psp_offline_store_route_cancel_status(
                 "https://tilefinch.local/offline/homebrew"), "STOPPING...")
          && !psp_offline_store_route_cancel_status("https://tilefinch.local/offline")
          && !psp_offline_store_route_cancel_status(
                 "https://tilefinch.local/offline/extract?id=x")
          && !psp_offline_store_route_cancel_status(
                 "https://example.com/offline/homebrew-guide")
          && !psp_offline_store_route_cancel_status(
                 "https://tilefinch.local/offline/homebrew?page=2"));
    CHECK(strstr(path, "-%D9%84%D8%B9%D8%A8%D8%A9.bin") != NULL);
    /* A finished download's toast is not shown again over the question,
       nor over the saved-file page; a failed route still reports. */
    snprintf(store.status, sizeof(store.status), "FILE SAVED");
    PspOfflineRouteResult opened = psp_offline_store_handle_url(&store, engine, NULL, url, NULL, true);
    CHECK(opened == PSP_OFFLINE_ROUTE_ZIP_PREVIEW && psp_offline_store_route_toast(&store, opened) == NULL);
    CHECK(store.zip_install && !strcmp(zip_install_preview(store.zip_install)->name, "Test"));
    snprintf(store.status, sizeof(store.status), "FILE SAVED");
    char page_url[128];
    snprintf(page_url, sizeof(page_url), "https://tilefinch.local/offline/file?id=%u", (unsigned)id);
    opened = psp_offline_store_handle_url(&store, engine, NULL, page_url, NULL, true);
    CHECK(opened == PSP_OFFLINE_ROUTE_PAGE && psp_offline_store_route_toast(&store, opened) == NULL);
    snprintf(store.status, sizeof(store.status), "FILE SAVED");
    opened = psp_offline_store_handle_url(&store, engine, NULL, "https://tilefinch.local/offline/nowhere", NULL, true);
    CHECK(opened == PSP_OFFLINE_ROUTE_ERROR
          && !strcmp(psp_offline_store_route_toast(&store, opened), "OFFLINE LINK IS INVALID"));
    /* Back or Forward that cannot move never reaches handle_url; it reports
       its own failure, not the last route's message. */
    snprintf(store.status, sizeof(store.status), "FILE SAVED");
    opened = psp_offline_store_history_unavailable(&store);
    CHECK(opened == PSP_OFFLINE_ROUTE_ERROR
          && !strcmp(psp_offline_store_route_toast(&store, opened), "PAGE UNAVAILABLE"));
    opened = psp_offline_store_handle_url(&store, engine, NULL, url, NULL, true);
    CHECK(opened == PSP_OFFLINE_ROUTE_ZIP_PREVIEW && psp_offline_store_route_toast(&store, opened) == NULL);
    /* The sheet is up while a load is pending, and the navigation pump
       begins the supervisor again over it. Cancel frees the preview: the
       supervisor's copy must let go of it first. */
    PspUiState live; psp_ui_init(&live);
    const ZipInstallPreview *sheet = zip_install_preview(store.zip_install);
    psp_ui_show_zip_install(&live, sheet);
    view_probe_begin(&store, &live);
    CHECK(view_probe.copy.screen == PSP_UI_SCREEN_ZIP_INSTALL);
    psp_offline_store_discard_zip(&store); CHECK(!store.zip_install);
    CHECK(view_probe_present(sheet) && view_probe.releases == 1u
          && view_probe.dropped == 1u
          && view_probe.copy.screen == PSP_UI_SCREEN_PAGE);
    view_probe_end(&store);
    CHECK(psp_offline_store_handle_url(&store, engine, NULL, url, NULL, true) == PSP_OFFLINE_ROUTE_ZIP_PREVIEW);
    TilefinchPlatformServices services = {.cooperate = cancel_install_checkpoint,
        .monotonic_time_us = frozen_clock};
    tilefinch_platform_set_services(&services);
    bool installed = psp_offline_store_install_prepared_zip(&store);
    tilefinch_platform_set_services(NULL);
    CHECK(installed && !store.zip_install && !access(path, F_OK));
    CHECK(offline_library_remove(&store.library, id));
    CHECK(access(path, F_OK)); /* Delete ZIP independently of the installation. */
    char names[4][ZIP_INSTALL_FOLDER_NAME_LIMIT]; size_t installed_count = 0;
    char titles[4][ZIP_INSTALL_TITLE_LIMIT];
    CHECK(zip_install_list(games, names, titles, 4, &installed_count) && installed_count == 1
          && !strcmp(names[0], "Test") && !strcmp(titles[0], "Test"));
    installed_count = 0; /* Count only. */
    CHECK(zip_install_list(games, NULL, NULL, 4, &installed_count) && installed_count == 1);
    /* The removal route's folder-name spelling round-trips, and only it. */
    char hex[ZIP_INSTALL_FOLDER_HEX_LIMIT], decoded[ZIP_INSTALL_FOLDER_NAME_LIMIT], longest[64];
    zip_install_encode_folder_name("Test", hex);
    CHECK(!strcmp(hex, "54657374") && zip_install_decode_folder_name(hex, decoded) && !strcmp(decoded, "Test"));
    memset(longest, 0xe9, sizeof(longest) - 1); longest[sizeof(longest) - 1] = 0;
    zip_install_encode_folder_name(longest, hex);
    CHECK(strlen(hex) == 126 && zip_install_decode_folder_name(hex, decoded) && !strcmp(decoded, longest));
    static const char *const refused[] = {"", "5", "54657", "5465737G", "54004", "540074",
        "54657A74", "0x", "5465 374"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        CHECK(!zip_install_decode_folder_name(refused[i], decoded));
    char too_long[130]; memset(too_long, '6', 128); too_long[128] = 0;
    CHECK(!zip_install_decode_folder_name(too_long, decoded));
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
    CHECK(access(boot, F_OK) && zip_install_list(games, names, NULL, 4, &installed_count) && installed_count == 0);
    CHECK(!zip_install_prepare_removal(browser_engine_budget(engine), games, "../Test", error, sizeof(error)));
    psp_offline_store_destroy(&store); browser_engine_destroy(engine);
    return true;
}

static long receipt_size(const char *games, const char *folder)
{
    char path[768]; struct stat info;
    snprintf(path, sizeof(path), "%s/%s/.tilefinch-homebrew", games, folder);
    return lstat(path, &info) ? -1 : (long) info.st_size;
}

static bool install_bare(Budget *budget, const char *archive, const char *games,
    const char *title, uint32_t id, const char *folder, const char *shown)
{
    char error[128];
    ZipInstall *install = zip_install_prepare(budget, archive, games, title, id, error, sizeof(error));
    CHECK(install);
    const ZipInstallPreview *view = zip_install_preview(install);
    char destination[600]; snprintf(destination, sizeof(destination), "%s/%s", games, folder);
    if (strcmp(view->name, folder) || strcmp(view->title, shown) || strcmp(view->destination, destination))
        fprintf(stderr, "bare ZIP %s: folder \"%s\" title \"%s\"\n", title, view->name, view->title);
    CHECK(!strcmp(view->name, folder) && !strcmp(view->title, shown)
          && !strcmp(view->destination, destination));
    CHECK(zip_install_begin(install) && finish(install) == ZIP_INSTALL_DONE);
    zip_install_destroy(install); CHECK(!budget->current);
    char boot[700]; snprintf(boot, sizeof(boot), "%s/EBOOT.PBP", destination);
    CHECK(!access(boot, F_OK));
    return true;
}

static bool remove_installed(Budget *budget, const char *games, const char *folder, const char *shown)
{
    char error[128];
    ZipInstall *job = zip_install_prepare_removal(budget, games, folder, error, sizeof(error));
    CHECK(job && !strcmp(zip_install_preview(job)->title, shown));
    CHECK(zip_install_begin(job) && finish(job) == ZIP_INSTALL_DONE);
    zip_install_destroy(job); CHECK(!budget->current);
    char path[600]; snprintf(path, sizeof(path), "%s/%s", games, folder);
    CHECK(access(path, F_OK));
    return true;
}

/* A top-level EBOOT.PBP installs to a folder named after the download.
   A title that is not a plain ASCII folder name (other letters, or too
   long) gets APP-<ID> instead, and the manager still shows the title. */
static bool bare_titles(Budget *budget, const char *archive, const char *games)
{
    static const char arabic[] = "\xd9\x84\xd8\xb9\xd8\xa8\xd8\xa9"; /* lu'ba */
    static const char hebrew[] = "\xd7\x9e\xd7\xa9\xd7\x97\xd7\xa7"; /* misxak */
    char title[128], long_ascii[128], long_stem[128], error[128];
    snprintf(title, sizeof(title), "%s.zip", arabic);
    CHECK(install_bare(budget, archive, games, title, 7, "APP-00000007", arabic));
    /* A version-2 receipt carries the title; ASCII-named apps keep the
       version-1 receipt earlier releases read. */
    CHECK(receipt_size(games, "APP-00000007") == 208);
    CHECK(install_bare(budget, archive, games, "Plain Game.zip", 8, "Plain Game", "Plain Game"));
    CHECK(receipt_size(games, "Plain Game") == 80);
    /* Too long for a folder name: ASCII too. */
    memset(long_stem, 'L', 70); long_stem[70] = 0;
    snprintf(long_ascii, sizeof(long_ascii), "%s.zip", long_stem);
    CHECK(install_bare(budget, archive, games, long_ascii, 9, "APP-00000009", long_stem));
    /* The same title again: a new download gets its own folder... */
    CHECK(install_bare(budget, archive, games, title, 10, "APP-0000000A", arabic));
    /* ...and installing one download twice never replaces the first. */
    CHECK(!zip_install_prepare(budget, archive, games, title, 7, error, sizeof(error))
          && strstr(error, "already exists") && !budget->current);
    /* Combining marks, CJK and an emoji title with no extension. */
    static const char cjk[] = "\xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x8e\xae";
    CHECK(install_bare(budget, archive, games, cjk, 11, "APP-0000000B", cjk));
    snprintf(title, sizeof(title), "%s.zip", hebrew);
    CHECK(install_bare(budget, archive, games, title, 12, "APP-0000000C", hebrew));
    /* Titles the download namer would not keep are refused outright. */
    static const char *const unsafe[] = {"../Escape.zip", "a/b.zip", ".hidden.zip",
        "\xd9\x84\xe2\x80\xae.zip", "bad\xff.zip", ""};
    for (size_t i = 0; i < sizeof(unsafe) / sizeof(unsafe[0]); i++)
        CHECK(!zip_install_prepare(budget, archive, games, unsafe[i], 13, error, sizeof(error))
              && !budget->current);
    char names[8][ZIP_INSTALL_FOLDER_NAME_LIMIT], titles[8][ZIP_INSTALL_TITLE_LIMIT];
    size_t count = 0;
    CHECK(zip_install_list(games, names, titles, 8, &count) && count == 6);
    bool seen_arabic = false;
    for (size_t i = 0; i < count; i++)
        if (!strcmp(names[i], "APP-00000007")) seen_arabic = !strcmp(titles[i], arabic);
    CHECK(seen_arabic);
    /* The manager page shows the title and links the folder's spelling. */
    char *html = NULL; size_t length = 0;
    CHECK(offline_library_build_homebrew_page(budget, games, &html, &length));
    char hex[ZIP_INSTALL_FOLDER_HEX_LIMIT];
    zip_install_encode_folder_name("APP-00000007", hex);
    CHECK(strstr(html, arabic) && strstr(html, hex) && strstr(html, hebrew) && strstr(html, long_stem)
          && !strstr(html, ">APP-"));
    budget_free(budget, html); CHECK(!budget->current);
    /* A changed title byte breaks the receipt: the app is no longer managed. */
    char receipt_path[768];
    snprintf(receipt_path, sizeof(receipt_path), "%s/APP-0000000C/.tilefinch-homebrew", games);
    FILE *marker = fopen(receipt_path, "r+b"); CHECK(marker);
    CHECK(!fseek(marker, 80, SEEK_SET) && fputc('X', marker) != EOF && !fclose(marker));
    CHECK(zip_install_list(games, names, titles, 8, &count) && count == 5);
    CHECK(!zip_install_prepare_removal(budget, games, "APP-0000000C", error, sizeof(error)));
    char folder[600]; snprintf(folder, sizeof(folder), "%s/APP-0000000C", games);
    CHECK(erase(folder, 0));
    CHECK(remove_installed(budget, games, "APP-00000007", arabic));
    CHECK(remove_installed(budget, games, "APP-0000000A", arabic));
    CHECK(remove_installed(budget, games, "APP-00000009", long_stem));
    CHECK(remove_installed(budget, games, "APP-0000000B", cjk));
    CHECK(remove_installed(budget, games, "Plain Game", "Plain Game"));
    CHECK(zip_install_list(games, names, titles, 8, &count) && count == 0);
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
    CHECK(install && !strcmp(zip_install_preview(install)->name, "Test.v1")
          && !strcmp(zip_install_preview(install)->title, "Test.v1"));
    zip_install_destroy(install); CHECK(!budget.current);
    CHECK(bare_titles(&budget, archive, games));
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
