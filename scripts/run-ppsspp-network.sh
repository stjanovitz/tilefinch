#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${PSP_BROWSER_PSP_BUILD_DIR:-}" ]; then
    build_dir=$PSP_BROWSER_PSP_BUILD_DIR
    build_dir_overridden=1
else
    build_dir="$root/build-preset-psp"
    build_dir_overridden=0
fi
ppsspp=${PPSSPP:-}
url=https://en.wikipedia.org/wiki/PlayStation_Portable
timeout_seconds=75
timeout_explicit=0
cancel_after_ms=0
preview_scroll=0
capture_frames=0
max_provisional_ms=0
play_media=0
media_stability_test=0
power_test=0
startup_test=0
media_fixture_test=0
raster_fixture_test=0
ge_present_probe=0
csc_order_probe=0
media_range_probe=0
launcher=0
update_e2e=0
update_e2e_url=
build=0
expect_network=ready
media_emulator_unsupported=0
media_emulator_missing_module=0
admitted_height=
youtube_test_url=${TILEFINCH_YOUTUBE_TEST_URL:-}

usage() {
    printf '%s\n' \
        "usage: scripts/run-ppsspp-network.sh [--build] [--launcher] [--update-e2e HTTPS_URL] [--url URL] [--timeout SECONDS] [--cancel-after-ms MILLISECONDS] [--preview-scroll] [--capture-frames] [--max-provisional-ms MILLISECONDS] [--play-media] [--media-stability-test] [--media-fixture-test] [--raster-fixture-test] [--ge-present-probe] [--csc-order-probe] [--media-range-probe] [--power-test] [--startup-test]" \
        "" \
        "Runs the live PSP EBOOT in an isolated PPSSPP home with WLAN enabled." \
        "--build uses the separate build-preset-psp-validation/ logging build." \
        "--launcher starts through the stable A/B launcher and slot-a tree." \
        "--update-e2e drives a signed check, download, install, launcher trial, and health confirmation against the supplied validation HTTPS endpoint." \
        "--power-test runs the validation-only two-minute automatic clock test." \
        "--startup-test runs the native HOME and background-WLAN cadence path for about 12 seconds." \
        "--media-stability-test runs two minutes of 360p playback and seeking." \
        "Live YouTube modes require TILEFINCH_YOUTUBE_TEST_URL; the public tree carries no real-video default." \
        "--media-fixture-test runs deterministic embedded 240p/360p decoder qualification." \
        "--raster-fixture-test checks the PSP page/font raster and saves its atlas." \
        "--ge-present-probe draws synthetic video frames through the graphics engine and checks the pixels." \
        "--csc-order-probe sweeps the firmware colour-conversion mode words over one decoded picture (hardware only)." \
        "--media-range-probe opens both bounded range sources and reads across fragment boundaries with no decoder (needs the network)." \
        "Results are copied beneath the selected PSP build directory."
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build)
            build=1
            shift
            ;;
        --url)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            url=$2
            shift 2
            ;;
        --timeout)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            timeout_seconds=$2
            timeout_explicit=1
            shift 2
            ;;
        --cancel-after-ms)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            cancel_after_ms=$2
            shift 2
            ;;
        --preview-scroll)
            preview_scroll=1
            shift
            ;;
        --capture-frames)
            capture_frames=1
            shift
            ;;
        --max-provisional-ms)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            max_provisional_ms=$2
            shift 2
            ;;
        --play-media)
            [ -n "$youtube_test_url" ] || {
                echo "--play-media requires TILEFINCH_YOUTUBE_TEST_URL" >&2
                exit 2
            }
            play_media=1
            # Exercise the native provider/player directly. Leaving the
            # default Wikipedia URL here produced a green network/page run
            # with validation_media_play enabled but no injected playback,
            # so the supposed media smoke never reached an AV module.
            url=$youtube_test_url
            shift
            ;;
        --media-stability-test)
            [ -n "$youtube_test_url" ] || {
                echo "--media-stability-test requires TILEFINCH_YOUTUBE_TEST_URL" >&2
                exit 2
            }
            media_stability_test=1
            url=$youtube_test_url
            shift
            ;;
        --media-fixture-test)
            media_fixture_test=1
            url=https://tilefinch.local/home
            expect_network=fixture
            shift
            ;;
        --raster-fixture-test)
            raster_fixture_test=1
            url=https://tilefinch.local/home
            expect_network=raster
            shift
            ;;
        --ge-present-probe)
            ge_present_probe=1
            url=https://tilefinch.local/home
            expect_network=ge-present
            shift
            ;;
        --csc-order-probe)
            csc_order_probe=1
            url=https://tilefinch.local/home
            expect_network=csc-order
            shift
            ;;
        --media-range-probe)
            [ -n "$youtube_test_url" ] || {
                echo "--media-range-probe requires TILEFINCH_YOUTUBE_TEST_URL" >&2
                exit 2
            }
            media_range_probe=1
            url=$youtube_test_url
            expect_network=media-range
            shift
            ;;
        --power-test)
            power_test=1
            url=https://tilefinch.local/home
            expect_network=warmup
            shift
            ;;
        --startup-test)
            startup_test=1
            url=https://tilefinch.local/home
            expect_network=warmup
            shift
            ;;
        --launcher)
            launcher=1
            shift
            ;;
        --update-e2e)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            update_e2e=1
            update_e2e_url=$2
            launcher=1
            expect_network=update
            url=https://tilefinch.local/home
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

if [ "$build" -eq 1 ] && [ "$build_dir_overridden" -eq 0 ]; then
    # Keep the ordinary PSP build and its user-facing EBOOT quiet. Validation
    # owns a separate cache because its logger is intentionally opt-in.
    build_dir="$root/build-preset-psp-validation"
fi

case "$url" in
    https://*) ;;
    *)
        printf 'PPSSPP network smoke URL must use https: %s\n' "$url" >&2
        exit 2
        ;;
esac
case "$url" in
    https://tilefinch.local/home|https://tilefinch.local/home/)
        if [ "$media_fixture_test" -eq 0 ] \
            && [ "$raster_fixture_test" -eq 0 ] \
            && [ "$ge_present_probe" -eq 0 ] \
            && [ "$csc_order_probe" -eq 0 ] \
            && [ "$update_e2e" -eq 0 ]; then
            expect_network=warmup
        fi
        ;;
esac
if [ "$update_e2e" -eq 1 ]; then
    case "$update_e2e_url" in
        https://*) ;;
        *)
            printf 'signed update validation URL must use https: %s\n' \
                "$update_e2e_url" >&2
            exit 2
            ;;
    esac
    if [ "$timeout_explicit" -eq 0 ] && [ "$timeout_seconds" -lt 180 ]; then
        timeout_seconds=180
    fi
fi
case "$timeout_seconds" in
    ''|*[!0-9]*)
        printf 'timeout must be an integer number of seconds\n' >&2
        exit 2
        ;;
