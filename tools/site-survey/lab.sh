#!/bin/sh
# Lab load of one page at 480x272 with shipping-default policy (basic content
# blocking, cookie notices hidden), realistic PSP profile, 8 MiB document cap.
# usage: lab.sh MODE NAME URL      MODE = capture | replay | pspreplay
#   capture:   live network, writes scratch/survey/NAME/capture (one fetch/page)
#   replay:    response-keyed replay of that capture, writes NAME/replay-*
#   pspreplay: response-keyed replay of the PSP's own capture
#              (NAME/psp-capture from ppsspp.sh), writes NAME/pspreplay-*
set -eu
MODE=$1 NAME=$2 URL=$3
R=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
S=${SURVEY_DIR:-$R/scratch/survey}/$NAME
mkdir -p "$S"
F=$S/frames-$MODE
rm -rf "$F"; mkdir -p "$F"
C=$S/$MODE.commands
cat > "$C" <<CMDS
status
tick 150 33
status
render $F/top.ppm
js document.title + ' | ' + location.href + ' | iw=' + innerWidth + ' ih=' + innerHeight + ' sh=' + document.documentElement.scrollHeight + ' sw=' + document.documentElement.scrollWidth
js (function(){var m=document.querySelector('meta[name=viewport]');return 'viewport=' + (m?m.content:'none') + ' ua=' + navigator.userAgent + ' dpr=' + devicePixelRatio + ' mq-mobile=' + matchMedia('(max-width: 600px)').matches + ' pointer-coarse=' + matchMedia('(pointer: coarse)').matches + ' hover=' + matchMedia('(hover: hover)').matches;})()
js 'text-len=' + document.body.innerText.length + ' imgs=' + document.images.length + ' loaded=' + [...document.images].filter(function(i){return i.complete && i.naturalWidth>0}).length + ' scripts=' + document.scripts.length + ' links=' + document.links.length + ' custom-elements=' + document.querySelectorAll(':not(:defined)').length
js document.body.innerText.slice(0,1200)
link-urls
page-down
render $F/p1.ppm
page-down
render $F/p2.ppm
page-down
render $F/p3.ppm
page-down
render $F/p4.ppm
script-report
status
quit
CMDS
case $MODE in
capture) rm -rf "$S/capture"; NET="--capture-http $S/capture" ;;
replay)  NET="--replay-http-response-keyed $S/capture --deterministic-replay-seed 1" ;;
pspreplay) NET="--replay-http-response-keyed $S/psp-capture --deterministic-replay-seed 1" ;;
*) echo "bad mode"; exit 2 ;;
esac
start=$(date +%s)
"$R/build-preset-release/psp-browser-interactive-lab" --url "$URL" \
  --fetch-scripts --psp-profile realistic --max-download-kb 8192 \
  --content-blocker basic --hide-cookie-banners $NET \
  --commands "$C" --no-loop-capture --output "$F/final.ppm" \
  > "$S/$MODE.log" 2>&1 || echo "lab exit=$?" >> "$S/$MODE.log"
echo "wall-s=$(( $(date +%s) - start ))" >> "$S/$MODE.log"
tail -3 "$S/$MODE.log"
