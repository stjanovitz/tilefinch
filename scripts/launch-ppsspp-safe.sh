#!/bin/sh
set -eu

dry_run=0
if [ "${1:-}" = "--dry-run" ]; then
    dry_run=1
    shift
fi

ppsspp=${PPSSPP:-}
if [ -z "$ppsspp" ]; then
    ppsspp=$(command -v PPSSPPSDL || true)
fi
[ -n "$ppsspp" ] && [ -x "$ppsspp" ] || {
    printf '%s\n' \
        "PPSSPPSDL was not found. Set PPSSPP=/absolute/path/to/PPSSPPSDL." >&2
    exit 2
}

if [ "$(uname -s)" != Darwin ]; then
    if [ "$dry_run" -eq 1 ]; then
        printf 'PPSSPP executable: %s\nLaunch mode: direct (non-macOS)\n' \
            "$ppsspp"
        exit 0
    fi
    exec "$ppsspp" "$@"
fi

# Directly executing SDL/Cocoa applications from a shell owned by another GUI
# application's process coalition can abort in _RegisterApplication before
# PPSSPP reaches any PSP code. Resolve the app bundle and let LaunchServices
# establish a normal Cocoa application context instead.
ppsspp_real=$(realpath "$ppsspp" 2>/dev/null || printf '%s' "$ppsspp")
ppsspp_bundle=
case "$ppsspp_real" in
    *.app/Contents/MacOS/*)
        ppsspp_bundle=${ppsspp_real%%.app/Contents/MacOS/*}.app
        ;;
    *)
        ppsspp_candidate=$(
            CDPATH= cd -- "$(dirname -- "$ppsspp_real")/.." 2>/dev/null \
                && pwd
        )/PPSSPPSDL.app
        if [ -d "$ppsspp_candidate" ]; then
            ppsspp_bundle=$ppsspp_candidate
        fi
        ;;
esac
[ -n "$ppsspp_bundle" ] && [ -d "$ppsspp_bundle" ] || {
    printf '%s\n' \
        "Could not resolve PPSSPPSDL's macOS app bundle." \
        "Direct execution is unsafe from this environment; reinstall PPSSPP or set PPSSPP to its app executable." >&2
    exit 2
}

temporary_bundle_root=
launch_args_file=
cleanup() {
    if [ -n "$temporary_bundle_root" ]; then
        rm -rf "$temporary_bundle_root"
    fi
    if [ -n "$launch_args_file" ]; then
        rm -f "$launch_args_file"
    fi
}
trap cleanup EXIT HUP INT TERM

launch_bundle=$ppsspp_bundle
if ! codesign --verify --deep --strict "$ppsspp_bundle" >/dev/null 2>&1; then
    temporary_bundle_root=$(
        mktemp -d "${TMPDIR:-/tmp}/tilefinch-ppsspp-launch.XXXXXX"
    )
    launch_bundle="$temporary_bundle_root/PPSSPPSDL.app"
    ditto "$ppsspp_bundle" "$launch_bundle"

    # Homebrew 1.20.4 can contain a relocated MoltenVK symlink whose target is
    # outside the prefix. Repair and ad-hoc sign only this disposable copy.
    fixed_molten="$launch_bundle/Contents/Frameworks/libMoltenVK.dylib"
    molten_prefix=$(brew --prefix molten-vk 2>/dev/null || true)
    molten_dylib="$molten_prefix/lib/libMoltenVK.dylib"
    if [ -L "$fixed_molten" ] && [ ! -e "$fixed_molten" ] \
        && [ -f "$molten_dylib" ]; then
        unlink "$fixed_molten"
        cp "$molten_dylib" "$fixed_molten"
        chmod u+w "$fixed_molten"
    fi

    plutil -replace CFBundleVersion -string 1.20.4 \
        "$launch_bundle/Contents/Info.plist"
    plutil -replace CFBundleShortVersionString -string 1.20.4 \
        "$launch_bundle/Contents/Info.plist"
    plutil -replace CFBundleLongVersionString -string 1.20.4 \
        "$launch_bundle/Contents/Info.plist"
    xattr -cr "$launch_bundle"
    codesign --force --deep --sign - "$launch_bundle" >/dev/null 2>&1
    codesign --verify --deep --strict "$launch_bundle" >/dev/null 2>&1 || {
        printf '%s\n' \
            "PPSSPP's app bundle is invalid and its isolated copy could not be repaired." \
            "Reinstall the Homebrew ppsspp package." >&2
        exit 2
    }
fi

if [ "$dry_run" -eq 1 ]; then
    printf 'PPSSPP bundle: %s\nLaunch mode: macOS LaunchServices\n' \
        "$launch_bundle"
    exit 0
fi

# LaunchServices starts GUI applications with `/` as their working directory.
# Resolve caller-relative file arguments before crossing that boundary.
caller_directory=$PWD
launch_args_file=$(mktemp "${TMPDIR:-/tmp}/tilefinch-ppsspp-args.XXXXXX")
for argument do
    case "$argument" in
        /*|*://*) resolved=$argument ;;
        --log=*)
            value=${argument#--log=}
            case "$value" in
                /*) resolved=$argument ;;
                *) resolved="--log=$caller_directory/$value" ;;
            esac
            ;;
        -*) resolved=$argument ;;
        *) resolved="$caller_directory/$argument" ;;
    esac
    printf '%s\0' "$resolved" >>"$launch_args_file"
done
xargs -0 open -n -W -a "$launch_bundle" --args <"$launch_args_file"
