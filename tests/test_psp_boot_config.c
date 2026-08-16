#include "tilefinch/psp_boot_config.h"

/* The gate this file exercises is spelled in the decoder policy header,
   and so is the table the in-app picker may write from. Both have to
   agree. */
#include "../src/media_backend_psp_policy.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tilefinch/site_adapter.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    size_t calls;
    size_t line;
    char key[64];
} WarningCapture;

static void capture_warning(
    void *context, const char *path, size_t line, const char *key)
{
    (void) path;
    WarningCapture *capture = context;
    capture->calls++;
    capture->line = line;
    snprintf(capture->key, sizeof(capture->key), "%s", key);
}

int main(void)
{
    PspBootConfig config;
    psp_boot_config_defaults(&config);
    CHECK(config.limit_mb == 32);
    CHECK(config.heap_mb == 5);
    CHECK(config.css_width == 480);
    CHECK(config.network_profile == 1);
    CHECK(config.validation_media_stability_seconds == 120);
    CHECK(!psp_boot_config_automation_requires_engine_first(&config));
    snprintf(config.input_script, sizeof(config.input_script),
             "%s", "menu-tour.txt");
    config.interactive_validation_ticks = 720;
    config.validation_power_test_auto = 1;
    config.dump_frame = 2;
    CHECK(!psp_boot_config_automation_requires_engine_first(&config));
    config.validation_cancel_after_ms = 1;
    CHECK(psp_boot_config_automation_requires_engine_first(&config));
    config.validation_cancel_after_ms = 0;
    config.validation_media_play = 1;
    CHECK(psp_boot_config_automation_requires_engine_first(&config));
    config.validation_media_play = 0;
    config.validation_media_fixture_auto = 1;
    CHECK(psp_boot_config_automation_requires_engine_first(&config));
    config.validation_media_fixture_auto = 0;
    config.validation_raster_fixture_auto = 1;
    CHECK(psp_boot_config_automation_requires_engine_first(&config));
    config.validation_raster_fixture_auto = 0;
    config.input_script[0] = '\0';
    config.interactive_validation_ticks = 0;
    config.validation_power_test_auto = 0;
    config.dump_frame = 0;
    const char *invalid = NULL;
    CHECK(psp_boot_config_validate(&config, &invalid));

    /* Shipping builds must not inherit a device-test driver from a copied
       boot file. Production choices survive the scrub. */
    PspBootConfig shipping = config;
    snprintf(shipping.url, sizeof(shipping.url), "%s",
             "https://example.test/watch");
    snprintf(shipping.trace, sizeof(shipping.trace), "%s", "scenario.trace");
    snprintf(shipping.input_script, sizeof(shipping.input_script), "%s",
             "media-run.txt");
    snprintf(shipping.exit_to, sizeof(shipping.exit_to), "%s",
             "ms0:/PSP/GAME/PSPLINK/EBOOT.PBP");
    snprintf(shipping.developer_update_url,
             sizeof(shipping.developer_update_url), "%s",
             "https://updates.example.test/dev/update.tfum");
    shipping.ticks = 100;
    shipping.dump_frame = 1;
    shipping.exit_after_report = 1;
    shipping.interactive_validation_ticks = 100;
    shipping.validation_cancel_after_ms = 1;
    shipping.validation_preview_scroll = 1;
    shipping.validation_media_play = 1;
    shipping.validation_media_stability_auto = 1;
    shipping.validation_media_stability_seconds = 900;
    shipping.validation_media_lifecycle_auto = 1;
    shipping.validation_media_seek_permille = 333;
    shipping.validation_media_au_dump = 1;
    shipping.validation_media_fixture_auto = 1;
    shipping.validation_raster_fixture_auto = 1;
    shipping.validation_power_test_auto = 1;
    shipping.validation_ge_present_probe = 1;
    shipping.validation_csc_order_probe = 1;
    shipping.validation_media_range_probe = 1;
    shipping.validation_update_auto = 1;
    snprintf(shipping.validation_update_url,
             sizeof(shipping.validation_update_url), "%s",
             "https://127.0.0.1/update.tfum");
    shipping.validation_media_refusal_reset = 1;
    shipping.validation_media_reset_mode = PSP_MEDIA_RESET_MODE_NO_TOUCH;
    psp_boot_config_disable_automation(&shipping);
    CHECK(strcmp(shipping.url, TILEFINCH_HOMEPAGE_URL) == 0);
    CHECK(strcmp(shipping.trace, "none") == 0);
    CHECK(shipping.input_script[0] == '\0' && shipping.exit_to[0] == '\0');
    CHECK(shipping.ticks == 0 && shipping.dump_frame == 0);
    CHECK(shipping.exit_after_report == 0);
    CHECK(shipping.interactive_validation_ticks == 0);
    CHECK(shipping.validation_cancel_after_ms == 0);
    CHECK(shipping.validation_preview_scroll == 0);
    CHECK(shipping.validation_media_play == 0);
    CHECK(shipping.validation_media_stability_auto == 0);
    CHECK(shipping.validation_media_stability_seconds == 120);
    CHECK(shipping.validation_media_lifecycle_auto == 0);
    CHECK(shipping.validation_media_seek_permille == 667);
    CHECK(shipping.validation_media_au_dump == 0);
    CHECK(shipping.validation_media_fixture_auto == 0);
    CHECK(shipping.validation_raster_fixture_auto == 0);
    CHECK(shipping.validation_power_test_auto == 0);
    CHECK(shipping.validation_ge_present_probe == 0);
    CHECK(shipping.validation_csc_order_probe == 0);
    CHECK(shipping.validation_media_range_probe == 0);
    CHECK(shipping.validation_update_auto == 0);
    CHECK(shipping.validation_update_url[0] == '\0');
    CHECK(shipping.validation_media_refusal_reset == 1);
    CHECK(shipping.validation_media_reset_mode
          == PSP_MEDIA_RESET_MODE_NO_TOUCH);
    CHECK(strcmp(shipping.developer_update_url,
                 "https://updates.example.test/dev/update.tfum") == 0);

    PspBootConfig custom_home = config;
    snprintf(custom_home.url, sizeof(custom_home.url), "%s",
             "https://example.test/start");
    psp_boot_config_disable_automation(&custom_home);
    CHECK(strcmp(custom_home.url, "https://example.test/start") == 0);

    char path[128];
    snprintf(path, sizeof(path), "/tmp/tilefinch-boot-config-%ld.cfg",
             (long) getpid());
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fputs(
        "limit=40\nnetwork_profile=2\n"
        "validation_media_fixture_auto=1\n"
        "validation_media_stability_seconds=900\n"
        "validation_raster_fixture_auto=1\n"
        "validation_ge_present_probe=1\n"
        "validation_csc_order_probe=1\n"
        "validation_media_range_probe=1\n"
        "validation_update_auto=1\n"
        "validation_update_url=https://127.0.0.1:8443/"
            "tilefinch-update-v1.tfum\n"
        "validation_media_lifecycle_auto=1\n"
        "validation_media_refusal_reset=1\n"
        "validation_media_reset_mode=2\n"
        "developer_update_url=https://192.0.2.1/beta/latest.tfum\n"
        "developer_package_url=https://1drv.ms/u/s!package-token\n"
        "mystery=1\n", file) >= 0);
    CHECK(fclose(file) == 0);

    WarningCapture warning = {0};
    CHECK(psp_boot_config_load(
        &config, path, capture_warning, &warning));
    CHECK(config.limit_mb == 40);
    CHECK(config.network_profile == 2);
    CHECK(config.validation_media_fixture_auto == 1);
    CHECK(config.validation_media_stability_seconds == 900);
    CHECK(config.validation_raster_fixture_auto == 1);
    /* The present probe draws its own pictures and needs no document, so
       unlike the other probes it must not force an engine-first boot. */
    CHECK(config.validation_ge_present_probe == 1);
    /* Same for the colour-order probe: it supplies its own picture from the
       embedded fixture and never navigates. */
    CHECK(config.validation_csc_order_probe == 1);
    /* The range probe resolves its own stream from `url` and never commits a
       document either, so it too leaves the shipping boot order alone. */
    CHECK(config.validation_media_range_probe == 1);
    CHECK(config.validation_update_auto == 1);
    CHECK(strcmp(config.validation_update_url,
                 "https://127.0.0.1:8443/tilefinch-update-v1.tfum") == 0);
    CHECK(config.validation_media_lifecycle_auto == 1);
    /* How a refused access unit is recovered. It changes only what happens
       after firmware has already refused something, so it commits no document
       and must not force an engine-first boot either. */
    CHECK(config.validation_media_refusal_reset == 1);
    /* And what a reposition does to the decoder object. Like the recovery
       itself it only changes what happens after a seek or a refusal has
       already occurred, so it commits no document either. */
    CHECK(config.validation_media_reset_mode
          == PSP_MEDIA_RESET_MODE_NO_TOUCH);
    CHECK(!psp_boot_config_automation_requires_engine_first(&config)
          || config.validation_media_fixture_auto != 0);
    CHECK(warning.calls == 1);
    CHECK(warning.line == 16);
    CHECK(strcmp(warning.key, "mystery") == 0);
    CHECK(strcmp(config.developer_update_url,
                 "https://192.0.2.1/beta/latest.tfum") == 0);
    CHECK(strcmp(config.developer_package_url,
                 "https://1drv.ms/u/s!package-token") == 0);
    CHECK(psp_boot_config_validate(&config, &invalid));

    config.validation_update_url[0] = '\0';
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "validation_update_url") == 0);
    config.validation_update_auto = 0;
    CHECK(psp_boot_config_validate(&config, &invalid));
    snprintf(config.validation_update_url,
             sizeof(config.validation_update_url),
             "http://127.0.0.1:8443/tilefinch-update-v1.tfum");
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "validation_update_url") == 0);
    snprintf(config.validation_update_url,
             sizeof(config.validation_update_url),
             "https://127.0.0.1:8443/tilefinch-update-v1.tfum");
    config.validation_update_auto = 1;
    CHECK(psp_boot_config_validate(&config, &invalid));

    config.limit_mb = 53;
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "limit_mb") == 0);
    config.limit_mb = 40;

    /* A typo in the refusal-recovery knob must halt at the config gate rather
       than read as "off": the whole point of setting it is that the run knows
       which recovery it is measuring. */
    config.validation_media_refusal_reset = 2;
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "validation_media_refusal_reset") == 0);
    config.validation_media_refusal_reset = 1;
    CHECK(psp_boot_config_validate(&config, &invalid));

    config.validation_media_lifecycle_auto = 2;
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "validation_media_lifecycle_auto") == 0);
    config.validation_media_lifecycle_auto = 1;
    CHECK(psp_boot_config_validate(&config, &invalid));

    config.validation_media_stability_seconds = 9;
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "validation_media_stability_seconds") == 0);
    config.validation_media_stability_seconds = 901;
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "validation_media_stability_seconds") == 0);
    config.validation_media_stability_seconds = 120;
    CHECK(psp_boot_config_validate(&config, &invalid));

    /* Zero means an uninterrupted soak; 1..999 retain the two-seek harness. */
    config.validation_media_seek_permille = 0;
    CHECK(psp_boot_config_validate(&config, &invalid));
    config.validation_media_seek_permille = 1000;
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "validation_media_seek_permille") == 0);
    config.validation_media_seek_permille = 50;
    CHECK(psp_boot_config_validate(&config, &invalid));

    /* Three shapes, and nothing outside them: a run has to know which
       reposition it is measuring. */
    config.validation_media_reset_mode = 3;
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "validation_media_reset_mode") == 0);
    config.validation_media_reset_mode = -1;
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "validation_media_reset_mode") == 0);
    config.validation_media_reset_mode = PSP_MEDIA_RESET_MODE_RECREATE;
    CHECK(psp_boot_config_validate(&config, &invalid));

    snprintf(config.developer_update_url,
             sizeof(config.developer_update_url),
             "https://user@example.test/update.tfum");
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "developer_update_url") == 0);
    /* The developer channel is the unsigned one, so its endpoints must be
       https: over cleartext anything on the path can substitute the
       package and nothing downstream would verify it. */
    snprintf(config.developer_update_url,
             sizeof(config.developer_update_url),
             "http://updates.example.test/dev/update.tfum");
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "developer_update_url") == 0);
    snprintf(config.developer_update_url,
             sizeof(config.developer_update_url),
             "https://updates.example.test/dev/update.tfum");
    snprintf(config.developer_package_url,
             sizeof(config.developer_package_url),
             "https://user@example.test/package.tfup");
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "developer_package_url") == 0);
    snprintf(config.developer_package_url,
             sizeof(config.developer_package_url),
             "http://updates.example.test/dev/package.tfup");
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "developer_package_url") == 0);
    snprintf(config.developer_package_url,
             sizeof(config.developer_package_url),
             "https://1drv.ms/u/s!package-token");

    /*
     * A boot configuration whose only fault is one of these two keys must
     * still boot. They are hand-edited on a Memory Stick and optional, and
     * a typo costs the user nothing but the Developer channel -- the same
     * thing an absent key costs -- so the drop pass clears them and leaves
     * the configuration valid. Every other key keeps halting.
     */
    PspBootConfig recoverable = config;
    snprintf(recoverable.developer_update_url,
             sizeof(recoverable.developer_update_url),
             "http://updates.example.test/dev/update.tfum");
    CHECK(!psp_boot_config_validate(&recoverable, &invalid));
    CHECK(psp_boot_config_drop_invalid_developer_urls(&recoverable)
          == PSP_BOOT_CONFIG_DROPPED_DEVELOPER_UPDATE_URL);
    CHECK(recoverable.developer_update_url[0] == '\0');
    /* A good package URL beside a bad metadata URL is left alone; it is
       simply unreachable while the channel is hidden. */
    CHECK(strcmp(recoverable.developer_package_url,
                 "https://1drv.ms/u/s!package-token") == 0);
    CHECK(psp_boot_config_validate(&recoverable, &invalid));

    /* Both keys at once, and the reverse pairing: a usable metadata URL
       with an unusable package URL keeps the channel and falls back to
       same-directory package lookup. */
    snprintf(recoverable.developer_update_url,
             sizeof(recoverable.developer_update_url),
             "https://updates.example.test/dev/update.tfum");
    snprintf(recoverable.developer_package_url,
             sizeof(recoverable.developer_package_url),
             "https://user@example.test/package.tfup");
    CHECK(psp_boot_config_drop_invalid_developer_urls(&recoverable)
          == PSP_BOOT_CONFIG_DROPPED_DEVELOPER_PACKAGE_URL);
    CHECK(recoverable.developer_package_url[0] == '\0');
    CHECK(strcmp(recoverable.developer_update_url,
                 "https://updates.example.test/dev/update.tfum") == 0);
    CHECK(psp_boot_config_validate(&recoverable, &invalid));

    /* Valid values survive the pass untouched, and nothing is reported. */
    CHECK(psp_boot_config_drop_invalid_developer_urls(&recoverable) == 0u);
    CHECK(strcmp(recoverable.developer_update_url,
                 "https://updates.example.test/dev/update.tfum") == 0);

    /* The pre-existing halting keys are deliberately not covered by it. */
    recoverable.limit_mb = 53;
    CHECK(psp_boot_config_drop_invalid_developer_urls(&recoverable) == 0u);
    CHECK(!psp_boot_config_validate(&recoverable, &invalid));
    CHECK(strcmp(invalid, "limit_mb") == 0);

    /* The experimental decoder knob defaults to absent, accepts exactly the
       documented spellings, and halts the boot on anything else. */
    CHECK(config.experimental_wide_video[0] == '\0');
    CHECK(psp_boot_config_validate(&config, &invalid));
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "wide-annexb");
    CHECK(psp_boot_config_validate(&config, &invalid));
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "boot4");
    CHECK(psp_boot_config_validate(&config, &invalid));
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "edram-real");
    CHECK(psp_boot_config_validate(&config, &invalid));
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "no-boot");
    CHECK(psp_boot_config_validate(&config, &invalid));
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "off");
    CHECK(psp_boot_config_validate(&config, &invalid));
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "annexb");
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "experimental_wide_video") == 0);
    /* A typo near the new spelling still halts rather than reading as off. */
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "boot3");
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "experimental_wide_video") == 0);
    /* Including the two the in-app picker can now write: a hand edit that
       misses either spelling has to halt exactly as before, which is why the
       picker is restricted to psp_media_wide_program_choice's table. */
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "edram");
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "experimental_wide_video") == 0);
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "noboot");
    CHECK(!psp_boot_config_validate(&config, &invalid));
    CHECK(strcmp(invalid, "experimental_wide_video") == 0);
    /* Every spelling the picker offers survives the gate and round-trips
       through the override writer below. */
    for (unsigned choice = 0;
         choice < PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT; choice++) {
        snprintf(config.experimental_wide_video,
                 sizeof(config.experimental_wide_video), "%s",
                 psp_media_wide_program_choice(choice));
        CHECK(psp_boot_config_validate(&config, &invalid));
    }
    snprintf(config.experimental_wide_video,
             sizeof(config.experimental_wide_video), "wide");

    CHECK(psp_boot_config_write_overrides(&config, path));
    PspBootConfig saved;
    psp_boot_config_defaults(&saved);
    CHECK(psp_boot_config_load(&saved, path, NULL, NULL));
    CHECK(saved.network_profile == 2);
    CHECK(strcmp(saved.experimental_wide_video, "wide") == 0);
    CHECK(strcmp(saved.url, config.url) == 0);
    CHECK(strcmp(saved.developer_update_url,
                 config.developer_update_url) == 0);
    CHECK(strcmp(saved.developer_package_url,
                 config.developer_package_url) == 0);

    /* The overrides file must be replaceable without a remove/rename loss
       window on FAT. A rewrite rotates the previous generation to .bak, and
       load falls back to it if power is lost before the new primary lands. */
    char temporary[192], backup[192];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    snprintf(backup, sizeof(backup), "%s.bak", path);
    config.network_profile = 1;
    snprintf(config.url, sizeof(config.url), "https://replaced.test/");
    CHECK(psp_boot_config_write_overrides(&config, path));
    psp_boot_config_defaults(&saved);
    CHECK(psp_boot_config_load(&saved, path, NULL, NULL));
    CHECK(saved.network_profile == 1);
    CHECK(strcmp(saved.url, "https://replaced.test/") == 0);
    CHECK(remove(path) == 0);
    psp_boot_config_defaults(&saved);
    CHECK(psp_boot_config_load(&saved, path, NULL, NULL));
    CHECK(saved.network_profile == 2);
    FILE *stranded = fopen(temporary, "rb");
    CHECK(stranded == NULL);
    CHECK(remove(backup) == 0);

    /* exit_to names the EBOOT this one hands the console back to, so the
       remote loop survives the browser exiting. It is a whole Memory Stick
       path rather than a leaf name, and it is read from a boot file like any
       other key. */
    char exit_path[192];
    snprintf(exit_path, sizeof(exit_path),
             "/tmp/tilefinch-boot-exit-%ld.cfg", (long) getpid());
    file = fopen(exit_path, "wb");
    CHECK(file != NULL);
    CHECK(fputs("exit_to=ms0:/PSP/GAME/PSPLINK/EBOOT.PBP\n", file) >= 0);
    CHECK(fclose(file) == 0);
    PspBootConfig handoff;
    psp_boot_config_defaults(&handoff);
    CHECK(handoff.exit_to[0] == '\0');
    CHECK(!psp_boot_config_drop_unusable_exit_to(&handoff));
    CHECK(psp_boot_config_load(&handoff, exit_path, NULL, NULL));
    CHECK(strcmp(handoff.exit_to, "ms0:/PSP/GAME/PSPLINK/EBOOT.PBP") == 0);
    CHECK(!psp_boot_config_drop_unusable_exit_to(&handoff));
    CHECK(psp_boot_config_validate(&handoff, &invalid));
    /* An in-app settings change rewrites the overrides file. A hand-added
       handoff has to survive that or the remote loop breaks the first time
       a developer touches a setting. */
    CHECK(psp_boot_config_write_overrides(&handoff, exit_path));
    psp_boot_config_defaults(&saved);
    CHECK(psp_boot_config_load(&saved, exit_path, NULL, NULL));
    CHECK(strcmp(saved.exit_to, "ms0:/PSP/GAME/PSPLINK/EBOOT.PBP") == 0);
    snprintf(backup, sizeof(backup), "%s.bak", exit_path);
    CHECK(remove(backup) == 0);

    /*
     * A typo here costs a developer the handoff and nothing else, so an
     * unusable value is cleared and the boot continues. Halting on it would
     * cost a trip to the console for a key that only exists to save one.
     */
    static const char *const unusable[] = {
        "PSPLINK/EBOOT.PBP",
        "ms0:/PSP/GAME/../../EBOOT.PBP",
        "ms0:/PSP/GAME/PSPLINK/",
        "ms0:/PSP/GAME/PSPLINK/EBOOT.ELF",
        "http://example.test/EBOOT.PBP",
        "/EBOOT.PBP"
    };
    for (size_t i = 0; i < sizeof(unusable) / sizeof(unusable[0]); i++) {
        snprintf(handoff.exit_to, sizeof(handoff.exit_to), "%s",
                 unusable[i]);
        CHECK(psp_boot_config_drop_unusable_exit_to(&handoff));
        CHECK(handoff.exit_to[0] == '\0');
        CHECK(psp_boot_config_validate(&handoff, &invalid));
    }
    CHECK(remove(exit_path) == 0);

    puts("psp-boot-config-tests: all checks passed");
    return 0;
}