esac
[ "$timeout_seconds" -ge 10 ] && [ "$timeout_seconds" -le 300 ] || {
    printf 'timeout must be between 10 and 300 seconds\n' >&2
    exit 2
}
case "$cancel_after_ms" in
    ''|*[!0-9]*)
        printf 'cancel delay must be an integer number of milliseconds\n' >&2
        exit 2
        ;;
esac
[ "$cancel_after_ms" -le 30000 ] || {
    printf 'cancel delay must be between 0 and 30000 milliseconds\n' >&2
    exit 2
}
case "$max_provisional_ms" in
    ''|*[!0-9]*)
        printf 'provisional deadline must be an integer number of milliseconds\n' >&2
        exit 2
        ;;
esac
[ "$max_provisional_ms" -le 30000 ] || {
    printf 'provisional deadline must be between 0 and 30000 milliseconds\n' >&2
    exit 2
}
[ "$cancel_after_ms" -eq 0 ] || [ "$max_provisional_ms" -eq 0 ] || {
    printf '%s\n' \
        "--max-provisional-ms cannot be combined with --cancel-after-ms" >&2
    exit 2
}
[ "$cancel_after_ms" -eq 0 ] || [ "$preview_scroll" -eq 0 ] || {
    printf '%s\n' \
        "--preview-scroll cannot be combined with --cancel-after-ms" >&2
    exit 2
}
[ "$play_media" -eq 0 ] || [ "$cancel_after_ms" -eq 0 ] || {
    printf '%s\n' \
        "--play-media cannot be combined with --cancel-after-ms" >&2
    exit 2
}
[ "$power_test" -eq 0 ] || [ "$cancel_after_ms" -eq 0 ] || {
    printf '%s\n' \
        "--power-test cannot be combined with --cancel-after-ms" >&2
    exit 2
}
[ "$power_test" -eq 0 ] || [ "$play_media" -eq 0 ] || {
    printf '%s\n' \
        "--power-test cannot be combined with --play-media" >&2
    exit 2
}
[ "$startup_test" -eq 0 ] || [ "$power_test" -eq 0 ] || {
    printf '%s\n' "--startup-test cannot be combined with --power-test" >&2
    exit 2
}
[ "$media_stability_test" -eq 0 ] || [ "$cancel_after_ms" -eq 0 ] || {
    printf '%s\n' \
        "--media-stability-test cannot be combined with --cancel-after-ms" >&2
    exit 2
}
[ "$media_stability_test" -eq 0 ] || [ "$play_media" -eq 0 ] || {
    printf '%s\n' \
        "--media-stability-test cannot be combined with --play-media" >&2
    exit 2
}
[ "$media_stability_test" -eq 0 ] || [ "$power_test" -eq 0 ] || {
    printf '%s\n' \
        "--media-stability-test cannot be combined with --power-test" >&2
    exit 2
}
[ "$power_test" -eq 0 ] || [ "$timeout_seconds" -ge 150 ] || {
    printf '%s\n' \
        "--power-test requires --timeout of at least 150 seconds" >&2
    exit 2
}
[ "$media_stability_test" -eq 0 ] \
    || [ "$timeout_seconds" -ge 150 ] || {
    printf '%s\n' \
        "--media-stability-test requires --timeout of at least 150 seconds" >&2
    exit 2
}
[ "$media_fixture_test" -eq 0 ] || [ "$cancel_after_ms" -eq 0 ] || {
    printf '%s\n' \
        "--media-fixture-test cannot be combined with --cancel-after-ms" >&2
    exit 2
}
[ "$media_fixture_test" -eq 0 ] || [ "$play_media" -eq 0 ] || {
    printf '%s\n' \
        "--media-fixture-test cannot be combined with --play-media" >&2
    exit 2
}
[ "$media_fixture_test" -eq 0 ] || [ "$media_stability_test" -eq 0 ] || {
    printf '%s\n' \
        "--media-fixture-test cannot be combined with --media-stability-test" >&2
    exit 2
}
# The present probe forces PPSSPP's software renderer -- the only
# configuration that writes graphics-engine output back into emulated PSP
# memory, and therefore the only one in which a program can read back the
# pixels it drew. It is also several times slower to boot, while never
# touching the network, so its default wait is longer than the network
# default. An explicit --timeout still wins.
if [ "$ge_present_probe" -eq 1 ] && [ "$timeout_explicit" -eq 0 ] \
    && [ "$timeout_seconds" -lt 120 ]; then
    timeout_seconds=120
fi
[ "$ge_present_probe" -eq 0 ] \
    || { [ "$raster_fixture_test" -eq 0 ] && [ "$media_fixture_test" -eq 0 ] \
         && [ "$play_media" -eq 0 ] && [ "$media_stability_test" -eq 0 ] \
         && [ "$power_test" -eq 0 ] && [ "$cancel_after_ms" -eq 0 ] \
         && [ "$csc_order_probe" -eq 0 ]; } || {
    printf '%s\n' \
        "--ge-present-probe cannot be combined with another probe" >&2
    exit 2
}
[ "$csc_order_probe" -eq 0 ] \
    || { [ "$raster_fixture_test" -eq 0 ] && [ "$media_fixture_test" -eq 0 ] \
         && [ "$play_media" -eq 0 ] && [ "$media_stability_test" -eq 0 ] \
         && [ "$power_test" -eq 0 ] && [ "$cancel_after_ms" -eq 0 ] \
         && [ "$media_range_probe" -eq 0 ]; } || {
    printf '%s\n' \
        "--csc-order-probe cannot be combined with another probe" >&2
    exit 2
}
[ "$media_range_probe" -eq 0 ] \
    || { [ "$raster_fixture_test" -eq 0 ] && [ "$media_fixture_test" -eq 0 ] \
         && [ "$play_media" -eq 0 ] && [ "$media_stability_test" -eq 0 ] \
         && [ "$power_test" -eq 0 ] && [ "$cancel_after_ms" -eq 0 ] \
         && [ "$ge_present_probe" -eq 0 ]; } || {
    printf '%s\n' \
        "--media-range-probe cannot be combined with another probe" >&2
    exit 2
}
[ "$raster_fixture_test" -eq 0 ] || [ "$cancel_after_ms" -eq 0 ] || {
    printf '%s\n' \
        "--raster-fixture-test cannot be combined with --cancel-after-ms" >&2
    exit 2
}
[ "$raster_fixture_test" -eq 0 ] || [ "$media_fixture_test" -eq 0 ] || {
    printf '%s\n' \
        "--raster-fixture-test cannot be combined with --media-fixture-test" >&2
    exit 2
}
[ "$raster_fixture_test" -eq 0 ] || [ "$play_media" -eq 0 ] || {
    printf '%s\n' \
        "--raster-fixture-test cannot be combined with --play-media" >&2
    exit 2
}

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
if [ "$(uname -s)" = Darwin ] \
    && [ "${PPSSPP_LAUNCHSERVICES:-1}" != 0 ]; then
    ppsspp_real=$(realpath "$ppsspp" 2>/dev/null || printf '%s' "$ppsspp")
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
    if [ -n "$ppsspp_bundle" ] && [ -d "$ppsspp_bundle" ]; then
        ppsspp_bundle_source=$ppsspp_bundle
        if codesign --verify --deep --strict "$ppsspp_bundle" \
                >/dev/null 2>&1; then
            ppsspp_launchservices=1
        else
            ppsspp_bundle=
        fi
    fi
