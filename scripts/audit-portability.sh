#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$root/build-release"}
flags_file="$build_dir/CMakeFiles/tilefinch_core.dir/flags.make"
sources_file="$build_dir/tilefinch-portability-sources.txt"
output_dir=${2:-"${TMPDIR:-/tmp}/tilefinch-portability-audit"}

if [ ! -f "$flags_file" ] || [ ! -f "$sources_file" ]; then
    printf 'Configure and build tilefinch_core first; expected %s and %s\n' \
        "$flags_file" "$sources_file" >&2
    exit 2
fi
mkdir -p "$output_dir/objects"
compiler=$(sed -n 's/^# compile C with //p' "$flags_file" | head -n 1)
if [ -z "$compiler" ]; then compiler=${CC:-cc}; fi
compiler_version=$("$compiler" --version 2>/dev/null || true)
narrowing_flag=
case "$compiler_version" in
    *clang*|*Clang*) narrowing_flag=-Wshorten-64-to-32 ;;
esac
defines=$(sed -n 's/^C_DEFINES = //p' "$flags_file")
includes=$(sed -n 's/^C_INCLUDES = //p' "$flags_file")

sources=$(cat "$sources_file")
if [ -z "$sources" ]; then
    printf 'Portability source manifest is empty: %s\n' "$sources_file" >&2
    exit 2
fi
for required in src/section_pager.c src/section_router.c src/section_store.c \
                src/main.c src/interactive_main.c \
                src/failure_recovery_main.c src/psp_ui.c; do
    if ! grep -Fx "$required" "$sources_file" >/dev/null 2>&1; then
        printf 'Portability source manifest is missing %s\n' "$required" >&2
        exit 2
    fi
done
strict_log="$output_dir/stack-16k.log"
detail_log="$output_dir/stack-8k.log"
: > "$strict_log"
: > "$detail_log"

compile_audit() {
    threshold=$1
    extra_define=$2
    destination=$3
    for source_entry in $sources; do
        case "$source_entry" in
            /*) source=$source_entry ;;
            *) source="$root/$source_entry" ;;
        esac
        if [ ! -f "$source" ]; then
            printf 'Portability source does not exist: %s\n' "$source" \
                >> "$destination"
            return 1
        fi
        unit=$(basename "$source" .c)
        object="$output_dir/objects/$unit-$threshold.o"
        # CMake's generated flag strings are intentionally word-split here.
        # Repository/build paths are required not to contain shell whitespace.
        "$compiler" $defines $includes $extra_define -std=c11 -O2 \
            -Wframe-larger-than="$threshold" $narrowing_flag \
            -Wpointer-to-int-cast -Werror=implicit-function-declaration \
            -c "$source" -o "$object" >> "$destination" 2>&1 || return 1
    done
}

# The PSP profile omits stb_image's 35 KiB GIF decoder frame. The desktop
# feature remains enabled by default and is measured separately below.
compile_audit 16384 -DTILEFINCH_DISABLE_GIF=1 "$strict_log"

if grep 'warning:' "$strict_log" >/dev/null 2>&1; then
    printf 'Portability audit found a 16 KiB stack, narrowing, or cast warning:\n' >&2
    grep 'warning:' "$strict_log" >&2
    exit 1
fi

compile_audit 8192 -DTILEFINCH_DISABLE_GIF=1 "$detail_log"
detail_count=$(grep -c 'warning: stack frame size' "$detail_log" || true)
if grep 'warning:' "$detail_log" | grep -v 'warning: stack frame size' \
    >/dev/null 2>&1; then
    printf 'Portability audit found a narrowing or cast warning:\n' >&2
    grep 'warning:' "$detail_log" | grep -v 'warning: stack frame size' >&2
    exit 1
fi
if grep 'section_store.c:.*warning: stack frame size' "$detail_log" \
    >/dev/null 2>&1; then
    printf 'Section indexing regressed above the 8 KiB scratch-frame gate:\n' >&2
    grep 'section_store.c:.*warning: stack frame size' "$detail_log" >&2
    exit 1
fi

gif_log="$output_dir/desktop-gif-stack.log"
: > "$gif_log"
# The configured build may already disable GIF (the checked-in Release preset
# does).  Undo that target policy for this one desktop-only measurement so the
# known stb exception is still audited instead of silently disappearing.
"$compiler" $defines $includes -UTILEFINCH_DISABLE_GIF \
    -std=c11 -O2 -Wframe-larger-than=16384 \
    -c "$root/src/image.c" -o "$output_dir/objects/image-desktop.o" \
    >> "$gif_log" 2>&1
gif_warnings=$(grep -c "stb_image.h:.*warning: stack frame size" \
    "$gif_log" || true)
unexpected_gif=$(grep 'warning:' "$gif_log" \
    | grep -v "stbi__load_gif_main" | grep -v "stbi__gif_load" \
    | grep -v "stbi_load_gif_from_memory" | grep -v "stbi__load_main" || true)
if [ -n "$unexpected_gif" ] || [ "$gif_warnings" -ne 2 ]; then
    printf 'Desktop decoder stack audit changed unexpectedly:\n' >&2
    grep 'warning:' "$gif_log" >&2 || true
    exit 1
fi

source_count=$(wc -l < "$sources_file" | tr -d ' ')
printf 'Portability audit passed: %s PSP-linked translation units are clean at the 16 KiB gate with PSP GIF policy; %s frames exceed 8 KiB (informational).\n' \
    "$source_count" "$detail_count"
printf 'Desktop GIF exception measured: 2 stb_image frames exceed 16 KiB; configure PSP_BROWSER_ENABLE_GIF=OFF for the PSP target.\n'
printf 'Logs: %s\n' "$output_dir"
