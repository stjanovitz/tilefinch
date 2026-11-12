/* The recorded update route's sidecar parser (src/psp_update_route.h): the
   validation seam may only ever point the update session at a separate
   test install on the Memory Stick, never at the user's own. */
#include "../src/psp_update_route.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

static bool parse(const char *text, PspUpdateRoute *route, char *why)
{
    return psp_update_route_parse(text, strlen(text), route, why, 64);
}

static bool refused(const char *text, const char *reason)
{
    PspUpdateRoute route;
    memset(&route, 0x5a, sizeof(route));
    unsigned char untouched[sizeof(route)];
    memcpy(untouched, &route, sizeof(route));
    char why[64] = "";
    bool accepted = parse(text, &route, why);
    if (accepted || strstr(why, reason) == NULL
        || memcmp(untouched, &route, sizeof(route)) != 0) {
        fprintf(stderr, "expected refusal \"%s\" for <%s>, got %d \"%s\"\n",
                reason, text, (int) accepted, why);
        return false;
    }
    return true;
}

int main(void)
{
    PspUpdateRoute route;
    char why[64] = "";

    /* The route acts as slot A of a separate install: the session's slot,
       install root and data directory are the ones an XMB start of
       ms0:/PSP/GAME/TFUPTEST/slot-a/EBOOT.PBP would derive. */
    CHECK(parse("# recorded update route\r\n"
                "install_root=ms0:/PSP/GAME/TFUPTEST\r\n\r\n"
                "running_slot=a\r\n", &route, why));
    CHECK(route.running_slot == 'a');
    CHECK(!route.journal_only);
    CHECK(route.paths.slotted);
    CHECK(strcmp(route.paths.slot_name, "slot-a") == 0);
    CHECK(strcmp(route.paths.program_dir,
                 "ms0:/PSP/GAME/TFUPTEST/slot-a") == 0);
    CHECK(strcmp(route.paths.install_root, "ms0:/PSP/GAME/TFUPTEST") == 0);
    CHECK(strcmp(route.paths.data_dir, "ms0:/PSP/GAME/TFUPTEST/data") == 0);

    CHECK(parse("running_slot=b\ninstall_root=ms0:/PSP/GAME/tf-up_2", &route,
                why));
    CHECK(route.running_slot == 'b');
    CHECK(strcmp(route.paths.slot_name, "slot-b") == 0);
    CHECK(strcmp(route.paths.data_dir, "ms0:/PSP/GAME/tf-up_2/data") == 0);

    /* A report-only route: same paths, nothing written. */
    CHECK(parse("install_root=ms0:/PSP/GAME/TFUPTEST\njournal_only=1\n"
                "running_slot=b\n", &route, why));
    CHECK(route.journal_only && route.running_slot == 'b');
    CHECK(strcmp(route.paths.data_dir, "ms0:/PSP/GAME/TFUPTEST/data") == 0);
    CHECK(refused("install_root=ms0:/PSP/GAME/TFUPTEST\nrunning_slot=a\n"
                  "journal_only=0\n", "journal_only not 1"));
    CHECK(refused("install_root=ms0:/PSP/GAME/TFUPTEST\nrunning_slot=a\n"
                  "journal_only=1\njournal_only=1\n", "journal_only repeated"));
    CHECK(refused("install_root=ms0:/PSP/GAME/TILEFINCH\nrunning_slot=a\n"
                  "journal_only=1\n", "main install"));

    /* Never the user's own install, in any case spelling. */
    CHECK(refused("install_root=ms0:/PSP/GAME/TILEFINCH\nrunning_slot=a\n",
                  "main install"));
    CHECK(refused("install_root=ms0:/PSP/GAME/Tilefinch\nrunning_slot=a\n",
                  "main install"));
    /* Only a plain directory directly under ms0:/PSP/GAME. */
    CHECK(refused("install_root=host0:/TFUPTEST\nrunning_slot=a\n",
                  "not under"));
    CHECK(refused("install_root=ms0:/PSP/GAMES/X\nrunning_slot=a\n",
                  "not under"));
    CHECK(refused("install_root=ms0:/PSP/GAME/\nrunning_slot=a\n",
                  "name length"));
    CHECK(refused("install_root=ms0:/PSP/GAME/../GAME/TILEFINCH\n"
                  "running_slot=a\n", "name character"));
    CHECK(refused("install_root=ms0:/PSP/GAME/TFUPTEST/slot-a\n"
                  "running_slot=a\n", "name character"));
    CHECK(refused("install_root=ms0:/PSP/GAME/TF UP\nrunning_slot=a\n",
                  "name character"));
    CHECK(refused("install_root=ms0:/PSP/GAME/"
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456\nrunning_slot=a\n",
                  "name length"));
    /* Every key exactly once, and nothing else. */
    CHECK(refused("install_root=ms0:/PSP/GAME/TFUPTEST\n", "no running_slot"));
    CHECK(refused("running_slot=a\n", "no install_root"));
    CHECK(refused("install_root=ms0:/PSP/GAME/TFUPTEST\nrunning_slot=c\n",
                  "not a or b"));
    CHECK(refused("install_root=ms0:/PSP/GAME/TFUPTEST\nrunning_slot=ab\n",
                  "not a or b"));
    CHECK(refused("install_root=ms0:/PSP/GAME/TFUPTEST\nrunning_slot=a\n"
                  "running_slot=b\n", "running_slot repeated"));
    CHECK(refused("install_root=ms0:/PSP/GAME/TFUPTEST\n"
                  "install_root=ms0:/PSP/GAME/TILEFINCH\nrunning_slot=a\n",
                  "install_root repeated"));
    CHECK(refused("install_root=ms0:/PSP/GAME/TFUPTEST\nrunning_slot=a\n"
                  "make_components=1\n", "unknown key"));
    CHECK(refused(" install_root=ms0:/PSP/GAME/TFUPTEST\nrunning_slot=a\n",
                  "unknown key"));

    /* An embedded NUL cannot hide a second value from the C string view. */
    static const char nul_text[] =
        "install_root=ms0:/PSP/GAME/TFUPTEST\0/x\nrunning_slot=a\n";
    char nul_why[64] = "";
    CHECK(!psp_update_route_parse(nul_text, sizeof(nul_text) - 1u, &route,
                                  nul_why, sizeof(nul_why))
          && strstr(nul_why, "NUL") != NULL);
    puts("psp update route tests passed");
    return 0;
}
