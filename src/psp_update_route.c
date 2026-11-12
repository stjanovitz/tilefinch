#include "psp_update_route.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define ROUTE_ROOT_PREFIX "ms0:/PSP/GAME/"
#define ROUTE_NAME_LIMIT 32u

static bool route_refuse(char *why, size_t why_size, const char *reason)
{
    if (why != NULL && why_size != 0) snprintf(why, why_size, "%s", reason);
    return false;
}

/* A plain directory name directly under ms0:/PSP/GAME, and not the user's
   own install: a throwaway-root candidate would be rejected by that
   install's launcher, but the installer would already have replaced its
   inactive slot. */
static bool route_root_valid(const char *root, char *why, size_t why_size)
{
    size_t prefix = sizeof(ROUTE_ROOT_PREFIX) - 1u;
    if (strncmp(root, ROUTE_ROOT_PREFIX, prefix) != 0)
        return route_refuse(why, why_size, "install_root not under "
                            ROUTE_ROOT_PREFIX);
    const char *name = root + prefix;
    size_t length = strlen(name);
    if (length == 0 || length > ROUTE_NAME_LIMIT)
        return route_refuse(why, why_size, "install_root name length");
    for (size_t at = 0; at < length; at++) {
        unsigned character = (unsigned char) name[at];
        if (!((character >= 'a' && character <= 'z')
              || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9')
              || character == '-' || character == '_'))
            return route_refuse(why, why_size,
                                "install_root name character");
    }
    if (strcasecmp(name, "TILEFINCH") == 0)
        return route_refuse(why, why_size,
                            "install_root is the main install");
    return true;
}

bool psp_update_route_parse(const char *text, size_t length,
                            PspUpdateRoute *route,
                            char *why, size_t why_size)
{
    if (text == NULL || route == NULL)
        return route_refuse(why, why_size, "no text");
    char root[TILEFINCH_INSTALL_PATH_LIMIT] = {0};
    char slot = '\0';
    bool have_root = false;
    bool journal_only = false;
    size_t at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n') end++;
        size_t line_length = end - at;
        if (line_length != 0 && text[at + line_length - 1u] == '\r')
            line_length--;
        char line[TILEFINCH_INSTALL_PATH_LIMIT + 32u];
        if (line_length >= sizeof(line))
            return route_refuse(why, why_size, "line too long");
        memcpy(line, text + at, line_length);
        line[line_length] = '\0';
        at = end + 1u;
        if (memchr(line, '\0', line_length) != NULL)
            return route_refuse(why, why_size, "NUL in line");
        if (line[0] == '\0' || line[0] == '#') continue;
        if (strncmp(line, "install_root=", 13) == 0) {
            if (have_root)
                return route_refuse(why, why_size, "install_root repeated");
            if (strlen(line + 13) >= sizeof(root))
                return route_refuse(why, why_size, "install_root too long");
            snprintf(root, sizeof(root), "%s", line + 13);
            have_root = true;
        } else if (strncmp(line, "running_slot=", 13) == 0) {
            if (slot != '\0')
                return route_refuse(why, why_size, "running_slot repeated");
            if (strcmp(line + 13, "a") != 0 && strcmp(line + 13, "b") != 0)
                return route_refuse(why, why_size, "running_slot not a or b");
            slot = line[13];
        } else if (strncmp(line, "journal_only=", 13) == 0) {
            if (journal_only)
                return route_refuse(why, why_size, "journal_only repeated");
            if (strcmp(line + 13, "1") != 0)
                return route_refuse(why, why_size, "journal_only not 1");
            journal_only = true;
        } else {
            return route_refuse(why, why_size, "unknown key");
        }
    }
    if (!have_root) return route_refuse(why, why_size, "no install_root");
    if (slot == '\0') return route_refuse(why, why_size, "no running_slot");
    if (!route_root_valid(root, why, why_size)) return false;
    char argv0[TILEFINCH_INSTALL_PATH_LIMIT];
    int written = snprintf(argv0, sizeof(argv0), "%s/slot-%c/EBOOT.PBP",
                           root, slot);
    PspUpdateRoute parsed = {.running_slot = slot,
                             .journal_only = journal_only};
    if (written <= 0 || (size_t) written >= sizeof(argv0)
        || !tilefinch_install_paths_derive(argv0, &parsed.paths)
        || !parsed.paths.slotted)
        return route_refuse(why, why_size, "paths not derivable");
    *route = parsed;
    return true;
}
