#!/bin/sh
set -eu

# Fast real-PSP edit loop. The wrapper establishes and verifies the Mac-side
# usbhostfs_pc bridge automatically before it touches the device.
#
#   scripts/psplink-device.sh memory  # zero Memory Stick writes
#   scripts/psplink-device.sh slot    # transactional slot-a deploy

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT/build-preset-psp-validation}
HOST_ROOT=${HOST_ROOT:-$BUILD_DIR}
PSPDEV=${PSPDEV:-}
JOBS=${JOBS:-8}
LINK_TIMEOUT_SECONDS=${LINK_TIMEOUT_SECONDS:-8}
DEPLOY_TIMEOUT_SECONDS=${DEPLOY_TIMEOUT_SECONDS:-30}

usage() {
    echo "usage: $0 memory | slot" >&2
    exit 2
}

[ "$#" -ge 1 ] || usage
MODE=$1
shift
[ "$#" -eq 0 ] || usage

[ -n "$PSPDEV" ] || {
    echo "PSPDEV must name the pspdev SDK root" >&2
    exit 2
}
PSPSH=${PSPSH:-$PSPDEV/bin/pspsh}
[ -x "$PSPSH" ] || {
    echo "pspsh not found at $PSPSH" >&2
    exit 2
}
export PSPDEV
PATH=$PSPDEV/bin:$PATH
export PATH

run_pspsh() {
    command_text=$1
    timeout=$2
    output=$(mktemp "${TMPDIR:-/tmp}/tilefinch-pspsh.XXXXXX")
    "$PSPSH" -e "$command_text" >"$output" 2>&1 &
    process=$!
    elapsed=0
    while kill -0 "$process" 2>/dev/null; do
        if [ "$elapsed" -ge "$timeout" ]; then
            kill "$process" 2>/dev/null || true
            wait "$process" 2>/dev/null || true
            cat "$output" >&2
            rm -f "$output"
            echo "PSPLink command timed out: $command_text" >&2
            return 1
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    status=0
    wait "$process" || status=$?
    cat "$output"
    rm -f "$output"
    return "$status"
}

unload_named_modules() {
    modules=$(run_pspsh "modlist" "$LINK_TIMEOUT_SECONDS")
    for module_name in "$@"; do
        if printf '%s\n' "$modules" | grep -q "Name: $module_name\$"; then
            run_pspsh "modstun @$module_name" "$LINK_TIMEOUT_SECONDS"
        fi
    done
}

require_module_absent() {
    module_name=$1
    modules=$(run_pspsh "modlist" "$LINK_TIMEOUT_SECONDS")
    if printf '%s\n' "$modules" | grep -q "Name: $module_name\$"; then
        echo "$module_name is still running on the PSP." >&2
        echo "Exit it normally with HOME before reloading; force-unloading " \
             "can orphan browser, network, and codec threads." >&2
        return 1
    fi
}

PSPSH=$PSPSH HOST_ROOT=$HOST_ROOT LINK_TIMEOUT_SECONDS=$LINK_TIMEOUT_SECONDS \
    "$ROOT/scripts/psplink-shell.sh" ready

case "$MODE" in
    memory)
        [ "$BUILD_DIR" = "$HOST_ROOT" ] || {
            echo "memory mode requires BUILD_DIR to be the served HOST_ROOT" >&2
            exit 2
        }
        # A named input script is a host0: runtime asset just like roots.pem
        # and the fonts.  Stage a committed scenario automatically so a clean
        # build directory cannot silently turn a scripted device run into an
        # ordinary interactive Home session.  Preserve explicitly staged
        # custom scripts when there is no repository scenario by that name.
        input_script=$(sed -n 's/^input_script=//p' "$BUILD_DIR/boot.cfg" \
            | sed -n '1p')
        if [ -n "$input_script" ]; then
            case "$input_script" in
                */*) ;;
                *)
                    scenario="$ROOT/tests/input-scripts/$input_script"
                    if [ -f "$scenario" ]; then
                        cp "$scenario" "$BUILD_DIR/$input_script"
                    fi
                    ;;
            esac
            [ -s "$BUILD_DIR/$input_script" ] || {
                echo "PSPLink input script missing: $input_script" >&2
                exit 1
            }
        fi
        # If a game was staged through stage-psp-game.sh and boot.cfg still
        # points at it, refresh the content digest before compiling the PRX.
        # This makes an edit after staging produce a new document and
        # subresource path even when the operator forgets to restage by hand.
        "$ROOT/scripts/stage-psp-game.sh" \
            --refresh-managed "$BUILD_DIR/boot.cfg"
        cmake --build "$BUILD_DIR" \
            --target psp-browser-script-dev-prx -j"$JOBS"
        # Do not load a module that can only reach Tilefinch's fatal boot
        # surface. These are runtime inputs for host0:, and the CMake target
        # owns them even when no link was necessary this invocation.
        for required in \
            "$BUILD_DIR/psp-browser-script-dev.prx" \
            "$BUILD_DIR/boot-defaults.cfg" \
            "$BUILD_DIR/roots.pem" \
            "$BUILD_DIR/fonts/TilefinchSans-Regular.ttf"; do
            [ -s "$required" ] || {
                echo "PSPLink browser asset missing: $required" >&2
                exit 1
            }
        done
        require_module_absent Tilefinch
        unload_named_modules tfdeploy
        load_output=$(run_pspsh \
            "ld host0:/psp-browser-script-dev.prx" \
            "$LINK_TIMEOUT_SECONDS")
        printf '%s\n' "$load_output"
        if printf '%s\n' "$load_output" | \
            grep -q 'Failed to Load/Start module'; then
            exit 1
        fi
        ;;
    slot)
        mkdir -p "$HOST_ROOT"
        "$ROOT/scripts/stage-psp-game.sh" \
            --refresh-managed "$BUILD_DIR/boot.cfg"
        cmake --build "$BUILD_DIR" --target psp-browser-script -j"$JOBS"
        make -C "$ROOT/tools/psplink-deploy"
        cp "$BUILD_DIR/EBOOT.PBP" "$HOST_ROOT/EBOOT-device-latest.PBP"
        cp "$ROOT/tools/psplink-deploy/tfdeploy.prx" "$HOST_ROOT/tfdeploy.prx"
        rm -f "$HOST_ROOT/tfdeploy.result"

        # Loading is asynchronous. The PRX publishes its result on host0 so
        # the host can distinguish a completed, promoted copy from a module
        # that merely started successfully.
        unload_named_modules tfdeploy
        run_pspsh "ld host0:/tfdeploy.prx" "$LINK_TIMEOUT_SECONDS"
        elapsed=0
        while [ ! -f "$HOST_ROOT/tfdeploy.result" ]; do
            if [ "$elapsed" -ge "$DEPLOY_TIMEOUT_SECONDS" ]; then
                echo "tfdeploy did not publish a result" >&2
                exit 1
            fi
            sleep 1
            elapsed=$((elapsed + 1))
        done
        cat "$HOST_ROOT/tfdeploy.result"
        deploy_ok=0
        grep -q '^status=ok ' "$HOST_ROOT/tfdeploy.result" || deploy_ok=$?
        [ "$deploy_ok" -eq 0 ] || exit 1

        ;;
    *) usage ;;
esac
