#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
wpt_root=${1:-"$root/../wpt"}
revision=$(sed -n '1p' "$root/benchmarks/wpt/REVISION")

if [ ! -d "$wpt_root/.git" ]; then
    git clone --filter=blob:none --no-checkout \
        https://github.com/web-platform-tests/wpt.git "$wpt_root"
elif [ -n "$(git -C "$wpt_root" status --porcelain)" ]; then
    printf 'error: refusing to modify dirty WPT checkout: %s\n' \
        "$wpt_root" >&2
    exit 2
fi

git -C "$wpt_root" fetch --depth 1 origin "$revision"
git -C "$wpt_root" checkout --detach "$revision"
git -C "$wpt_root" sparse-checkout init --no-cone
sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' \
    -e 's#^#/#' \
    "$root/benchmarks/wpt/paths.txt" \
    "$root/benchmarks/wpt/expanded-paths.txt" \
    "$root/benchmarks/wpt/dom-interaction-paths.txt" \
    "$root/benchmarks/wpt/text-flow-paths.txt" \
    "$root/benchmarks/wpt/secondary-sites-paths.txt" \
    "$root/benchmarks/wpt/component-reactivity-paths.txt" \
    "$root/benchmarks/wpt/modern-component-apis-paths.txt" \
    "$root/benchmarks/wpt/scroll-interaction-paths.txt" \
    "$root/benchmarks/wpt/modern-mobile-css-paths.txt" |
    git -C "$wpt_root" sparse-checkout set --no-cone --stdin
git -C "$wpt_root" checkout

printf 'upstream-wpt root=%s revision=%s status=READY\n' \
    "$wpt_root" "$revision"
