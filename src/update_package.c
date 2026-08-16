#include "tilefinch/update.h"

#include <string.h>

static uint16_t package_u16(const uint8_t *bytes)
{
    return (uint16_t) ((uint16_t) bytes[0] << 8 | bytes[1]);
}

static uint32_t package_u32(const uint8_t *bytes)
{
    return (uint32_t) bytes[0] << 24
        | (uint32_t) bytes[1] << 16
        | (uint32_t) bytes[2] << 8 | bytes[3];
}

static uint64_t package_u64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (size_t index = 0; index < 8; index++)
        value = value << 8 | bytes[index];
    return value;
}

static bool path_component_valid(const char *start, size_t length)
{
    if (length == 0 || (length == 1 && start[0] == '.')
        || (length == 2 && start[0] == '.' && start[1] == '.')) return false;
    for (size_t index = 0; index < length; index++) {
        unsigned value = (unsigned char) start[index];
        if (!((value >= 'a' && value <= 'z')
              || (value >= 'A' && value <= 'Z')
              || (value >= '0' && value <= '9')
              || value == '.' || value == '_' || value == '-')) return false;
    }
    return true;
}

static bool package_path_safe(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/'
        || strchr(path, '\\') != NULL || strchr(path, ':') != NULL)
        return false;
    const char *component = path;
    for (const char *at = path;; at++) {
        if (*at != '/' && *at != '\0') continue;
        if (!path_component_valid(component, (size_t) (at - component)))
            return false;
        if (*at == '\0') break;
        component = at + 1;
    }
    return true;
}

bool tilefinch_update_package_path_allowed(const char *path)
{
    static const char *const exact[] = {
        "EBOOT.PBP", "roots.pem", "boot-defaults.cfg"
    };
    if (!package_path_safe(path)) return false;
    for (size_t index = 0; index < sizeof(exact) / sizeof(exact[0]); index++)
        if (strcmp(path, exact[index]) == 0) return true;
    return strncmp(path, "fonts/", 6) == 0;
}

bool tilefinch_update_voice_package_path_allowed(const char *path)
{
    if (!package_path_safe(path)) return false;
    return strcmp(path, "model-info.tfv") == 0
        || strncmp(path, "model/", 6) == 0
        || strncmp(path, "LICENSES/", 9) == 0;
}

static bool paths_collide(const char *left, const char *right)
{
    size_t left_length = strlen(left), right_length = strlen(right);
    if (strcmp(left, right) == 0) return true;
    return (left_length < right_length
            && memcmp(left, right, left_length) == 0
            && right[left_length] == '/')
        || (right_length < left_length
            && memcmp(left, right, right_length) == 0
            && left[right_length] == '/');
}

static TilefinchUpdateStatus parse_package_table(
    const uint8_t *bytes, size_t length, uint64_t package_size,
    TilefinchUpdatePackage *package, const uint8_t magic[8],
    bool (*path_allowed)(const char *))
{
    if (bytes == NULL || package == NULL)
        return TILEFINCH_UPDATE_INVALID_ARGUMENT;
    if (length < 16u) return TILEFINCH_UPDATE_TRUNCATED;
    if (memcmp(bytes, magic, 8u) != 0)
        return TILEFINCH_UPDATE_BAD_MAGIC;
    uint16_t schema = package_u16(bytes + 8);
    uint16_t count = package_u16(bytes + 10);
    uint32_t table_length = package_u32(bytes + 12);
    if (schema != 1) return TILEFINCH_UPDATE_UNSUPPORTED_SCHEMA;
    if (count == 0 || count > TILEFINCH_UPDATE_MAX_FILES
        || table_length > TILEFINCH_UPDATE_MAX_PACKAGE_TABLE_BYTES - 16u)
        return TILEFINCH_UPDATE_LIMIT;
    if (table_length > length - 16u) return TILEFINCH_UPDATE_TRUNCATED;
    uint64_t payload_start = UINT64_C(16) + table_length;
    if (payload_start > package_size) return TILEFINCH_UPDATE_TRUNCATED;
    TilefinchUpdatePackage parsed = {
        .file_count = count,
        .table_length = table_length,
        .payload_start = payload_start
    };
    size_t at = 16, table_end = 16u + table_length;
    uint64_t expected_offset = payload_start;
    for (size_t index = 0; index < count; index++) {
        if (at >= table_end) return TILEFINCH_UPDATE_TRUNCATED;
        uint8_t path_length = bytes[at++];
        if (path_length == 0 || path_length > TILEFINCH_UPDATE_MAX_PATH_BYTES
            || path_length > table_end - at) return TILEFINCH_UPDATE_BAD_PATH;
        memcpy(parsed.entries[index].path, bytes + at, path_length);
        parsed.entries[index].path[path_length] = '\0';
        at += path_length;
        if (!path_allowed(parsed.entries[index].path))
            return TILEFINCH_UPDATE_BAD_PATH;
        if (table_end - at < 48u) return TILEFINCH_UPDATE_TRUNCATED;
        parsed.entries[index].size = package_u64(bytes + at);
        at += 8;
        memcpy(parsed.entries[index].sha256, bytes + at, 32);
        at += 32;
        parsed.entries[index].payload_offset = package_u64(bytes + at);
        at += 8;
        if (parsed.entries[index].size == 0
            || parsed.entries[index].payload_offset != expected_offset
            || parsed.entries[index].size > package_size - expected_offset) {
            return TILEFINCH_UPDATE_PACKAGE_MISMATCH;
        }
        expected_offset += parsed.entries[index].size;
        for (size_t prior = 0; prior < index; prior++)
            if (paths_collide(parsed.entries[prior].path,
                              parsed.entries[index].path))
                return TILEFINCH_UPDATE_DUPLICATE_PATH;
    }
    if (at != table_end || expected_offset != package_size)
        return TILEFINCH_UPDATE_PACKAGE_MISMATCH;
    *package = parsed;
    return TILEFINCH_UPDATE_OK;
}

TilefinchUpdateStatus tilefinch_update_parse_package_table(
    const uint8_t *bytes, size_t length, uint64_t package_size,
    TilefinchUpdatePackage *package)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'U', 'P', 'v', '1', 0, 0
    };
    return parse_package_table(
        bytes, length, package_size, package, magic,
        tilefinch_update_package_path_allowed);
}

TilefinchUpdateStatus tilefinch_update_parse_voice_package_table(
    const uint8_t *bytes, size_t length, uint64_t package_size,
    TilefinchUpdatePackage *package)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'V', 'P', 'v', '1', 0, 0
    };
    return parse_package_table(
        bytes, length, package_size, package, magic,
        tilefinch_update_voice_package_path_allowed);
}
