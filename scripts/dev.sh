#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
action=${1:-build}
if [ "$#" -gt 0 ]; then shift; fi

jobs=${PSP_BROWSER_JOBS:-}
if [ -z "$jobs" ]; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
fi
if [ -z "$jobs" ]; then jobs=4; fi

fast_build_dir=${PSP_BROWSER_BUILD_DIR:-"$root/build-dev"}
release_build_dir=${PSP_BROWSER_RELEASE_BUILD_DIR:-"$root/build-release"}
dependency_source_build=${PSP_BROWSER_DEP_SOURCE_BUILD:-"$fast_build_dir"}
build_dir=$fast_build_dir

quickjs_source_is_portable_baseline() {
    source_dir=$1
    [ -f "$source_dir/quickjs.c" ] || return 1
    [ -f "$source_dir/quickjs.h" ] || return 1
    c_line=$(cmake -E sha256sum "$source_dir/quickjs.c" 2>/dev/null) \
        || return 1
    h_line=$(cmake -E sha256sum "$source_dir/quickjs.h" 2>/dev/null) \
        || return 1
    c_hash=${c_line%% *}
    h_hash=${h_line%% *}
    [ "$h_hash" = 2165f47772af9faee1798999a599fa9de850d1bf0259502dade1c25d4a588316 ] \
        && { [ "$c_hash" = a68622cecb806f39bf24738c376a0a73032ea8913478cad702e0265c46f7999f ] \
             || [ "$c_hash" = 3a1c22544909d0f1f59124945de4980141b4bb27ee10f4b1d7fb1c09068ddb9b ] \
             || [ "$c_hash" = 3d09497d302cacbcc514715ac75355d32e2203fe17652663131fb854a4d76d13 ] \
             || [ "$c_hash" = 2a16ea21fb125a86c3c5449175f799056e50737a8a43ec8931a73bc5ac0c2712 ] \
             || [ "$c_hash" = e634cb7eb9b58bbbcf865ed9dfb77eb5b8f0730c8d6963acc13dd70d070d0d89 ]; }
}

find_dependency_source() {
    dependency=$1
    for candidate in \
        "$dependency_source_build" \
        "$root/build-dev" \
        "$root/build-bellard-clean-current" \
        "$root/build"
    do
        source_path=$candidate/_deps/$dependency-src
        if [ -f "$candidate/CMakeCache.txt" ]; then
            cached_source=$(awk -v dependency="$dependency" '
                BEGIN {
                    key = "FETCHCONTENT_SOURCE_DIR_" toupper(dependency) ":PATH="
                }
                index($0, key) == 1 {
                    print substr($0, length(key) + 1)
                    exit
                }
            ' "$candidate/CMakeCache.txt")
            if [ -d "$cached_source" ]; then source_path=$cached_source; fi
        fi
        if [ -d "$source_path" ]; then
            if [ "$dependency" = quickjs ] \
                && ! quickjs_source_is_portable_baseline "$source_path"; then
                printf 'Ignoring unqualified cached Bellard QuickJS source: %s\n' \
                    "$source_path" >&2
                continue
            fi
            printf '%s\n' "$source_path"
            return 0
        fi
    done
    return 0
}

configure_common() {
    set -- "$@" \
        -DPSP_BROWSER_USE_BELLARD_QUICKJS=ON \
        -DPSP_BROWSER_APPLY_BELLARD_QUICKJS_PATCH=OFF \
        -DPSP_BROWSER_QUICKJS_FUNCTION_RECYCLE=ON \
        -DPSP_BROWSER_QUICKJS_PORTABLE_REGION=ON \
        -DPSP_BROWSER_BUILD_JSC_SPIKE=OFF \
        -DPSP_BROWSER_BUILD_TESTS=ON

    lexbor_source=$(find_dependency_source lexbor)
    quickjs_source=$(find_dependency_source quickjs)
    stb_source=$(find_dependency_source stb)
    nanosvg_source=$(find_dependency_source nanosvg)
    dejavu_source=$(find_dependency_source dejavu_fonts)
    if [ -n "$lexbor_source" ]; then
        set -- "$@" "-DFETCHCONTENT_SOURCE_DIR_LEXBOR=$lexbor_source"
    fi
    if [ -n "$quickjs_source" ]; then
        set -- "$@" "-DFETCHCONTENT_SOURCE_DIR_QUICKJS=$quickjs_source"
    fi
    if [ -n "$stb_source" ]; then
        set -- "$@" "-DFETCHCONTENT_SOURCE_DIR_STB=$stb_source"
    fi
    if [ -n "$nanosvg_source" ]; then
        set -- "$@" "-DFETCHCONTENT_SOURCE_DIR_NANOSVG=$nanosvg_source"
    fi
    if [ -n "$dejavu_source" ]; then
        set -- "$@" "-DFETCHCONTENT_SOURCE_DIR_DEJAVU_FONTS=$dejavu_source"
    fi
    cmake -S "$root" -B "$build_dir" "$@"
    cmake -E touch "$build_dir/.tilefinch-dev-configured"
}

configure_fast() {
    configure_common \
        -DCMAKE_BUILD_TYPE=Debug \
        "-DCMAKE_C_FLAGS_DEBUG=-O0 -g0"
}

configure_release() {
    configure_common -DCMAKE_BUILD_TYPE=Release
}

cmake_inputs_changed() {
    stamp=$1/.tilefinch-dev-configured
    if [ ! -f "$stamp" ] \
        || [ "$root/CMakeLists.txt" -nt "$stamp" ] \
        || [ "$root/scripts/dev.sh" -nt "$stamp" ]; then
        return 0
    fi
    newer=$(find "$root/cmake" "$root/patches" -type f \
        -newer "$stamp" -print -quit)
    [ -n "$newer" ]
}

ensure_fast() {
    if cmake_inputs_changed "$build_dir"; then
        configure_fast
    fi
}

ensure_release() {
    build_dir=$release_build_dir
    if cmake_inputs_changed "$build_dir"; then
        configure_release
    fi
}

build_targets() {
    cmake --build "$build_dir" --target "$@" -j "$jobs"
}

run_tests() {
    build_targets tilefinch-test-binaries
    ctest --test-dir "$build_dir" --output-on-failure
}

run_unit_suite() {
    suite=${1:-core}
    if [ "$suite" = "layout" ]; then
        build_targets tilefinch-layout-tests
        "$build_dir/tilefinch-layout-tests"
        return
    fi
    build_targets tilefinch-tests
    "$build_dir/tilefinch-tests" --filter "$suite"
    if [ "$suite" = "core" ]; then
        build_targets tilefinch-layout-tests
        "$build_dir/tilefinch-layout-tests"
    fi
}

case "$action" in
    build|interactive)
        ensure_fast
        build_targets psp-browser-interactive-lab
        ;;
    static)
        ensure_fast
        build_targets psp-browser-lab
        ;;
    run)
        ensure_fast
        build_targets psp-browser-interactive-lab
        exec "$build_dir/psp-browser-interactive-lab" "$@"
        ;;
    run-static)
        ensure_fast
        build_targets psp-browser-lab
        exec "$build_dir/psp-browser-lab" "$@"
        ;;
    test)
        ensure_fast
        run_tests
        ;;
    unit)
        if [ "$#" -gt 1 ]; then
            printf '%s\n' "usage: scripts/dev.sh unit [core|foundation|web-runtime|layout|sections]" >&2
            exit 2
        fi
        ensure_fast
        run_unit_suite "${1:-core}"
        ;;
    psp)
        # Cross-compile ratchet: keeps host-only assumptions out of the
        # engine (docs/engineering/DEVICE_QUALIFICATION.md). Requires PSPDEV to point at a
        # pspdev SDK.
        if [ -z "${PSPDEV:-}" ]; then
            printf '%s
