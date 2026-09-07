#!/bin/sh
# Replay a scripted-input scenario against the browser EBOOT under PPSSPP and
# diff its receiver trace against a checked-in golden.
#
# The 720-tick smoke in scripts/run-ppsspp-network.sh exercises the action and
# settings dispatch seams but cannot enter a case body: nothing presses a
# button. This boots the same validation EBOOT with an `input_script=` key.
# The key replaces only pad input: splash, native HOME, tab creation,
# chrome, engine, and action receivers are the shipping path.
#
# Usage:
#   scripts/run-ppsspp-input-script.sh [options]
#     --build-dir DIR   PSP build directory (default build-preset-psp-validation)
#     --script NAME     scenario in tests/input-scripts (default menu-tour)
#     --url URL         start on an HTTPS page instead of native HOME
#     --offline-library DIR
#                       seed an exact offline/ directory into the isolated run
#     --heap-mb N       validation JavaScript heap override
#     --script-file-kb N
#                       validation per-script source override
#     --timeout N       seconds to wait for the run (default 300)
#     --runs N          replay N times and require identical traces (default 1)
#     --debug-log       add PPSSPP's -d syscall trace to the emulator log
#     --update-golden   rewrite the golden from this run instead of diffing
#
# Set TILEFINCH_PPSSPP_CPU_MHZ to a positive emulated PSP clock for a
# deterministic pressure run, or leave it unset/0 for PPSSPP's normal clock.
# PPSSPP 1.20 renamed the canonical setting to [CPU] CPUSpeed; the older
# LockedCPUSpeed spelling is silently ignored.
#
# Exits 0 only when the EBOOT prints `tilefinch-input-script: outcome=`, the
# extracted trace matches the golden, and the run reaches
# `tilefinch-validation: outcome=clean-exit`. Artifacts land in
# <build-dir>/ppsspp-input-script-latest/.
#
# The trace needs no wall-clock masking, which is the point of the readiness
# gate: the stepper advances only on frames the browser could have taken the
# press on, so every counter in the trace is a function of the script rather
# than of how fast the host ran. The evidence is the receiver names the log
# reports, not a success counter the app kept for itself.
#
# The one genuinely host-speed quantity the harness measures -- how many
# frames the browser was busy -- is printed under the separate
# `tilefinch-input-telemetry:` prefix, which the extraction below does not
# match. It stays in the log and still gates live scenarios on being
# non-zero, but its value never reaches a golden that a real PSP would have
# to reproduce exactly.
#
# The launch mechanics (isolated HOME, disposable re-signed app bundle,
# LaunchServices on macOS, kill-by-unique---log) are the ones
# scripts/run-ppsspp-network.sh established; this script is deliberately the
# same shape so all three age together.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$root/build-preset-psp-validation"
scenario=menu-tour
timeout_seconds=300
runs=1
debug_log=0
update_golden=0
start_url=
offline_library=
heap_mb=
script_file_kb=
graphics=${TILEFINCH_PPSSPP_GRAPHICS:-opengl}
case "$graphics" in
    opengl) graphics_backend='0 (OPENGL)' ;;
    vulkan) graphics_backend='3 (VULKAN)' ;;
    *) printf 'TILEFINCH_PPSSPP_GRAPHICS must be opengl or vulkan\n' >&2; exit 2 ;;
esac
ppsspp_cpu_mhz=${TILEFINCH_PPSSPP_CPU_MHZ:-${TREADLINE_CPU_MHZ:-0}}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir=$2; shift 2 ;;
        --build-dir=*) build_dir=${1#--build-dir=}; shift ;;
        --script) scenario=$2; shift 2 ;;
        --script=*) scenario=${1#--script=}; shift ;;
        --url) start_url=$2; shift 2 ;;
        --url=*) start_url=${1#--url=}; shift ;;
        --offline-library) offline_library=$2; shift 2 ;;
        --offline-library=*) offline_library=${1#--offline-library=}; shift ;;
        --heap-mb) heap_mb=$2; shift 2 ;;
        --heap-mb=*) heap_mb=${1#--heap-mb=}; shift ;;
        --script-file-kb) script_file_kb=$2; shift 2 ;;
        --script-file-kb=*) script_file_kb=${1#--script-file-kb=}; shift ;;
        --timeout) timeout_seconds=$2; shift 2 ;;
        --timeout=*) timeout_seconds=${1#--timeout=}; shift ;;
        --runs) runs=$2; shift 2 ;;
        --runs=*) runs=${1#--runs=}; shift ;;
        --debug-log) debug_log=1; shift ;;
        --update-golden) update_golden=1; shift ;;
        -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
        *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done

if [ "$scenario" = wikipedia-article-section-live ] || [ "$scenario" = wikipedia-section-input-live ]; then
    # This receiver trace starts at History; an unfragmented article is a
    # different layout/input journey and cannot be compared to its golden.
    case "$start_url" in
        *'#History') ;;
        *) printf '%s\n' 'wikipedia-article-section-live requires a #History start URL' >&2
           exit 2 ;;
    esac
fi

case "$runs:$timeout_seconds" in
    *[!0-9:]*|0:*|*:0) printf 'runs and timeout must be positive integers\n' >&2; exit 2 ;;
