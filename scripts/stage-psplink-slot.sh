#!/bin/sh
set -eu

# Host-side preflight only; no PSPLink calls or device writes. The output is
# an ordered fixed-artifact plan for psplink-device.sh (EBOOT always last).
[ "$#" -eq 2 ] || { echo "usage: $0 BUILD_DIR HOST_ROOT" >&2; exit 2; }
build=$1
host=$2
cache=$build/CMakeCache.txt
[ -s "$cache" ] || { echo "Missing build configuration: $cache" >&2; exit 1; }
# PSP builds always supply the lazy wasm component; the similarly named
# HOST_WEBASSEMBLY option controls host labs, not the PSP dependency.
components=wasm
if grep -q '^PSP_BROWSER_ENABLE_PSP_VOICE:BOOL=ON$' "$cache"; then
    components="$components voice"
fi

# Validate the complete enabled set before even replacing host0 staging.
for component in $components eboot; do
    case "$component" in
        eboot) file=$build/EBOOT.PBP ;;
        *) file=$build/tilefinch-$component.prx ;;
    esac
    [ -s "$file" ] || { echo "Required slot artifact missing: $file" >&2; exit 1; }
    size=$(wc -c < "$file" | tr -d ' ')
    [ "$size" -le 8388608 ] || { echo "Slot artifact exceeds 8 MiB: $file" >&2; exit 1; }
    magic=$(od -An -tx1 -N4 "$file" | tr -d ' \n')
    case "$component:$magic" in
        eboot:00504250|wasm:7f454c46|voice:7f454c46) ;;
        *) echo "Invalid slot artifact header: $file" >&2; exit 1 ;;
    esac
done
mkdir -p "$host"
for component in $components eboot; do
    case "$component" in
        eboot) cp "$build/EBOOT.PBP" "$host/EBOOT-device-latest.PBP" ;;
        *) cp "$build/tilefinch-$component.prx" "$host/tilefinch-$component-device-latest.prx" ;;
    esac
done
printf '%s\n' $components eboot
