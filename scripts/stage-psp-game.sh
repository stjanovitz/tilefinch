#!/bin/sh
set -eu

# Stage a web game for PSP validation under an immutable, content-addressed
# URL and point an existing validation boot.cfg at that exact tree. A new
# authored byte always produces a new document *and* subresource URL, so the
# browser cache cannot quietly turn a device run into a test of yesterday's
# game.js.

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir=
stage_root=
base_url=
boot_config=
stage_name=
query=

usage() {
    cat >&2 <<'EOF'
usage: stage-psp-game.sh --source DIR --stage-root DIR --base-url URL \
  --boot-config FILE [--name NAME] [--query QUERY]
       stage-psp-game.sh --refresh-managed BOOT_CONFIG
EOF
    exit 2
}

managed_value() {
    key=$1
    file=$2
    sed -n "s/^$key=//p" "$file" | sed -n '1p'
}

if [ "${1:-}" = "--refresh-managed" ]; then
    [ "$#" -eq 2 ] || usage
    refresh_boot=$2
    case "$refresh_boot" in /*) ;; *) refresh_boot=$root/$refresh_boot ;; esac
    managed=$refresh_boot.tilefinch-game-stage
    [ -f "$managed" ] || exit 0
    [ -f "$refresh_boot" ] || {
        echo "managed game stage lost its boot config: $refresh_boot" >&2
        exit 1
    }
    managed_url=$(managed_value url "$managed")
    current_url=$(sed -n 's/^url=//p' "$refresh_boot" | sed -n '1p')
    # A developer who points boot.cfg elsewhere has deliberately left this
    # managed game. Preserve the new task instead of resurrecting the old URL.
    [ -n "$managed_url" ] && [ "$current_url" = "$managed_url" ] || exit 0
    refresh_source=$(managed_value source "$managed")
    refresh_stage_root=$(managed_value stage-root "$managed")
    refresh_base_url=$(managed_value base-url "$managed")
    refresh_name=$(managed_value name "$managed")
    refresh_query=$(managed_value query "$managed")
    [ -n "$refresh_source" ] && [ -n "$refresh_stage_root" ] \
        && [ -n "$refresh_base_url" ] && [ -n "$refresh_name" ] || {
        echo "invalid managed game stage: $managed" >&2
        exit 1
    }
    exec "$0" --source "$refresh_source" --stage-root "$refresh_stage_root" \
        --base-url "$refresh_base_url" --boot-config "$refresh_boot" \
        --name "$refresh_name" --query "$refresh_query"
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --source) source_dir=$2; shift 2 ;;
        --source=*) source_dir=${1#--source=}; shift ;;
        --stage-root) stage_root=$2; shift 2 ;;
        --stage-root=*) stage_root=${1#--stage-root=}; shift ;;
        --base-url) base_url=$2; shift 2 ;;
        --base-url=*) base_url=${1#--base-url=}; shift ;;
        --boot-config) boot_config=$2; shift 2 ;;
        --boot-config=*) boot_config=${1#--boot-config=}; shift ;;
        --name) stage_name=$2; shift 2 ;;
        --name=*) stage_name=${1#--name=}; shift ;;
        --query) query=$2; shift 2 ;;
        --query=*) query=${1#--query=}; shift ;;
        -h|--help) usage ;;
        *) usage ;;
    esac
done

[ -n "$source_dir" ] && [ -n "$stage_root" ] \
    && [ -n "$base_url" ] && [ -n "$boot_config" ] || usage
case "$source_dir" in /*) ;; *) source_dir=$root/$source_dir ;; esac
case "$stage_root" in /*) ;; *) stage_root=$root/$stage_root ;; esac
case "$boot_config" in /*) ;; *) boot_config=$root/$boot_config ;; esac
[ -d "$source_dir" ] && [ -f "$source_dir/index.html" ] || {
    echo "game source must contain index.html: $source_dir" >&2
    exit 2
}
[ -f "$boot_config" ] || {
    echo "missing validation boot config: $boot_config" >&2
    exit 2
}
[ -n "$stage_name" ] || stage_name=$(basename "$source_dir")
case "$stage_name" in *[!A-Za-z0-9._-]*|'')
    echo "invalid stage name: $stage_name" >&2; exit 2 ;;
esac
base_url=${base_url%/}
case "$base_url" in http://*|https://*) ;; *)
    echo "base URL must use HTTP or HTTPS" >&2; exit 2 ;;
esac

if command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$@"; }
elif command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$@"; }
else
    echo "staging requires shasum or sha256sum" >&2
    exit 2
fi

tree_digest() {
    directory=$1
    python3 - "$directory" <<'PY'
import hashlib
import os
import stat
import struct
import sys

root = sys.argv[1]
nofollow = getattr(os, "O_NOFOLLOW", 0)
directory_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | nofollow
file_flags = os.O_RDONLY | nofollow
tree = hashlib.sha256()


def framed(kind, relative):
    encoded = os.fsencode(relative)
    tree.update(kind)
    tree.update(struct.pack(">I", len(encoded)))
    tree.update(encoded)


def walk(directory_fd, prefix):
    names = sorted(os.listdir(directory_fd), key=os.fsencode)
    for name in names:
        relative = name if not prefix else prefix + "/" + name
        before = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if not prefix and name == ".tilefinch-stage-digest":
            if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
                raise RuntimeError("unsafe root stage marker")
            # Only the generated root marker is metadata. A nested entry with
            # this basename is authored content and must pass normal checks.
            continue
        if stat.S_ISDIR(before.st_mode):
            child_fd = os.open(name, directory_flags, dir_fd=directory_fd)
            try:
                opened = os.fstat(child_fd)
                if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
                    raise RuntimeError("directory changed during staging")
                framed(b"D", relative)
                walk(child_fd, relative)
            finally:
                os.close(child_fd)
            continue
        if not stat.S_ISREG(before.st_mode):
            raise RuntimeError("non-regular entry: " + relative)
        if before.st_nlink != 1:
            raise RuntimeError("hardlinked entry: " + relative)
        file_fd = os.open(name, file_flags, dir_fd=directory_fd)
        try:
            opened = os.fstat(file_fd)
            if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
                raise RuntimeError("file changed during staging")
            if not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1:
                raise RuntimeError("non-private regular entry: " + relative)
            framed(b"F", relative)
            tree.update(struct.pack(">Q", opened.st_size))
            while True:
                chunk = os.read(file_fd, 65536)
                if not chunk:
                    break
                tree.update(chunk)
            after = os.fstat(file_fd)
            stable = (opened.st_dev, opened.st_ino, opened.st_size,
                      opened.st_mtime_ns, opened.st_ctime_ns)
            final = (after.st_dev, after.st_ino, after.st_size,
                     after.st_mtime_ns, after.st_ctime_ns)
            if stable != final:
                raise RuntimeError("file changed while hashing: " + relative)
        finally:
            os.close(file_fd)


try:
    root_fd = os.open(root, directory_flags)
    try:
        walk(root_fd, "")
    finally:
        os.close(root_fd)
except (OSError, RuntimeError) as error:
    raise SystemExit("game stage tree is unsafe: " + str(error))

print(tree.hexdigest())
PY
}

validate_stage_tree() {
    directory=$1
    tree_digest "$directory" >/dev/null
}

validate_stage_tree "$source_dir"
digest=$(tree_digest "$source_dir")
short_digest=$(printf '%s' "$digest" | cut -c1-16)
mkdir -p "$stage_root"
target=$stage_root/$stage_name-$short_digest
marker=$target/.tilefinch-stage-digest
if [ -L "$target" ]; then
    echo "refusing symlinked immutable game stage: $target" >&2
    exit 1
elif [ -d "$target" ]; then
    validate_stage_tree "$target"
    [ -f "$marker" ] && [ "$(cat "$marker")" = "$digest" ] \
        && [ "$(tree_digest "$target")" = "$digest" ] || {
        echo "refusing corrupt immutable game stage: $target" >&2
        exit 1
    }
else
    temporary=$(mktemp -d "$stage_root/.tilefinch-stage.XXXXXX")
    cleanup() { rm -rf "$temporary"; }
    trap cleanup EXIT HUP INT TERM
    rsync -a "$source_dir/" "$temporary/"
    validate_stage_tree "$temporary"
    [ "$(tree_digest "$temporary")" = "$digest" ] || {
        echo "staged game digest changed during copy" >&2
        exit 1
    }
    # A source tree may contain a prior private root marker. Remove that
    # ordinary temporary file before publishing the freshly computed value;
    # validation above guarantees this cannot follow a symlink or hardlink.
    rm -f "$temporary/.tilefinch-stage-digest"
    printf '%s\n' "$digest" >"$temporary/.tilefinch-stage-digest"
    mv "$temporary" "$target"
    trap - EXIT HUP INT TERM
fi

stage_url=$base_url/$(basename "$target")/index.html
if [ -n "$query" ]; then
    stage_url=$stage_url?$query\&stage=$short_digest
else
    stage_url=$stage_url?stage=$short_digest
fi
temporary_config=$boot_config.tmp.$$
# The Game Profile permits one bounded 384 KiB authored script. Make direct
# device staging exercise that same contract instead of the generic 256 KiB
# web-page default; otherwise a valid installed game can degrade to its static
# shell only in the PSP validation route.
awk -v staged_url="$stage_url" '
    BEGIN { replaced = 0; file_replaced = 0 }
    /^url=/ && !replaced { print "url=" staged_url; replaced = 1; next }
    /^file_kb=/ && !file_replaced {
        print "file_kb=384"; file_replaced = 1; next
    }
    { print }
    END {
        if (!replaced) print "url=" staged_url
        if (!file_replaced) print "file_kb=384"
    }
' "$boot_config" >"$temporary_config"
mv "$temporary_config" "$boot_config"

managed=$boot_config.tilefinch-game-stage
temporary_managed=$managed.tmp.$$
{
    printf 'source=%s\n' "$source_dir"
    printf 'stage-root=%s\n' "$stage_root"
    printf 'base-url=%s\n' "$base_url"
    printf 'name=%s\n' "$stage_name"
    printf 'query=%s\n' "$query"
    printf 'url=%s\n' "$stage_url"
} >"$temporary_managed"
mv "$temporary_managed" "$managed"

printf 'stage-digest=%s\nstage-url=%s\n' "$digest" "$stage_url"
