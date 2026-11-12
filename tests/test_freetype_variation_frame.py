#!/usr/bin/env python3
"""Execute the vendored parser's frame-admission block with modeled 32-bit types.

This is not a full 32-bit ABI test. In particular, host unsigned long can be
64-bit; the frame sink and FT_ULONG_MAX deliberately retain their PSP widths.
UBSan makes the original signed multiply fail even on an LP64 host. Normal
font decoding and the real PSP build remain separate integration gates.
"""
import argparse
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    source = args.source.read_text()
    start = source.index("    itemStore->regionCount = region_count;",
                         source.index("tt_var_load_item_variation_store("))
    end = source.index("    bytes = stream->cursor;", start)
    # Compile the actual admission block, not a duplicate of its arithmetic.
    block = source[start:end]
    program = r'''
#include <stdint.h>
#include <stdio.h>
typedef int32_t FT_Long;
typedef uint32_t FT_ULong;
typedef uint32_t FT_UInt;
typedef uint16_t FT_UShort;
#define FT_ULONG_MAX UINT32_MAX
#define FT_TRACE2(args) ((void)0)
#define FT_THROW(name) 1
static uint32_t requested, available;
static unsigned calls;
static int enter_frame(uint32_t size) {
    requested = size; calls++; return size > available;
}
#define FT_FRAME_ENTER(size) (error = enter_frame(size))
static int admit(FT_UInt region_count, FT_UShort axis_count) {
    struct { FT_UInt regionCount; } store, *itemStore = &store;
    int error = 0;
''' + block + r'''
Exit:
    return error;
}
static int check(unsigned regions, unsigned axes, uint32_t bytes,
                 int want_error, unsigned want_calls, uint32_t want_size) {
    available = bytes; requested = 0; calls = 0;
    int error = admit(regions, axes);
    if (!!error != want_error || calls != want_calls || requested != want_size) {
        fprintf(stderr, "regions=%u axes=%u error=%d calls=%u size=%u\n",
                regions, axes, error, calls, requested);
        return 1;
    }
    return 0;
}
int main(void) {
    /* Valid inputs, empty stores, exact-end and truncated streams. */
    if (check(2, 4, 48, 0, 1, 48) || check(2, 4, 47, 1, 1, 48) ||
        check(0, 4, 0, 0, 1, 0) || check(7, 0, 0, 0, 1, 0)) return 1;
    /* Both counts satisfy the real fvar/region limits. Old code has signed
       32-bit overflow here; the corrected stream request must stay exact. */
    if (check(32767, 16382, 0, 1, 1, UINT32_C(3220733964))) return 1;
    /* White-box overflow control: the real fvar admission rejects this axis
       count earlier, but the shared frame boundary must still fail closed. */
    if (check(32767, 65535, UINT32_MAX, 1, 0, 0)) return 1;
    puts("FreeType modeled 32-bit variation frame admission: PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="tilefinch-ft-frame-") as directory:
        path = pathlib.Path(directory)
        (path / "test.c").write_text(program)
        subprocess.run([args.compiler, "-std=c99", "-O1",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
