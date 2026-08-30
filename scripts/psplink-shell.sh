#!/bin/sh
set -eu

# Establish the Mac-side host0 bridge before issuing a bounded PSPLink command.
# The PSP can visibly be inside PSPLink while pspsh still reports connection
# refused: that means usbhostfs_pc is absent on the host, not that PSPLink on
# the device is broken.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HOST_ROOT=${HOST_ROOT:-$ROOT/build-preset-psp-validation}
PSPDEV=${PSPDEV:-}
PSPSH=${PSPSH:-}
USBHOSTFS=${USBHOSTFS:-}
LINK_TIMEOUT_SECONDS=${LINK_TIMEOUT_SECONDS:-8}
BRIDGE_START_TIMEOUT_SECONDS=${BRIDGE_START_TIMEOUT_SECONDS:-12}
STATE_DIR=${PSPLINK_STATE_DIR:-${TMPDIR:-/tmp}/tilefinch-psplink}

if [ -z "$PSPSH" ]; then
    [ -n "$PSPDEV" ] || {
        echo "PSPDEV or PSPSH is required" >&2
        exit 2
    }
    PSPSH=$PSPDEV/bin/pspsh
fi
case "$PSPSH" in
    */*) ;;
    *) PSPSH=$(command -v "$PSPSH" 2>/dev/null || true) ;;
esac
if [ -z "$USBHOSTFS" ]; then
    if [ -n "$PSPDEV" ]; then
        USBHOSTFS=$PSPDEV/bin/usbhostfs_pc
    else
        USBHOSTFS=$(dirname -- "$PSPSH")/usbhostfs_pc
    fi
fi
case "$USBHOSTFS" in
    */*) ;;
    *) USBHOSTFS=$(command -v "$USBHOSTFS" 2>/dev/null || true) ;;
esac
[ -x "$PSPSH" ] || { echo "pspsh not found at $PSPSH" >&2; exit 2; }
[ -x "$USBHOSTFS" ] || {
    echo "usbhostfs_pc not found at $USBHOSTFS" >&2
    exit 2
}

usage() {
    echo "usage: $0 ready | exec 'pspsh command'" >&2
    exit 2
}

mkdir -p "$HOST_ROOT" "$STATE_DIR"
BRIDGE_PID=$STATE_DIR/.tilefinch-usbhostfs.pid
BRIDGE_LOG=$STATE_DIR/.tilefinch-usbhostfs.log
BRIDGE_ROOT=$STATE_DIR/.tilefinch-usbhostfs.root

run_pspsh() {
    command_text=$1
    timeout=$2
    output=$(mktemp "${TMPDIR:-/tmp}/tilefinch-psplink-shell.XXXXXX")
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
            return 124
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    result=0
    wait "$process" || result=$?
    cat "$output"
    rm -f "$output"
    return "$result"
}

probe_ready() {
    probe=$(run_pspsh ver "$LINK_TIMEOUT_SECONDS" 2>&1) || return 1
    printf '%s\n' "$probe" | grep -q 'PSPLink v'
}

recorded_bridge_live() {
    [ -f "$BRIDGE_PID" ] || return 1
    pid=$(sed -n '1p' "$BRIDGE_PID")
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    kill -0 "$pid" 2>/dev/null
}

recorded_bridge_serves_host_root() {
    [ -f "$BRIDGE_ROOT" ] || return 1
    recorded_root=$(sed -n '1p' "$BRIDGE_ROOT")
    [ "$recorded_root" = "$HOST_ROOT" ]
}

adopt_legacy_bridge_record() {
    [ -f "$BRIDGE_PID" ] && return 0
    legacy_pid_file=$HOST_ROOT/.tilefinch-usbhostfs.pid
    [ -f "$legacy_pid_file" ] || return 0
    legacy_pid=$(sed -n '1p' "$legacy_pid_file")
    case "$legacy_pid" in ''|*[!0-9]*) return 0 ;; esac
    kill -0 "$legacy_pid" 2>/dev/null || return 0
    printf '%s\n' "$legacy_pid" >"$BRIDGE_PID"
    printf '%s\n' "$HOST_ROOT" >"$BRIDGE_ROOT"
}

stop_recorded_bridge() {
    recorded_bridge_live || return 0
    pid=$(sed -n '1p' "$BRIDGE_PID")
    kill "$pid" 2>/dev/null || true
    elapsed=0
    while kill -0 "$pid" 2>/dev/null && [ "$elapsed" -lt 3 ]; do
        sleep 1
        elapsed=$((elapsed + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "usbhostfs_pc did not stop while changing host0 root." >&2
        return 1
    fi
    rm -f "$BRIDGE_PID" "$BRIDGE_ROOT"
}

ensure_ready() {
    adopt_legacy_bridge_record

    if recorded_bridge_live; then
        if ! recorded_bridge_serves_host_root; then
            stop_recorded_bridge
        elif probe_ready; then
            return 0
        fi
    elif probe_ready; then
        echo "PSPLink is using an untracked usbhostfs_pc bridge." >&2
        echo "Stop it once, then rerun this wrapper so host0 ownership is known." >&2
        return 1
    fi

    if ! recorded_bridge_live; then
        rm -f "$BRIDGE_PID"
        : >"$BRIDGE_LOG"
        nohup "$USBHOSTFS" "$HOST_ROOT" >"$BRIDGE_LOG" 2>&1 &
        bridge_process=$!
        printf '%s\n' "$bridge_process" >"$BRIDGE_PID"
        printf '%s\n' "$HOST_ROOT" >"$BRIDGE_ROOT"
    fi

    elapsed=0
    while [ "$elapsed" -lt "$BRIDGE_START_TIMEOUT_SECONDS" ]; do
        if probe_ready; then return 0; fi
        if grep -q '^bind: Operation not permitted' "$BRIDGE_LOG" \
                2>/dev/null; then
            stop_recorded_bridge || true
            echo "The host environment denied usbhostfs_pc its local socket." \
                >&2
            echo "Rerun this command with permission to bind host sockets; " \
                "PSPLink on the device does not need to be relaunched." >&2
            return 1
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    echo "PSPLink is not reachable after ${BRIDGE_START_TIMEOUT_SECONDS}s." >&2
    echo "The host bridge log is $BRIDGE_LOG" >&2
    if recorded_bridge_live; then
        echo "usbhostfs_pc is running; reconnect USB or relaunch PSPLink." >&2
    elif [ -f "$BRIDGE_LOG" ]; then
        tail -20 "$BRIDGE_LOG" >&2
    fi
    return 1
}

[ "$#" -ge 1 ] || usage
mode=$1
shift
case "$mode" in
    ready)
        [ "$#" -eq 0 ] || usage
        ensure_ready
        printf 'PSPLink ready; host0: %s\n' "$HOST_ROOT"
        ;;
    exec)
        [ "$#" -eq 1 ] || usage
        ensure_ready
        run_pspsh "$1" "$LINK_TIMEOUT_SECONDS"
        ;;
    *) usage ;;
esac
