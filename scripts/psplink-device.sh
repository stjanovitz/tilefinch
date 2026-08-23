#!/bin/sh
set -eu

# Fast real-PSP edit loop. usbhostfs_pc must already serve HOST_ROOT as host0.
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

case "$MODE" in
    memory)
        [ "$BUILD_DIR" = "$HOST_ROOT" ] || {
            echo "memory mode requires BUILD_DIR to be the served HOST_ROOT" >&2
            exit 2
        }
        cmake --build "$BUILD_DIR" \
            --target psp-browser-script-dev-prx -j"$JOBS"
        unload_named_modules Tilefinch tfdeploy
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