fi

if [ "$build" -eq 1 ]; then
    [ -n "${PSPDEV:-}" ] || {
        printf '%s\n' "export PSPDEV before using --build" >&2
        exit 2
    }
    cmake --preset psp -B "$build_dir" \
        -DTILEFINCH_PSP_VALIDATION_LOG=ON
    cmake --build "$build_dir" --target psp-browser-script
    if [ "$launcher" -eq 1 ]; then
        cmake --build "$build_dir" --target tilefinch-psp-install-tree
    fi
fi

[ -f "$build_dir/CMakeCache.txt" ] \
    && grep -q '^TILEFINCH_PSP_VALIDATION_LOG:BOOL=ON$' \
        "$build_dir/CMakeCache.txt" || {
    printf '%s\n' \
        "PPSSPP network smoke requires a validation-logging PSP build." \
        "Use --build, or point PSP_BROWSER_PSP_BUILD_DIR at a build configured with -DTILEFINCH_PSP_VALIDATION_LOG=ON." >&2
    exit 2
}

[ -f "$build_dir/EBOOT.PBP" ] || {
    printf 'missing live PSP build: %s\n' "$build_dir/EBOOT.PBP" >&2
    exit 2
}
[ -f "$build_dir/roots.pem" ] || {
    printf 'missing PSP trust bundle: %s\n' "$build_dir/roots.pem" >&2
    exit 2
}

run_base=${TMPDIR:-/tmp}
run_dir=$(mktemp -d "${run_base%/}/tilefinch-ppsspp-network.XXXXXX")

# Homebrew 1.20.4 can contain a relocated MoltenVK symlink whose target is
# outside the Homebrew prefix. That invalidates the bundle seal, so
# LaunchServices reports a misleading "executable is missing" error. Repair a
# disposable copy for this run; never mutate the installed emulator.
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
    plutil -replace CFBundleVersion -string 1.20.4 \
        "$fixed_bundle/Contents/Info.plist"
    plutil -replace CFBundleShortVersionString -string 1.20.4 \
        "$fixed_bundle/Contents/Info.plist"
    plutil -replace CFBundleLongVersionString -string 1.20.4 \
        "$fixed_bundle/Contents/Info.plist"
    # The repaired copy must not reuse LaunchServices' registration for the
    # broken Homebrew bundle. A per-run identifier makes the disposable app a
    # distinct registration; otherwise `open` can resolve the valid copied
    # path through stale metadata and report kLSNoExecutableErr even though
    # Contents/MacOS/PPSSPPSDL is present and executable.
    plutil -replace CFBundleIdentifier \
        -string "org.tilefinch.ppsspp.validation.$$" \
        "$fixed_bundle/Contents/Info.plist"
    # Homebrew's downloaded bundle can carry com.apple.provenance on every
    # member. LaunchServices may then reject an otherwise valid ad-hoc-signed
    # disposable copy as "executable is missing". The copy is isolated and
    # throwaway, so remove inherited extended attributes before signing it.
    xattr -cr "$fixed_bundle"
    if [ -x "$fixed_bundle/Contents/MacOS/PPSSPPSDL" ] \
        && codesign --force --deep --sign - "$fixed_bundle" \
            >/dev/null 2>&1 \
        && codesign --verify --deep --strict "$fixed_bundle" \
            >/dev/null 2>&1; then
        # `open -a /absolute/bundle` registers this disposable bundle as part
        # of launching it. An explicit lsregister preflight is both redundant
        # and less reliable on macOS 26; falling back to direct execution when
        # it failed recreated the _RegisterApplication abort this path exists
        # to prevent.
        ppsspp_bundle=$fixed_bundle
        ppsspp_launchservices=1
    else
        printf '%s\n' \
            "PPSSPP's macOS app bundle is invalid and its isolated copy could not be repaired." \
            "Reinstall PPSSPP; direct execution is not safe on macOS from this environment." >&2
        exit 2
    fi
fi
if [ "$(uname -s)" = Darwin ] \
    && [ "${PPSSPP_LAUNCHSERVICES:-1}" != 0 ] \
    && [ "$ppsspp_launchservices" -ne 1 ]; then
    printf '%s\n' \
        "No safe LaunchServices PPSSPP bundle is available." \
        "Reinstall PPSSPP rather than invoking PPSSPPSDL directly." >&2
    exit 2
fi

result_dir="$build_dir/ppsspp-network-latest"
home_dir="$run_dir/home"
app_dir="$home_dir/.config/ppsspp/PSP/GAME/TILEFINCH"
emulator_log="$run_dir/ppsspp.log"
emulator_console="$run_dir/ppsspp-console.log"
emulator_stdout="$run_dir/ppsspp-stdout.log"
emulator_stderr="$run_dir/ppsspp-stderr.log"
mkdir -p "$app_dir" "$home_dir/.config/ppsspp/PSP/SYSTEM"
: >"$home_dir/.config/ppsspp/PSP/SYSTEM/controls.ini"
if [ "$launcher" -eq 1 ]; then
    install_tree="$build_dir/tilefinch-install/Tilefinch"
    [ -f "$install_tree/EBOOT.PBP" ] \
        && [ -f "$install_tree/slot-a/EBOOT.PBP" ] || {
        printf 'missing launcher install tree: %s\n' "$install_tree" >&2
        exit 2
    }
    cp -R "$install_tree/." "$app_dir/"
    config_path="$app_dir/data/boot-overrides.cfg"
    validation_log="$app_dir/data/tilefinch-validation.txt"
else
    cp "$build_dir/EBOOT.PBP" "$build_dir/roots.pem" "$app_dir/"
    for asset_dir in fonts voice-model; do
        if [ -d "$build_dir/$asset_dir" ]; then
            cp -R "$build_dir/$asset_dir" "$app_dir/$asset_dir"
        fi
    done
    config_path="$app_dir/boot.cfg"
    validation_log="$app_dir/tilefinch-validation.txt"
fi

# An opt-in release qualification can stage signed optional components into
# this run's isolated Memory Stick. The source directory is produced by the
# host glyph release-proof test; no developer PPSSPP home is read or changed.
component_stage=${TILEFINCH_PPSSPP_COMPONENT_STAGE:-}
component_validation_ticks=${TILEFINCH_PPSSPP_COMPONENT_TICKS:-180}
case "$component_validation_ticks" in
    ''|*[!0-9]*)
        printf 'component validation ticks must be an integer\n' >&2
        exit 2
        ;;
