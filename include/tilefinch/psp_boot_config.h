#ifndef TILEFINCH_PSP_BOOT_CONFIG_H
#define TILEFINCH_PSP_BOOT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char url[512];
    char trace[256];
    long ticks;
    long tick_ms;
    long limit_mb;
    long heap_mb;
    long total_mb;
    long file_kb;
    long count;
    long window_kb;
    long gc_growth_pct;
    long script_timeout_ms;
    long css_width;
    long css_height;
    long network_profile;
    long dump_frame;
    long exit_after_report;
    long interactive_validation_ticks;
    long validation_cancel_after_ms;
    long validation_preview_scroll;
    long validation_media_play;
    long validation_media_stability_auto;
    /* Explicit soak duration. The ordinary validation default remains two
       minutes; long device gates opt in up to fifteen minutes. */
    long validation_media_stability_seconds;
    /* Validation-only logical lifecycle cycle during the media soak. This
       drives the production quiesce/recover state machine without requesting
       physical firmware sleep, which cannot be auto-woken from user mode. */
    long validation_media_lifecycle_auto;
    /*
     * Where the stability soak's forward seek lands, in permille of the
     * stream's duration. 667 is the two-thirds the fraction was hardcoded to.
     *
     * It is configurable because it is the one variable that separates the two
     * stories about the hang at ~167s: a seek to 667 lands just before it and
     * dies there, while a seek to 333 reaches the same content by playing
     * continuously into it. Same content, different decoder state.
     */
    long validation_media_seek_permille;
    /*
     * Record every video access unit submitted from the seek's decoder reset
     * onward to ms0:/PSP/GAME/tilefinch/au-dump.bin, so a hang collects the
     * byte sequence that produced it without needing another instrumented
     * cycle. Bounded and truncated per run; see psp_media_au_dump_record.
     */
    long validation_media_au_dump;
    /*
     * How a refused access unit is recovered: 0 skips it and keeps decoding,
     * which is what shipping does; 1 resets the decoder, reprimes it, and
     * resumes from the next keyframe.
     *
     * It is a knob because it is the discriminating experiment for the hang
     * that follows the soak's forward seek. Three consecutive device runs put
     * an event=au-refused within ten to seventeen log lines of a Media Engine
     * call that never returned, and the victim call differed across them --
     * twice video, once audio -- so the damage is in shared engine state
     * rather than in one codec. Skipping keeps decoding against whatever the
     * refusal left behind; resetting throws that state away. If the soak
     * survives its two minutes with this set, the skip was the damage.
     *
     * Default 0: shipping behaviour is unchanged until the device says
     * otherwise.
     */
    long validation_media_refusal_reset;
    /*
     * What a reposition -- a seek, or a refusal recovery -- does to the sceMpeg
     * decoder. One key rather than a flag per shape, because the three are
     * alternatives and a run picks exactly one:
     *
     *   0  flush and reprime in place (sceMpegAvcDecodeFlush + sceMpegInitAu).
     *      Today's shipping behaviour and the default.
     *   1  delete the decoder object and build a new one out of the same
     *      buffers, which is the sequence an open performs.
     *   2  do not touch the decoder at all -- no flush, no re-init, no
     *      rebuild. The demuxer lands on a keyframe and decoding continues
     *      with the parameter sets firmware already holds.
     *
     * Mode 2 is the reference's shape, and it is the reason this is a knob at
     * all. PMPlayer Advance -- the hardware-proven raw-NAL player this bridge
     * follows -- issues zero sceMpeg calls on a seek, calls sceMpegInitAu
     * exactly once per file, and has no sceMpegAvcDecodeFlush anywhere in its
     * source. Meanwhile four device soaks have put every Media Engine wedge
     * four to six seconds after one of our resets, while the first prime of a
     * session has never produced one. So the two calls mode 0 makes are
     * precisely the two the reference never makes, and mode 2 removes them.
     */
    long validation_media_reset_mode;
    long validation_media_fixture_auto;
    long validation_raster_fixture_auto;
    long validation_power_test_auto;
    /*
     * Draw synthetic frames through the graphics-engine presenter and check
     * the pixels, then exit. It exists because nothing else can reach that
     * draw off-device: the emulator has no raw-NAL decoder, so no decoded
     * picture is ever produced and the presenter is never called.
     */
    long validation_ge_present_probe;
    /*
     * Decode one picture from the embedded fixture, then re-run the firmware
     * colour conversion over that same picture with a short list of candidate
     * mode words and print what each one wrote. It answers the one question
     * the present probe left open -- whether the surface's byte order can be
     * made the one the graphics engine reads -- and it is device-only, because
     * no emulator has the raw-NAL decoder that produces the picture.
     */
    long validation_csc_order_probe;
    /*
     * Resolve `url`, open both bounded range sources, and read samples far
     * enough to cross fragment boundaries -- with no decoder behind them.
     * The transport half of a playback session, on its own, so a range or
     * fragment-window regression can be seen without the firmware decoder
     * that only real hardware has. It needs the network, so it is a manual or
     * nightly run rather than a gate.
     */
    long validation_media_range_probe;
    /*
     * Validation-only signed-update transaction. Ordinary PSP builds parse
     * these fields so one staged tree can boot after the trial handoff, but
     * the driver which consumes them is compiled only with
     * TILEFINCH_PSP_VALIDATION_LOG. The endpoint therefore cannot override
     * Stable in a shipping EBOOT.
     */
    long validation_update_auto;
    char validation_update_url[768];
    char profile[16];
    /*
     * Optional public/no-credentials developer metadata endpoint. Merely
     * configuring it does not select it; the user must also choose Developer
     * in Experimental options. Developer payloads are unsigned but retain
     * bounded package and per-file digest verification plus A/B rollback.
     */
    char developer_update_url[768];
    /* Optional direct TFUP/package endpoint. This is useful for hosts such
       as OneDrive where two public share links are unrelated. Empty keeps
       the historical same-directory package resolution. */
    char developer_package_url[768];
    /*
     * Historically named decoder override. Empty is the shipping wide
     * program after its PSP-3000 promotion gate; explicit "off" selects the
     * 240p compatibility program. "wide-annexb" additionally converts
     * submitted access units to Annex-B start-code framing, and "boot4"
     * restores the historical Baseline boot type 4 while leaving wide
     * pictures clamped. Diagnostic spellings remain in validation UI only.
     */
    char experimental_wide_video[16];
    /* Names a scripted-input file beside the EBOOT. The input source is
       automation only: it must not change the splash, boot order, or initial
       surface that the same configuration would use on a real PSP. */
    char input_script[64];
    /*
     * Names an EBOOT.PBP to hand control to when this one exits, so a
     * hands-free device run can return to whatever launched it -- PSPLink
     * -- instead of dropping to the XMB and stranding the remote loop.
     * Empty is the shipping state and exits as it always did. Unlike
     * input_script this is a whole Memory Stick path, because the target
     * lives wherever its own installer put it.
     */
    char exit_to[128];
} PspBootConfig;

