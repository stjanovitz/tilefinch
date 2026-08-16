#!/bin/sh
# Boot the browser EBOOT under PPSSPP on a hermetic scenario and check the
# device's cost counters against the committed baseline.
#
# The counter-baseline CTest measures the HOST lab; every device number the
# project quotes came from a manual script. This is the device side of that
# ratchet: budget totals, per-category owned bytes and peaks, the render-job
# work/slice counters, and the presentation cadence, compared field by field
# against tests/psp-device-cost-baseline.tsv.
#
# Usage:
#   scripts/run-ppsspp-device-cost.sh [options]
#     --build-dir DIR   PSP build directory (default build-preset-psp-validation)
#     --scenario NAME   start-page or menu-tour (default start-page)
#     --timeout N       seconds to wait for each run (default 300)
#     --runs N          boot N times; runs 2+ prove determinism (default 2)
#     --debug-log       add PPSSPP's -d syscall trace to the emulator log
#     --update-baseline rewrite the baseline from this run instead of checking
#     --skip-when-missing  exit 77 (CTest's skip code) instead of 2 when
#                       PPSSPP, python3, or the validation EBOOT is absent
#
# Regenerating the baseline (the only supported recipe):
#   cmake --preset psp -B build-preset-psp-validation \
#       -DTILEFINCH_PSP_VALIDATION_LOG=ON
#   cmake --build build-preset-psp-validation --target psp-browser-script
#   scripts/run-ppsspp-device-cost.sh --runs 2 --update-baseline
# --update-baseline still requires the runs to agree with each other, so a
# baseline can never be minted from a nondeterministic boot.
#
# Both scenarios are hermetic on purpose: an empty `url=` follows the ordinary
# native-HOME boot, so nothing is fetched and no wall-clock-dependent network
# work enters the counters.
#
#   start-page  `exit_after_report=1` takes the normal report and cleanup
#               paths without any controller injection: the floor cost of
#               booting the browser and shutting it down again. This is the
#               scenario the committed baseline and the CTest gate use: 119
#               counters compare exactly, 21 carry a measured band (the
#               resource category and the totals it feeds — see
#               tests/compare_psp_device_cost.py), and 14 wall-clock counters
#               are recorded but not compared.
#   menu-tour   replays tests/input-scripts/menu-tour.txt through the ordinary
#               native-HOME boot, then opens a local tab for page-only actions.
#               It remains measured but not baselined: its many UI operations
#               deliberately mutate profile and resource state, while the
#               start-page floor is the smaller deterministic boot oracle.
#
# In the baselined scenario the run is a fixed amount of work, which is what
# makes the owned-byte counters comparable at all.
#
# The shipping build compiles printf to (0); tilefinch-validation.txt only
# exists in a TILEFINCH_PSP_VALIDATION_LOG=ON build, which is why this refuses
# to run against any other EBOOT.
#
# The launch mechanics (isolated HOME, disposable re-signed app bundle,
# LaunchServices on macOS, kill-by-unique---log) are the ones
# scripts/run-ppsspp-network.sh established; this script is deliberately the
# same shape so they all age together.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$root/build-preset-psp-validation"
scenario=start-page
timeout_seconds=300
runs=2
debug_log=0
update_baseline=0
skip_when_missing=0

# CTest registers this test unconditionally so a lane that cannot run it says
# so out loud. `missing` turns a precondition failure into CTest's skip code
# instead of a red test; the reason is always printed either way.
missing_exit=2
missing() {
    printf '%s\n' "$@" >&2
    [ "$skip_when_missing" -eq 1 ] \
        && printf 'SKIP: the device-cost gate cannot run here.\n'
    exit "$missing_exit"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir=$2; shift 2 ;;
        --build-dir=*) build_dir=${1#--build-dir=}; shift ;;
        --scenario) scenario=$2; shift 2 ;;
        --scenario=*) scenario=${1#--scenario=}; shift ;;
        --timeout) timeout_seconds=$2; shift 2 ;;
        --timeout=*) timeout_seconds=${1#--timeout=}; shift ;;
        --runs) runs=$2; shift 2 ;;
        --runs=*) runs=${1#--runs=}; shift ;;
        --debug-log) debug_log=1; shift ;;
        --update-baseline) update_baseline=1; shift ;;
        --skip-when-missing) skip_when_missing=1; missing_exit=77; shift ;;
        -h|--help) sed -n '2,62p' "$0"; exit 0 ;;
        *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done

case "$runs:$timeout_seconds" in
    *[!0-9:]*|0:*|*:0) printf 'runs and timeout must be positive integers\n' >&2; exit 2 ;;
esac
if [ "$update_baseline" -eq 1 ] && [ "$runs" -lt 2 ]; then
    printf '%s\n' \
        'updating the device-cost baseline requires at least two agreeing runs' >&2
    exit 2
fi