esac
if [ -n "$component_stage" ]; then
    [ "$launcher" -eq 1 ] || {
        printf '%s\n' \
            'optional-component staging requires --launcher' >&2
        exit 2
    }
    [ -d "$component_stage/components" ] \
        && [ -f "$component_stage/profile.cfg" ] || {
        printf 'invalid optional-component stage: %s\n' \
            "$component_stage" >&2
        exit 2
    }
    mkdir -p "$app_dir/components"
    cp -R "$component_stage/components/." "$app_dir/components/"
    cp "$component_stage/profile.cfg" "$app_dir/data/profile.cfg"
fi

{
    printf '%s\n' \
        "# Generated only for the isolated PPSSPP network smoke test." \
        "url=$url" \
        "trace=none" \
        "profile=realistic" \
        "network_profile=1" \
        "ticks=0" \
        "dump_frame=$((capture_frames * 2))" \
        "exit_after_report=0" \
        "interactive_validation_ticks=$(
            if [ "$update_e2e" -eq 1 ]; then
                printf 0
            elif [ "$media_stability_test" -eq 1 ]; then
                printf 0
            elif [ "$play_media" -eq 1 ]; then
                printf 1200
            elif [ "$power_test" -eq 1 ]; then
                printf 0
            elif [ "$startup_test" -eq 1 ]; then
                printf 720
            elif [ "$expect_network" = warmup ]; then
                printf 720
            elif [ -n "$component_stage" ]; then
                printf '%s' "$component_validation_ticks"
            else
                printf 180
            fi
        )" \
        "validation_cancel_after_ms=$cancel_after_ms" \
        "validation_preview_scroll=$preview_scroll" \
        "validation_media_play=$play_media" \
        "validation_media_stability_auto=$media_stability_test" \
        "validation_media_fixture_auto=$media_fixture_test" \
        "validation_raster_fixture_auto=$raster_fixture_test" \
        "validation_ge_present_probe=$ge_present_probe" \
        "validation_csc_order_probe=$csc_order_probe" \
        "validation_media_range_probe=$media_range_probe" \
        "validation_power_test_auto=$power_test" \
        "validation_update_auto=$update_e2e" \
        "validation_update_url=$update_e2e_url"
} >"$config_path"

# PPSSPP hardware renderers keep graphics-engine output in a host framebuffer
# object and never write it back into emulated PSP memory: with -d the log
# says "Creating 565 FBO at 04000000" and nothing else ever touches that
# address. A program that reads back its own drawn pixels therefore sees
# whatever was there before, however correct the draw was. The software
# renderer rasterizes into PSP memory, which is the only configuration in
# which the present probe can check anything at all.
#
# The key is SoftwareRenderer. SoftwareRendering is an older spelling PPSSPP
# still writes into its own ini and no longer reads, which is a silent way to
# think this is on when it is not.
software_rendering=False
if [ "$ge_present_probe" -eq 1 ]; then
    software_rendering=True
fi

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
        "SoftwareRenderer = $software_rendering" \
        "[SystemParam]" \
        "PSPModel = 1" \
        "PSPFirmwareVersion = 660"
} >"$run_dir/network.ini"
cp "$run_dir/network.ini" \
    "$home_dir/.config/ppsspp/PSP/SYSTEM/ppsspp.ini"

stop_emulator() {
    if [ -n "${emulator_pid:-}" ] \
        && kill -0 "$emulator_pid" 2>/dev/null; then
        if [ "$ppsspp_launchservices" -eq 1 ]; then
            # `open -W` is the child we can wait on. Terminate only the PPSSPP
            # instance whose command line contains this run's unique EBOOT.
            ppsspp_pids=$(
                pgrep -f -- "--log=$emulator_log" 2>/dev/null || true
            )
            if [ -n "$ppsspp_pids" ]; then
                # The isolated app is detached by LaunchServices, so the
                # `open` process is not its parent. Its unique --log path is
                # the reliable ownership token for this run.
                kill $ppsspp_pids 2>/dev/null || true
            fi
        else
            kill "$emulator_pid" 2>/dev/null || true
        fi
        wait "$emulator_pid" 2>/dev/null || true
    fi
}
cleanup() {
    stop_emulator
}
trap cleanup EXIT HUP INT TERM

printf 'PPSSPP network smoke: %s\n' "$url"
debug_flag=
[ "${PPSSPP_DEBUG_LOG:-0}" -eq 1 ] && debug_flag=-d
: >"$emulator_stdout"
: >"$emulator_stderr"

start_emulator() {
    # shellcheck disable=SC2086
    if [ "$ppsspp_launchservices" -eq 1 ]; then
        # On current macOS, directly exec'ing the SDL app from another GUI
        # application's process coalition can abort inside RegisterApplication
        # before PPSSPP reaches PSP code. LaunchServices establishes the normal
        # Cocoa application context while --env preserves the isolated PSP home.
        open -n -W \
            --env "HOME=$home_dir" \
            --stdout "$emulator_stdout" \
            --stderr "$emulator_stderr" \
            -a "$ppsspp_bundle" \
            --args \
            --windowed --escape-exit $debug_flag \
            "--log=$emulator_log" \
            "--appendconfig=$run_dir/network.ini" \
            "$app_dir/EBOOT.PBP" &
    else
        HOME="$home_dir" "$ppsspp" \
            --windowed --escape-exit $debug_flag \
            "--log=$emulator_log" \
            "--appendconfig=$run_dir/network.ini" \
            "$app_dir/EBOOT.PBP" >>"$emulator_console" 2>&1 &
    fi
    emulator_pid=$!
}

start_emulator

