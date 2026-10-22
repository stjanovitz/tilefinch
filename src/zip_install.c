#include "tilefinch/zip_install.h"
#include "tilefinch/file_download.h"
#include "tilefinch/platform.h"
#include "tilefinch/update.h"
#include "tilefinch/url.h"

#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define ZIP_PATH 512u
#define ZIP_CHUNK 8192u
#define ZIP_DIRECTORY_LIMIT 512u
#define ZIP_REMOVAL_LIMIT 1024u
#define ZIP_RECEIPT ".tilefinch-homebrew"

typedef struct {
    char name[ZIP_INSTALL_NAME_LIMIT];
    uint32_t crc, compressed, unpacked, local, data, end;
    uint16_t method, flags;
    bool directory, included, created;
} ZipEntry;

struct ZipInstall {
    Budget *budget;
    FILE *archive, *output;
    ZipEntry *entries;
    char (*directories)[ZIP_INSTALL_NAME_LIMIT];
    size_t directory_count, directory_capacity, count, current, prefix;
    ZipInstallPreview view;
    ZipInstallStatus status;
    char stage[ZIP_PATH], root[256], error[128];
    uint32_t consumed, produced, crc;
    uint32_t id;
    uint8_t pbp[40];
    size_t pbp_bytes;
    z_stream z;
    bool z_active, stage_owned, receipt_owned;
    unsigned char input[ZIP_CHUNK], output_bytes[ZIP_CHUNK];
};

static uint16_t u16(const uint8_t *p) { return (uint16_t) (p[0] | (uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t) u16(p) | (uint32_t) u16(p+2) << 16; }
static bool read_at(FILE *file, uint32_t offset, void *bytes, size_t length)
{
    return fseek(file, (long) offset, SEEK_SET) == 0 && fread(bytes, 1, length, file) == length;
}

static bool fail(ZipInstall *install, const char *message)
{
    snprintf(install->error, sizeof(install->error), "%s", message);
    install->status = ZIP_INSTALL_FAILED;
    return false;
}

bool zip_install_path_candidate(const char *path)
{
    if (path == NULL) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    unsigned char signature[4];
    bool candidate = fread(signature, 1, sizeof(signature), file) == sizeof(signature)
        && u32(signature) == UINT32_C(0x04034b50);
    fclose(file);
    return candidate;
}

static bool path_valid(const char *name)
{
    size_t length = strlen(name), component = 0, depth = 0;
    if (length == 0 || length >= ZIP_INSTALL_NAME_LIMIT || name[0] == '/') return false;
    for (size_t i = 0; i <= length; i++) {
        unsigned char c = (unsigned char) name[i];
        if (c == '/' || c == 0) {
            size_t count = i - component;
            if (count == 0 && i == length && i != 0 && name[i-1] == '/') break;
            if (count == 0 || count >= 64 || ++depth > 8
                || name[i-1] == '.' || name[i-1] == ' ') return false;
            char part[64]; memcpy(part, name + component, count); part[count] = 0;
            /* Ordinary dotfiles are app data, not traversal. Validate their
               remaining basename too; '.', '..' and aliases stay forbidden. */
            const char *base = part[0] == '.' ? part + 1 : part;
            if (!*base || *base == '.' || !file_download_filename_valid(base)) return false;
            component = i + 1;
        } else if (c < 32 || c >= 127 || strchr("\\<>:\"|?*", c)) return false;
    }
    return true;
}

static bool receipt(const char *directory, const char *name, uint32_t *id, bool write)
{
    unsigned char record[80] = {0};
    char path[ZIP_PATH];
    if (strlen(directory) + sizeof("/" ZIP_RECEIPT) > sizeof(path)
        || !name || !*name || strlen(name) >= ZIP_INSTALL_FOLDER_NAME_LIMIT || name[0] == '.'
        || strchr(name, '/') || !path_valid(name)) return false;
    snprintf(path, sizeof(path), "%s/" ZIP_RECEIPT, directory);
    if (write) {
        memcpy(record, "TFHB001", 8); strcpy((char *) record + 8, name);
        for (unsigned i = 0; i < 4; i++) record[72+i] = (uint8_t)(*id >> (i*8));
        uint32_t crc = (uint32_t) crc32(0, record, 76);
        for (unsigned i = 0; i < 4; i++) record[76+i] = (uint8_t)(crc >> (i*8));
    } else {
        struct stat info;
        if (lstat(path, &info) || !S_ISREG(info.st_mode) || info.st_size != sizeof(record)) return false;
    }
    FILE *file = fopen(path, write ? "wbx" : "rb");
    if (!file) return false;
    bool okay = write ? fwrite(record, 1, sizeof(record), file) == sizeof(record)
        : fread(record, 1, sizeof(record), file) == sizeof(record) && fgetc(file) == EOF;
    if (write && fflush(file)) okay = false;
    if (ferror(file)) okay = false;
    if (fclose(file)) okay = false;
    if (!okay) { if (write) remove(path); return false; }
    if (write) return true;
    if (memcmp(record, "TFHB001", 8) || !memchr(record+8, 0, ZIP_INSTALL_FOLDER_NAME_LIMIT)
        || strcmp((char *)record+8, name) || u32(record+76) != (uint32_t)crc32(0, record, 76)
        || !u32(record+72)) return false;
    *id = u32(record+72);
    return true;
}

bool zip_install_list(const char *root, char names[][ZIP_INSTALL_FOLDER_NAME_LIMIT],
    size_t capacity, size_t *count)
{
    if (!root || !count || !capacity) return false;
    *count = 0;
    DIR *dir = opendir(root);
    if (!dir) return false;
    bool okay = true; size_t visited = 0; struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (++visited > ZIP_INSTALL_GAME_SCAN_LIMIT) { okay = false; break; }
        if (entry->d_name[0] == '.' || strlen(entry->d_name) >= ZIP_INSTALL_FOLDER_NAME_LIMIT) continue;
        char path[ZIP_PATH]; struct stat info; uint32_t id = 0;
        int length = snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) { okay = false; break; }
        if (!lstat(path, &info) && S_ISDIR(info.st_mode) && receipt(path, entry->d_name, &id, false)) {
            if (*count == capacity) { okay = false; break; }
            if (names) strcpy(names[*count], entry->d_name);
            ++*count;
        }
        if (!tilefinch_platform_cooperate("homebrew-list", visited)) { okay = false; break; }
    }
    closedir(dir);
    return okay;
}

