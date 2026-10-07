#!/bin/sh
# PPSSPP @111 MHz run of the news-load scenario for one page.
# usage: ppsspp.sh MODE NAME URL [extra runner args]
#   capture: live WLAN, records the PSP's own requests to NAME/psp-capture
#   replay:  response-keyed replay of NAME/psp-capture (or $TRACE)
set -u
MODE=$1 NAME=$2 URL=$3; shift 3
R=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
S=${SURVEY_DIR:-$R/scratch/survey}/$NAME
mkdir -p "$S"
: "${PSPDEV:?export PSPDEV first}"; export PATH=$PSPDEV/bin:$PATH
export TILEFINCH_PPSSPP_CPU_MHZ=111
case $MODE in
capture) rm -rf "$S/psp-capture"; NET="--capture-trace $S/psp-capture" ;;
replay)  NET="--trace ${TRACE:-$S/psp-capture} --trace-keyed" ;;
*) echo bad mode; exit 2 ;;
esac
start=$(date +%s)
"$R/scripts/run-ppsspp-input-script.sh" --build-dir "$R/build-preset-psp-validation" \
  --script ${SCRIPT:-news-load} --url "$URL" --measure --timeout ${TIMEOUT:-600} \
  --boot validation_js_profile=0 $NET "$@" > "$S/ppsspp-$MODE.out" 2>&1
rc=$?
echo "rc=$rc wall-s=$(( $(date +%s) - start ))" >> "$S/ppsspp-$MODE.out"
rm -rf "$S/ppsspp-$MODE"
cp -R "$R/build-preset-psp-validation/ppsspp-input-script-latest" "$S/ppsspp-$MODE"
tail -4 "$S/ppsspp-$MODE.out"