elapsed=0
update_cold_reboots=0
while kill -0 "$emulator_pid" 2>/dev/null; do
    if [ "$update_e2e" -eq 1 ] \
        && [ -f "$validation_log" ] \
        && grep -Eq \
            'tilefinch-update-e2e: outcome=(complete|failed)' \
            "$validation_log" \
        && grep -q 'tilefinch-validation: outcome=clean-exit' \
            "$validation_log"; then
        stop_emulator
        emulator_pid=
        break
    elif [ "$update_e2e" -eq 1 ] \
        && [ "$update_cold_reboots" -eq 0 ] \
        && { { [ -f "$validation_log" ] \
                && grep -q \
                    'tilefinch-update-e2e: outcome=installed action=cold-reboot' \
                    "$validation_log" \
                && grep -q 'tilefinch-validation: outcome=clean-exit' \
                    "$validation_log"; } \
            || { [ -f "$app_dir/data/tilefinch-validation.previous.txt" ] \
                && grep -q \
                    'tilefinch-update-e2e: outcome=installed action=cold-reboot' \
                    "$app_dir/data/tilefinch-validation.previous.txt" \
                && grep -q 'tilefinch-validation: outcome=clean-exit' \
                    "$app_dir/data/tilefinch-validation.previous.txt"; }; }; then
        # PPSSPP's nested LoadExec does not provide a reliable fresh argv[0]
        # for the candidate slot. A cold process is also the stronger launcher
        # test: it consumes the persisted PENDING journal and independently
        # verifies the staged slot before starting the trial.
        stop_emulator
        emulator_pid=
        update_cold_reboots=1
        start_emulator
        continue
    elif [ "$update_e2e" -eq 0 ] \
        && [ -f "$validation_log" ] \
        && grep -Eq \
            'tilefinch-validation: outcome=(clean-exit|qualification-failed)' \
            "$validation_log"; then
        # sceKernelExitGame returns to PPSSPP's game browser. The EBOOT has
        # completed cleanly, so close the isolated emulator process ourselves.
        #
        # A qualification-failed outcome is equally final: the boot mode has
        # printed its answer and is on its way to sceKernelExitGame, and no
        # amount of further waiting can change it. Waiting for clean-exit
        # alone turned every determinate probe failure into a full-timeout
        # run that reported "timed out" instead of what the probe found.
        stop_emulator
        emulator_pid=
        break
    fi
    if [ "$elapsed" -ge "$timeout_seconds" ]; then
        printf 'PPSSPP network smoke timed out after %ss\n' \
            "$timeout_seconds" >&2
        exit 1
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done
if [ -n "${emulator_pid:-}" ]; then
    wait "$emulator_pid" || emulator_status=$?
fi
emulator_status=${emulator_status:-0}
emulator_pid=
if [ "$ppsspp_launchservices" -eq 1 ]; then
    {
        cat "$emulator_stdout"
        cat "$emulator_stderr"
    } >"$emulator_console"
fi

rm -rf "$result_dir"
mkdir -p "$result_dir"
if [ -f "$emulator_log" ]; then
    cp "$emulator_log" "$result_dir/ppsspp.log"
fi
cp "$emulator_console" "$result_dir/ppsspp-console.log"
for artifact in \
    tilefinch-validation.txt tilefinch-crash.txt \
    tilefinch-last-error.txt \
    frame-provisional.ppm frame-device.ppm frame-raster.ppm
do
    artifact_root="$app_dir"
    if [ "$launcher" -eq 1 ]; then
        case "$artifact" in
            frame-*.ppm) artifact_root="$app_dir/slot-a" ;;
            *) artifact_root="$app_dir/data" ;;
        esac
    fi
    if [ -f "$artifact_root/$artifact" ]; then
        cp "$artifact_root/$artifact" "$result_dir/$artifact"
    fi
done

validation="$result_dir/tilefinch-validation.txt"
if [ "$emulator_status" -ne 0 ]; then
    printf 'PPSSPP exited with status %s; see %s\n' \
        "$emulator_status" "$result_dir" >&2
    exit 1
fi
if [ ! -f "$validation" ]; then
    printf 'Tilefinch did not produce a validation log; see %s\n' \
        "$result_dir" >&2
    exit 1
fi
if [ "$expect_network" = update ]; then
    if ! grep -q 'tilefinch-update-e2e: outcome=complete' "$validation"; then
        grep 'tilefinch-update-e2e:' "$validation" >&2 || true
        printf 'Signed A/B update qualification failed; see %s\n' \
            "$validation" >&2
        exit 1
    fi
    if ! grep -q 'tilefinch-update-health: confirmed' "$validation"; then
        printf 'Updated slot did not confirm trial health; see %s\n' \
            "$validation" >&2
        exit 1
    fi
elif [ "$expect_network" = ge-present ]; then
    # Pass and fail are both determinate answers here, so neither is a broken
    # run. The probe supplies its own pictures, so what it reports is a
    # property of the presenter and the panel, not of the network or a
    # decoder; report the outcome the probe reached and exit on it.
    if ! grep -q 'tilefinch-media-present-probe: event=pass' \
        "$validation"; then
        grep 'tilefinch-media-present-probe:' "$validation" || true
        if grep -q 'tilefinch-media-present-probe: .*passthrough=no' \
            "$validation"; then
            printf '%s\n' \
                'FAIL: the graphics engine did not copy the source pixel unchanged into the 8888 target.' \
                'Both ends are 8888, so there is no conversion to disagree about: the drawn= and source= values above are the whole answer, and the channel-map line says which byte moved. This is the probe reaching its answer, not a broken run.' >&2
        else
            printf 'PSP graphics-engine present probe failed; see %s\n' \
                "$validation" >&2
        fi
        exit 1
    fi
    # The mode switch is the other half of what this probe now proves: the
    # panel has to accept 8888 and hand itself back afterwards.
    if ! grep -q 'tilefinch-media-present-probe: .*surface=1/1' \
        "$validation"; then
        grep 'tilefinch-media-present-probe: event=' "$validation" || true
        printf '%s\n' \
            'FAIL: the fullscreen-video display surface was not entered and left cleanly.' >&2
        exit 1
    fi
elif [ "$expect_network" = raster ]; then
    if ! grep -q 'tilefinch-raster-fixture: event=psp-pass' \
            "$validation"; then
        printf 'PSP raster fixture failed; see %s\n' "$validation" >&2
        exit 1
    fi
    if [ ! -f "$result_dir/frame-raster.ppm" ]; then
        printf 'PSP raster atlas was not captured; see %s\n' \
            "$result_dir" >&2
        exit 1
    fi
elif [ "$expect_network" = fixture ]; then
    if grep -q 'tilefinch-media-fixture: event=hardware-pass' "$validation"; then
        :
    elif grep -q 'tilefinch-media-fixture: event=emulator-untested' \
            "$validation"; then
        media_emulator_unsupported=1
    else
        printf 'Deterministic media fixture failed; see %s\n' \
            "$validation" >&2
        exit 1
    fi
elif [ "$expect_network" = media-range ]; then
    # Network-dependent, and deliberately not a gate: the resolver, the CDN
    # and the wireless link all decide whether this can pass today.
    if ! grep -q 'tilefinch-media-range-probe: event=pass' "$validation"; then
        printf 'Range/fragment probe failed; see %s\n' "$validation" >&2
        grep 'tilefinch-media-range-probe:' "$validation" >&2 || true
        exit 1
    fi
elif [ "$expect_network" = csc-order ]; then
    # The sweep repeats the conversion of a picture the firmware decoded, so
    # the emulator reaches the same missing-mpeg_vsh boundary the embedded
    # fixture reaches and answers UNTESTED. That still proves the boot mode,
    # its budget accounting, and its exit handoff.
    if grep -q 'tilefinch-media-csc-probe: event=hardware-pass' \
            "$validation"; then
        :
    elif grep -q 'tilefinch-media-csc-probe: event=emulator-untested' \
            "$validation"; then
        media_emulator_unsupported=1
    else
        printf 'Colour-order probe failed; see %s\n' "$validation" >&2
        grep 'tilefinch-media-csc-probe:' "$validation" >&2 || true
        exit 1
    fi