void zip_install_encode_folder_name(const char *name,
    char output[ZIP_INSTALL_FOLDER_HEX_LIMIT])
{
    static const char digits[] = "0123456789abcdef";
    size_t used = 0;
    for (; *name && used + 2 < ZIP_INSTALL_FOLDER_HEX_LIMIT; name++) {
        output[used++] = digits[(unsigned char) *name >> 4];
        output[used++] = digits[(unsigned char) *name & 15u];
    }
    output[used] = 0;
}

bool zip_install_decode_folder_name(const char *hex,
    char output[ZIP_INSTALL_FOLDER_NAME_LIMIT])
{
    size_t length = strnlen(hex, ZIP_INSTALL_FOLDER_HEX_LIMIT);
    if (!length || length >= ZIP_INSTALL_FOLDER_HEX_LIMIT || length % 2) return false;
    for (size_t i = 0; i < length; i += 2) {
        int high = tilefinch_hex_digit((unsigned char) hex[i]);
        int low = tilefinch_hex_digit((unsigned char) hex[i + 1]);
        /* Only the encoder's lowercase spelling, and no NUL byte. */
        if (high < 0 || low < 0 || (high | low) == 0
            || (hex[i] >= 'A' && hex[i] <= 'F') || (hex[i + 1] >= 'A' && hex[i + 1] <= 'F'))
            return false;
        output[i / 2] = (char) (high * 16 + low);
    }
    output[length / 2] = 0;
    return true;
}

