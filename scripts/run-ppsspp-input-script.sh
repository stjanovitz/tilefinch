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
#     --timeout N       seconds to wait for the run (default 300)
#     --runs N          replay N times and require identical traces (default 1)
#     --debug-log       add PPSSPP's -d syscall trace to the emulator log
#     --update-golden   rewrite the golden from this run instead of diffing
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

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir=$2; shift 2 ;;
        --build-dir=*) build_dir=${1#--build-dir=}; shift ;;
        --script) scenario=$2; shift 2 ;;
        --script=*) scenario=${1#--script=}; shift ;;
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

case "$runs:$timeout_seconds" in
    *[!0-9:]*|0:*|*:0) printf 'runs and timeout must be positive integers\n' >&2; exit 2 ;;
esac

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
ppsspp_direct_fallback=0
if [ "$(uname -s)" = Darwin ] && [ "${PPSSPP_LAUNCHSERVICES:-1}" != 0 ]; then
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
if [ "$(uname -s)" = Darwin ] \
    && [ "${PPSSPP_LAUNCHSERVICES:-1}" != 0 ] \
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
    xattr -cr "$fixed_bundle"
    if codesign --force --deep --sign - "$fixed_bundle" >/dev/null 2>&1 \
        && codesign --verify --deep --strict "$fixed_bundle" \
            >/dev/null 2>&1; then
        lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
        if "$lsregister" -f "$fixed_bundle" >/dev/null 2>&1; then
            ppsspp_bundle=$fixed_bundle
            ppsspp_launchservices=1
        else
            ppsspp_direct_fallback=1
        fi
    else
        printf '%s\n' \
            "PPSSPP's macOS app bundle is invalid and could not be repaired." >&2
        exit 2
    fi
fi
if [ "$(uname -s)" = Darwin ] \
    && [ "${PPSSPP_LAUNCHSERVICES:-1}" != 0 ] \
    && [ "$ppsspp_launchservices" -ne 1 ] \
    && [ "$ppsspp_direct_fallback" -ne 1 ]; then
    printf '%s\n' "No safe LaunchServices PPSSPP bundle is available." >&2
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
    cp "$build_dir/EBOOT.PBP" "$build_dir/roots.pem" "$app_dir/"
    for asset_dir in fonts voice-model; do
        if [ -d "$build_dir/$asset_dir" ]; then
            cp -R "$build_dir/$asset_dir" "$app_dir/$asset_dir"
        fi
    done
    cp "$script_source" "$app_dir/input-script.txt"
    validation_log="$app_dir/tilefinch-validation.txt"

    {
        printf '%s\n' \
            "# Generated only for the isolated PPSSPP scripted-input run." \
            "url=" \
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
    } >"$app_dir/boot.cfg"

    {
        printf '%s\n' \
            "[General]" \
            "FirstRun = False" \
            "Enable Logging = True" \
            "AutoRun = True" \
            "[Network]" \
            "EnableWlan = True" \
            "InfrastructureAutoDNS = True" \
            "[Graphics]" \
            "GraphicsBackend = 0 (OPENGL)" \
            "[SystemParam]" \
            "PSPModel = 1" \
            "PSPFirmwareVersion = 660"
    } >"$run_dir/script.ini"
    cp "$run_dir/script.ini" \
        "$home_dir/.config/ppsspp/PSP/SYSTEM/ppsspp.ini"

    debug_flag=
    [ "$debug_log" -eq 1 ] && debug_flag=-d

    printf 'PPSSPP scripted input: %s run %s/%s\n' \
        "$scenario" "$run_index" "$runs"
    if [ "$ppsspp_launchservices" -eq 1 ]; then
        : >"$emulator_stdout"
        : >"$emulator_stderr"
        # shellcheck disable=SC2086
        open -n -W \
            --env "HOME=$home_dir" \
            --stdout "$emulator_stdout" \
            --stderr "$emulator_stderr" \
            -a "$ppsspp_bundle" \
            --args \
            --windowed --escape-exit $debug_flag \
            "--log=$emulator_log" \
            "--appendconfig=$run_dir/script.ini" \
            "$app_dir/EBOOT.PBP" &
    else
        # shellcheck disable=SC2086
        HOME="$home_dir" "$ppsspp" \
            --windowed --escape-exit $debug_flag \
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
    for f in "$app_dir"/frame-mark-*.ppm; do
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
    if ! grep -q \
        'tilefinch-boot-order: surface=native-home deferred=no url-override=0 trace=0 validation=0' \
        "$validation_log"; then
        printf '%s\n' \
            'FAIL: scripted input did not use the shipping native-HOME boot.' \
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
if grep -Eq '^[[:space:]]*mark-live([[:space:]]|$)' "$script_source"; then
    grep -Eq 'tilefinch-input-script: capture=.* written=1' "$trace" || {
        printf 'FAIL: live script wrote no temporal frame.\n' >&2
        exit 1
    }
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
if [ "$scenario" = cursor-latency ]; then
    cursor_line=$(grep 'tilefinch-ui-cadence: phase=controlled-exit' \
        "$telemetry_log" | tail -1 || true)
    cursor_samples=$(printf '%s\n' "$cursor_line" \
        | sed -n 's/.*cursor-samples=\([0-9][0-9]*\).*/\1/p')
    cursor_presents=$(printf '%s\n' "$cursor_line" \
        | sed -n 's/.*cursor-presents=\([0-9][0-9]*\).*/\1/p')
    cursor_coalesced=$(printf '%s\n' "$cursor_line" \
        | sed -n 's/.*cursor-coalesced=\([0-9][0-9]*\).*/\1/p')
    [ -n "$cursor_samples" ] && [ "$cursor_samples" -gt 0 ] \
        && [ "$cursor_samples" = "$cursor_presents" ] \
        && [ "$cursor_coalesced" = 0 ] || {
        printf '%s\n' \
            'FAIL: cursor samples did not receive one immediate accepted presentation.' \
            "$cursor_line" >&2
        exit 1
    }
fi

# Determinism first, then conformance: two runs that disagree with each other
# make a golden meaningless whichever way it compares.
run_index=2
while [ "$run_index" -le "$runs" ]; do
    if ! cmp -s "$trace" "$result_dir/run-$run_index/trace.txt"; then
        printf 'FAIL: run 1 and run %s produced different traces.\n' \
            "$run_index" >&2
        diff -u "$trace" "$result_dir/run-$run_index/trace.txt" >&2 || true
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
    || grep 'tilefinch-ui-cadence: phase=controlled-exit' "$telemetry_log"
printf 'artifacts: %s\n' "$result_dir"
rm -rf "$session_dir"
printf '\nscripted input: PASS (%s, %s run(s))\n' "$scenario" "$runs"
