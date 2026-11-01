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

    puts("test: boot finds the add-on where the loader will look for it");
    {
        static const char *const programs[] = {
            "ms0:/PSP/GAME/TILEFINCH/slot-a/EBOOT.PBP",
            "ms0:/PSP/GAME/TILEFINCH/EBOOT.PBP",
            "host0:/psp-browser-script-dev.prx"
        };
        for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
            TilefinchInstallPaths program_paths;
            char expected[TILEFINCH_INSTALL_PATH_LIMIT];
            char early[TILEFINCH_INSTALL_PATH_LIMIT];
            CHECK(tilefinch_install_paths_derive(programs[i], &program_paths)
                  && tilefinch_swdec_component_path(
                      &program_paths, "tilefinch-swdec.prx",
                      expected, sizeof(expected))
                  && tilefinch_swdec_component_prx_path_for_program(
                      programs[i], early, sizeof(early))
                  && strcmp(expected, early) == 0);
        }
        char small[16];
        CHECK(!tilefinch_swdec_component_prx_path_for_program(
                  "ms0:/PSP/GAME/TILEFINCH/slot-a/EBOOT.PBP",
                  small, sizeof(small))
              && !tilefinch_swdec_component_prx_path_for_program(
                  NULL, small, sizeof(small)));
        CHECK(tilefinch_psp_heap_threshold_kb(false) == 2048
              && tilefinch_psp_heap_threshold_kb(true) == 4096);
    }

    puts("test: a component the firmware cannot start is not called stale");
    /* 0x80020190: the firmware's out-of-memory refusal (start thread). */
    CHECK(tilefinch_swdec_component_start_failure(
              (int) 0x80020190u, -1, false)
          == TILEFINCH_SWDEC_FAILURE_RUNTIME);
    /* module_start refused this browser's start block (-2: other ABI). */
    CHECK(tilefinch_swdec_component_start_failure(0, -2, false)
          == TILEFINCH_SWDEC_FAILURE_REBUILD);
    /* Started, but the table it returned is not this ABI's. */
    CHECK(tilefinch_swdec_component_start_failure(0, 0, false)
          == TILEFINCH_SWDEC_FAILURE_REBUILD);
    CHECK(tilefinch_swdec_component_start_failure(0, 0, true)
          == TILEFINCH_SWDEC_FAILURE_NONE);
    char message[96];
    tilefinch_swdec_component_failure_message(
        TILEFINCH_SWDEC_FAILURE_RUNTIME, (int) 0x80020190u,
        message, sizeof(message));
    CHECK(strcmp(message,
                 "optional video decoder is unavailable (0x80020190)") == 0
          && strstr(message, "REBUILD") == NULL);
    tilefinch_swdec_component_failure_message(
        TILEFINCH_SWDEC_FAILURE_REBUILD, 0, message, sizeof(message));
    CHECK(strcmp(message,
                 "OPTIONAL VIDEO DECODER NEEDS REBUILD - SEE README") == 0);
    tilefinch_swdec_component_failure_message(
        TILEFINCH_SWDEC_FAILURE_MISSING, 0, message, sizeof(message));
    CHECK(strcmp(message,
                 "OPTIONAL VIDEO DECODER NOT INSTALLED - SEE README") == 0);

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