case "$build_dir" in
    /*) ;;
    *) build_dir="$root/$build_dir" ;;
esac

input_script=
case "$scenario" in
    start-page) ;;
    menu-tour) input_script="$root/tests/input-scripts/menu-tour.txt" ;;
    *) printf 'unknown scenario: %s (start-page|menu-tour)\n' "$scenario" >&2
       exit 2 ;;
esac
if [ -n "$input_script" ] && [ ! -f "$input_script" ]; then
    printf 'missing input script: %s\n' "$input_script" >&2
    exit 2
fi

python3=${PYTHON3:-}
if [ -z "$python3" ]; then
    python3=$(command -v python3 || true)
fi
[ -n "$python3" ] \
    || missing "python3 is required for the cost comparator."
comparator="$root/tests/compare_psp_device_cost.py"
[ -f "$comparator" ] || missing "missing comparator: $comparator"

eboot="$build_dir/EBOOT.PBP"
[ -f "$eboot" ] || missing \
    "missing browser EBOOT: $eboot" \
    "Build it first:" \
    "  cmake --preset psp -B $build_dir -DTILEFINCH_PSP_VALIDATION_LOG=ON" \
    "  cmake --build $build_dir --target psp-browser-script"
# A shipping EBOOT compiles printf to (0): it would boot, exit cleanly, and
# write no counters at all. Refuse rather than compare an empty log.
grep -q '^TILEFINCH_PSP_VALIDATION_LOG:BOOL=ON$' "$build_dir/CMakeCache.txt" \
    2>/dev/null || missing \
    "$build_dir was not configured with TILEFINCH_PSP_VALIDATION_LOG=ON;" \
    "that EBOOT reports no counters."

ppsspp=${PPSSPP:-}
if [ -z "$ppsspp" ]; then
    ppsspp=$(command -v PPSSPPSDL || true)
fi
[ -n "$ppsspp" ] && [ -x "$ppsspp" ] || missing \
    "PPSSPPSDL was not found. Set PPSSPP=/absolute/path/to/PPSSPPSDL."
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
session_dir=$(mktemp -d "${run_base%/}/tilefinch-ppsspp-cost.XXXXXX")

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

result_dir="$build_dir/ppsspp-device-cost-latest"
rm -rf "$result_dir"
mkdir -p "$result_dir"

# Every run gets its own HOME. A profile or site-data store left behind by an
# earlier boot would change what the second boot allocates, which is exactly
# the kind of drift this test exists to catch — so it must not be able to
# leak in from the harness itself.
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
    validation_log="$app_dir/tilefinch-validation.txt"

    boot_input_script=
    exit_after_report=1
    if [ -n "$input_script" ]; then
        cp "$input_script" "$app_dir/input-script.txt"
        boot_input_script=input-script.txt
        # The script walks out through the menu's own EXIT row, so the run
        # must not short-circuit before the chrome has been touched.
        exit_after_report=0
    fi

    {
        printf '%s\n' \
            "# Generated only for the isolated PPSSPP device-cost run." \
            "url=" \
            "trace=none" \
            "profile=realistic" \
            "network_profile=1" \
            "limit=32" \
            "ticks=0" \
            "dump_frame=0" \
            "exit_after_report=$exit_after_report" \
            "input_script=$boot_input_script" \
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
    } >"$run_dir/cost.ini"
    cp "$run_dir/cost.ini" \
        "$home_dir/.config/ppsspp/PSP/SYSTEM/ppsspp.ini"

    debug_flag=
    [ "$debug_log" -eq 1 ] && debug_flag=-d

    printf 'PPSSPP device cost: %s run %s/%s\n' "$scenario" "$run_index" "$runs"
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
            "--appendconfig=$run_dir/cost.ini" \
            "$app_dir/EBOOT.PBP" &
    else
        # shellcheck disable=SC2086
        HOME="$home_dir" "$ppsspp" \
            --windowed --escape-exit $debug_flag \
            "--log=$emulator_log" \
            "--appendconfig=$run_dir/cost.ini" \
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
            printf 'timed out after %ss waiting for the cost run\n' \
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
    [ "$saw_outcome" -eq 1 ] || {
        printf 'FAIL: run %s never reached a clean exit.\n' "$run_index" >&2
        return 1
    }
    if [ -n "$input_script" ] \
        && ! grep -q \
            'tilefinch-boot-order: surface=native-home deferred=no url-override=0 trace=0 validation=0' \
            "$validation_log"; then
        printf '%s\n' \
            'FAIL: menu-tour did not use the shipping native-HOME boot.' \
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

set -- --scenario "$scenario"
[ "$update_baseline" -eq 1 ] && set -- "$@" --update
run_index=1
while [ "$run_index" -le "$runs" ]; do
    set -- "$@" "$result_dir/run-$run_index/tilefinch-validation.txt"
    run_index=$((run_index + 1))
done

# The comparator proves determinism across the supplied logs before it looks
# at the baseline: two boots that disagree with each other make a baseline
# meaningless whichever way it compares.
status=0
"$python3" "$comparator" "$@" || status=$?
printf 'artifacts: %s\n' "$result_dir"
[ "$status" -eq 0 ] || exit "$status"
rm -rf "$session_dir"
