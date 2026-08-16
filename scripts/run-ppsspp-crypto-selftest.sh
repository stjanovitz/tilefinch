#!/bin/sh
# Run the crypto selftest EBOOT under PPSSPP and gate on its result.
#
# The device-side gate for the Allegrex bignum core
# (docs/engineering/PSP_TRANSPORT.md). PPSSPP executes
# madd/maddu and the HI/LO pair faithfully, so mbed TLS's own mpi/rsa/ecp
# selftests plus the baked known-answer vectors are a real check of the
# assembly, not just of the build.
#
# Usage:
#   scripts/run-ppsspp-crypto-selftest.sh [options]
#     --build-dir DIR   PSP build directory (default build-psp-update-proof)
#     --timeout N       seconds to wait for the EBOOT (default 180)
#     --label NAME      tag for the result directory (default: the core
#                       name the EBOOT reports)
#
# Exits 0 only when the EBOOT prints `tilefinch-crypto: outcome=pass` with
# `failures=0`. The full log is copied to
# <build-dir>/ppsspp-crypto-selftest-latest/.
#
# The launch mechanics (isolated HOME, disposable re-signed app bundle,
# LaunchServices on macOS, kill-by-unique---log) are the ones
# scripts/run-ppsspp-network.sh established; this script is deliberately
# the same shape so both age together.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$root/build-psp-update-proof"
timeout_seconds=180
label=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir=$2; shift 2 ;;
        --build-dir=*) build_dir=${1#--build-dir=}; shift ;;
        --timeout) timeout_seconds=$2; shift 2 ;;
        --timeout=*) timeout_seconds=${1#--timeout=}; shift ;;
        --label) label=$2; shift 2 ;;
        --label=*) label=${1#--label=}; shift ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done

case "$build_dir" in
    /*) ;;
    *) build_dir="$root/$build_dir" ;;
esac

eboot="$build_dir/crypto-selftest/EBOOT.PBP"
[ -f "$eboot" ] || {
    printf '%s\n' \
        "missing crypto selftest EBOOT: $eboot" \
        "Build it first:  cmake --build $build_dir --target psp-crypto-selftest" >&2
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
run_dir=$(mktemp -d "${run_base%/}/tilefinch-ppsspp-crypto.XXXXXX")

# Homebrew's bundle seal can be invalid (a relocated MoltenVK symlink, or a
# resource-less signature). Repair a disposable copy for this run; never
# mutate the installed emulator.
if [ "$(uname -s)" = Darwin ] \
    && [ "${PPSSPP_LAUNCHSERVICES:-1}" != 0 ] \
    && [ "$ppsspp_launchservices" -eq 0 ] \
    && [ -n "$ppsspp_bundle_source" ]; then
    fixed_bundle="$run_dir/PPSSPPSDL.app"
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

home_dir="$run_dir/home"
app_dir="$home_dir/.config/ppsspp/PSP/GAME/TILEFINCH"
emulator_log="$run_dir/ppsspp.log"
emulator_console="$run_dir/ppsspp-console.log"
emulator_stdout="$run_dir/ppsspp-stdout.log"
emulator_stderr="$run_dir/ppsspp-stderr.log"
mkdir -p "$app_dir" "$home_dir/.config/ppsspp/PSP/SYSTEM"
: >"$home_dir/.config/ppsspp/PSP/SYSTEM/controls.ini"
cp "$eboot" "$app_dir/EBOOT.PBP"

{
    printf '%s\n' \
        "[General]" \
        "FirstRun = False" \
        "Enable Logging = True" \
        "AutoRun = True" \
        "[Graphics]" \
        "GraphicsBackend = 0 (OPENGL)" \
        "[SystemParam]" \
        "PSPModel = 1" \
        "PSPFirmwareVersion = 660"
} >"$run_dir/selftest.ini"
cp "$run_dir/selftest.ini" \
    "$home_dir/.config/ppsspp/PSP/SYSTEM/ppsspp.ini"

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
cleanup() { stop_emulator; }
trap cleanup EXIT HUP INT TERM

printf 'PPSSPP crypto selftest: %s\n' "$eboot"
if [ "$ppsspp_launchservices" -eq 1 ]; then
    : >"$emulator_stdout"
    : >"$emulator_stderr"
    open -n -W \
        --env "HOME=$home_dir" \
        --stdout "$emulator_stdout" \
        --stderr "$emulator_stderr" \
        -a "$ppsspp_bundle" \
        --args \
        --windowed --escape-exit -d \
        "--log=$emulator_log" \
        "--appendconfig=$run_dir/selftest.ini" \
        "$app_dir/EBOOT.PBP" &
else
    HOME="$home_dir" "$ppsspp" \
        --windowed --escape-exit -d \
        "--log=$emulator_log" \
        "--appendconfig=$run_dir/selftest.ini" \
        "$app_dir/EBOOT.PBP" >"$emulator_console" 2>&1 &
fi
emulator_pid=$!

# The EBOOT calls sceKernelExitGame itself, which drops PPSSPP back to its
# game browser rather than quitting, so poll for the sentinel and stop the
# emulator ourselves.
elapsed=0
saw_outcome=0
while kill -0 "$emulator_pid" 2>/dev/null; do
    if grep -q 'tilefinch-crypto: outcome=' "$emulator_log" 2>/dev/null; then
        saw_outcome=1
        stop_emulator
        emulator_pid=
        break
    fi
    if [ "$elapsed" -ge "$timeout_seconds" ]; then
        printf 'timed out after %ss waiting for the selftest\n' \
            "$timeout_seconds" >&2
        stop_emulator
        emulator_pid=
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done
[ -z "${emulator_pid:-}" ] || wait "$emulator_pid" 2>/dev/null || true

core=$(sed -n 's/.*tilefinch-crypto: boot core=\([a-z0-9-]*\).*/\1/p' \
    "$emulator_log" 2>/dev/null | tail -1)
[ -n "$core" ] || core=unknown
[ -n "$label" ] || label=$core

result_dir="$build_dir/ppsspp-crypto-selftest-latest/$label"
rm -rf "$result_dir"
mkdir -p "$result_dir"
for f in "$emulator_log" "$emulator_console" "$emulator_stdout" \
         "$emulator_stderr"; do
    [ -f "$f" ] && cp "$f" "$result_dir/" 2>/dev/null || true
done
# The EBOOT's own lines, in order, with PPSSPP's log prefix stripped.
sed -n 's/.*\(tilefinch-crypto: .*\)/\1/p' "$emulator_log" 2>/dev/null \
    >"$result_dir/selftest.txt" || true

printf '\n--- %s ---\n' "$label"
cat "$result_dir/selftest.txt" 2>/dev/null || true
printf 'artifacts: %s\n' "$result_dir"

rm -rf "$run_dir"

[ "$saw_outcome" -eq 1 ] || {
    printf '\nFAIL: the selftest never reported an outcome.\n' >&2
    exit 1
}
grep -q 'tilefinch-crypto: outcome=pass' "$result_dir/selftest.txt" || {
    printf '\nFAIL: selftest reported failures.\n' >&2
    exit 1
}
grep -q 'tilefinch-crypto: core=.* failures=0' "$result_dir/selftest.txt" || {
    printf '\nFAIL: selftest failure count is not zero.\n' >&2
    exit 1
}
printf '\ncrypto selftest: PASS (core=%s)\n' "$core"