esac
case "$ppsspp_cpu_mhz" in
    ''|*[!0-9]*)
        printf 'TILEFINCH_PPSSPP_CPU_MHZ must be a non-negative integer\n' >&2
        exit 2 ;;
esac
case "$heap_mb:$script_file_kb" in
    *[!0-9:]*|0:*|*:0)
        printf 'heap and script-file overrides must be positive integers\n' >&2
        exit 2 ;;
esac

if [ -n "$start_url" ]; then
    case "$start_url" in
        https://*|http://127.0.0.1:*|http://localhost:*) ;;
        *) printf '%s\n' \
            'scripted-input start URL must use HTTPS or loopback HTTP' >&2
            exit 2 ;;
    esac
fi
if [ -n "$offline_library" ]; then
    case "$offline_library" in
        /*) ;;
        *) offline_library="$root/$offline_library" ;;
    esac
    [ -f "$offline_library/library.bin" ] || {
        printf 'offline library has no library.bin: %s\n' \
            "$offline_library" >&2
        exit 2
    }
fi

case "$build_dir" in
    /*) ;;
    *) build_dir="$root/$build_dir" ;;
esac

script_source="$root/tests/input-scripts/$scenario.txt"
golden="$root/tests/input-scripts/$scenario.device-golden.txt"
[ -f "$script_source" ] || {
    printf 'missing input script: %s\n' "$script_source" >&2
    exit 2
}

eboot="$build_dir/EBOOT.PBP"
[ -f "$eboot" ] || {
    printf '%s\n' \
        "missing browser EBOOT: $eboot" \
        "Build it first:" \
        "  cmake --preset psp -B $build_dir -DTILEFINCH_PSP_VALIDATION_LOG=ON" \
        "  cmake --build $build_dir --target psp-browser-script" >&2
    exit 2
}
# The harness is compiled out of shipping EBOOTs, so a non-validation build
# would boot, ignore `input_script=`, and report nothing. Fail loudly instead.
grep -q '^TILEFINCH_PSP_VALIDATION_LOG:BOOL=ON$' "$build_dir/CMakeCache.txt" \
    2>/dev/null || {
    printf '%s\n' \
        "$build_dir was not configured with TILEFINCH_PSP_VALIDATION_LOG=ON;" \
        "the scripted-input harness is not compiled into that EBOOT." >&2
    exit 2
}

ppsspp=${PPSSPP:-}
if [ -z "$ppsspp" ]; then
    ppsspp=$(command -v PPSSPPSDL || true)
fi
[ -n "$ppsspp" ] && [ -x "$ppsspp" ] || {
    printf '%s\n' \
        "PPSSPPSDL was not found. Set PPSSPP=/absolute/path/to/PPSSPPSDL." >&2
    exit 2
}
ppsspp_launchservices=0
ppsspp_bundle=
ppsspp_bundle_source=
is_darwin=0
[ "$(uname -s)" = Darwin ] && is_darwin=1
if [ "$is_darwin" -eq 1 ] && [ "${PPSSPP_LAUNCHSERVICES:-1}" = 0 ]; then
    printf '%s\n' \
        "Direct PPSSPP launch is unsafe on macOS." \
        "Remove PPSSPP_LAUNCHSERVICES=0 and allow LaunchServices instead." >&2
    exit 2
fi
if [ "$is_darwin" -eq 1 ]; then
    ppsspp_real=$(realpath "$ppsspp" 2>/dev/null || printf '%s' "$ppsspp")
    case "$ppsspp_real" in
        *.app/Contents/MacOS/*)
            ppsspp_bundle=${ppsspp_real%%.app/Contents/MacOS/*}.app ;;
        *)
            ppsspp_candidate=$(
                CDPATH= cd -- "$(dirname -- "$ppsspp_real")/.." 2>/dev/null \
                    && pwd
            )/PPSSPPSDL.app
            [ -d "$ppsspp_candidate" ] && ppsspp_bundle=$ppsspp_candidate ;;
    esac
    if [ -n "$ppsspp_bundle" ] && [ -d "$ppsspp_bundle" ]; then
        ppsspp_bundle_source=$ppsspp_bundle
        if codesign --verify --deep --strict "$ppsspp_bundle" >/dev/null 2>&1
        then
            ppsspp_launchservices=1
        else
            ppsspp_bundle=
        fi
    fi
fi

run_base=${TMPDIR:-/tmp}
session_dir=$(mktemp -d "${run_base%/}/tilefinch-ppsspp-script.XXXXXX")

# Homebrew's bundle seal can be invalid (a relocated MoltenVK symlink, or a
# resource-less signature). Repair a disposable copy for this session; never
# mutate the installed emulator.
if [ "$is_darwin" -eq 1 ] \
    && [ "$ppsspp_launchservices" -eq 0 ] \
    && [ -n "$ppsspp_bundle_source" ]; then
    fixed_bundle="$session_dir/PPSSPPSDL.app"
    ditto "$ppsspp_bundle_source" "$fixed_bundle"
    fixed_molten="$fixed_bundle/Contents/Frameworks/libMoltenVK.dylib"
    molten_prefix=$(brew --prefix molten-vk 2>/dev/null || true)
    molten_dylib="$molten_prefix/lib/libMoltenVK.dylib"
    if [ -L "$fixed_molten" ] && [ ! -e "$fixed_molten" ] \
        && [ -f "$molten_dylib" ]; then
        unlink "$fixed_molten"
        cp "$molten_dylib" "$fixed_molten"
        chmod u+w "$fixed_molten"
    fi
    for key in CFBundleVersion CFBundleShortVersionString \
            CFBundleLongVersionString; do
        plutil -replace "$key" -string 1.20.4 \
            "$fixed_bundle/Contents/Info.plist" >/dev/null 2>&1 || true
    done
    plutil -replace CFBundleIdentifier \
        -string "org.tilefinch.ppsspp.input.$$" \
        "$fixed_bundle/Contents/Info.plist" >/dev/null 2>&1 || true
    xattr -cr "$fixed_bundle"
    if codesign --force --deep --sign - "$fixed_bundle" >/dev/null 2>&1 \
        && codesign --verify --deep --strict "$fixed_bundle" \
            >/dev/null 2>&1; then
        # `open -a /absolute/bundle` registers this unique disposable copy as
        # part of launching it. Never turn a registration/preflight refusal
        # into direct execution: Cocoa can abort in _RegisterApplication
        # before PPSSPP reaches any PSP code.
        ppsspp_bundle=$fixed_bundle
        ppsspp_launchservices=1
    else
        printf '%s\n' \
            "PPSSPP's macOS app bundle is invalid and could not be repaired." >&2
        exit 2
    fi
fi
if [ "$is_darwin" -eq 1 ] \
    && [ "$ppsspp_launchservices" -ne 1 ]; then
    printf '%s\n' \
        "No safe LaunchServices PPSSPP bundle is available." \
        "Reinstall PPSSPP rather than invoking PPSSPPSDL directly." >&2
    exit 2
fi

emulator_pid=
emulator_log=
stop_emulator() {
    if [ -n "${emulator_pid:-}" ] && kill -0 "$emulator_pid" 2>/dev/null; then
        if [ "$ppsspp_launchservices" -eq 1 ]; then
            # LaunchServices detaches the app, so `open`'s pid is not its
            # parent; this run's unique --log path is the ownership token.
            ppsspp_pids=$(pgrep -f -- "--log=$emulator_log" 2>/dev/null || true)
            [ -n "$ppsspp_pids" ] && kill $ppsspp_pids 2>/dev/null || true
        else
            kill "$emulator_pid" 2>/dev/null || true
        fi
        wait "$emulator_pid" 2>/dev/null || true
    fi
}
cleanup() {
    stop_emulator
    rm -rf "$session_dir"
}
trap cleanup EXIT HUP INT TERM

result_dir="$build_dir/ppsspp-input-script-latest"
rm -rf "$result_dir"
mkdir -p "$result_dir"

# Every run gets its own HOME, so the profile the settings and the bookmark
# toggle write is created fresh. That is what lets the trace be exact rather
# than "exact after the first run".
run_once() {
    run_index=$1
    run_dir="$session_dir/run-$run_index"
    home_dir="$run_dir/home"
    app_dir="$home_dir/.config/ppsspp/PSP/GAME/TILEFINCH"
    emulator_log="$run_dir/ppsspp.log"
    emulator_console="$run_dir/ppsspp-console.log"
    emulator_stdout="$run_dir/ppsspp-stdout.log"
    emulator_stderr="$run_dir/ppsspp-stderr.log"
    mkdir -p "$app_dir" "$home_dir/.config/ppsspp/PSP/SYSTEM"
    : >"$home_dir/.config/ppsspp/PSP/SYSTEM/controls.ini"
    # Release qualification can seed signed optional components into this
    # run's isolated Memory Stick. Components require the real slotted layout,
    # so only this opt-in path launches the staged A/B install tree. Normal
    # scripted-input runs preserve the faster legacy single-EBOOT fixture.
    component_stage=${TILEFINCH_PPSSPP_COMPONENT_STAGE:-}
    if [ -n "$component_stage" ]; then
        [ -d "$component_stage/components" ] \
            && [ -f "$component_stage/profile.cfg" ] || {
            printf 'invalid optional-component stage: %s\n' \
                "$component_stage" >&2
            return 1
        }
        install_tree="$build_dir/tilefinch-install/Tilefinch"
        [ -f "$install_tree/EBOOT.PBP" ] \
            && [ -f "$install_tree/slot-a/EBOOT.PBP" ] || {
            printf 'missing launcher install tree: %s\n' "$install_tree" >&2
            return 1
        }
        cp -R "$install_tree/." "$app_dir/"
        mkdir -p "$app_dir/components"
        cp -R "$component_stage/components/." "$app_dir/components/"
        cp "$component_stage/profile.cfg" "$app_dir/data/profile.cfg"
        cp "$script_source" "$app_dir/slot-a/input-script.txt"
        config_path="$app_dir/data/boot-overrides.cfg"
        validation_log="$app_dir/data/tilefinch-validation.txt"
    else
        cp "$build_dir/EBOOT.PBP" "$build_dir/roots.pem" \
            "$build_dir/tilefinch-wasm.prx" "$app_dir/"
        if [ -f "$build_dir/tilefinch-voice.prx" ]; then
            cp "$build_dir/tilefinch-voice.prx" "$app_dir/"
        fi
        for asset_dir in fonts voice-model; do
            if [ -d "$build_dir/$asset_dir" ]; then
                cp -R "$build_dir/$asset_dir" "$app_dir/$asset_dir"
            fi
        done
        cp "$script_source" "$app_dir/input-script.txt"
        config_path="$app_dir/boot.cfg"
        validation_log="$app_dir/tilefinch-validation.txt"
    fi
    if [ -n "$offline_library" ]; then
        mkdir -p "$app_dir/offline"
        cp -R "$offline_library/." "$app_dir/offline/"
    fi

    {
        printf '%s\n' \
            "# Generated only for the isolated PPSSPP scripted-input run." \
            "url=$start_url" \
            "trace=none" \
            "profile=realistic" \
            "network_profile=1" \
            "ticks=0" \
            "dump_frame=0" \
            "exit_after_report=0" \
            "input_script=input-script.txt" \
            "interactive_validation_ticks=0" \
            "validation_cancel_after_ms=0" \
            "validation_preview_scroll=0" \
            "validation_media_play=0" \
            "validation_media_stability_auto=0" \
            "validation_power_test_auto=0"
    } >"$config_path"
    [ -z "$heap_mb" ] || printf 'heap_mb=%s\n' "$heap_mb" >>"$config_path"
    [ -z "$script_file_kb" ] \
        || printf 'file_kb=%s\n' "$script_file_kb" >>"$config_path"

    {
        printf '%s\n' \
            "[General]" \
            "FirstRun = False" \
            "Enable Logging = True" \
            "AutoRun = True" \
            "[CPU]" \
            "CPUSpeed = $ppsspp_cpu_mhz" \
            "[Network]" \
            "EnableWlan = True" \
            "InfrastructureAutoDNS = True" \
            "[Graphics]" \
            "GraphicsBackend = $graphics_backend" \
            "[SystemParam]" \
            "PSPModel = 1" \
            "PSPFirmwareVersion = 660"
        # Keep the emulated mixer/syscalls running, but never play background
        # Treadline test audio on the host. Only this disposable run is muted.
        # PPSSPP 1.20 uses GameVolume; GlobalVolume covers older installations.
        case "$scenario" in
            treadline-*)
                printf '%s\n' \
                    "[Sound]" \
                    "Enable = True" \
                    "GameVolume = 0" \
                    "GlobalVolume = 0" \
                    "UIVolume = 0" \
                    "GamePreviewVolume = 0" \
                    "AchievementVolume = 0"
                ;;
        esac
    } >"$run_dir/script.ini"
    cp "$run_dir/script.ini" \
        "$home_dir/.config/ppsspp/PSP/SYSTEM/ppsspp.ini"

    debug_flag=
    [ "$debug_log" -eq 1 ] && debug_flag=-d

    printf 'PPSSPP scripted input: %s run %s/%s\n' \
        "$scenario" "$run_index" "$runs"
    if [ "$is_darwin" -eq 1 ]; then
        : >"$emulator_stdout"
        : >"$emulator_stderr"
        # shellcheck disable=SC2086
        open -g -n -W \
            --env "HOME=$home_dir" \
            --stdout "$emulator_stdout" \
            --stderr "$emulator_stderr" \
            -a "$ppsspp_bundle" \
            --args \
            --windowed --escape-exit "--graphics=$graphics" $debug_flag \
            "--log=$emulator_log" \
            "--appendconfig=$run_dir/script.ini" \
            "$app_dir/EBOOT.PBP" &
    else
        # shellcheck disable=SC2086
        HOME="$home_dir" "$ppsspp" \
            --windowed --escape-exit "--graphics=$graphics" $debug_flag \
            "--log=$emulator_log" \
            "--appendconfig=$run_dir/script.ini" \
            "$app_dir/EBOOT.PBP" >"$emulator_console" 2>&1 &
    fi
    emulator_pid=$!

    # The EBOOT calls sceKernelExitGame itself, which drops PPSSPP back to its
    # game browser rather than quitting, so poll for the sentinel and stop the
    # emulator ourselves.
    elapsed=0
    saw_outcome=0
    while kill -0 "$emulator_pid" 2>/dev/null; do
        if grep -q 'tilefinch-validation: outcome=clean-exit' \
            "$validation_log" 2>/dev/null; then
            saw_outcome=1
            stop_emulator
            emulator_pid=
            break
        fi
        if [ "$elapsed" -ge "$timeout_seconds" ]; then
            printf 'timed out after %ss waiting for the scripted run\n' \
                "$timeout_seconds" >&2
            stop_emulator
            emulator_pid=
            break
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    [ -z "${emulator_pid:-}" ] || wait "$emulator_pid" 2>/dev/null || true

    run_result="$result_dir/run-$run_index"
    mkdir -p "$run_result"
    for f in "$emulator_log" "$emulator_console" "$emulator_stdout" \
             "$emulator_stderr" "$validation_log"; do
        [ -f "$f" ] && cp "$f" "$run_result/" 2>/dev/null || true
    done
    for f in "$app_dir"/frame-mark-*.ppm "$app_dir"/data/frame-mark-*.ppm; do
        [ -f "$f" ] && cp "$f" "$run_result/" 2>/dev/null || true
    done
    # The harness's own lines, in order. Nothing here is wall-clock derived,
    # so the extraction is the whole normalizer.
    sed -n 's/^\(tilefinch-input-script: .*\)$/\1/p' "$validation_log" \
        2>/dev/null >"$run_result/trace.txt" || true
    [ "$saw_outcome" -eq 1 ] || {
        printf 'FAIL: run %s never reached a clean exit.\n' "$run_index" >&2
        return 1
    }
    if [ -z "$start_url" ] && ! grep -q \
            'tilefinch-boot-order: surface=native-home deferred=no url-override=0 trace=0 validation=0' \
            "$validation_log"; then
        printf '%s\n' \
            'FAIL: scripted input did not use the shipping native-HOME boot.' \
            "See $run_result/tilefinch-validation.txt" >&2
        return 1
    fi
    if [ -n "$start_url" ] && ! grep -q \
            'tilefinch-boot-order: .*url-override=1 trace=0 validation=0' \
            "$validation_log"; then
        printf '%s\n' \
            'FAIL: scripted input did not use the requested direct URL.' \
            "See $run_result/tilefinch-validation.txt" >&2
        return 1
    fi
    return 0
}

run_index=1
while [ "$run_index" -le "$runs" ]; do
    run_once "$run_index" || exit 1
    run_index=$((run_index + 1))
done

trace="$result_dir/run-1/trace.txt"
# Runtime input is deliberately replayed after the VM task returns. The
# script cursor can advance at intervening checkpoints, so its current step
# number (and re-observation of a live mark) is not the input's identity.
# Keep the raw trace; compare ordered receiver actions, exact action counts,
# completion and captures without these asynchronous cursor annotations.
if [ "$scenario" = runtime-input-live ] || [ "$scenario" = runtime-cancel-live ]; then
    run_index=1
    while [ "$run_index" -le "$runs" ]; do
        sed -E '/tilefinch-input-script: mark=/d; s/tilefinch-input-script: step=[0-9]+ action=/tilefinch-input-script: action=/' \
            "$result_dir/run-$run_index/trace.txt" \
            > "$result_dir/run-$run_index/semantic-trace.txt"
        run_index=$((run_index + 1))
    done
    trace="$result_dir/run-1/semantic-trace.txt"
fi
if [ "$scenario" = wikipedia-navigation-live ] || [ "$scenario" = wikipedia-article-section-live ] \
    || [ "$scenario" = wikipedia-section-input-live ] \
    || [ "$scenario" = youtube-results-focus-live ]; then
    # Faster paint changes which sampled cursor is current when a pending
    # page press is reported. Preserve marks and exact receiver order/counts.
    run_index=1
    while [ "$run_index" -le "$runs" ]; do
        # Live marks can fall between periodic supervisor presentations;
        # their latched capture is required below, not this optional observer.
        sed -E '/tilefinch-input-script: mark=expansion-busy /d; s/tilefinch-input-script: step=[0-9]+ action=/tilefinch-input-script: action=/' \
            "$result_dir/run-$run_index/trace.txt" \
            > "$result_dir/run-$run_index/semantic-trace.txt"
        run_index=$((run_index + 1))
    done
    trace="$result_dir/run-1/semantic-trace.txt"
fi
[ -s "$trace" ] || {
    printf 'FAIL: the run produced no tilefinch-input-script lines.\n' >&2
    printf 'The EBOOT probably could not read input-script.txt.\n' >&2
    exit 1
}

# Only a terminal success is eligible for comparison or promotion.  Merely
# printing an outcome is insufficient: stalled and load-failed runs also do
# that, and must never be blessable as a device golden.
grep -Eq 'tilefinch-input-script: outcome=(complete|exit-action)( |$)' \
    "$trace" || {
    printf 'FAIL: scripted input did not reach a successful outcome.\n' >&2
    grep 'tilefinch-input-script: outcome=' "$trace" >&2 || true
    exit 1
}

# A live scenario is useful only if it actually crossed the busy boundary.
# Keep this semantic gate ahead of --update-golden so a readiness mistake can
# never bless a trace that merely performed the same press after the work.
#
# The busy counters are read from the run's validation log rather than from
# the extracted trace, because they are host-speed telemetry: how many frames
# the browser spent not-ready differs between PPSSPP and 333 MHz Allegrex.
# The gate is about whether the boundary was crossed at all, which is a
# property of the script, so a non-zero test belongs here while the exact
# number must stay out of the goldened trace.
telemetry_log="$result_dir/run-1/tilefinch-validation.txt"
if grep -Eq '^[[:space:]]*(wait|tap|hold|press|mark)-live([[:space:]]|$)' \
        "$script_source"; then
    grep -Eq 'tilefinch-input-telemetry: .*busy-frames=[1-9][0-9]*' \
        "$telemetry_log" || {
        printf 'FAIL: live script spent no frame while the browser was busy.\n' \
            >&2
        exit 1
    }
fi
if grep -Eq '^[[:space:]]*(tap|hold|press)-live([[:space:]]|$)' \
        "$script_source"; then
    grep -Eq 'tilefinch-input-telemetry: .*busy-presses=[1-9][0-9]*' \
        "$telemetry_log" || {
        printf 'FAIL: live script delivered no press edge while busy.\n' >&2
        exit 1
    }
fi
if grep -E '^[[:space:]]*mark-live([[:space:]]|$)' "$script_source" \
        | grep -Ev '^[[:space:]]*mark-live[[:space:]]+(webgl-measure-start|webgl-measure-end|auto-controls|controls-exited)([[:space:]]|$)' \
        >/dev/null; then
    grep -Eq 'tilefinch-input-script: capture=.* written=1' "$trace" || {
        printf 'FAIL: live script wrote no temporal frame.\n' >&2
        exit 1
    }
fi
if [ "$scenario" = runtime-input-live ]; then
    # Raster-time input alone cannot qualify post-load runtime cooperation.
    grep -Eq 'tilefinch-ui-supervisor-input: scope=page-runtime queued=[1-4] dropped=0' \
        "$telemetry_log" || {
        printf 'FAIL: no lossless input queue during page runtime.\n' >&2
        exit 1
    }
    grep -Eq 'tilefinch-ui-supervisor: scope=page-runtime .*presentations=[1-9][0-9]* .*input-acks=[1-9][0-9]* .*max-ack=[1-9][0-9]*us' \
        "$telemetry_log" || {
        printf 'FAIL: runtime input had no measured visible acknowledgement.\n' >&2
        exit 1
    }
fi
if [ "$scenario" = runtime-cancel-live ]; then
    grep -Eq 'tilefinch-ui-supervisor: scope=page-runtime .*presentations=[1-9][0-9]* .*cancelled=1 .*input-acks=[1-9][0-9]* .*max-ack=[1-9][0-9]*us .*drained=1' \
        "$telemetry_log" || {
        printf 'FAIL: Circle did not cancel and drain post-load runtime work with visible feedback.\n' >&2
        exit 1
    }
fi
if [ "$scenario" = wikipedia-navigation-live ] || [ "$scenario" = youtube-results-focus-live ]; then
    focus_samples=4
    focus_budget=150000
    if [ "$scenario" = youtube-results-focus-live ]; then
        focus_samples=5
        focus_budget=200000
    fi
    awk -v expected="$focus_samples" -v budget="$focus_budget" '
    /tilefinch-focus-feedback:/ {
        count++;
        for (i=1;i<=NF;i++) if ($i ~ /^elapsed=/) {
            value=$i; sub(/^elapsed=/,"",value); sub(/us$/,"",value);
            samples++; if (value !~ /^[0-9]+$/) bad=1;
            if (value+0 > budget) slow=1;
        }
    } END {
        if (count != expected || samples != expected || bad) {
            print "FAIL: missing or malformed focus-publication samples." > "/dev/stderr";
            exit 1;
        }
        if (slow) {
            print "FAIL: focus publication exceeded the " budget " us emulator budget." > "/dev/stderr";
            exit 1;
        }
    }' "$telemetry_log" || exit 1
fi
if [ "$scenario" = wikipedia-navigation-live ]; then
    for mark in focus-one focus-two; do
        grep -Eq "tilefinch-input-focus-target: mark=$mark .*indicator=(authored|browser) " \
            "$telemetry_log" || {
            printf 'FAIL: Wikipedia %s has no visible focus.\n' "$mark" >&2
            exit 1
        }
    done
    grep -Eq 'tilefinch-control-activation: ok=1 .*elapsed=[1-9][0-9]*us' \
        "$telemetry_log" || {
        printf 'FAIL: Wikipedia menu activation was not measured successfully.\n' >&2
        exit 1
    }
    first_rect=$(sed -n 's/.*focus-probe: mark=focus-one visible=[01] rect=\([^ ]*\).*/\1/p' "$telemetry_log" | head -1)
    second_rect=$(sed -n 's/.*focus-probe: mark=focus-two visible=[01] rect=\([^ ]*\).*/\1/p' "$telemetry_log" | head -1)
    [ "$first_rect" != "$second_rect" ] || {
        printf 'FAIL: Wikipedia focus did not move to another element.\n' >&2
        exit 1
    }
    grep -Eq 'tilefinch-input-script-js: mark=focus-(one|two) .*pending=[1-9][0-9]* ' \
        "$telemetry_log" || {
        printf 'FAIL: Wikipedia focus was tested only after runtime work finished.\n' >&2
        exit 1
    }
    for mark in focus-one focus-two activated; do
        [ -s "$result_dir/run-1/frame-mark-$mark.ppm" ] || {
            printf 'FAIL: missing Wikipedia %s visual evidence.\n' "$mark" >&2
            exit 1
        }
    done