static bool removal_inventory(ZipInstall *job, const char *relative, unsigned depth)
{
    if (depth > 8) return false;
    char path[ZIP_PATH];
    int length = snprintf(path, sizeof(path), "%s%s%s", job->view.destination,
        *relative ? "/" : "", relative);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;
    DIR *dir = opendir(path);
    if (!dir) return false;
    bool okay = true; struct dirent *entry;
    while (okay && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!*relative && !strcmp(entry->d_name, ZIP_RECEIPT)) continue;
        char name[ZIP_INSTALL_NAME_LIMIT];
        length = snprintf(name, sizeof(name), "%s%s%s", relative, *relative ? "/" : "", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(name) || !path_valid(name)
            || job->count == ZIP_REMOVAL_LIMIT) { okay = false; break; }
        snprintf(path, sizeof(path), "%s/%s", job->view.destination, name);
        struct stat info;
        if (lstat(path, &info) || (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode))) { okay = false; break; }
        bool directory = S_ISDIR(info.st_mode);
        if (directory && !removal_inventory(job, name, depth+1)) { okay = false; break; }
        if (job->count == ZIP_REMOVAL_LIMIT || (!directory && (info.st_size < 0
            || (uint64_t)info.st_size > ZIP_INSTALL_UNPACKED_LIMIT - job->view.unpacked_bytes))) { okay = false; break; }
        ZipEntry *out = &job->entries[job->count++];
        strcpy(out->name, name); out->directory = directory;
        if (!directory) { job->view.files++; job->view.unpacked_bytes += (uint32_t)info.st_size; }
        if (!tilefinch_platform_cooperate("homebrew-check", job->count)) okay = false;
    }
    closedir(dir);
    return okay;
}

ZipInstall *zip_install_prepare_removal(Budget *budget, const char *root,
    const char *name, char *error, size_t error_capacity)
{
    ZipInstall *job = NULL; struct stat info;
    if (!budget || !root || !name || strlen(root) >= 256 || !*name || strlen(name) >= ZIP_INSTALL_FOLDER_NAME_LIMIT
        || name[0] == '.' || strchr(name, '/') || !path_valid(name)) goto refused;
    job = budget_calloc_category(budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*job));
    if (!job) goto refused;
    job->budget = budget; strcpy(job->view.name, name); strcpy(job->root, root);
    snprintf(job->view.destination, sizeof(job->view.destination), "%s/%s", root, name);
    job->view.removing = true;
    if (lstat(job->view.destination, &info) || !S_ISDIR(info.st_mode)
        || !receipt(job->view.destination, name, &job->id, false)) goto refused;
    job->entries = budget_calloc_category(budget, BUDGET_CATEGORY_SESSION, ZIP_REMOVAL_LIMIT, sizeof(ZipEntry));
    if (!job->entries || !removal_inventory(job, "", 0)) goto refused;
    return job;
refused:
    if (error && error_capacity) snprintf(error, error_capacity,
        "Removal refused: no valid Tilefinch receipt, unsafe contents, cancelled, or out of memory");
    zip_install_destroy(job); return NULL;
}

static bool extras(FILE *file, uint32_t offset, uint16_t length)
{
    if (length > 4096) return false;
    for (uint32_t used = 0; used < length;) {
        uint8_t header[4];
        if (length - used < 4 || !read_at(file, offset + used, header, 4)) return false;
        uint16_t size = u16(header + 2);
        if (u16(header) == 1 || size > length - used - 4) return false; /* No ZIP64. */
        used += 4u + size;
    }
    return true;
}

static bool local_header(ZipInstall *install, ZipEntry *entry, uint32_t central)
{
    uint8_t h[30]; char name[ZIP_INSTALL_NAME_LIMIT];
    if (entry->local > central || central - entry->local < sizeof(h)
        || !read_at(install->archive, entry->local, h, sizeof(h))
        || u32(h) != UINT32_C(0x04034b50) || u16(h+4) > 20 || u16(h+6) != entry->flags
        || u16(h+8) != entry->method || u16(h+26) != strlen(entry->name)) return false;
    uint32_t after = entry->local + 30u;
    uint16_t names = u16(h+26), extra = u16(h+28);
    if (names > central - after || extra > central - after - names
        || !read_at(install->archive, after, name, names)
        || memcmp(name, entry->name, names)
        || !extras(install->archive, after + names, extra)) return false;
    if (!(entry->flags & 8u) && (u32(h+14) != entry->crc
        || u32(h+18) != entry->compressed || u32(h+22) != entry->unpacked)) return false;
    if ((entry->flags & 8u) && ((u32(h+14) && u32(h+14) != entry->crc)
        || (u32(h+18) && u32(h+18) != entry->compressed)
        || (u32(h+22) && u32(h+22) != entry->unpacked))) return false;
    entry->data = after + names + extra;
    if (entry->compressed > central - entry->data) return false;
    entry->end = entry->data + entry->compressed;
    /* A data descriptor is not trusted for sizes, but must agree if used. */
    if (entry->flags & 8u) {
        uint8_t descriptor[16];
        if (central - entry->end < 12 || !read_at(install->archive, entry->end, descriptor, 12)) return false;
        size_t start = u32(descriptor) == UINT32_C(0x08074b50) ? 4 : 0;
        if (start && (central - entry->end < 16
            || !read_at(install->archive, entry->end, descriptor, 16))) return false;
        if (u32(descriptor+start) != entry->crc || u32(descriptor+start+4) != entry->compressed
            || u32(descriptor+start+8) != entry->unpacked) return false;
        entry->end += (uint32_t) start + 12;
    }
    return true;
}

