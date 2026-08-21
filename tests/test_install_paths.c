#include "tilefinch/install_paths.h"
#include "tilefinch/voice_component.h"
#include "tilefinch/swdec_component_store.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    puts("test: slotted program resources and shared mutable data");
    TilefinchInstallPaths paths;
    CHECK(tilefinch_install_paths_derive(
              "ms0:/PSP/GAME/TILEFINCH/slot-b/EBOOT.PBP", &paths)
          && paths.slotted
          && strcmp(paths.slot_name, "slot-b") == 0
          && strcmp(paths.program_dir,
                    "ms0:/PSP/GAME/TILEFINCH/slot-b") == 0
          && strcmp(paths.install_root,
                    "ms0:/PSP/GAME/TILEFINCH") == 0
          && strcmp(paths.data_dir,
                    "ms0:/PSP/GAME/TILEFINCH/data") == 0);
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    CHECK(tilefinch_install_program_path(
              &paths, "fonts/ui.ttf", path, sizeof(path))
          && strcmp(path,
                    "ms0:/PSP/GAME/TILEFINCH/slot-b/fonts/ui.ttf") == 0
          && tilefinch_install_data_path(
              &paths, "profile.cfg", path, sizeof(path))
          && strcmp(path,
                    "ms0:/PSP/GAME/TILEFINCH/data/profile.cfg") == 0
          && tilefinch_install_advisory_matches(
              &paths, "slot-b", "ms0:/PSP/GAME/TILEFINCH/data")
          && !tilefinch_install_advisory_matches(
              &paths, "slot-a", "ms0:/PSP/GAME/TILEFINCH/data"));
    CHECK(tilefinch_voice_component_path(&paths, path, sizeof(path))
          && strcmp(path,
                    "ms0:/PSP/GAME/TILEFINCH/components/voice-en-us/"
                    "active/model") == 0);
    CHECK(tilefinch_swdec_component_path(
              &paths, "tilefinch-swdec.prx", path, sizeof(path))
          && strcmp(path,
                    "ms0:/PSP/GAME/TILEFINCH/components/swdec/"
                    "tilefinch-swdec.prx") == 0);

    puts("test: optional-decoder compatibility record is bounded");
    uint16_t abi = 0;
    static const char compatible[] =
        "tilefinch-swdec-component-v1\nabi=4\n";
    CHECK(tilefinch_swdec_component_info_parse(
              compatible, sizeof(compatible) - 1u, &abi)
              == TILEFINCH_SWDEC_COMPONENT_INFO_VALID
          && abi == 4u);
    static const char zero_abi[] =
        "tilefinch-swdec-component-v1\nabi=0\n";
    static const char trailing_info[] =
        "tilefinch-swdec-component-v1\nabi=4\ntrailing";
    CHECK(tilefinch_swdec_component_info_parse(
              zero_abi, strlen(zero_abi), &abi)
              == TILEFINCH_SWDEC_COMPONENT_INFO_INVALID);
    CHECK(tilefinch_swdec_component_info_parse(
              trailing_info, strlen(trailing_info), &abi)
              == TILEFINCH_SWDEC_COMPONENT_INFO_INVALID);

    puts("test: legacy single-EBOOT layout remains compatible");
    CHECK(tilefinch_install_paths_derive(
              "ms0:/PSP/GAME/TILEFINCH/EBOOT.PBP", &paths)
          && !paths.slotted
          && strcmp(paths.program_dir,
                    "ms0:/PSP/GAME/TILEFINCH") == 0
          && strcmp(paths.data_dir,
                    "ms0:/PSP/GAME/TILEFINCH") == 0);

    puts("test: root launcher shares the slotted data directory");
    CHECK(tilefinch_install_paths_derive_launcher(
              "ms0:/PSP/GAME/TILEFINCH/EBOOT.PBP", &paths)
          && !paths.slotted
          && strcmp(paths.install_root,
                    "ms0:/PSP/GAME/TILEFINCH") == 0
          && strcmp(paths.data_dir,
                    "ms0:/PSP/GAME/TILEFINCH/data") == 0
          && !tilefinch_install_paths_derive_launcher(
              "ms0:/PSP/GAME/TILEFINCH/slot-a/EBOOT.PBP", &paths));
    puts("install-path-tests: all checks passed");
    return 0;
}
