#include "tilefinch/psp_boot_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_backend_psp_policy.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/site_adapter.h"
#include "tilefinch/update.h"

#define PSP_DEFAULT_SCRIPT_HEAP_MB 5L
#define PSP_DEFAULT_SCRIPT_TOTAL_MB 2L
#define PSP_DEFAULT_SCRIPT_FILE_KB 256L
#define PSP_DEFAULT_CSS_WIDTH 480L
#define PSP_DEFAULT_CSS_HEIGHT 272L

_Static_assert(
    PSP_DEFAULT_SCRIPT_FILE_KB
        <= PSP_DEFAULT_SCRIPT_TOTAL_MB * 1024L,
    "the PSP default per-script limit must fit the total script admission");

void psp_boot_config_defaults(PspBootConfig *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    snprintf(config->url, sizeof(config->url), "%s",
             TILEFINCH_HOMEPAGE_URL);
    snprintf(config->trace, sizeof(config->trace), "none");
    snprintf(config->profile, sizeof(config->profile), "realistic");
    config->tick_ms = 16;
    /*
     * Physical validation measured 43 MiB as allocatable through newlib
     * after the executable, VRAM, stacks, kernel modules, and network
     * reserve. This 32 MiB shared envelope leaves frontend/allocator
     * headroom while allowing a large page and the voice recognizer to
     * coexist.
     */
    /* Two thirds, which is where the fraction was hardcoded. */
    config->validation_media_seek_permille = 667;
    config->validation_media_stability_seconds = 120;
    config->limit_mb = 32;
    config->heap_mb = PSP_DEFAULT_SCRIPT_HEAP_MB;
    config->total_mb = PSP_DEFAULT_SCRIPT_TOTAL_MB;
    config->file_kb = PSP_DEFAULT_SCRIPT_FILE_KB;
    config->count = 256;
    config->gc_growth_pct = 150;
    config->script_timeout_ms = 60000;
    config->css_width = PSP_DEFAULT_CSS_WIDTH;
    config->css_height = PSP_DEFAULT_CSS_HEIGHT;
    config->network_profile = 1;
}

void psp_boot_config_disable_automation(PspBootConfig *config)
{
    if (config == NULL) return;
    bool was_automated = strcmp(config->trace, "none") != 0
        || config->ticks != 0
        || config->dump_frame != 0
        || config->exit_after_report != 0
        || config->interactive_validation_ticks != 0
        || config->validation_cancel_after_ms != 0
        || config->validation_preview_scroll != 0
        || config->validation_media_play != 0
        || config->validation_media_stability_auto != 0
        || config->validation_media_lifecycle_auto != 0
        || config->validation_media_au_dump != 0
        || config->validation_media_fixture_auto != 0
        || config->validation_raster_fixture_auto != 0
        || config->validation_power_test_auto != 0
        || config->validation_ge_present_probe != 0
        || config->validation_csc_order_probe != 0
        || config->validation_media_range_probe != 0
        || config->validation_update_auto != 0
        || config->validation_update_url[0] != '\0'
        || config->input_script[0] != '\0'
        || config->exit_to[0] != '\0';
    if (was_automated)
        snprintf(config->url, sizeof(config->url), "%s",
                 TILEFINCH_HOMEPAGE_URL);
    snprintf(config->trace, sizeof(config->trace), "none");
    config->ticks = 0;
    config->dump_frame = 0;
    config->exit_after_report = 0;
    config->interactive_validation_ticks = 0;
    config->validation_cancel_after_ms = 0;
    config->validation_preview_scroll = 0;
    config->validation_media_play = 0;
    config->validation_media_stability_auto = 0;
    config->validation_media_stability_seconds = 120;
    config->validation_media_lifecycle_auto = 0;
    config->validation_media_seek_permille = 667;
    config->validation_media_au_dump = 0;
    config->validation_media_fixture_auto = 0;
    config->validation_raster_fixture_auto = 0;
    config->validation_power_test_auto = 0;
    config->validation_ge_present_probe = 0;
    config->validation_csc_order_probe = 0;
    config->validation_media_range_probe = 0;
    config->validation_update_auto = 0;
    config->validation_update_url[0] = '\0';
    config->input_script[0] = '\0';
    config->exit_to[0] = '\0';
}