static bool root_documentation(const char *name)
{
    if (strchr(name, '/')) return false;
    const char *extension = strrchr(name, '.');
    return !strncasecmp(name, "readme", 6) || !strncasecmp(name, "license", 7)
        || !strncasecmp(name, "copying", 7) || !strncasecmp(name, "changelog", 9)
        || (extension && (!strcasecmp(extension, ".txt") || !strcasecmp(extension, ".md")));
}

static bool desktop_metadata(const char *name)
{
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    return !strncasecmp(name, "__MACOSX/", 9) || !strcasecmp(base, ".DS_Store")
        || (base[0] == '.' && base[1] == '_');
}

static bool choose_root(ZipInstall *install, const char *suggested)
{
    ZipEntry *boot = NULL;
    for (size_t i = 0; i < install->count; i++) {
        ZipEntry *entry = &install->entries[i];
        const char *base = strrchr(entry->name, '/'); base = base ? base + 1 : entry->name;
        if (!entry->directory && !strcasecmp(base, "EBOOT.PBP")) {
            if (boot != NULL) return false;
            boot = entry;
        }
    }
    if (boot == NULL || boot->unpacked < 40) return false;
    install->prefix = strlen(boot->name) - strlen("EBOOT.PBP");
    char prefix[ZIP_INSTALL_NAME_LIMIT];
    memcpy(prefix, boot->name, install->prefix); prefix[install->prefix] = 0;
    if (install->prefix) {
        prefix[install->prefix - 1] = 0;
        char *base = strrchr(prefix, '/'); base = base ? base+1 : prefix;
        if (strlen(base) >= sizeof(install->view.name)) return false;
        strcpy(install->view.name, base);
    } else {
        const char *extension = strrchr(suggested, '.');
        size_t length = extension ? (size_t) (extension - suggested) : strlen(suggested);
        if (!length || length >= sizeof(install->view.name)) return false;
        memcpy(install->view.name, suggested, length);
    }
    if (install->view.name[0] == '.' || !path_valid(install->view.name)
        || strchr(install->view.name, '/')) return false;
    snprintf(install->view.destination, sizeof(install->view.destination), "%s/%s", install->root, install->view.name);
    for (size_t i = 0; i < install->count; i++) {
        ZipEntry *entry = &install->entries[i];
        entry->included = !install->prefix || (!strncasecmp(entry->name, boot->name, install->prefix)
            && strlen(entry->name) > install->prefix);
        if (desktop_metadata(entry->name)) entry->included = false;
        if (entry->included && !strcasecmp(entry->name + install->prefix, ZIP_RECEIPT)) return false;
        if (!entry->included && !entry->directory && !root_documentation(entry->name)
            && !desktop_metadata(entry->name)) return false;
        if (!entry->directory) {
            if (entry->included) {
                if (entry->unpacked > ZIP_INSTALL_UNPACKED_LIMIT - install->view.unpacked_bytes) return false;
                install->view.unpacked_bytes += entry->unpacked;
                install->view.files++;
            } else install->view.skipped_files++;
        }
    }
    return true;
}