elif [ "$expect_network" = ready ]; then
    if ! grep -q 'tilefinch-network: status=ready' "$validation"; then
        printf 'PSP network did not reach ready; see %s\n' "$validation" >&2
        exit 1
    fi
else
    if ! grep -q \
        'tilefinch-boot-order: surface=native-home deferred=no url-override=0 trace=0 validation=0' \
        "$validation"; then
        printf 'Homepage validation bypassed the shipping boot path; see %s\n' \
            "$validation" >&2
        exit 1
    fi
    if ! grep -q \
        'tilefinch-network: deferred for native HOME' \
        "$validation"; then
        printf 'Native HOME did not defer network startup; see %s\n' \
            "$validation" >&2
        exit 1
    fi
    if ! grep -q \
        'tilefinch-network-warmup: status=started after-homepage=yes' \
        "$validation"; then
        printf 'Native HOME did not start background Wi-Fi; see %s\n' \
            "$validation" >&2
        exit 1
    fi
    if ! grep -q 'tilefinch-network-warmup: status=ready' "$validation"; then
        printf 'Background Wi-Fi did not reach ready; see %s\n' \
            "$validation" >&2
        exit 1
    fi
    load_line=$(
        grep -n 'tilefinch-psp-script: load ok' "$validation" \
            | head -1 | cut -d: -f1
    )
    warmup_line=$(
        grep -n \
            'tilefinch-network-warmup: status=started after-homepage=yes' \
            "$validation" | head -1 | cut -d: -f1
    )
    if [ -z "$load_line" ] || [ -z "$warmup_line" ] \
        || [ "$load_line" -ge "$warmup_line" ]; then
        printf 'Wi-Fi began before the homepage completed; see %s\n' \
            "$validation" >&2
        exit 1
    fi
fi
if ! grep -q 'tilefinch-validation: outcome=clean-exit' "$validation"; then
    printf 'Tilefinch did not cleanly exit; see %s\n' "$validation" >&2
    exit 1
fi
if [ "$startup_test" -eq 1 ]; then
    if ! grep -q 'tilefinch-ui-cadence: phase=controlled-exit' \
            "$validation"; then
        printf 'Startup cadence metrics were not recorded; see %s\n' \
            "$validation" >&2
        exit 1
    fi
    grep 'tilefinch-ui-cadence: phase=controlled-exit' "$validation" \
        | tail -1