static bool psp_boot_config_load_one(
    PspBootConfig *config, const char *path,
    PspBootConfigWarning warning, void *warning_context)
{
    if (config == NULL || path == NULL) return false;
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    PspBootConfig loaded = *config;
    char line[1600];
    size_t line_number = 0;
    bool complete = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        size_t raw_length = strlen(line);
        if (raw_length == 0 || (line[raw_length - 1u] != '\n'
                                && line[raw_length - 1u] != '\r')) {
            complete = false;
            break;
        }
        char *equals = strchr(line, '=');
        if (equals == NULL || line[0] == '#') continue;
        *equals = '\0';
        char *value = equals + 1;
        value[strcspn(value, "\r\n")] = '\0';
        if (strcmp(line, "url") == 0) {
            snprintf(loaded.url, sizeof(loaded.url), "%s", value);
        } else if (strcmp(line, "trace") == 0) {
            snprintf(loaded.trace, sizeof(loaded.trace), "%s", value);
        } else if (strcmp(line, "input_script") == 0) {
            snprintf(loaded.input_script, sizeof(loaded.input_script),
                     "%s", value);
        } else if (strcmp(line, "exit_to") == 0) {
            snprintf(loaded.exit_to, sizeof(loaded.exit_to), "%s", value);
        } else if (strcmp(line, "profile") == 0) {
            snprintf(loaded.profile, sizeof(loaded.profile), "%s", value);
        } else if (strcmp(line, "ticks") == 0) {
            loaded.ticks = atol(value);
        } else if (strcmp(line, "tick_ms") == 0) {
            loaded.tick_ms = atol(value);
        } else if (strcmp(line, "limit_mb") == 0
                   || strcmp(line, "limit") == 0) {
            loaded.limit_mb = atol(value);
        } else if (strcmp(line, "heap_mb") == 0) {
            loaded.heap_mb = atol(value);
        } else if (strcmp(line, "total_mb") == 0) {
            loaded.total_mb = atol(value);
        } else if (strcmp(line, "file_kb") == 0) {
            loaded.file_kb = atol(value);
        } else if (strcmp(line, "count") == 0) {
            loaded.count = atol(value);
        } else if (strcmp(line, "window_kb") == 0) {
            loaded.window_kb = atol(value);
        } else if (strcmp(line, "gc_growth_pct") == 0) {
            loaded.gc_growth_pct = atol(value);
        } else if (strcmp(line, "script_timeout_ms") == 0) {
            loaded.script_timeout_ms = atol(value);
        } else if (strcmp(line, "css_width") == 0) {
            loaded.css_width = atol(value);
        } else if (strcmp(line, "css_height") == 0) {
            loaded.css_height = atol(value);
        } else if (strcmp(line, "network_profile") == 0) {
            loaded.network_profile = atol(value);
        } else if (strcmp(line, "developer_update_url") == 0) {
            snprintf(loaded.developer_update_url,
                     sizeof(loaded.developer_update_url), "%s", value);
        } else if (strcmp(line, "developer_package_url") == 0) {
            snprintf(loaded.developer_package_url,
                     sizeof(loaded.developer_package_url), "%s", value);
        } else if (strcmp(line, "experimental_wide_video") == 0) {
            snprintf(loaded.experimental_wide_video,
                     sizeof(loaded.experimental_wide_video), "%s", value);
        } else if (strcmp(line, "dump_frame") == 0) {
            loaded.dump_frame = atol(value);
        } else if (strcmp(line, "exit_after_report") == 0) {
            loaded.exit_after_report = atol(value);
        } else if (strcmp(line, "interactive_validation_ticks") == 0) {
            loaded.interactive_validation_ticks = atol(value);
        } else if (strcmp(line, "validation_cancel_after_ms") == 0) {
            loaded.validation_cancel_after_ms = atol(value);
        } else if (strcmp(line, "validation_preview_scroll") == 0) {
            loaded.validation_preview_scroll = atol(value);
        } else if (strcmp(line, "validation_media_play") == 0) {
            loaded.validation_media_play = atol(value);
        } else if (strcmp(line, "validation_media_stability_auto") == 0) {
            loaded.validation_media_stability_auto = atol(value);
        } else if (strcmp(line, "validation_media_stability_seconds") == 0) {
            loaded.validation_media_stability_seconds = atol(value);
        } else if (strcmp(line, "validation_media_lifecycle_auto") == 0) {
            loaded.validation_media_lifecycle_auto = atol(value);
        } else if (strcmp(line, "validation_media_seek_permille") == 0) {
            loaded.validation_media_seek_permille = atol(value);
        } else if (strcmp(line, "validation_media_au_dump") == 0) {
            loaded.validation_media_au_dump = atol(value);
        } else if (strcmp(line, "validation_media_refusal_reset") == 0) {
            loaded.validation_media_refusal_reset = atol(value);
        } else if (strcmp(line, "validation_media_reset_mode") == 0) {
            loaded.validation_media_reset_mode = atol(value);
        } else if (strcmp(line, "validation_media_fixture_auto") == 0) {
            loaded.validation_media_fixture_auto = atol(value);
        } else if (strcmp(line, "validation_raster_fixture_auto") == 0) {
            loaded.validation_raster_fixture_auto = atol(value);
        } else if (strcmp(line, "validation_ge_present_probe") == 0) {
            loaded.validation_ge_present_probe = atol(value);
        } else if (strcmp(line, "validation_csc_order_probe") == 0) {
            loaded.validation_csc_order_probe = atol(value);
        } else if (strcmp(line, "validation_media_range_probe") == 0) {
            loaded.validation_media_range_probe = atol(value);
        } else if (strcmp(line, "validation_update_auto") == 0) {
            loaded.validation_update_auto = atol(value);
        } else if (strcmp(line, "validation_update_url") == 0) {
            snprintf(loaded.validation_update_url,
                     sizeof(loaded.validation_update_url), "%s", value);
        } else if (strcmp(line, "validation_power_test_auto") == 0) {
            loaded.validation_power_test_auto = atol(value);
        } else if (warning != NULL) {
            warning(warning_context, path, line_number, line);
        }
    }
    bool read_ok = !ferror(file);
    bool ok = fclose(file) == 0 && complete && read_ok;
    if (ok) *config = loaded;
    return ok;
}