static bool directory_read(ZipInstall *install, uint32_t size, const char *suggested)
{
    size_t tail_size = size < 65557u ? size : 65557u;
    uint8_t *tail = budget_malloc_category(install->budget, BUDGET_CATEGORY_SESSION, tail_size);
    if (tail == NULL) return false;
    bool found = false; uint32_t central = 0, bytes = 0, eocd = 0; uint16_t count = 0;
    if (tail_size >= 22 && read_at(install->archive, size - (uint32_t) tail_size, tail, tail_size)) {
        for (size_t i = tail_size - 22;; i--) {
            if (u32(tail+i) == UINT32_C(0x06054b50) && u16(tail+i+20) == tail_size-i-22) {
                count = u16(tail+i+10); central = u32(tail+i+16); bytes = u32(tail+i+12);
                eocd = size - (uint32_t)tail_size + (uint32_t)i;
                found = !u16(tail+i+4) && !u16(tail+i+6) && count == u16(tail+i+8)
                    && count && count <= ZIP_INSTALL_ENTRY_LIMIT && central <= eocd && bytes <= eocd-central;
                break;
            }
            if (!i) break;
        }
    }
    budget_free(install->budget, tail);
    if (!found || bytes == UINT32_MAX) return false;
    install->entries = budget_calloc_category(install->budget, BUDGET_CATEGORY_SESSION, count, sizeof(ZipEntry));
    if (install->entries == NULL) return false;
    install->count = count;
    uint32_t at = central;
    for (size_t i = 0; i < count; i++) {
        uint8_t h[46]; ZipEntry *entry = &install->entries[i];
        if (at > central + bytes || central + bytes - at < sizeof(h)
            || !read_at(install->archive, at, h, sizeof(h)) || u32(h) != UINT32_C(0x02014b50)) return false;
        uint16_t name = u16(h+28), extra = u16(h+30), comment = u16(h+32);
        entry->flags = u16(h+8); entry->method = u16(h+10); entry->crc = u32(h+16);
        entry->compressed = u32(h+20); entry->unpacked = u32(h+24); entry->local = u32(h+42);
        if (!name || name >= sizeof(entry->name) || extra > 4096 || comment > 1024 || u16(h+34)
            || u16(h+6) > 20 || (entry->flags & ~UINT16_C(0x080e))
            || (entry->method != 0 && entry->method != 8)
            || (entry->method == 0 && ((entry->flags & 6u) || entry->compressed != entry->unpacked))
            || entry->unpacked > ZIP_INSTALL_FILE_LIMIT || entry->compressed > size
            || name + extra + comment > central + bytes - at - sizeof(h)) return false;
        if (!read_at(install->archive, at+46, entry->name, name)
            || memchr(entry->name, 0, name) || !path_valid(entry->name)
            || !extras(install->archive, at+46+name, extra)) return false;
        entry->directory = entry->name[name-1] == '/';
        uint32_t attributes = u32(h+38), type = (attributes >> 16) & 0170000u;
        if ((attributes & 8u) || ((attributes & 16u) && !entry->directory)
            || (((u16(h+4) >> 8) == 3) && type
            && type != (entry->directory ? 0040000u : 0100000u))
            || (entry->directory && (entry->compressed || entry->unpacked))) return false;
        if (!local_header(install, entry, central)) return false;
        for (size_t j = 0; j < i; j++) {
            ZipEntry *prior = &install->entries[j];
            size_t a = strlen(entry->name), b = strlen(prior->name);
            if (entry->directory) a--; if (prior->directory) b--;
            if ((a == b && !strncasecmp(entry->name, prior->name, a))
                || (!entry->directory && b > a && prior->name[a] == '/' && !strncasecmp(entry->name, prior->name, a))
                || (!prior->directory && a > b && entry->name[b] == '/' && !strncasecmp(entry->name, prior->name, b))
                || (entry->local < prior->end && prior->local < entry->end)) return false;
            /* FAT aliases case. Refuse conflicting spellings of any shared
               directory rather than creating host/PSP-dependent trees. */
            for (size_t k = 0; k <= a && k <= b; k++) {
                if (entry->name[k] == '/' && prior->name[k] == '/'
                    && !strncasecmp(entry->name, prior->name, k)
                    && memcmp(entry->name, prior->name, k)) return false;
            }
        }
        at += 46u + name + extra + comment;
        if (!tilefinch_platform_cooperate("zip-preview", i+1)) return false;
    }
    return at == central + bytes && choose_root(install, suggested);
}