fi
if [ "$scenario" = wikipedia-section-input-live ]; then
    grep -Eq 'tilefinch-control-activation: ok=1 kind=2 .*relayouts=1$' "$telemetry_log" || exit 1
    grep -Eq 'tilefinch-ui-supervisor-input: scope=page-runtime queued=1 dropped=0' "$telemetry_log" || exit 1
    grep -Eq 'tilefinch-ui-supervisor-input: replay=0x[0-9a-f]+ remaining=0' "$telemetry_log" || exit 1
    grep -Eq 'tilefinch-activation-feedback: shown=1 elapsed=[0-9]+us' "$telemetry_log" || exit 1
    grep -Eq 'tilefinch-input-focus-target: mark=section-after .* id=History ' "$telemetry_log" || exit 1
    # A live press serviced by a later idle task is not expansion feedback.
    # Require the sampled button edge and queue publication inside activation,
    # with ordinary input replay strictly after the transaction completes.
    awk '/tilefinch-activation-feedback: shown=1 / {ack=NR}
         /tilefinch-input-script-edge: .*receiver=supervisor ready=0 / {busy=NR}
         /tilefinch-ui-supervisor-input: scope=page-runtime queued=1 dropped=0/ {queued=NR}
         /tilefinch-control-activation: ok=1 kind=2 / {done=NR}
         /tilefinch-ui-supervisor-input: replay=/ {replay=NR}
         END {exit(!(ack && ack < busy && busy < queued &&
                     queued < done && done < replay))}' "$telemetry_log" || exit 1
    awk '/tilefinch-ui-supervisor: scope=page-runtime/ && /input-acks=1 / {
        seen++;
        for(i=1;i<=NF;i++) if($i ~ /^max-ack=/) {
            value=$i; sub(/^max-ack=/,"",value); sub(/us$/,"",value);
            if(value !~ /^[0-9]+$/ || value+0 > 100000) bad=1;
        }
    } END {exit(seen != 1 || bad)}' "$telemetry_log" || exit 1
    for mark in expansion-busy section-after section-content; do
        [ -s "$result_dir/run-1/frame-mark-$mark.ppm" ] || exit 1
    done
