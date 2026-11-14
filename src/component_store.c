#include "tilefinch/component_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tilefinch/install_paths.h"

bool tilefinch_component_store_join(
    const char *directory, const char *relative,
    char *output, size_t output_size)
{
    if (directory == NULL || relative == NULL || output == NULL
        || output_size == 0) return false;
    int written = snprintf(output, output_size, "%s/%s", directory, relative);
    return written > 0 && (size_t) written < output_size;
}

bool tilefinch_component_store_exists(const char *path)
{
    struct stat information;
    return path != NULL && stat(path, &information) == 0;
}

bool tilefinch_component_store_make_parents(char *path)
{
    for (char *at = path; *at != '\0'; at++) {
        if (*at != '/') continue;
        if (at == path || (at > path && at[-1] == ':')) continue;
        char saved = *at;
        *at = '\0';
        if (mkdir(path, 0777) != 0 && errno != EEXIST) {
            *at = saved;
            return false;
        }
        *at = saved;
    }
    return true;
}

bool tilefinch_component_store_make_directory(const char *path)
{
    char mutable_path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (path == NULL) return false;
    int written = snprintf(mutable_path, sizeof(mutable_path), "%s", path);
    return written > 0 && (size_t) written < sizeof(mutable_path)
        && tilefinch_component_store_make_parents(mutable_path)
        && (mkdir(path, 0777) == 0 || errno == EEXIST);
}

static bool remove_tree_at(const char *path, unsigned depth,
                           unsigned depth_limit, size_t entry_limit,
                           size_t *entries)
{
    if (depth > depth_limit || *entries >= entry_limit) return false;
    DIR *directory = opendir(path);
    if (directory == NULL) return errno == ENOENT;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) continue;
        if (++(*entries) > entry_limit) {
            ok = false;
            break;
        }
        char child[TILEFINCH_INSTALL_PATH_LIMIT];
        if (!tilefinch_component_store_join(
                path, entry->d_name, child, sizeof(child))) {
            ok = false;
            break;
        }
        struct stat information;
        if (lstat(child, &information) != 0) ok = false;
        else if (S_ISDIR(information.st_mode))
            ok = remove_tree_at(child, depth + 1u, depth_limit, entry_limit,
                                entries);
        else
            ok = unlink(child) == 0;
    }
    if (closedir(directory) != 0) ok = false;
    return ok && rmdir(path) == 0;
}

bool tilefinch_component_store_remove_tree(
    const char *path, unsigned depth_limit, size_t entry_limit)
{
    size_t entries = 0;
    return path != NULL
        && remove_tree_at(path, 0, depth_limit, entry_limit, &entries);
}

bool tilefinch_component_store_write_file(
    const char *path, const void *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL && fwrite(bytes, 1, length, file) == length
        && fflush(file) == 0;
#if !defined(__PSP__)
    if (ok) ok = fsync(fileno(file)) == 0;
#endif
    if (file != NULL && fclose(file) != 0) ok = false;
    /* A durability hint: the checked write already succeeded, and some real
       firmware rejects every sync (tilefinch_memory_stick_sync). */
    if (ok) (void) tilefinch_memory_stick_sync();
    return ok;
}

bool tilefinch_component_store_file_matches(
    const char *path, const void *expected, size_t length)
{
    uint8_t found[32];
    if (length > sizeof(found)) return false;
    FILE *file = path == NULL ? NULL : fopen(path, "rb");
    bool ok = file != NULL
        && fread(found, 1, length, file) == length
        && fgetc(file) == EOF
        && memcmp(found, expected, length) == 0;
    if (file != NULL) fclose(file);
    return ok;
}

uint8_t *tilefinch_component_store_read_file(
    Budget *budget, const char *path, size_t limit, size_t *length)
{
    FILE *file = path == NULL ? NULL : fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    long signed_length = ftell(file);
    if (signed_length <= 0 || (unsigned long) signed_length > limit
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    size_t size = (size_t) signed_length;
    uint8_t *bytes = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, size);
    bool read = bytes != NULL
        && fread(bytes, 1, size, file) == size && fgetc(file) == EOF;
    fclose(file);
    if (!read) {
        budget_free(budget, bytes);
        return NULL;
    }
    *length = size;
    return bytes;
}

static size_t parent_length(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash == NULL ? 0u : (size_t) (slash - path);
}

bool tilefinch_component_store_rename(const char *from, const char *to)
{
    if (from == NULL || to == NULL) {
        errno = EINVAL;
        return false;
    }
    size_t parent = parent_length(from);
    if (parent != parent_length(to) || memcmp(from, to, parent) != 0) {
        errno = EXDEV;
        return false;
    }
    return rename(from, to) == 0;
}