fi
if [ "$cancel_after_ms" -eq 0 ]; then
    if [ "$raster_fixture_test" -eq 1 ]; then
        if ! grep -q \
            'tilefinch-raster-fixture: event=psp-pass' \
            "$validation"; then
            printf 'Raster fixture did not qualify; see %s\n' \
                "$validation" >&2
            exit 1
        fi
    elif [ "$media_fixture_test" -eq 1 ]; then
        if [ "$media_emulator_unsupported" -eq 0 ] \
            && ! grep -q \
            'tilefinch-media-fixture: event=hardware-pass clips=3' \
            "$validation"; then
            printf 'Media fixture did not qualify all clips; see %s\n' \
                "$validation" >&2
            exit 1
        fi
    elif [ "$media_range_probe" -eq 1 ]; then
        # This mode never loads a page either; its own assertion is above.
        :
    elif [ "$ge_present_probe" -eq 1 ]; then
        # Nor does this one: it draws its own pictures and exits from the
        # qualification path before any navigation. Without this arm a
        # passing probe fell through to the page-load check below and was
        # reported as "HTTP/TLS/page load did not succeed". Its own
        # assertion is above; what is owed here is that every case the probe
        # enumerates was actually run.
        if ! grep -q 'tilefinch-media-present-probe: case=' "$validation"; then
            printf 'Present probe reported no cases; see %s\n' \
                "$validation" >&2
            exit 1
        fi
    elif [ "$csc_order_probe" -eq 1 ]; then
        # This mode never loads a page, so the load-ok arm below cannot apply
        # to it. Where the firmware decoder exists, every candidate must have
        # been asked and answered.
        if [ "$media_emulator_unsupported" -eq 0 ] \
            && ! grep -q \
            'tilefinch-media-csc-probe: outcome=complete candidates=8' \
            "$validation"; then
            printf 'Colour-order sweep did not reach every candidate; see %s\n' \
                "$validation" >&2
            exit 1
        fi
    elif [ "$media_stability_test" -eq 1 ]; then
        if grep -q 'AV ML 80010002 (load-mpeg-vsh)' "$validation"; then
            media_emulator_unsupported=1
            media_emulator_missing_module=1
        elif grep -Eq \
            'tilefinch-media-stability: event=failed reason="PSP (raw-NAL decoder unavailable in this environment|AV module .*failed: 0x80010002)"' \
            "$validation"; then
            media_emulator_unsupported=1
        elif ! grep -q \
            'tilefinch-media-stability: event=complete' \
            "$validation"; then
            printf 'Media stability test did not complete; see %s\n' \
                "$validation" >&2
            exit 1
        fi
        if [ "$media_emulator_unsupported" -eq 0 ] \
            && ! grep -q \
            'tilefinch-media-stability: event=started requested=360p actual=.*x360' \
            "$validation"; then
            printf 'Media stability test did not start at 360p; see %s\n' \
                "$validation" >&2
            exit 1
        fi
    elif [ "$power_test" -eq 1 ]; then
        if ! grep -q \
            'tilefinch-power-auto: event=summary reason=complete' \
            "$validation"; then
            printf 'Automatic power test did not complete; see %s\n' \
                "$validation" >&2
            exit 1
        fi
        segment_count=$(
            grep -c \
                'tilefinch-power-auto: event=segment-finish' \
                "$validation" || true
        )
        if [ "$segment_count" -ne 4 ]; then
            printf 'Automatic power test completed %s/4 segments; see %s\n' \
                "$segment_count" "$validation" >&2
            exit 1
        fi
        automatic_summary=$(
            grep 'tilefinch-power-auto: event=summary reason=complete' \
                "$validation" | tail -1
        )
        adaptive_ms=$(
            printf '%s\n' "$automatic_summary" \
                | sed -n 's/.* adaptive-ms=\([0-9][0-9]*\)ms .*/\1/p'
        )
        fixed_ms=$(
            printf '%s\n' "$automatic_summary" \
                | sed -n 's/.* fixed-ms=\([0-9][0-9]*\)ms .*/\1/p'
        )
        if [ -z "$adaptive_ms" ] || [ -z "$fixed_ms" ] \
            || [ "$adaptive_ms" -lt 59000 ] \
            || [ "$adaptive_ms" -gt 61000 ] \
            || [ "$fixed_ms" -lt 59000 ] \
            || [ "$fixed_ms" -gt 61000 ]; then
            printf 'Automatic power test policy time was unequal; see %s\n' \
                "$validation" >&2
            exit 1
        fi
    elif [ "$play_media" -eq 1 ]; then
        if grep -q 'AV ML 80010002 (load-mpeg-vsh)' "$validation"; then
            media_emulator_unsupported=1
            media_emulator_missing_module=1
        elif grep -Eq \
            'PSP (raw-NAL decoder unavailable in this environment|AV module .*failed: 0x80010002)' \
            "$validation"; then
            media_emulator_unsupported=1
        elif ! grep -q \
            'tilefinch-media-validation: playback-confirmed' \
            "$validation"; then
            printf 'Media playback did not advance; see %s\n' \
                "$validation" >&2
            exit 1
        fi
        if [ "$media_emulator_unsupported" -eq 1 ]; then
            # PPSSPP cannot cross the firmware Media Engine boundary, but it
            # must still prove that the real PSP executable rendered the
            # provider page and resolved current split direct streams at the
            # quality the shipping clamp admits. The executable's own
            # tilefinch-media-quality line records that decision, so the
            # check follows the clamp (240p today) and the wide-program A/B
            # knob instead of hardcoding a resolution, while a resolver
            # regression still cannot hide behind the skip. Whether the
            # bounded range sources also opened depends on where this
            # environment's boundary sits; each branch below asserts the
            # evidence its boundary can actually produce.
            admitted_height=$(
                sed -n \
                    's/.*tilefinch-media-quality: .*admitted=\([0-9][0-9]*\)p .*/\1/p' \
                    "$validation" | tail -1
            )
            if [ -z "$admitted_height" ]; then
                printf 'No admitted media quality was logged; see %s\n' \
                    "$validation" >&2
                exit 1
            fi
            grep -Eq \
                "tilefinch-media: resolved .*split=1 .*source=[0-9]+x${admitted_height} " \
                "$validation" \
                || { printf '%sp split provider resolution failed; see %s\n' \
                         "$admitted_height" "$validation" >&2; exit 1; }
            grep -Eq \
                'tilefinch-youtube: stage=player status=200 .*itag=' \
                "$validation" \
                || { printf 'YouTube player response was not accepted; see %s\n' \
                         "$validation" >&2; exit 1; }
            if [ "$media_emulator_missing_module" -eq 1 ]; then
                # PPSSPP does not expose flash0:/kd/mpeg_vsh.prx, and module
                # preparation deliberately precedes the range opens, so the
                # bounded range sources are never opened here and transport
                # cannot be exercised (a 240p retry would only repeat the
                # same module failure). Pin that boundary instead: AVCodec
                # loads, and the failing open reports zero video/audio range
                # activity, proving no transport failure hides in the skip.
                grep -q \
                    'tilefinch-media-modules: stage=load-avcodec status=0x00000000' \
                    "$validation" \
                    || { printf 'AVCodec did not load before the PPSSPP boundary; see %s\n' \
                             "$validation" >&2; exit 1; }
                grep -Eq \
                    'tilefinch-media: open-phase=[0-9]+ video-ranges=0/retry=0/0B/fail=0/http=0 audio-ranges=0/retry=0/0B/fail=0/http=0' \
                    "$validation" \
                    || { printf 'Media open did not stop at the module boundary before range transport; see %s\n' \
                             "$validation" >&2; exit 1; }
            else
                # The AV modules loaded, so this environment's boundary is
                # the decoder itself and the open crossed the range and
                # demux phases first: pipeline-ready is the proof that both
                # bounded range sources opened without transport failure.
                # The clamp admits a single quality per run, so there is no
                # second-quality fallback left to require.
                grep -Eq \
                    'tilefinch-media: pipeline-ready .*split=1' \
                    "$validation" \
                    || { printf 'Split pipeline did not open both range sources; see %s\n' \
                             "$validation" >&2; exit 1; }
            fi
        fi
        if [ "$media_emulator_unsupported" -eq 0 ] \
            && ! grep -q \
            'tilefinch-media: pipeline-ready .*split=1' \
            "$validation"; then
            printf 'Adaptive video/audio pipeline was not used; see %s\n' \
                "$validation" >&2
            exit 1
        fi
    elif ! grep -q 'tilefinch-psp-script: load ok' "$validation"; then
        printf 'HTTP/TLS/page load did not succeed; see %s\n' \
            "$validation" >&2
        exit 1
    fi
    if [ "$capture_frames" -eq 1 ]; then
        # Site-adapter pages can commit their bounded native document in one
        # step and therefore have no streaming provisional frame. The final
        # device frame is mandatory; provisional-specific modes below retain
        # their own stricter assertions.
        if [ ! -f "$result_dir/frame-device.ppm" ]; then
            printf 'Requested device frame was not captured: %s\n' \
                "$result_dir/frame-device.ppm" >&2
            exit 1
        fi
    fi
    if [ "$max_provisional_ms" -ne 0 ]; then
        provisional_us=$(
            sed -n \
                's/.*tilefinch-navigation-job: initial .* first=\([0-9][0-9]*\)us .*/\1/p' \
                "$validation" | tail -1
        )
        if [ -z "$provisional_us" ]; then
            printf 'No useful provisional viewport was presented; see %s\n' \
                "$validation" >&2
            exit 1
        fi
        maximum_provisional_us=$((max_provisional_ms * 1000))
        if [ "$provisional_us" -gt "$maximum_provisional_us" ]; then
            printf 'Useful provisional viewport missed %sms: %sus; see %s\n' \
                "$max_provisional_ms" "$provisional_us" "$validation" >&2
            exit 1
        fi
    fi
    if [ "$preview_scroll" -eq 1 ]; then
        if ! grep -q \
            'tilefinch-preview-validation: scroll=page-down' \
            "$validation"; then
            printf 'Provisional page-down was not observed; see %s\n' \
                "$validation" >&2
            exit 1
        fi
        if ! grep -Eq \
            'tilefinch-navigation-job: initial .*preview=1/2 .*scrolls=1 y=[1-9][0-9]*' \
            "$validation"; then
            printf 'Provisional scroll metrics did not converge; see %s\n' \
                "$validation" >&2
            exit 1
        fi
    fi
else
    if ! grep -q \
        'tilefinch-ui-supervisor: scope=initial .*cancelled=1 .*injected=1' \
        "$validation"; then
        printf 'Injected navigation stop was not observed; see %s\n' \
            "$validation" >&2
        exit 1
    fi
fi

if [ "$expect_network" = update ]; then
    network_summary=$(
        grep 'tilefinch-update-e2e: outcome=complete' "$validation" | tail -1
    )
elif [ "$expect_network" = ge-present ]; then
    # This has to be a branch of the same chain, not a statement before it:
    # as a separate `if` the assignment was immediately overwritten by the
    # chain's `else`, which greps for a warmup line this mode never prints.
    network_summary='tilefinch-network: not used by the present probe'