bool psp_boot_config_load(
    PspBootConfig *config, const char *path,
    PspBootConfigWarning warning, void *warning_context)
{
    if (config == NULL || path == NULL) return false;
    if (psp_boot_config_load_one(
            config, path, warning, warning_context)) return true;
    char backup[TILEFINCH_INSTALL_PATH_LIMIT + 8u];
    int written = snprintf(backup, sizeof(backup), "%s.bak", path);
    return written > 0 && (size_t) written < sizeof(backup)
        && psp_boot_config_load_one(
               config, backup, warning, warning_context);
}

bool psp_boot_config_write_overrides(
    const PspBootConfig *config, const char *path)
{
    if (config == NULL || path == NULL) return false;
    char temporary[TILEFINCH_INSTALL_PATH_LIMIT + 8u];
    char backup[TILEFINCH_INSTALL_PATH_LIMIT + 8u];
    int written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (written <= 0 || (size_t) written >= sizeof(temporary)) return false;
    written = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (written <= 0 || (size_t) written >= sizeof(backup)) return false;
    FILE *file = fopen(temporary, "wb");
    bool ok = file != NULL;
    if (ok)
        ok = fprintf(file,
                     "url=%s\nnetwork_profile=%ld\n"
                     "developer_update_url=%s\n"
                     "developer_package_url=%s\n"
                     "experimental_wide_video=%s\n"
                     /* Hand-edited like the developer endpoints above and
                        never set from the UI, so it is written back for the
                        same reason they are: an in-app settings change
                        rewrites this file, and a key it did not carry
                        forward would silently disappear. */
                     "exit_to=%s\n",
                     config->url, config->network_profile,
                     config->developer_update_url,
                     config->developer_package_url,
                     config->experimental_wide_video,
                     config->exit_to) > 0
            && fflush(file) == 0;
    if (file != NULL && fclose(file) != 0) ok = false;
    if (!ok) {
        remove(temporary);
        return false;
    }
    bool had_previous = rename(path, backup) == 0;
    if (!had_previous) {
        if (errno == ENOENT) return rename(temporary, path) == 0;
        (void) remove(backup);
        had_previous = rename(path, backup) == 0;
        if (!had_previous && errno != ENOENT) {
            remove(temporary);
            return false;
        }
    }
    if (rename(temporary, path) == 0) return true;
    if (had_previous) (void) rename(backup, path);
    remove(temporary);
    return false;
}