fi
if [ "$scenario" = wikipedia-article-section-live ]; then
    grep -Eq 'tilefinch-control-activation: ok=1 kind=2 .*relayouts=1$' "$telemetry_log" || {
        printf 'FAIL: article section activation did not complete its layout.\n' >&2
        exit 1
    }
    grep -Eq 'tilefinch-input-focus-target: mark=section-after .* id=History ' "$telemetry_log" || {
        printf 'FAIL: article route did not activate History; inspect the captures.\n' >&2
        exit 1
    }
    grep -Eq 'tilefinch-focus-probe: mark=section-after visible=1 ' "$telemetry_log" || exit 1
    awk '/tilefinch-control-activation:/ {
        for (i=1;i<=NF;i++) if ($i ~ /^elapsed=/) {
            value=$i; sub(/^elapsed=/,"",value); sub(/us$/,"",value);
            samples++; if (value !~ /^[0-9]+$/ || value+0 > 1250000) bad=1;
        }
    } /tilefinch-control-layout:/ && /full=0 / {
        retained++;
        for (i=1;i<=NF;i++) if ($i ~ /^flow=/) {
            value=$i; sub(/^flow=/,"",value); sub(/us$/,"",value);
            flows++;
            if (value !~ /^[0-9]+$/ || value+0 > 750000) bad=1;
        }
    }
    END {exit(samples != 1 || retained != 1 || flows != 1 || bad)}' "$telemetry_log" || {
        printf 'FAIL: article expansion rebuilt resources, exceeded 1.25 s, or flow exceeded 750 ms in PPSSPP.\n' >&2
        exit 1
    }
    for mark in section-before section-after section-content; do
        [ -s "$result_dir/run-1/frame-mark-$mark.ppm" ] || exit 1
    done
    if cmp -s "$result_dir/run-1/frame-mark-section-before.ppm" \
              "$result_dir/run-1/frame-mark-section-after.ppm"; then
        printf 'FAIL: section activation did not change the captured page.\n' >&2
        exit 1
    fi
