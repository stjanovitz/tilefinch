#!/bin/sh
set -eu

# Deterministic real-PSP regression for the ordinary provider journey:
# natural autoplay, three rapid seek edges, and a committed latest target.
# usbhostfs_pc must already serve BUILD_DIR as host0:. The browser, profile,
# input script, and validation log all live on host0; this run performs zero
# Memory Stick writes.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT/build-preset-psp-validation}
PSPSH=${PSPSH:-pspsh}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-240}
YOUTUBE_TEST_URL=${TILEFINCH_YOUTUBE_TEST_URL:-}
[ -n "$YOUTUBE_TEST_URL" ] || {
    echo "TILEFINCH_YOUTUBE_TEST_URL is required" >&2
    exit 2
}
LOG=$BUILD_DIR/tilefinch-validation.txt
SCENARIO=youtube-autoplay-seek-live.txt
BACKUP=$(mktemp -d "${TMPDIR:-/tmp}/tilefinch-psplink-ux.XXXXXX")

cleanup() {
    # Discard only profile state created by this validation run, then restore
    # whatever host-side developer profile existed before it.
    if [ -f "$BUILD_DIR/profile.cfg" ]; then
        mv "$BUILD_DIR/profile.cfg" "$BACKUP/profile.run.cfg"
    fi
    if [ -f "$BUILD_DIR/profile.cfg.bak" ]; then
        mv "$BUILD_DIR/profile.cfg.bak" "$BACKUP/profile.run.cfg.bak"
    fi
    if [ -f "$BACKUP/profile.cfg" ]; then
        mv "$BACKUP/profile.cfg" "$BUILD_DIR/profile.cfg"
    fi
    if [ -f "$BACKUP/profile.cfg.bak" ]; then
        mv "$BACKUP/profile.cfg.bak" "$BUILD_DIR/profile.cfg.bak"
    fi
}
trap cleanup EXIT HUP INT TERM

cmake --build "$BUILD_DIR" --target psp-browser-script-dev-prx -j8
cp "$ROOT/tests/input-scripts/$SCENARIO" "$BUILD_DIR/$SCENARIO"

if [ -f "$BUILD_DIR/profile.cfg" ]; then
    mv "$BUILD_DIR/profile.cfg" "$BACKUP/profile.cfg"
fi
if [ -f "$BUILD_DIR/profile.cfg.bak" ]; then
    mv "$BUILD_DIR/profile.cfg.bak" "$BACKUP/profile.cfg.bak"
fi
if [ -f "$LOG" ]; then
    mv "$LOG" "$BACKUP/validation.previous.txt"
fi

cat > "$BUILD_DIR/boot.cfg" <<EOF
url=$YOUTUBE_TEST_URL
trace=none
profile=realistic
network_profile=1
ticks=0
dump_frame=0
exit_after_report=0
interactive_validation_ticks=7200
validation_cancel_after_ms=0
validation_preview_scroll=0
validation_media_play=0
validation_media_stability_auto=0
validation_media_stability_seconds=120
validation_media_seek_permille=0
validation_media_lifecycle_auto=0
validation_media_refusal_reset=1
validation_media_reset_mode=2
validation_power_test_auto=0
input_script=youtube-autoplay-seek-live.txt
EOF

"$PSPSH" -e ver
"$PSPSH" -e modlist > "$BACKUP/modules.before.txt"
if grep -q 'Name: Tilefinch$' "$BACKUP/modules.before.txt"; then
    "$PSPSH" -e 'modstun @Tilefinch' > "$BACKUP/module-stop.txt" 2>&1
    "$PSPSH" -e modlist > "$BACKUP/modules.after.txt"
    if grep -q 'Name: Tilefinch$' "$BACKUP/modules.after.txt"; then
        echo "FAIL: stale Tilefinch module could not be unloaded" >&2
        cat "$BACKUP/module-stop.txt" >&2
        exit 1
    fi
fi

load_output=$("$PSPSH" -e "ld host0:/psp-browser-script-dev.prx" 2>&1)
printf '%s\n' "$load_output"
if printf '%s\n' "$load_output" | grep -q 'Failed to Load/Start module'; then
    echo "FAIL: PSPLink rejected the freshly built validation PRX" >&2
    exit 1
fi

elapsed=0
while [ "$elapsed" -lt "$TIMEOUT_SECONDS" ]; do
    if [ -f "$LOG" ] \
        && grep -q "tilefinch-log: finish outcome=" "$LOG"; then
        if grep -q "tilefinch-log: finish outcome=clean-exit" "$LOG"; then
            python3 "$ROOT/scripts/verify-psplink-youtube-ux.py" "$LOG"
            exit 0
        fi

        outcome=$(sed -n \
            's/^tilefinch-log: finish outcome=\([^ ]*\).*/\1/p' \
            "$LOG" | tail -1)
        echo "FAIL: PSP run finished with outcome=${outcome:-unknown}" >&2
        tail -40 "$LOG" >&2
        exit 1
    fi
    sleep 5
    elapsed=$((elapsed + 5))
done

echo "FAIL: PSPLink YouTube UX run exceeded ${TIMEOUT_SECONDS}s" >&2
if [ -f "$LOG" ]; then tail -40 "$LOG" >&2; fi
exit 1