unsigned psp_boot_config_drop_invalid_developer_urls(PspBootConfig *config)
{
    if (config == NULL) return 0u;
    unsigned dropped = 0u;
    if (config->developer_update_url[0] != '\0'
        && !tilefinch_update_url_is_valid(
               config->developer_update_url,
               sizeof(config->developer_update_url))) {
        config->developer_update_url[0] = '\0';
        dropped |= PSP_BOOT_CONFIG_DROPPED_DEVELOPER_UPDATE_URL;
    }
    if (config->developer_package_url[0] != '\0'
        && !tilefinch_update_url_is_valid(
               config->developer_package_url,
               sizeof(config->developer_package_url))) {
        config->developer_package_url[0] = '\0';
        dropped |= PSP_BOOT_CONFIG_DROPPED_DEVELOPER_PACKAGE_URL;
    }
    return dropped;
}

static bool exit_to_names_an_eboot(const char *path)
{
    /* A whole Memory Stick path, so the leaf-name rule input_script uses
       does not apply. What is required is only enough to be a handoff
       target the firmware could load: one of the devices a PSP can boot an
       EBOOT from, no parent-directory escape, and the EBOOT name itself.
       Anything past that is the firmware's judgement, not this parser's. */
    static const char *const devices[] = {"ms0:/", "ef0:/", "host0:/"};
    bool device_known = false;
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        if (strncmp(path, devices[i], strlen(devices[i])) == 0) {
            device_known = true;
            break;
        }
    }
    if (!device_known || strstr(path, "..") != NULL) return false;
    size_t length = strlen(path);
    static const char suffix[] = "/EBOOT.PBP";
    size_t suffix_length = sizeof(suffix) - 1u;
    return length > suffix_length
        && strcmp(path + length - suffix_length, suffix) == 0;
}

bool psp_boot_config_drop_unusable_exit_to(PspBootConfig *config)
{
    if (config == NULL || config->exit_to[0] == '\0') return false;
    if (exit_to_names_an_eboot(config->exit_to)) return false;
    config->exit_to[0] = '\0';
    return true;
}