ZipInstall *zip_install_prepare(Budget *budget, const char *archive,
    const char *game_directory, const char *suggested_name, uint32_t id,
    char *error, size_t error_capacity)
{
    ZipInstall *install = NULL;
    if (!budget || !archive || !game_directory || !suggested_name || !id
        || strlen(game_directory) >= 256 || !file_download_filename_valid(suggested_name)) goto refused;
    install = budget_calloc_category(budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*install));
    if (!install) goto refused;
    install->budget = budget;
    install->id = id;
    strcpy(install->root, game_directory);
    snprintf(install->stage, sizeof(install->stage), "%s/.tilefinch-unpack-%08x", game_directory, (unsigned)id);
    install->archive = fopen(archive, "rb");
    struct stat status;
    if (!install->archive || fstat(fileno(install->archive), &status) || !S_ISREG(status.st_mode)
        || status.st_size < 22 || (uint64_t)status.st_size > FILE_DOWNLOAD_LIMIT
        || !directory_read(install, (uint32_t) status.st_size, suggested_name)) goto refused;
    if (lstat(install->view.destination, &status) == 0 || errno != ENOENT) {
        if (error && error_capacity) snprintf(error, error_capacity, "Destination already exists; nothing was replaced");
        zip_install_destroy(install); return NULL;
    }
    /* Keep every installer-created folder reachable in the bounded manager.
       ZIP deletion does not free this independent installation slot. */
    size_t installed_count = 0;
    if (!zip_install_list(game_directory, NULL, ZIP_INSTALL_MANAGED_LIMIT, &installed_count)
        || installed_count == ZIP_INSTALL_MANAGED_LIMIT) goto refused;
    return install;
refused:
    if (error && error_capacity) snprintf(error, error_capacity,
        "ZIP refused: unsupported, unsafe, ambiguous, cancelled, or out of memory");
    zip_install_destroy(install); return NULL;
}

const ZipInstallPreview *zip_install_preview(const ZipInstall *install) { return install ? &install->view : NULL; }
const char *zip_install_error(const ZipInstall *install) { return install ? install->error : "ZIP installation unavailable"; }

static voidpf zalloc_budget(voidpf opaque, uInt count, uInt size)
{
    return budget_calloc_category(opaque, BUDGET_CATEGORY_SESSION, count, size);
}
static void zfree_budget(voidpf opaque, voidpf pointer) { budget_free(opaque, pointer); }

static void end_entry(ZipInstall *install)
{
    if (install->z_active) { inflateEnd(&install->z); install->z_active = false; }
    if (install->output) { fclose(install->output); install->output = NULL; }
}

static bool parents(ZipInstall *install, const char *name)
{
    char relative[ZIP_INSTALL_NAME_LIMIT]; snprintf(relative, sizeof(relative), "%s", name);
    for (size_t i = 0; relative[i]; i++) {
        if (relative[i] != '/') continue;
        relative[i] = 0;
        bool known = false;
        for (size_t j = 0; j < install->directory_count; j++)
            if (!strcasecmp(install->directories[j], relative)) { known = true; break; }
        if (!known) {
            if (install->directory_count == ZIP_DIRECTORY_LIMIT) return false;
            if (install->directory_count == install->directory_capacity) {
                size_t capacity = install->directory_capacity ? install->directory_capacity * 2 : 8;
                void *grown = budget_realloc_category(install->budget, BUDGET_CATEGORY_SESSION,
                    install->directories, capacity * sizeof(*install->directories));
                if (!grown) return false;
                install->directories = grown; install->directory_capacity = capacity;
            }
            char path[ZIP_PATH]; snprintf(path, sizeof(path), "%s/%s", install->stage, relative);
            if (mkdir(path, 0700)) return false;
            strcpy(install->directories[install->directory_count++], relative);
        }
        relative[i] = '/';
    }
    return true;
}

bool zip_install_begin(ZipInstall *install)
{
    if (!install || install->status != ZIP_INSTALL_PREVIEW) return false;
    if (install->view.removing) {
        uint32_t id = 0;
        struct stat info;
        if (lstat(install->view.destination, &info) || !S_ISDIR(info.st_mode)
            || !receipt(install->view.destination, install->view.name, &id, false) || id != install->id)
            return fail(install, "Installation changed; removal refused");
        install->status = ZIP_INSTALL_PENDING; return true;
    }
    uint64_t space = 0; struct stat exists;
    if (!tilefinch_update_query_free_space(install->root, &space)
        || !offline_download_space_admits(space, install->view.unpacked_bytes))
        return fail(install, "Not enough free Memory Stick space");
    if (!lstat(install->view.destination, &exists) || errno != ENOENT)
        return fail(install, "Destination already exists; nothing was replaced");
    if (mkdir(install->stage, 0700))
        return fail(install, "Cannot create staging folder; an unfinished installation may exist");
    install->stage_owned = true;
    install->status = ZIP_INSTALL_PENDING;
    return true;
}

