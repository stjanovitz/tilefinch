#include "tilefinch/install_paths.h"
#include "tilefinch/voice_component.h"

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