fi
if [ "$scenario" = section-disclosure-live ]; then
    [ "$(grep -Ec 'tilefinch-control-activation: ok=1 .*relayouts=1$' "$telemetry_log")" -eq 2 ] || {
        printf 'FAIL: section expand/collapse did not complete two layout actions.\n' >&2
        exit 1
    }
    grep -Eq 'tilefinch-input-script-js: mark=expanded .*pending=[1-9][0-9]* ' "$telemetry_log" || {
        printf 'FAIL: section expansion was tested only after runtime work finished.\n' >&2
        exit 1
    }
    for mark in before expanded collapsed; do
        [ -s "$result_dir/run-1/frame-mark-$mark.ppm" ] || exit 1
    done
fi
if [ "$scenario" = navigation-cancel-live ]; then
    validation_log="$result_dir/run-1/tilefinch-validation.txt"
    grep -Eq 'tilefinch-ui-supervisor: .*cancelled=1 .*input-acks=[1-9]' \
        "$validation_log" || {
        printf 'FAIL: cancellation did not cross the PSP UI supervisor.\n' >&2
        exit 1
    }
    grep -Eq 'tilefinch-navigation-cooperate: scope=interactive .*cancelled=1' \
        "$validation_log" || {
        printf 'FAIL: page navigation did not observe cancellation.\n' >&2
        exit 1
    }
    grep -Eq 'tilefinch-background-transport: .*stream-starts=[1-9][0-9]*' \
        "$validation_log" || {
        printf 'FAIL: page navigation never reached the shared transport worker.\n' \
            >&2
        exit 1
    }
    grep -q 'tilefinch-input-script: capture=cancel-ack written=1' \
        "$trace" || {
        printf 'FAIL: immediate cancellation acknowledgement was not captured.\n' \
            >&2
        exit 1
    }