static bool begin_entry(ZipInstall *install, ZipEntry *entry)
{
    const char *name = entry->name + install->prefix;
    if (!parents(install, name)) return fail(install, "Cannot create installation directories");
    char path[ZIP_PATH]; snprintf(path, sizeof(path), "%s/%s", install->stage, name);
    install->output = fopen(path, "wb");
    if (!install->output) return fail(install, "Cannot create installed file");
    entry->created = true;
    install->consumed = install->produced = 0;
    install->crc = (uint32_t) crc32(0, Z_NULL, 0);
    install->pbp_bytes = 0;
    if (fseek(install->archive, (long) entry->data, SEEK_SET)) return fail(install, "ZIP input read failed");
    if (entry->method == 8) {
        install->z = (z_stream) {.zalloc = zalloc_budget, .zfree = zfree_budget, .opaque = install->budget};
        if (inflateInit2(&install->z, -MAX_WBITS) != Z_OK) return fail(install, "Not enough memory to unpack ZIP");
        install->z_active = true;
    }
    return true;
}

static bool valid_pbp(const ZipInstall *install, const ZipEntry *entry)
{
    if (strcasecmp(entry->name + install->prefix, "EBOOT.PBP")) return true;
    if (install->pbp_bytes != 40 || memcmp(install->pbp, "\0PBP", 4)) return false;
    uint32_t previous = 40;
    for (size_t at = 8; at < 40; at += 4) {
        uint32_t offset = u32(install->pbp + at);
        if (offset < previous || offset > entry->unpacked) return false;
        previous = offset;
    }
    return true;
}

