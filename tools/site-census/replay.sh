#!/bin/sh
# Replay the site-census corpus (or a subset by page name or tag) offline,
# in parallel, and write OUT/census.jsonl. See tools/site-census/README.md.
#
# usage: replay.sh OUT [census.py replay options] [SELECTOR...]
#   e.g. replay.sh ../census/runs/main
#        replay.sh ../census/runs/branch -j 8 news reference
#        LAB=/path/to/other/psp-browser-interactive-lab replay.sh OUT
#        CORPUS=../census/corpus-v2 SITES=pages.tsv replay.sh OUT
#   (CORPUS: another corpus generation; SITES: a page list other than
#   sites.tsv, for pages the default list does not name)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=$1; shift
set -- --out "$OUT" "$@"
[ -n "${LAB:-}" ] && set -- --lab "$LAB" replay "$@" || set -- replay "$@"
[ -n "${CORPUS:-}" ] && set -- --corpus "$CORPUS" "$@"
[ -n "${SITES:-}" ] && set -- --sites "$SITES" "$@"
exec python3 "$HERE/census.py" "$@"