' "psp: export PSPDEV=/path/to/pspdev first" >&2
            exit 2
        fi
        cmake --preset psp
        cmake --build "$root/build-preset-psp" --target \
            tilefinch_core psp-browser-fixture psp-browser-script -j 8
        printf '%s\n' \
            "psp: fixture and full interactive EBOOTs built for Allegrex"
        ;;
    release)
        ensure_release
        build_targets psp-browser-interactive-lab
        ;;
    release-static)
        ensure_release
        build_targets psp-browser-lab
        ;;
    run-release)
        ensure_release
        build_targets psp-browser-interactive-lab
        exec "$build_dir/psp-browser-interactive-lab" "$@"
        ;;
    test-release)
        ensure_release
        run_tests
        ;;
    verify)
        ensure_release
        run_tests
        verify_base=${TMPDIR:-/tmp}
        verify_root=${verify_base%/}/psp-browser-verify-$$
        "$root/benchmarks/run-async-network.sh" "$build_dir" \
            "$verify_root/async"
        "$root/benchmarks/run-http-replay.sh" "$build_dir" \
            "$verify_root/http-replay"
        "$root/benchmarks/run-client-hint-replay.sh" "$build_dir" \
            "$verify_root/client-hints"
        ;;
    sanitize)
        build_dir=${PSP_BROWSER_SANITIZE_BUILD_DIR:-"$root/build-dev-sanitize"}
        if cmake_inputs_changed "$build_dir"; then
            configure_common \
                -DCMAKE_BUILD_TYPE=Debug \
                "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer" \
                "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
        fi
        build_targets tilefinch-test-binaries
        "$root/scripts/run-sanitizer-tests.sh" "$build_dir"
        ;;
    configure)
        configure_fast
        ;;
    configure-release)
        build_dir=$release_build_dir
        configure_release
        ;;
    *)
        printf '%s\n' \
            "usage: scripts/dev.sh [build|static|run|run-static|unit|test|psp|release|release-static|run-release|test-release|verify|sanitize|configure|configure-release] [arguments...]" >&2
        exit 2
        ;;
esac