fi

# This scenario exists specifically to cover a Deploy delivered while the
# installed game's scripts are still restoring. Reaching the end of the input
# file only proves that the receiver stayed alive; it must not bless a page
# left indefinitely on the shell's "Starting..." state.
if [ "$scenario" = treadline-immediate-deploy ]; then
    grep -Eq 'tilefinch-input-script-js: mark=deploy-(30|90|210) .*summary="TREADLINE-PLAYING" error=""' \
        "$telemetry_log" || {
        printf 'FAIL: immediate Deploy did not reach TREADLINE-PLAYING.\n' >&2
        grep 'tilefinch-input-script-js: mark=deploy-' "$telemetry_log" \
            >&2 || true
        exit 1
    }
fi
if [ "$scenario" = treadline-offline-controls ]; then
    gamepad_line=$(grep 'tilefinch-input-gamepad:' "$telemetry_log" \
        | tail -1 || true)
    printf '%s\n' "$gamepad_line" | grep -Eq \
        'connected-frames=[1-9][0-9]* button-frames=[1-9][0-9]* analog-frames=[1-9][0-9]* publications=[1-9][0-9]* connections=1 buttons=0x0000303F axis-x=-32767/32767 axis-y=-32767/32767' \
        || {
            printf '%s\n' \
                'FAIL: offline Treadline inputs did not cross the captured Gamepad path.' \
                "$gamepad_line" >&2
            exit 1
        }
