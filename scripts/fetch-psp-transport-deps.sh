#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lock="$root/third_party/psp_transport/dependencies.lock"
cache=${TILEFINCH_PSP_TRANSPORT_CACHE:-"$root/third_party/cache"}

mkdir -p "$cache"

hash_file()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

while IFS='|' read -r name version archive expected url; do
    case "$name" in
        ''|'#'*) continue ;;
    esac
    destination="$cache/$archive"
    if [ -f "$destination" ]; then
        actual=$(hash_file "$destination")
        if [ "$actual" = "$expected" ]; then
            printf '%s %s: cached and verified\n' "$name" "$version"
            continue
        fi
        printf '%s: cached archive has the wrong SHA-256; refusing to overwrite it\n' \
            "$destination" >&2
        exit 1
    fi

    temporary="$destination.part.$$"
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    printf '%s %s: downloading official source\n' "$name" "$version"
    curl --fail --location --proto '=https' --tlsv1.2 \
        --output "$temporary" "$url"
    actual=$(hash_file "$temporary")
    if [ "$actual" != "$expected" ]; then
        printf '%s: SHA-256 mismatch (expected %s, got %s)\n' \
            "$archive" "$expected" "$actual" >&2
        exit 1
    fi
    mv "$temporary" "$destination"
    trap - EXIT HUP INT TERM
done < "$lock"

printf 'PSP transport source cache is complete: %s\n' "$cache"