typedef void (*PspBootConfigWarning)(
    void *context, const char *path, size_t line_number, const char *key);

void psp_boot_config_defaults(PspBootConfig *config);

/*
 * Remove every driver that exists only to automate emulator/device runs.
 * Shipping builds call this after reading boot files, so a validation
 * boot.cfg copied beside an ordinary EBOOT cannot silently take ownership of
 * navigation or controller input. A start URL remains a normal user choice
 * when it appears alone; when an automation driver is present, its coupled
 * target URL is reset to native HOME as part of the same scrub. Production
 * decoder policy remains untouched.
 *
 * Validation builds also use it when a person presses a physical button
 * during a scripted run: that press is an intentional handoff from the
 * harness to the controller, not another input source to merge with it.
 */
void psp_boot_config_disable_automation(PspBootConfig *config);

bool psp_boot_config_load(
    PspBootConfig *config, const char *path,
    PspBootConfigWarning warning, void *warning_context);

bool psp_boot_config_write_overrides(
    const PspBootConfig *config, const char *path);

bool psp_boot_config_validate(
    const PspBootConfig *config, const char **invalid_field);

#define PSP_BOOT_CONFIG_DROPPED_DEVELOPER_UPDATE_URL 0x1u
#define PSP_BOOT_CONFIG_DROPPED_DEVELOPER_PACKAGE_URL 0x2u
/*
 * Clear developer update endpoints that do not meet the update client's
 * policy, returning a mask of which were cleared.
 *
 * A malformed value in these two keys is an ordinary mistake, not a reason
 * to refuse to boot. They are hand-edited on a Memory Stick, they are
 * optional, and a typo in them costs the user only the Developer channel --
 * which is exactly what happens when the keys are absent. Clearing them
 * before validation is what keeps them out of the halting arm; the
 * validator's own rule stays intact for any other caller.
 *
 * This deliberately does not extend to `url=` or any other key: their
 * halt-on-invalid posture predates the developer channel and is unchanged.
 */
unsigned psp_boot_config_drop_invalid_developer_urls(PspBootConfig *config);

/*
 * Clear an exit_to that does not name an EBOOT this build could hand off to,
 * returning true when one was cleared.
 *
 * Same posture as the developer endpoints above, for the same reason: it is
 * hand-edited on a Memory Stick, it is optional, and a typo in it costs a
 * developer only the handoff -- which is exactly what an absent key costs.
 * It is a convenience for the remote loop, never a trust surface: the target
 * is loaded by the firmware under the user's own CFW, so refusing to boot
 * over a bad value would buy nothing and cost a device trip.
 */
bool psp_boot_config_drop_unusable_exit_to(PspBootConfig *config);

/* True only for automation that requires a document to exist before the
   interactive loop. Passive drivers (input scripts, bounded interactive
   ticks, and the power test) deliberately preserve the shipping boot path. */
bool psp_boot_config_automation_requires_engine_first(
    const PspBootConfig *config);

#endif