fi
if [ "$scenario" = cursor-latency ]; then
    cursor_line=$(grep 'tilefinch-ui-cadence: phase=controlled-exit' \
        "$telemetry_log" | tail -1 || true)
    cursor_cadence_line=$(grep \
        'tilefinch-cursor-cadence: phase=controlled-exit' \
        "$telemetry_log" | tail -1 || true)
    cursor_samples=$(printf '%s\n' "$cursor_line" \
        | sed -n 's/.*cursor-samples=\([0-9][0-9]*\).*/\1/p')
    cursor_presents=$(printf '%s\n' "$cursor_line" \
        | sed -n 's/.*cursor-presents=\([0-9][0-9]*\).*/\1/p')
    cursor_coalesced=$(printf '%s\n' "$cursor_line" \
        | sed -n 's/.*cursor-coalesced=\([0-9][0-9]*\).*/\1/p')
    cursor_intervals=$(printf '%s\n' "$cursor_cadence_line" \
        | sed -n 's/.*intervals=\([0-9][0-9]*\).*/\1/p')
    cursor_cadence_average=$(printf '%s\n' "$cursor_cadence_line" \
        | sed -n 's/.*average=\([0-9][0-9]*\)us.*/\1/p')
    [ -n "$cursor_samples" ] && [ "$cursor_samples" -gt 0 ] \
        && [ "$cursor_samples" = "$cursor_presents" ] \
        && [ "$cursor_coalesced" = 0 ] || {
        printf '%s\n' \
            'FAIL: cursor samples did not receive one immediate accepted presentation.' \
            "$cursor_line" >&2
        exit 1
    }
    [ -n "$cursor_intervals" ] && [ "$cursor_intervals" -gt 0 ] \
        && [ -n "$cursor_cadence_average" ] \
        && [ "$cursor_cadence_average" -le 20000 ] || {
        printf '%s\n' \
            'FAIL: cursor sampling did not sustain near-display cadence.' \
            "$cursor_cadence_line" >&2
        exit 1
    }
