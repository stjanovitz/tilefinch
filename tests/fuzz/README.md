# libFuzzer targets

Coverage-guided fuzzers for the parsers that see untrusted network input.
They complement `tests/test_hostile_parsers.c`, a deterministic
random-mutation harness. It is not part of the default test suite: it is
built and run only under the opt-in `hostile` preset
(`cmake --preset hostile`, then `ctest --preset hostile`, or `ctest -L fuzz`
in that tree).

| Target | Harness | What it covers |
| --- | --- | --- |
| url | `fuzz_url.c` | `src/url.c` parse/normalize/resolve/origin/site key, public suffix, `src/data_url.c` |
| css | `fuzz_css.c` | stylesheet, inline style, selector, `@media`/`@supports`, color parsing; cascade over a fixed document; layout |
| html | `fuzz_html.c` | streamed `document_parse` glue, DOM helpers, stylesheet from `<style>`/`style=`, layout, `document_refresh`; byte 0 bit 7 parses with scripting enabled and applies the static `<noscript>` shell fallback (innerHTML replacement) |
| http | `fuzz_http.c` | response-header consumers: security metadata, CSP and frame embedding, Set-Cookie/cookie dates, HSTS, Content-Range, client hints, SRI, request-header validation, WebSocket close/text payloads |
| image | `fuzz_image.c` | `image_decode_probe_info` + `image_resource_decode_checked` (stb PNG/JPEG/GIF/BMP, scaled JPEG, first-frame GIF, libwebp) and `image_svg_decode` |
| mp4 | `fuzz_mp4.c` | MP4 demuxer (progressive and fragmented/sidx), sample reads and seeks, avcC/esds/Annex-B helpers |
| hls | `fuzz_hls.c` | HLS playlist parsing (one-shot and incremental, including live-window compaction), variant/rendition selection |
| hls_source | `fuzz_hls_source.c` | HLS segment pipeline: TS/ADTS segments through the TS demuxer, queue and sample reads |
| ts | `fuzz_ts.c` | `src/swdec/swdec_ts.c` MPEG-TS demuxer |
| font | `fuzz_font.c` | page web fonts (sfnt/WOFF1) through `font_face_load_encoded` and the FreeType metric/raster paths |
| bidi | `fuzz_bidi.c` | `src/text_bidi.c` paragraph analysis and line reordering |
| misc | `fuzz_misc.c` | web app manifest JSON, WebVTT captions, multiplayer packets, YouTube player-response JSON |

Every harness uses a fresh `Budget` per input and aborts if it is not
balanced afterwards, so budget leaks are reported as crashes.

## Requirements

clang with libFuzzer (`-fsanitize=fuzzer`). Apple's Xcode clang does not
ship libFuzzer; on macOS use Homebrew LLVM (`brew install llvm`). On Linux,
any recent upstream clang works. The usual host dependencies (zlib, OpenSSL
crypto, libcurl >= 7.85) are needed; FetchContent downloads the rest.

## Build

    tests/fuzz/build.sh            # configures and builds build-fuzz/

`build.sh` configures a separate tree with the whole engine compiled with
`-fsanitize=fuzzer-no-link,address,undefined`, so `libtilefinch_core` and
its vendored dependencies carry coverage instrumentation, and then builds
the `tilefinch-fuzzers` target. Override the compiler with `CC`/`CXX`.

## Run

    tests/fuzz/run.sh css 3600 4 -max_len=8192

runs `fuzz_css` for an hour with four fork-mode workers, continuing past
crashes. The corpus and crash artifacts live in
`build-fuzz/fuzz-work/<target>/{corpus,artifacts}`. Seeds (and the CSS/HTML
dictionaries, harvested from string literals in the sources) are generated
from inline samples and repository fixtures by `make_seeds.py`; they are not
checked in.

Allocation limits: `run.sh` passes `-malloc_limit_mb=64` and
`-rss_limit_mb=2048`. The PSP has about 24 MB for everything, so a single
allocation above 64 MB that scales with input is a finding.

Suggested `-max_len`: url 4096, css 8192, html 16384, http 4096,
image 32768, mp4 16384, hls 8192, hls_source 16384, ts 8192,
font 131072, bidi 4096, misc 8192.

## Reproducing one input

Each target also runs a single saved input without fuzzing, which is how
fixed findings are checked before and after a change:

    build-fuzz/tests/fuzz/fuzz_css path/to/input

Saved reproducers and their regression tests belong in the ignored private
investigation area, never in public fixtures or inline public test cases.
The optional private CTest lane is described in
[Development](../../docs/DEVELOPMENT.md#optional-private-tests); public clones
build and test without it. General synthetic behavior tests and the fuzzing
harnesses themselves remain public.

## Triage

    tests/fuzz/triage.sh css         # re-run every artifact, print top frames
    build-fuzz/tests/fuzz/fuzz_css -minimize_crash=1 -max_total_time=120 \
        -exact_artifact_path=min.css build-fuzz/fuzz-work/css/artifacts/crash-...

`fuzz_mp4` prints why `media_mp4_open` rejected an input when
`TILEFINCH_FUZZ_DEBUG` is set, which helps when checking seed quality.