elif [ "$expect_network" = raster ]; then
    network_summary='tilefinch-network: not used by embedded raster fixture'
elif [ "$expect_network" = fixture ]; then
    network_summary='tilefinch-network: not used by embedded media fixture'
elif [ "$expect_network" = csc-order ]; then
    network_summary='tilefinch-network: not used by the colour-order probe'
elif [ "$expect_network" = media-range ]; then
    network_summary=$(
        grep 'tilefinch-network: status=ready' "$validation" | tail -1
    )
elif [ "$expect_network" = ready ]; then
    network_summary=$(
        grep 'tilefinch-network: status=ready' "$validation" | tail -1
    )
else
    network_summary=$(
        grep 'tilefinch-network-warmup: status=ready' \
            "$validation" | tail -1
    )
fi
if [ "$expect_network" = update ]; then
    load_summary=$(
        grep 'tilefinch-update-health: confirmed' "$validation" | tail -1)
elif [ "$ge_present_probe" -eq 1 ]; then
    # Same chain, same reason: as a separate `if` this was overwritten by the
    # `else` below, which greps for a page-load line this mode never prints.
    load_summary=$(
        grep 'tilefinch-media-present-probe: event=' "$validation" | tail -1)
elif [ "$raster_fixture_test" -eq 1 ]; then
    load_summary=$(grep 'tilefinch-raster-fixture: event=' "$validation" | tail -1)
elif [ "$media_fixture_test" -eq 1 ]; then
    load_summary=$(grep 'tilefinch-media-fixture: event=' "$validation" | tail -1)
elif [ "$csc_order_probe" -eq 1 ]; then
    load_summary=$(grep 'tilefinch-media-csc-probe: event=' "$validation" | tail -1)
elif [ "$media_range_probe" -eq 1 ]; then
    load_summary=$(grep 'tilefinch-media-range-probe: outcome=' "$validation" | tail -1)
else
    load_summary=$(grep 'tilefinch-psp-script: load ' "$validation" | tail -1)
fi
printf '%s\n%s\n' "$network_summary" "$load_summary"
if [ "$expect_network" = update ]; then
    printf 'PASS: signed HTTPS update, transactional install, launcher trial, and health confirmation (%ss)\n' \
        "$elapsed"
    printf 'Artifacts: %s\n' "$result_dir"
    rm -rf "$run_dir"
    exit 0
fi
if [ "$ge_present_probe" -eq 1 ]; then
    printf 'PASS: graphics-engine video presentation (%ss)\n' "$elapsed"
    grep 'tilefinch-media-present-probe: case=' "$validation" || true
    exit 0
fi
if [ "$media_range_probe" -eq 1 ]; then
    grep 'tilefinch-media-range-probe: source=' "$validation" || true
    printf 'PASS: both bounded range sources read across fragment boundaries (%ss)\n' \
        "$elapsed"
    exit 0
fi
if [ "$csc_order_probe" -eq 1 ]; then
    grep 'tilefinch-media-csc-probe: mode0=' "$validation" || true
    grep 'tilefinch-media-csc-probe: scaler-head=' "$validation" || true
    if [ "$media_emulator_unsupported" -eq 1 ]; then
        printf '%s\n' \
            'PARTIAL: the colour-order boot mode ran, reported its budget, and handed the console back.' \
            'UNTESTED: only a real PSP decodes the picture this sweep re-converts; run it there to answer the question.'
        exit 0
    fi
    printf 'PASS: firmware colour-conversion byte order swept over a decoded picture (%ss)\n' \
        "$elapsed"
    exit 0
fi
if [ "$raster_fixture_test" -eq 1 ]; then
    printf 'PASS: PSP-target page/font raster invariants and atlas capture (%ss)\n' \
        "$elapsed"
elif [ "$media_emulator_unsupported" -eq 1 ]; then
    if [ "$play_media" -eq 1 ]; then
        if [ "$media_emulator_missing_module" -eq 1 ]; then
            printf '%s\n' \
                'QUALIFIED: live PSP YouTube page, resolver, and AVCodec load reached the PPSSPP missing-mpeg_vsh boundary.'
        else
            printf 'QUALIFIED: live PSP YouTube page, resolver, %sp transport, and MP4 demux reached the firmware decoder boundary.\n' \
                "$admitted_height"
        fi
    fi
    if [ "$media_emulator_missing_module" -eq 1 ]; then
        printf '%s\n' \
            'PARTIAL: PPSSPP cannot load the CFW/firmware mpeg_vsh module, so range-open, demux, and frame output were not exercised.' \
            'UNTESTED: run the embedded media fixture on real PSP hardware before claiming playback support.'
    else
        printf '%s\n' \
            'PARTIAL: PSP demux/control flow reached the firmware decoder boundary; PPSSPP cannot qualify Media Engine frame output.' \
            'UNTESTED: run the embedded media fixture on real PSP hardware before claiming playback support.'
    fi
elif [ "$media_fixture_test" -eq 1 ]; then
    media_summary=$(
        grep 'tilefinch-media-fixture: event=hardware-pass' \
            "$validation" | tail -1
    )
    printf '%s\n' "$media_summary"
    printf 'PASS: firmware decoded changing 240p, 360p, seek, and replay frames (%ss)\n' \
        "$elapsed"
elif [ "$media_stability_test" -eq 1 ]; then
    media_summary=$(
        grep 'tilefinch-media-stability: event=complete' \
            "$validation" | tail -1
    )
    printf '%s\n' "$media_summary"
    printf 'PASS: automatic two-minute PSP 360p media test completed (%ss)\n' \
        "$elapsed"
elif [ "$power_test" -eq 1 ]; then
    power_summary=$(
        grep 'tilefinch-power-auto: event=summary reason=complete' \
            "$validation" | tail -1
    )
    printf '%s\n' "$power_summary"
    printf 'PASS: automatic two-minute PSP clock test completed (%ss)\n' \
        "$elapsed"
elif [ "$play_media" -eq 1 ]; then
    media_summary=$(
        grep 'tilefinch-media-validation: playback-confirmed' \
            "$validation" | tail -1
    )
    printf '%s\n' "$media_summary"
    printf 'PASS: PSP adaptive media playback advanced in PPSSPP (%ss)\n' \
        "$elapsed"
elif [ "$cancel_after_ms" -eq 0 ] && [ "$expect_network" = ready ]; then
    printf 'PASS: PSP networking, verified HTTPS, page load, and cleanup (%ss)\n' \
        "$elapsed"
elif [ "$cancel_after_ms" -eq 0 ]; then
    printf 'PASS: native HOME rendered before background Wi-Fi became ready (%ss)\n' \
        "$elapsed"
else
    printf 'PASS: PSP navigation stop at %sms and clean recovery (%ss)\n' \
        "$cancel_after_ms" "$elapsed"
fi
printf 'Artifacts: %s\n' "$result_dir"
rm -rf "$run_dir"