fi

# Determinism first, then conformance: two runs that disagree with each other
# make a golden meaningless whichever way it compares.
run_index=2
while [ "$run_index" -le "$runs" ]; do
    compared_trace="$result_dir/run-$run_index/trace.txt"
    if [ "$scenario" = runtime-input-live ] || [ "$scenario" = runtime-cancel-live ] \
        || [ "$scenario" = wikipedia-navigation-live ] \
        || [ "$scenario" = wikipedia-article-section-live ] \
        || [ "$scenario" = wikipedia-section-input-live ] \
        || [ "$scenario" = youtube-results-focus-live ]; then
        compared_trace="$result_dir/run-$run_index/semantic-trace.txt"
    fi
    if ! cmp -s "$trace" "$compared_trace"; then
        printf 'FAIL: run 1 and run %s produced different traces.\n' \
            "$run_index" >&2
        diff -u "$trace" "$compared_trace" >&2 || true
        exit 1
    fi
    run_index=$((run_index + 1))
done

if [ "$update_golden" -eq 1 ]; then
    cp "$trace" "$golden"
    printf 'updated %s\n' "$golden"
    rm -rf "$session_dir"
    exit 0
fi

[ -f "$golden" ] || {
    printf '%s\n' \
        "missing golden: $golden" \
        "Create it with --update-golden after reviewing the trace." >&2
    exit 1
}
if ! diff -u "$golden" "$trace"; then
    printf '\nFAIL: the receiver trace does not match the golden.\n' >&2
    exit 1
fi

printf '\n--- %s ---\n' "$scenario"
grep 'covered ' "$trace" || true
grep 'outcome=' "$trace" || true
# Not goldened, but still worth seeing: it is how a reader tells a live run
# that raced the work from one that merely followed it.
grep 'tilefinch-input-telemetry: ' "$telemetry_log" || true
[ "$scenario" != cursor-latency ] \
    || grep -E \
        'tilefinch-(ui|cursor)-cadence: phase=controlled-exit' "$telemetry_log"
printf 'artifacts: %s\n' "$result_dir"
rm -rf "$session_dir"
printf '\nscripted input: PASS (%s, %s run(s))\n' "$scenario" "$runs"
