#!/usr/bin/env python3
"""Source-level guards for PSP syscall contracts PPSSPP does not enforce."""

from pathlib import Path
import configparser
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()


# String and char literals may contain comment delimiters ("*/*" Accept
# headers, "https://..." URLs). The literal alternatives come first and are
# kept, so the leftmost match protects a literal's interior from ever
# opening a comment; escaped quotes stay inside their literal via \\. .
_COMMENT_TOKENS = re.compile(
    r'"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'"
    r"|/\*.*?\*/"
    r"|//[^\n]*",
    re.DOTALL)


def without_comments(source: str):
    return _COMMENT_TOKENS.sub(
        lambda match: "" if match.group(0)[0] == "/" else match.group(0),
        source)


def call_arguments(source: str, function: str):
    needle = function + "("
    offset = 0
    while True:
        start = source.find(needle, offset)
        if start < 0:
            return
        cursor = start + len(needle)
        depth = 1
        quote = None
        escaped = False
        while cursor < len(source) and depth:
            char = source[cursor]
            if quote is not None:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = None
            elif char in ('"', "'"):
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            cursor += 1
        if depth == 0:
            yield source[start + len(needle):cursor - 1]
        offset = max(cursor, start + len(needle))


def split_arguments(arguments: str):
    parts = []
    start = 0
    depth = 0
    for index, char in enumerate(arguments):
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(arguments[start:index].strip())
            start = index + 1
    parts.append(arguments[start:].strip())
    return parts


def psp_media_session_sources():
    """Return the lifecycle subsystem in service-to-facade order.

    These source-contract tests intentionally inspect PSP-only code which the
    host cannot execute. The session was split into bounded service modules;
    keeping the suite tied to the former monolith would make a file move look
    like a device-contract regression.
    """
    names = (
        "psp_media_open.c",
        "psp_media_seek.c",
        "psp_media_present_session.c",
        "psp_media_buffering.c",
        "psp_media_telemetry.c",
        "psp_media_session.c",
    )
    return "\n".join(
        (ROOT / "src" / name).read_text(encoding="utf-8")
        for name in names)


def psp_firmware_backend_create(source: str):
    """Return the shared physical constructor, not its demux/source wrappers."""
    return source[
        source.index("static bool psp_media_backend_create_track_info("):
        source.index("static void psp_media_collect_demux_tracks(")]


def psp_firmware_backend_destroy(source: str):
    return source[
        source.index("static void psp_media_destroy("):
        source.index("static bool psp_media_backend_create_track_info(")]


class PspSdkContractTests(unittest.TestCase):
    def test_setup_cpu_donation_preserves_priority_and_input_ownership(self):
        transport = without_comments((ROOT / "src/fetch/background_transport.inc").read_text())
        worker = transport[transport.index("static int fetch_background_worker_main("):
                           transport.index("bool fetch_background_transport_available(")]
        self.assertLess(worker.index("&fetch_background.setup_active, true"),
                        worker.index("sceKernelChangeThreadPriority("))
        cooperate = transport[transport.index("void fetch_background_transport_cooperate("):
                              transport.index("bool fetch_background_transport_initialize(")]
        self.assertIn("sceKernelGetThreadId() != fetch_background.owner_thread", cooperate)
        self.assertIn("fetch_background.stop_requested", cooperate)
        self.assertIn("fetch_background_setup_yield_due", cooperate)
        self.assertIn("(status.status & PSP_THREAD_READY) == 0", cooperate)
        self.assertIn("&fetch_background.poll_active", cooperate)
        poll_begin = worker.index("&fetch_background.poll_active, true")
        poll_call = worker.index("CURLMcode poll_code = curl_multi_poll(")
        poll_end = worker.index("&fetch_background.poll_active, false")
        self.assertLess(poll_begin, poll_call)
        self.assertLess(poll_call, poll_end)
        self.assertLess(cooperate.index("fetch_background_setup_yield_due"),
                        cooperate.index("sceKernelReferThreadRunStatus"))
        self.assertIn("sceKernelDelayThread(FETCH_BACKGROUND_SETUP_YIELD_US)", cooperate)
        self.assertNotIn("sceKernelChangeThreadPriority", cooperate)
        runtime = without_comments((ROOT / "src/psp_app/psp_app_runtime.c").read_text())
        checkpoint = runtime[runtime.index("bool psp_platform_cooperate("):
                             runtime.index("#define PSP_BOOT_ENTRANCE_SLOW_US")]
        self.assertLess(checkpoint.index("psp_ui_update("),
                        checkpoint.index("if (running) fetch_background_transport_cooperate(now_us)"))
        self.assertIn("bool running = !tilefinch_cancellation_requested", checkpoint)

    def test_transport_attribution_is_validation_only_and_read_only(self):
        targets = (ROOT / "cmake/TilefinchTargets.cmake").read_text()
        source = (ROOT / "src/psp_transport_probe.c").read_text()
        start = targets.index("if(TILEFINCH_PSP_VALIDATION_LOG AND NOT PSP_BROWSER_CURL_STUB)")
        end = targets.index("endif()", start)
        self.assertIn("src/psp_transport_probe.c", targets[start:end])
        self.assertIn("LINKER:--wrap=${_symbol}", targets[start:end])
        self.assertIn("sceKernelReferThreadRunStatus", source)
        self.assertIn("__real_curl_multi_perform(multi, running)", source)
        self.assertIn("getsockopt(fd, SOL_SOCKET, SO_NONBLOCK", source)
        self.assertNotIn("sceNetInetGetsockopt(", source)
        self.assertNotIn("setsockopt(", source)
        self.assertNotIn("sceKernelChangeThreadPriority(", source)
        self.assertIn("errno = saved_errno;", source)
        # Snapshot before logging: the logger's semaphore is not transport work.
        self.assertLess(source.index("memcpy(calls, probe_calls"),
                        source.index('psp_log_printf("tilefinch-transport-call:'))

    def test_navigation_watchdog_is_one_bounded_minute(self):
        header = (ROOT / "src/psp_app/psp_app_internal.h").read_text()
        self.assertRegex(header, r"#define PSP_NAVIGATION_JOB_TIMEOUT_US UINT64_C\(60000000\)")
        for path in ("src/psp_script_main.c", "src/psp_app/psp_app_page.c"):
            source = (ROOT / path).read_text()
            self.assertIn(">= PSP_NAVIGATION_JOB_TIMEOUT_US", source)
            self.assertIn("browser_engine_cancel_navigation(", source)
            self.assertIn("tilefinch_platform_cooperate(", source)

    def test_ordinary_boot_skips_synthetic_clock_work_and_splash_waits(self):
        source = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        main = source[source.index("int main(int argc, char *argv[])"):]
        probe = main.index("psp_clock_validation_probe();")
        self.assertGreater(probe, main.index('PSP_BOOT_TIMING_MARK("config-accepted")'))
        self.assertIn("if (process.config.validation_power_test_auto != 0)",
                      main[probe - 160:probe])
        self.assertEqual(len(list(call_arguments(
            main, "psp_present_boot_entrance"))), 1)
        self.assertLess(main.index("media_psp_backend_reserve_pool();"),
                        main.index("browser_engine_create("))
        self.assertIn("bool fast_page_followup = native_home_boot;", source)

    def test_home_activation_ack_precedes_navigation_preparation(self):
        source = without_comments(
            (ROOT / "src/psp_app/psp_app_input.c").read_text(encoding="utf-8"))
        ack = source[source.index("const char *psp_ui_action_acknowledgement("):]
        self.assertIn('case PSP_UI_ACTION_HOME_ACTIVATE: return "OPENING...";', ack)
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        ack = main[main.index("const char *action_ack ="):]
        self.assertLess(ack.index("psp_present("),
                        ack.index("psp_app_dispatch_action("))

    def test_speech_engine_is_lazy_slot_owned_and_has_no_private_heap(self):
        targets = (ROOT / "cmake/TilefinchTargets.cmake").read_text()
        browser = targets[targets.index("if(PSP_BROWSER_ENABLE_PSP_VOICE)"):]
        self.assertIn("src/stt/stt_component_loader.c", browser)
        self.assertNotIn("src/stt/stt_engine.c", browser)
        self.assertNotIn("tilefinch_pocketsphinx pspaudio", targets)
        voice = (ROOT / "src/psp_voice_input.c").read_text()
        initialize = voice[voice.index("void psp_voice_input_init("):
                           voice.index("bool psp_voice_input_set_enabled(")]
        self.assertNotIn("psp_voice_component_load", initialize)
        self.assertIn("psp_voice_component_load(voice->budget)", voice)
        config = (ROOT / "cmake/PspVoice.cmake").read_text()
        for allocator in ("malloc", "calloc", "realloc", "free", "memalign",
                          "_malloc_r", "_calloc_r", "_realloc_r", "_free_r"):
            self.assertIn(allocator, config)
        exports = (ROOT / "src/stt/stt_exports.exp").read_text()
        self.assertIn("PSP_EXPORT_FUNC_HASH(module_stop)", exports)
        component = (ROOT / "src/stt/stt_component_psp.c").read_text()
        self.assertNotIn("PSP_HEAP_SIZE_KB", component)
        self.assertIn("__stdio_exit_handler()", component)
        self.assertIn("_reclaim_reent(&retired)", component)
        self.assertIn("host.release(slot)", component)
        self.assertIn("__locks_init()", component)
        self.assertIn("__locks_deinit()", component)
        loader = (ROOT / "src/stt/stt_component_loader.c").read_text()
        self.assertIn('"%s/tilefinch-voice.prx", directory', loader)
        self.assertIn("sceKernelQueryModuleInfo", loader)
        self.assertIn("TILEFINCH_STT_RESIDENT_LIMIT - resident", loader)

    def test_game_profile_v1_matches_runtime_ceiling_contracts(self):
        profile_path = ROOT / "docs/tilefinch-game-profile-v1.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        self.assertEqual(profile["profile_id"],
                         "tilefinch-game-profile-v1")
        self.assertEqual(profile["version"], 1)
        self.assertEqual(profile["authority"], "docs/GAME_PROFILE.md")

        self.assertEqual(profile["memory_profiles"], {
            "shipping": {
                "page_budget_bytes": 24 * 1024 * 1024,
                "javascript_heap_bytes": 5 * 1024 * 1024,
            },
            "strict": {
                "page_budget_bytes": 16 * 1024 * 1024,
                "javascript_heap_bytes": 4 * 1024 * 1024,
            },
        })
        self.assertEqual(profile["limits"]["canvas_2d"], {
            "maximum_width_px": 480,
            "maximum_height_px": 272,
            "maximum_pixels": 131072,
            "maximum_rgba_bytes_per_canvas": 512 * 1024,
            "native_snapshot_count": 4,
            "native_snapshot_bytes_total": 1024 * 1024,
            "pending_commit_count": 8,
            "path_commands": 256,
            "rectangle_batch_commands": 64,
            "image_batch_commands": 16,
            "path_text_batch_commands": 16,
            "image_bitmap_count": 8,
            "image_bitmap_bytes_total": 1024 * 1024,
        })
        self.assertEqual(profile["limits"]["webgl"], {
            "contexts_per_realm": 2,
            "buffer_objects": 24,
            "buffer_bytes_total": 512 * 1024,
            "texture_objects": 8,
            "texture_bytes_total": 416 * 1024,
            "shader_objects": 16,
            "shader_source_bytes_each": 16 * 1024,
            "program_objects": 8,
            "vertex_array_objects": 8,
            "draw_command_template_words": 96,
            "draw_command_template_bytes_per_plan": 384,
            "draw_command_template_bytes_maximum": 3456,
            "framebuffer_objects": 4,
            "renderbuffer_objects": 4,
            "draws_per_submission": 64,
            "effective_vertices_per_draw": 4096,
            "instances_per_draw": 64,
            "payload_sources": 32,
            "maximum_texture_dimension_px": 512,
            "maximum_width_px": 480,
            "maximum_height_px": 272,
            "maximum_pixels": 131072,
            "vertex_attributes": 8,
            "texture_image_units": 1,
            "varying_vectors": 2,
            "vertex_uniform_vectors": 16,
            "fragment_uniform_vectors": 4,
        })
        self.assertEqual(profile["limits"]["web_audio"], {
            "contexts": 1,
            "decoded_buffers": 8,
            "simultaneous_voices": 4,
            "audio_nodes": 16,
            "decoded_pcm_bytes_total": 512 * 1024,
            "mixer_block_frames": 512,
            "schedule_ahead_us": 10 * 1000 * 1000,
            "minimum_sample_rate_hz": 8000,
            "maximum_sample_rate_hz": 48000,
            "channels": [1, 2],
            "sample_bits": [8, 16],
        })
        self.assertEqual(profile["limits"]["offline_installation"], {
            "manifest_bytes": 64 * 1024,
            "document_bytes": 1024 * 1024,
            "resource_count": 32,
            "resource_body_bytes_total": 1024 * 1024,
            "classic_script_bytes_each": 384 * 1024,
            "classic_script_compile_count": 8,
            "classic_script_compile_source_bytes": 512 * 1024,
            "classic_script_bytecode_bytes": 512 * 1024,
            "classic_script_source_fallback": True,
            "resource_pack_bytes": 1024 * 1024 + 512 * 1024 + 160 * 1024,
            "native_icon_width_px": 16,
            "native_icon_height_px": 16,
            "native_icon_rgba_bytes": 16 * 16 * 4,
            "library_items": 12,
        })

        canvas = (ROOT / "src/bootstrap/canvas.js").read_text(
            encoding="utf-8")
        for token in (
            "pixelByteLimit = 512 * 1024",
            "maximumDrawingWidth = 480",
            "maximumDrawingHeight = 272",
            "imageBitmapByteLimit = 1024 * 1024",
            "imageBitmapCountLimit = 8",
            "state.paintCommands.length >= 16",
            "state.rectCommands.length >= 32",
            "target.imageCommands.length >= 16",
            "pendingCommits.length < 8",
            "this._commands.length < 256",
        ):
            self.assertIn(token, canvas)
        image = (ROOT / "src/image.c").read_text(encoding="utf-8")
        for token in (
            "#define IMAGE_CANVAS_SURFACE_LIMIT 4u",
            "#define IMAGE_CANVAS_BYTES_LIMIT (1024u * 1024u)",
            "#define IMAGE_CANVAS_SINGLE_BYTES_LIMIT (512u * 1024u)",
        ):
            self.assertIn(token, image)

        webgl = (ROOT / "src/bootstrap/webgl.js").read_text(
            encoding="utf-8")
        for token in (
            "MAX_CONTEXTS = 2",
            "MAX_BUFFER_BYTES = 512 * 1024",
            "MAX_TEXTURE_BYTES = 416 * 1024",
            "MAX_BUFFER_OBJECTS = 24",
            "MAX_TEXTURE_OBJECTS = 8",
            "MAX_SHADER_OBJECTS = 16",
            "MAX_PROGRAM_OBJECTS = 8",
            "MAX_VERTEX_ARRAY_OBJECTS = 8",
            "MAX_FRAMEBUFFER_OBJECTS = 4",
            "MAX_RENDERBUFFER_OBJECTS = 4",
            "MAX_SHADER_BYTES = 16 * 1024",
            "MAX_DRAWS = 64",
            "MAX_INSTANCES = 64",
            "MAX_VERTICES = 4096",
            "MAX_DRAWING_WIDTH = 480",
            "MAX_DRAWING_HEIGHT = 272",
            "MAX_DRAWING_PIXELS = 131072",
        ):
            self.assertIn(token, webgl)

        audio_header = (ROOT / "include/tilefinch/game_audio.h").read_text(
            encoding="utf-8")
        for token in (
            "#define TILEFINCH_GAME_AUDIO_BUFFER_LIMIT 8u",
            "#define TILEFINCH_GAME_AUDIO_VOICE_LIMIT 4u",
            "#define TILEFINCH_GAME_AUDIO_PCM_BYTE_LIMIT (512u * 1024u)",
            "#define TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES 512u",
            "#define TILEFINCH_GAME_AUDIO_SCHEDULE_LIMIT_SECONDS 10.0",
        ):
            self.assertIn(token, audio_header)
        audio_js = (ROOT / "src/bootstrap/game-audio.js").read_text(
            encoding="utf-8")
        self.assertIn("nodeLimit = 16", audio_js)
        self.assertIn("Only one game AudioContext is available", audio_js)

        offline = (ROOT / "include/tilefinch/offline_library.h").read_text(
            encoding="utf-8")
        for token in (
            "#define OFFLINE_LIBRARY_ITEM_LIMIT 12u",
            "#define OFFLINE_LIBRARY_APP_DOCUMENT_LIMIT (1024u * 1024u)",
            "#define OFFLINE_LIBRARY_APP_RESOURCE_LIMIT (1024u * 1024u)",
            "#define OFFLINE_LIBRARY_APP_BYTECODE_LIMIT (512u * 1024u)",
            "+ OFFLINE_LIBRARY_APP_BYTECODE_LIMIT + 160u * 1024u)",
            "#define OFFLINE_LIBRARY_APP_ICON_EDGE 16u",
        ):
            self.assertIn(token, offline)
        manifest = (ROOT / "include/tilefinch/web_app_manifest.h").read_text(
            encoding="utf-8")
        self.assertIn(
            "#define TILEFINCH_WEB_APP_MANIFEST_LIMIT (64u * 1024u)",
            manifest)
        session = (ROOT / "include/tilefinch/session.h").read_text(
            encoding="utf-8")
        self.assertIn("#define BROWSER_STORAGE_ENTRIES 64", session)
        self.assertIn("#define BROWSER_OFFLINE_CACHE_ENTRY_LIMIT 32u",
                      session)
        session_source = (ROOT / "src/session.c").read_text(
            encoding="utf-8")
        self.assertIn("#define DEFAULT_STORAGE_BYTES (16u * 1024u)",
                      session_source)

        docs_map = (ROOT / "docs/README.md").read_text(encoding="utf-8")
        guide = (ROOT / "docs/GAME_PROFILE.md").read_text(encoding="utf-8")
        self.assertIn("[Game Profile v1](GAME_PROFILE.md)", docs_map)
        self.assertIn("tilefinch-game-profile-v1.json", guide)

    def test_activation_cooperates_and_acknowledges_only_a_successful_latch(self):
        actions = without_comments((ROOT / "src/psp_app/psp_app_actions.c").read_text())
        begin = actions.index("bool activation_scope =")
        end = actions.index("if (activated) {", begin)
        scope = actions[begin:end]
        self.assertIn("!psp_navigation_cooperate_active()", scope)
        self.assertIn("!app->process->presentation.ui.page_gamepad_capture", scope)
        self.assertLess(scope.index("psp_runtime_cooperate_begin("),
                        scope.index("browser_engine_activate("))
        self.assertLess(scope.index("browser_engine_activate("),
                        scope.index("psp_runtime_cooperate_end("))
        inputs = without_comments((ROOT / "src/psp_app/psp_app_input.c").read_text())
        ack = inputs[inputs.index("bool psp_present_action_ack("):]
        self.assertLess(ack.index("if (!shown) return false;"),
                        ack.index("tilefinch-input-ack:"))
        main = without_comments((ROOT / "src/psp_script_main.c").read_text())
        dispatch = main.index("psp_app_dispatch_action(&app, &frame, &intent)")
        self.assertIn("psp_ui_set_page_activation(&process->presentation.ui, false)",
                      main[dispatch:dispatch + 300])

    def test_runtime_input_cooperation_is_lazy_and_owner_thread_only(self):
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(encoding="utf-8"))
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        begin = runtime[runtime.index("void psp_runtime_cooperate_begin("):
                        runtime.index("bool psp_runtime_cooperate_end(")]
        self.assertIn("ui->page_gamepad_capture", begin)
        self.assertNotIn("supervisor_ui =", begin)
        self.assertIn("sceKernelGetThreadId()", begin)
        state = (ROOT / "src/psp_app/psp_app_internal.h").read_text()
        self.assertIn("bool input_yield;", state)
        start = runtime[runtime.index("static void psp_work_cooperate_begin_internal("):
                        runtime.index("void psp_work_cooperate_begin(")]
        self.assertIn("psp_navigation_cooperate.input_yield = false", start)
        end = runtime[runtime.index("void psp_navigation_cooperate_end("):
                      runtime.index("bool psp_navigation_cooperate_take_media_intent(")]
        self.assertIn("psp_navigation_cooperate.input_yield", end)
        self.assertNotIn("psp_runtime_cooperate.input_yield", runtime)
        self.assertIn("memset(&psp_navigation_cooperate, 0", end)
        tick = runtime[runtime.index("static void psp_work_ui_tick(bool owner_thread)\n{"):
                       runtime.index("bool psp_platform_cooperate(")]
        self.assertLess(tick.index("cooperate->owner_thread_only != owner_thread"),
                        tick.index("__sync_bool_compare_and_swap"))
        self.assertIn('"PAGE UPDATE - INPUT QUEUED"', tick)
        self.assertIn("cooperate->pending_page_input_count++", tick)
        self.assertIn("else if (!psp_present_supervisor_ui(", tick)
        self.assertLess(tick.index("else if (!psp_present_supervisor_ui("),
                        tick.index("cooperate->presentations++"))
        cooperate = runtime[runtime.index("bool psp_platform_cooperate("):]
        self.assertLess(cooperate.index("sceKernelGetThreadId()"),
                        cooperate.index("psp_work_ui_tick(true)"))
        self.assertIn("UINT64_C(8000)", cooperate)
        self.assertIn("UINT64_C(16000)", cooperate)
        pointer = main[main.index("static TILEFINCH_OUT_OF_LINE bool psp_dispatch_page_pointer("):
                       main.index("static TILEFINCH_OUT_OF_LINE bool psp_advance_page_runtime(")]
        self.assertIn("bool supervise = !psp_navigation_cooperate_active()", pointer)
        self.assertLess(pointer.index("psp_runtime_cooperate_begin("),
                        pointer.index("browser_engine_pointer_event("))
        self.assertLess(pointer.index("browser_engine_pointer_event("),
                        pointer.index("psp_runtime_cooperate_end("))
        self.assertIn("supervise && psp_runtime_cooperate_end", pointer)
        self.assertIn("psp_cursor_latency_sample(now_us)", tick)
        advance = main[main.index("static TILEFINCH_OUT_OF_LINE bool psp_advance_page_runtime("):
                       main.index("psp_advance_page_runtime_with_reader_recovery(")]
        self.assertLess(advance.index("psp_runtime_cooperate_begin("),
                        advance.index("browser_engine_advance_runtime("))
        self.assertLess(advance.index("browser_engine_advance_runtime("),
                        advance.index("psp_runtime_cooperate_end("))
        self.assertIn("app->views->frame", advance)
        self.assertIn("timed_runtime = timed_navigation == NULL ? NULL", advance)
        # GE submissions are synchronous and never expose a cooperation hook
        # while a list is under construction or in flight.
        webgl = without_comments(
            (ROOT / "src/js_webgl_bridge.c").read_text(encoding="utf-8"))
        self.assertNotIn("tilefinch_platform_cooperate(", webgl)
        self.assertIn("sceGuSync(0, 0)", webgl)

    def test_home_exit_returns_callback_thread_to_the_busy_supervisor(self):
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        callback = runtime[
            runtime.index("static int psp_exit_callback("):
            runtime.index("bool psp_home_exit_pending(")]
        self.assertIn("psp_start_home_exit_watchdog()", callback)
        self.assertIn("if (psp_start_home_exit_watchdog()) return 0;",
                      callback)
        watchdog = runtime[
            runtime.index("static int psp_home_exit_watchdog("):
            runtime.index("static int psp_exit_callback(")]
        self.assertIn("PSP_HOME_EXIT_GRACE_MS", watchdog)
        self.assertIn("sceKernelExitGame();", watchdog)

        tick = runtime[
            runtime.index("void psp_background_ui_tick(void)"):
            runtime.index("bool psp_platform_cooperate(")]
        self.assertIn("if (psp_home_exit_pending()", tick)
        self.assertIn(
            "tilefinch_cancellation_request(&cooperate->cancellation)",
            tick)
        self.assertIn('"CLOSING..."', tick)

    def test_xmb_redirect_is_event_driven_read_only_and_fail_open(self):
        source = without_comments(
            (ROOT / "src/psp_xmb_redirect.c").read_text(encoding="utf-8"))

        self.assertIn("sctrlHENSetStartModuleHandler(on_module_start)", source)
        self.assertIn("TILEFINCH_XMB_REDIRECT_BYPASS_BUTTON PSP_CTRL_LTRIGGER",
                      source)
        self.assertIn("sceIoGetstat(tilefinch_ms_launcher", source)
        self.assertIn("sceIoGetstat(tilefinch_ef_launcher", source)
        self.assertIn("previous_start_handler(module)", source)
        self.assertIn("launch_armed = 0;", source)
        self.assertIn("TILEFINCH_XMB_REDIRECT_ACTIVATION_BUTTONS", source)
        handler = source[
            source.index("static int on_module_start("):
            source.index("int module_start(")]
        for loader_unsafe_call in (
                "sceCtrlPeekBufferPositive", "sceIoGetstat",
                "sceKernelDelayThread", "sceKernelCreateThread",
                "sceKernelStartThread", "sctrlKernelLoadExecVSH"):
            self.assertNotIn(loader_unsafe_call, handler)
        self.assertLess(
            handler.index("previous_start_handler(module)"),
            handler.index("sceKernelSetEventFlag("))
        self.assertIn("sceKernelWaitEventFlag(", source)
        self.assertIn("TILEFINCH_XMB_REDIRECT_MODULE_SETTLE_US", source)
        self.assertIn(
            "TILEFINCH_XMB_REDIRECT_MODULE_SETTLE_US) < 0",
            source)
        module_start = source[source.index("int module_start("):]
        self.assertLess(
            module_start.index("sceKernelStartThread("),
            module_start.index(
                "sctrlHENSetStartModuleHandler(on_module_start)"))
        self.assertIn("previous == on_module_start ? NULL : previous", source)
        self.assertIn("char launcher[sizeof(tilefinch_ms_launcher)]", source)
        cmake = (ROOT / "cmake/TilefinchTargets.cmake").read_text(
            encoding="utf-8")
        redirect_target = cmake[
            cmake.index("add_prx_module(tilefinch-xmb-redirect"):
            cmake.index("# Stable, deliberately small A/B launcher")]
        self.assertIn("-fno-pic", redirect_target)
        self.assertNotIn("sctrlHENPatchSyscall", source)
        self.assertNotIn("sctrlHookImportByNID", source)
        for mutation in (
                "sceIoOpen(", "sceIoWrite(", "sceIoRemove(",
                "sceIoRename(", "sceIoMkdir(", "sceIoRmdir("):
            self.assertNotIn(
                mutation, source,
                "The optional VSH redirect must never mutate storage.")

    def test_psplink_deployer_is_bounded_and_transactional(self):
        source = without_comments(
            (ROOT / "tools" / "psplink-deploy" / "main.c").read_text(
                encoding="utf-8"))

        self.assertIn("TF_DEPLOY_CHUNK_BYTES (1024u * 1024u)", source)
        self.assertIn("TF_DEPLOY_MAX_BYTES (8u * 1024u * 1024u)", source)
        self.assertIn("TfDeployWriteBound", source)
        self.assertIn('"ms0:/PSP/GAME/TILEFINCH/slot-a/EBOOT.PBP"', source)
        self.assertIn("write_calls > 8u", source)
        self.assertEqual(1, source.count("sceIoWrite(file, data, size)"))
        self.assertIn("read_unit(source_file, transfer_buffer, request)",
                      source)
        self.assertIn('destination, ".new"', source)
        self.assertIn('destination, ".previous"', source)
        self.assertLess(
            source.index('sceIoRename(previous, destination)'),
            source.index('sceIoRemove(temporary)'))
        self.assertLess(
            source.index("sceIoRename(destination, previous)"),
            source.index("sceIoRename(temporary, destination)"))
        self.assertIn('sceIoSync("ms0:", 0)', source)
        self.assertIn("sceKernelSelfStopUnloadModule(1, 0, NULL)", source)
        self.assertNotIn("tilefinch-validation", source)
        self.assertNotIn("tilefinch-crash", source)

    def test_psplink_module_guards_accept_device_crlf(self):
        source = (ROOT / "scripts/psplink-device.sh").read_text()
        helpers = source[source.index("unload_named_modules()"):
                         source.index("PSPSH=$PSPSH HOST_ROOT=$HOST_ROOT")]
        stub = ('set -eu\nLINK_TIMEOUT_SECONDS=1\n'
                'run_pspsh() {\n'
                '  if [ "$1" = modlist ]; then\n'
                '    printf "UID: 1 Name: tfdeploy\\r\\n"\n'
                '  else printf "%s\\n" "$1"; fi\n}\n')
        result = subprocess.run(["sh", "-c", stub + helpers +
                                 '\nunload_named_modules tfdeploy\n'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("modstun @tfdeploy", result.stdout)
        result = subprocess.run(["sh", "-c", stub + helpers +
                                 '\nrequire_module_absent tfdeploy\n'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        native = (ROOT / "tools/psplink-deploy/main.c").read_text()
        self.assertIn("PSP_HEAP_SIZE_KB(64)", native)
        self.assertLess(native.index("__libcglue_deinit();"),
                        native.index("sceKernelSelfStopUnloadModule(1, 0, NULL)"))

    def test_psplink_slot_stages_matching_components_before_eboot(self):
        stage = ROOT / "scripts/stage-psplink-slot.sh"
        deploy = (ROOT / "scripts/psplink-device.sh").read_text()
        native = (ROOT / "tools/psplink-deploy/main.c").read_text()
        for component in ("wasm", "voice"):
            self.assertIn(f'"host0:/tilefinch-{component}-device-latest.prx"', native)
            self.assertIn(f'"ms0:/PSP/GAME/TILEFINCH/slot-a/tilefinch-{component}.prx"', native)
        self.assertIn('"ld host0:/tfdeploy.prx $artifact"', deploy)
        self.assertIn('require_module_absent tilefinch_wasm', deploy)
        self.assertIn('require_module_absent tilefinch_voice', deploy)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, host = root / "build", root / "host"
            build.mkdir()
            host.mkdir()
            config = build / "CMakeCache.txt"
            config.write_text("PSP_BROWSER_ENABLE_PSP_VOICE:BOOL=ON\n")
            (build / "EBOOT.PBP").write_bytes(b"\0PBPbrowser")
            (build / "tilefinch-wasm.prx").write_bytes(b"\x7fELFwasm-abi7")
            (host / "EBOOT-device-latest.PBP").write_bytes(b"old-stage")
            result = subprocess.run(["sh", str(stage), str(build), str(host)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((host / "EBOOT-device-latest.PBP").read_bytes(), b"old-stage")
            (build / "tilefinch-voice.prx").write_bytes(b"\x7fELFvoice")
            result = subprocess.run(["sh", str(stage), str(build), str(host)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.split(), [b"wasm", b"voice", b"eboot"])
            self.assertEqual((host / "tilefinch-wasm-device-latest.prx").read_bytes(), b"\x7fELFwasm-abi7")
            self.assertEqual((host / "tilefinch-voice-device-latest.prx").read_bytes(), b"\x7fELFvoice")
            # A stale voice artifact in a validation tree is not enabled by
            # mere file existence. Missing/malformed enabled files refuse.
            config.write_text("PSP_BROWSER_ENABLE_PSP_VOICE:BOOL=OFF\n")
            (build / "tilefinch-voice.prx").write_bytes(b"stale")
            result = subprocess.run(["sh", str(stage), str(build), str(host)], capture_output=True)
            self.assertEqual(result.stdout.split(), [b"wasm", b"eboot"])
            self.assertEqual(result.returncode, 0, result.stderr)
            (build / "tilefinch-wasm.prx").write_bytes(b"wrong header")
            result = subprocess.run(["sh", str(stage), str(build), str(host)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"Invalid slot artifact header", result.stderr)

    def test_render_shell_fault_uses_shared_host_fault_record(self):
        source = (ROOT / "src/browser_engine.c").read_text()
        faults = (ROOT / "src/tilefinch_test_faults.h").read_text()
        self.assertIn("bool refuse_next_render_shell_init;", faults)
        self.assertIn("tilefinch_test_faults()->refuse_next_render_shell_init = true", source)
        self.assertIn("tilefinch_test_faults()->refuse_next_render_shell_init = false", source)
        self.assertNotIn("static bool browser_engine_test_refuse_render_shell_init", source)

    def test_quickjs_variant_refreshes_header_and_supporting_sources(self):
        dependencies = (ROOT / "cmake/TilefinchDependencies.cmake").read_text()
        # Exercise the production preparation block in a disposable, dependency-
        # free CMake project; never alter a shared engine or build tree.
        block = dependencies.split("if(PSP_BROWSER_USE_BELLARD_QUICKJS)\n    # The engine", 1)[1]
        block = "if(PSP_BROWSER_USE_BELLARD_QUICKJS)\n    # The engine" + block.split("    add_library(qjs STATIC", 1)[0] + "endif()\n"
        with tempfile.TemporaryDirectory() as temporary:
            source, build = Path(temporary) / "source", Path(temporary) / "build"
            vendor = source / "third_party/quickjs"
            shutil.copytree(ROOT / "third_party/quickjs", vendor)
            (source / "patches").symlink_to(ROOT / "patches", target_is_directory=True)
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.24)\nproject(qjs_review NONE)\n'
                'find_program(PATCH_EXECUTABLE patch REQUIRED)\n'
                'set(PSP_BROWSER_USE_BELLARD_QUICKJS ON)\n'
                'set(PSP_BROWSER_QUICKJS_CAPTURE_GETTER_FASTPATH OFF)\n'
                'set(PSP_BROWSER_QUICKJS_COMPACT_CHAR_ARRAY ON)\n'
                'set(PSP_BROWSER_JS_PROPERTY_FAULT_TRACE OFF)\n' + block +
                'add_custom_target(check_copy ALL COMMAND "${CMAKE_COMMAND}" -E compare_files '
                '"${quickjs_SOURCE_DIR}/cutils.h" "${tilefinch_quickjs_vendor_dir}/cutils.h")\n')
            command = ["cmake", "-S", str(source), "-B", str(build)]
            result = subprocess.run(command, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            header = build / "quickjs-variant/quickjs.h"
            header.write_text(header.read_text() + "\n/* stale variant */\n")
            result = subprocess.run(command, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(header.read_bytes(), (vendor / "quickjs.h").read_bytes())
            support = vendor / "cutils.h"
            support.write_text(support.read_text() + "\n/* vendor update */\n")
            # No explicit configure: the ordinary build must refresh the copy.
            result = subprocess.run(["cmake", "--build", str(build)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((build / "quickjs-variant/cutils.h").read_bytes(), support.read_bytes())
            cache = (build / "CMakeCache.txt").read_text()
            self.assertIn(f"TILEFINCH_QUICKJS_COMPILE_SOURCE_DIR:INTERNAL={build}/quickjs-variant", cache)

    def test_incremental_benchmark_checks_selected_engine_not_fetch_override(self):
        spec = importlib.util.spec_from_file_location(
            "tilefinch_build_benchmark", ROOT / "benchmarks/measure-incremental-build.py")
        benchmark = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(benchmark)
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            for selected in (ROOT / "third_party/quickjs", build / "quickjs-variant"):
                (build / "CMakeCache.txt").write_text(
                    "FETCHCONTENT_SOURCE_DIR_QUICKJS:PATH=/obsolete/engine\n"
                    f"TILEFINCH_QUICKJS_COMPILE_SOURCE_DIR:INTERNAL={selected}\n")
                self.assertEqual(benchmark.compiled_quickjs_directory(build), selected)

    def test_psplink_screenshot_requires_the_user_partition_capability(self):
        patch = (ROOT / "tools/psplink-home-exit/"
                 "psplink-v3.2.1-safe-screenshot.patch").read_text()
        added = "\n".join(line[1:] for line in patch.splitlines()
                          if line.startswith("+") and not line.startswith("+++"))
        self.assertIn('sceKernelAllocPartitionMemory(2, "scrshot"', added)
        self.assertNotIn("sceKernelAllocPartitionMemory(4,", added)
        self.assertIn('SHELL_CMD("scrshot-user"', added)
        builder = (ROOT / "scripts/build-psplink-home-exit.sh").read_text()
        self.assertIn("psplink-v3.2.1-safe-screenshot.patch", builder)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pspsh = root / "pspsh"
            pspsh.write_text(
                '#!/bin/sh\nif [ "$2" = ver ]; then echo "PSPLink v3.2.1"; '
                'else printf "%s\\n" "$2"; fi\n')
            pspsh.chmod(0o755)
            (root / ".tilefinch-usbhostfs.pid").write_text(str(os.getpid()))
            (root / ".tilefinch-usbhostfs.root").write_text(str(root))
            environment = dict(os.environ, HOST_ROOT=str(root),
                               PSPLINK_STATE_DIR=str(root), PSPSH=str(pspsh),
                               USBHOSTFS="/usr/bin/true",
                               PSPLINK_POLLS_PER_SECOND="10")
            for command in ("scrshot", "ss", "scrshot host0:/frame.bmp",
                            "ss host0:/frame.bmp"):
                result = subprocess.run(
                    [str(ROOT / "scripts/psplink-shell.sh"), "exec", command],
                    env=environment, capture_output=True, text=True,
                    timeout=5, check=False)
                self.assertEqual(0, result.returncode, result.stderr)
                expected = "scrshot-user" + (
                    " host0:/frame.bmp" if " " in command else "")
                self.assertEqual(expected, result.stdout.strip())

    def test_psplink_device_loop_establishes_the_host_bridge(self):
        shell = (ROOT / "scripts/psplink-shell.sh").read_text(
            encoding="utf-8")
        deploy = (ROOT / "scripts/psplink-device.sh").read_text(
            encoding="utf-8")
        youtube = (ROOT / "scripts/run-psplink-youtube-ux.sh").read_text(
            encoding="utf-8")
        self.assertIn('nohup "$USBHOSTFS" "$HOST_ROOT"', shell)
        self.assertIn("BRIDGE_START_TIMEOUT_SECONDS", shell)
        self.assertIn('run_pspsh ver "$LINK_TIMEOUT_SECONDS"', shell)
        self.assertIn('scripts/psplink-shell.sh" ready', deploy)
        self.assertIn('scripts/psplink-shell.sh" ready', youtube)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            host = root / "host"
            host.mkdir()
            second_host = root / "second-host"
            second_host.mkdir()
            ready = root / "ready"
            active_root = root / "active-root"
            pspsh = root / "pspsh"
            usbhostfs = root / "usbhostfs_pc"
            pspsh.write_text(
                "#!/bin/sh\n"
                "if [ -f \"$MOCK_PSP_READY\" ] "
                "&& [ \"$(cat \"$MOCK_ACTIVE_ROOT\")\" = "
                "\"$MOCK_EXPECTED_ROOT\" ]; then\n"
                "  echo 'PSPLink v3.2.1'\n"
                "  exit 0\n"
                "fi\n"
                "echo 'connect: Connection refused' >&2\n"
                "exit 1\n", encoding="utf-8")
            usbhostfs.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$1\" >\"$MOCK_ACTIVE_ROOT\"\n"
                ": > \"$MOCK_PSP_READY\"\n"
                "sleep 8\n", encoding="utf-8")
            pspsh.chmod(0o755)
            usbhostfs.chmod(0o755)
            environment = os.environ.copy()
            environment.update({
                "HOST_ROOT": str(host),
                "PSPLINK_STATE_DIR": str(root),
                "PSPSH": str(pspsh),
                "USBHOSTFS": str(usbhostfs),
                "MOCK_PSP_READY": str(ready),
                "MOCK_ACTIVE_ROOT": str(active_root),
                "MOCK_EXPECTED_ROOT": str(host),
                "LINK_TIMEOUT_SECONDS": "2",
                "BRIDGE_START_TIMEOUT_SECONDS": "3",
                "PSPLINK_POLLS_PER_SECOND": "10",
            })
            result = subprocess.run(
                [str(ROOT / "scripts/psplink-shell.sh"), "ready"],
                env=environment, capture_output=True, text=True,
                timeout=8, check=False)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertIn("PSPLink ready", result.stdout)
            self.assertTrue(ready.exists())
            self.assertTrue((root / ".tilefinch-usbhostfs.pid").exists())
            self.assertEqual(
                str(host),
                (root / ".tilefinch-usbhostfs.root").read_text(
                    encoding="utf-8").strip())

            environment["HOST_ROOT"] = str(second_host)
            environment["MOCK_EXPECTED_ROOT"] = str(second_host)
            switched = subprocess.run(
                [str(ROOT / "scripts/psplink-shell.sh"), "ready"],
                env=environment, capture_output=True, text=True,
                timeout=8, check=False)
            self.assertEqual(0, switched.returncode, switched.stderr)
            self.assertEqual(
                str(second_host), active_root.read_text(
                    encoding="utf-8").strip())
            self.assertEqual(
                str(second_host),
                (root / ".tilefinch-usbhostfs.root").read_text(
                    encoding="utf-8").strip())
            bridge_pid = int((root / ".tilefinch-usbhostfs.pid").read_text(
                encoding="utf-8").strip())
            try:
                os.kill(bridge_pid, 15)
            except ProcessLookupError:
                pass

    def test_psplink_bridge_reports_host_socket_denial(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            host = root / "host"
            host.mkdir()
            pspsh = root / "pspsh"
            usbhostfs = root / "usbhostfs_pc"
            pspsh.write_text(
                "#!/bin/sh\n"
                "echo 'connect: Connection refused' >&2\n"
                "exit 1\n", encoding="utf-8")
            usbhostfs.write_text(
                "#!/bin/sh\n"
                "echo 'bind: Operation not permitted' >&2\n"
                "sleep 8\n", encoding="utf-8")
            pspsh.chmod(0o755)
            usbhostfs.chmod(0o755)
            environment = os.environ.copy()
            environment.update({
                "HOST_ROOT": str(host),
                "PSPLINK_STATE_DIR": str(root),
                "PSPSH": str(pspsh),
                "USBHOSTFS": str(usbhostfs),
                "LINK_TIMEOUT_SECONDS": "1",
                "BRIDGE_START_TIMEOUT_SECONDS": "3",
                "PSPLINK_POLLS_PER_SECOND": "10",
            })
            result = subprocess.run(
                [str(ROOT / "scripts/psplink-shell.sh"), "ready"],
                env=environment, capture_output=True, text=True,
                timeout=8, check=False)
            self.assertNotEqual(0, result.returncode)
            self.assertIn(
                "host environment denied usbhostfs_pc", result.stderr)
            self.assertIn(
                "does not need to be relaunched", result.stderr)

    def test_official_psp_preset_embeds_the_pinned_public_update_root(self):
        presets = json.loads(
            (ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        psp = next(
            preset for preset in presets["configurePresets"]
            if preset["name"] == "psp")
        self.assertEqual(
            "${sourceDir}/trust/root-v1.tfur",
            psp["cacheVariables"].get("TILEFINCH_UPDATE_ROOT_V1"))

        root = (ROOT / "trust/root-v1.tfur").read_bytes()
        self.assertEqual(212, len(root))
        self.assertEqual(
            "0eb708ab00b966a70d7220555718ec421"
            "158c1f14df2c506616e54fd27c51777",
            hashlib.sha256(root).hexdigest())
        self.assertNotIn(b"PRIVATE KEY", root)

    def test_media_lifecycle_stats_survive_a_busy_worker_sample(self):
        session = without_comments(
            (ROOT / "src/psp_media_session.c").read_text(encoding="utf-8"))
        snapshot = session[
            session.index("bool psp_media_backend_stats_snapshot("):
            session.index("void psp_media_pipeline_destroy(")]
        self.assertIn("media_playback_backend_stats(media->playback, stats)",
                      snapshot)
        self.assertIn("media->backend_stats_snapshot = *stats", snapshot)
        self.assertIn("if (!media->backend_stats_snapshot_valid)", snapshot)
        self.assertIn("*stats = media->backend_stats_snapshot", snapshot)
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        finish = main[
            main.index("psp_finish_media_stability("):
            main.index("psp_media_stability_schedule_seeks(")]
        self.assertIn("psp_media_backend_stats_snapshot(", finish)

    def test_browser_uses_the_explicit_newlib_heap_threshold(self):
        source = (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8")
        self.assertIn("PSP_HEAP_SIZE_KB(-1);", source)
        self.assertIn("PSP_HEAP_THRESHOLD_SIZE_KB(2048);", source)
        self.assertNotIn("PSP_HEAP_SIZE_KB(-4096);", source)
        self.assertNotIn("PSP_HEAP_SIZE_KB(-2048);", source)

    def test_nonblocking_code_does_not_use_timed_thread_waits(self):
        offenders = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".c", ".inc"}:
                continue
            source = without_comments(path.read_text(encoding="utf-8"))
            for arguments in call_arguments(source, "sceKernelWaitThreadEnd"):
                parts = split_arguments(arguments)
                if len(parts) != 2 or parts[1] != "NULL":
                    offenders.append(
                        f"{path.relative_to(ROOT)}: "
                        f"sceKernelWaitThreadEnd({arguments})")
        self.assertEqual(
            [], offenders,
            "Timed PSP thread waits must go through "
            "psp_thread_wait_end_bounded; nonblocking observation must use "
            "psp_thread_observe. A zero timeout pointer is rejected by real "
            "6.6x firmware even when PPSSPP accepts it.")

    def test_thread_status_struct_initialization_is_centralized(self):
        offenders = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".c", ".inc"}:
                continue
            source = without_comments(path.read_text(encoding="utf-8"))
            if "sceKernelReferThreadStatus(" in source:
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(
            [], offenders,
            "Use psp_thread_observe so SceKernelThreadInfo.size is always "
            "initialized before the firmware call.")

    def test_utility_module_result_classification_is_centralized(self):
        offenders = []
        calls = ("sceUtilityLoadAvModule(", "sceUtilityLoadNetModule(")
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".c", ".inc"}:
                continue
            source = without_comments(path.read_text(encoding="utf-8"))
            if any(call in source for call in calls):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(
            [], offenders,
            "Use psp_utility_module_contract so the negative "
            "already-resident result is not mistaken for failure and module "
            "ownership remains explicit.")

    def test_av_module_load_accepts_both_already_resident_codes(self):
        source = without_comments(
            (ROOT / "src/psp_module_policy.h").read_text(encoding="utf-8"))
        net = source[
            source.index("psp_utility_net_module_load_disposition(int"):
            source.index("psp_utility_av_module_load_disposition(int")]
        av = source[
            source.index("psp_utility_av_module_load_disposition(int"):
            source.index("static inline bool psp_utility_module_load_owned(")]
        self.assertIn("PSP_MODULE_AV_ALREADY_LOADED", av)
        self.assertIn("PSP_MODULE_NET_ALREADY_LOADED", av)
        self.assertIn("PSP_MODULE_NET_ALREADY_LOADED", net)
        self.assertNotIn(
            "PSP_MODULE_AV_ALREADY_LOADED", net,
            "Widening the AV classifier must not weaken the network "
            "already-resident boundary.")

    def test_psp_wall_clock_is_not_monotonic_uptime(self):
        offenders = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".c", ".inc"}:
                continue
            source = without_comments(path.read_text(encoding="utf-8"))
            if re.search(r"\.wall_time_ns\s*=\s*psp_time_ns\b", source):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(
            [], offenders,
            "PSP wall_time_ns must use its RTC-backed Unix epoch, not "
            "sceKernelGetSystemTimeWide uptime; TLS/media expiry and Date "
            "semantics otherwise diverge only on device.")

    def test_media_suspend_invalidates_process_global_decoder_state(self):
        session = (ROOT / "src/psp_media_session.c").read_text(
            encoding="utf-8")
        suspend = session[session.index("void psp_media_suspend("):
                          session.index("void psp_media_resume(")]
        self.assertIn("psp_media_pipeline_destroy(media);", suspend)
        self.assertIn("media_psp_backend_system_suspend();", suspend)
        self.assertLess(
            suspend.index("psp_media_pipeline_destroy(media);"),
            suspend.index("media_psp_backend_system_suspend();"),
            "The per-stream worker must stop before sceMpegFinish resets "
            "process-global firmware state.")
        backend = (ROOT / "src/media_backend_psp.c").read_text(
            encoding="utf-8")
        system_suspend = backend[
            backend.index("void media_psp_backend_system_suspend("):
            backend.index("static const char *psp_media_module_failure_code")]
        self.assertIn("psp_media_me_boot_type = -1;", system_suspend)
        self.assertNotIn(
            "psp_media_me_boot_type = PSP_MEDIA_DEFAULT_ME_BOOT_TYPE;",
            system_suspend,
            "Resume must not guess whether firmware retained or reset the "
            "pre-suspend Media Engine program.")

    def test_media_close_retires_first_frame_and_seek_continuations(self):
        source = without_comments(
            psp_media_session_sources())
        close = source[
            source.index("void psp_media_close("):
            source.index("void psp_media_execute_intent(")]
        self.assertIn("media->open_service_pending = false;", close)
        self.assertIn("psp_media_cancel_decode(media);", close)
        self.assertIn("media->ui.visible = false;", close)
        self.assertIn("psp_media_pipeline_destroy(media);", close)
        self.assertLess(
            close.index("psp_media_cancel_decode(media);"),
            close.index("media->ui.visible = false;"),
            "Close must retire the pending first-frame/seek continuation "
            "before hiding the player, or the next main-loop iteration "
            "re-enters native decode after Circle was acknowledged.")

    def test_cancelled_native_media_work_cannot_be_retained(self):
        source = without_comments(
            psp_media_session_sources())
        interrupt = source[
            source.index("void psp_media_interrupt_decode("):
            source.index("bool psp_media_seek_decode_pump(")]
        self.assertIn(
            "media->clock_us = psp_media_recovery_position_us(media)",
            interrupt)
        self.assertIn("media->recovery_service_active = true", interrupt)
        self.assertLess(
            interrupt.index("psp_media_recovery_position_us(media)"),
            interrupt.index("psp_media_cancel_decode(media)"))
        self.assertGreaterEqual(
            source.count("psp_media_interrupt_decode(media);"), 2,
            "Both seek and ordinary bounded decoder cancellation must mark "
            "the native pipeline incomplete.")
        close = source[
            source.index("void psp_media_close("):
            source.index("bool psp_media_reclaim_hidden_pipeline(")]
        self.assertIn("media->recovery_service_active", close)
        destroy = source[
            source.index("void psp_media_pipeline_destroy("):
            source.index("void psp_media_init(")]
        self.assertIn("media->recovery_service_active = false", destroy)

    def test_media_open_supervisor_keeps_the_native_player_surface(self):
        source = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        start = source.index(
            "if (media_open_before && !navigation_still_pending")
        scope = source[start:source.index("bool media_decode_scope", start)]
        self.assertIn("psp_work_cooperate_begin_media_open(", scope)
        self.assertNotIn("psp_navigation_cooperate_begin(", scope)
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        begin = runtime[
            runtime.index("void psp_work_cooperate_begin_media_open("):
            runtime.index("bool psp_navigation_cancel_requested(")]
        self.assertIn("psp_work_cooperate_begin(", begin)
        self.assertIn('"media-open", NULL, media_ui', begin)

    def test_hidden_media_does_not_run_background_decode_or_refresh(self):
        source = without_comments(
            psp_media_session_sources())
        pending = source[
            source.index("bool psp_media_decode_work_pending("):
            source.index("void psp_media_prepare_route(")]
        self.assertIn("!media->ui.visible", pending)
        advance = source[source.index("bool psp_media_advance("):]
        self.assertIn("if (!media->ui.visible) return changed;", advance)
        self.assertLess(
            advance.index("if (!media->ui.visible) return changed;"),
            advance.index("psp_media_open_work_pending(media)"),
            "A hidden retained player must stop before open, seek, expiry, "
            "or decoder work is considered.")

    def test_leaving_watch_route_retires_pending_reopen(self):
        source = without_comments(
            psp_media_session_sources())
        route = source[
            source.index("static void psp_media_prepare_route_kind("):
            source.index("if (media->suspended_for_internal_view", source.index(
                "static void psp_media_prepare_route_kind("))]
        clear = route.index("media->open_service_pending = false;")
        close = route.index('"leave-video-route"')
        destroy = route.index("psp_media_pipeline_destroy(media);")
        self.assertLess(clear, close)
        self.assertLess(clear, destroy)

    def test_unsafe_audio_teardown_quarantines_future_backends(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        destroy = psp_firmware_backend_destroy(source)
        # The latch and the Media Engine pool's poison are one call, so no
        # teardown can refuse future backends while leaving the pool reusable.
        self.assertGreaterEqual(destroy.count("psp_media_quarantine();"), 2)
        create = psp_firmware_backend_create(source)
        self.assertIn("if (psp_media_backend_is_quarantined)", create)
        self.assertLess(
            create.index("if (psp_media_backend_is_quarantined)"),
            create.index("budget_calloc_category("),
            "A quarantined firmware worker must refuse a retry before any "
            "new backend memory is committed.")

    def test_every_open_phase_is_deadlined_and_answers_a_stop_request(self):
        """A device open sat past `tilefinch-media-modules: stage=ready` for
        more than ten minutes, printed nothing, honoured no deadline and
        ignored CIRCLE. Module preparation was the only deadlined phase; the
        rest inherited the bound of whatever they called into, and a
        fifteen-second per-window read bound composes to nothing across the
        many reads an MP4 open performs. The bounds themselves are host-tested
        in tests/test_media_mp4.c and tests/test_media_http_range.c; this pins
        the wiring, which has no host build."""
        source = without_comments(
            psp_media_session_sources())
        # The pump's *definition*: its name also appears as a forward
        # declaration well above the helpers this pins.
        pump = source[
            source.index(
                "static bool psp_media_open_pump_step(PspMediaSession *media)"
                "\n{"):
            source.index("bool psp_media_open_work_pending(")]
        # Before the unit, so a phase that is already over is not given one
        # more blocking read to spend.
        self.assertIn("psp_media_open_retire_if_over(media)", pump)
        self.assertLess(
            pump.index("psp_media_open_retire_if_over(media)"),
            pump.index("switch (media->job_phase)"))
        # Whatever the unit calls into inherits the transaction's remaining
        # time rather than re-arming a per-window timeout of its own.
        self.assertIn("psp_media_open_arm_wait_budget(media)", pump)
        self.assertLess(
            pump.index("psp_media_open_arm_wait_budget(media)"),
            pump.index("switch (media->job_phase)"))
        self.assertIn("psp_media_open_clear_wait_budget(media)", pump)
        # A stuck open must be visible while it is still stuck.
        self.assertIn("psp_media_open_report(media, \"progress\")", pump)
        retire = source[
            source.index("static bool psp_media_open_retire_if_over("):
            source.index("bool psp_media_open_watchdog(")]
        self.assertIn("psp_media_open_watch(", retire)
        self.assertIn("psp_media_cancel_requested(media)", retire)
        self.assertIn("psp_media_open_stage_name(", retire)
        self.assertIn("psp_media_job_failed(media,", retire)
        arm = source[
            source.index("void psp_media_open_arm_wait_budget("):
            source.index("void psp_media_open_clear_wait_budget(")]
        self.assertIn("psp_media_open_wait_budget_us(", arm)
        self.assertIn("media_http_range_set_wait_budget_us(media->range", arm)
        self.assertIn(
            "media_http_range_set_wait_budget_us(media->audio_range", arm)
        # The frames on which nothing pumps the open still run its deadline
        # and its cancel path.
        loop = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        suppressed = loop[
            loop.index("if ((!media_open_before || !navigation_still_pending)"):]
        suppressed = suppressed[:suppressed.index("bool stability_needs_play")]
        self.assertIn("psp_media_open_watchdog(&browser->media)", suppressed)
        self.assertIn("else if", suppressed)

    def test_media_module_prepare_has_a_terminal_deadline(self):
        source = without_comments(
            psp_media_session_sources())
        # Anchored inside the pump: the phase enumerators also appear in the
        # stage-name table the open's reports are built from.
        pump = source.index(
            "static bool psp_media_open_pump_step(PspMediaSession *media)\n{")
        start = source.index(
            "case PSP_MEDIA_JOB_OPEN_DECODER_PREPARE:", pump)
        phase = source[start:source.index(
            "case PSP_MEDIA_JOB_OPEN_PLAYBACK:", start)]
        self.assertIn("PSP_MEDIA_MODULE_PREPARE_TIMEOUT_US", phase)
        self.assertIn("psp_media_deadline_reached(", phase)

    def test_media_open_presents_one_stable_progress_label(self):
        """Opening telemetry remains stage-specific, but the user should not
        see the range/demux/codec implementation vocabulary flash through the
        player panel as those stages retire."""
        opening = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        pump = opening[
            opening.index(
                "static bool psp_media_open_pump_step(PspMediaSession *media)"
                "\n{"):
            opening.index("bool psp_media_open_work_pending(")]
        for internal_label in (
            "Finding video", "Opening video", "Reading video",
            "Opening audio", "Reading audio", "Starting decoder",
            "Preparing playback", "DECODING FIRST FRAME",
            "Retrying at 240p"):
            self.assertNotIn(internal_label, pump)
        self.assertNotIn("Retrying at 240p", opening)
        self.assertGreaterEqual(pump.count('"Loading..."'), 2)

        session = without_comments(
            (ROOT / "src/psp_media_session.c").read_text(encoding="utf-8"))
        first_frame = session[
            session.index("if (first_frame_pending) {"):
            session.index("psp_ui_media_set_buffering(",
                          session.index("if (first_frame_pending) {"))]
        self.assertIn('psp_ui_media_set_resolving_progress(\n'
                      '            &media->ui, "Loading...",', first_frame)
        self.assertNotIn("DECODING FIRST FRAME", first_frame)
        self.assertNotIn("BUFFERING VIDEO", first_frame)

    def test_media_uses_measured_device_elapsed_time(self):
        # Video is presented against the wall clock, so the media session must
        # be handed the frame time that actually elapsed. This test used to
        # pin the name `ui_elapsed_ms`, which was the measured value when it
        # was written; the scripted-input harness later pinned that same
        # variable to a nominal 16 ms so a slow host could not turn a tap into
        # a long press, and the media clock silently began advancing at half
        # the speed of a ~32 ms device frame. Everything downstream followed --
        # the decode horizon admitted half the content per second and audio
        # ran at 23 of its 43 blocks a second. Assert the property, not a name.
        source = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        call = source[source.index("psp_media_advance("):
                      source.index("psp_media_advance(") + 240]
        self.assertIn("&browser->media, media_elapsed_ms,", call)
        self.assertNotIn("config.tick_ms", call)
        self.assertNotIn("&browser->media, ui_elapsed_ms,", source)
        # It is the measured value, taken before any harness override...
        definition = source.index("unsigned media_elapsed_ms = ui_elapsed_ms;")
        measured = source.index("uint64_t ui_elapsed_us =")
        self.assertLess(measured, definition)
        override = source.find("ui_elapsed_ms = 16u;")
        self.assertNotEqual(override, -1)
        self.assertLess(
            definition, override,
            "media_elapsed_ms must be captured before the scripted harness "
            "pins ui_elapsed_ms to a nominal frame time")
        # ...and nothing may reassign it afterwards.
        self.assertEqual(
            len(re.findall(r"\bmedia_elapsed_ms\s*=", source)), 1,
            "the media clock's elapsed time is assigned once, from the "
            "measured frame delta")

    def test_page_gamepad_consumes_the_effective_input_source(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        start = main.index("static TILEFINCH_OUT_OF_LINE bool "
                           "psp_update_page_gamepad(")
        gamepad = main[start:main.index(
            "static TILEFINCH_OUT_OF_LINE bool psp_update_page_fullscreen(",
            start + 80)]
        self.assertIn("PspUiInput *input", gamepad)
        self.assertIn("psp_gamepad_standard_ui_buttons(", gamepad)
        self.assertIn("input->held", gamepad)
        self.assertIn("input->analog_x", gamepad)
        self.assertIn("input->analog_y", gamepad)
        self.assertNotIn("pad->Buttons", gamepad)
        self.assertNotIn("pad->Lx", gamepad)
        self.assertNotIn("pad->Ly", gamepad)
        self.assertNotIn("ui->page_fullscreen && chord", gamepad)
        self.assertNotIn("browser_engine_exit_page_fullscreen", gamepad)

        call = main[main.index("bool gamepad_visual_changed ="):
                    main.index("bool gamepad_visual_changed =") + 300]
        self.assertIn("psp_update_page_gamepad(", call)
        self.assertIn("&input", call)
        self.assertNotIn("&pad", call)

        fullscreen_start = main.index(
            "static TILEFINCH_OUT_OF_LINE bool psp_update_page_fullscreen(",
            start + 80)
        fullscreen = main[fullscreen_start:main.index(
            "static TILEFINCH_OUT_OF_LINE void psp_suspend_page_runtime(",
            fullscreen_start)]
        self.assertIn("PSP_UI_BUTTON_TOOLBAR", fullscreen)
        self.assertIn("browser_engine_exit_page_fullscreen", fullscreen)

    def test_page_controls_keep_triangle_in_the_game(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        start = main.index("static TILEFINCH_OUT_OF_LINE bool "
                           "psp_update_page_fullscreen(",
                           main.index("static TILEFINCH_OUT_OF_LINE bool "
                                      "psp_update_page_fullscreen(") + 1)
        end = main.index("static TILEFINCH_OUT_OF_LINE void "
                         "psp_suspend_page_runtime", start)
        fullscreen = main[start:end]
        self.assertIn("!app->interactive->gamepad_capture.active", fullscreen)
        self.assertIn("PSP_UI_BUTTON_TOOLBAR", fullscreen)

    def test_voice_reclaims_only_a_hidden_media_pipeline(self):
        session = without_comments(
            psp_media_session_sources())
        start = session.index("bool psp_media_reclaim_hidden_pipeline(")
        reclaim = session[start:session.index(
            "void psp_media_execute_intent(", start)]
        self.assertIn("media->ui.visible", reclaim)
        self.assertIn("media->playback == NULL", reclaim)
        self.assertIn("psp_media_pipeline_destroy(media);", reclaim)
        voice = without_comments(
            (ROOT / "src/psp_app/psp_app_input.c").read_text(
                encoding="utf-8"))
        self.assertIn(
            "psp_media_reclaim_hidden_pipeline(context->media)", voice)

    def test_navigation_reclaims_hidden_media_except_same_video(self):
        session = without_comments(
            psp_media_session_sources())
        start = session.index(
            "bool psp_media_reclaim_hidden_pipeline_for_navigation(")
        reclaim = session[start:session.index(
            "void psp_media_execute_intent(", start)]
        self.assertIn("media->ui.visible", reclaim)
        self.assertIn("media->playback == NULL", reclaim)
        self.assertIn("strcmp(media->source, target_url) == 0", reclaim)
        self.assertIn("youtube_watch_url_video_id(media->source", reclaim)
        self.assertIn("youtube_watch_url_video_id(target_url", reclaim)
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        pump = main[
            main.index(
                "if (browser_engine_navigation_pending(browser->engine)) {"):
            main.index("browser_engine_pump_navigation(")]
        self.assertIn(
            "psp_media_reclaim_hidden_pipeline_for_navigation(", pump)
        self.assertIn(
            "browser_engine_pending_navigation_url(browser->engine)", pump)

    def test_background_downloads_do_not_starve_behind_hidden_media(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        download = main[
            main.index("bool offline_download_active ="):
            main.index("bool runtime_layout_changed = false;")]
        self.assertLess(
            download.index(
                "psp_media_reclaim_hidden_pipeline(&browser->media)"),
            download.index("browser->media.playback == NULL"))
        updater = main[
            main.index("if (update_check_pending"):
            main.index("PspUiIntent intent =")]
        self.assertIn(
            "psp_media_reclaim_hidden_pipeline(&browser->media)", updater)

    def test_offline_startup_does_not_wake_network_background_work(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        update = main[
            main.index("bool background_network_allowed ="):
            main.index("bool update_check_running = false;")]
        self.assertIn("!offline_startup", update)
        self.assertIn("background_network_allowed", update)
        warmup = main[
            main.index("tilefinch-network-warmup: status=started") - 1000:
            main.index("tilefinch-network-warmup: status=started")]
        self.assertIn("background_network_allowed", warmup)
        dispatch = main[main.rindex("psp_app_run_interactive("):]
        self.assertIn("psp_offline_url(startup_url)", dispatch)
        initial_network = main[
            main.index("psp_log_checkpoint(\"network-begin\")") - 700:
            main.index("psp_log_checkpoint(\"network-begin\")")]
        self.assertIn("!psp_offline_url(startup_url)", initial_network)
        initial_load = main[
            main.index("if (native_home_boot) {"):
            main.index("psp_validation_cancel_after_ms = 0;")]
        self.assertIn("psp_offline_store_handle_url(", initial_load)

    def test_quarantined_media_retry_stops_before_resolution(self):
        source = without_comments(
            psp_media_session_sources())
        start = source.index("bool psp_media_open_pump(")
        pump = source[start:source.index(
            "bool psp_media_open_work_pending(", start)]
        self.assertLess(
            pump.index("media_psp_backend_quarantined()"),
            pump.index(
                "youtube_resolve_progressive_mp4_cancelable_with_preferences("))

    def test_media_resume_writes_use_debounced_profile_store(self):
        session = without_comments(
            psp_media_session_sources())
        record = session[session.index("bool psp_media_record_resume("):
                         session.index("void psp_media_suspend(")]
        self.assertIn("psp_media_finish_resume_update(", record)
        pending = session[
            session.index("static bool psp_media_finish_resume_update("):
            session.index("void psp_media_pipeline_destroy(")]
        self.assertIn("media->resume_profile_dirty = dirty", pending)
        self.assertIn("if (dirty) psp_media_persist_profile_change(media)", pending)
        self.assertIn("media->resume_profile_dirty = false", pending)
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        self.assertIn(
            ".profile_changed = psp_media_platform_profile_changed", main)
        callback = main[
            main.index("static void psp_media_platform_profile_changed("):
            main.index("bool psp_request_omnibox(")]
        self.assertIn("psp_profile_store_mark_dirty(context, now_us)", callback)

    def test_fresh_media_open_does_not_apply_durable_resume(self):
        source = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        self.assertNotIn("browser_profile_resume(", source)
        definition = source.index(
            "static bool psp_media_open_pump_step(",
            source.index("static bool psp_media_open_pump_step(") + 1)
        opening = source[
            definition:
            source.index("bool psp_media_open_work_pending(", definition)]
        self.assertIn(
            "bool reopen_resume_available = media->reopen_resume_pending",
            opening)
        self.assertIn(
            "media->job_target_us = media->reopen_resume_us > duration_us",
            opening)
        self.assertNotIn("BrowserProfileResume", opening)

    def test_media_retry_preserves_seek_target_and_short_resume(self):
        source = without_comments(
            psp_media_session_sources())
        recovery = source[
            source.index("uint64_t psp_media_recovery_position_us("):
            source.index("bool psp_media_retry_transport(")]
        self.assertIn("media->job_target_us", recovery)
        self.assertIn("media->job_restore_us", recovery)
        self.assertIn("media->job_preview", recovery)
        self.assertIn("media->seek_preview_started", recovery)
        retry = source[
            source.index("bool psp_media_retry_transport("):
            source.index("bool psp_media_retry_transport_expiry(")]
        self.assertIn("psp_media_recovery_position_us(media)", retry)
        fallback = source[
            source.index("bool psp_media_retry_240p("):
            source.index("bool psp_media_open_pump(")]
        self.assertIn("psp_media_recovery_position_us(media)", fallback)
        open_pump = source[
            source.index("bool psp_media_open_pump("):
            source.index("bool psp_media_open_work_pending(")]
        self.assertIn("media->reopen_resume_pending", open_pump)
        self.assertNotIn(
            "media->reopen_resume_us >= UINT64_C(5000000)", open_pump)
        suspend = source[
            source.index("void psp_media_suspend("):
            source.index("void psp_media_resume(")]
        self.assertIn("psp_media_recovery_position_us(media)", suspend)
        self.assertIn("media->seek_preview_was_playing", suspend)
        close = source[
            source.index("void psp_media_close("):
            source.index("bool psp_media_reclaim_hidden_pipeline(")]
        self.assertLess(
            close.index("psp_media_recovery_position_us(media)"),
            close.index("psp_media_cancel_decode(media)"))

    def test_media_prime_rejection_refreshes_then_uses_bounded_240p(self):
        open_source = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        retry = open_source[
            open_source.index("bool psp_media_retry_transport("):
            open_source.index("bool psp_media_retry_transport_expiry(")]
        self.assertIn("delivery_candidate_rejected", retry)
        self.assertIn(
            "(!delivery_candidate_rejected && !refresh_needed)", retry)
        self.assertIn('"pinned"', retry)
        self.assertIn("quality-policy=%s", retry)

        fallback = open_source[
            open_source.index("bool psp_media_retry_240p("):
            open_source.index("static bool psp_media_open_pump_step(")]
        self.assertIn("if (delivery_candidate_rejected)", fallback)
        self.assertIn("media->transport_reresolve_attempts++", fallback)
        self.assertIn(
            "PSP_MEDIA_TRANSPORT_REFRESH_MAXIMUM_ATTEMPTS", fallback)

        failure = open_source[
            open_source.index("bool delivery_candidate_rejected ="):
            open_source.index("return true;", open_source.index(
                "bool delivery_candidate_rejected ="))]
        self.assertIn(
            "phase_before == PSP_MEDIA_JOB_OPEN_VIDEO_PRIME", failure)
        self.assertIn("video_stats.failures != 0", failure)
        self.assertIn("psp_media_retry_delivery_failure(", failure)
        policy = open_source[
            open_source.index("bool psp_media_retry_delivery_failure("):
            open_source.index("static bool psp_media_open_pump_step(")]
        self.assertIn(
            "media->transport_reresolve_attempts != 0", policy)
        self.assertLess(
            policy.index("psp_media_retry_240p("),
            policy.index("psp_media_retry_transport("))
        self.assertIn(
            'psp_media_report_failure_snapshot( media, "media-open"',
            " ".join(failure.split()))

    def test_terminal_media_failure_writes_one_bounded_snapshot(self):
        session = without_comments(
            (ROOT / "src/psp_media_session.c").read_text(encoding="utf-8"))
        report = session[
            session.index("void psp_media_report_failure_snapshot("):
            session.index("void psp_media_raise_error(")]
        self.assertIn("failure_report_writes", report)
        self.assertIn("PSP_MEDIA_FAILURE_REPORT_MAXIMUM_WRITES", report)
        self.assertIn("video.last_read_offset", report)
        self.assertIn("audio.last_read_offset", report)
        self.assertIn("audio.bytes_in_flight", report)
        self.assertIn("char detail[1024]", report)
        self.assertIn("media->transport_reresolve_attempts", report)
        raised = session[
            session.index("void psp_media_raise_error("):
            session.index("void psp_media_retire_first_frame(")]
        self.assertIn("psp_media_report_failure_snapshot(", raised)
        buffering = without_comments(
            (ROOT / "src/psp_media_buffering.c").read_text(encoding="utf-8"))
        self.assertIn('"media-buffering"', buffering)
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        self.assertIn(
            ".write_failure_report = psp_media_platform_write_failure_report",
            main)
        self.assertIn('detail=%.1023s', main)

    def test_manual_media_retry_remembers_failure_position(self):
        source = without_comments(
            psp_media_session_sources())
        helper = source[
            source.index("void psp_media_remember_retry_state("):
            source.index("void psp_media_job_failed(")]
        self.assertIn("psp_media_recovery_position_us(media)", helper)
        failed = source[
            source.index("void psp_media_job_failed("):
            source.index("static void psp_youtube_log_text(")]
        self.assertLess(
            failed.index("psp_media_remember_retry_state("),
            failed.index("media->job_phase = PSP_MEDIA_JOB_NONE"))
        self.assertIn(
            "media->seek_preview_cancel_pending = false", failed)
        facade = without_comments(
            (ROOT / "src/psp_media_session.c").read_text(encoding="utf-8"))
        route = facade[
            facade.index("static void psp_media_prepare_route_kind("):
            facade.index("void psp_media_close(")]
        self.assertIn("media->clock_us = 0;", route)

    def test_failed_seek_leaves_the_clock_where_the_source_is(self):
        """A seek moves the demuxer before it reaches a picture. When the
        second step fails, a clock left behind puts the eligibility horizon
        below everything the source can serve and the session freezes -- the
        device log's clock=2947000us against buffered=19783401us. The rule
        and the classifier are host-tested in tests/test_media_mp4.c; what
        cannot be built on a host is this file, so pin the wiring."""
        source = without_comments(
            psp_media_session_sources())
        failed = source[
            source.index("void psp_media_job_failed("):
            source.index("static void psp_youtube_log_text(")]
        # Both answers are phase derived, so both must be sampled before the
        # phase is cleared, and the clock must take the source's position
        # rather than the retry ladder's resume position.
        self.assertIn("psp_media_seek_failure_clock_us(", failed)
        self.assertLess(
            failed.index("psp_media_seek_failure_clock_us("),
            failed.index("media->job_phase = PSP_MEDIA_JOB_NONE"))
        self.assertLess(
            failed.index("media->clock_us = source_us"),
            failed.index("media->job_phase = PSP_MEDIA_JOB_NONE"))
        # Only the phases that actually repositioned the demuxer, and only
        # while a pipeline exists to be inconsistent with.
        self.assertIn("psp_media_job_moved_the_source(media)", failed)
        self.assertIn("media->playback != NULL", failed)
        moved = source[
            source.index("static bool psp_media_job_moved_the_source("):
            source.index("void psp_media_job_failed(")]
        for phase in (
                "PSP_MEDIA_JOB_SEEK_PREPARE",
                "PSP_MEDIA_JOB_SEEK_PRIME",
                "PSP_MEDIA_JOB_SEEK_DECODE",
                "PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE",
                "PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE"):
            self.assertIn(phase, moved)
        # The retry ladder keeps its own answer: a preview scrub stays
        # uncommitted even though the source moved to its target.
        self.assertIn("psp_media_remember_retry_state(", failed)
        # The stall line asks the shared classifier rather than re-deriving
        # an order in which the pending window came first.
        advance = source[source.index("bool psp_media_advance("):]
        self.assertIn("psp_media_stall_suspect_name(", advance)
        self.assertIn("psp_media_stall_suspect(", advance)
        self.assertNotIn('? "source-window"', advance)

    def test_seek_preview_cancel_restores_play_state_and_retires(self):
        source = without_comments(
            psp_media_session_sources())
        pump = source[
            source.index("bool psp_media_seek_decode_pump("):
            source.index("void psp_media_close(")]
        self.assertIn("media->seek_preview_cancel_pending", pump)
        self.assertIn("media->seek_preview_was_playing", pump)
        self.assertIn("psp_ui_media_cancel_seek_preview(&media->ui)", pump)
        intents = source[
            source.index("void psp_media_execute_intent("):
            source.index("bool psp_media_advance(")]
        self.assertIn("media->seek_preview_cancel_pending = true", intents)

    def test_provider_routes_require_an_explicit_play_activation(self):
        source = without_comments(psp_media_session_sources())
        route = source[
            source.index("static void psp_media_prepare_route_kind("):
            source.index("void psp_media_close(")]
        dispatch = route[:route.index('"same-video-open"')]
        dispatch = dispatch[dispatch.rfind("psp_media_dispatch("):]
        self.assertIn(".autoplay = true", dispatch)
        dispatch = route[:route.index('"route-open"')]
        dispatch = dispatch[dispatch.rfind("psp_media_dispatch("):]
        self.assertIn(".autoplay = autoplay", dispatch)
        facade = source[
            source.index("void psp_media_prepare_route("):
            source.index("bool psp_media_open_page_source(")]
        self.assertIn(
            "psp_media_prepare_route_kind( media, url, generation, PSP_MEDIA_ROUTE_DOCUMENT, offline_autoplay)",
            " ".join(facade.split()))
        self.assertIn(
            "media, url, backing_generation, PSP_MEDIA_ROUTE_PROVIDER_VIDEO, true",
            " ".join(facade.split()))
        self.assertNotIn(
            ".autoplay = media->reopen_resume_playing", route)
        open_source = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        self.assertNotIn("browser_profile_resume(", open_source)
        resume = open_source[
            open_source.index("bool reopen_resume_available"):
            open_source.index("media->reopen_resume_us = 0",
                              open_source.index("bool reopen_resume_available"))]
        self.assertIn("media->job_resume_playing = media->reopen_resume_playing",
                      resume)
        wants = source[
            source.index("bool psp_media_machine_wants_playing("):
            source.index("static void psp_media_apply_active_projection(")]
        self.assertIn("PSP_MEDIA_SESSION_OPENING", wants)
        first_frame_start = open_source.index("bool wants_playing =")
        first_frame = open_source[
            first_frame_start:
            open_source.index(
                "media_playback_set_playing(media->playback, wants_playing)",
                first_frame_start) + 64]
        self.assertIn("psp_media_machine_wants_playing(media)", first_frame)
        self.assertIn("if (!wants_playing)", first_frame)
        self.assertIn(
            "media_playback_set_playing(media->playback, wants_playing)",
            first_frame)
        pump = open_source[
            open_source.rindex("static bool psp_media_open_pump_step("):]
        create = pump[
            pump.index("case PSP_MEDIA_JOB_OPEN_PLAYBACK:"):
            pump.index("tilefinch-media-first-frame:")]
        self.assertIn("media->pause_boundary_pending =", create)
        self.assertIn("!psp_media_machine_wants_playing(media)", create)

    def test_repeated_preview_seek_coalesces_without_replacing_the_job(self):
        source = without_comments(psp_media_session_sources())
        intents = source[
            source.index("void psp_media_execute_intent("):
            source.index("bool psp_media_advance(")]
        preview_case = intents[
            intents.index("case PSP_UI_MEDIA_ACTION_PREVIEW_SEEK:"):
            intents.index("case PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW:")]
        self.assertIn("psp_media_seek_phase(media->job_phase)", preview_case)
        self.assertIn("media->ui.seek_preview_time_us = intent.seek_time_us",
                      preview_case)
        self.assertEqual(1, preview_case.count("psp_media_request_seek("))

        advance = source[source.index("bool psp_media_advance("):]
        coalesce = advance[
            advance.index("media->ui.seek_preview_time_us"):
            advance.index('"seek-complete"')]
        self.assertIn("media->job_target_us", coalesce)
        self.assertIn("psp_media_request_seek(", coalesce)

        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        tick = runtime[
            runtime.index("void psp_background_ui_tick(void)"):
            runtime.index("bool psp_platform_cooperate(")]
        self.assertIn("psp_ui_media_update(", tick)
        self.assertIn("cooperate->pending_media_intent", tick)
        self.assertIn("cooperate->pending_page_input", tick)
        self.assertIn("psp_controller_take_latched_pressed()", tick)
        self.assertIn("PSP_SUPERVISOR_PAGE_INPUT_LIMIT", tick)
        self.assertLess(tick.index("cooperate->pending_page_input"),
                        tick.index('"BUSY - CIRCLE CANCELS"'))
        end = runtime[
            runtime.index("void psp_navigation_cooperate_end("):
            runtime.index("bool psp_navigation_cooperate_take_media_intent(")]
        self.assertLess(
            end.index("while (psp_navigation_cooperate.presenting != 0)"),
            end.index("psp_completed_supervisor_media_intent ="))
        self.assertIn("psp_completed_supervisor_page_input", end)

        open_source = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        fallback = open_source[
            open_source.index("bool psp_media_retry_240p("):
            open_source.index("static bool psp_media_open_pump_step(")]
        self.assertLess(
            fallback.index("preview_target_us ="),
            fallback.index("psp_media_pipeline_destroy(media)"))
        self.assertIn("media->reopen_preview_pending = preview_pending",
                      fallback)
        self.assertIn("media->reopen_preview_target_us = preview_target_us",
                      fallback)
        self.assertIn("psp_media_machine_wants_playing(media)", fallback)
        self.assertIn("media->machine.preview_active", fallback)
        self.assertIn("media->job_preview", fallback)
        self.assertIn("media->ui.duration_us = duration_us", fallback)
        transport_retry = open_source[
            open_source.index("bool psp_media_retry_transport("):
            open_source.index("bool psp_media_retry_240p(")]
        self.assertIn("media->ui.duration_us = duration_us", transport_retry)
        self.assertIn("media->preview_commit_pending", open_source)
        self.assertIn("media->preview_commit_target_us", open_source)
        advance = source[source.index("bool psp_media_advance("):]
        self.assertIn("media->reopen_preview_pending", advance)
        self.assertIn("psp_ui_media_set_seek_preview(&media->ui, target_us)",
                      advance)
        self.assertIn("psp_media_request_seek(media, target_us, true)",
                      advance)
        self.assertIn("psp_media_start_pending_preview_commit(media)",
                      advance)
        open_advance = advance[
            advance.index("bool open_work ="):
            advance.index("if (psp_media_start_pending_preview_commit(media))")]
        self.assertIn("media->reopen_preview_pending", open_advance)
        self.assertIn("media->preview_commit_pending", open_advance)
        self.assertIn("psp_ui_media_commit_seek(", open_advance)
        self.assertIn("media->preview_commit_target_us", open_advance)
        self.assertIn("psp_ui_media_set_seek_preview(", open_advance)
        self.assertIn("media->reopen_preview_target_us", open_advance)

        commit_case = intents[
            intents.index("case PSP_UI_MEDIA_ACTION_SEEK:"):
            intents.index("case PSP_UI_MEDIA_ACTION_RETRY:")]
        self.assertIn("media->preview_commit_pending = true", commit_case)
        self.assertIn("media->reopen_preview_pending = false", commit_case)
        self.assertIn("psp_ui_media_commit_seek(", commit_case)
        commit_start = advance.index(
            "psp_media_start_pending_preview_commit(media)")
        self.assertLess(
            commit_start,
            advance.index("media->reopen_preview_pending", commit_start))

        open_source = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        failure = open_source[
            open_source.index("void psp_media_job_failed("):
            open_source.index("static void psp_youtube_log_text(")]
        self.assertIn("bool tentative_preview =", failure)
        self.assertIn("tentative_preview && !media->preview_commit_pending",
                      failure)
        self.assertIn("action=restore", failure)
        self.assertIn("tentative_preview && media->preview_commit_pending",
                      failure)
        self.assertIn("media->job_target_us = preview_target_us", failure)
        self.assertIn("action=retain-target", failure)
        self.assertLess(
            failure.index("action=retain-target"),
            failure.index("psp_media_raise_error(media, error, NULL)"))

    def test_seek_keeps_the_last_complete_scanout_latched(self):
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        hold = runtime[
            runtime.index("static bool psp_media_seek_holds_scanout("):
            runtime.index("bool psp_present_internal(")]
        for phase in (
                "PSP_MEDIA_JOB_SEEK_PREPARE",
                "PSP_MEDIA_JOB_SEEK_PRIME",
                "PSP_MEDIA_JOB_SEEK_DECODE",
                "PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE",
                "PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE"):
            self.assertIn(phase, hold)
        self.assertIn("media->job_resume_open", hold)
        present = runtime[
            runtime.index("bool psp_present_internal("):
            runtime.index("static unsigned psp_local_hour(")]
        self.assertLess(
            present.index("psp_media_seek_holds_scanout("),
            present.index("psp_media_video_surface_follow("))
        self.assertLess(
            present.index("psp_media_seek_holds_scanout("),
            present.index("psp_display_back_buffer("))

    def test_restarting_ended_media_rearms_resume_tracking(self):
        source = without_comments(
            psp_media_session_sources())
        seek = source[
            source.index("bool psp_media_request_seek_with_resume("):
            source.index("static bool psp_media_copy_preview(")]
        self.assertIn("!preview", seek)
        self.assertIn("media->last_resume_saved_us == UINT64_MAX", seek)
        self.assertIn("media->last_resume_saved_us = 0", seek)

    def test_the_video_surface_is_left_by_every_path_that_draws_16_bit(self):
        """Fullscreen video scans out 8888 so the decoder's bytes reach the
        panel unconverted. Every other composer in the process writes 16-bit
        rows, and one of them running while the panel is still latched on the
        video surface is the whole screen turned to noise -- so each asserts
        the surface it is about to write instead of assuming the last present
        left it there."""
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        for function, next_function in (
                ("static void psp_present_supervisor_media(",
                 "void psp_present_boot_surface("),
                ("void psp_present_boot_surface(",
                 "PspNavigationCooperate psp_navigation_cooperate;"),
                ("void psp_present_boot_entrance(", None)):
            body = runtime[runtime.index(function):]
            if next_function is not None:
                body = body[:body.index(next_function)]
            self.assertIn("psp_display_video_end(&psp_display)", body)
            self.assertLess(
                body.index("psp_display_video_end(&psp_display)"),
                body.index("psp_display_back_buffer(&psp_display)"))
        supervisor = runtime[
            runtime.index("static void psp_present_supervisor_media("):
            runtime.index("void psp_present_boot_surface(")]
        self.assertLess(
            supervisor.index("psp_display_video_active(&psp_display)"),
            supervisor.index("psp_display_video_end(&psp_display)"))
        self.assertIn("psp_display_video_front_buffer(&psp_display)",
                      supervisor)
        self.assertIn("psp_display_video_back_buffer(&psp_display)",
                      supervisor)
        self.assertIn("memcpy(back, front, PSP_DISPLAY_VIDEO_BUFFER_BYTES)",
                      supervisor)
        self.assertIn("psp_ui_media_composite_controls_8888(", supervisor)
        # The ownership window is derived per present, never latched at open,
        # so every way a session ends is an exit path without enumerating
        # them: close, the failed panel, a quarantine, a navigation and a
        # suspend all stop producing decoded frames.
        follow = runtime[
            runtime.index("static bool psp_media_video_surface_follow("):
            runtime.index("static bool psp_present_video_surface(")]
        self.assertIn("PSP_MEDIA_PRESENT_MODE_GE_SMOOTH", follow)
        self.assertIn("psp_media_present_ge_reason() == NULL", follow)
        self.assertIn("psp_display_video_end(&psp_display)", follow)
        # And the passthrough is proven before anything is published, into a
        # buffer the panel has never shown.
        self.assertIn(
            "psp_media_present_ge_passthrough_check(", follow)
        self.assertLess(
            follow.index("psp_display_video_begin(&psp_display)"),
            follow.index("psp_media_present_ge_passthrough_check("))
        present = runtime[
            runtime.index("bool psp_present_internal("):
            runtime.index("static unsigned psp_local_hour(")]
        self.assertLess(
            present.index("psp_media_video_surface_follow("),
            present.index("psp_display_back_buffer(&psp_display)"))
        # The two exits no present covers: handing the console to another
        # EBOOT, and coming back from suspend.
        loop = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        exit_console = loop[
            loop.index("static TILEFINCH_COLD_PATH void "
                       "psp_exit_console("):]
        exit_console = exit_console[:exit_console.index("sceKernelExitGame()")]
        self.assertIn("psp_display_video_end(&psp_display)", exit_console)
        rearm = loop[:loop.index("psp_display_rearm(&psp_display)")]
        self.assertIn("psp_display_video_end(&psp_display)", rearm)

    def test_the_engines_wait_is_spent_feeding_the_decoder(self):
        """The device measured one 8888 present at 46ms against the software
        scaler's 6-10ms, and almost all of it is the wait: the display list is
        microseconds. Blocking straight through it gave the decoder nothing
        for 6.45s of a thirty-second session, while the feed report said
        horizon=0 and unit-cap=54 -- ready to submit, out of frames to do it
        in. Submit, feed, then wait; and nothing may touch the destination
        rows in between."""
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        video = runtime[
            runtime.index("static bool psp_present_media_frame_video("):
            runtime.index("static bool psp_media_video_surface_follow(")]
        for call in ("psp_media_present_ge_submit(",
                     "psp_media_pump_while_drawing(",
                     "psp_media_present_ge_complete("):
            self.assertIn(call, video)
        self.assertLess(
            video.index("psp_media_present_ge_submit("),
            video.index("psp_media_pump_while_drawing("))
        self.assertLess(
            video.index("psp_media_pump_while_drawing("),
            video.index("psp_media_present_ge_complete("))
        # The rows belong to the engine until complete returns: the bands and
        # the accounting come after it, never between.
        self.assertLess(
            video.index("psp_media_present_ge_complete("),
            video.index("psp_media_fill_bands_video("))
        # The pump run inside a present must not advance a clock, take a
        # frame, or run while a transaction owns the pipeline.
        session = without_comments(
            (ROOT / "src/psp_media_present_session.c").read_text(
                encoding="utf-8"))
        pump = session[
            session.index("static size_t psp_media_pump_present("):]
        # The pump inside a present is bounded by what it waits on -- the
        # engine's draw, or the parallel stage copy -- not by a slice of its
        # own: a 40ms budget inside a 33ms frame ran 23 units and 15.1ms deep
        # while the engine needed 2.5ms. The blocking sync/join is still the
        # guarantee, so a misleading poll is safe.
        self.assertIn("still_busy()", pump)
        self.assertIn("psp_media_present_ge_drawing", pump)
        # What each of these pumps may afford, which the backend cannot see
        # for itself. Blocking for the codec worker is free on both -- the
        # thread is waiting on hardware either way, and a soak measured that
        # same wait costing the frame's own advance 8.8ms of every 46.4ms
        # frame -- so both declare where they are and both put it back.
        # The copy is reading the decoded surface, so the pump that overlaps
        # it may not admit a video access unit: accepting one emits a picture,
        # and emitting one is a colour conversion into those exact bytes. The
        # engine reads its own staged copy and is free of that -- unless
        # staging declined, when the engine is reading the surface too.
        self.assertIn("media_psp_backend_set_advance_mode(mode)", pump)
        self.assertIn(
            "media_psp_backend_set_advance_mode(PSP_MEDIA_ADVANCE_FRAME)",
            pump)
        self.assertIn("PSP_MEDIA_ADVANCE_DRAW", pump)
        self.assertIn("media->present_texture_staged", pump)
        finish = session[
            session.index("void psp_media_present_texture_finish("):
            session.index("size_t psp_media_feed_before_blocking(")]
        self.assertIn("PSP_MEDIA_ADVANCE_STAGE_COPY", finish)
        self.assertNotIn("PSP_MEDIA_ADVANCE_DRAW", finish)
        # And the copy is not presented on trust: a join that failed is
        # repeated on this thread, and a picture that cannot be re-staged at
        # all is not left claimed as staged forever.
        self.assertIn("psp_media_present_ge_stage_dma_join();", finish)
        self.assertIn("if (!joined) {", finish)
        # ...and names WHICH picture it means. A join only fails after it has
        # waited, and with two decoded-output slots the surface it copied from
        # can hold a different picture by then; repeating the copy blind would
        # stage that successor under this identity, which nothing downstream
        # could detect.
        self.assertIn(
            "psp_media_present_ge_stage_dma_recover(\n"
            + " " * 16 + "media->frame.slot, media->frame.generation)",
            finish)
        self.assertIn("media->present_stage_identity = 0", finish)
        self.assertIn("media->job_phase != PSP_MEDIA_JOB_NONE", pump)
        self.assertIn(
            "media->machine.state == PSP_MEDIA_SESSION_OPENING", pump)
        self.assertIn("media->pause_boundary_pending", pump)
        self.assertNotIn("media->clock_us =", pump)
        self.assertNotIn("media_playback_take_video_frame(", pump)
        # The wait must be reported apart from the pump that fills it. Once a
        # pump ran inside submit->complete, sync_us stopped measuring the
        # engine: a device cycle read 11.06ms of "sync" that was 8.3ms of
        # decoder feeding, and the two are opposite conclusions.
        presenter = without_comments(
            (ROOT / "src/psp_media_present_ge.c").read_text(encoding="utf-8"))
        # The definition, not the forward declaration above it.
        complete = presenter[
            presenter.index("bool psp_media_present_ge_complete("):
            presenter.index(
                "static bool psp_media_present_ge_draw_plan(\n"
                "    const PspMediaPresentPlan *plan, "
                "const PspMediaPresentTexture *texture,\n"
                "    uint32_t *destination, PspMediaPresentGeCost *cost)\n{")]
        self.assertIn("cost->wait_us", complete)
        self.assertIn("entered_us", complete)
        self.assertIn("ge-wait=", without_comments(
            psp_media_session_sources()))
        # And where the engine read its texture from, which is the largest
        # term in a frame, is reported rather than assumed.
        self.assertIn("psp_display_in_edram(&psp_display", runtime)
        self.assertIn("sampled=%s@%s", runtime)
        # And the video presents are back in the cadence accounting: they were
        # the most expensive frames in the run and the line could not see them.
        self.assertIn("psp_cadence_composed(", runtime)
        surface = runtime[
            runtime.index("static bool psp_present_video_surface("):
            runtime.index("bool psp_present_internal(")]
        self.assertIn("psp_cadence_composed(", surface)
        self.assertIn("psp_cadence_published(", surface)
        self.assertIn("psp_cadence_video_published(", surface)
        self.assertIn("tilefinch-video-scanout:", runtime)
        self.assertIn("video_scanout_identity == frame->identity", runtime)
    def test_the_engine_reads_a_staged_texture_staged_per_picture(self):
        """The device measured a linear 2 KiB-pitch texture at 47.4ms of wait
        per frame (ge-submit=1832us against ge-sync=1612292us over 34 frames)
        and the Media Engine sharing that memory slowed with it. The staged
        layout is the texture unit's designed path; the copy that produces it
        is only affordable because it runs when the picture changes, not when
        the panel is presented."""
        session = without_comments(
            psp_media_session_sources())
        stage = session[
            session.index("void psp_media_present_texture_for("):
            session.index("static size_t psp_media_pump_present(")]
        # The gate: one copy per decoded picture, never per present, and never
        # for the second buffer of a double-buffered pair.
        self.assertIn("media->present_stage_identity != frame->identity",
                      stage)
        self.assertIn("media->present_stage_identity = frame->identity",
                      stage)
        # Only when the plan admits it, and the engine must be able to see it.
        self.assertIn("psp_media_present_stage_fits(", stage)
        self.assertIn("psp_media_present_ge_stage_flush(", stage)
        self.assertLess(
            stage.index("psp_media_present_stage("),
            stage.index("psp_media_present_ge_stage_flush("))
        # An allocation that cannot be made leaves the linear layout, which is
        # slower and correct -- it must never leave a half-staged texture.
        self.assertIn("texture->staged = false", stage)
        # Budget owned, outside the Media Engine pool: only the graphics
        # engine reads it, so it needs none of the pool's physical visibility.
        # The staging is the display's EDRAM, not an allocation: the device
        # measured the engine reading a main-memory texture at 10,312us
        # against 456us to write the frame, so where it lives is the frame.
        self.assertNotIn("budget_malloc_category(", stage)
        self.assertNotIn("psp_media_pool_alloc(", stage)
        # The half-megabyte copy is posted to the DMA worker for the shipping
        # stride-equals-width case so the controller runs it in parallel with
        # the present's feed, off the interactive thread. A device cycle
        # measured even the synchronous DMA still on the critical path at 1.67ms
        # -- enough to push the 17.8ms frame past the 16.67ms vblank. The
        # synchronous DMA and then the CPU stage remain the strided/host
        # fallback, and only the CPU stage needs the writeback.
        self.assertIn("psp_media_present_ge_stage_dma_submit(", stage)
        self.assertIn("media->present_stage_async = true", stage)
        self.assertIn("frame->stride_pixels == texture_width", stage)
        self.assertLess(
            stage.index("psp_media_present_ge_stage_dma_submit("),
            stage.index("psp_media_present_stage("))
        self.assertIn("psp_media_present_ge_stage_flush(staging", stage)
        # The present-time feed takes no frame (NULL out), so the picture the
        # parallel stage copy is reading is never overwritten under it.
        pump = session[
            session.index("static size_t psp_media_pump_present("):
            session.index("size_t psp_media_pump_while_drawing(")]
        self.assertIn("PSP_MEDIA_JOB_MAXIMUM_PACKETS, NULL,", pump)
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        self.assertIn("psp_display_video_texture(&psp_display)", runtime)
        self.assertIn("PSP_DISPLAY_VIDEO_TEXTURE_BYTES", runtime)
        # The posted copy is collected before the list is started, so the
        # engine never samples an unfinished texture.
        present = runtime[
            runtime.index("psp_media_present_texture_for("):
            runtime.index("psp_media_present_ge_complete(")]
        self.assertLess(
            present.index("psp_media_present_texture_finish("),
            present.index("psp_media_present_ge_submit("))
        # And the probe measures the read playback performs, from the same
        # EDRAM, or its texture-sync number describes nothing that ships.
        loop = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        self.assertIn("psp_display_video_texture(&psp_display)", loop)
        # And the probe certifies the layout playback actually draws from.
        presenter = without_comments(
            (ROOT / "src/psp_media_present_ge.c").read_text(encoding="utf-8"))
        probe = presenter[
            presenter.index("static bool psp_media_present_probe_case("):
            presenter.index("bool psp_media_present_ge_probe(")]
        self.assertIn("psp_media_present_stage_fits(", probe)
        # The probe stages through the shipped async worker -- submit then join,
        # no feed between -- so it certifies that path's copy and cache
        # discipline, not a CPU substitute.
        self.assertIn("psp_media_present_probe_draw(", probe)
        probe_draw = presenter[
            presenter.index("static bool psp_media_present_probe_draw("):
            presenter.index("static bool psp_media_present_probe_case(")]
        self.assertIn("psp_media_present_probe_stage(", probe_draw)
        self.assertIn("report->staged = staged", probe)
        helper = presenter[
            presenter.index("static void psp_media_present_probe_stage("):
            presenter.index("static bool psp_media_present_probe_case(")]
        self.assertIn("psp_media_present_ge_stage_dma_submit(", helper)
        self.assertIn("psp_media_present_ge_stage_dma_join(", helper)

    def test_scrubbing_is_ui_only_and_commit_primes_both_sources(self):
        """Moving the nub must not churn the decoder or HTTP windows. A
        committed seek performs one reset, then cooperatively proves that the
        first video and audio payload windows are readable before returning to
        the existing presentation priming state."""
        source = without_comments(
            psp_media_session_sources())
        request = source[
            source.index("bool psp_media_request_seek_with_resume("):
            source.index("static bool psp_media_copy_preview(")]
        preview = request[
            request.index("if (preview) {"):
            request.index("if (!media->audio_only && psp_media_seek_reopens_backend(")]
        self.assertIn("PSP_MEDIA_EVENT_PREVIEW_STARTED", preview)
        self.assertIn("psp_ui_media_set_seek_preview", preview)
        self.assertIn("return true", preview)
        self.assertNotIn("media_playback_seek(", preview)
        self.assertNotIn("media_playback_warm_", preview)
        self.assertNotIn("PSP_MEDIA_JOB_SEEK_PREPARE", preview)

        pump = source[
            source.index("bool psp_media_seek_decode_pump("):
            source.index("void psp_media_close(")]
        skipped = pump[pump.index("bool decodes_to_the_target ="):]
        skipped = skipped[:skipped.index("media->job_phase = restore")]
        self.assertIn("media->job_phase = PSP_MEDIA_JOB_SEEK_PRIME", skipped)
        self.assertIn("media->clock_us = target_us", skipped)
        self.assertNotIn("PSP_MEDIA_JOB_SEEK_DECODE", skipped)
        prime = pump[
            pump.index("if (media->job_phase == PSP_MEDIA_JOB_SEEK_PRIME)"):
            pump.index("if (media->job_phase == PSP_MEDIA_JOB_SEEK_PREPARE")]
        self.assertIn("media_playback_prime_video_source(", prime)
        self.assertIn("media_playback_prime_audio_source(", prime)
        self.assertIn("PSP_MEDIA_SEEK_PRIME_ALL", prime)
        self.assertIn("psp_media_retry_delivery_failure(", prime)
        self.assertLess(
            prime.index("media->job_prime_ready_mask & required_ready"),
            prime.index("media_playback_set_playing(media->playback, playing)"))

    def test_backward_reopen_completion_is_telemetry_not_state(self):
        session_header = without_comments(
            (ROOT / "include/tilefinch/psp_media_session.h").read_text(
                encoding="utf-8"))
        state_header = without_comments(
            (ROOT / "include/tilefinch/psp_media_state.h").read_text(
                encoding="utf-8"))
        sources = without_comments(psp_media_session_sources())
        marker = "reopen_seek_completion_pending"
        self.assertIn(marker, session_header)
        self.assertNotIn(marker, state_header)
        self.assertIn("media->" + marker + " = true", sources)
        self.assertIn("media->" + marker + " = false", sources)

        # Recreating the firmware backend is mandatory; repeating the
        # multi-client YouTube resolve is not. A still-valid direct grant is
        # consumed by exactly the replacement open, while expiry/403 and the
        # ordinary retry ladders retain their authoritative re-resolution.
        self.assertIn("psp_media_resolved_stream_reusable(media)", sources)
        self.assertIn("event=reuse-resolved-stream", sources)
        self.assertIn("media->reopen_reuse_resolved_stream = false", sources)

        # A reused or offline resolve can complete in the same pump that
        # changes the physical phase from NONE to RESOLVE. The controller must
        # still receive that phase completion rather than lagging forever.
        open_pump = sources[
            sources.index("bool psp_media_open_pump("):
            sources.index("static const char *psp_media_open_stage_name(")]
        self.assertIn("if (starting) {", open_pump)
        self.assertIn("phase_before = PSP_MEDIA_JOB_OPEN_RESOLVE", open_pump)

    def test_long_gop_seek_holds_audio_until_the_presentation_floor(self):
        """A skipped exact-frame seek still decodes prerequisite video from
        its random-access point. Split audio must not start at the requested
        time while that private video preroll is still in progress."""
        source = without_comments(
            psp_media_session_sources())
        release = source[
            source.index("void psp_media_release_presentation_preroll("):
            source.index("uint64_t psp_media_session_decode_clock_us(")]
        self.assertIn("psp_media_apply_audio_hold(media)", release)
        self.assertIn("media_playback_set_audio_submission_blocked(", release)

        decode_clock = source[
            source.index("uint64_t psp_media_session_decode_clock_us("):
            source.index("bool psp_media_cancel_requested(")]
        self.assertIn("media->presentation_preroll_audio_held", decode_clock)
        self.assertIn("media_playback_buffered_until_us", decode_clock)

        seek = source[
            source.index("bool psp_media_seek_decode_pump("):
            source.index("void psp_media_close(")]
        skipped = seek[seek.index("bool decodes_to_the_target ="):]
        skipped = skipped[:skipped.index("media->job_phase = restore")]
        self.assertIn("media->presentation_floor_us = target_us", skipped)
        request = source[
            source.index("bool psp_media_request_seek_with_resume("):
            source.index("static bool psp_media_copy_preview(")]
        self.assertLess(
            request.index("PSP_MEDIA_EVENT_SEEK"),
            request.index("media->job_phase = PSP_MEDIA_JOB_SEEK_PREPARE"))

        advance = source[source.index("bool psp_media_advance("):]
        self.assertIn(
            "media->machine.state == PSP_MEDIA_SESSION_PLAYING\n"
            "        && !media->machine.preview_active\n"
            "        && !media->presentation_preroll_audio_held", advance)
        self.assertLess(
            advance.index("media_playback_discard_video_before("),
            advance.index("media_playback_take_video_frame("))
        floor = advance[advance.index("if (received_frame) {"):]
        self.assertIn("frame.pts_us >= media->presentation_floor_us", floor)
        self.assertIn(
            "psp_media_complete_priming_if_ready(", floor)

    def test_startup_holds_audio_until_first_primed_frame_is_displayed(self):
        """The first audio cursor must not outrun an empty video pipeline.
        Startup fills the complete two-slot decoded queue before sound can
        establish the presentation clock; this is separate from seek floors
        because its requested time is legitimately zero."""
        source = without_comments(
            psp_media_session_sources())
        begin = source[
            source.index("bool psp_media_begin_startup_preroll("):
            source.index("uint64_t psp_media_session_decode_clock_us(")]
        self.assertIn("media->clock_us != 0", begin)
        self.assertIn("media->controller_audio_hold = true", begin)
        self.assertIn("psp_media_apply_audio_hold(media)", begin)
        self.assertIn("media->presentation_preroll_startup = true", begin)

        advance = source[source.index("bool psp_media_advance("):]
        self.assertLess(
            advance.index("media_playback_discard_video_before("),
            advance.index("media_playback_set_presentation_clock_us("))
        self.assertIn("media_playback_ready_video_start_us", advance)
        prime = advance[
            advance.index("if (media->presentation_preroll_startup) {"):
            advance.index("if (received_frame) {")]
        self.assertIn("media_playback_ready_video_frames", prime)
        self.assertIn("media_playback_startup_ready_frames", prime)
        self.assertIn("ready >= ready_target", prime)
        self.assertIn("media_playback_displayed_video_frames", prime)
        self.assertIn("presentation_preroll_startup_claimed", prime)
        self.assertIn("startup_claim_allowed = true", prime)
        self.assertIn(
            "displayed\n"
            "                 > media->presentation_preroll_displayed_baseline",
            prime)
        self.assertIn("psp_media_complete_priming_if_ready(", prime)

        shadow_prime = source[
            source.index(
                "static void psp_media_complete_priming_if_ready("):
            source.index("#else\nvoid psp_media_session_checkpoint")]
        self.assertIn("!media->have_frame", shadow_prime)
        self.assertIn("media->pause_boundary_pending", shadow_prime)
        self.assertIn("presentation_preroll_displayed_baseline", shadow_prime)
        self.assertIn("PSP_MEDIA_EVENT_PRIME_READY", shadow_prime)
        self.assertLess(
            shadow_prime.index("PSP_MEDIA_EVENT_PRIME_READY"),
            shadow_prime.index(
                "psp_media_release_presentation_preroll(media, true)"))

    def test_audio_clock_does_not_count_firmware_queue_as_heard(self):
        """The PSP accepts a deep startup queue. Audible time follows elapsed
        wall time, capped by successful submissions, and freezes with the
        output worker across pause and buffering."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        cursor = source[
            source.index("static bool psp_media_audio_cursor_us("):
            source.index("static unsigned psp_media_ready_video_frames(")]
        self.assertIn("audio_first_output_us", cursor)
        self.assertIn("psp_media_stamp_us", cursor)
        self.assertIn("psp_media_audio_cursor_advance_us", cursor)
        self.assertNotIn("sceAudioGetChannelRestLen", cursor)

    def test_the_frames_own_advance_never_waits_for_the_codec_worker(self):
        """The bounded collect wait yields to a worker that runs one priority
        below the browser thread, which is what lets one frame carry more than
        one access unit. It is free where the thread was already waiting on
        hardware and it is frame time everywhere else: a device soak measured
        962 of them costing 2.65 seconds across 300 playing frames, 8.8ms of
        every 46.4ms frame, on the one path that overlaps nothing."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        advance = source[
            source.index("static bool psp_media_advance("):
            source.index("static bool psp_media_reset(")]
        self.assertIn("psp_media_wait_codec_job(", advance)
        self.assertIn(
            "psp_media_advance_may_wait(psp_media_advance_mode)", advance)
        # And a submission that could emit a picture is held while something
        # is reading the surface that picture is converted into. The frame
        # taken for a present is a taken frame, so the frame_ready/frame_taken
        # guard says nothing here -- this is the guard that does, and it comes
        # before the emission it exists to prevent.
        submit = source[
            source.index("static MediaBackendResult psp_media_submit("):
            source.index("static MediaBackendResult psp_media_drain(")]
        self.assertIn(
            "psp_media_advance_may_submit_video(psp_media_advance_mode)",
            submit)
        self.assertLess(
            submit.index("psp_media_advance_may_submit_video("),
            submit.index("psp_media_emit_batch_into_free_slots("))
        drain = source[
            source.index("static MediaBackendResult psp_media_drain("):
            source.index("static bool psp_media_advance(")]
        self.assertIn(
            "psp_media_advance_may_submit_video(psp_media_advance_mode)",
            drain)
        self.assertLess(
            drain.index("psp_media_advance_may_submit_video("),
            drain.index("psp_media_emit_batch_into_free_slots("))

    def test_a_firmware_call_that_never_returns_is_named(self):
        """Two soaks entered sceMpegGetAvcNalAu with the same access unit and
        never came out. The first bridge transaction is committed before
        firmware is entered; later jobs retain their current native stage and
        packet metadata in RAM so the browser-side watchdog can classify a
        hang without steady-state per-AU Memory Stick traffic. Everything
        after it is consequence: no completion, so no submission, so no byte
        was ever needed and the transport looked idle. The worker's liveness
        check reports a thread inside firmware as healthy, which is true and
        not the question."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        collect = source[
            source.index("static int psp_media_collect_codec_job("):
            source.index("static void psp_media_retire_codec_job(")
            if "static void psp_media_retire_codec_job(" in source
            else source.index("static MediaBackendResult "
                              "psp_media_queue_codec_job(")]
        self.assertIn("PSP_MEDIA_CODEC_JOB_WATCHDOG_US", collect)
        # It names the firmware call rather than the absence of packets, and
        # it publishes through the same door the worker would have used.
        self.assertIn("psp_media_codec_stage_name(wedged_stage)", collect)
        self.assertIn("did not return after", collect)
        self.assertIn("PSP_MEDIA_CODEC_JOB_DONE", collect)
        # Below the frontend's no-progress ceiling, so the specific stage is
        # named before the generic one fires.
        policy = without_comments(
            (ROOT / "src/media_backend_psp_policy.h").read_text(
                encoding="utf-8"))
        watchdog_us = int(re.search(
            r"PSP_MEDIA_CODEC_JOB_WATCHDOG_US\s+(\d+)u", policy).group(1))
        no_progress_ms = int(re.search(
            r"PSP_MEDIA_DECODE_NO_PROGRESS_MS\s+(\d+)u", policy).group(1))
        self.assertLess(watchdog_us, no_progress_ms * 1000)

    def test_a_seek_primes_both_sources_and_a_dead_window_terminates(self):
        """Both post-seek source heads must be present before playback can
        resume. A demanded window which stops making progress has a bounded
        reconnect/deadline outcome rather than remaining pending forever."""
        source = without_comments(
            psp_media_session_sources())
        pump = source[
            source.index("bool psp_media_seek_decode_pump("):
            source.index("void psp_media_close(")]
        prime = pump[
            pump.index("if (media->job_phase == PSP_MEDIA_JOB_SEEK_PRIME)"):
            pump.index("if (media->job_phase == PSP_MEDIA_JOB_SEEK_PREPARE")]
        self.assertIn("media_playback_prime_video_source(", prime)
        self.assertIn("media_playback_prime_audio_source(", prime)
        self.assertIn("media->job_prime_audio_us", prime)
        self.assertIn("required_ready = media->audio_only", prime)
        self.assertIn("PSP_MEDIA_SEEK_PRIME_AUDIO : PSP_MEDIA_SEEK_PRIME_ALL",
                      prime)
        self.assertIn("media->job_prime_ready_mask & required_ready", prime)
        self.assertLess(
            prime.index("media->job_prime_ready_mask & required_ready"),
            prime.index("media_playback_set_playing(media->playback, playing)"))

        http = without_comments(
            (ROOT / "src/media_http.c").read_text(encoding="utf-8"))
        restart = http[
            http.index("static bool range_restart_stalled_fill("):
            http.index("static void range_watch_window_liveness(")]
        watch = http[
            http.index("static void range_watch_window_liveness("):
            http.index("static void range_pump(")]
        self.assertIn("media_http_window_tracker_observe(", watch)
        self.assertIn("MEDIA_HTTP_WINDOW_FAIL", watch)
        self.assertIn("MEDIA_HTTP_WINDOW_RECONNECT", watch)
        self.assertIn("range_window_fail(", watch)
        self.assertNotIn("media_http_range_should_reconnect_at_rate(", watch)
        pump_range = http[
            http.index("static void range_pump("):
            http.index("static bool range_cancelled(")]
        self.assertIn("if (range->fill_request == 0) {", pump_range)
        self.assertIn("range_watch_window_liveness(range)", pump_range)
        self.assertIn("MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS", restart)
        self.assertIn("range_fill_abandon(", restart)
        self.assertIn("range->stats.reconnects++", restart)
        self.assertIn("range->stats.starved_reconnects++", restart)

    def test_audio_only_open_and_seek_have_no_video_dependency(self):
        """An audio-only route owns only AAC. Opening and seeking therefore
        cannot admit a video range or assume AAC is source one in a
        two-source playback object."""
        opening = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        pump = opening[opening.rindex("static bool psp_media_open_pump_step(") :]
        decoder = pump[
            pump.index("case PSP_MEDIA_JOB_OPEN_DECODER_PREPARE:"):
            pump.index("case PSP_MEDIA_JOB_OPEN_PLAYBACK:")]
        self.assertIn(
            "media->audio_only ? PSP_MEDIA_JOB_OPEN_AUDIO_RANGE",
            " ".join(decoder.split()))
        self.assertIn(
            "media->page_audio ? PSP_MEDIA_JOB_OPEN_DECODER_PREPARE",
            " ".join(pump.split()))
        self.assertIn(
            "media->audio_only && !media->page_audio",
            " ".join(pump.split()))
        audio_demux = pump[
            pump.index("case PSP_MEDIA_JOB_OPEN_AUDIO_DEMUX:"):
            pump.index("case PSP_MEDIA_JOB_OPEN_DECODER_PREPARE:")]
        self.assertIn(
            "media->audio_only ? PSP_MEDIA_JOB_OPEN_PLAYBACK",
            " ".join(audio_demux.split()))

        playback = without_comments(
            (ROOT / "src/media_backend.c").read_text(encoding="utf-8"))
        prime = playback[
            playback.index("media_playback_prime_audio_source("):
            playback.index("static bool media_playback_seek_internal(")]
        prime_flat = " ".join(prime.split())
        self.assertIn("playback->source_count == 1u", prime_flat)
        self.assertIn("? 0u : 1u", prime_flat)

        session = without_comments(psp_media_session_sources())
        page_open = session[
            session.index("static bool psp_media_open_page_source_kind("):
            session.index("bool psp_media_open_page_source(")]
        # A structured AudioObject preview intentionally has no synthetic
        # DOM media node. Handle zero is therefore valid; only negative input
        # is rejected, and state feedback becomes an ordinary no-op.
        self.assertIn("node_handle < 0", page_open)
        self.assertNotIn("node_handle <= 0", page_open)

    def test_swdec_shared_memory_and_reordered_pts_contracts(self):
        """ME-owned ranges must not share allocator cache lines, and a
        reordered picture must publish the source AU's timing rather than the
        most recently submitted job's timing."""
        source = without_comments(
            (ROOT / "src/media_backend_psp_swdec.c").read_text(
                encoding="utf-8"))
        create = source[source.index(
            "bool media_psp_swdec_backend_create_sources("):]
        for field in ("decoder_arena", "aux_arena", "rgb_ring",
                      "audio_packets"):
            self.assertRegex(
                create,
                rf"{field}\s*=\s*budget_malloc_cacheline_category\(")
        submit = source[
            source.index("static MediaBackendResult swdec_submit("):
            source.index("static MediaBackendResult swdec_drain(")]
        self.assertIn("sceKernelDcacheWritebackRange(", submit)
        publish = source[
            source.index("static void swdec_publish_picture("):
            source.index("static void swdec_select_speed(")]
        self.assertIn("uint64_t pts_us = mapping->pts_us", publish)
        self.assertIn("uint64_t duration_us = mapping->duration_us", publish)
        self.assertIn("state->pts_us = pts_us", publish)
        self.assertNotIn("state->pts_us = backend->job_pts_us", publish)
        # A flush reserves an output surface just like an ordinary decode.
        # The returned picture's source AU supplies timing, while conversion
        # is bound only after receive_frame identifies that picture.
        drain = source[
            source.index("static MediaBackendResult swdec_drain("):
            source.index("static bool swdec_advance(")]
        self.assertIn("int slot = swdec_find_free_slot(backend)", drain)
        self.assertIn("backend->job_slot = slot", drain)
        decode = source[
            source.index("static void swdec_decode_job("):
            source.index("static int swdec_worker_main(")]
        self.assertIn("if (result == 1) swdec_publish_picture", decode)
        self.assertIn("backend->api->csc_picture(", publish)
        self.assertIn("int slot = backend->job_slot", publish)
        self.assertIn("sceKernelDcacheInvalidateRange(", publish)
        self.assertIn("backend->rgb_slot_bytes", publish)
        self.assertIn(
            "backend->rgb_slot_bytes = swdec_cache_extent(", create)
        self.assertIn("backend->rgb_slot_bytes, picture", publish)
        self.assertIn(
            "media_h264_annexb_sample_is_admitted(", submit)
        release_prior = source[
            source.index("static void swdec_release_prior_claim("):
            source.index("static bool swdec_take_video_frame(")]
        release = source[
            source.index("static void swdec_release(void *opaque"):
            source.index("static void swdec_quarantine(")]
        self.assertLess(
            release_prior.index("retire_when_unborrowed"),
            release_prior.index("state->readers"))
        self.assertIn("atomic_exchange_explicit(", release)
        self.assertIn("swdec_free_slot(backend, (int) slot)", release)
        component = without_comments(
            (ROOT / "src/swdec/swdec.c").read_text(encoding="utf-8"))
        self.assertIn("d->max_width = max_width", component)
        self.assertIn("swdec_dimensions_admitted(", component)
        self.assertIn("swdec_audio_channels_admitted(channels)", component)
        me = without_comments(
            (ROOT / "src/swdec/swdec_me.c").read_text(encoding="utf-8"))
        self.assertGreaterEqual(
            me.count("swdec_rgb565_destination_fits("), 3)
        self.assertGreaterEqual(
            me.count("swdec_audio_channels_admitted(out.channels)"), 2)

    def test_hls_routes_from_sampled_profile_to_firmware_or_optional_decoder(self):
        """HLS is a source shape, not a decoder choice. Actual Annex-B SPS
        data selects the standard firmware path for Baseline/Main."""
        opened = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        hls = opened[
            opened.index("else if (media->page_hls"):
            opened.index("else if (media->page_source")]
        self.assertIn("media_h264_annexb_decoder_route(", hls)
        self.assertIn("MEDIA_H264_DECODER_ROUTE_HIGH_EXTENSION", hls)
        self.assertIn("psp_swdec_component_prepare(", hls)
        self.assertNotIn("media->decoder_profile_idc = 100u", hls)
        playback = opened[
            opened.index("static bool psp_media_create_playback("):
            opened.index("static bool psp_media_create_audio_http_range(")]
        self.assertIn("media_psp_backend_create_sources(", playback)
        self.assertIn("media_psp_swdec_backend_create_sources(", playback)
        self.assertIn("media_playback_create_sources(", playback)

        firmware = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        worker = firmware[
            firmware.index("static MediaBackendResult psp_media_decode_staged_video("):
            firmware.index("static MediaBackendResult psp_media_decode_staged_audio(")]
        self.assertIn("media_h264_annexb_to_avcc_in_place(", worker)
        submit = firmware[
            firmware.index("static MediaBackendResult psp_media_submit("):
            firmware.index("static MediaBackendResult psp_media_drain(")]
        self.assertIn("media_h264_annexb_sample_matches_config(", submit)

        hls_transport = without_comments(
            (ROOT / "src/psp_media_hls.c").read_text(encoding="utf-8"))
        self.assertIn(
            ".raw_gzip_encoding = raw_gzip", hls_transport,
            "pumpable HLS must own incremental playlist decompression")
        gzip_pump = without_comments(
            (ROOT / "src/psp_hls_gzip.c").read_text(encoding="utf-8"))
        self.assertIn("inflate(&gzip->stream, Z_NO_FLUSH)", gzip_pump)
        background_transport = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        self.assertIn("CURLOPT_HTTP_CONTENT_DECODING", background_transport)
        self.assertIn("slot->raw_gzip_encoding", background_transport)
        self.assertIn(
            "PSP_HLS_PLAYLIST_CHUNK_BYTES", hls_transport,
            "large playlists must use the bounded full publication quantum")
        self.assertIn(
            "psp_hls_playlist_retry_allowed(", hls_transport,
            "initial playlist transport failure needs one bounded restart")

    def test_swdec_prx_cleanup_tracks_partial_module_and_me_ownership(self):
        source = without_comments(
            (ROOT / "src/psp_swdec_component.c").read_text(encoding="utf-8"))
        load = source[
            source.index("static bool psp_swdec_component_load("):
            source.index("bool psp_swdec_component_prepare(")]
        self.assertLess(
            load.index("component->module_id = module"),
            load.index("sceKernelStartModule("))
        self.assertIn("component_unload_module(component)", load)
        cleanup = source[
            source.index("static bool component_unload_module("):
            source.index("static void component_failed_message(")]
        self.assertIn("component->took_me", cleanup)
        self.assertIn("sceKernelStopModule(", cleanup)
        self.assertIn("sceKernelUnloadModule(", cleanup)
        self.assertLess(
            cleanup.index("sceKernelUnloadModule("),
            cleanup.index("budget_reservation_release("))
        prepare = source[
            source.index("bool psp_swdec_component_prepare("):
            source.index("bool psp_swdec_component_suspend(")]
        self.assertLess(
            prepare.index("component->took_me = true"),
            prepare.index("component->api.attach_me("))
        self.assertIn("component->took_me = false", prepare)

    def test_hls_seek_enters_the_hls_source_and_preserves_probe_progress(self):
        seek = without_comments(
            (ROOT / "src/psp_media_seek.c").read_text(encoding="utf-8"))
        request = seek[
            seek.index("bool psp_media_request_seek_with_resume("):
            seek.index("bool psp_media_seek_decode_pump(")]
        self.assertIn("media->hls == NULL", request)
        hls = without_comments(
            (ROOT / "src/media_hls.c").read_text(encoding="utf-8"))
        common = hls[
            hls.index("static bool hls_seek_common("):
            hls.index("static bool hls_seek_us(")]
        self.assertLess(common.index("source->seek_pristine"),
                        common.index("hls_cancel_requests(source)"))
        self.assertIn("source->seek_pristine = false", hls)

    def test_media_ranges_leave_curl_progress_on_the_transport_worker(self):
        """A curl_multi pump can spend seconds in PSP DNS/TCP/TLS despite a
        nominally bounded callback quota. Live media therefore uses the
        process worker and the browser-thread pump only observes atomics.
        Replay/substituted transports deliberately retain their old path."""
        source = without_comments(
            (ROOT / "src/media_http.c").read_text(encoding="utf-8"))
        create = source[
            source.index("MediaHttpRange *media_http_range_create("):
            source.index("MediaRangeReader media_http_range_reader(")]
        self.assertIn("fetch_background_transport_available()", create)
        issue = source[
            source.index("static RangeFillIssueStatus range_fill_issue("):
            source.index("static bool range_fill_install(")]
        self.assertIn(
            "fetch_background_transport_enqueue_media_diagnosed(", issue)
        self.assertIn("RANGE_FILL_DEFERRED", issue)
        self.assertIn("range->fill_admission_deferred = true", issue)
        prime = source[
            source.index("MediaHttpRangePrimeStatus "
                         "media_http_range_prime_successor("):
            source.index("void media_http_range_set_aggressive_readahead(")]
        self.assertIn("range_window_mark_demanded(range)", prime)
        self.assertIn("range->fill_attempts >= 2u", prime)
        self.assertIn("MEDIA_HTTP_RANGE_PRIME_FAILED", prime)
        fill = source[
            source.index("static MediaRangeReadStatus range_fill_window("):
            source.index("static MediaRangeReadStatus range_read_bounded(")]
        self.assertIn("issue == RANGE_FILL_FAILED", fill)
        self.assertNotIn("!range_fill_issue(", fill)
        pump = source[
            source.index("static void range_pump("):
            source.index("static bool range_cancelled(")]
        self.assertIn("if (!range_uses_background(range))", pump)
        self.assertIn("fetch_scheduler_pump(", pump)
        self.assertNotIn("curl_", pump)

    def test_native_media_ranges_use_compact_response_metadata(self):
        """A page FetchResult carries bounded CSP, client-hint, header and
        cookie state and is about 14 KiB on the 32-bit target. Media ranges
        are credential-less and consume none of it, so neither their header
        poll nor native completion path may put that generic object on the
        browser thread's repeatedly-entered stack."""
        header = without_comments(
            (ROOT / "include/tilefinch/fetch.h").read_text(
                encoding="utf-8"))
        self.assertIn(
            "sizeof(FetchBackgroundMediaResponse) < 3u * 1024u", header)

        media = without_comments(
            (ROOT / "src/media_http.c").read_text(encoding="utf-8"))
        drain = media[
            media.index("static void range_stream_drain_background("):
            media.index("static void range_record_superseded(")]
        self.assertIn("FetchBackgroundMediaResponse metadata", drain)
        self.assertNotIn("FetchResult", drain)
        install = media[
            media.index("static bool range_fill_install_into("):
            media.index("static bool range_fill_install(")]
        self.assertNotIn("FetchResult", install)
        self.assertNotIn("FetchBackgroundResult", install)
        self.assertLess(
            install.index("range_stream_drain_background(range)"),
            install.index("range_take_background_stream("),
            "a completed transfer must publish its final queued bytes before "
            "the compact result is consumed")
        self.assertIn("range->fill_streamed_bytes != wanted", install)
        self.assertIn("range_take_background_stream(", install)
        self.assertIn("range_take_background_fixed(", install)
        for helper in (
                "range_take_background_stream",
                "range_take_background_fixed",
                "range_take_scheduler_stream",
                "range_take_scheduler_fixed"):
            declaration = media[media.index("static TILEFINCH_OUT_OF_LINE "):
                                media.index("static bool range_fill_install_into(")]
            self.assertIn(helper + "(", declaration)
        prepare = media[
            media.index("static bool range_prepare_request("):
            media.index("static bool range_admit_values(")]
        self.assertIn(".credentials = FETCH_CREDENTIALS_OMIT", prepare)

        transport = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        compact = transport[
            transport.index(
                "bool fetch_background_transport_take_media_result_consumed("):
            transport.index("bool fetch_background_transport_take_fetch_result(")]
        self.assertIn("fetch_background_transport_take_media(", compact)
        self.assertNotIn("budget_malloc", compact)
        self.assertNotIn("response_cookies", compact)

    def test_background_transport_has_fixed_ownership_and_no_worker_side_effects(self):
        """The network worker may own curl and fixed response buffers only.
        Budget, logging, UI and filesystem work remain on the browser thread;
        otherwise cancellation could race intrusive ledgers or hot-path I/O."""
        header = without_comments(
            (ROOT / "include/tilefinch/fetch.h").read_text(
                encoding="utf-8"))
        source = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        self.assertIn("FETCH_BACKGROUND_REQUEST_LIMIT 6u", header)
        self.assertIn("FETCH_BACKGROUND_ACTIVE_LIMIT 2u", header)
        self.assertIn("FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT 6u", header)
        self.assertIn(
            "FETCH_BACKGROUND_MAXIMUM_RESPONSE_BYTES (256u * 1024u)",
            header)
        self.assertIn(
            "if (buffer < 0) continue;",
            source,
            "fixed-buffer pressure must not serialize independent streams")
        self.assertIn("FETCH_BACKGROUND_STREAM_CHUNK_TARGET", source)
        self.assertIn("FETCH_BACKGROUND_STREAM_PUBLICATION_MAX", source)
        initialize = source[
            source.index("bool fetch_background_transport_initialize("):
            source.index("static bool fetch_background_preconnect_begin(")]
        self.assertNotIn(
            "budget_malloc_category", initialize,
            "bodyless HOME speculation must not allocate response buffers")
        self.assertIn(
            "fetch_background_ensure_stream_capacity()", source,
            "stream buffers must grow only when a real response is admitted")
        preconnect = without_comments(
            (ROOT / "src/fetch/preconnect.inc").read_text(encoding="utf-8"))
        self.assertIn("CURLOPT_NOBODY, 1L", preconnect)
        self.assertIn("CURLOPT_NOBODY, 1L", source)
        self.assertIn("CURLOPT_USERAGENT, TILEFINCH_BROWSER_USER_AGENT",
                      preconnect)
        self.assertIn("CURLOPT_USERAGENT, FETCH_BACKGROUND_USER_AGENT",
                      source)
        self.assertNotIn("CURLOPT_CONNECT_ONLY,", preconnect)
        self.assertEqual(
            source.count("CURLOPT_CONNECT_ONLY,"), 1,
            "only the bounded WebSocket lane may retain a connected socket")
        websocket_config = source[
            source.index("static bool fetch_background_websocket_configure("):
            source.index("static void fetch_background_websocket_start_queued(")]
        self.assertIn("CURLOPT_CONNECT_ONLY, 2L", websocket_config)
        websocket_open = source[
            source.index("uint64_t fetch_background_websocket_open("):
            source.index("bool fetch_background_websocket_send(")]
        self.assertIn("fetch_websocket_prepare_admission(", websocket_open)
        self.assertIn(
            "socket->private_network.required = protect_private_network",
            websocket_open)
        self.assertNotIn(
            "request->block_private_network", websocket_open,
            "page WebSockets must derive PNA at the native sink")
        self.assertLess(
            websocket_open.index("fetch_websocket_prepare_admission("),
            websocket_open.index("FETCH_BACKGROUND_WEBSOCKET_PREPARING"),
            "policy must pass before a worker slot is claimed")
        websocket_send = source[
            source.index("static void fetch_background_websocket_service_send("):
            source.index(
                "static bool fetch_background_websocket_publish_pending_drain(")]
        self.assertIn(
            "fetch_background_websocket_publish_close_bytes(",
            websocket_send,
            "delayed peer-close publication must preserve its explicit length")
        self.assertIn("socket->peer_close_reason_length", websocket_send)
        self.assertIn(
            "FETCH_BACKGROUND_WEBSOCKET_DRAIN_PENDING", websocket_send,
            "a busy browser-event slot must not discard send completion")
        self.assertNotIn(
            "socket->event.kind = FETCH_WEBSOCKET_EVENT_DRAIN",
            websocket_send,
            "send completion must enter the pending latch before publication")
        websocket_drain = source[
            source.index(
                "static bool fetch_background_websocket_publish_pending_drain("):
            source.index("static void fetch_background_websocket_service_receive(")]
        self.assertIn(
            "FETCH_BACKGROUND_WEBSOCKET_DRAIN_PUBLISHED", websocket_drain)
        self.assertNotIn(
            "if (sent == 0)", websocket_drain,
            "a zero-byte text send still requires DRAIN to release the JS queue")
        websocket_receive = source[
            source.index("static void fetch_background_websocket_service_receive("):
            source.index("static void fetch_background_websocket_service(void)")]
        self.assertLess(
            websocket_receive.index("fetch_websocket_text_payload_valid("),
            websocket_receive.index("FETCH_WEBSOCKET_EVENT_MESSAGE_TEXT"),
            "malformed text must close with 1007 before JavaScript delivery")
        websocket_service = source[
            source.index("static void fetch_background_websocket_service(void)"):
            source.index("static int fetch_background_worker_main(")]
        self.assertGreaterEqual(
            websocket_service.count(
                "fetch_background_websocket_publish_pending_drain(socket)"),
            2,
            "pending send completion must run before receive and immediately "
            "after a completed send")
        websocket_take = source[
            source.index("bool fetch_background_websocket_take_event("):
            source.index("static bool fetch_background_all_free(")]
        self.assertIn(
            "FETCH_BACKGROUND_WEBSOCKET_DRAIN_PENDING", websocket_take,
            "consuming a busy event slot must wake pending DRAIN publication")
        websocket_native_send = source[
            source.index("bool fetch_background_websocket_send("):
            source.index("bool fetch_background_websocket_close(")]
        self.assertIn("length != 0 && data == NULL", websocket_native_send)
        self.assertIn(
            "FETCH_BACKGROUND_WEBSOCKET_DRAIN_EMPTY", websocket_native_send,
            "the next native send must wait for the preceding DRAIN event")
        raw_transport_source = (
            ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8")
        self.assertIn(
            "Hash-pinned curl 8.21.0 auto-enqueues", raw_transport_source)
        curl_config = (ROOT / "cmake/PspOwnedTransport.cmake").read_text(
            encoding="utf-8")
        self.assertIn("curl-8.21.0", curl_config)
        self.assertNotIn("CURLWS_NOAUTOPONG", websocket_config)
        self.assertIn("curl_easy_attach_shared_state(slot->easy)", source)
        self.assertIn("CURLOPT_TIMEOUT_MS, 0L", source)
        shared_state = (ROOT / "src/fetch/response_stream.inc").read_text(
            encoding="utf-8")
        attach = shared_state[
            shared_state.index("static bool curl_easy_attach_shared_state("):
            shared_state.index("static bool curl_global_release(")]
        self.assertIn("#if defined(__PSP__)", attach)
        self.assertIn("CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4", attach)
        self.assertNotIn("CURL_IPRESOLVE_V4", shared_state[:
            shared_state.index("static bool curl_easy_attach_shared_state(")])
        policy = (ROOT / "src/fetch/policy.inc").read_text(encoding="utf-8")
        transport = (ROOT / "src/fetch/transport.inc").read_text(
            encoding="utf-8")
        self.assertIn(
            "#if defined(AF_INET6) && !defined(__PSP__)", policy)
        self.assertIn(
            "#if defined(AF_INET6) && !defined(__PSP__)", transport)
        worker_work = source[
            source.index("static bool fetch_background_has_worker_work("):
            source.index("static int fetch_background_worker_main(")]
        self.assertIn("FETCH_BACKGROUND_SLOT_QUEUED", worker_work)
        self.assertIn("FETCH_BACKGROUND_SLOT_RUNNING", worker_work)
        worker = source[
            source.index("static int fetch_background_worker_main("):
            source.index("bool fetch_background_transport_available(")]
        for forbidden in (
                "budget_", "fopen(", "fwrite(", "sceIo",
                "tilefinch_platform_log", "psp_ui_"):
            self.assertNotIn(forbidden, worker)
        self.assertIn("curl_multi_perform(", worker)
        self.assertIn(
            "running > 0 || fetch_background_has_worker_work()", worker)

    def test_transport_owners_die_before_worker_and_network_teardown(self):
        network = without_comments(
            (ROOT / "src/psp_app/psp_app_network.c").read_text(
                encoding="utf-8"))
        shutdown = network[
            network.index("static bool psp_network_lifecycle_pump_stopping(",
                          network.index("bool psp_connect_network(")):
            network.index("bool psp_ensure_network_for_navigation(")]
        self.assertLess(
            shutdown.index("fetch_background_transport_request_quiesce("),
            shutdown.index("psp_network_shutdown_begin("))
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        cleanup = main[main.index("cleanup-begin") :]
        engine_destroy = cleanup.index("browser_engine_destroy(")
        transport_shutdown = cleanup.index(
            "fetch_background_transport_shutdown(")
        network_shutdown = cleanup.index("psp_shutdown_network_logged(")
        self.assertLess(engine_destroy, transport_shutdown)
        self.assertLess(transport_shutdown, network_shutdown)
        self.assertRegex(
            cleanup,
            r"background_transport_stopped\s*&&\s*"
            r"psp_network_lifecycle_started")

    def test_youtube_provider_moves_only_transport_to_worker(self):
        source = without_comments(
            (ROOT / "src/youtube_lite.c").read_text(encoding="utf-8"))
        begin = source[
            source.index(
                "YoutubeLiteLoadJob *youtube_lite_load_begin_configured("):
            source.index("YoutubeLiteLoadStatus youtube_lite_load_pump(")]
        self.assertIn("fetch_scheduler_create_ex(", begin)
        self.assertIn(
            "fetch_scheduler_enable_background_transport(", begin)
        self.assertLess(
            begin.index("fetch_scheduler_create_ex("),
            begin.index("fetch_scheduler_enable_background_transport("))

    def test_home_preconnect_uses_dedicated_worker_descriptor(self):
        transport = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        service = transport[
            transport.index("static void fetch_background_service_preconnect("):
            transport.index("static bool fetch_background_finish_preconnect(")]
        self.assertIn("FETCH_BACKGROUND_PRECONNECT_QUEUED", service)
        self.assertIn("fetch_background_preconnect_configure()", service)
        preconnect = without_comments(
            (ROOT / "src/fetch/preconnect.inc").read_text(encoding="utf-8"))
        start = preconnect[
            preconnect.index("bool fetch_preconnect("):
            preconnect.index("void fetch_preconnect_pump(")]
        self.assertLess(
            start.index("fetch_background_preconnect_begin("),
            start.index("curl_global_acquire("))
        self.assertIn("if (fetch_background_transport_available())", start)
        self.assertIn("return false;", start)

    def test_home_preconnect_is_limited_to_adapter_owned_destination(self):
        source = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        loop = source[
            source.index("const char *preconnect_url ="):
            source.index("fetch_preconnect_pump();")]
        self.assertIn("site_adapter_handles_navigation(", loop)
        self.assertIn('"GET", preconnect_url', loop)
        warmup = source[
            source.index("if (network_was_warming"):
            source.index("if (process->config.interactive_validation_ticks")]
        self.assertIn('site_adapter_handles_navigation("GET", url)', warmup)

    def test_page_script_networking_keeps_policy_on_bridge_thread(self):
        source = without_comments(
            (ROOT / "src/js_runtime/bridge_state.inc").read_text(
                encoding="utf-8"))
        create = source[
            source.index("static FetchScheduler *bridge_fetch_scheduler_create("):
            source.index("uintptr_t js_rt_node_owner_identity(")]
        self.assertIn("fetch_scheduler_create_in_domain(", create)
        self.assertIn(
            "fetch_scheduler_enable_background_transport(", create)
        self.assertNotIn("fetch_background_transport_enqueue", create)

    def test_offline_download_worker_never_owns_filesystem(self):
        source = without_comments(
            (ROOT / "src/offline_download.c").read_text(encoding="utf-8"))
        start = source[
            source.index("bool offline_download_manager_start("):
            source.index("bool offline_download_manager_start_next_queued(")]
        self.assertIn("fetch_scheduler_enable_background_transport(", start)
        worker = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        worker = worker[
            worker.index("static int fetch_background_worker_main("):
            worker.index("bool fetch_background_transport_available(")]
        for forbidden in ("fopen(", "fwrite(", "sceIo"):
            self.assertNotIn(forbidden, worker)

    def test_updater_worker_never_owns_trust_or_install_state(self):
        source = without_comments(
            (ROOT / "src/update_client.c").read_text(encoding="utf-8"))
        create = source[
            source.index("TilefinchUpdateClient *tilefinch_update_client_create("):
            source.index("static void update_close_part(")]
        self.assertIn("fetch_scheduler_enable_background_transport(", create)
        worker = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        worker = worker[
            worker.index("static int fetch_background_worker_main("):
            worker.index("bool fetch_background_transport_available(")]
        for forbidden in (
                "tilefinch_update", "manifest", "sha256", "fopen(",
                "fwrite(", "rename(", "sceIo"):
            self.assertNotIn(forbidden, worker)

    def test_updater_download_is_batched_but_time_bounded(self):
        source = without_comments(
            (ROOT / "src/update_client.c").read_text(encoding="utf-8"))
        self.assertIn("#define UPDATE_DOWNLOAD_PUMP_CALLBACKS 4u", source)
        self.assertIn(
            "UPDATE_DOWNLOAD_PUMP_CALLBACKS * UPDATE_PUMP_BYTES", source)
        pump = source[
            source.index("bool tilefinch_update_client_pump("):
            source.index("bool tilefinch_update_client_snapshot(")]
        self.assertIn("client->request_is_download", pump)
        self.assertIn("UPDATE_DOWNLOAD_PUMP_CALLBACKS", pump)
        self.assertIn("UPDATE_DOWNLOAD_PUMP_BYTES", pump)
        self.assertIn("maximum_time_us == 0 ? 2000u : maximum_time_us", pump)

    def test_updater_retains_download_authority_without_skipping_files(self):
        client = without_comments(
            (ROOT / "src/update_client.c").read_text(encoding="utf-8"))
        self.assertIn("update_capture_package_table", client)
        self.assertIn("package_table_verified", client)
        self.assertIn("tilefinch_update_client_download_proof", client)

        installer = without_comments(
            (ROOT / "src/update_installer.c").read_text(encoding="utf-8"))
        create = installer[
            installer.index("TilefinchUpdateInstallJob *tilefinch_update_install_create("):
            installer.index("void tilefinch_update_install_destroy(")]
        self.assertIn("options->download_proof", create)
        self.assertIn("TILEFINCH_UPDATE_INSTALL_PREPARING", create)
        extraction = installer[
            installer.index("static bool install_open_entry("):
            installer.index("bool tilefinch_update_install_snapshot(")]
        self.assertIn("tilefinch_sha256_update", extraction)
        self.assertIn("memcmp(digest, entry->sha256, 32)", extraction)

        session = without_comments(
            (ROOT / "src/psp_update_session.c").read_text(encoding="utf-8"))
        self.assertIn("tilefinch_update_client_download_proof", session)
        self.assertIn(".download_proof = have_download_proof", session)
        self.assertIn("TILEFINCH_UPDATE_INSTALL_VERIFYING", session)

    def test_updater_table_capture_is_an_optional_fast_path(self):
        client = without_comments(
            (ROOT / "src/update_client.c").read_text(encoding="utf-8"))
        capture = client[
            client.index("static bool update_capture_package_table("):
            client.index("static bool update_package_body(")]
        self.assertIn("package_table_capture_disabled", capture)
        self.assertIn(
            "client->package_table_capture_disabled = true", capture)
        # A rejected or unallocatable retained table must not abort the
        # authenticated download; the installer owns the fallback reread.
        self.assertNotIn("return false", capture)
        finish = client[
            client.index("static void update_finish_download("):
            client.index("bool tilefinch_update_client_pump(")]
        self.assertNotIn(
            "client->artifact == TILEFINCH_UPDATE_ARTIFACT_BROWSER && (",
            finish)

        session = without_comments(
            (ROOT / "src/psp_update_session.c").read_text(encoding="utf-8"))
        self.assertIn(
            "have_download_proof ? &download_proof : NULL", session)

    def test_the_worker_is_given_work_before_the_thread_stops(self):
        """The codec worker completion is collected through a bounded wait, so
        work offered before a hardware wait can finish inside that wait rather
        than a frame later. Two consequences the device measured. A single four-millisecond
        collect wait consumed a 4.45ms draw window and returned 0.6
        submissions a frame, where several short looks would have returned
        more. And the frame's longest block, the wait for the vertical blank,
        was never fed at all: whatever the present happened to leave queued
        was all the worker had for it."""
        source = without_comments(
            psp_media_session_sources())
        pump = source[
            source.index("static size_t psp_media_pump_present("):
            source.index("size_t psp_media_pump_while_drawing(")]
        # A unit may sleep, but never past the window that made it free.
        self.assertIn("media_psp_backend_set_wait_limit_us(", pump)
        self.assertIn("PSP_MEDIA_PUMP_DRAW_SLICE_US - spent_us", pump)
        # The feed before the block takes no wait of any kind: it runs on the
        # frame's own terms, which is what makes it affordable there.
        feed = source[
            source.index("size_t psp_media_feed_before_blocking("):
            source.index("void psp_media_present_texture_release(")]
        self.assertIn("media_playback_advance_bounded_cancelable(", feed)
        self.assertNotIn("media_psp_backend_set_advance_mode(", feed)
        self.assertIn("media->job_phase != PSP_MEDIA_JOB_NONE", feed)
        self.assertNotIn("media->clock_us =", feed)
        # And it is the last thing the frame does before it stops.
        loop = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        feed_at = loop.index(
            "psp_media_feed_before_blocking(&browser->media)")
        blank_at = loop.index("psp_wait_before_interactive_input(", feed_at)
        self.assertLess(feed_at, blank_at)
        self.assertNotIn(
            "psp_media_advance(", loop[feed_at:blank_at])
        # Fullscreen playback is ultimately paced by the presenter's own
        # vblank. Its pre-work yield is shorter so 24-fps PTS checks are not
        # quantized to a second display period, but it still blocks after the
        # feed and therefore still gives this lower-priority worker CPU.
        between = loop[feed_at:blank_at]
        self.assertIn("fullscreen_media_poll", between)
        wait = loop[
            loop.index("static TILEFINCH_OUT_OF_LINE void "
                       "psp_wait_before_interactive_input("):
            loop.index("static TILEFINCH_OUT_OF_LINE bool "
                       "psp_update_page_fullscreen(")]
        self.assertIn("sceKernelDelayThread(", wait)
        self.assertIn("PSP_MEDIA_FULLSCREEN_POLL_YIELD_US", wait)
        self.assertIn("sceDisplayWaitVblankStart()", wait)
        # NEXTFRAME latches only at vblank. Menu motion must not share the
        # media shortcut or the next loop can overwrite the buffer that is
        # still being scanned, a hardware-only flicker the host compositor
        # cannot reproduce.
        self.assertNotIn("psp_ui_motion_pending", between)

    def test_a_cold_source_is_not_a_stalled_decoder(self):
        """The no-packet ceiling assumes a pipeline holding everything it
        needs. A cold position does not: two 256 KiB windows over a link that
        delivers 150-250 KB/s are three to four seconds before the first
        access unit. A device resume at 168s of a 252s stream died seven
        seconds in with source-block=134 refill-block=134 and the refill still
        on the wire -- and the retired-leg seek makes every far seek resume
        into exactly that state."""
        source = without_comments(
            psp_media_session_sources())
        # Bytes alone cannot see it: a window fully received and not yet
        # installed holds its total still, and so does a source with no
        # request outstanding. The pending window is what says "working".
        refilling = source[
            source.index("static bool psp_media_source_refilling("):
            source.index("size_t psp_media_range_bytes(")]
        self.assertIn("window_pending", refilling)
        self.assertIn("bytes_in_flight", refilling)
        # The budget is chosen by that, and the failure says which it was.
        self.assertIn(
            "psp_media_decode_no_progress_budget_ms(source_refilling)",
            source)
        self.assertIn(
            "psp_media_decode_no_progress_watchdog_active(", source)
        self.assertIn("media->machine.preview_active", source)
        self.assertNotIn("media->decode_no_progress_ms >= 2000u", source)
        self.assertIn("refilling=%d", source)

    def test_the_raw_nal_probe_cannot_kill_a_proven_session(self):
        """The probe asks whether this firmware's raw-NAL bridge produces
        pictures at all, and it answers "no" from one access unit that
        produced none past its sixteen-packet bound. That is a question about
        leading data. Asked again mid-stream it is simply wrong: output
        arrives in batches of two and three, so the units in between produce
        nothing. A device soak died two seconds after a forward seek with 245
        of 248 access units decoded -- a working decoder, declared
        unavailable, because the seek's reset re-armed the probe and the
        disarm sat behind an early return that a proven surface never
        passes."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        # The definition, not the forward declaration above it.
        validate = source[
            source.rindex(
                "static bool psp_media_validate_output_surface(\n"
                "    PspMediaBackend *backend, unsigned write_slot,\n"
                "    char *error, size_t error_size)\n{"):
            source.index("static MediaBackendResult "
                         "psp_media_raw_nal_probe_failed(")]
        # A picture answers the probe whether or not it also had to prove the
        # surface extent, so the disarm comes before that return.
        self.assertLess(
            validate.index("backend->raw_nal_probe_pending = false"),
            validate.index("sceKernelDcacheWritebackInvalidateRange("))
        # And a reset only asks a question that is still unanswered.
        reset = source[
            source.index("static bool psp_media_reset("):
            source.index("static bool psp_media_take_frame(")]
        self.assertIn("backend->raw_nal_probe_pending = true", reset)
        self.assertLess(
            reset.index(
                "if (backend->have_video && !psp_media_surface_proven(backend))"),
            reset.index("backend->raw_nal_probe_pending = true"))

    def test_the_frame_is_claimed_after_the_pump_not_before(self):
        """A claim frees the slot it supersedes, and a free slot is what lets
        a video access unit into the decoder. The surface borrow does not
        engage until the present. So every instruction between the claim and
        the present is a window in which a conversion may land in a slot the
        pipeline has just been given back -- and while the ownership machine
        now makes the CLAIMED slot unwritable for as long as the frontend can
        still refer to it, the ordering here is what keeps that window empty of
        submissions rather than merely survivable.

        Claiming before the pump made that window a whole pump wide, with the
        pump submitting into it, and was reverted for exactly that. After the
        pump the window holds no submission at all. Keep it that way: this is
        an ordering the compiler cannot check and a plausible-sounding
        optimisation wants to undo."""
        session = without_comments(
            (ROOT / "src/psp_media_session.c").read_text(encoding="utf-8"))
        advance = session[
            session.index("bool psp_media_advance(\n    PspMediaSession"):]
        self.assertLess(
            advance.index("media_playback_advance_bounded_cancelable("),
            advance.index("media_playback_take_video_frame("))
        # One claim in the advance, so one per present.
        self.assertEqual(
            1, advance.count("media_playback_take_video_frame("))

    def test_a_decoded_slot_has_one_owner_at_a_time(self):
        """Holding video at the submit boundary fences the writes the browser
        thread orders. It cannot fence the one it does not: an access unit
        accepted before a read window opens is converted into a surface by the
        codec worker whenever firmware finishes, which can be in the middle of
        the stage copy or of a draw sampling that surface directly. The margin
        that made that safe was arithmetic -- decode plus conversion against a
        window six milliseconds wide -- and it narrows every time the decoder
        gets faster.

        With two slots the question is per slot, and the answer has to be too.
        A single flag pair would have made a claim on either slot fence a
        conversion into the other, which is the whole of what the second slot
        was reserved to stop; and an unqualified release would free whichever
        slot happened to be indexed first, which is worse than not releasing at
        all. Every claim, every release and every announcement names its
        slot."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        # Two flags claimed in opposite orders, per slot. Each side stores
        # before it loads, so on one CPU at least one of them sees the other;
        # either order of assertions below failing would be a claim that can be
        # missed.
        borrow = source[
            source.index(
                "bool media_psp_backend_borrow_surface("
                "unsigned slot, uint32_t generation)\n{"):
            source.index("void media_psp_backend_end_surface_read(")]
        self.assertLess(
            borrow.index("atomic_store(&psp_media_surface_borrowed[slot], 1)"),
            borrow.index("atomic_load(&psp_media_surface_writing[slot])"))
        self.assertIn("PSP_MEDIA_SURFACE_BORROW_WAIT_US", borrow)
        # And the claim says WHICH picture it means. A generation that no
        # longer describes the slot is refused with no lease taken: the
        # address is still valid memory holding a different picture, which is
        # the one failure nothing else in the system would notice.
        self.assertLess(
            borrow.index("psp_media_surface_lease_matches(slot, generation)"),
            borrow.index("atomic_load(&psp_media_surface_writing[slot])"))
        self.assertIn(
            "atomic_store(&psp_media_surface_borrowed[slot], 0)", borrow)
        write = source[
            source.index(
                "static bool psp_media_surface_begin_write(unsigned slot)"):
            source.index(
                "static void psp_media_surface_end_write(unsigned slot)")]
        self.assertLess(
            write.index("atomic_store(&psp_media_surface_writing[slot], 1)"),
            write.index("atomic_load(&psp_media_surface_borrowed[slot])"))
        self.assertIn("PSP_MEDIA_SURFACE_LEASE_WAIT_US", write)
        # Every conversion any writer performs is inside the announcement, and
        # one that could not be made leaves the picture captured for a later
        # visit rather than dropping it. All of them go through one loop, so
        # this is checked once rather than at three call sites that could
        # drift apart.
        emit = source[
            source.index(
                "static int psp_media_emit_batch_into_free_slots("):
            source.index("static const char *psp_media_codec_stage_name(")]
        self.assertLess(
            emit.index("psp_media_surface_begin_write((unsigned) slot)"),
            emit.index("psp_media_emit_captured_picture("))
        self.assertLess(
            emit.index("psp_media_emit_captured_picture("),
            emit.index("psp_media_surface_end_write((unsigned) slot)"))
        # It converts into a FREE slot and stops at the first one it cannot
        # get. Converting into anything else is a write over a picture
        # somebody still owns.
        self.assertIn("psp_media_slot_free_index(", emit)
        self.assertIn("if (slot < 0) break;", emit)
        # The reviewer's condition: as many pictures from the batch as there
        # are free slots, before the job is published. Deferring a surplus
        # picture while a slot sits free is the phase this change removes.
        self.assertIn("while (psp_media_batch_pending(backend))", emit)
        # Every writer uses that loop, and nothing calls the single-picture
        # conversion directly except it.
        self.assertEqual(
            2, source.count("psp_media_emit_captured_picture("),
            "the per-picture conversion has one caller and one declaration; "
            "a second call site would be a writer outside the announcement")
        for function, following in (
                ("static MediaBackendResult psp_media_decode_staged_video(",
                 "static MediaBackendResult psp_media_decode_staged_audio("),
                ("static MediaBackendResult psp_media_drain_staged_video(",
                 "static MediaBackendResult psp_media_run_teardown_job("),
                ("static bool psp_media_emit_pending_video(",
                 "static bool psp_media_stats(")):
            body = source[
                source.index(function):source.index(following)]
            self.assertIn("psp_media_emit_batch_into_free_slots(", body)
        # The conversion refuses a target that is not free, at the bottom as
        # well as at every call site. This is the invariant the whole design
        # rests on, so it is checked where it cannot be skipped.
        convert = source[
            source.index(
                "static int psp_media_emit_captured_picture(\n"
                "    PspMediaBackend *backend, unsigned write_slot, "
                "uint64_t epoch,\n"
                "    char *error, size_t error_size)\n{"):
            source.rindex(
                "static bool psp_media_validate_output_surface(\n"
                "    PspMediaBackend *backend, unsigned write_slot,\n"
                "    char *error, size_t error_size)\n{")]
        self.assertIn(
            "psp_media_slot_observe(&backend->slots[write_slot])\n"
            "           != PSP_MEDIA_SLOT_FREE",
            convert)
        # ME_WRITING is entered before the firmware call and READY published
        # after every field that describes the picture, so a claim can never
        # observe READY over metadata still being written.
        self.assertLess(
            convert.index(
                "psp_media_slot_publish(state, PSP_MEDIA_SLOT_ME_WRITING)"),
            convert.index("psp_media_convert_avc_picture("))
        self.assertLess(
            convert.index("psp_media_convert_avc_picture("),
            convert.index(
                "psp_media_slot_publish(state, PSP_MEDIA_SLOT_READY)"))
        self.assertLess(
            convert.index("state->identity = backend->frame_identity"),
            convert.index(
                "psp_media_slot_publish(state, PSP_MEDIA_SLOT_READY)"))
        # The generation moves as the slot leaves FREE, so a reader holding
        # the previous picture's pair can tell.
        self.assertLess(
            convert.index("state->generation++"),
            convert.index("psp_media_convert_avc_picture("))
        # And the conversion never writes that word directly. The global
        # take-vs-job gate used to supply the ordering between this thread and
        # the browser thread -- the worker release-stored the job DONE, the
        # browser acquired it before reading any slot, and no take ran in
        # between. With the gate gone, the slot's own state word is the only
        # thing pairing them, so it moves exclusively through the release
        # store in psp_media_slot_publish. A plain assignment here would
        # compile and would silently be the data race.
        self.assertNotIn("state->state =", convert)
        # The release-point conversion is ordered by the borrow protocol
        # alone. It must NOT consult the advance mode: that hold fences
        # submissions, whose conversion lands on the worker at a time the
        # browser thread does not choose, and reusing it here would refuse
        # exactly the releases this exists to act on.
        emit_pending = source[
            source.index("static bool psp_media_emit_pending_video("):
            source.index("static bool psp_media_stats(")]
        self.assertNotIn("psp_media_advance_mode", emit_pending)
        # A claim normally frees exactly the reader it supersedes. The one
        # additional hand-back is the bounded A/V catch-up: it may retire one
        # still-READY late head only when a due READY successor exists. It is
        # never a reader release, so the loop cannot still point at its bytes.
        take = source[
            source.index("static bool psp_media_take_frame("):
            source.index("static size_t psp_media_discard_video_before(")]
        self.assertEqual(
            2, take.count("PSP_MEDIA_SLOT_FREE"),
            "a claim frees its prior reader and at most one READY late head")
        self.assertIn("psp_media_video_should_drop_late(", take)
        self.assertIn("psp_media_slot_holds_picture(", take)
        self.assertIn("backend->reading_slot != chosen", take)
        self.assertIn(
            "psp_media_slot_publish(picture, PSP_MEDIA_SLOT_READING)", take)
        # The claim writes that word through the same release store as every
        # other transition, and reads it through the acquire in
        # psp_media_slot_take_index. Those two are the entire ordering between
        # this thread and the codec worker now.
        self.assertNotIn("picture->state =", take)
        self.assertNotIn("->state = PSP_MEDIA_SLOT", take)
        # Earliest presentation time, and never past an ineligible earliest:
        # displaying late is recoverable, displaying out of order is not.
        self.assertIn("psp_media_slot_take_index(", take)
        self.assertLess(
            take.index("psp_media_slot_take_index("),
            take.index("PSP_MEDIA_TAKE_EARLY"))
        self.assertEqual(
            1, take.count("psp_media_slot_take_index("),
            "an ineligible earliest picture is waited for, not skipped")
        # Seek catch-up is a separate pre-claim operation. It may retire only
        # READY pictures from the fixed slot pool, never a borrowed picture,
        # and remains bounded independently of GOP length.
        discard = source[
            source.index("static size_t psp_media_discard_video_before("):
            source.index("static bool psp_media_release_video_slot(")]
        self.assertIn("pass < PSP_MEDIA_SURFACE_SLOTS", discard)
        self.assertIn("psp_media_slot_take_index(", discard)
        self.assertIn("picture->pts_us >= floor_us", discard)
        self.assertIn("psp_media_surface_writing[chosen]", discard)
        self.assertIn(
            "psp_media_slot_publish(picture, PSP_MEDIA_SLOT_FREE)", discard)
        self.assertNotIn("PSP_MEDIA_SLOT_READING", discard)
        # The submit gate that used to be the rate now asks whether ANY slot
        # is free rather than whether the one surface was claimed.
        submit = source[
            source.index("static MediaBackendResult psp_media_submit("):
            source.index("static MediaBackendResult psp_media_drain(")]
        self.assertIn("psp_media_slot_free_count(", submit)
        self.assertLess(
            submit.index("psp_media_slot_free_count("),
            submit.index("psp_media_emit_batch_into_free_slots("))
        # The extent canary is armed on every slot the conversion could
        # choose, because the check happens in whichever slot the worker
        # picked and a proof armed elsewhere proves nothing.
        self.assertIn("state->canary_armed = true", submit)
        self.assertIn(
            "for (unsigned slot = 0; slot < PSP_MEDIA_SURFACE_SLOTS; slot++)",
            submit)
        # The readers: the stage copy in either form, the draw that samples
        # the surface because the picture could not be staged, the software
        # scaler, and the seek-preview copy.
        session = without_comments(
            psp_media_session_sources())
        # Every release offers the decoder the window it just opened, and
        # always after the claim is dropped -- asking first could only ever be
        # refused by begin_write. Checked per function rather than by textual
        # adjacency, because the staged path now chooses between two kinds of
        # release and offers the window after either.
        for opening, closing in (
                ("void psp_media_present_texture_for(",
                 "static bool psp_media_refusal_reset_recover("),
                ("void psp_media_present_texture_finish(",
                 "size_t psp_media_feed_before_blocking("),
                ("void psp_media_present_texture_release(",
                 "bool psp_media_advance(\n    PspMediaSession")):
            body = session[session.index(opening):session.index(closing)]
            last_release = max(
                body.rfind("psp_media_present_release_claimed_surface(media)"),
                body.rfind("psp_media_finish_staged_surface(media)"))
            self.assertNotEqual(-1, last_release, opening)
            self.assertLess(
                last_release,
                body.rindex("psp_media_present_emit_after_release(media)"),
                opening)
        # A release names the slot it holds, and -1 means it holds none: an
        # unqualified release would free whichever slot was indexed first.
        claimed = session[
            session.index("static int psp_media_claimed_slot("):
            session.index("void psp_media_present_release_claimed_surface(")]
        self.assertIn("media->have_frame", claimed)
        self.assertIn("return -1", claimed)
        stage = session[
            session.index("void psp_media_present_texture_for("):
            session.index("bool psp_media_advance(")]
        self.assertEqual(
            2, stage.count("media_playback_borrow_video_slot(\n"))
        self.assertLess(
            stage.index("media_playback_borrow_video_slot(\n"),
            stage.index("psp_media_present_ge_stage_dma_submit("))
        # A claim the backend refused means the picture is gone, so the staged
        # path must not copy anyway.
        self.assertIn(
            "|| !media_playback_borrow_video_slot(\n",
            stage)
        # The synchronous stage copy completes before it returns, so the slot
        # is finished with and handed back rather than merely unleased.
        self.assertIn("psp_media_finish_staged_surface(media);", stage)
        finish = session[
            session.index("void psp_media_present_texture_finish("):
            session.index("size_t psp_media_feed_before_blocking(")]
        self.assertLess(
            finish.index("psp_media_present_ge_stage_dma_join()"),
            finish.index("psp_media_finish_staged_surface(media)"))
        # The staging re-copy reads the slot, so it happens before the slot is
        # handed back -- and a join that timed out keeps the claim, because
        # that is the one case where the controller may still be reading.
        self.assertLess(
            finish.index("psp_media_present_ge_stage_dma_recover("),
            finish.index("psp_media_finish_staged_surface(media)"))
        self.assertIn("if (joined) {", finish)
        self.assertIn("psp_media_present_release_claimed_surface(media);", finish)

    def test_only_the_staged_path_ends_a_claim_early(self):
        """Which release ends a claim is a property of the PATH, and getting
        it wrong is worth a whole soak either way.

        Holding every claim until a later one superseded it made the pipeline
        exactly one conversion per claim: the device measured hold-video
        frame:15683 unchanged at ~131/s and conversions stuck at 18.3/s with
        the second surface reserved and idle. The supersede rule was protecting
        re-presents -- and on the staged path there is nothing to protect,
        because a re-present samples the staged texture and skips the slot
        entirely. So the staged path hands the slot back at its join.

        The unstaged graphics-engine draw and the software scaler are the
        opposite: they sample the slot on every redraw, so their claim must
        outlive the frame. Freeing there is the silent substitution this whole
        design is answerable for -- the successor drawn under the claimed
        picture's identity, with every counter reporting a clean frame."""
        session = without_comments(
            psp_media_session_sources())
        # Exactly the two staged sites end a claim, and nothing else does.
        self.assertEqual(
            2, session.count("psp_media_finish_staged_surface(media);"),
            "only the synchronous stage copy and the async join may end a "
            "claim; a third caller is a path reading the slot after it was "
            "handed back to the Media Engine")
        # The fact the early release rests on, and the one that would make it
        # a silent substitution if it stopped being true: a present of a
        # picture that has already been staged does not read the slot at all.
        # The staging is the whole of the identity-guarded branch, and the
        # texture handed back outside it points at the staged copy.
        texture_for = session[
            session.index("void psp_media_present_texture_for("):
            session.index("static bool psp_media_refusal_reset_recover(")]
        guard = texture_for.index(
            "if (media->present_stage_identity != frame->identity) {")
        assign = texture_for.rindex("texture->pixels = staging;")
        self.assertLess(guard, assign)
        # Every read of the slot for staging is inside that branch.
        for read in ("psp_media_present_ge_stage_dma_submit(",
                     "psp_media_present_ge_stage_dma(",
                     "psp_media_present_stage("):
            self.assertLess(
                guard, texture_for.index(read), read)
            self.assertLess(
                texture_for.index(read), assign, read)
        staged = session[
            session.index("static void psp_media_finish_staged_surface("):
            session.index("void psp_media_present_texture_for(")]
        self.assertIn("media_playback_release_video_read(", staged)
        self.assertIn("media_playback_release_video_slot(", staged)
        self.assertLess(
            staged.index("media_playback_release_video_read("),
            staged.index("media_playback_release_video_slot("))
        # The paths that read the slot per draw keep their claim: they use the
        # lease-only release, which leaves the slot READING.
        unstaged = session[
            session.index("void psp_media_present_texture_release("):
            session.index("bool psp_media_advance(\n    PspMediaSession")]
        self.assertIn("psp_media_present_release_claimed_surface(media)", unstaged)
        self.assertNotIn("psp_media_finish_staged_surface", unstaged)
        advance = session[
            session.index("bool psp_media_advance(\n    PspMediaSession"):]
        self.assertNotIn("psp_media_finish_staged_surface", advance)
        # The software scaler reads the slot for its whole blit and never ends
        # a claim; the backend would refuse it anyway, but the call must not
        # be there to be refused.
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        self.assertNotIn("release_video_slot", runtime)
        # And the presenter is handed the session's OWN frame, which is what
        # makes "the slot this session is holding" and "the slot being staged"
        # the same slot. A presenter fed some other frame would end a claim
        # the session never made.
        self.assertIn(
            "psp_media_present_texture_for(\n"
            + " " * 8 + "psp_active_media, &plan, frame,", runtime)
        self.assertIn("&psp_active_media->frame,", runtime)
        # And the backend refuses a release that does not name the picture the
        # caller actually read, or a slot nobody is holding.
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        release = source[
            source.index("static bool psp_media_release_video_slot("):
            source.index("static bool psp_media_emit_pending_video(")]
        self.assertIn(
            "psp_media_slot_observe(state) != PSP_MEDIA_SLOT_READING",
            release)
        self.assertIn("state->generation != generation", release)
        self.assertIn(
            "atomic_load(&psp_media_surface_borrowed[slot]) != 0", release)
        self.assertIn(
            "psp_media_slot_publish(state, PSP_MEDIA_SLOT_FREE)", release)
        self.assertIn("backend->reading_slot = -1", release)
        # This release has never had a codec-job gate above it -- the
        # presenter calls it mid-frame, whatever the worker is doing -- so it
        # is the hand-back where the erase-then-publish order matters most:
        # the worker may be inside psp_media_slot_free_index right now, and
        # FREE is the word it is waiting for.
        self.assertLess(
            release.index("state->identity = 0"),
            release.index(
                "psp_media_slot_publish(state, PSP_MEDIA_SLOT_FREE)"),
            "a slot is erased before it is offered, never after")
        # The seek-preview copy is a reader like any other, and one that runs
        # at the moment the pipeline is most likely to move underneath it.
        preview = session[
            session.index("static bool psp_media_copy_preview("):
            session.index("static bool psp_media_seek_preview_ready(")
            if "static bool psp_media_seek_preview_ready(" in session
            else session.index("static bool psp_media_copy_preview(") + 4000]
        self.assertIn("media_playback_borrow_video_slot(", preview)
        self.assertIn("media_playback_end_auxiliary_video_read(", preview)
        # A present that failed with the rows in hand keeps its claim, because
        # the software scaler the caller falls back to reads the same surface.
        # No claim survives into the next frame's advance.
        advance = session[
            session.index("bool psp_media_advance(\n    PspMediaSession"):]
        self.assertLess(
            advance.index("psp_media_present_release_claimed_surface(media)"),
            advance.index("psp_media_pump_ranges(media)"))
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        video = runtime[
            runtime.index("static bool psp_present_media_frame_video("):
            runtime.index("static bool psp_media_video_surface_follow(")]
        self.assertLess(
            video.index("psp_media_present_ge_complete("),
            video.index("psp_media_present_texture_release("))
        # The software scaler reads the decoder's surface directly for the
        # whole of its blit, so it claims its slot too. It had no claim at all
        # while one surface and one writer made that survivable.
        software = runtime[
            runtime.index("static bool psp_present_media_frame("):
            runtime.index("static bool psp_present_media_frame_video(")]
        self.assertLess(
            software.index("media_playback_borrow_video_slot("),
            software.index("psp_media_present_software(&plan, frame, vram)"))
        self.assertLess(
            software.index("psp_media_present_software(&plan, frame, vram)"),
            software.index("media_playback_release_video_read("))

    def test_the_stage_copy_cannot_wedge_the_browser(self):
        """The copy runs on a worker above the interactive thread and the
        device measured it at 4.2ms at worst. Both of its unbounded waits were
        therefore invisible until they were not: a worker whose event wait
        failed left its loop without ever setting the done flag, and the join
        waited on that flag forever with no timeout, no recovery and no line
        in the log."""
        source = without_comments(
            (ROOT / "src/psp_media_present_ge.c").read_text(encoding="utf-8"))
        worker = source[
            source.index("static int psp_media_dma_worker_main("):
            source.index("static bool psp_media_dma_worker_start(")]
        # A worker that leaves its loop latches that, publishes a failure and
        # releases whoever is already waiting on this copy.
        self.assertIn("psp_media_dma_dead", worker)
        self.assertIn("psp_media_dma_done, PSP_MEDIA_DMA_WAKE", worker)
        join = source[
            source.index(
                "PspMediaPresentDmaJoin "
                "psp_media_present_ge_stage_dma_join(void)"):
            source.index(
                "bool psp_media_present_ge_stage_dma_recover(\n"
                "    int slot, uint32_t generation)")]
        self.assertIn("PSP_MEDIA_DMA_JOIN_TIMEOUT_US", join)
        self.assertNotIn("&bits, NULL)", join)
        self.assertIn("psp_media_dma_dead", join)
        # One unanswerable copy costs the session the overlap and nothing
        # else: every caller of submit already copies for itself on a refusal.
        submit = source[
            source.index("bool psp_media_present_ge_stage_dma_submit("):
            source.index("void psp_media_present_ge_stage_dma_stats(")]
        self.assertIn("psp_media_dma_dead", submit)
        # The copy records which picture it is copying, so the recovery can
        # refuse to repeat it for a different one. A join only fails after it
        # has waited, and the slot it read can hold a successor by then.
        self.assertIn("psp_media_dma_slot = slot", submit)
        self.assertIn("psp_media_dma_generation = generation", submit)
        recover = source[
            source.index(
                "bool psp_media_present_ge_stage_dma_recover(\n"
                "    int slot, uint32_t generation)"):
            source.index("bool psp_media_present_ge_stage_dma_submit(\n")]
        self.assertIn(
            "(unsigned) slot != psp_media_dma_slot", recover)
        self.assertIn(
            "generation != psp_media_dma_generation", recover)
        # And a display list that was started is never abandoned to run under
        # a caller that has fallen back to writing those rows itself.
        draw = source[
            source.index("bool psp_media_present_ge_submit(\n"):
            source.index("static void psp_media_present_probe_fill(")]
        self.assertLess(
            draw.index("sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT)"),
            draw.index('psp_media_present_ge_latch("display-list-exhausted")'))

    def test_a_batched_audio_job_never_outranks_video_or_the_worker(self):
        """Two AAC access units per job halves the round trips that were
        costing video the shared slot. It can only do that safely if the
        browser thread stages both -- the worker must never reach for a second
        unit that has not arrived, because that would put demux or network
        work on a thread that holds the one job slot -- and if a video access
        unit that could go in right now always wins.

        The soak this answers: audio owned 27.9s of the slot to video's 24.5s
        and refused video 1,474 times to video's own 314, while the firmware
        call was only about 1.1ms of an 8.8ms job."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        policy = without_comments(
            (ROOT / "src/media_backend_psp_policy.h").read_text(
                encoding="utf-8"))
        # The cap is a named constant with the reasoning beside it, not a
        # literal at the loop.
        self.assertIn("#define PSP_MEDIA_AUDIO_BATCH_MAXIMUM 2u", policy)
        job = source[
            source.index(
                "static MediaBackendResult psp_media_decode_staged_audio("):
            source.index(
                "static MediaBackendResult psp_media_drain_staged_video(")]
        # The worker decodes what is staged and nothing else: no demux, no
        # range read, no staging of its own.
        for forbidden in ("media_mp4_", "media_http_", "memcpy(backend->audio_staging",
                          "psp_media_ensure_packet_staging"):
            self.assertNotIn(forbidden, job)
        self.assertIn("backend->audio_staged_count", job)
        self.assertIn("PSP_MEDIA_AUDIO_BATCH_BUDGET_US", job)
        # A job that stops on its budget leaves the remainder staged rather
        # than dropping it, so the batch is only retired when it is finished.
        self.assertIn(
            "if (backend->audio_staged_index >= backend->audio_staged_count) {",
            job)
        # Every other guard is the browser thread's, checked BEFORE the job is
        # queued -- discovering them afterwards would mean a job already
        # holding the slot.
        submit = source[
            source.index("static MediaBackendResult psp_media_submit("):
            source.index("static MediaBackendResult psp_media_drain(")]
        self.assertIn("psp_media_audio_pending_should_wait(", source)
        # The video check is NOT here. Holding the first unit leaves the job
        # slot idle and so cannot delay video; asking it here made 60% of
        # audio jobs single on device. It lives on the decision that really
        # competes with video -- the credit that fetches the partner.
        self.assertNotIn("psp_media_video_admissible(", submit)
        # An access unit larger than the staging ceiling is refused rather
        # than written past the end of the slot it was given.
        self.assertIn("length > PSP_MEDIA_AUDIO_AU_BYTES", submit)
        # The deferral is bounded by a flush the pump always reaches, or a
        # unit held for a partner that never comes would never be decoded.
        advance = source[
            source.index("static bool psp_media_advance(void *opaque"):
            source.index("static bool psp_media_reset(")]
        self.assertIn("psp_media_flush_staged_audio(backend)", advance)
        # The staging is its own buffer. Sharing the packet staging is exactly
        # what a deferred unit cannot survive: the next video submission
        # overwrites it.
        self.assertIn("backend->audio_staging", source)
        video_stage = submit[
            submit.index("memcpy(backend->packet_staging, payload, length)"):
            submit.index("PSP_MEDIA_CODEC_KIND_VIDEO")]
        self.assertNotIn("audio_staging", video_stage)
        # ...and the audio branch never stages into the shared buffer, which
        # is the same statement from the other side.
        # The busy-job fast path may enqueue audio before the canonical audio
        # staging branch. Inspect the branch that performs native staging,
        # not the earlier bounded-copy admission path.
        audio_stage = submit[
            submit.rindex("if (sample->kind == MEDIA_MP4_TRACK_AUDIO)"):]
        self.assertNotIn("packet_staging", audio_stage)
        self.assertIn("psp_media_enqueue_pending_audio", submit)
        self.assertIn("PSP_MEDIA_AUDIO_PENDING_SLOTS", source)
        # And a seek does not carry a pre-seek access unit into a post-seek
        # job.
        reset = source[
            source.index("static bool psp_media_reset("):
            source.index("static bool psp_media_take_frame(")]
        self.assertIn("backend->audio_staged_count = 0u", reset)
        # The supply side. A staged unit only ever becomes a pair if the pump
        # offers its partner in the same visit, and the per-visit packet
        # budget is shared with video -- so a visit that spent its first
        # packet on video had nothing left, and the soak measured 1.32 blocks
        # per job against a ceiling of 2 for exactly that reason.
        wants = source[
            source.index("static bool psp_media_wants_paired_submit("):
            source.index(
                "static MediaBackendResult psp_media_queue_staged_audio(")]
        self.assertIn("MEDIA_MP4_TRACK_AUDIO", wants)
        # Re-asked, never remembered: a slot may have been claimed since the
        # first unit was staged, and if it has, the visit belongs to video.
        self.assertIn("psp_media_video_admissible(backend)", wants)
        self.assertIn("PSP_MEDIA_AUDIO_BATCH_MAXIMUM", wants)
        # It buys budget and never changes what is selected, so a video unit
        # that is due first is still submitted first.
        scheduler = without_comments(
            (ROOT / "src/media_backend.c").read_text(encoding="utf-8"))
        pump = scheduler[
            scheduler.index(
                "MediaPlaybackAdvanceResult "
                "media_playback_advance_bounded_cancelable("):
            scheduler.index("void media_playback_job_stats(")]
        self.assertIn("processed >= maximum_packets + paired_credit", pump)
        self.assertIn("MEDIA_PLAYBACK_MAXIMUM_PAIRED_SUBMITS", pump)
        # Granted only for a submit the backend took WITHOUT starting native
        # work; a queued unit already owns the decoder.
        self.assertIn("result == MEDIA_BACKEND_ACCEPTED", pump)
        self.assertLess(
            pump.index("selected_time = candidate"),
            pump.index("paired_credit++"),
            "the earliest-first selection has to stay ahead of the budget "
            "extension, or the extension would be choosing the track")
        # And the extension never fetches: an unbuffered partner takes the
        # same would-block exit as any other unbuffered sample.
        self.assertNotIn("wants_paired_submit", pump[:pump.index(
            "MediaBackendResult result = playback->backend.submit(")])

    def test_the_occupancy_instrumentation_leaves_the_hot_path_alone(self):
        """Telemetry that changes what it measures is worse than none. The
        occupancy ladder is counters, timestamp subtractions and one atomic
        load on paths that already run, and this pins the two properties that
        make that claim checkable: no decision anywhere reads a counter, and
        every ownership transition is still written exactly where it was.

        The second half is the one a reviewer cannot hold in their head. A
        dwell charge names the state being LEFT, so it must sit BEFORE the
        assignment; a pipeline sample describes the state arrived at, so it
        must sit AFTER. Both wrong-way-round versions still compile, still
        produce plausible numbers, and are wrong by exactly one transition."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        convert = source[
            source.index(
                "static int psp_media_emit_captured_picture(\n"
                "    PspMediaBackend *backend, unsigned write_slot, "
                "uint64_t epoch,\n"
                "    char *error, size_t error_size)\n{"):
            source.rindex(
                "static bool psp_media_validate_output_surface(\n"
                "    PspMediaBackend *backend, unsigned write_slot,\n"
                "    char *error, size_t error_size)\n{")]
        # Adjacency, not merely order: "somewhere before" is satisfied by the
        # charge belonging to the PREVIOUS transition, which is exactly the
        # off-by-one-transition bug this is meant to catch.
        for transition in (
                "psp_media_slot_publish(state, PSP_MEDIA_SLOT_ME_WRITING);",
                "psp_media_slot_publish(state, PSP_MEDIA_SLOT_READY);"):
            self.assertIn(
                "psp_media_slot_charge(backend, write_slot);\n"
                "    " + transition, convert,
                "the dwell charge names the state being left, so it belongs "
                "immediately before the transition and nowhere else")
        self.assertIn(
            "psp_media_slot_publish(state, PSP_MEDIA_SLOT_READY);\n"
            "    psp_media_slot_sample(backend);", convert,
            "the pipeline sample describes the state arrived at, so it "
            "belongs immediately after the transition")
        # The claim frees the slot it supersedes and charges the state it is
        # leaving, in that order, on the same slot.
        take = source[
            source.index("static bool psp_media_take_frame("):
            source.index("static bool psp_media_release_video_slot(")]
        self.assertIn(
            "psp_media_slot_charge(backend, previous);\n"
            "        psp_media_slot_publish(\n"
            "            &backend->slots[previous], PSP_MEDIA_SLOT_FREE);",
            take,
            "the dwell charge names the state being left, so it belongs "
            "immediately before the transition and nowhere else")
        # Nothing in the pipeline may consult a telemetry counter. Every one
        # of these is written on a hot path; a branch on any of them would
        # make the measurement part of the thing measured.
        for counter in ("slot_free_idle_us", "slot_no_free_us",
                        "video_csc_total_us", "worker_dispatch_total_us",
                        "batch_defer_free", "take_blocked_other_slot",
                        "job_audio_buckets", "slot_ready_total"):
            for guard in ("if (backend->stats.%s" % counter,
                          "if (stats->%s" % counter,
                          "while (backend->stats.%s" % counter):
                self.assertNotIn(guard, source)
        # The one field the worker publishes for the browser thread to read
        # across the completion handoff is written BEFORE the release store,
        # like every other field that crosses it.
        worker = source[
            source.index("static int psp_media_codec_thread("):
            source.index("static int psp_media_collect_codec_job(")]
        self.assertLess(
            worker.rindex("backend->codec_job_done_us = done_us"),
            worker.rindex(
                "&backend->codec_job_state, PSP_MEDIA_CODEC_JOB_DONE"))

    def test_the_prepared_codec_job_is_one_way_bounded_and_cancelled(self):
        """The rate experiment may overlap one staged AAC batch with an AVC
        job, but must not turn into a second decoder, a second video admission
        path, or accepted work that survives reset/teardown."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        pair = source[
            source.index("static bool psp_media_prepared_pair_allowed("):
            source.index("static MediaBackendResult psp_media_queue_codec_job(")]
        self.assertIn("active == PSP_MEDIA_CODEC_KIND_VIDEO", pair)
        self.assertIn("prepared == PSP_MEDIA_CODEC_KIND_AUDIO", pair)
        self.assertNotIn("active == PSP_MEDIA_CODEC_KIND_AUDIO", pair)
        prepare = source[
            source.index("static MediaBackendResult psp_media_prepare_staged_audio("):
            source.index("static void psp_media_cancel_prepared_job(")]
        self.assertIn("psp_media_prepared_try_publish", prepare)
        self.assertNotIn("packet_staging", prepare)
        worker = source[
            source.index("static bool psp_media_worker_chain_prepared("):
            source.index("static int psp_media_codec_thread(")]
        self.assertIn("PSP_MEDIA_CODEC_COMPLETION_EMPTY", worker)
        self.assertIn("PSP_MEDIA_CODEC_COMPLETION_READY", worker)
        self.assertIn("psp_media_prepared_worker_take_or_close", worker)
        ownership = without_comments(
            (ROOT / "src/psp_media_ownership.h").read_text(encoding="utf-8"))
        for state in ("PSP_MEDIA_CODEC_PREPARED_EMPTY",
                      "PSP_MEDIA_CODEC_PREPARED_READY",
                      "PSP_MEDIA_CODEC_PREPARED_CLAIMED",
                      "PSP_MEDIA_CODEC_PREPARED_CLOSED"):
            self.assertIn(state, ownership)
        reset = source[
            source.index("static bool psp_media_reset("):
            source.index("static bool psp_media_take_frame(")]
        destroy = psp_firmware_backend_destroy(source)
        self.assertLess(
            reset.index("psp_media_cancel_prepared_job(backend)"),
            reset.index("backend->session_epoch = psp_media_epoch_advance("))
        self.assertIn("psp_media_cancel_prepared_job(backend)", destroy)
        # And the phase tag is a relaxed load of a value nothing decides on.
        session = without_comments(
            psp_media_session_sources())
        self.assertIn(
            "media_psp_backend_set_loop_phase(PSP_MEDIA_LOOP_PHASE_PUMP)",
            session)
        self.assertIn(
            "media_psp_backend_set_loop_phase(PSP_MEDIA_LOOP_PHASE_VBLANK)",
            session)
        self.assertNotIn("if (psp_media_loop_phase", source)
        # The present phase is derived from the advance mode rather than
        # published a second time, so the two cannot fall out of step.
        self.assertNotIn("PSP_MEDIA_LOOP_PHASE_PRESENT)", session)

    def test_an_unjoinable_stage_copy_quarantines_both_its_addresses(self):
        """A join that times out did not stop the transfer -- it stopped
        waiting for it. The controller may still be READING the decoded slot
        and WRITING the staging texture, and the old path treated that as an
        ordinary failure: it dropped the borrow lease, repeated the copy into
        the same destination, and let both addresses be reused.

        Two silent substitutions follow, and neither is detectable downstream.
        A later claim or reset frees the source slot and the Media Engine
        converts over memory the transfer is still reading. And the shared
        staging destination is reused by a NEWER picture, which the abandoned
        transfer then overwrites with the previous one -- after the newer
        picture's signature was sampled, so the one check built to catch a
        wrong-pixels substitution provably cannot see this one.

        So the join reports three outcomes rather than two, and the third
        quarantines both addresses until the transfer is OBSERVED to end."""
        source = without_comments(
            (ROOT / "src/psp_media_present_ge.c").read_text(encoding="utf-8"))
        header = without_comments(
            (ROOT / "include/tilefinch/psp_media_present.h")
            .read_text(encoding="utf-8"))
        # A completion and an abandonment are different answers. A bool cannot
        # carry that, which is why this is an enum and why the API changed.
        for outcome in ("PSP_MEDIA_DMA_JOIN_SUCCESS",
                        "PSP_MEDIA_DMA_JOIN_COMPLETED_FAILURE",
                        "PSP_MEDIA_DMA_JOIN_TIMED_OUT_STILL_LIVE"):
            self.assertIn(outcome, header)
        self.assertIn(
            "PspMediaPresentDmaJoin psp_media_present_ge_stage_dma_join(void)",
            header)
        join = source[
            source.index(
                "PspMediaPresentDmaJoin "
                "psp_media_present_ge_stage_dma_join(void)"):
            source.index("bool psp_media_present_ge_stage_dma_quarantined(")]
        # The timeout arm is the only one that quarantines, and it records
        # both addresses -- the slot the copy read and the buffer it wrote.
        self.assertIn("psp_media_dma_quarantine_live = true", join)
        self.assertIn("psp_media_dma_quarantine_slot =", join)
        self.assertIn("psp_media_dma_quarantine_dst =", join)
        self.assertIn("PSP_MEDIA_DMA_JOIN_TIMED_OUT_STILL_LIVE", join)
        # A worker that answered is a completion whatever it answered: both
        # addresses are free, and the caller may repeat the copy.
        self.assertIn("PSP_MEDIA_DMA_JOIN_COMPLETED_FAILURE", join)
        # The quarantine is lifted by observing the completion, never by a
        # timer. Poll, never wait: this runs on frames with work to do.
        poll = source[
            source.index("bool psp_media_present_ge_stage_dma_quarantine_poll("):
            source.index(
                "bool psp_media_present_ge_stage_dma_quarantine_expired(")]
        self.assertIn("sceKernelPollEventFlag(", poll)
        self.assertNotIn("sceKernelWaitEventFlag(", poll)
        self.assertIn("psp_media_dma_quarantine_live = false", poll)
        self.assertIn("psp_media_dma_late_completions", poll)
        session = without_comments(
            psp_media_session_sources())
        finish = session[
            session.index("void psp_media_present_texture_finish("):
            session.index("size_t psp_media_feed_before_blocking(")]
        # The still-live arm does none of what the completed arm does. Each of
        # these three would be a use of an address the transfer still owns.
        self.assertIn("PSP_MEDIA_DMA_JOIN_TIMED_OUT_STILL_LIVE", finish)
        self.assertIn("media_playback_quarantine_video_slot(", finish)
        self.assertLess(
            finish.index("bool still_live ="),
            finish.index("psp_media_present_ge_stage_dma_recover("),
            "the recovery must be reachable only after the still-live case "
            "has been separated out; repeating the copy into a destination a "
            "live controller is writing is the defect this fixes")
        self.assertIn("if (!still_live) {", finish)
        # Staging into a quarantined destination is refused at the source.
        stage = session[
            session.index("void psp_media_present_texture_for("):
            session.index("static bool psp_media_refusal_reset_recover(")]
        self.assertIn(
            "psp_media_present_ge_stage_dma_quarantine_holds_staging(staging)",
            stage)
        # And the decoded slot is never handed back to its writer while the
        # quarantine stands -- at the claim that supersedes it, at the staged
        # release, and at the reader quiesce a seek or reset performs. That
        # last one is the whole reason a lease was not enough: the thread that
        # abandoned the transfer had already dropped the lease.
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        quiesce = backend[
            backend.index("static bool psp_media_surface_quiesce_readers("):
            backend.index("static void psp_media_surface_handshake_reset(")]
        self.assertIn("psp_media_surface_quarantined[slot]", quiesce)
        take = backend[
            backend.index("static bool psp_media_take_frame("):
            backend.index("static bool psp_media_release_video_slot(")]
        self.assertIn("psp_media_surface_quarantined[", take)
        release = backend[
            backend.index("static bool psp_media_release_video_slot("):
            backend.index("static bool psp_media_emit_pending_video(")]
        self.assertIn("psp_media_surface_quarantined[slot]", release)
        self.assertLess(
            release.index("psp_media_surface_quarantined[slot]"),
            release.index(
                "psp_media_slot_publish(state, PSP_MEDIA_SLOT_FREE)"))
        # A new session does not stop somebody else's DMA controller, so the
        # handshake reset must NOT clear it. Clearing it there would restore
        # the defect on the next seek.
        handshake = backend[
            backend.index("static void psp_media_surface_handshake_reset("):
            backend.index(
                "static int psp_media_emit_batch_into_free_slots(")]
        self.assertNotIn("psp_media_surface_quarantined", handshake)
        # The session owes a release when the transfer is finally seen to
        # end, and the refusal has to be withdrawn before the path it refuses
        # is taken.
        pump = session[
            session.index("static bool psp_media_pump_dma_quarantine("):
            session.index("bool psp_media_advance(\n    PspMediaSession")]
        self.assertLess(
            pump.index("media_playback_release_video_slot_quarantine("),
            pump.index("media_playback_release_video_slot("))
        # A transfer never observed to finish ends the session rather than
        # being assumed complete.
        self.assertIn(
            "psp_media_present_ge_stage_dma_quarantine_expired()", pump)
        self.assertIn("psp_media_raise_error(", pump)

    def test_a_refused_access_unit_says_which_side_produced_it(self):
        """The AU descriptor read back after a 0x80628002 refusal has twice
        shown esSize=0 with a valid esBuffer, which cannot separate "the
        bridge handed firmware an empty access unit" from "firmware consumed
        the elementary stream and zeroed the field on its way out". Those are
        opposite defects, so the descriptor is captured on the way in too."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        # Captured after the descriptor is filled and before it is consumed.
        fill = source.index("sceMpegGetAvcNalAu(")
        decode = source.index("sceMpegAvcDecode(", fill)
        between = source[fill:decode]
        self.assertIn("memcpy(au_before, backend->video_au", between)
        self.assertIn("event=au-before", source)

    def test_a_refused_access_unit_is_skipped_not_fatal(self):
        """au-before proved the bridge builds valid access units and firmware
        refuses particular content: esBuffer=0x09010000 with esSize=0x132,
        exactly the 306-byte packet, and sceMpegAvcDecode still answered
        0x80628002. Two sessions died on 306-byte packets at unrelated stream
        positions. Dropping one unit costs a frame; ending the session costs
        the session."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        # Classified structurally: the bridge filled the descriptor, the decode
        # stage refused, and this decoder has already produced pictures.
        self.assertIn("bool content_refusal", source)
        self.assertIn("PSP_MEDIA_CODEC_STAGE_AVC_DECODE", source)
        self.assertIn("refused_es_size != 0", source)
        self.assertIn("backend->stats.decoded_video_frames != 0", source)
        self.assertIn("psp_media_avc_refusal_survivable(", source)
        self.assertIn("event=au-refused", source)
        self.assertIn("event=au-refused-escalated", source)
        skip = source[
            source.index("bool content_refusal"):
            source.index("backend->stats.last_native_error = status;",
                         source.index("bool content_refusal"))]
        # A skipped unit is retired, not failed: nothing is torn down, and
        # last_native_error stays clean because every recovery path reads it
        # as "the decoder is broken".
        self.assertIn("return MEDIA_BACKEND_ACCEPTED", skip)
        self.assertNotIn("last_native_error", skip)
        self.assertNotIn("psp_media_log_failure", skip)
        # The refused unit's timestamp goes with it, or A/V sync walks by a
        # frame for the rest of the session.
        restore = source[
            source.index("if (status < 0) {"):
            source.index("bool content_refusal")]
        self.assertIn("codec_job_timestamps_before", restore)
        # And a decoded unit ends the run.
        self.assertIn("backend->video_refusals_consecutive = 0", source)

    def test_a_decoder_rebuild_reuses_every_engine_pool_pin(self):
        """The Media Engine pool is a bump allocator with no per-block free.
        Re-running the create ladder's allocations would take a second 4 MiB
        set, exhaust the pool, fall back to the newlib heap above the engine's
        addressable limit, and fail both codecs on structurally valid input --
        the exact defect the pool exists to prevent. A rebuild therefore hands
        the same buffers back to sceMpegCreate and never allocates."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        job = source[
            source.index(
                "static MediaBackendResult psp_media_run_recreate_job("):
            source.index("static int psp_media_codec_thread(")]
        # The pins, reused where they lie.
        self.assertIn("backend->mpeg_memory", job)
        self.assertIn("backend->mpeg_memory_bytes", job)
        self.assertIn("backend->ddr_memory", job)
        # And no allocation of any kind, by any of the three routes.
        self.assertNotIn("psp_media_alloc", job)
        self.assertNotIn("memalign", job)
        self.assertNotIn("malloc", job)
        # A size query is a check, never a reason to allocate: a larger answer
        # fails the reset rather than moving a pin.
        self.assertIn("sceMpegQueryMemSize", job)
        # sceMpegDelete and sceMpegCreate are unbounded Media Engine calls, so
        # they run on the codec worker like the teardown tail -- never on the
        # browser thread inside psp_media_reset.
        self.assertIn("sceMpegDelete", job)
        self.assertIn("sceMpegCreate", job)
        reset = source[
            source.index("static bool psp_media_reset("):
            source.index("static bool psp_media_take_frame(")]
        self.assertNotIn("sceMpegDelete", reset)
        self.assertNotIn("sceMpegCreate", reset)
        self.assertIn("PSP_MEDIA_CODEC_KIND_RECREATE", reset)
        # Both rebuild and in-place reset are worker jobs. No unbounded
        # firmware reset call is allowed to fall back to the browser thread.
        self.assertNotIn("sceMpegAvcDecodeFlush(", reset)
        self.assertNotIn("sceMpegInitAu(", reset)
        self.assertNotIn("sceAudiocodecInit(", reset)
        self.assertIn("PSP_MEDIA_CODEC_KIND_RESET", reset)
        worker_reset = source[
            source.index("static MediaBackendResult psp_media_run_reset_job("):
            source.index("static MediaBackendResult psp_media_run_recreate_job(")]
        self.assertIn("sceMpegAvcDecodeFlush(", worker_reset)
        self.assertIn("sceMpegInitAu(", worker_reset)
        self.assertIn("psp_media_reset_audio_program", worker_reset)
        self.assertIn("psp_media_reset_mode", reset)

    def test_a_refused_unit_keeps_the_timestamp_its_picture_will_claim(self):
        """The phantom five device runs were chasing. A content refusal is a
        unit whose elementary stream firmware had already ingested -- nonzero
        esSize going in, esSize=0 in the descriptor read back -- so the engine
        keeps a picture for it. The blanket rollback threw away the only
        timestamp that picture could pair with, and the counts disagreed by one
        from then on: a four-picture batch against three timestamps in no-touch
        mode, and a wedge four to six seconds after the reset in the other two
        modes. The rollback must therefore be undone for exactly that case."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        decode = source[
            source.index("static MediaBackendResult psp_media_decode_staged_video("):
            source.index("static MediaBackendResult psp_media_decode_staged_audio(")]
        # The submitted queue is captured before the rollback overwrites it.
        rollback = decode.index(
            "backend->video_timestamps =\n"
            "            backend->codec_job_timestamps_before;")
        capture = decode.index("timestamps_submitted =")
        self.assertLess(capture, rollback)
        # And restored on the survivable content-refusal path, before it
        # reports the unit accepted.
        skip = decode[decode.index("if (content_refusal) {"):]
        restore = skip.index("backend->video_timestamps = timestamps_submitted;")
        accepted = skip.index("return MEDIA_BACKEND_ACCEPTED;")
        self.assertLess(restore, accepted)
        # A unit firmware never took keeps the rollback: content_refusal is the
        # condition that says the elementary stream was ingested.
        self.assertIn("refused_es_size != 0", decode)

    def test_a_published_refusal_holds_new_jobs_until_a_stable_decision(self):
        """Two device runs wedged the Media Engine by submitting one or two
        more access units in the window between the worker publishing a
        survivable refusal and the browser thread processing it. Prepared audio
        exposed a second form: a running audio job made the stats snapshot
        unavailable, which was mistaken for no refusal and released the latch.
        Existing claimed audio may finish, but no new codec work may keep the
        slot from reaching the stable recovery decision."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        # Latched on the survivable refusal path, before the unit is reported
        # accepted -- so a thread that can see the completion sees the hold.
        skip = source[
            source.index("if (content_refusal) {"):
            source.index("backend->stats.last_native_error = status;",
                         source.index("if (content_refusal) {"))]
        self.assertIn("psp_media_video_refusal_hold, true", skip)
        # Consulted beside the stage-copy hold, not instead of it.
        submit = source[
            source.index("static MediaBackendResult psp_media_submit("):]
        gate = submit[:submit.index("backend->last_packet_bytes = length;")]
        self.assertIn(
            "psp_media_advance_may_submit_video(psp_media_advance_mode)", gate)
        self.assertIn("psp_media_video_refusal_hold", gate)
        # Already-claimed audio is allowed to complete, but neither a staged
        # flush nor a new audio submission may replace it while recovery needs
        # an idle stats snapshot.
        queued_audio = source[
            source.index("static MediaBackendResult psp_media_queue_staged_audio("):
            source.index("static void psp_media_flush_staged_audio(")]
        self.assertIn("psp_media_video_refusal_hold", queued_audio)
        audio_submit = submit[submit.index(
            "if (sample->kind == MEDIA_MP4_TRACK_AUDIO) {"):]
        self.assertIn("psp_media_video_refusal_hold", audio_submit)
        # Stats-unavailable is tri-state, not "no refusal": release is guarded
        # by a completed decision. With recovery disabled the deliberate
        # "continue" decision still becomes ready, so the hold cannot stick.
        session = without_comments(
            psp_media_session_sources())
        advance = session[session.index("bool psp_media_advance("):]
        release = advance.index("media_psp_backend_release_refusal_hold();")
        recovered = advance.index("if (refusal_recovered) return true;")
        self.assertLess(release, recovered)
        self.assertIn("if (refusal_decision_ready)", advance[:release])
        recovery = session[
            session.index("static bool psp_media_refusal_reset_recover("):
            session.index("bool psp_media_advance(")]
        unavailable = recovery.index(
            "if (!media_playback_backend_stats(media->playback, &stats))")
        ready = recovery.index("*decision_ready = true;", unavailable)
        self.assertLess(unavailable, ready)

    def test_one_refusal_rebuilds_at_a_later_random_access_point(self):
        """Hardware proved that a refused AU can hang the very next Decode.
        Stop submissions immediately, force a recreate even under NO_TOUCH,
        and use a later random-access point rather than replaying or omitting a
        predictive AU. The fallback may skip, but it cannot poison firmware."""
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        decode = backend[
            backend.index("static MediaBackendResult psp_media_decode_staged_video("):
            backend.index("static MediaBackendResult psp_media_decode_staged_audio(")]
        refusal = decode[decode.index("if (content_refusal) {"):]
        self.assertIn("&backend->video_refusal_dirty", refusal)
        submit = backend[
            backend.index("static MediaBackendResult psp_media_submit("):]
        dirty = submit.index("&backend->video_refusal_dirty")
        copy = submit.index("memcpy(backend->packet_staging, payload, length)")
        self.assertLess(dirty, copy)
        gate = submit[dirty:copy]
        self.assertIn("return MEDIA_BACKEND_WOULD_BLOCK", gate)
        audio = submit[submit.index(
            "if (sample->kind == MEDIA_MP4_TRACK_AUDIO) {"):]
        self.assertIn("video_refusal_dirty", audio)
        reset = backend[
            backend.index("static bool psp_media_reset("):
            backend.index("static bool psp_media_take_frame(")]
        self.assertIn("&backend->video_refusal_dirty, false", reset)
        self.assertIn("refusal_rebuild = atomic_load_explicit", reset)
        self.assertIn(
            "rebuild_requested = backend->have_video && (refusal_rebuild",
            reset)
        self.assertIn(
            "no_touch = backend->have_video && !refusal_rebuild", reset)

        session = without_comments(
            (ROOT / "src/psp_media_session.c").read_text(encoding="utf-8"))
        recovery = session[
            session.index("static bool psp_media_refusal_reset_recover("):
            session.index("bool psp_media_advance(")]
        self.assertIn("psp_media_refusal_resume_has_room", recovery)
        self.assertIn("media_playback_seek_after(", recovery)
        self.assertIn("media->clock_us = landing_us", recovery)
        self.assertNotIn("skip_video_sample", recovery)
        self.assertNotIn("refusal_catchup_target_us", recovery)

    def test_every_successful_avc_decode_completes_the_detail_transaction(self):
        """The established PSP raw-NAL contract queries DecodeDetail2 after
        every successful decode, including zero-picture AUs. Gating the detail
        call on picture output leaves firmware state pending across AUs."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        decode = source[
            source.index("static MediaBackendResult psp_media_decode_staged_video("):
            source.index("static MediaBackendResult psp_media_decode_staged_audio(")]
        query = decode.index("psp_media_query_avc_detail(")
        raw_probe = decode.index("if (backend->raw_nal_probe_pending)")
        picture_gate = decode.index("if (backend->video_status > 0)")
        self.assertLess(query, raw_probe)
        self.assertLess(query, picture_gate)
        self.assertIn("backend, backend->video_status, &detail", decode[query:])

    def test_an_unpairable_picture_is_recovered_not_fatal(self):
        """One unpairable picture ended a run the Media Engine had not failed.
        The cause is fixed at the source, but the invariant is bounded the way
        a refusal is bounded rather than being immediately fatal."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        capture = source[
            source.index("static int psp_media_capture_avc_pictures("):
            source.index("static int psp_media_convert_avc_picture(")]
        self.assertIn("psp_media_unpaired_pictures(", capture)
        # Same escalation rule as refusals, not a bespoke one.
        self.assertIn("psp_media_avc_refusal_survivable(", capture)
        self.assertIn("event=batch-overrun", capture)
        # Counted apart from refusals: the session watches the refusal total to
        # decide whether to reposition, and an overrun is recovered in place.
        self.assertIn("video_batch_overruns_total", capture)
        self.assertNotIn("video_refusals_total", capture)

    def test_a_no_touch_reposition_says_nothing_to_firmware(self):
        """Mode 2 is the reference's shape: PMPlayer Advance issues zero
        sceMpeg calls on a seek, calls sceMpegInitAu once per file, and has no
        sceMpegAvcDecodeFlush in its source at all. So the no-touch branch must
        reach neither firmware call, must leave decoder_primed alone so the
        next unit goes as mode 0 rather than mode 3, and must not re-init the
        AAC program -- while everything that is purely ours (the in-flight job,
        the audio output quiesce, our queues) still runs."""
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        reset = source[
            source.index("static bool psp_media_reset("):
            source.index("static bool psp_media_take_frame(")]
        # Primed is cleared only when something was actually taken away.
        self.assertIn(
            "if (refusal_rebuild\n"
            "        || psp_media_reset_mode != PSP_MEDIA_RESET_MODE_NO_TOUCH)\n"
            "        backend->decoder_primed = false;",
            reset)
        # The audio re-init is admitted only through a worker job when the
        # no-touch branch is false, never called inline from reset.
        self.assertIn("bool needs_worker = !no_touch", reset)
        self.assertNotIn("sceAudiocodecInit(", reset)
        # The quiesce is not optional in any mode: the demuxer and the packet
        # staging are repositioned under a decoder that may still be reading
        # them, so an in-flight job is always collected first.
        quiesce = reset[:reset.index("bool no_touch")]
        self.assertIn("psp_media_collect_codec_job", quiesce)
        self.assertIn("PSP_MEDIA_CODEC_QUIESCE_WAIT_US", quiesce)
        collect_failure = quiesce[
            quiesce.index("if (psp_media_collect_codec_job("):
            quiesce.index("audio_resetting")]
        self.assertIn("backend->admissions_closed = false", collect_failure)
        # Our own queues are still dropped -- a pre-seek timestamp must never
        # pair with a post-seek picture.
        self.assertIn("backend->video_timestamps.count = 0", quiesce)
        self.assertIn("backend->video_picture_count = 0", quiesce)
        # And every decoded-output slot is discarded the same way in every
        # mode -- but only after both the Media Engine writer and the readers
        # have provably stopped, because FREE is permission for the engine to
        # write there and a bump-pool reset can alias a slot a timed-out
        # writer is still inside.
        self.assertIn(
            "psp_media_slot_charge(backend, slot);\n"
            "        psp_media_slot_publish(&backend->slots[slot], PSP_MEDIA_SLOT_FREE);",
            quiesce)
        self.assertIn("backend->slots[slot].generation++", quiesce)
        self.assertLess(
            quiesce.index("psp_media_surface_quiesce_readers("),
            quiesce.index(
                "psp_media_slot_publish(&backend->slots[slot], PSP_MEDIA_SLOT_FREE)"))
        self.assertLess(
            quiesce.index("PSP_MEDIA_CODEC_QUIESCE_WAIT_US"),
            quiesce.index("psp_media_surface_quiesce_readers("))
        # The epoch moves before the quiesce, so a writer that never comes
        # back cannot publish a picture of the stream being left as though it
        # belonged to the one arriving.
        self.assertLess(
            quiesce.index("backend->session_epoch = psp_media_epoch_advance("),
            quiesce.index("PSP_MEDIA_CODEC_QUIESCE_WAIT_US"))
        # And admissions stop first of all, so everything after it is
        # reasoning about a pipeline nothing is still feeding.
        self.assertLess(
            quiesce.index("backend->admissions_closed = true"),
            quiesce.index("backend->session_epoch = psp_media_epoch_advance("))
        # A quiesce that timed out frees nothing. Both bounded waits bail out
        # before the discard loop.
        for failure in ("codec-quiesce-reset", "surface-quiesce-reset"):
            self.assertLess(
                quiesce.index(failure),
                quiesce.index(
                    "psp_media_slot_publish(&backend->slots[slot], PSP_MEDIA_SLOT_FREE)"))

    def test_a_feed_report_says_how_long_it_was_playing(self):
        """Every counter in the feed line samples the playing window and
        nothing else. A cycle read a 0.75-second window's 136 units as a
        session-long feed collapse, because playback had died 0.75s after the
        play press and the remaining 116 seconds sat on a failed panel."""
        source = without_comments(
            psp_media_session_sources())
        report = source[
            source.index("void psp_media_telemetry_report_feed("):
            source.index("void psp_media_pipeline_destroy(")]
        self.assertIn("playing-frames=%zu", report)
        self.assertIn("paused-frames=%zu", report)
        self.assertIn("media->advance_periods", report)

    def test_validation_reports_do_not_perturb_live_playback(self):
        """The PSPLink host0 writer can stop the browser thread for hundreds
        of milliseconds. Playback retains cumulative counters and samples
        health into RAM, but the expensive report is emitted at teardown."""
        session = without_comments(
            psp_media_session_sources())
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        self.assertIn(
            'psp_media_telemetry_report_feed(media, "teardown")', session)
        self.assertNotIn('psp_media_report_feed(media, "playing")', session)
        self.assertNotIn(
            'tilefinch-media-stability: event=sample', main)
        self.assertIn('UINT64_C(250000)', main)
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        self.assertNotIn("psp_media_report_audio_output(backend, false)", backend)
        self.assertIn("psp_media_report_audio_output_final(backend)", backend)

    def test_feed_report_samples_both_clocks_before_host_io(self):
        source = without_comments(
            (ROOT / "src/psp_media_telemetry.c").read_text(encoding="utf-8"))
        report = source[source.index("void psp_media_telemetry_report_feed("):]
        self.assertLess(report.index("media_playback_audio_cursor_us("),
                        report.index("printf("))

    def test_media_close_accounts_for_the_last_claim_and_skew_transients(self):
        """An intentional close is a terminal funnel outcome, while seek and
        priming skew must not masquerade as steady playback drift."""
        session = without_comments(psp_media_session_sources())
        destroy = session[
            session.index("void psp_media_pipeline_destroy("):
            session.index("void psp_media_prepare_route(")]
        self.assertLess(
            destroy.index("media_playback_note_frame_quiesced"),
            destroy.index('psp_media_telemetry_report_feed(media, "teardown")'))

        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        verdict = backend[
            backend.index("void media_psp_backend_note_frame_quiesced("):
            backend.index("void media_psp_backend_note_stage_signature(")]
        self.assertIn("psp_media_claims_dropped", verdict)
        self.assertIn("psp_media_claims_quiesced", verdict)
        self.assertIn("psp_media_display_key", verdict)

        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        self.assertIn("pipeline-skips=%zu", main)
        self.assertIn("display-drops=%zu", main)
        self.assertIn("quiesce-drops=%zu", main)
        self.assertIn("steady-max-av-skew=%lluus", main)
        sampler = main[
            main.index("psp_sample_media_stability("):
            main.index("psp_media_stability_schedule_seeks(")]
        self.assertIn("PSP_MEDIA_STABILITY_STEADY_DELAY_US", sampler)
        self.assertIn("PSP_MEDIA_SESSION_PLAYING", sampler)
        self.assertIn("presentation_preroll_audio_held", sampler)
        self.assertIn("media_playback_audio_cursor_us(", sampler)
        self.assertNotIn("audio_output_blocks * PSP_MEDIA_AUDIO_SAMPLES", sampler)

        discard = backend[
            backend.index("static size_t psp_media_discard_video_before("):
            backend.index("static bool psp_media_release_video_slot(")]
        self.assertIn("stats.discarded_seek_video_frames++", discard)
        self.assertNotIn("stats.dropped_video_frames++", discard)

    def test_first_frame_player_surface_is_black_not_the_page(self):
        source = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        present = source[
            source.index("bool psp_present_internal("):
            source.index("static unsigned psp_local_hour(")]
        self.assertIn("if (media_visible && !media_replaces_page)", present)
        black = present[
            present.index("if (media_visible && !media_replaces_page)"):
            present.index("} else if (!media_replaces_page")]
        self.assertIn("memset(", black)
        self.assertNotIn("memcpy(", black)

    def test_fullscreen_media_never_composites_page_chrome(self):
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        present = runtime[
            runtime.index("bool psp_present_internal("):
            runtime.index("static unsigned psp_local_hour(")]
        media_overlay = present.index("if (media_visible) {")
        page_overlay = present.index("} else if (ui != NULL) {")
        self.assertLess(media_overlay, page_overlay)
        self.assertIn(
            "psp_ui_media_composite_with_preview(",
            present[media_overlay:page_overlay])
        self.assertIn(
            "psp_ui_composite(",
            present[page_overlay:])

        preview = without_comments(
            (ROOT / "src/psp_ui_preview.c").read_text(encoding="utf-8"))
        guard = preview.index("if (!preview_media_mode(mode)) {")
        media = preview.index("if (preview_media_mode(mode)) {", guard)
        self.assertIn("psp_ui_composite(", preview[guard:media])
        self.assertNotIn("psp_ui_composite(", preview[media:])

    def test_private_raw_avc_au_is_initialized_at_its_allocated_extent(self):
        source = (ROOT / "src/media_backend_psp.c").read_text(
            encoding="utf-8")
        # The size lives with the pool the AU object is carved out of.
        self.assertIn(
            "#define PSP_MEDIA_VIDEO_AU_BYTES 64u",
            (ROOT / "src/media_backend_psp_pool.h").read_text(
                encoding="utf-8"))
        self.assertNotIn(
            "memset(backend->video_au, 0xFF, sizeof(*backend->video_au))",
            source,
            "The raw bridge consumes PMPlayer's 64-byte private AU object, "
            "not only PSPSDK's smaller public prefix.")
        self.assertGreaterEqual(
            source.count(
                "memset(backend->video_au, 0xFF, "
                "PSP_MEDIA_VIDEO_AU_BYTES)"),
            2,
            "Creation and seek reset must both initialize the private AU "
            "tail before sceMpegInitAu.")

    def test_media_audio_release_is_bounded_and_owns_dma_lifetime(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        self.assertEqual(1, source.count("sceAudioSRCChRelease("))
        self.assertEqual(1, source.count("sceAudioChRelease("))
        self.assertIn("PSP_MEDIA_AUDIO_CHANNEL_DRAIN_WAIT_US", source)
        self.assertIn("event=audio-quarantine", source)
        self.assertLess(
            source.index("event=audio-quarantine"),
            source.index("psp_media_release(backend->audio_queue)"),
            "A failed channel release must retain the PCM queue which real "
            "firmware may still own.")

    def test_raw_avc_descriptor_cache_ownership_is_explicit(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        bridge = source[source.index("status = sceMpegGetAvcNalAu("):
                        source.index("if (status < 0) {",
                                     source.index("status = sceMpegGetAvcNalAu("))]
        before = source[:source.index("status = sceMpegGetAvcNalAu(")]
        self.assertIn("sceKernelDcacheWritebackInvalidateRange(",
                      before[-500:])
        self.assertIn("sceKernelDcacheWritebackInvalidateAll();",
                      before[-500:])
        self.assertLess(
            before.rindex("sceKernelDcacheWritebackInvalidateRange("),
            before.rindex("sceKernelDcacheWritebackInvalidateAll();"))
        self.assertIn("sceKernelDcacheWritebackRange(", bridge)
        self.assertLess(bridge.index("sceKernelDcacheWritebackRange("),
                        bridge.index("status = sceMpegAvcDecode("))

    def test_native_codec_calls_run_off_the_browser_thread(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        worker = source[
            source.index("static int psp_media_codec_thread("):
            source.index("static int psp_media_collect_codec_job(")]
        self.assertIn("psp_media_decode_staged_video(", worker)
        self.assertIn("psp_media_decode_staged_audio(", worker)
        submit = source[
            source.index("static MediaBackendResult psp_media_submit("):
            source.index("static MediaBackendResult psp_media_drain(")]
        self.assertNotIn("sceMpegAvcDecode(", submit)
        self.assertNotIn("sceAudiocodecDecode(", submit)
        # Video queues directly; audio queues through the staged-batch helper,
        # which is still a queue to the worker and not a firmware call here.
        self.assertGreaterEqual(
            submit.count("psp_media_queue_codec_job(")
            + submit.count("psp_media_queue_staged_audio("), 2)
        queue_audio = source[
            source.index(
                "static MediaBackendResult psp_media_queue_staged_audio("):
            source.index("static void psp_media_flush_staged_audio(")]
        self.assertIn("psp_media_queue_codec_job(", queue_audio)
        self.assertNotIn("sceAudiocodecDecode(", queue_audio)
        drain = source[
            source.index("static MediaBackendResult psp_media_drain("):
            source.index("static bool psp_media_advance(")]
        self.assertNotIn("sceMpegAvcDecodeStop(", drain)
        self.assertIn("PSP_MEDIA_CODEC_KIND_DRAIN", drain)
        self.assertIn(
            "TILEFINCH_PSP_THREAD_PRIORITY_CODEC, 32u * 1024u", source)
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        self.assertIn(
            "PSP_MAIN_THREAD_PRIORITY("
            "TILEFINCH_PSP_THREAD_PRIORITY_BROWSER);", main)
        # And where in the band it sits, which is the whole contract. Above
        # the browser thread: at 0x21 a submitted job could not start until
        # that thread next blocked, so every job paid its time-to-next-block
        # and submissions stuck near 40 a second with the worker a third
        # busy. Below the copy and audio-output workers: their work is
        # microseconds and must not queue behind a codec job that is
        # milliseconds inside firmware.
        priorities = without_comments(
            (ROOT / "include/tilefinch/psp_threads.h").read_text(
                encoding="utf-8"))
        def priority(name):
            match = re.search(
                rf"TILEFINCH_PSP_THREAD_PRIORITY_{name}"
                r"\s+(0x[0-9a-fA-F]+)", priorities)
            self.assertIsNotNone(match)
            return int(match.group(1), 16)
        codec_priority = priority("CODEC")
        presenter = without_comments(
            (ROOT / "src/psp_media_present_ge.c").read_text(encoding="utf-8"))
        self.assertIn(
            "psp_media_dma_worker_main,\n"
            "        TILEFINCH_PSP_THREAD_PRIORITY_DMA", presenter)
        self.assertIn('"tilefinch_media_audio", psp_media_audio_thread,\n'
                      "                TILEFINCH_PSP_THREAD_PRIORITY_AUDIO",
                      source)
        self.assertLess(priority("DMA"), codec_priority)
        self.assertLess(priority("AUDIO"), codec_priority)
        self.assertLess(codec_priority, priority("TRANSPORT"))
        self.assertLess(priority("TRANSPORT"), priority("BROWSER"))
        self.assertLess(
            priority("BROWSER"), priority("TRANSPORT_SETUP"))
        self.assertLess(priority("BROWSER"), priority("CLOCK"))
        self.assertLess(priority("BROWSER"), priority("IMAGE_DECODE"))

        image = without_comments(
            (ROOT / "src/image.c").read_text(encoding="utf-8"))
        image_decode = without_comments(
            (ROOT / "src/image_decode.c").read_text(encoding="utf-8"))
        image_svg_decode = without_comments(
            (ROOT / "src/image_svg_decode.c").read_text(encoding="utf-8"))
        self.assertNotIn("NANOSVG_IMPLEMENTATION", image)
        self.assertNotIn("NANOSVGRAST_IMPLEMENTATION", image)
        self.assertIn("NANOSVG_IMPLEMENTATION", image_svg_decode)
        self.assertIn("NANOSVGRAST_IMPLEMENTATION", image_svg_decode)
        self.assertIn(
            "TILEFINCH_PSP_THREAD_PRIORITY_IMAGE_DECODE", image_decode)
        self.assertIn("IMAGE_DECODE_WORKER_QUEUED", image_decode)
        self.assertIn("IMAGE_DECODE_WORKER_RUNNING", image_decode)
        self.assertIn("IMAGE_DECODE_WORKER_READY", image_decode)
        self.assertIn("images_decode_worker_shutdown", image_decode)
        busy = image_decode[
            image_decode.index("bool image_decode_busy(void)"):
            image_decode.index("static void *image_arena_malloc(")]
        self.assertIn("atomic_load_explicit(&decode_gate", busy)
        self.assertNotIn("atomic_compare_exchange", busy)
        self.assertNotIn("atomic_flag_test_and_set", busy)
        finish_pending = image[
            image.index("static bool finish_one_pending("):
            image.index("static void cancel_pending(")]
        self.assertLess(
            finish_pending.index("if (image_decode_busy()) continue;"),
            finish_pending.index("fetch_scheduler_take("),
            "A contended decoder must leave the completed image response "
            "in scheduler ownership for a later pump.")

        transport = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        self.assertIn(
            "TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT", transport)
        self.assertIn(
            "TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT_SETUP", transport)
        self.assertIn(
            "fetch_background_perform_has_setup_work()", transport)
        self.assertIn("sceKernelChangeThreadPriority(", transport)

    def test_stuck_codec_worker_is_bounded_and_quarantined(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        destroy = psp_firmware_backend_destroy(source)
        self.assertIn("PSP_MEDIA_CODEC_QUIESCE_WAIT_US", destroy)
        self.assertIn(
            "PSP_MEDIA_CODEC_QUIESCE_WAIT_US 250000u", source)
        self.assertIn("event=codec-quarantine", destroy)
        codec_start = destroy.index("if (backend->codec_thread >= 0)")
        codec_teardown = destroy[
            codec_start:
            destroy.index("if (backend->codec_event >= 0)", codec_start)]
        self.assertNotIn("sceKernelTerminateDeleteThread", codec_teardown)
        self.assertIn("psp_thread_observe(", codec_teardown)
        self.assertIn("codec_emergency_event_delete", destroy)
        self.assertLess(
            destroy.index("event=codec-quarantine"),
            destroy.index("psp_media_release(backend->surfaces[slot])"))
        suspend = source[
            source.index("void media_psp_backend_system_suspend("):
            source.index("static const char *psp_media_module_failure_code")]
        self.assertIn("if (psp_media_backend_is_quarantined)", suspend)

    def test_media_engine_teardown_is_bounded_on_the_codec_worker(self):
        # sceMpegDelete and sceMpegFinish are unbounded firmware calls. On
        # hardware a rejected stream left the Media Engine wedged and the
        # main-thread teardown tail hung the app forever after every other
        # step had completed. They must run on the codec worker, where the
        # existing bounded wait and quarantine apply.
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        job = source[
            source.index(
                "static MediaBackendResult psp_media_run_teardown_job("):
            source.index("static int psp_media_codec_thread(")]
        for call in ("sceAudiocodecReleaseEDRAM(", "sceMpegDelete(",
                     "sceMpegFinish()"):
            self.assertIn(call, job)
        worker = source[
            source.index("static int psp_media_codec_thread("):
            source.index("static int psp_media_collect_codec_job(")]
        self.assertIn("PSP_MEDIA_CODEC_KIND_TEARDOWN", worker)
        self.assertIn("psp_media_run_teardown_job(", worker)
        destroy = psp_firmware_backend_destroy(source)
        for call in ("sceMpegDelete(", "sceMpegFinish()",
                     "sceAudiocodecReleaseEDRAM("):
            self.assertNotIn(call, destroy)
        self.assertIn("PSP_MEDIA_CODEC_KIND_TEARDOWN", destroy)
        self.assertIn("event=mpeg-quarantine", destroy)
        self.assertIn("mpeg-teardown=%s", destroy)
        # In-flight job quiesce, the teardown job itself, and the worker join
        # are three separately bounded waits.
        self.assertGreaterEqual(
            destroy.count("PSP_MEDIA_CODEC_QUIESCE_WAIT_US"), 3)
        # The audio DMA owner has to be gone before the Media Engine tail, and
        # the worker may only be stopped after it has run that tail.
        self.assertLess(
            destroy.index("PSP_MEDIA_AUDIO_CHANNEL_DRAIN_WAIT_US"),
            destroy.index("PSP_MEDIA_CODEC_KIND_TEARDOWN"))
        self.assertLess(
            destroy.index("PSP_MEDIA_CODEC_KIND_TEARDOWN"),
            destroy.index(
                "backend->codec_thread, PSP_MEDIA_CODEC_QUIESCE_WAIT_US"))
        tail = destroy[destroy.index("event=mpeg-quarantine"):]
        quarantined = tail.index("psp_media_quarantine();")
        released = tail.index("psp_media_release(backend->surfaces[slot])")
        self.assertLess(quarantined, released)
        self.assertLess(tail.index("return;", quarantined), released)

    def test_psp_reports_print_64_bit_counters_with_llu(self):
        # The browser EBOOT routes printf through psp_log_printf, which has no
        # format attribute, and psp-browser-script is not built with -Wall, so
        # the compiler says nothing about a mismatched specifier here. On MIPS
        # o32 an 8-byte vararg is 8-byte aligned: printing a uint64_t with %zu
        # consumed half a slot, so `declarations=` printed uninitialized stack
        # (0xDEADBEEF on device) and every later specifier on the line took the
        # wrong argument.
        source = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        # Scoped to the stylesheet counters the compact report prints. A
        # repository-wide name set collides with same-named 32-bit fields in
        # other structs, and a full-EBOOT sweep for this class is a compiler
        # job: build psp_script_main.c with -Wall after giving psp_log_printf
        # a printf format attribute.
        wide = set(re.findall(
            r"^\s*uint64_t\s+(\w+);",
            (ROOT / "include/tilefinch/style.h").read_text(encoding="utf-8"),
            re.M))
        self.assertIn("diagnostic_declarations", wide)
        specifier = re.compile(
            r"%[-+ #0]*[0-9.*]*(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGaAcsp%]")
        offenders = []
        for arguments in call_arguments(source, "printf"):
            parts = split_arguments(arguments)
            if len(parts) < 2 or not parts[0].lstrip().startswith('"'):
                continue
            found = [s for s in specifier.findall(parts[0]) if s != "%%"]
            # A call this crude parser cannot line up is skipped rather than
            # guessed at; the mismatch class it exists for is unambiguous.
            if len(found) != len(parts) - 1:
                continue
            for used, argument in zip(found, parts[1:]):
                field = argument.rsplit(".", 1)[-1].rsplit("->", 1)[-1].strip()
                if field not in wide or "unsigned long long" in argument:
                    continue
                offenders.append((field, used))
        self.assertEqual(offenders, [])

    def test_avc_bridge_telemetry_costs_a_shipping_build_nothing(self):
        source = (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8")
        decode = source[
            source.index(
                "static MediaBackendResult psp_media_decode_staged_video("):
            source.index(
                "static MediaBackendResult psp_media_decode_staged_audio(")]
        event = decode.index("event=avc-bridge-submit")
        guard = decode.rindex(
            "#if defined(TILEFINCH_PSP_VALIDATION_LOG)", 0, event)
        end = decode.index("#endif", guard)
        block = decode[guard:end]
        self.assertIn("event=avc-bridge-submit", block)
        self.assertIn("nal.nal_prefix_size", block)
        self.assertIn("nal.nal_size", block)
        self.assertIn("nal.sps_size", block)
        self.assertIn("nal.pps_size", block)
        # The hex formatting, not only the log call, has to be compiled out:
        # psp_media_log is already a no-op macro in a shipping build. The
        # formatter is shared with the post-failure dumps, so it lives in the
        # file-scope logging block -- which is itself guarded -- and every use
        # of it in the decode path stays inside a guard of its own.
        self.assertIn("psp_media_hex_bytes(", block)
        logging = source[
            source.index("#if defined(TILEFINCH_PSP_VALIDATION_LOG)"):
            source.index("#define psp_media_log(...) ((void) 0)")]
        self.assertIn("static void psp_media_hex_bytes(", logging)
        self.assertIn("static void psp_media_hex_words(", logging)
        self.assertIn("psp_media_hex_digits", logging)
        outside = decode
        while "#if defined(TILEFINCH_PSP_VALIDATION_LOG)" in outside:
            start = outside.index("#if defined(TILEFINCH_PSP_VALIDATION_LOG)")
            outside = (outside[:start]
                       + outside[outside.index("#endif", start):])
        self.assertNotIn("psp_media_hex", outside)
        # It also stays ahead of the cache maintenance, so publishing the MPEG
        # workspace remains the last thing before the firmware call.
        self.assertLess(
            end, decode.index("sceKernelDcacheWritebackInvalidateAll();"))

    def test_rejected_wide_decoder_program_is_latched_for_the_process(self):
        # A device that rejects the wide program (create mode 5 / ME boot
        # type 1) at the first access unit rejects it for every video. Without
        # a latch each new 360p open repeats the whole resolve/demux/create/
        # prime round trip before the 240p ladder can recover it.
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        decode = backend[
            backend.index(
                "static MediaBackendResult psp_media_decode_staged_video("):
            backend.index(
                "static MediaBackendResult psp_media_decode_staged_audio(")]
        self.assertIn("psp_media_wide_program_rejected_by(", decode)
        self.assertIn("event=wide-program-rejected", decode)
        self.assertIn("PSP_MEDIA_CODEC_STAGE_AVC_BRIDGE", decode)
        self.assertIn("PSP_MEDIA_CODEC_STAGE_AVC_DECODE", decode)
        # decoder_primed is set by a successful bridge call, so the admission
        # verdict has to be sampled before the bridge is entered.
        self.assertLess(
            decode.index("primed_before = backend->decoder_primed"),
            decode.index("status = sceMpegGetAvcNalAu("))
        self.assertIn(
            "bool media_psp_backend_wide_program_rejected(void)", backend)
        # The latch is memory-only; nothing may persist it to the profile.
        self.assertNotIn(
            "psp_media_wide_program_rejected",
            (ROOT / "src/browser_profile.c").read_text(encoding="utf-8"))
        session = without_comments(
            psp_media_session_sources())
        chooser = session[
            session.index(
                "BrowserYoutubeQuality psp_media_open_quality("):
            session.index("bool psp_media_offline_route(")]
        self.assertIn("psp_media_admitted_quality(", chooser)
        self.assertIn("media_psp_backend_wide_program_rejected()", chooser)
        self.assertIn("wide-program-rejected", chooser)
        route = session[
            session.index("static void psp_media_prepare_route_kind("):]
        self.assertIn("psp_media_open_quality(media)", route)
        self.assertNotIn(
            "browser_profile_youtube_quality(media->profile)", route)

    def test_wide_decoder_program_has_a_fail_closed_override(self):
        # The hardware promotion gate made wide the absent-config default.
        # Decoder admission still requires an enabled program, so explicit
        # off and the process-local rejection latch retain the 240p escape.
        policy = without_comments(
            (ROOT / "src/media_backend_psp_policy.h").read_text(
                encoding="utf-8"))
        decoder = policy[
            policy.index("static inline bool psp_media_decoder_policy("):
            policy.index(
                "static inline bool psp_media_wide_program_rejected_by(")]
        self.assertIn("bool wide_program_enabled", decoder)
        self.assertIn("if (wide && !wide_program_enabled) return false;",
                      decoder)
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        self.assertIn(
            "void media_psp_backend_set_wide_program(const char *name)",
            backend)
        setter = backend[
            backend.index(
                "void media_psp_backend_set_wide_program(const char *name)"):
            backend.index("int media_psp_backend_wide_program(void)")]
        self.assertIn("psp_media_wide_program_configured(name)", setter)
        create = psp_firmware_backend_create(backend)
        clamp = create.index("event=wide-program-clamped")
        self.assertIn("psp_media_wide_program_required(", create)
        self.assertLess(clamp, create.index("psp_media_boot_media_engine("))
        self.assertLess(clamp, create.index("sceMpegQueryMemSize("))
        session = without_comments(
            psp_media_session_sources())
        chooser = session[
            session.index(
                "BrowserYoutubeQuality psp_media_open_quality("):
            session.index("bool psp_media_offline_route(")]
        self.assertIn("media_psp_backend_wide_program()", chooser)
        self.assertIn("psp_media_wide_program_enabled(", chooser)
        # The knob is a boot-configuration override, not a profile setting:
        # nothing may persist it beside the ordinary preferences.
        self.assertNotIn(
            "experimental_wide_video",
            (ROOT / "src/browser_profile.c").read_text(encoding="utf-8"))
        boot = without_comments(
            (ROOT / "src/psp_boot_config.c").read_text(encoding="utf-8"))
        self.assertIn('strcmp(line, "experimental_wide_video") == 0', boot)
        self.assertIn("psp_media_wide_program_name_valid(", boot)
        self.assertIn("experimental_wide_video=%s", boot)

    def test_audio_codec_work_buffer_follows_pmplayer_not_the_grant(self):
        # PMPlayer, the hardware-proven implementation this backend is
        # transcribed from, never calls sceAudiocodecGetEDRAM for any codec:
        # cooleyesAudiocodecGetEDRAM allocates control word 4 bytes of
        # ordinary 64-aligned storage and writes the pointer into control word
        # 3. That bypass is deliberate in a player that works on this
        # hardware, so the substitute is the default and the firmware grant is
        # the thing behind a knob. Firmware must also never be asked to
        # release memory it never granted.
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        fake = backend[
            backend.index("static int psp_media_pool_audiocodec_edram("):]
        fake = fake[:fake.index("\n}\n") + 3]
        # Control word 4 is the size firmware asked for; control word 3 is
        # where the pointer goes. Both come straight from PMPlayer.
        self.assertIn("backend->audio_codec[4]", fake)
        self.assertIn("backend->audio_codec[3] = (unsigned long)", fake)
        # Pool storage, not the heap: the codec reads this buffer by DMA.
        self.assertIn("psp_media_alloc64(", fake)
        self.assertNotIn("memalign(", fake)
        create = psp_firmware_backend_create(backend)
        # Exactly one grant call, and the knob check has to precede it, so the
        # default path cannot reach sceAudiocodecGetEDRAM at all.
        self.assertEqual(create.count("sceAudiocodecGetEDRAM("), 1)
        self.assertIn("psp_media_real_edram_enabled(", create)
        self.assertLess(
            create.index("psp_media_real_edram_enabled("),
            create.index("sceAudiocodecGetEDRAM("))
        self.assertIn("psp_media_pool_audiocodec_edram(", create)
        # Which mechanism supplied the buffer has to be on the card before
        # firmware is handed the control block.
        self.assertIn("event=audio-edram mode=%s size=%u ", create)
        self.assertIn("status=0x%08X buffer=0x%08X high=%d", create)
        logged = create.index("event=audio-edram")
        self.assertLess(logged, create.index("sceAudiocodecInit("))
        self.assertLess(
            create.index("psp_media_commit_log();", logged),
            create.index("sceAudiocodecInit("))
        # Only a real grant is released, and only a real grant is followed by
        # the invalidate that drops firmware's writes; invalidating after the
        # substitute's own CPU store would discard the pointer.
        job = backend[
            backend.index(
                "static MediaBackendResult psp_media_run_teardown_job("):
            backend.index("static int psp_media_codec_thread(")]
        self.assertIn("if (backend->audio_edram_real)", job)
        self.assertLess(
            job.index("if (backend->audio_edram_real)"),
            job.index("sceAudiocodecReleaseEDRAM("))
        # PMPlayer's release zeroes control word 3; only the zeroing is ours,
        # because the pool owns the storage.
        self.assertIn("backend->audio_codec[3] = 0;", job)

    def test_the_first_media_open_of_a_process_boots_the_engine(self):
        # PMPlayer's cached boot type also starts at 3, but its first stream
        # was always type 4 or type 1, so it always made a real boot call
        # before decoding and the type-3 short-circuit only ran after one had
        # succeeded. Tilefinch's default program IS type 3, so the same
        # initializer meant the Media Engine may never have been handed the
        # codec program at all. Start unknown instead.
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        self.assertIn("static int psp_media_me_boot_type = -1;", backend)
        self.assertNotIn(
            "static int psp_media_me_boot_type = "
            "PSP_MEDIA_DEFAULT_ME_BOOT_TYPE;",
            backend)
        # `no-boot` restores the old assumption, and seeds it exactly once,
        # where the knob is published and before any video can open.
        knob = backend[
            backend.index("void media_psp_backend_set_wide_program("):]
        knob = knob[:knob.index("\n}\n") + 3]
        self.assertIn("psp_media_cold_boot_call_enabled(", knob)
        self.assertIn(
            "psp_media_me_boot_type = PSP_MEDIA_DEFAULT_ME_BOOT_TYPE;", knob)
        self.assertEqual(
            backend.count(
                "psp_media_me_boot_type = PSP_MEDIA_DEFAULT_ME_BOOT_TYPE;"),
            1)
        # The short-circuit itself is unchanged: it is what makes a repeat
        # open of the same program skip, and it must still be reached.
        boot = backend[
            backend.index("static int psp_media_boot_media_engine("):
            backend.index("static int psp_media_module_loader(")]
        self.assertIn("if (boot_type == psp_media_me_boot_type) {", boot)
        self.assertIn("psp_media_me_boot_type = boot_type;", boot)

    def test_the_decoder_knob_picker_can_only_write_known_values(self):
        # The picker exists so a device A/B costs a menu press instead of a
        # shutdown, a USB cable, and a hand edit. It must therefore be unable
        # to produce the one outcome a hand edit still can: a spelling the
        # boot config gate halts on. It writes only from the choice table, and
        # it writes through the same transactional override writer the
        # developer URL editor uses.
        settings = without_comments(
            (ROOT / "src/psp_app/psp_app_settings.c").read_text(
                encoding="utf-8"))
        picker = settings[
            settings.index("bool psp_app_set_video_decoder(PspApp *app)"):]
        self.assertIn("psp_media_wide_program_choice(", picker)
        self.assertIn("psp_boot_config_write_overrides(", picker)
        self.assertIn("psp_app_boot_override_path(", picker)
        # A failed write restores the previous spelling rather than leaving
        # the in-memory config disagreeing with the card.
        self.assertIn("experimental_wide_video), \"%s\",\n", picker)
        self.assertIn("previous", picker)
        # Nothing here may synthesize a spelling of its own.
        self.assertNotIn('"wide"', picker)
        self.assertNotIn('"edram-real"', picker)
        # The knob is read at boot, so applying it means restarting through
        # the existing launcher relaunch, not mutating a running process.
        self.assertIn("psp_exit_plan_request(", picker)
        self.assertIn("PSP_EXIT_CONFIG_RESTART", picker)
        self.assertNotIn("media_psp_backend_set_wide_program(", picker)
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        self.assertIn(".interactive = interactive", main)
        self.assertIn(
            "if (restart_launcher && process.install_paths.slotted)", main)
        internal = without_comments(
            (ROOT / "src/psp_app/psp_app_internal.h").read_text(
                encoding="utf-8"))
        self.assertNotIn("bool exit_requested;", internal)
        self.assertNotIn("bool restart_requested;", internal)
        # The picker is a validation-build diagnostic, like the power and
        # video test rows: a shipping EBOOT offers no A/B and must not offer
        # settings hardware has already rejected.
        ui = without_comments(
            (ROOT / "src/psp_ui.c").read_text(encoding="utf-8"))
        row = ui.index("UI_EXPERIMENTAL_ROW_VIDEO_DECODER")
        self.assertIn(
            "TILEFINCH_PSP_POWER_TEST_MENU",
            ui[max(0, row - 400):row])
        self.assertIn("#define UI_EXPERIMENTAL_OPTIONS_ITEM_COUNT 7u", ui)
        self.assertIn("#define UI_EXPERIMENTAL_OPTIONS_ITEM_COUNT 6u", ui)
        self.assertIn("PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT", ui)
        self.assertIn("PSP_UI_ACTION_SET_VIDEO_DECODER", ui)

    def test_baseline_defaults_to_the_boot_call_free_program(self):
        # Every Media Engine program Tilefinch has booted from user mode --
        # wide Main on type 1, then 240p Baseline on type 4 -- was rejected at
        # the raw-NAL bridge on a PSP-3000, and so was type 3 back when it
        # made no kernel call at all. Baseline defaults to type 3, which is
        # now really booted (see the first-open boot test above), and the
        # historical type 4 stays reachable only through the knob.
        policy = without_comments(
            (ROOT / "src/media_backend_psp_policy.h").read_text(
                encoding="utf-8"))
        decoder = policy[
            policy.index("static inline bool psp_media_decoder_policy("):
            policy.index(
                "static inline bool psp_media_wide_program_rejected_by(")]
        self.assertIn("bool baseline_boot_call", decoder)
        self.assertIn("policy->me_boot_type = baseline_boot_call", decoder)
        # No branch may reach boot type 4 without the knob having asked.
        self.assertNotIn("me_boot_type = 4;", decoder)
        self.assertIn('strcmp(name, "boot4") == 0', policy)
        self.assertIn('|| strcmp(name, "boot4") == 0', policy)
        # The knob moves exactly one decision: `boot4` is not a wide
        # spelling, so a wide picture stays inadmissible under it.
        enabled = policy[
            policy.index(
                "static inline bool psp_media_wide_program_enabled("):
            policy.index(
                "static inline bool psp_media_wide_program_annexb(")]
        self.assertNotIn("BASELINE_BOOT4", enabled)
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        self.assertIn("psp_media_baseline_boot_call_enabled(", backend)

    def test_media_failure_surfaces_commit_the_log_to_the_card(self):
        # The validation log is only synchronized to the Memory Stick at
        # checkpoints and at exit, and the media pipeline reaches neither. A
        # freeze or a power-off therefore lost the whole media section of the
        # device log. Schedule the commit where the diagnosis is produced,
        # but perform card I/O only after the browser thread collects it.
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        commit = backend[
            backend.index(
                "static void psp_media_schedule_failure_log_commit("):
            backend.index("static void psp_media_log_failure(")]
        # Failures repeat per packet and per retry; a card sync each time
        # would turn a diagnosable failure into a hang.
        self.assertIn("PSP_MEDIA_FAILURE_SYNC_INTERVAL_US", commit)
        self.assertIn("sceKernelGetSystemTimeWide()", commit)
        self.assertIn("psp_media_commit_log();", commit)
        self.assertLess(
            commit.index("static void psp_media_commit_pending_failure_log("),
            commit.index("psp_media_commit_log();"))
        decode = backend[
            backend.index(
                "static MediaBackendResult psp_media_decode_staged_video("):
            backend.index(
                "static MediaBackendResult psp_media_decode_staged_audio(")]
        submitted = decode.index("event=avc-bridge-submit")
        primed = decode.index("if (!primed_before) psp_media_commit_log();")
        # The submitted bytes must be on the card before the firmware call
        # that has frozen the machine, and only while priming: a per-frame
        # card sync would cost many times the steady-state frame budget.
        self.assertLess(submitted, primed)
        self.assertLess(primed, decode.index("sceMpegGetAvcNalAu("))
        create = psp_firmware_backend_create(backend)
        boot = create.index("psp_media_boot_media_engine(")
        logged = create.index("event=me-boot", boot)
        self.assertLess(logged, create.index("sceMpegQueryMemSize("))
        self.assertLess(logged, create.index("psp_media_commit_log();", boot))
        self.assertIn("skipped=1", create)
        session = without_comments(
            psp_media_session_sources())
        raised = session[
            session.index("void psp_media_raise_error("):
            session.index("void psp_media_retire_first_frame(")]
        self.assertIn("bool was_failed = media->ui.failed;", raised)
        self.assertIn("if (!was_failed) {", raised)
        self.assertIn("psp_media_report_failure_snapshot(", raised)
        self.assertIn("(void) psp_log_flush(true);", raised)
        # One sync as the panel becomes visible means one raise path: every
        # other failure surface in the session goes through the helper.
        self.assertEqual(session.count("psp_ui_media_set_error"), 2)

    def test_validation_logging_does_not_flush_the_memory_stick_per_line(self):
        source = without_comments(
            (ROOT / "src/psp_log.c").read_text(encoding="utf-8"))
        self.assertIn("PSP_LOG_STREAM_BUFFER_BYTES", source)
        self.assertIn("_IOFBF", source)
        write = source[
            source.index("int psp_log_printf(const char *format, ...)",
                         source.index("#else")):
            source.index("FILE *psp_log_file(void)")]
        self.assertNotIn("fflush(", write)
        completion = (ROOT / "src/media_backend_psp.c").read_text(
            encoding="utf-8")
        account = completion[
            completion.index("static int psp_media_account_codec_completion("):
            completion.index("static int psp_media_collect_codec_job(")]
        self.assertNotIn("event=codec-job-complete", account)
        # The optional whole-AU failure artifact is also buffered, but even
        # its one teardown write belongs on the PSPLink host. Playback
        # diagnostics have no Memory Stick fallback.
        self.assertIn(
            '#define PSP_MEDIA_AU_DUMP_PATH '
            '"host0:/tilefinch-au-dump.bin"', completion)
        self.assertNotIn(
            '#define PSP_MEDIA_AU_DUMP_PATH "ms0:', completion)

    def test_failed_codec_calls_dump_the_memory_firmware_owns(self):
        # Both firmware codecs report a rejection as one opaque status, which
        # cannot say whether the Media Engine ran at all. The buffers firmware
        # owns can: the AU descriptor is memset to 0xFF before sceMpegInitAu
        # and the AAC control block is firmware's to mutate, so reading them
        # back after a failure separates an argument the bridge refused from
        # content the engine really decoded and rejected. Neither dump may buy
        # its own card sync -- both accompany a failure line, and that line
        # already commits through the rate limiter.
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        decode = backend[
            backend.index(
                "static MediaBackendResult psp_media_decode_staged_video("):
            backend.index(
                "static MediaBackendResult psp_media_decode_staged_audio(")]
        dump = decode.index("event=au-dump stage=%s bytes=%s")
        self.assertLess(decode.index("sceMpegGetAvcNalAu("), dump)
        self.assertLess(decode.index("sceMpegAvcDecode("), dump)
        # Read RAM, not a line the CPU refilled across the firmware call --
        # but write back before dropping. Both bridge calls run on the main
        # CPU and update this descriptor through the data cache, so a pure
        # invalidate would destroy the very partial writes the dump exists to
        # read.
        invalidated = decode.rindex(
            "sceKernelDcacheWritebackInvalidateRange(", 0, dump)
        self.assertIn(
            "backend->video_au, PSP_MEDIA_VIDEO_AU_BYTES",
            decode[invalidated:dump])
        failure = decode.index("psp_media_log_failure(backend, stage_name")
        self.assertLess(dump, failure)
        self.assertNotIn("psp_media_commit_log();", decode[dump:failure])
        # The firmware call and its refusal dump live in the per-unit decode,
        # which the batched job runs once per staged access unit.
        audio = backend[
            backend.index(
                "static MediaBackendResult psp_media_decode_one_audio_au("):]
        audio = audio[:audio.index("\n}\n") + 3]
        words = audio.index("event=codec-dump words=%s")
        refused = audio.index("if (status < 0) {")
        self.assertLess(audio.index("sceAudiocodecDecode("), refused)
        self.assertLess(refused, words)
        # The control block gets its writeback-invalidate after every decode
        # call already, so this dump must not pay for a second cache op or a
        # second sync.
        self.assertNotIn(
            "sceKernelDcacheInvalidateRange(", audio[refused:words])
        self.assertNotIn(
            "sceKernelDcacheWritebackInvalidateRange(", audio[refused:words])
        audio_failure = audio.index("psp_media_log_failure(", refused)
        self.assertLess(words, audio_failure)
        self.assertNotIn("psp_media_commit_log();", audio[words:audio_failure])

    def test_media_engine_buffers_come_from_a_boot_time_pool(self):
        # The Media Engine cannot address the PSP-2000/3000 extra RAM bank at
        # 0x0A000000 and above, and with the extra-memory unlock active the
        # browser's heap grows straight through that line. A media buffer
        # allocated when a video opens can therefore land where firmware is
        # physically unable to read it, which is what makes two independent
        # codecs reject structurally perfect input. Every buffer firmware DMAs
        # from must come out of one reservation taken while the heap cursor is
        # still low, and every run must say where the buffers actually landed.
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        pool = without_comments(
            (ROOT / "src/media_backend_psp_pool.h").read_text(
                encoding="utf-8"))
        self.assertIn("#define PSP_MEDIA_ME_VISIBLE_LIMIT", pool)
        self.assertIn("0x0A000000", pool)
        # Exactly two allocation calls in the whole backend: the boot
        # reservation, and the one helper that draws from it. Anything else
        # would be a buffer that can land above the Media Engine's reach.
        self.assertEqual(backend.count("memalign("), 2)
        reserve = backend[
            backend.index("void media_psp_backend_reserve_pool(void)"):]
        reserve = reserve[:reserve.index("\n}\n") + 3]
        self.assertIn(
            "memalign(PSP_MEDIA_DDR_ALIGNMENT, PSP_MEDIA_POOL_BYTES)",
            reserve)
        self.assertIn("event=me-pool ", reserve)
        # A successful boot reservation is not a media failure. In a host0
        # PRX run, synchronously flushing after pspsh has detached can block
        # the main thread before browser creation. The ordinary boot
        # checkpoint persists the already-buffered address line instead.
        self.assertNotIn("psp_media_commit_log();", reserve)
        # A heap no bigger than a stock partition lies entirely below the
        # Media Engine limit, so it must keep the pre-pool memory behaviour
        # rather than give up several megabytes for a reservation it cannot
        # need. The quantity that decides this is heap capacity: the pool's
        # memalign draws from the heap, and PSPSDK has already given newlib
        # the whole partition by the time main() runs, so
        # sceKernelTotalFreeMemSize reports a constant ~2.2 MB on every
        # device and admitted nothing at all on the hardware the pool exists
        # for. It stays in the log because every other boot line quotes it,
        # but it may never be the signal again.
        self.assertIn(
            "psp_media_pool_reservation_admitted(capacity_bytes)", reserve)
        self.assertIn(
            "capacity_bytes = psp_media_probe_heap_capacity();", reserve)
        self.assertNotIn(
            "psp_media_pool_reservation_admitted((size_t) free_bytes)",
            reserve)
        # Both readings are on the line, so a device log says which one the
        # verdict came from.
        self.assertIn("heap=%u free=%u", reserve)
        self.assertIn("sceKernelTotalFreeMemSize()", reserve)
        # The probe is bounded by the verdict itself, and every block is back
        # in the heap before the reservation asks for its 4 MiB-aligned base:
        # a probe that still held memory would push that base upward, which
        # is the exact failure the pool exists to prevent.
        probe = backend[
            backend.index("static size_t psp_media_probe_heap_capacity("):]
        probe = probe[:probe.index("\n}\n") + 3]
        self.assertIn("void *blocks[PSP_MEDIA_POOL_PROBE_BLOCKS];", probe)
        self.assertIn("malloc(PSP_MEDIA_POOL_PROBE_BLOCK_BYTES)", probe)
        self.assertLess(
            probe.index("free(blocks[index]);"), probe.index("return "))
        self.assertLess(
            backend.index("static size_t psp_media_probe_heap_capacity("),
            backend.index("void media_psp_backend_reserve_pool(void)"))
        shared = backend[
            backend.index("static void *psp_media_alloc_shared("):
            backend.index("static void *psp_media_alloc64(")]
        self.assertIn("psp_media_pool_alloc(", shared)
        self.assertIn("event=me-pool-exhausted need=%u ", shared)
        self.assertIn("remaining=%u", shared)
        self.assertIn("memalign(", shared)
        create = psp_firmware_backend_create(backend)
        # No allocation in the create path may bypass the pool.
        self.assertNotIn("memalign(", create)
        self.assertNotIn("malloc(", create)
        self.assertIn(
            "psp_media_alloc_shared(\n"
            "        PSP_MEDIA_DDR_BYTES, PSP_MEDIA_DDR_ALIGNMENT)", create)
        sets = backend[
            backend.index("static bool psp_media_copy_parameter_sets("):
            backend.index("static int psp_media_capture_avc_pictures(")]
        self.assertIn("psp_media_alloc64(bytes)", sets)
        # The addresses have to be on the card before anything can reach
        # firmware, because every device run so far has ended in a freeze or a
        # power-off somewhere after this point.
        memory = create.index("event=me-memory")
        self.assertIn("high=%d", create[memory:])
        self.assertLess(memory, create.index("sceMpegCreate("))
        self.assertLess(
            create.index("psp_media_commit_log();", memory),
            create.index("sceMpegCreate("))
        self.assertIn("psp_media_me_invisible_count(", create)
        # Quarantine leaks every firmware-visible buffer on purpose, so the
        # pool must be poisoned rather than rewound by that path -- and no
        # site may latch the refusal without doing it.
        self.assertNotIn("psp_media_backend_is_quarantined = true", create)
        destroy = psp_firmware_backend_destroy(backend)
        self.assertNotIn("psp_media_backend_is_quarantined = true", destroy)
        self.assertIn("psp_media_quarantine();", destroy)
        self.assertNotIn("free(backend->", destroy)
        self.assertIn("psp_media_release(backend->ddr_memory);", destroy)
        self.assertIn("psp_media_pool_reset(&psp_media_pool);", destroy)
        quarantine = backend[
            backend.index("static void psp_media_quarantine(void)"):]
        quarantine = quarantine[:quarantine.index("\n}\n") + 3]
        self.assertIn("psp_media_pool_poison(&psp_media_pool);", quarantine)
        self.assertIn("event=me-pool-poisoned", quarantine)
        # Boot order: the reservation is only worth anything while the heap
        # cursor is still low, so nothing that allocates may precede it.
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        reserved = main.index("media_psp_backend_reserve_pool();")
        self.assertLess(reserved, main.index('psp_log_checkpoint("boot-ready'))
        self.assertLess(
            reserved, main.index("psp_report_heap_capacity_lower_bound();"))
        self.assertLess(reserved, main.index("psp_boot_config_defaults("))
        self.assertLess(reserved, main.index("browser_engine_create("))
        # Shipping builds get the pool too: only the log lines are gated.
        self.assertNotIn(
            "TILEFINCH_PSP_VALIDATION_LOG",
            main[main.index("tilefinch_platform_set_services(&services);"):
                 reserved])

    def test_annexb_rewrite_is_scoped_to_the_wide_program(self):
        # The conversion runs on the codec worker, on this backend's own
        # staging copy, before the cache writeback publishes it -- and only
        # for the program the experiment is aimed at.
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        decode = backend[
            backend.index(
                "static MediaBackendResult psp_media_decode_staged_video("):
            backend.index(
                "static MediaBackendResult psp_media_decode_staged_audio(")]
        self.assertIn("psp_media_wide_program_annexb(", decode)
        self.assertIn("backend->mpeg_mode == 5", decode)
        rewrite = decode.index("psp_media_annexb_rewrite(")
        self.assertLess(
            rewrite, decode.index("sceKernelDcacheWritebackRange("))
        self.assertLess(rewrite, decode.index("sceMpegGetAvcNalAu("))
        self.assertIn("event=annexb-rewrite-rejected", decode)
        self.assertIn("event=annexb-submit", decode)
        # A refused conversion must restore the reorder queue exactly like
        # every other submission failure, and must not allocate.
        refusal = decode[
            rewrite:decode.index("event=annexb-rewrite-rejected")]
        self.assertIn(
            "backend->video_timestamps = backend->codec_job_timestamps_before",
            refusal)
        self.assertNotIn("malloc", decode)
        self.assertNotIn("memalign", decode)
        # Only the codec worker runs this function.
        worker = backend[
            backend.index("static int psp_media_codec_thread("):
            backend.index("static int psp_media_collect_codec_job(")]
        self.assertIn("psp_media_decode_staged_video(", worker)
        submit = backend[
            backend.index("static MediaBackendResult psp_media_submit("):
            backend.index("static MediaBackendResult psp_media_drain(")]
        self.assertNotIn("psp_media_decode_staged_video(", submit)
        self.assertNotIn("psp_media_annexb_rewrite(", submit)

    def test_results_playback_script_does_not_wait_for_idle_media(self):
        source = (ROOT / "tests/input-scripts/"
                  "youtube-results-return-play-live.txt").read_text()
        playback = source.split("tap-live cross\n", 1)[1]
        self.assertIn("mark-live progressed", playback)
        self.assertIn("tap-live circle", playback)
        for line in playback.splitlines():
            self.assertNotIn(line.split(" ", 1)[0], ("wait", "mark", "tap"))

    def test_codec_worker_death_and_retry_quarantine_are_immediate(self):
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        collect = backend[
            backend.index("static int psp_media_collect_codec_job("):
            backend.index("static MediaBackendResult psp_media_queue_codec_job(")]
        self.assertIn("psp_thread_observe(", collect)
        self.assertIn("psp_unexpected_worker_exit_status(", collect)
        self.assertIn("UINT32_C(100000)", collect)
        self.assertIn("codec_worker_next_health_us == 0", collect)
        watchdog = collect[collect.index("int wedged_stage ="):]
        diagnostic = watchdog[
            watchdog.index("#if defined(TILEFINCH_PSP_VALIDATION_LOG)"):
            watchdog.index("#endif")]
        self.assertIn("psp_thread_snapshot(", diagnostic)
        self.assertIn("event=codec-watchdog", diagnostic)
        self.assertIn("thread_info.waitId", diagnostic)
        self.assertIn("thread_info.runClocks", diagnostic)
        session = without_comments(
            psp_media_session_sources())
        transport = session[
            session.index("bool psp_media_retry_transport("):
            session.index("bool psp_media_retry_transport_expiry(")]
        fallback = session[
            session.index("bool psp_media_retry_240p("):
            session.index("bool psp_media_open_pump(")]
        for retry in (transport, fallback):
            self.assertLess(
                retry.index("psp_media_pipeline_destroy(media)"),
                retry.index("media_psp_backend_quarantined()",
                            retry.index("psp_media_pipeline_destroy(media)")))
            self.assertIn("VIDEO DECODER NEEDS APP RESTART", retry)
        # Refusing the retry in the backend is not enough: a failed panel
        # that still draws Retry is false hope on a device whose decoder is
        # quarantined for the rest of the process.
        failed = session[
            session.index("void psp_media_job_failed("):
            session.index("static bool psp_media_transport_refresh_needed(")]
        self.assertIn(
            "media->ui.retry_unavailable = media_psp_backend_quarantined()",
            failed)
        self.assertLess(
            failed.index("psp_media_raise_error(media, error, NULL)"),
            failed.index("media->ui.retry_unavailable ="))
        ui = without_comments(
            (ROOT / "src/psp_ui.c").read_text(encoding="utf-8"))
        self.assertIn("media->retry_unavailable = false;", ui)
        self.assertIn("!media->retry_unavailable", ui)
        self.assertIn("Restart Tilefinch to use video", ui)

    def test_a_completed_picture_is_not_blocked_by_an_unrelated_job(self):
        """A claim asks about its slot, never about the shared job slot.

        This used to be a narrower rule: the take was refused for the whole
        of any running job EXCEPT an audio one, carved out because AAC owns
        no surface. The carve-out was right and did not go far enough -- a
        video job converting slot 1 says exactly as little about a picture
        READY in slot 0 as an audio job does. On device the remaining gate
        refused 342 to 405 due pictures per soak, about 13% of takes.

        So the take now consults no job state at all, and what makes that
        safe is per-slot release/acquire rather than a wider gate. Both
        halves are pinned here, because either one alone is a bug: dropping
        the gate without the ordering is a data race on the slot metadata,
        and the ordering without dropping the gate is the anti-phase this
        removed.
        """
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        take = source[
            source.index("static bool psp_media_take_frame("):
            source.index("static bool psp_media_release_video_slot(")]
        for job_state in (
                "codec_job_state", "codec_job_kind",
                "PSP_MEDIA_CODEC_JOB_RUNNING", "PSP_MEDIA_CODEC_JOB_DONE",
                "PSP_MEDIA_CODEC_KIND_AUDIO", "video_surface_stable"):
            self.assertNotIn(
                job_state, take,
                "a claim is a question about one slot, not about the job")
        # What is left is per-slot, and it is the one thing that genuinely
        # must hold a picture back: claimed and writable are never both true
        # of a slot, so a claim waits out a write lease that still stands.
        # The ME, the DMA controller and the GE really do overlap here -- no
        # single-core serialization argument is available.
        self.assertIn(
            "atomic_load(&psp_media_surface_writing[chosen]) != 0", take)
        self.assertIn("backend->cadence.take_busy++", take)
        # And the ordering that replaced the gate, at its definition.
        policy = without_comments(
            (ROOT / "src/media_backend_psp_policy.h").read_text(
                encoding="utf-8"))
        self.assertIn("_Atomic int state;", policy)
        self.assertIn(
            "atomic_load_explicit(&slot->state, memory_order_acquire)",
            policy)
        self.assertIn(
            "atomic_store_explicit(&slot->state, state, memory_order_release)",
            policy)
        # The claim reads the state through the acquiring accessor, so every
        # metadata store the converting thread made before its release store
        # is visible before any field of the picture is read.
        holds = policy[
            policy.index("static inline bool psp_media_slot_holds_picture("):
            policy.index("static inline int psp_media_slot_take_index(")]
        self.assertIn("psp_media_slot_observe(slot)", holds)
        self.assertNotIn("slot->state ==", holds)
        # ...and the worker's free-slot scan acquires too: it is about to
        # write the slot, so it must first see the erasure of the picture
        # the browser thread handed back.
        free_index = policy[
            policy.index("static inline int psp_media_slot_free_index("):
            policy.index("static inline unsigned psp_media_slot_free_count(")]
        self.assertIn("psp_media_slot_observe(&slots[at])", free_index)

    def test_seek_waits_for_a_healthy_codec_unit_before_failing(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        reset = source[
            source.index("static bool psp_media_reset("):
            source.index("static bool psp_media_take_frame(")]
        self.assertIn("PSP_MEDIA_CODEC_QUIESCE_WAIT_US", reset)
        self.assertIn("event=codec-reset-quiesced", reset)

    def test_aac_control_block_is_invalidated_after_firmware_mutation(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        decode = source[source.index("int status = sceAudiocodecDecode("):
                        source.index("if (status < 0) {",
                                     source.index("int status = sceAudiocodecDecode("))]
        self.assertRegex(
            decode,
            r"sceKernelDcacheWritebackInvalidateRange\(\s*"
            r"backend->audio_codec,\s*"
            r"psp_media_cache_extent\(PSP_MEDIA_AUDIO_CODEC_BYTES\)\);")
        self.assertLess(decode.index("sceAudiocodecDecode("),
                        decode.index("backend->audio_codec"))

    def test_aac_busy_retry_is_bounded_and_retains_the_staged_unit(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        decode = source[
            source.index("static MediaBackendResult psp_media_decode_one_audio_au("):
            source.index("static MediaBackendResult psp_media_decode_staged_audio(")]
        self.assertIn("status == PSP_MEDIA_ERROR_BUSY", decode)
        self.assertIn("audio_decode_busy_retries < 16u", decode)
        self.assertIn("busy_elapsed_us < UINT32_C(500000)", decode)
        self.assertIn("return MEDIA_BACKEND_WOULD_BLOCK", decode)
        self.assertNotIn("audio_staged_index++", decode)
        self.assertIn("audio_decode_busy_retries = 0u", decode)
        batch = source[
            source.index("static MediaBackendResult psp_media_decode_staged_audio("):
            source.index("static MediaBackendResult psp_media_drain_staged_video(")]
        self.assertLess(
            batch.index("if (result != MEDIA_BACKEND_ACCEPTED) return result"),
            batch.index("backend->audio_staged_index++"))
        reset = source[
            source.index("static bool psp_media_reset("):
            source.index("static void psp_media_destroy(")]
        self.assertIn("audio_decode_busy_retries = 0u", reset)

    def test_codec_worker_never_synchronizes_the_memory_stick(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        failure = source[
            source.index(
                "static void psp_media_schedule_failure_log_commit("):
            source.index("static void psp_media_commit_pending_failure_log(")]
        self.assertNotIn("psp_media_commit_log()", failure)
        self.assertIn("psp_media_failure_sync_pending", failure)
        collector = source[
            source.index("static int psp_media_collect_codec_job("):
            source.index("static bool psp_media_prepared_pair_allowed(")]
        self.assertIn("psp_media_commit_pending_failure_log()", collector)

    def test_firmware_written_control_blocks_are_never_purely_invalidated(
            self):
        # sceAudiocodecCheckNeedMem, sceAudiocodecGetEDRAM, sceAudiocodecInit,
        # sceAudiocodecDecode, sceMpegInitAu, sceMpegGetAvcNalAu and
        # sceMpegAvcDecode all run on the main CPU and write their control
        # structures through the CPU's own data cache. A pure invalidate over
        # one of those ranges discards the dirty line before it reaches RAM
        # and the reread refills whatever memory held beforehand: that is how
        # a PSP-3000 reported `event=audio-edram mode=pool size=0` with the
        # engine pool otherwise healthy, and how the AU descriptor kept
        # reading back as the 0xFF fill sceMpegInitAu was supposed to replace.
        # PPSSPP does not model the data cache, so no emulator gate can catch
        # a regression here -- only this one.
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        firmware_written = ("backend->audio_codec", "backend->video_au")
        offenders = []
        for arguments in call_arguments(
                source, "sceKernelDcacheInvalidateRange"):
            target = split_arguments(arguments)[0]
            if any(block in target for block in firmware_written):
                offenders.append(" ".join(arguments.split()))
        self.assertEqual(
            [], offenders,
            "Control blocks firmware writes from the main CPU must use "
            "sceKernelDcacheWritebackInvalidateRange, which preserves those "
            "writes and still refetches anything the Media Engine changed.")
        # The two ranges that may still be invalidated outright, and why: the
        # Media Engine writes each of them exclusively, and the call before it
        # writeback-invalidates the same range so no dirty CPU line survives
        # into the firmware call. The surface appears twice because the
        # validation-only byte-order probe repeats the conversion of an
        # already decoded picture into that same range with the same
        # handshake; it is the identical range with the identical exclusive
        # writer, not a new one.
        engine_written = []
        for arguments in call_arguments(
                source, "sceKernelDcacheInvalidateRange"):
            engine_written.append(split_arguments(arguments)[0])
        self.assertEqual(
            # The decoded surface -- now a slot of it, and the validation
            # probe's slot 0 -- and the audio PCM block. Nothing else may be
            # dropped from cache without being written back first.
            ["surface", "backend->audio_pcm", "backend->surfaces[0]"],
            engine_written,
            "A new pure invalidate needs a documented exclusive Media Engine "
            "writer before it may be added to this list.")

    def test_cache_invalidate_lengths_are_64_byte_multiples(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        # Lengths that are already 64-byte multiples by construction:
        # 64u, 1024 stereo int16 samples, and stride(512|768) * rows * 4.
        aligned = (
            "psp_media_cache_extent(",
            "PSP_MEDIA_VIDEO_AU_BYTES",
            "PSP_MEDIA_AUDIO_PCM_BYTES",
            "backend->surface_bytes",
        )
        offenders = []
        # Both forms invalidate, so both carry the alignment requirement.
        for call in ("sceKernelDcacheInvalidateRange",
                     "sceKernelDcacheWritebackInvalidateRange"):
            for arguments in call_arguments(source, call):
                length = split_arguments(arguments)[-1]
                if not any(token in length for token in aligned):
                    offenders.append(" ".join(arguments.split()))
        self.assertEqual(
            [], offenders,
            "Real firmware requires a 64-byte-aligned address and length for "
            "a dcache op that invalidates. The status is discarded, so an "
            "exact struct size silently voids the protection instead of "
            "failing.")

    def test_raw_avc_rejects_unrepresentable_parameter_set_families(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        parser = source[source.index("static bool psp_avcc_sets("):
                        source.index("static bool psp_media_copy_parameter_sets")]
        self.assertIn("sps_count != 1u", parser)
        self.assertIn("pps_count != 1u", parser)

    def test_zero_mpeg_workspace_is_a_native_failure(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        query = source[source.index("sceMpegQueryMemSize("):
                       source.index("backend->ddr_memory =")]
        self.assertIn("backend->mpeg_memory_bytes <= 0", query)
        self.assertIn("backend->mpeg_memory_bytes < 0", query)
        self.assertIn(": -1", query)
        self.assertNotIn("status = backend->mpeg_memory_bytes;", query)

    def test_audiocodec_does_not_load_the_unrelated_sce_aac_module(self):
        source = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        loader = source[source.index("static int psp_media_module_loader("):
                        source.index("MediaPspPrepareResult ")]
        self.assertIn("PSP_AV_MODULE_AVCODEC", loader)
        self.assertNotIn("PSP_AV_MODULE_AAC", loader)
        self.assertIn("sceAudiocodecCheckNeedMem(", source)
        self.assertIn("sceAudiocodecDecode(", source)

    def test_voice_quarantine_preserves_module_and_working_ownership(self):
        source = without_comments(
            (ROOT / "src/psp_voice_input.c").read_text(encoding="utf-8"))
        killed = source[source.index("if (lifecycle_platform_state.worker_killed) {"):]
        killed = killed[:killed.index("\n    if (job->create_engine)")]
        self.assertIn("psp_voice_component_quarantine();", killed)
        self.assertIn("voice->engine = NULL;", killed)
        self.assertIn("return false;", killed)
        self.assertNotIn("stt_engine_", killed)
        disable = source[source.index("bool psp_voice_input_set_enabled("):
                         source.index("void psp_voice_input_set_adaptive_memory(")]
        self.assertRegex(disable, r"if \(!psp_voice_component_is_quarantined\(\)\)\s*\{\s*budget_reservation_release")
        evict = source[source.index("void psp_voice_input_evict("):
                       source.index("void psp_voice_input_trim(")]
        self.assertIn("psp_voice_component_is_quarantined()", evict)
        self.assertLess(evict.index("stt_engine_destroy("),
                        evict.rindex("psp_voice_component_unload();"))

    def test_voice_reclaim_closes_transport_before_network_teardown(self):
        source = without_comments(
            (ROOT / "src/psp_app/psp_app_input.c").read_text(
                encoding="utf-8"))
        function = source[source.index("size_t psp_text_input_prepare_voice("):
                          source.index("uint32_t psp_ui_buttons(")]
        self.assertLess(
            function.index("browser_engine_cancel_network_work("),
            function.index("psp_shutdown_network_logged("))
        self.assertLess(
            function.index("fetch_preconnect_cancel("),
            function.index("psp_shutdown_network_logged("))

    def test_psp_osk_shutdown_is_requested_once(self):
        source = without_comments(
            (ROOT / "src/psp_text_input.c").read_text(encoding="utf-8"))
        osk = source[source.index("static bool open_keyboard("):
                     source.index("static size_t refresh_danzeff_view(")]
        quit_case = osk[osk.index("case PSP_UTILITY_DIALOG_QUIT:"):
                        osk.index("case PSP_UTILITY_DIALOG_NONE:")]
        self.assertIn("if (!shutdown_requested)", quit_case)

    def test_psp_osk_uses_the_sdk_button_swap_selector(self):
        source = without_comments(
            (ROOT / "src/psp_text_input.c").read_text(encoding="utf-8"))
        osk = source[source.index("static bool open_keyboard("):
                     source.index("static size_t refresh_danzeff_view(")]
        self.assertIn("PSP_SYSTEMPARAM_ID_INT_UNKNOWN", osk)
        self.assertNotIn("PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP", osk)

    def test_psp_concurrent_pool_does_not_retry_permanent_semaphore_errors(self):
        source = without_comments(
            (ROOT / "src/budget.c").read_text(encoding="utf-8"))
        lock = source[source.index("static bool concurrent_pool_lock("):
                      source.index("static void concurrent_pool_unlock(")]
        self.assertIn("return sceKernelWaitSema(", lock)
        self.assertNotIn("while (sceKernelWaitSema(", lock)

    def test_cancelled_media_scope_detaches_the_player_from_the_screen(self):
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        status = runtime[
            runtime.index("static void psp_supervisor_show_status("):
            runtime.index("bool psp_navigation_cooperate_active(")]
        self.assertIn(
            "cooperate->supervisor_media_ui.visible = false;", status)
        self.assertIn("cooperate->media_surface = false;", status)
        self.assertIn("cooperate->media_detached = true;", status)
        self.assertIn(
            "psp_ui_show_status(\n        &cooperate->supervisor_ui",
            status,
            "A cancelled media scope must repaint the retained page frame "
            "through the supervisor's browser snapshot, not repaint the "
            "player overlay with a stopping caption: the main thread cannot "
            "retire the open job until its blocking primitive returns.")
        self.assertNotIn("media->resolving = true;", status)
        tick = runtime[
            runtime.index("void psp_background_ui_tick("):
            runtime.index("bool psp_platform_cooperate(")]
        self.assertIn(
            "cooperate->media_surface || cooperate->media_detached", tick,
            "Later presses during a detached media unwind must still say "
            "the video is what is stopping.")
        self.assertIn("if (cooperate->media_surface)\n"
                      "            psp_present_supervisor_media(", tick)

    def test_cancelled_media_scope_closes_before_the_next_main_present(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        scope_end = main[
            main.index("if (media_open_scope) {"):
            main.index("psp_log_set_phase(PSP_LOG_PHASE_INTERACTIVE);",
                       main.index("if (media_open_scope) {"))]
        self.assertIn("psp_media_close(&browser->media);", scope_end)
        self.assertLess(
            scope_end.index("psp_media_close(&browser->media);"),
            scope_end.index("psp_navigation_cooperate_end(\"media-open\")"),
            "The player must be hidden in the main thread's own media state "
            "before the cooperate scope is released, or the first ordinary "
            "present after a detached unwind composites the overlay again.")
        advance = main[
            main.index("media_visual_changed =\n                "
                       "psp_media_advance("):
            main.index("if (media_open_scope) {")]
        self.assertNotIn(
            "psp_present(", advance,
            "Nothing may present between the open pump returning and the "
            "close that hides the player.")
        # A video opened while the previous open is still unwinding must not
        # start a second cooperate scope: the unwinding one still owns the
        # cancellation token, the supervisor snapshot and the alternating
        # VRAM buffer.
        begin = main[
            main.index("bool media_open_before ="):
            main.index("bool media_decode_scope = false;")]
        self.assertIn("!psp_navigation_cooperate_active()", begin)
        decode_begin = main[
            main.index("bool media_decode_scope = false;"):
            main.index(
                "if ((!media_open_before || !navigation_still_pending)")]
        self.assertIn("!psp_navigation_cooperate_active()", decode_begin)

    def test_media_network_attempts_bound_their_blind_connect_phase(self):
        session = without_comments(
            psp_media_session_sources())
        self.assertIn("#define PSP_MEDIA_CONNECT_TIMEOUT_MS 3000", session)
        self.assertEqual(
            3, session.count(
                ".connect_timeout_ms = PSP_MEDIA_CONNECT_TIMEOUT_MS"),
            "The page-video probe and both range windows must bound connect; "
            "libcurl reaches the cancellation callback only from its "
            "progress callback, which does not run during connect.")
        resolver = without_comments(
            (ROOT / "src/youtube_resolver.c").read_text(encoding="utf-8"))
        self.assertIn(
            "#define YOUTUBE_RESOLVER_CONNECT_TIMEOUT_MS 3000L", resolver)
        self.assertEqual(
            3, resolver.count(
                ".connect_timeout_ms = "
                "YOUTUBE_RESOLVER_CONNECT_TIMEOUT_MS"))
        transport = without_comments(
            (ROOT / "src/fetch/transport.inc").read_text(encoding="utf-8"))
        self.assertNotIn(
            "CURLOPT_CONNECTTIMEOUT_MS,\n               timeout_ms / 2",
            transport,
            "The blocking transport must route its connect bound through "
            "fetch_connect_timeout_ms so a caller can tighten it.")
        self.assertEqual(
            2, transport.count("fetch_connect_timeout_ms("))
        fetch = without_comments(
            (ROOT / "src/fetch.c").read_text(encoding="utf-8"))
        helper = fetch[
            fetch.index("static long fetch_connect_timeout_ms("):
            fetch.index("static bool fetch_configure_stall_watchdog(")]
        self.assertIn("request->connect_timeout_ms < connect_ms", helper,
                      "connect_timeout_ms may only tighten the default.")

    def test_blocking_transfers_carry_a_stall_watchdog(self):
        fetch = without_comments(
            (ROOT / "src/fetch.c").read_text(encoding="utf-8"))
        self.assertIn(
            "#define FETCH_STALL_LOW_SPEED_BYTES_PER_SECOND 1L", fetch)
        self.assertIn("#define FETCH_STALL_LOW_SPEED_SECONDS 6L", fetch)
        watchdog = fetch[
            fetch.index("static bool fetch_configure_stall_watchdog("):
            fetch.index("static bool fetch_try_tls12_compatibility(")]
        self.assertIn("CURLOPT_LOW_SPEED_LIMIT", watchdog)
        self.assertIn("CURLOPT_LOW_SPEED_TIME", watchdog)
        transport = without_comments(
            (ROOT / "src/fetch/transport.inc").read_text(encoding="utf-8"))
        self.assertIn("!fetch_configure_stall_watchdog(easy)", transport)

    def test_youtube_resolution_caps_each_attempt(self):
        resolver = without_comments(
            (ROOT / "src/youtube_resolver.c").read_text(encoding="utf-8"))
        self.assertIn(
            "#define YOUTUBE_RESOLVER_ATTEMPT_TIMEOUT_MS 10000L", resolver)
        self.assertEqual(
            6, resolver.count("youtube_attempt_timeout_ms(") - 1,
            "Both synchronous/replay and pumpable/device resolver paths "
            "must cap their client-profile, watch, and enriched-player "
            "attempts rather than inherit the whole phase deadline.")
        self.assertNotIn(
            "remaining_ms, cancel, cancel_opaque,", resolver)

    def test_youtube_device_resolution_is_pumpable_over_worker_chunks(self):
        resolver = without_comments(
            (ROOT / "src/youtube_resolver.c").read_text(encoding="utf-8"))
        pump = resolver[
            resolver.index("YoutubeResolveJobStatus youtube_resolve_job_pump("):
            resolver.index("bool youtube_resolve_job_take(")]
        self.assertIn(
            "fetch_background_transport_enqueue_media_stream_diagnosed(",
            resolver)
        self.assertIn("fetch_background_transport_take_chunk(", resolver)
        self.assertIn("YOUTUBE_RESOLVE_PHASE_DIRECT_WAIT", pump)
        self.assertIn("YOUTUBE_RESOLVE_PHASE_WATCH_WAIT", pump)
        self.assertIn("YOUTUBE_RESOLVE_PHASE_ENRICHED_WAIT", pump)
        session = without_comments(
            psp_media_session_sources())
        open_start = session.index(
            "static bool psp_media_open_pump_step(",
            session.index("bool psp_media_open_pump(") + 1)
        open_step = session[
            open_start:session.index(
                "bool psp_media_open_work_pending(", open_start)]
        self.assertIn("youtube_resolve_job_pump(", open_step)
        self.assertIn("YOUTUBE_RESOLVE_JOB_PENDING", open_step)
        self.assertIn("fetch_background_transport_available()", open_step)
        self.assertIn("youtube_lite_resolver_identity_get(", resolver)
        self.assertIn("enriched_from_cached_identity", resolver)
        self.assertIn("youtube_resolve_job_take_watch_prefix(", resolver)
        self.assertIn("watch configuration prefix complete", resolver)
        start = resolver[
            resolver.index(
                "static YoutubeRequestStartStatus "
                "youtube_resolve_job_start_request("):
            resolver.index("static bool youtube_resolve_job_poll_response(")]
        self.assertIn("FetchRequest transport_request = *request", start)
        self.assertIn("transport_request.cookie_session = NULL", start)
        self.assertIn("transport_request.cookie_context = NULL", start)
        self.assertIn("url, &transport_request", start)
        self.assertIn("YOUTUBE_REQUEST_START_DEFERRED", start)
        self.assertIn("FETCH_BACKGROUND_ENQUEUE_SATURATED", start)
        self.assertIn("FETCH_BACKGROUND_ENQUEUE_ADMISSION_CLOSED", start)
        self.assertNotIn("queue unavailable", start)

    def test_youtube_watch_requests_share_redirect_and_linear_scan_policy(self):
        resolver = without_comments(
            (ROOT / "src/youtube_resolver.c").read_text(encoding="utf-8"))
        sync_start = resolver.index(
            "bool youtube_resolve_progressive_mp4_cancelable_with_preferences(")
        request_start = resolver.index("FetchRequest watch_request", sync_start)
        sync_request = resolver[request_start:request_start + 1600]
        self.assertIn(".redirect_same_origin_only = true", sync_request)
        self.assertIn(
            ".redirect_url_validator = youtube_resolver_url_supported",
            sync_request)
        prefix = resolver[
            resolver.index("static bool youtube_resolve_job_take_watch_prefix("):
            resolver.index("static bool youtube_resolve_job_prepare_watch(")]
        self.assertIn("watch_scan_offset", prefix)
        self.assertIn("YOUTUBE_WATCH_SCAN_OVERLAP", prefix)
        self.assertNotIn(
            "job->response.data, job->response.length, \"VISITOR_DATA\"",
            prefix)

    def test_native_track_changes_preserve_playback_ownership(self):
        session = without_comments(
            (ROOT / "src/psp_media_session.c").read_text(encoding="utf-8"))
        audio = session[
            session.index("static void psp_media_request_track_reopen("):
            session.index("static void psp_media_reset_subtitle_document(")]
        self.assertIn("psp_media_recovery_position_us(media)", audio)
        self.assertIn("psp_media_machine_wants_playing(media)", audio)
        self.assertLess(
            audio.index('"track-switch-close"'),
            audio.index("psp_media_pipeline_destroy(media)"))
        self.assertLess(
            audio.index("psp_media_pipeline_destroy(media)"),
            audio.index('"track-switch-open"'))
        self.assertIn("media->reopen_resume_playing = resume_playing", audio)

        captions = session[
            session.index("static void psp_media_request_caption_resolution("):
            session.index("void psp_media_execute_intent(")]
        self.assertIn("youtube_caption_catalog_url", captions)
        self.assertIn(
            "media->caption_resolution_pending = catalog_url == NULL",
            captions)
        self.assertIn("psp_media_reset_subtitle_document", captions)
        self.assertNotIn("psp_media_pipeline_destroy", captions)
        pump = session[
            session.index("static bool psp_media_subtitle_load_pump("):
            session.index("static bool psp_media_refusal_reset_recover(")]
        self.assertIn("youtube_resolve_job_take_captions", pump)
        self.assertIn("psp_media_publish_track_catalog", pump)
        self.assertNotIn("psp_media_pipeline_destroy", pump)
        self.assertNotIn(
            "media->machine.state != PSP_MEDIA_SESSION_PAUSED", pump)
        self.assertIn("PSP_MEDIA_SUBTITLE_PARSE_BYTES", pump)
        self.assertNotIn("for (unsigned unit", pump)
        self.assertIn("PSP_MEDIA_SUBTITLE_RESERVE_US", pump)
        self.assertIn("PSP_MEDIA_SUBTITLE_FRAME_ADMISSION_US", pump)
        self.assertIn("psp_media_source_refilling(media)", pump)

        advance = session[session.index("bool psp_media_advance("):]
        subtitle_pump = advance.index(
            "psp_media_subtitle_load_pump(media)")
        self.assertGreater(
            subtitle_pump,
            advance.index("media_playback_advance_bounded_cancelable("))
        self.assertGreater(
            subtitle_pump,
            advance.index("psp_ui_media_set_buffering("))
        self.assertGreater(
            advance.index("psp_media_subtitle_update_display(media)"),
            subtitle_pump)

        opening = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        resolver_catalog = opening[
            opening.index("YoutubeCaptionCatalog *catalog ="):
            opening.index(
                "youtube_resolve_job_destroy(media->resolver_job)",
                opening.index("YoutubeCaptionCatalog *catalog ="))]
        self.assertIn(
            "budget_free(media->budget, media->caption_catalog)",
            resolver_catalog)

    def test_youtube_result_preresolution_is_bounded_and_adopted(self):
        frontend = without_comments(
            (ROOT / "src/psp_app/psp_app_youtube.c").read_text(
                encoding="utf-8"))
        internal = without_comments(
            (ROOT / "src/psp_app/psp_app_internal.h").read_text(
                encoding="utf-8"))
        actions = without_comments(
            (ROOT / "src/psp_app/psp_app_actions.c").read_text(
                encoding="utf-8"))
        session = without_comments(
            (ROOT / "src/psp_media_session.c").read_text(
                encoding="utf-8"))
        opening = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))

        self.assertIn("YoutubeResolveJob *job", internal)
        self.assertIn("PSP_YOUTUBE_PRERESOLVE_DWELL_US", frontend)
        self.assertIn("PSP_YOUTUBE_PRERESOLVE_RETENTION_US", frontend)
        self.assertIn("PSP_YOUTUBE_PRERESOLVE_PENDING_ADOPT_US", frontend)
        self.assertIn("budget_pressure_required(", frontend)
        self.assertIn("youtube_resolve_job_begin_bounded(", frontend)
        self.assertIn("PSP_YOUTUBE_PRERESOLVE_WATCH_BYTES", frontend)
        self.assertIn("PSP_YOUTUBE_PRERESOLVE_PLAYER_BYTES", frontend)
        self.assertIn(
            "transport.occupied_slots < FETCH_BACKGROUND_REQUEST_LIMIT",
            frontend)
        self.assertNotIn("!transport_capacity || !pump_allowed", frontend)
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        idle_start = main.index(
            "static TILEFINCH_OUT_OF_LINE bool "
            "psp_schedule_page_render_work(")
        idle = main[idle_start:main.index(
            "browser_engine_run_idle_work(", idle_start)]
        self.assertNotIn("browser->youtube_preresolve.state", idle)
        self.assertNotIn("PSP_YOUTUBE_PRERESOLVE_RESOLVING", idle)
        self.assertIn("page_input_active ||", idle)
        self.assertIn("app->browser->media.ui.visible", idle)
        self.assertIn(
            "psp_media_open_work_pending(&app->browser->media)", idle)
        observe = main.index("psp_app_youtube_preresolve_tick(")
        idle_schedule = main.index(
            "psp_schedule_page_render_work(", observe)
        self.assertLess(observe, idle_schedule)
        present = main.index(
            "bool published = psp_present(", idle_schedule)
        after_present = main.index(
            "psp_deferred_image_after_present(", present)
        self.assertLess(present, after_present)
        helper = main.index("psp_deferred_image_after_present(")
        image_pump = main.index(
            "browser_engine_run_deferred_image_work(", helper)
        post_present_guard = main[helper:image_pump]
        self.assertIn("page_idle_pumped", post_present_guard)
        self.assertIn("render_job_pending", post_present_guard)
        self.assertIn("input_active", post_present_guard)
        self.assertIn("media.ui.visible", post_present_guard)
        self.assertIn(
            "browser_engine_prepare_focused_provider_media_thumbnail(",
            frontend)
        self.assertIn("PSP_YOUTUBE_PRERESOLVE_THUMBNAIL", frontend)
        self.assertIn("observed_focus_index", internal)
        self.assertIn("observed_focus_kind", internal)
        self.assertIn("PSP_YOUTUBE_PRERESOLVE_DWELL", frontend)
        self.assertIn("youtube_resolve_job_pump(", frontend)
        self.assertIn("youtube_resolve_job_cancel(", frontend)
        self.assertNotIn("media_http_", frontend)
        self.assertNotIn("googlevideo", frontend.lower())
        self.assertIn("observed_focus_moves", frontend)
        self.assertIn("psp_youtube_focused_video_id(", frontend)
        self.assertIn(
            "psp_media_open_provider_route_prepared(", actions)
        self.assertIn("prepared_resolver_job = *prepared_resolver_job", session)
        self.assertIn("youtube_resolve_job_matches(", session)
        self.assertIn("*prepared_resolver_job = NULL", session)
        self.assertIn(
            "youtube_resolve_job_destroy(media->prepared_resolver_job)",
            session)

        service_start = opening.index("if (media->open_service_pending)")
        service = opening[
            service_start:opening.index(
                "if (media->job_phase < PSP_MEDIA_JOB_OPEN_RESOLVE",
                service_start)]
        self.assertLess(
            service.index("media->prepared_resolver_job = NULL"),
            service.index("psp_media_pipeline_destroy(media)"))
        self.assertLess(
            service.index("psp_media_pipeline_destroy(media)"),
            service.index("media->resolver_job = prepared_resolver"))

    def test_video_open_prioritizes_media_and_does_not_wait_for_readahead(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        route_start = main.index("bool media_open_was_pending =")
        route = main[
            route_start:
            main.index("psp_log_set_phase(PSP_LOG_PHASE_MEDIA)", route_start)]
        self.assertLess(
            route.index("psp_media_prepare_route("),
            route.index("browser_engine_cancel_network_work("))
        self.assertIn('"video selected"', route)

        open_source = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        pump_declaration = open_source.index(
            "static bool psp_media_open_pump_step(")
        pump_start = open_source.index(
            "static bool psp_media_open_pump_step(", pump_declaration + 1)
        prime_start = open_source.index(
            "case PSP_MEDIA_JOB_OPEN_VIDEO_PRIME:", pump_start)
        prime = open_source[
            prime_start:
            open_source.index(
                "case PSP_MEDIA_JOB_OPEN_AUDIO_RANGE:", prime_start)]
        self.assertIn(
            "ok = primed != MEDIA_HTTP_RANGE_PRIME_FAILED", prime)
        self.assertNotIn(
            "primed == MEDIA_HTTP_RANGE_PRIME_PENDING) break", prime)

    def test_codec_watchdog_never_forges_a_worker_completion(self):
        backend = without_comments(
            (ROOT / "src/media_backend_psp.c").read_text(encoding="utf-8"))
        collect = backend[
            backend.index("static int psp_media_collect_codec_job("):
            backend.index("static MediaBackendResult psp_media_queue_codec_job(")]
        self.assertIn("codec_watchdog_candidate_started_us", collect)
        self.assertIn("confirmed_started_us != started_us", collect)
        self.assertIn("return -1", collect)
        watchdog = collect[
            collect.index("running_us >= PSP_MEDIA_CODEC_JOB_WATCHDOG_US"):
            collect.index("if (state == PSP_MEDIA_CODEC_JOB_RUNNING) return 1")]
        self.assertNotIn(
            "codec_job_state, PSP_MEDIA_CODEC_JOB_DONE", watchdog,
            "Only the codec worker/death path may publish a completion; a "
            "timeout cannot steal fields from firmware that may still return.")

    def test_media_trickle_retry_forces_a_new_connection(self):
        media_http = without_comments(
            (ROOT / "src/media_http.c").read_text(encoding="utf-8"))
        prepare = media_http[
            media_http.index("static bool range_prepare_request("):
            media_http.index("static bool range_admit_values(")]
        self.assertIn(
            ".force_fresh_connection = range->window_tracker.reconnects != 0u",
            prepare)
        transport = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        configure = transport[
            transport.index("static bool fetch_background_configure_easy("):
            transport.index("static int fetch_background_find_buffer(")]
        self.assertIn("CURLOPT_FRESH_CONNECT", configure)
        self.assertIn("slot->force_fresh_connection ? 1L : 0L", configure)
        self.assertGreaterEqual(
            transport.count(
                "slot->force_fresh_connection = request->force_fresh_connection;"
            ),
            2,
            "streaming and fixed-window requests must both preserve the flag",
        )

    def test_background_streaming_keeps_budget_and_parsing_on_browser_thread(self):
        source = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        worker = source[
            source.index("static int fetch_background_worker_main("):
            source.index("bool fetch_background_transport_available(")]
        self.assertNotIn("budget_", worker)
        self.assertIn("FETCH_BACKGROUND_STREAM_CHUNK_TARGET", source)
        self.assertIn("FETCH_BACKGROUND_CHUNK_READY", source)
        self.assertIn("CURL_WRITEFUNC_PAUSE", source)
        expire = source[source.index("static void fetch_background_expire_active_deadlines("):
                        source.index("static bool fetch_background_has_worker_work(")]
        self.assertIn("fetch_background_deadline_eligible(", expire)
        self.assertIn("slot->transfer_done, slot->curl_paused", expire)
        receive = source[source.index("static size_t fetch_background_receive_body("):
                         source.index("static int fetch_background_transfer_progress(")]
        self.assertLess(receive.index("slot->pause_started_us = slot->last_progress_us;",
                                      receive.index("slot->length >= slot->stream_chunk_target")),
                        receive.index("FETCH_BACKGROUND_CHUNK_READY"))
        take = source[
            source.index("bool fetch_background_transport_take_chunk("):
            source.index("static void fetch_background_release_slot(")]
        self.assertIn("FETCH_BACKGROUND_CHUNK_TAKING", take)
        self.assertLess(take.index("memcpy("), take.index("CHUNK_EMPTY"))
        admission = source[
            source.index(
                "bool fetch_background_transport_stream_shape_supported("):
            source.index("#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)")]
        self.assertIn("request->redirect_same_origin_only", admission)
        self.assertIn("request->redirect_url_validator != NULL", admission)
        native_admission = source[
            source.index("static bool fetch_background_stream_request_supported("):
            source.index("uint64_t fetch_background_transport_enqueue_stream(")]
        self.assertIn(
            "fetch_background_transport_stream_shape_supported(",
            native_admission)
        # Page/resolver streams retain the worker's coarse throughput quantum;
        # only media range successors opt into the measured 16 KiB latency
        # policy. A global constant change would regress navigation throughput.
        normal = source[
            source.index("uint64_t fetch_background_transport_enqueue_stream("):
            source.index("uint64_t fetch_background_transport_enqueue_hop_stream(")]
        self.assertIn("FETCH_BACKGROUND_STREAM_CHUNK_TARGET", normal)
        media_http = without_comments(
            (ROOT / "src/media_http.c").read_text(encoding="utf-8"))
        self.assertIn(
            "fetch_background_transport_enqueue_media_stream_sized_diagnosed(",
            media_http)
        session = without_comments(
            psp_media_session_sources())
        self.assertIn(".stream_publication_bytes = 16u * KIB", session)

    def test_media_transport_reserves_foreground_admission(self):
        background = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        policy = without_comments(
            (ROOT / "src/fetch/background_slot_policy.h").read_text(
                encoding="utf-8"))
        resolver = without_comments(
            (ROOT / "src/youtube_resolver.c").read_text(encoding="utf-8"))
        media_http = without_comments(
            (ROOT / "src/media_http.c").read_text(encoding="utf-8"))
        session = without_comments(psp_media_session_sources())
        self.assertIn("FETCH_BACKGROUND_MEDIA_RESERVED_SLOTS 2u", policy)
        self.assertIn("fetch_background_admission_slot_limit(", background)
        self.assertIn(
            "fetch_background_transport_enqueue_media_stream_diagnosed(",
            resolver)
        self.assertIn(
            "fetch_background_transport_enqueue_media_stream_sized_diagnosed(",
            media_http)
        self.assertIn(
            "fetch_background_transport_enqueue_media_diagnosed(", media_http)
        self.assertIn(
            "fetch_background_transport_set_media_priority(active)", session)
        self.assertIn(
            "psp_media_set_transport_priority(media, true)", session)
        self.assertIn(
            "psp_media_set_transport_priority(media, false)", session)
        failure = session[
            session.index("void psp_media_raise_error("):
            session.index("void psp_media_retire_first_frame(")]
        self.assertIn(
            "psp_media_set_transport_priority(media, false)", failure)
        route = session[
            session.index("static void psp_media_prepare_route_kind("):
            session.index("void psp_media_prepare_route(")]
        route_prefix = route[:route.index("if (!online_route")]
        self.assertNotIn(
            "psp_media_set_transport_priority(media, true)", route_prefix,
            "Recognizing the incumbent watch URL runs every frame and must "
            "not reacquire media slots after Close or a failed open.")
        internal_hide = route[
            route.index('"internal-view-hide"'):
            route.index('"internal-view-hidden"')]
        self.assertIn(
            "psp_media_set_transport_priority(media, false)",
            internal_hide)
        close = session[
            session.index("void psp_media_close("):
            session.index("bool psp_media_reclaim_hidden_pipeline(")]
        self.assertIn(
            "psp_media_set_transport_priority(media, false)", close)
        reclaim = session[
            session.index("bool psp_media_reclaim_hidden_pipeline("):
            session.index(
                "bool psp_media_reclaim_hidden_pipeline_for_navigation(")]
        self.assertIn(
            "psp_media_set_transport_priority(media, false)", reclaim)
        opening = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        open_pump = opening[
            opening.rindex("static bool psp_media_open_pump_step("):
            opening.index("bool psp_media_open_work_pending(")]
        self.assertIn("psp_media_set_transport_priority(", open_pump)

    def test_provider_handoff_retires_page_work_before_resolver_progress(self):
        background = without_comments(
            (ROOT / "src/fetch/background_transport.inc").read_text(
                encoding="utf-8"))
        retire = background[
            background.index("static void fetch_background_retire_cancelled("):
            background.index("static void fetch_background_resume_streams(")]
        self.assertIn("FETCH_BACKGROUND_SLOT_RUNNING", retire)
        self.assertIn("fetch_background_slot_release_curl(slot)", retire)
        self.assertIn("CURLE_ABORTED_BY_CALLBACK", retire)
        self.assertIn("claimed_ready", retire)
        self.assertLess(
            retire.index("atomic_compare_exchange_strong_explicit("),
            retire.index("slot->length = 0"),
            "Cancellation must atomically claim a READY stream chunk before "
            "clearing the browser thread's memcpy source.")
        worker = background[
            background.index("static int fetch_background_worker_main("):
            background.index("bool fetch_background_transport_available(")]
        self.assertLess(
            worker.index("fetch_background_retire_cancelled()"),
            worker.index("curl_multi_perform("),
            "A cancelled thumbnail in DNS/TCP/TLS setup must be removed "
            "before the next potentially blocking curl call.")

        actions = without_comments(
            (ROOT / "src/psp_app/psp_app_actions.c").read_text(
                encoding="utf-8"))
        handoff = actions[
            actions.index("bool opened = entry != NULL"):
            actions.index("action.prefer_native_media = false;")]
        opened = handoff[handoff.index("if (opened) {"):]
        self.assertIn("browser_engine_cancel_network_work(", opened)
        self.assertIn("provider_handoff_present_pending", opened)
        self.assertLess(
            opened.index("browser_engine_prepare_optional_memory_reclaim("),
            opened.index("provider_handoff_present_pending ="))
        self.assertIn(
            "provider_handoff_present_pending =\n"
            "                            true;",
            opened)
        self.assertIn(
            "browser_engine_prepare_optional_memory_reclaim(", opened)
        self.assertNotIn("browser_engine_reclaim_optional_memory(", opened)

        text_input = without_comments(
            (ROOT / "src/psp_text_input.c").read_text(encoding="utf-8"))
        request = text_input[
            text_input.index("bool psp_text_input_request_with_submit("):
            text_input.index("bool psp_text_input_request_voice(")]
        self.assertLess(
            request.index("present(service, frame, ui);"),
            request.index("text_input_retire_modal_latch();"),
            "The modal's final presentation can sample input; retire its "
            "latch only after that last modal-owned frame.")

        loop = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        media_pump = loop[
            loop.index("bool media_open_before ="):
            loop.index("if (browser->media.page_source", loop.index(
                "bool media_open_before ="))]
        self.assertIn(
            "!interactive->provider_handoff_present_pending", media_pump)
        self.assertIn(
            "browser_engine_optional_memory_reclaim_pending(", media_pump)
        media_scope = loop[
            loop.index("if (media_open_before && !navigation_still_pending"):
            loop.index("bool media_decode_scope = false;")]
        self.assertIn(
            "!interactive->provider_handoff_present_pending", media_scope)
        self.assertIn(
            "browser_engine_optional_memory_reclaim_pending(", media_scope)
        runtime = loop[
            loop.index("bool runtime_layout_changed = false;"):
            loop.index("ScriptMediaRequest dom_media_request;")]
        self.assertIn(
            "psp_advance_page_runtime_with_reader_recovery(", runtime)
        runtime_helper = loop[
            loop.index("static TILEFINCH_OUT_OF_LINE bool "
                       "psp_advance_page_runtime("):
            loop.index("static TILEFINCH_HOT_BOUNDARY "
                       "PspInteractiveResult psp_app_run_interactive(")]
        self.assertIn("provider_handoff_present_pending", runtime_helper)
        self.assertIn(
            "browser_engine_optional_memory_reclaim_pending(", runtime_helper)
        self.assertLess(
            runtime_helper.index(
                "browser_engine_optional_memory_reclaim_pending("),
            runtime_helper.index("browser_engine_advance_runtime("))
        present = loop[
            loop.index("if (frame.page_dirty || render_visual_state != 0"):
            loop.index("if (psp_deferred_image_after_present(")]
        condition = present[:present.index("if (!psp_navigation_cooperate_supervised())")]
        self.assertIn("interactive->provider_handoff_present_pending", condition,
                      "A cancelled page paint must not consume the player's presentation wakeup")
        self.assertLess(
            present.index("bool published = psp_present("),
            present.index("psp_app_pump_provider_handoff_reclaim("))
        self.assertIn("&app, frame.ui_sample_us, published", present,
                      "Only a successful publication may release the loading handoff")

        reclaim = actions[
            actions.index(
                "void psp_app_pump_provider_handoff_reclaim("):]
        self.assertIn(
            "browser_engine_pump_optional_memory_reclaim(", reclaim)
        self.assertNotIn("browser_engine_reclaim_optional_memory(", reclaim)

    def test_audio_open_precedes_aggressive_video_successor(self):
        opening = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        open_step = opening.rindex("static bool psp_media_open_pump_step(")
        video_demux = opening[
            opening.index("case PSP_MEDIA_JOB_OPEN_VIDEO_DEMUX:", open_step):
            opening.index("case PSP_MEDIA_JOB_OPEN_VIDEO_PRIME:", open_step)]
        audio_demux = opening[
            opening.index("case PSP_MEDIA_JOB_OPEN_AUDIO_DEMUX:", open_step):
            opening.index("case PSP_MEDIA_JOB_OPEN_DECODER_PREPARE:", open_step)]
        self.assertIn("? PSP_MEDIA_JOB_OPEN_AUDIO_RANGE", video_demux)
        self.assertIn("PSP_MEDIA_JOB_OPEN_VIDEO_PRIME", audio_demux)

    def test_exhausted_successor_stall_replaces_the_candidate(self):
        http = without_comments(
            (ROOT / "src/media_http.c").read_text(encoding="utf-8"))
        restart = http[
            http.index("static bool range_restart_stalled_fill("):
            http.index("static void range_watch_window_liveness(")]
        self.assertIn("range_window_fail(range, reason)", restart)
        terminal = http[
            http.index("static void range_window_fail("):
            http.index("static size_t range_fill_progress(")]
        self.assertIn("range->window_state = RANGE_WINDOW_FAILED", terminal)
        self.assertIn("range->fill_stall_exhausted = true", terminal)
        self.assertIn("stalled_reconnect_exhaustions++", terminal)
        session = without_comments(psp_media_session_sources())
        first_frame = session[
            session.index("PspMediaFirstFrameVerdict verdict"):
            session.index("if (was_pending != media->decode_job_pending)")]
        self.assertIn("psp_media_delivery_candidate_stalled(media)", first_frame)
        self.assertIn("psp_media_retry_delivery_failure(", first_frame)
        self.assertIn('"first-frame-stalled-candidate"', first_frame)

    def test_open_side_media_cancellation_uses_the_callers_token(self):
        session = without_comments(
            psp_media_session_sources())
        requested = session[
            session.index("bool psp_media_cancel_requested("):
            session.index("bool psp_media_cancel_callback(")]
        self.assertIn(
            "tilefinch_cancellation_requested(media->open_cancellation)",
            requested,
            "Open-side cancellation must not depend only on the "
            "process-global scope-ownership invariant.")
        pump = session[
            session.index("bool psp_media_open_pump("):
            session.index("static bool psp_media_open_pump_step(",
                          session.index("bool psp_media_open_pump("))]
        self.assertIn("media->open_cancellation = cancellation;", pump)
        self.assertIn("media->open_cancellation = NULL;", pump)
        advance = session[session.index("bool psp_media_advance("):]
        self.assertIn(
            "psp_media_open_pump(media, cancellation)", advance)

    def test_psp_dns_resolution_is_bounded(self):
        network = without_comments(
            (ROOT / "src/psp_network.c").read_text(encoding="utf-8"))
        self.assertIn("struct hostent *gethostbyname(const char *name)",
                      network,
                      "curl's synchronous resolver calls gethostbyname and "
                      "cannot time it out, and PSPSDK's implementation "
                      "hardcodes 2s x 4 attempts, so the browser owns this "
                      "symbol and its bound.")
        self.assertIn("#define PSP_DNS_TIMEOUT_SECONDS 2u", network)
        self.assertIn("#define PSP_DNS_RETRIES 1", network)
        resolve = network[
            network.index("struct hostent *gethostbyname(const char *name)"):]
        self.assertIn(
            "PSP_DNS_TIMEOUT_SECONDS, PSP_DNS_RETRIES", resolve)
        self.assertIn("sceNetResolverDelete(resolver_id)", resolve)
        self.assertLess(
            resolve.index("sceNetResolverDelete(resolver_id)"),
            resolve.index("if (resolved < 0) return NULL;"),
            "The resolver object must be released on the failure path too.")

    def test_exit_handoff_uses_the_launcher_load_recipe(self):
        handoff = without_comments(
            (ROOT / "src/psp_app/psp_app_exit_handoff.c")
            .read_text(encoding="utf-8"))
        # The CFW VSH memory-stick call is the one that can start another
        # homebrew on post-1.xx firmware; the published API call is kept
        # after it for PPSSPP and for anything without SystemCtrl. Order is
        # the contract: a successful VSH call never returns.
        self.assertIn("sctrlKernelLoadExecVSHMs2(target, &cfw_parameters)",
                      handoff)
        self.assertIn("sceKernelLoadExec(target, &fallback_parameters)",
                      handoff)
        self.assertLess(
            handoff.index("sctrlKernelLoadExecVSHMs2("),
            handoff.index("sceKernelLoadExec("),
            "The CFW handoff must be attempted before the standard call.")
        self.assertEqual(
            2, handoff.count('.key = "game"'),
            "Both parameter blocks name the game key the firmware expects "
            "for a Memory Stick homebrew.")
        self.assertIn(".unk5 = 0x10000", handoff)
        # Production restart-to-update needs the same CFW-safe load recipe.
        # Only the arbitrary validation exit-to wrapper remains guarded.
        self.assertIn("#ifdef TILEFINCH_PSP_VALIDATION_LOG", handoff)
        self.assertLess(
            handoff.index("bool psp_load_exec_eboot("),
            handoff.index("#ifdef TILEFINCH_PSP_VALIDATION_LOG"))
        self.assertGreater(
            handoff.index("bool psp_exit_handoff("),
            handoff.index("#ifdef TILEFINCH_PSP_VALIDATION_LOG"))
        targets = (ROOT / "cmake/TilefinchTargets.cmake").read_text(
            encoding="utf-8")
        browser_sources = targets[
            targets.index("add_executable(psp-browser-script"):
            targets.index("target_link_libraries(psp-browser-script PRIVATE")]
        self.assertIn("src/psp_app/psp_app_exit_handoff.c", browser_sources)

    def test_launcher_fast_path_keeps_safe_start_and_symbols(self):
        launcher = without_comments(
            (ROOT / "src/update_launcher_psp.c").read_text(encoding="utf-8"))
        self.assertIn(
            "#define LAUNCHER_SAFE_START_WINDOW_US UINT64_C(80000)",
            launcher)
        self.assertIn(
            "#define LAUNCHER_SAFE_START_MAXIMUM_US UINT64_C(160000)",
            launcher)
        self.assertIn("launcher_sample_button(", launcher)
        self.assertIn("&& sample_observed", launcher)
        self.assertIn('"HOLD L WHILE STARTING"', launcher)
        self.assertNotIn("launcher_wait_held", launcher)
        self.assertLess(
            launcher.index("tilefinch_update_journal_load("),
            launcher.index(
                "recovery_button = launcher_finish_safe_start_window("),
            "The bounded journal read should overlap the safe-start window.")
        missing_state = launcher[
            launcher.index("if (!tilefinch_update_journal_load("):
            launcher.index('LAUNCHER_TIMING_MARK("journal-ready")')]
        self.assertNotIn(
            "tilefinch_update_journal_store(", missing_state,
            "First install must not synchronously flush a default journal "
            "before handoff.")

        targets = (ROOT / "cmake/TilefinchTargets.cmake").read_text(
            encoding="utf-8")
        launcher_target = targets[
            targets.index("add_executable(tilefinch-launcher"):
            targets.index("add_executable(psp-browser-fixture")]
        self.assertIn("TILEFINCH_PSP_LAUNCHER_TIMING=1", launcher_target)
        self.assertIn("$<TARGET_FILE:tilefinch-launcher>.unstripped",
                      launcher_target)
        self.assertIn('"${PSPDEV}/bin/psp-strip"', launcher_target)
        self.assertLess(
            launcher_target.index('"${PSPDEV}/bin/psp-strip"'),
            launcher_target.index("create_pbp_file("),
            "The launcher must be stripped before create_pbp_file packages it.")

    def test_every_process_exit_hands_the_console_back(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        # A device run driven from a host has nobody standing at the PSP: an
        # exit that lands on the XMB strands the remote loop until the console
        # is power-cycled by hand. Three validation qualification modes exited
        # without the handoff for exactly that reason, so the rule is now
        # structural -- one funnel, checked here. If some future exit
        # legitimately must not hand off, add it to EXEMPT with the reason
        # rather than deleting this test.
        EXEMPT = ()
        sites = [
            match.start()
            for match in re.finditer(r"sceKernelExitGame\s*\(", main)]
        self.assertEqual(
            len(sites), len(EXEMPT) + 1,
            "every process exit must go through psp_exit_console(), which is "
            "the only site allowed to call sceKernelExitGame()")
        preceding = main[max(0, sites[0] - 400):sites[0]]
        self.assertIn("psp_exit_handoff(exit_to)", preceding)
        self.assertIn("#ifdef TILEFINCH_PSP_VALIDATION_LOG", preceding)
        # Every mode that stops the process reaches the funnel: the six
        # validation qualifications plus the ordinary clean exit. Scan past the
        # funnel's own definition, which is above every caller.
        callers = re.findall(
            r"psp_exit_console\(([^;]*)\);", main[sites[0]:])
        self.assertEqual(
            sorted(argument.strip() for argument in callers),
            sorted([
                "process.config.exit_to",
                "config->exit_to",
                "exit_to",
                "exit_to",
                "exit_to",
                "exit_to",
                "restart_launcher ? NULL : process.config.exit_to",
            ]),
            "a qualification mode that exits must pass the configured "
            "exit_to through; only a restart request may suppress it")
        # The halt path does not exit at all, and keeps its own handoff.
        halted = main[main.index('psp_log_finish("halted");'):
                      main.index("sceKernelSleepThread();")]
        self.assertIn("psp_exit_handoff(process.config.exit_to)", halted)
        self.assertIn("#ifdef TILEFINCH_PSP_VALIDATION_LOG", halted)

    def test_exit_handoff_runs_after_the_log_is_durable(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        # The load replaces the process, so anything the log still owes the
        # card at that point is lost. Every exit must have finished its log
        # before it reaches the handoff.
        callers = main[main.index("sceKernelExitGame"):]
        self.assertTrue(re.search(r"psp_exit_console\(", callers))
        close = main[
            main.index("static TILEFINCH_COLD_PATH PspShutdownReport "
                       "psp_browser_close("):
            main.index("\nint main(")]
        self.assertIn("psp_log_finish(\"clean-exit\")", close)
        for match in re.finditer(r"psp_exit_console\(", callers):
            preceding = callers[max(0, match.start() - 1400):match.start()]
            if "restart_launcher ? NULL" in callers[
                    match.start():match.start() + 160]:
                self.assertIn(
                    "psp_browser_close(", preceding,
                    "the ordinary exit must complete coordinated teardown "
                    "and its durable log before the handoff")
            else:
                self.assertIn(
                    "psp_log_finish(", preceding,
                    "psp_exit_console() must follow psp_log_finish(): the "
                    "handoff load replaces the process")

    def test_unread_deferred_cache_is_never_replaced_at_exit(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        gate = main[
            main.index("static TILEFINCH_OUT_OF_LINE bool "
                       "psp_site_data_restore_gate_navigation("):
            main.index("static void psp_site_data_restore_destroy(")]
        self.assertIn("cache_restore_cancelled_unread = true", gate)
        self.assertIn("restore->phase = PSP_SITE_DATA_RESTORE_DONE", gate)

        save = main[
            main.index("static TILEFINCH_COLD_PATH bool "
                       "psp_save_site_data_on_exit("):
            main.index("static void psp_site_data_restore_init(")]
        self.assertIn("bool preserve_persisted_cache", save)
        self.assertIn("&& !preserve_persisted_cache", save)

        close = main[
            main.index("static TILEFINCH_COLD_PATH PspShutdownReport "
                       "psp_browser_close("):
            main.index("\nint main(")]
        preserve = close.index("bool preserve_persisted_cache =")
        destroy = close.index("psp_site_data_restore_destroy(", preserve)
        persist = close.index("psp_save_site_data_on_exit(", destroy)
        self.assertLess(preserve, destroy)
        self.assertLess(destroy, persist)
        self.assertIn("preserve_persisted_cache,", close[persist:persist + 500])

    def test_deferred_cache_idle_work_is_terminal_and_not_watchdog_time(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        idle = main[
            main.index("static TILEFINCH_OUT_OF_LINE bool "
                       "psp_site_data_restore_idle_pump("):
            main.index("typedef struct {", main.index(
                "psp_site_data_restore_idle_pump("))]
        terminal = idle.index("PSP_SITE_DATA_RESTORE_DONE) return false")
        veto_chain = idle.index("bool cache_restore_may_run")
        self.assertLess(terminal, veto_chain)

        navigation = main[
            main.index("bool site_data_waiting ="):
            main.index("if (navigation_status == BROWSER_NAVIGATION_JOB_PENDING", main.index(
                "bool site_data_waiting ="))]
        waiting = navigation[
            navigation.index("if (site_data_waiting)"):]
        self.assertIn("navigation_job_started_us = now_us", waiting)

    def test_media_overlay_has_no_unreachable_close_control(self):
        ui = without_comments(
            (ROOT / "src/psp_ui.c").read_text(encoding="utf-8"))
        activate = ui[
            ui.index("PspUiMediaIntent psp_ui_media_activate_at("):
            ui.index("void psp_ui_media_composite_with_preview(")]
        self.assertNotIn(
            "x >= width - 42 && y < 42", activate,
            "The overlay's top-right close corner is unreachable on a "
            "buttons-only device; only the host harness could click it.")
        composite = ui[
            ui.index("void psp_ui_media_composite_with_preview("):]
        self.assertNotIn('"X"', composite)
        self.assertNotIn("{width - 38, 6, 30, 30}", composite)

    def test_open_phase_completion_keeps_its_physical_service_token(self):
        opening = without_comments(
            (ROOT / "src/psp_media_open.c").read_text(encoding="utf-8"))
        pump = opening[
            opening.index("bool psp_media_open_pump("):
            opening.index("static const char *psp_media_open_stage_name(")]
        snapshot = pump.index("phase_service_command = media->service.command")
        step = pump.index("psp_media_open_pump_step(media)")
        completion = pump.index("PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE", step)
        self.assertLess(snapshot, step)
        guard = pump[step:completion]
        self.assertIn("media->service.command == phase_service_command", guard)
        self.assertIn("media->service.epoch == phase_service_epoch", guard)
        event = pump[completion:completion + 300]
        self.assertIn(".service_command = phase_service_command", event)
        self.assertIn(".service_epoch = phase_service_epoch", event)
        self.assertNotIn("psp_media_service_completion(", event)

    def test_supervisor_media_invalidates_present_records_before_scanout(self):
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        supervisor = runtime[
            runtime.index("static void psp_present_supervisor_media("):
            runtime.index("void psp_present_boot_surface(")]
        forget = supervisor.index("psp_media_present_forget_buffers()")
        self.assertLess(
            forget,
            supervisor.index("psp_display_video_active(&psp_display)"))
        self.assertLess(forget, supervisor.index("memcpy(back, front"))
        self.assertLess(
            forget,
            supervisor.index("memcpy(vram, front"))
        self.assertIn("psp_ui_media_composite_supervisor_565(", supervisor)

    def test_subtitle_only_overlay_invalidates_present_records(self):
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        self.assertGreaterEqual(
            runtime.count(
                "psp_ui_media_overlay_paints(&psp_active_media->ui)"),
            2)
        video_calls = [
            split_arguments(arguments)
            for arguments in call_arguments(
                runtime, "psp_present_media_frame_video")
            if "psp_active_media->frame" in arguments
        ]
        software_calls = [
            split_arguments(arguments)
            for arguments in call_arguments(runtime, "psp_present_media_frame")
            if "psp_active_media->frame" in arguments
        ]
        self.assertEqual(len(video_calls), 1)
        self.assertEqual(len(software_calls), 1)
        self.assertEqual(video_calls[0][2], "overlay_paints")
        self.assertEqual(software_calls[0][2], "overlay_paints")

    def test_present_records_cover_the_three_buffer_software_surface(self):
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        declaration = runtime[
            runtime.index("static PspMediaPresentRecord"):
            runtime.index("static int psp_media_present_reported_mode")]
        self.assertIn(
            "psp_media_present_records[PSP_DISPLAY_PAGE_BUFFER_COUNT]",
            declaration)
        reset = runtime[
            runtime.index("void psp_media_present_forget_buffers(void)"):
            runtime.index("static PspMediaPresentMode", runtime.index(
                "void psp_media_present_forget_buffers(void)"))]
        self.assertIn("PSP_DISPLAY_PAGE_BUFFER_COUNT", reset)
        prepare = runtime[
            runtime.index("static bool psp_media_present_prepare("):
            runtime.index("static bool psp_present_media_frame(")]
        self.assertIn(
            "psp_display.back_buffer % PSP_DISPLAY_PAGE_BUFFER_COUNT",
            prepare)
        self.assertNotIn("back_buffer & 1", prepare)

    def test_navigation_reconciles_the_frontend_render_job_flag(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        helper = main[
            main.index("static TILEFINCH_OUT_OF_LINE bool "
                       "psp_schedule_page_render_work("):
            main.index("#ifdef TILEFINCH_PSP_VALIDATION_LOG", main.index(
                "psp_schedule_page_render_work("))]
        self.assertIn("!browser_engine_navigation_pending(engine)", helper)
        self.assertIn("navigation->page.loaded", helper)
        self.assertIn("browser_engine_render_shell(engine) != NULL", helper)
        self.assertIn("!psp_ui_screen_is_native_surface(", helper)
        self.assertIn("browser_engine_cancel_render_job(engine)", helper)
        self.assertIn("*render_job_pending = false", helper)
        self.assertIn("frame->page_dirty = false", helper)

        deferred = main[
            main.index("static TILEFINCH_OUT_OF_LINE bool "
                       "psp_deferred_image_after_present("):
            main.index("static TILEFINCH_OUT_OF_LINE bool "
                       "psp_schedule_page_render_work(")]
        self.assertIn(
            "browser_engine_render_shell(app->browser->engine) == NULL",
            deferred)
        self.assertIn("!navigation->page.loaded", deferred)
        self.assertIn("psp_ui_screen_is_native_surface(", deferred)

        raster_guard = main[
            main.index("static TILEFINCH_OUT_OF_LINE void "
                       "psp_reconcile_page_render_before_raster("):
            main.index("#ifdef TILEFINCH_PSP_VALIDATION_LOG", main.index(
                "psp_reconcile_page_render_before_raster("))]
        self.assertIn("!*render_job_pending", raster_guard)
        self.assertIn("browser_engine_navigation_pending(engine)", raster_guard)
        self.assertIn("navigation->page.loaded", raster_guard)
        self.assertIn("browser_engine_render_shell(engine) != NULL", raster_guard)
        self.assertIn("psp_ui_screen_is_native_surface(", raster_guard)
        self.assertIn("browser_engine_cancel_render_job(engine)", raster_guard)
        self.assertIn("*render_job_pending = false", raster_guard)
        self.assertIn("frame->page_dirty = false", raster_guard)

        loop = main[
            main.index("PspInteractiveResult psp_app_run_interactive("):]
        reconcile = loop.index("psp_schedule_page_render_work(")
        raster_guard_call = loop.index(
            "psp_reconcile_page_render_before_raster(", reconcile)
        raster = loop.index(
            "&& !psp_ui_page_work_paused(&process->presentation.ui)",
            raster_guard_call)
        self.assertLess(reconcile, raster)
        self.assertLess(raster_guard_call, raster)

    def test_home_tab_rows_reuse_the_tab_action_receiver(self):
        actions = without_comments(
            (ROOT / "src/psp_app/psp_app_actions.c").read_text(
                encoding="utf-8"))
        resolver = actions[
            actions.index("static bool psp_app_resolve_home_tab_action("):
            actions.index("void psp_app_dispatch_action(")]
        self.assertIn("PSP_HOME_TARGET_TAB", resolver)
        self.assertIn("resolved->action = PSP_UI_ACTION_SWITCH_TAB", resolver)
        self.assertIn("resolved->tab_index = target", resolver)
        heavy = actions[
            actions.index("static void psp_app_dispatch_heavy_action("):
            actions.index("case PSP_UI_ACTION_TOGGLE_BOOKMARK:")]
        remap = heavy.index("psp_app_resolve_home_tab_action(")
        switch = heavy.index("switch (intent->action)")
        tab_case = heavy.index("case PSP_UI_ACTION_SWITCH_TAB:")
        self.assertLess(remap, switch)
        self.assertLess(switch, tab_case)
        self.assertIn("psp_tabs_request(", heavy[tab_case:])

    def test_focus_feedback_composes_before_publishing_and_clearing_dirty(self):
        actions = without_comments(
            (ROOT / "src/psp_app/psp_app_actions.c").read_text(
                encoding="utf-8"))
        focus = actions[actions.index("static void psp_app_finish_focus_action("):
                        actions.index("typedef struct", actions.index(
                            "static void psp_app_finish_focus_action("))]
        prepare = focus.index("browser_engine_render_frame_bounded(")
        complete = focus.index("rendered != BROWSER_RENDER_JOB_COMPLETE")
        refresh = focus.index("psp_engine_views_refresh(")
        present = focus.index("psp_present_internal(")
        clean = focus.index("frame->page_dirty = false")
        self.assertLess(prepare, complete)
        self.assertLess(complete, refresh)
        self.assertLess(refresh, present)
        self.assertLess(present, clean)
        self.assertIn("return;", focus[complete:present])
        self.assertIn("PSP_RENDER_JOB_BUDGET_US, 4u", focus)
        self.assertNotIn("browser_engine_render_frame(", focus)

    def test_focus_probe_waits_for_new_frame_and_successful_publication(self):
        header = (ROOT / "src/psp_app/psp_app_internal.h").read_text(encoding="utf-8")
        self.assertIn("#define PSP_RENDER_JOB_BUDGET_US 2000u", header)
        self.assertIn("#define PSP_RENDER_JOB_MAXIMUM_TILES 4u", header)
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(encoding="utf-8"))
        probe = runtime[runtime.index("static void psp_focus_feedback_published("):
                        runtime.index("bool psp_present_internal(")]
        self.assertIn("navigation->generation != psp_focus_feedback.generation", probe)
        self.assertIn("render->frames_rendered <= psp_focus_feedback.rendered_frames", probe)
        self.assertIn("browser_engine_render_frame_pending(engine)", probe)
        self.assertIn("frame != browser_engine_framebuffer(engine, NULL)", probe)
        publish = runtime.index("bool psp_present_internal(")
        body = runtime[publish:runtime.index("static unsigned psp_local_hour", publish)]
        self.assertLess(body.index("psp_display_publish("),
                        body.index("psp_focus_feedback_published(frame, ui)"))
        self.assertIn("if (published && !media_visible) psp_focus_feedback_published", body)
        main = (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8")
        self.assertLess(main.index("psp_focus_feedback_begin(NULL, 0, 0)"),
                        main.index("browser_engine_destroy(browser->engine)"))

    def test_blank_failure_return_reuses_exact_history_receiver(self):
        actions = without_comments(
            (ROOT / "src/psp_app/psp_app_actions.c").read_text(
                encoding="utf-8"))
        resolver = actions[
            actions.index("static bool psp_app_resolve_recovery_return_action("):
            actions.index("void psp_app_dispatch_action(")]
        self.assertIn("PSP_UI_ACTION_RECOVERY_RETURN", resolver)
        self.assertIn("lifecycle_retry_return_back", resolver)
        self.assertIn("lifecycle_retry_return_forward", resolver)
        self.assertIn("lifecycle_retry_return_home", resolver)
        self.assertIn("PSP_UI_ACTION_FORWARD : PSP_UI_ACTION_BACK", resolver)
        self.assertIn(
            "lifecycle_retry_available = false", resolver)
        heavy = actions[
            actions.index("static void psp_app_dispatch_heavy_action("):
            actions.index("case PSP_UI_ACTION_TOGGLE_BOOKMARK:")]
        remap = heavy.index("psp_app_resolve_recovery_return_action(")
        switch = heavy.index("switch (intent->action)")
        back = heavy.index("case PSP_UI_ACTION_BACK:")
        self.assertLess(remap, switch)
        self.assertLess(switch, back)
        back_branch = heavy[back:heavy.index(
            "case PSP_UI_ACTION_VOICE_FOCUSED_TEXT:", back)]
        self.assertIn("browser_engine_begin_navigation_history(", back_branch)
        self.assertIn("navigation_back(", back_branch)
        local_fragment = back_branch.index(
            "navigation_url_is_same_document(")
        self.assertIn("browser_engine_history_move(engine, forward)",
                      back_branch[local_fragment:])
        self.assertLess(local_fragment,
                        back_branch.index(
                            "psp_ensure_network_for_navigation(",
                            local_fragment))
        self.assertLess(local_fragment,
                        back_branch.index("psp_ui_set_loading(",
                                          local_fragment))
        self.assertLess(local_fragment,
                        back_branch.index(
                            "psp_leave_reader_for_navigation(",
                            local_fragment))
        ordinary_return = heavy[heavy.index(
            "case PSP_UI_ACTION_RECOVERY_RETURN:"):
            heavy.index("case PSP_UI_ACTION_RECOVERY_READER:")]
        self.assertIn("RETURNED TO LAST USABLE PAGE", ordinary_return)
        self.assertIn(
            "lifecycle_retry_return_back = false", ordinary_return)
        self.assertIn("lifecycle_retry_return_home", ordinary_return)
        self.assertIn("psp_show_native_home(app)", ordinary_return)
        disable = heavy[heavy.index(
            "case PSP_UI_ACTION_RECOVERY_DISABLE_JAVASCRIPT:"):
            heavy.index("case PSP_UI_ACTION_RECOVERY_READER:")]
        self.assertIn("lifecycle_retry_url", disable)
        self.assertIn("goto recovery_reload", disable)
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        ordinary_failure = main[
            main.index("static TILEFINCH_OUT_OF_LINE void "
                       "psp_navigation_present_failure("):
            main.index("static TILEFINCH_COLD_PATH bool "
                       "psp_handle_update_version_intent(")]
        self.assertIn("lifecycle_retry_return_back = false", ordinary_failure)
        self.assertIn("lifecycle_retry_return_forward = false", ordinary_failure)
        blank_recovery = main[
            main.index("static TILEFINCH_OUT_OF_LINE void "
                       "psp_apply_reader_after_navigation_dirty("):
            main.index("static TILEFINCH_OUT_OF_LINE bool "
                       "psp_deferred_image_after_present(")]
        unavailable = main[
            main.index("static TILEFINCH_OUT_OF_LINE void "
                       "psp_present_blank_reader_unavailable("):
            main.index("static TILEFINCH_OUT_OF_LINE void "
                       "psp_apply_reader_after_navigation_dirty(")]
        self.assertIn(
            "browser_engine_last_navigation_return_target", unavailable)
        self.assertIn("BROWSER_NAVIGATION_RETURN_BACK", unavailable)
        self.assertIn("BROWSER_NAVIGATION_RETURN_FORWARD", unavailable)
        self.assertIn("BROWSER_NAVIGATION_RETURN_NONE", unavailable)
        self.assertIn("lifecycle_retry_return_home", unavailable)
        self.assertIn("BROWSER_BLANK_READER_RECOVERY_DEFERRED", blank_recovery)
        basic_prepare = blank_recovery.index(
            "browser_engine_prepare_basic_view_recovery")
        reader_prepare = blank_recovery.index(
            "browser_engine_prepare_blank_reader_recovery")
        self.assertLess(basic_prepare, reader_prepare)
        self.assertIn(
            "BROWSER_BASIC_VIEW_RECOVERY_AVAILABLE", blank_recovery)
        self.assertIn("ui->basic_mode = true", blank_recovery)
        self.assertIn("ui->reader_mode = false", blank_recovery)
        self.assertIn("psp_present_blank_reader_unavailable", blank_recovery)
        self.assertIn("blank_reader_recovery_pending = true", blank_recovery)
        self.assertIn(
            "blank_reader_recovery_generation", blank_recovery)
        retry_helper = main[
            main.index("static TILEFINCH_OUT_OF_LINE void "
                       "psp_retry_deferred_blank_reader("):
            main.index("static TILEFINCH_OUT_OF_LINE bool "
                       "psp_deferred_image_after_present(")]
        self.assertIn("blank_reader_recovery_pending", retry_helper)
        self.assertIn("blank_reader_recovery_generation", retry_helper)
        self.assertIn("psp_apply_reader_after_navigation", retry_helper)
        runtime_wrapper = main[
            main.index("psp_advance_page_runtime_with_reader_recovery("):
            main.index("static TILEFINCH_OUT_OF_LINE "
                       "BrowserRenderJobStatus psp_render_page_job(")]
        self.assertIn("psp_advance_page_runtime", runtime_wrapper)
        self.assertIn("psp_retry_deferred_blank_reader", runtime_wrapper)
        self.assertIn("bool *layout_changed", runtime_wrapper)
        runtime_retry = main[
            main.index("bool runtime_layout_changed = false;"):
            main.index("ScriptMediaRequest dom_media_request;")]
        self.assertIn(
            "psp_advance_page_runtime_with_reader_recovery", runtime_retry)

    def test_successful_saved_app_launch_clears_library_toast(self):
        actions = without_comments(
            (ROOT / "src/psp_app/psp_app_actions.c").read_text(
                encoding="utf-8"))
        branch = actions[
            actions.index("char offline_url[NAVIGATION_URL_LIMIT]"):
            actions.index("break;", actions.index("bool offline_opened ="))]
        opened = branch[branch.index("if (offline_opened) {"):]
        self.assertIn("presentation.ui.status[0] = '\\0'", opened)
        self.assertIn("presentation.ui.toast_frames = 0u", opened)
        self.assertIn("presentation.ui.toast_entry_frames = 0u", opened)
        self.assertIn("if (fullscreen_offline_app)", opened)
        self.assertIn("presentation.ui.chrome_visible = false", opened)
        self.assertIn("presentation.ui.cursor_visible = false", opened)
        self.assertIn(
            "== TILEFINCH_WEB_APP_DISPLAY_FULLSCREEN",
            branch[:branch.index("bool offline_opened =")])
        self.assertLess(
            opened.index("presentation.ui.status[0] = '\\0'"),
            opened.index("if (!offline_opened)"))

    def test_site_allowlist_uses_local_surface_reload_policy(self):
        settings = without_comments(
            (ROOT / "src/psp_app/psp_app_settings.c").read_text(
                encoding="utf-8"))
        branch = settings[
            settings.index("PSP_UI_SETTING_CONTENT_BLOCKER_SITE_ALLOWED"):
            settings.index("PSP_UI_SETTING_COOKIE_BANNER_HIDDEN")]
        self.assertIn("psp_app_reload_for_site_security_setting(", branch)
        self.assertNotIn("psp_begin_page_load(", branch)
        helper = settings[
            settings.index("static void psp_app_reload_for_site_security_setting("):
            settings.index("void psp_app_refresh_network_profile_label(")]
        self.assertIn("base_screen == PSP_UI_SCREEN_HOME", helper)
        self.assertIn("if (started && !local_surface)", helper)

    def test_x25519_comparison_requires_a_valid_generic_result(self):
        source = without_comments(
            (ROOT / "src/psp_crypto_selftest_main.c").read_text(
                encoding="utf-8"))
        check = source[
            source.index("static int check_x25519("):
            source.index("static int run_x25519_vectors(")]
        self.assertIn("bool generic_ok = false", check)
        self.assertIn("generic_ok = true", check)
        self.assertIn("else if (generic_ok", check)
        self.assertIn("memcmp(got_everest, got_generic, 32)", check)

    def test_experimental_resume_reverses_failed_history_move(self):
        commands = without_comments(
            (ROOT / "src/interactive/commands.inc").read_text(
                encoding="utf-8"))
        resume = commands[
            commands.index("static bool experimental_resume_history("):
            commands.index("static bool loop_history(")]
        rebuild = resume.index("bool rebuilt = experimental_rebuild_document(")
        rollback = resume.index("if (!rebuilt)", rebuild)
        self.assertIn("forward ? navigation_back", resume[rollback:])
        self.assertIn(": navigation_forward", resume[rollback:])
        self.assertIn("return rebuilt", resume[rollback:])

    def test_dom_event_counter_tolerates_reduced_bridge_state(self):
        bindings = without_comments(
            (ROOT / "src/js_dom_bindings.c").read_text(encoding="utf-8"))
        recorder = bindings[
            bindings.index("JSValue js_dom_record_event("):
            bindings.index("JSValue js_dom_record_event_handler(")]
        self.assertIn("bridge != NULL && bridge->result != NULL", recorder)
        self.assertLess(
            recorder.index("bridge->result != NULL"),
            recorder.index("bridge->result->events_dispatched++"))

    def test_focus_probe_best_effort_restores_live_marker(self):
        resolver = without_comments(
            (ROOT / "src/style_resolve.c").read_text(encoding="utf-8"))
        helper = resolver[
            resolver.index("static bool restore_focus_marker("):
            resolver.index("static bool computed_style_equal_without_outline(")]
        self.assertIn("set_focus_marker(node, false)", helper)
        classify = resolver[
            resolver.index("StyleFocusChange style_focus_change_classify("):
            resolver.index("bool style_focus_change_is_outline_only(")]
        first = classify.index("restored = restore_focus_marker(")
        retry = classify.index("restored = restore_focus_marker(", first + 1)
        unsafe = classify.index("return STYLE_FOCUS_CHANGE_UNSAFE", retry)
        self.assertLess(first, retry)
        self.assertLess(retry, unsafe)

    def test_psp_tls_clock_uses_one_bounded_status(self):
        clock = without_comments(
            (ROOT / "src/psp_time.c").read_text(encoding="utf-8"))
        self.assertIn("return PSP_TIME_UNREADABLE", clock)
        self.assertIn("psp_time_classify_utc(&sampled, epoch)", clock)
        self.assertIn("psp_time_read_utc(&result)", clock)
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        self.assertIn("PspTimeStatus clock_status = psp_time_read_utc(NULL)",
                      runtime)
        report = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        self.assertIn("psp_time_read_utc_fields(", report)
        self.assertIn("rtc-status=%s", report)
        self.assertNotIn("sceRtcCheckValid", report)

    def test_invalid_rtc_does_not_halt_local_startup(self):
        main = without_comments(
            (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8"))
        clock = main[main.index(
            "PspTimeStatus rtc_status = psp_time_read_utc_fields("):]
        clock = clock[:clock.index("psp_log_checkpoint(\"tls-assets-ready\")")]
        invalid = clock[clock.index("if (rtc_status != PSP_TIME_OK)"):]
        self.assertIn("action=continue-offline", invalid)
        self.assertNotIn("goto sleep_forever", invalid)
        halted = main[main.index("sleep_forever:"):]
        self.assertIn("psp_report_startup_failure(", halted)
        helper = main[
            main.index("static TILEFINCH_COLD_PATH void "
                       "psp_report_startup_failure("):
            main.index("int main(int argc", main.index(
                "static TILEFINCH_COLD_PATH void "
                "psp_report_startup_failure("))]
        self.assertIn("psp_write_failure_report_data(", helper)
        self.assertIn('"startup", detail, url, 0, 0, NULL, true', helper)
        self.assertIn("startup_failure_screen", halted)
        self.assertNotIn("STARTUP FAILED - SEE LOG", halted)

    def test_psplink_browser_target_owns_required_runtime_assets(self):
        targets = (ROOT / "cmake/TilefinchTargets.cmake").read_text(
            encoding="utf-8")
        start = targets.index(
            "add_custom_target(psp-browser-script-dev-assets")
        assets = targets[start:targets.index(
            "add_dependencies(psp-browser-script-dev-prx", start)]
        self.assertIn("${PSP_BROWSER_PSP_CA_BUNDLE}", assets)
        self.assertIn('"${CMAKE_CURRENT_BINARY_DIR}/roots.pem"', assets)
        self.assertIn("${PSP_BROWSER_METRIC_SANS_FONT}", assets)
        self.assertIn("boot-defaults.cfg", assets)
        device = (ROOT / "scripts/psplink-device.sh").read_text(
            encoding="utf-8")
        self.assertIn('"$BUILD_DIR/roots.pem"', device)
        self.assertIn('input_script=$(sed -n \'s/^input_script=//p\'',
                      device)
        self.assertIn('"$ROOT/tests/input-scripts/$input_script"', device)
        self.assertIn('"$BUILD_DIR/$input_script"', device)
        self.assertIn("PSPLink input script missing", device)
        self.assertLess(
            device.index('"$BUILD_DIR/roots.pem"'),
            device.index("require_module_absent Tilefinch"))

    def test_psplink_asset_probe_does_not_require_host0_seeking(self):
        source = without_comments(
            (ROOT / "src/psp_app/psp_app_input.c").read_text(
                encoding="utf-8"))
        probe = source[source.index("bool psp_probe_file("):]
        probe = probe[:probe.index("\n}\n") + 3]
        self.assertIn("sceIoOpen(", probe)
        self.assertIn("sceIoRead(", probe)
        self.assertIn("sceIoClose(", probe)
        self.assertNotIn("fopen(", probe)
        self.assertNotIn("fread(", probe)
        self.assertNotIn("fseek(", probe)
        self.assertNotIn("ftell(", probe)
        self.assertIn("sceIoGetstat(", probe)
        self.assertIn("file_stat.st_size > (uint64_t) maximum_bytes", probe)
        self.assertIn("static uint8_t psp_probe_file_scratch[4096]", source)
        self.assertIn("sceIoRead(file, psp_probe_file_scratch, request)", probe)
        self.assertIn("tilefinch_sha256_update", probe)
        self.assertIn("tilefinch_sha256_final", probe)

    def test_psplink_input_script_uses_the_native_host0_driver(self):
        source = (ROOT / "src/psp_input_script.c").read_text(
            encoding="utf-8")
        loader = source[source.index("bool psp_input_script_load("):]
        loader = loader[:loader.index("bool psp_input_script_armed(")]
        psp = loader[loader.index("#ifdef __PSP__"):
                     loader.index("#else", loader.index("#ifdef __PSP__"))]
        self.assertIn("sceIoGetstat(path, &file_stat)", psp)
        self.assertIn("sceIoOpen(path, PSP_O_RDONLY, 0)", psp)
        self.assertIn("while (!read_failed && read < expected)", psp)
        self.assertIn("sceIoRead(file, text + read, expected - read)", psp)
        self.assertIn("sceIoClose(file)", psp)
        self.assertNotIn("fopen(", psp)
        self.assertNotIn("fread(", psp)
        self.assertNotIn("fseek(", psp)

        app_source = (ROOT / "src/psp_app/psp_app_input_script.c").read_text(
            encoding="utf-8")
        begin = app_source[app_source.index("bool psp_input_script_begin("):]
        begin = begin[:begin.index("bool psp_input_script_running(")]
        self.assertIn("if (install_paths != NULL)", begin)
        self.assertIn("tilefinch_install_program_path(", begin)
        self.assertNotIn("install_paths->slotted", begin)

    def test_validation_ca_diagnostic_cannot_brick_local_startup(self):
        main = (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8")
        probe = main[main.index("if (!psp_probe_file("):]
        probe = probe[:probe.index("#endif", probe.index("if (!psp_probe_file("))]
        self.assertIn("trust bundle diagnostic unavailable", probe)
        self.assertNotIn("goto sleep_forever", probe)

    def test_psp_ca_blob_loader_does_not_seek_device_files(self):
        fetch = (ROOT / "src/fetch.c").read_text(encoding="utf-8")
        loader = fetch[fetch.index("/* Reads the configured trust bundle"):]
        loader = loader[:loader.index("#endif /* TILEFINCH_FETCH_CA_BLOB */")]
        psp_branch = loader[loader.index("#if defined(__PSP__)"):
                            loader.index("#else")]
        self.assertIn("sceIoGetstat(", psp_branch)
        self.assertIn("sceIoOpen(", psp_branch)
        self.assertIn("sceIoRead(", psp_branch)
        self.assertNotIn("fseek(", psp_branch)
        self.assertNotIn("ftell(", psp_branch)

    def test_validation_preloads_ca_after_media_pool_reservation(self):
        main = (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8")
        call = "psp_preload_validation_ca_bundle(&process.install_paths);"
        self.assertIn("fetch_prepare_ca_bundle()", main)
        self.assertGreater(main.index(call),
                           main.index("media_psp_backend_reserve_pool();"))
        fetch = (ROOT / "src/fetch.c").read_text(encoding="utf-8")
        self.assertIn("fetch_ca_blob_psp_storage[64u * 1024u]", fetch)
        setter = fetch[fetch.index("bool fetch_set_ca_bundle_path("):
                       fetch.index("const char *fetch_ca_bundle_path(")]
        self.assertIn("strcmp(fetch_ca_bundle, path) == 0", setter)

    def test_psp_curl_patch_retains_and_resets_verify_flags(self):
        patch = (ROOT / "patches/curl-8.21.0-psp.patch").read_text(
            encoding="utf-8")
        callback = patch[
            patch.index("static int mbed_verify_cb("):
            patch.index("static CURLcode mbed_connect_step1(")]
        self.assertIn("ssl_config->certverifyresult |= (long)*flags", callback)
        handshake = patch[patch.index("static CURLcode mbed_connect_step1("):]
        reset = handshake.index("ssl_config->certverifyresult = 0")
        version_gate = handshake.index("if((conn_config->version")
        self.assertLess(reset, version_gate)

    def test_tls_verify_availability_reaches_guidance_and_report(self):
        fetch = without_comments(
            (ROOT / "src/fetch.c").read_text(encoding="utf-8"))
        self.assertIn(
            "result->tls_verify_result_available = curl_easy_getinfo(", fetch)
        navigation = without_comments(
            (ROOT / "src/navigation/load_state.inc").read_text(
                encoding="utf-8"))
        self.assertIn("last_tls_verify_result_available", navigation)
        runtime = without_comments(
            (ROOT / "src/psp_app/psp_app_runtime.c").read_text(
                encoding="utf-8"))
        self.assertIn("tls_verify_result_available, clock_status", runtime)
        self.assertNotIn(
            "tilefinch_error_is_certificate_verification_failure(detail)",
            runtime)
        report = (ROOT / "src/psp_script_main.c").read_text(encoding="utf-8")
        self.assertIn("tls-verify-available=%d", report)
        self.assertIn("tls-verification-failed=%d", report)

    def test_owned_tls_role_configuration_is_shared_with_consumers(self):
        config = (ROOT / "cmake/psp_transport/mbedtls_user_config.h").read_text(
            encoding="utf-8")
        build = (ROOT / "cmake/PspOwnedTransport.cmake").read_text(
            encoding="utf-8")
        self.assertIn("#undef MBEDTLS_SSL_SRV_C", config)
        self.assertIn("#undef MBEDTLS_DEBUG_C", config)
        self.assertIn("#undef MBEDTLS_VERSION_FEATURES", config)
        for required in ("MBEDTLS_SSL_CLI_C", "MBEDTLS_SSL_PROTO_TLS1_2",
                         "MBEDTLS_SSL_PROTO_TLS1_3"):
            self.assertIn(f"!defined({required})", config)
            self.assertNotIn(f"#undef {required}", config)
        for target in ("tilefinch_psp_transport", "tilefinch_psp_crypto"):
            self.assertRegex(build, rf"target_compile_definitions\({target} INTERFACE"
                             r'\s+MBEDTLS_USER_CONFIG_FILE="\$\{_transport_mbedtls_config\}"')
        curl = build[build.index("ExternalProject_Add(tilefinch_psp_curl"):]
        self.assertIn("${_transport_mbedtls_consumer_flags}", curl)
        for target in ("tilefinch_psp_mbedtls", "tilefinch_psp_curl"):
            self.assertIn(f"ExternalProject_Add_Step({target} user-config", build)
        self.assertEqual(build.count('DEPENDS "${_transport_mbedtls_config}"'), 2)

    def test_owned_curl_omits_unused_subsystems_but_keeps_browser_hooks(self):
        build = (ROOT / "cmake/PspOwnedTransport.cmake").read_text(
            encoding="utf-8")
        for option in ("PROXY", "AWS", "MIME", "FORM_API", "PROGRESS_METER",
                       "GETOPTIONS"):
            self.assertIn(f'"-DCURL_DISABLE_{option}=ON"', build)
        self.assertIn('"-DCURL_DISABLE_WEBSOCKETS=OFF"', build)
        self.assertIn('"-DHTTP_ONLY=OFF"', build)
        self.assertIn("TILEFINCH_PSP_CURL_NO_PROXY=1", build)
        source = (ROOT / "src/fetch/transport.inc").read_text(encoding="utf-8")
        guard = source[source.index("static bool fetch_configure_private_network("):
                       source.index("bool fetch_url(")]
        proxy_start = guard.index("#if !defined(TILEFINCH_PSP_CURL_NO_PROXY)")
        proxy_end = guard.index("#endif", proxy_start)
        self.assertIn('CURLOPT_PROXY, ""', guard[proxy_start:proxy_end])
        for option in ("OPENSOCKETFUNCTION", "OPENSOCKETDATA",
                       "PREREQFUNCTION", "PREREQDATA"):
            self.assertIn(f"CURLOPT_{option}", guard[proxy_end:])
        self.assertIn("CURLOPT_XFERINFOFUNCTION", source)
        self.assertIn("CURLOPT_NOPROGRESS, 0L", source)
        worker = (ROOT / "src/fetch/background_transport.inc").read_text(
            encoding="utf-8")
        self.assertIn("CURLOPT_XFERINFOFUNCTION", worker)
        self.assertIn("CURLOPT_NOPROGRESS, 0L", worker)

    def test_macos_ppsspp_harnesses_never_fall_back_to_direct_launch(self):
        harnesses = (
            "scripts/run-ppsspp-input-script.sh",
            "scripts/run-ppsspp-device-cost.sh",
            "scripts/run-ppsspp-crypto-selftest.sh",
            "scripts/run-ppsspp-network.sh",
        )
        for relative in harnesses:
            source = (ROOT / relative).read_text(encoding="utf-8")
            with self.subTest(harness=relative):
                self.assertNotIn("ppsspp_direct_fallback", source)
                self.assertNotIn("lsregister=", source)
                self.assertIn("Direct PPSSPP launch is unsafe on macOS", source)
                self.assertIn("CFBundleIdentifier", source)
                self.assertIn("open -g -n -W", source)
                direct = source.find('HOME="$home_dir" "$ppsspp"')
                self.assertGreater(direct, 0)
                launch_branch = source.rfind(
                    'if [ "$is_darwin" -eq 1 ]; then', 0, direct)
                self.assertGreater(launch_branch, 0)
                self.assertLess(launch_branch, direct)
        manual = (ROOT / "scripts/launch-ppsspp-safe.sh").read_text(
            encoding="utf-8")
        self.assertIn("open -g -n -W", manual)

    def test_treadline_harness_mutes_output_without_disabling_audio(self):
        source = (ROOT / "scripts/run-ppsspp-input-script.sh").read_text(
            encoding="utf-8")
        end_marker = '} >"$run_dir/script.ini"'
        end = source.index(end_marker) + len(end_marker)
        start = source.rfind("    {\n", 0, end)
        self.assertGreaterEqual(start, 0)
        with tempfile.TemporaryDirectory() as temporary:
            for scenario in ("treadline-offline-controls", "treadline-long-soak",
                             "menu-tour"):
                with self.subTest(scenario=scenario):
                    subprocess.run(["/bin/sh", "-eu", "-c", source[start:end]],
                        env={**os.environ, "run_dir": temporary,
                             "ppsspp_cpu_mhz": "0", "scenario": scenario,
                             "graphics_backend": "3 (VULKAN)"},
                        check=True)
                    config = configparser.ConfigParser()
                    config.read(Path(temporary) / "script.ini")
                    self.assertEqual(config.get("Graphics", "GraphicsBackend"), "3 (VULKAN)")
                    if not scenario.startswith("treadline-"):
                        self.assertNotIn("Sound", config)
                        continue
                    self.assertTrue(config.getboolean("Sound", "Enable"))
                    for key in ("GameVolume", "GlobalVolume", "UIVolume",
                                "GamePreviewVolume", "AchievementVolume"):
                        self.assertEqual(config.getint("Sound", key), 0)

    def test_input_harness_does_not_require_images_for_control_marks(self):
        source = (ROOT / "scripts/run-ppsspp-input-script.sh").read_text(
            encoding="utf-8")
        capture_gate = source[
            source.index("if grep -E '^[[:space:]]*mark-live"):
            source.index("if [ \"$scenario\" = navigation-cancel-live ]")]
        for mark in ("webgl-measure-start", "webgl-measure-end",
                     "auto-controls", "controls-exited"):
            self.assertIn(mark, capture_gate)
        self.assertIn("grep -Ev", capture_gate)

    def test_article_section_harness_rejects_unfragmented_journey(self):
        result = subprocess.run([
            "/bin/sh", str(ROOT / "scripts/run-ppsspp-input-script.sh"),
            "--script", "wikipedia-article-section-live", "--url",
            "https://example.test/article"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("requires a #History start URL", result.stderr)

    def test_expansion_harness_requires_input_inside_activation(self):
        source = (ROOT / "scripts/run-ppsspp-input-script.sh").read_text()
        start = source.index("    awk '/tilefinch-activation-feedback: shown=1 ")
        end = source.index(" || exit 1", start) + len(" || exit 1")
        gate = source[start:end]
        events = [
            "tilefinch-activation-feedback: shown=1 elapsed=80000us\n",
            "tilefinch-input-script-edge: step=8 buttons=0x0002 receiver=supervisor ready=0 at-us=81000\n",
            "tilefinch-ui-supervisor-input: scope=page-runtime queued=1 dropped=0\n",
            "tilefinch-control-activation: ok=1 kind=2 elapsed=1000000us relayouts=1\n",
            "tilefinch-ui-supervisor-input: replay=0x0002 remaining=0\n"]
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "trace.txt"
            for order, expected in (((0, 1, 2, 3, 4), 0),
                                    ((0, 3, 1, 2, 4), 1),
                                    ((0, 1, 3, 4), 1)):
                log.write_text("".join(events[i] for i in order))
                result = subprocess.run(["sh", "-eu", "-c", gate],
                    env={**os.environ, "telemetry_log": str(log)}, capture_output=True)
                self.assertEqual(result.returncode, expected, result.stderr)

    def test_article_section_harness_rejects_wrong_semantic_target(self):
        source = (ROOT / "scripts/run-ppsspp-input-script.sh").read_text()
        article_gate = source.index('if [ "$scenario" = wikipedia-article-section-live ]; then')
        start = source.index("    grep -Eq 'tilefinch-input-focus-target: mark=section-after .* id=History '", article_gate)
        gate = source[start:source.index("    }", start) + 5]
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "trace.txt"
            for target, expected in (("History", 0), ("Games", 1), ("", 1)):
                log.write_text("tilefinch-control-activation: ok=1 kind=2 elapsed=1000us relayouts=1\n"
                    f"tilefinch-input-focus-target: mark=section-after kind=3 index=0 indicator=browser id={target} class=section url=\n")
                result = subprocess.run(["sh", "-eu", "-c", gate],
                    env={**os.environ, "telemetry_log": str(log)}, capture_output=True)
                self.assertEqual(result.returncode, expected, result.stderr)

    def test_ppsspp_live_network_harness_has_bounded_dns_alias_seam(self):
        source = (ROOT / "scripts/run-ppsspp-network.sh").read_text(
            encoding="utf-8")
        self.assertIn("TILEFINCH_PPSSPP_HOST_ALIASES", source)
        self.assertIn("TILEFINCH_PPSSPP_TRACE_DIRECTORY", source)
        self.assertIn("[HostAliases]", source)
        self.assertIn("invalid PPSSPP host alias", source)
        self.assertIn("invalid PPSSPP replay trace", source)
        self.assertIn('"trace=$trace_name"', source)
        self.assertIn("InfrastructureAutoDNS = True", source)
        base_copy = source.index('cp "$run_dir/network.ini"')
        alias_append = source.index(
            '} >>"$home_dir/.config/ppsspp/PSP/SYSTEM/ppsspp.ini"')
        self.assertLess(base_copy, alias_append)

    def test_canvas_rect_batch_fits_native_work_allowance(self):
        # canvas.js retains a rectangle batch whose native call reported
        # status 2, so the facade's batch cap and surface ceiling must match
        # the constants the bridge's _Static_assert checks against the
        # per-call work allowance.
        facade = (ROOT / "src/bootstrap/canvas.js").read_text(
            encoding="utf-8")
        bridge = (ROOT / "src/js_canvas_bridge.c").read_text(
            encoding="utf-8")
        self.assertIn("const pixelByteLimit = 512 * 1024,", facade)
        self.assertIn("state.rectCommands.length >= 32 &&", facade)
        self.assertIn(
            "#define CANVAS_SURFACE_PIXEL_LIMIT ((512u * 1024u) / 4u)", bridge)
        self.assertIn("#define CANVAS_RECT_BATCH_LIMIT 32u", bridge)
        self.assertIn("_Static_assert(CANVAS_RECT_BATCH_LIMIT * "
                      "CANVAS_SURFACE_PIXEL_LIMIT", bridge)
        self.assertIn("<= CANVAS_RASTER_WORK_LIMIT", bridge)


if __name__ == "__main__":
    # CTest passes the source root as argv[1]; keep unittest from parsing it.
    unittest.main(argv=[sys.argv[0]])
