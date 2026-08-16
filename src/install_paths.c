#include "tilefinch/install_paths.h"

#include <stdio.h>
#include <string.h>

static bool path_copy(char *output, size_t size, const char *value)
{
    if (output == NULL || size == 0 || value == NULL) return false;
    size_t length = strlen(value);
    if (length == 0 || length >= size) return false;
    memcpy(output, value, length + 1u);
    return true;
}

static bool path_parent(const char *path, char *output, size_t size)
{
    if (path == NULL) return false;
    const char *slash = strrchr(path, '/');
    if (slash == NULL) return path_copy(output, size, ".");
    size_t length = slash == path ? 1u : (size_t) (slash - path);
    if (length >= size) return false;
    memcpy(output, path, length);
    output[length] = '\0';
    return true;
}

static bool path_join(
    const char *directory, const char *relative,
    char *output, size_t output_size)
{
    if (directory == NULL || relative == NULL || relative[0] == '\0'
        || relative[0] == '/' || strchr(relative, '\\') != NULL
        || strstr(relative, "..") != NULL) return false;
    size_t directory_length = strlen(directory);
    size_t relative_length = strlen(relative);
    if (directory_length + 1u + relative_length >= output_size)
        return false;
    memcpy(output, directory, directory_length);
    output[directory_length] = '/';
    memcpy(
        output + directory_length + 1u,
        relative, relative_length + 1u);
    return true;
}

bool tilefinch_install_paths_derive(
    const char *argv0, TilefinchInstallPaths *paths)
{
    if (argv0 == NULL || argv0[0] == '\0' || paths == NULL) return false;
    TilefinchInstallPaths derived = {0};
    if (!path_parent(
            argv0, derived.program_dir, sizeof(derived.program_dir)))
        return false;
    const char *basename = strrchr(derived.program_dir, '/');
    basename = basename == NULL ? derived.program_dir : basename + 1;
    derived.slotted = strcmp(basename, "slot-a") == 0
        || strcmp(basename, "slot-b") == 0;
    if (derived.slotted) {
        if (!path_parent(
                derived.program_dir, derived.install_root,
                sizeof(derived.install_root))) return false;
        if (!path_join(
                derived.install_root, "data", derived.data_dir,
                sizeof(derived.data_dir))) return false;
        if (!path_copy(
                derived.slot_name, sizeof(derived.slot_name), basename))
            return false;
    } else {
        if (!path_copy(
                derived.install_root, sizeof(derived.install_root),
                derived.program_dir)
            || !path_copy(
                derived.data_dir, sizeof(derived.data_dir),
                derived.program_dir)) return false;
    }
    *paths = derived;
    return true;
}

bool tilefinch_install_paths_derive_launcher(
    const char *argv0, TilefinchInstallPaths *paths)
{
    TilefinchInstallPaths derived;
    if (!tilefinch_install_paths_derive(argv0, &derived)
        || derived.slotted
        || !path_join(
            derived.install_root, "data", derived.data_dir,
            sizeof(derived.data_dir))) return false;
    *paths = derived;
    return true;
}

bool tilefinch_install_program_path(
    const TilefinchInstallPaths *paths, const char *relative,
    char *output, size_t output_size)
{
    return paths != NULL && path_join(
        paths->program_dir, relative, output, output_size);
}

bool tilefinch_install_data_path(
    const TilefinchInstallPaths *paths, const char *relative,
    char *output, size_t output_size)
{
    return paths != NULL && path_join(
        paths->data_dir, relative, output, output_size);
}

bool tilefinch_install_advisory_matches(
    const TilefinchInstallPaths *paths,
    const char *slot_name, const char *data_dir)
{
    if (paths == NULL) return false;
    bool slot_matches = slot_name == NULL || slot_name[0] == '\0'
        || (paths->slotted && strcmp(slot_name, paths->slot_name) == 0);
    bool data_matches = data_dir == NULL || data_dir[0] == '\0'
        || strcmp(data_dir, paths->data_dir) == 0;
    return slot_matches && data_matches;
}