ZipInstallStatus zip_install_pump(ZipInstall *install)
{
    if (!install) return ZIP_INSTALL_FAILED;
    if (install->status != ZIP_INSTALL_PENDING) return install->status;
    uint64_t start = tilefinch_platform_monotonic_time_us();
    if (install->view.removing) {
        for (unsigned steps = 0; steps < 8; steps++) {
            char path[ZIP_PATH];
            if (install->current == install->count) {
                snprintf(path, sizeof(path), "%s/" ZIP_RECEIPT, install->view.destination);
                if (remove(path) || rmdir(install->view.destination)) {
                    /* Preserve discoverability if a concurrent new file prevents
                       the final rmdir. A retry inventories only what remains. */
                    (void) receipt(install->view.destination, install->view.name, &install->id, true);
                    fail(install, "Removal incomplete; retry or finish through USB");
                } else install->status = ZIP_INSTALL_DONE;
                break;
            }
            ZipEntry *entry = &install->entries[install->current];
            snprintf(path, sizeof(path), "%s/%s", install->view.destination, entry->name);
            struct stat info;
            if (lstat(path, &info) == 0) {
                if ((entry->directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))
                    || (entry->directory ? rmdir(path) : remove(path))) {
                    fail(install, "Removal incomplete; contents changed or write failed"); break;
                }
            } else if (errno != ENOENT) { fail(install, "Removal read failed"); break; }
            install->current++;
            uint64_t now = tilefinch_platform_monotonic_time_us();
            if (now >= start && now - start >= 2000) break;
        }
        return install->status;
    }
    size_t written = 0;
    for (unsigned steps = 0; steps < 16 && written < 32768; steps++) {
        if (install->current == install->count) {
            struct stat exists;
            bool marked = receipt(install->stage, install->view.name, &install->id, true);
            install->receipt_owned = marked;
            if (!marked || !lstat(install->view.destination, &exists) || errno != ENOENT
                || rename(install->stage, install->view.destination))
                fail(install, "Completed installation could not be published; nothing was replaced");
            else { install->stage_owned = false; install->status = ZIP_INSTALL_DONE; }
            break;
        }
        ZipEntry *entry = &install->entries[install->current];
        if (!entry->included) { install->current++; continue; }
        if (entry->directory) {
            if (!parents(install, entry->name + install->prefix)) {
                fail(install, "Cannot create installation directories"); break;
            }
            install->current++; continue;
        }
        if (!install->output && !begin_entry(install, entry)) break;
        uint32_t remaining = entry->compressed - install->consumed;
        size_t capacity = 32768u - written;
        if (capacity > ZIP_CHUNK) capacity = ZIP_CHUNK;
        size_t count = 0; bool ended = false;
        if (!entry->method) {
            count = remaining < capacity ? remaining : capacity;
            if (fread(install->output_bytes, 1, count, install->archive) != count) {
                fail(install, "ZIP input was truncated"); break;
            }
            install->consumed += (uint32_t) count; ended = install->consumed == entry->compressed;
        } else {
            if (!install->z.avail_in && remaining) {
                size_t wanted = remaining < ZIP_CHUNK ? remaining : ZIP_CHUNK;
                if (fread(install->input, 1, wanted, install->archive) != wanted) {
                    fail(install, "ZIP input was truncated"); break;
                }
                install->z.next_in = install->input; install->z.avail_in = (uInt) wanted;
                install->consumed += (uint32_t) wanted;
            }
            uInt before = install->z.avail_in;
            install->z.next_out = install->output_bytes; install->z.avail_out = (uInt) capacity;
            int result = inflate(&install->z, Z_NO_FLUSH);
            count = capacity - install->z.avail_out; ended = result == Z_STREAM_END;
            if ((result != Z_OK && !ended) || (!count && before == install->z.avail_in)) {
                fail(install, "ZIP compressed data is invalid"); break;
            }
        }
        if (install->produced > entry->unpacked || count > entry->unpacked - install->produced
            || fwrite(install->output_bytes, 1, count, install->output) != count) {
            fail(install, "ZIP size limit or Memory Stick write failed"); break;
        }
        size_t keep = sizeof(install->pbp) - install->pbp_bytes;
        if (keep > count) keep = count;
        memcpy(install->pbp + install->pbp_bytes, install->output_bytes, keep); install->pbp_bytes += keep;
        install->crc = (uint32_t) crc32(install->crc, install->output_bytes, (uInt) count);
        install->produced += (uint32_t) count; written += count;
        if (ended) {
            bool okay = install->produced == entry->unpacked && install->consumed == entry->compressed
                && (!install->z_active || !install->z.avail_in) && install->crc == entry->crc
                && valid_pbp(install, entry) && !fflush(install->output) && !ferror(install->output);
            if (fclose(install->output)) okay = false;
            install->output = NULL; end_entry(install);
            if (!okay) { fail(install, "ZIP checksum, size, application header, or write verification failed"); break; }
            install->current++;
        }
        uint64_t now = tilefinch_platform_monotonic_time_us();
        if (now >= start && now - start >= 2000) break;
    }
    return install->status;
}

void zip_install_cancel(ZipInstall *install)
{
    if (!install || install->status == ZIP_INSTALL_DONE) return;
    end_entry(install); install->status = ZIP_INSTALL_CANCELLED;
}

void zip_install_destroy(ZipInstall *install)
{
    if (!install) return;
    end_entry(install);
    if (install->archive) fclose(install->archive);
    if (install->stage_owned) {
        char path[ZIP_PATH];
        if (install->receipt_owned) {
            snprintf(path, sizeof(path), "%s/" ZIP_RECEIPT, install->stage);
            (void) remove(path);
        }
        for (size_t i = 0; i < install->count; i++) if (install->entries[i].created) {
            snprintf(path, sizeof(path), "%s/%s", install->stage, install->entries[i].name + install->prefix);
            (void) remove(path);
            (void) tilefinch_platform_cooperate("zip-cleanup", i+1);
        }
        for (size_t i = install->directory_count; i != 0; i--) {
            snprintf(path, sizeof(path), "%s/%s", install->stage, install->directories[i-1]);
            (void) rmdir(path);
            (void) tilefinch_platform_cooperate("zip-cleanup", install->count + install->directory_count - i + 1);
        }
        (void) rmdir(install->stage);
    }
    budget_free(install->budget, install->directories);
    budget_free(install->budget, install->entries);
    budget_free(install->budget, install);
}