bool psp_boot_config_validate(
    const PspBootConfig *config, const char **invalid_field)
{
    if (invalid_field != NULL) *invalid_field = "unknown";
#define REQUIRE_CONFIG(condition, field) \
    do { \
        if (!(condition)) { \
            if (invalid_field != NULL) *invalid_field = field; \
            return false; \
        } \
    } while (0)
    REQUIRE_CONFIG(config != NULL, "config");
    REQUIRE_CONFIG(
        strncmp(config->url, "https://", 8) == 0
            || strncmp(config->url, "http://", 7) == 0,
        "url");
    REQUIRE_CONFIG(config->ticks >= 0 && config->ticks <= 600000, "ticks");
    REQUIRE_CONFIG(
        config->tick_ms >= 1 && config->tick_ms <= 1000, "tick_ms");
    REQUIRE_CONFIG(
        config->limit_mb >= 12 && config->limit_mb <= 52, "limit_mb");
    REQUIRE_CONFIG(
        config->heap_mb >= 2 && config->heap_mb <= 32, "heap_mb");
    REQUIRE_CONFIG(
        config->total_mb >= 1 && config->total_mb <= 16
            && config->total_mb <= config->heap_mb,
        "total_mb");
    REQUIRE_CONFIG(
        config->file_kb >= 64 && config->file_kb <= 4096
            && config->file_kb <= config->total_mb * 1024,
        "file_kb");
    REQUIRE_CONFIG(config->count >= 1 && config->count <= 256, "count");
    REQUIRE_CONFIG(
        config->window_kb >= 0 && config->window_kb <= 16384, "window_kb");
    REQUIRE_CONFIG(
        config->gc_growth_pct >= 100 && config->gc_growth_pct <= 400,
        "gc_growth_pct");
    REQUIRE_CONFIG(
        config->script_timeout_ms >= 1000
            && config->script_timeout_ms <= 600000,
        "script_timeout_ms");
    REQUIRE_CONFIG(
        config->css_width >= 240 && config->css_width <= 960, "css_width");
    REQUIRE_CONFIG(
        config->css_height >= 136 && config->css_height <= 544, "css_height");
    REQUIRE_CONFIG(
        config->network_profile >= 1 && config->network_profile <= 100,
        "network_profile");
    REQUIRE_CONFIG(
        config->developer_update_url[0] == '\0'
            || tilefinch_update_url_is_valid(
                   config->developer_update_url,
                   sizeof(config->developer_update_url)),
        "developer_update_url");
    REQUIRE_CONFIG(
        config->developer_package_url[0] == '\0'
            || tilefinch_update_url_is_valid(
                   config->developer_package_url,
                   sizeof(config->developer_package_url)),
        "developer_package_url");
    /* A typo here must halt at the config gate rather than quietly reading as
       "off": the whole point of the knob is that a developer knows which
       decoder program the next boot will attempt. */
    REQUIRE_CONFIG(
        psp_media_wide_program_name_valid(config->experimental_wide_video),
        "experimental_wide_video");
    REQUIRE_CONFIG(
        strcmp(config->profile, "lab") == 0
            || strcmp(config->profile, "realistic") == 0
            || strcmp(config->profile, "strict") == 0,
        "profile");
    REQUIRE_CONFIG(
        config->dump_frame >= 0 && config->dump_frame <= 2, "dump_frame");
    REQUIRE_CONFIG(
        config->exit_after_report == 0 || config->exit_after_report == 1,
        "exit_after_report");
    REQUIRE_CONFIG(
        config->interactive_validation_ticks >= 0
            && config->interactive_validation_ticks <= 36000,
        "interactive_validation_ticks");
    REQUIRE_CONFIG(
        config->validation_cancel_after_ms >= 0
            && config->validation_cancel_after_ms <= 30000,
        "validation_cancel_after_ms");
    REQUIRE_CONFIG(
        config->validation_preview_scroll == 0
            || config->validation_preview_scroll == 1,
        "validation_preview_scroll");
    REQUIRE_CONFIG(
        config->validation_media_play == 0
            || config->validation_media_play == 1,
        "validation_media_play");
    REQUIRE_CONFIG(
        config->validation_media_stability_auto == 0
            || config->validation_media_stability_auto == 1,
        "validation_media_stability_auto");
    REQUIRE_CONFIG(
        config->validation_media_stability_seconds >= 10
            && config->validation_media_stability_seconds <= 900,
        "validation_media_stability_seconds");
    REQUIRE_CONFIG(
        config->validation_media_lifecycle_auto == 0
            || config->validation_media_lifecycle_auto == 1,
        "validation_media_lifecycle_auto");
    /* Zero explicitly disables scripted seeks so the stability harness can
       measure ordinary uninterrupted playback. Nonzero targets must land
       inside the stream and leave something after them to play. */
    REQUIRE_CONFIG(
        config->validation_media_seek_permille >= 0
            && config->validation_media_seek_permille < 1000,
        "validation_media_seek_permille");
    REQUIRE_CONFIG(
        config->validation_media_au_dump == 0
            || config->validation_media_au_dump == 1,
        "validation_media_au_dump");
    REQUIRE_CONFIG(
        config->validation_media_refusal_reset == 0
            || config->validation_media_refusal_reset == 1,
        "validation_media_refusal_reset");
    /* A typo must halt at the config gate rather than read as "in place": the
       whole point of setting it is that the run knows which reposition shape
       it is measuring. */
    REQUIRE_CONFIG(
        config->validation_media_reset_mode >= PSP_MEDIA_RESET_MODE_IN_PLACE
            && config->validation_media_reset_mode
                   <= PSP_MEDIA_RESET_MODE_NO_TOUCH,
        "validation_media_reset_mode");
    REQUIRE_CONFIG(
        config->validation_media_fixture_auto == 0
            || config->validation_media_fixture_auto == 1,
        "validation_media_fixture_auto");
    REQUIRE_CONFIG(
        config->validation_raster_fixture_auto == 0
            || config->validation_raster_fixture_auto == 1,
        "validation_raster_fixture_auto");
    REQUIRE_CONFIG(
        config->validation_power_test_auto == 0
            || config->validation_power_test_auto == 1,
        "validation_power_test_auto");
    REQUIRE_CONFIG(
        config->validation_ge_present_probe == 0
            || config->validation_ge_present_probe == 1,
        "validation_ge_present_probe");
    REQUIRE_CONFIG(
        config->validation_csc_order_probe == 0
            || config->validation_csc_order_probe == 1,
        "validation_csc_order_probe");
    REQUIRE_CONFIG(
        config->validation_media_range_probe == 0
            || config->validation_media_range_probe == 1,
        "validation_media_range_probe");
    REQUIRE_CONFIG(
        config->validation_update_auto == 0
            || config->validation_update_auto == 1,
        "validation_update_auto");
    REQUIRE_CONFIG(
        config->validation_update_url[0] == '\0'
            || tilefinch_update_url_is_valid(
                   config->validation_update_url,
                   sizeof(config->validation_update_url)),
        "validation_update_url");
    REQUIRE_CONFIG(
        config->validation_update_auto == 0
            || config->validation_update_url[0] != '\0',
        "validation_update_url");
    /* A leaf name beside the EBOOT. Rejecting separators keeps a boot file
       from naming an arbitrary path on the Memory Stick. */
    REQUIRE_CONFIG(
        strchr(config->input_script, '/') == NULL
            && strchr(config->input_script, '\\') == NULL
            && strstr(config->input_script, "..") == NULL,
        "input_script");
#undef REQUIRE_CONFIG
    return true;
}

bool psp_boot_config_automation_requires_engine_first(
    const PspBootConfig *config)
{
    if (config == NULL) return true;
    /* These probes act on an initial document or its load. Frame capture is
       deliberately absent: it observes whichever native surface shipping
       boot selected and must not change that selection. The media-play flag
       is included explicitly even though today's runner also supplies an
       external URL; the policy must remain correct without that coupling. */
    return config->validation_cancel_after_ms != 0
        || config->validation_preview_scroll != 0
        || config->validation_media_play != 0
        || config->validation_media_stability_auto != 0
        || config->validation_media_fixture_auto != 0
        || config->validation_raster_fixture_auto != 0
        || config->ticks > 0;
}
