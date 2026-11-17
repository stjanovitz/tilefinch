# Diagnostic QR reports

This page is for PSP owners sending a diagnostic report and for maintainers
decoding one. Tilefinch can display its existing diagnostic logs as one or
more QR codes. This is a transport view only: building a report does not
alter, delete, or replace any log file, and it creates no new file on the
Memory Stick. It can also include the latest opt-in Wi-Fi failure report
directly from RAM.

## User flow

Open **Menu → Help & diagnostics → Diagnostic QR**, press Cross to build the
report, then photograph every page. Use D-pad Left/Right or L/R to move
between pages, and D-pad Up/Down to move between report parts. Photograph
every page of every part. The report ID, part count, and page count stay
visible so photographs from different reports are easy to separate. Press
Circle when done; the current compressed part and QR working memory are
released immediately.

The release build includes `data/tilefinch-last-error.txt` when it exists.
Ordinary failures create or replace that file only when **Menu → Settings →
Device & storage → Save error reports** is On; the default is Off. A fatal
startup failure still writes one report because this screen may not become
reachable. A validation build additionally flushes its buffered validation log
once, in response to the explicit Build action, and can include the current
and prior validation/crash logs. Missing files are skipped. The last-error log
comes first, followed by current and previous validation/crash logs. A file
larger than one part is divided into exact contiguous segments, so no log
bytes are dropped. Each part contains at most 56 KiB of input and 64 QR pages.
Moving to another part frees the current compressed payload before reading and
building the requested part, so memory use stays the same whatever the total
log size.

With **Wi-Fi diagnostics** enabled, a failed connection keeps a report of at
most 4 KiB in RAM. Diagnostic QR puts that report first and owns a snapshot
of its bytes so another connection cannot change an open report. No file is
created for it, even when Save error reports is Off. It contains numeric
connection state, error codes and configuration flags—not network names,
credentials or addresses. Turning diagnostics Off clears the retained report.

## QR geometry

Every page is QR Version 27 with medium error correction. Its 125-by-125
matrix is painted directly into RGB565 at exactly 2-by-2 display pixels per
module. A four-module white quiet zone on each side produces a 266-by-266
pixel square on the 480-by-272 display. The renderer draws integer rectangles
directly; textures, filtering, and fractional scaling are not involved.

## Page envelope

The QR text uses the QR alphanumeric alphabet and has this form:

```text
TFD2:<report-id>:<part>:<parts>:<page>:<pages>:<compressed-crc>:<chunk-crc>:<base45>
```

- CRC fields are eight uppercase hexadecimal digits and use zlib CRC-32.
- Part and part count are eight uppercase hexadecimal digits. Page and page
  count are two uppercase hexadecimal digits. Both are numbered from one.
- Each page carries at most 960 compressed bytes encoded as RFC 9285 Base45.
- Chunk CRC detects a bad photograph/scan independently. The compressed CRC
  verifies the reassembled stream of one part.
- The report ID is itself a CRC-32, taken over the report's metadata and its
  file names and sizes. Every page and part repeats it, and the decompressed
  bundle stores it again, so pages from different reports cannot be mixed.
  Each file segment carries its own CRC, which verifies the decompressed
  bytes.

Pages and parts may be scanned in any order. Duplicate identical pages are
harmless; conflicting duplicates, mixed report IDs, missing pages or parts,
overlapping file segments, or failed checksums are rejected.

## Compressed bundle

The chunks for each part concatenate into one zlib/DEFLATE stream. Its
decompressed payload is a `TFDG` version-2 part below; multibyte integers are
big-endian. The desktop decoder remains compatible with version 1 reports.

| Field | Encoding |
|---|---|
| magic | `TFDG` (4 bytes) |
| version, segment count | `u8`, `u8`; version 2 has one segment per part |
| flags, reserved | two zero bytes |
| report ID | `u32`; stable across every part |
| release sequence, creation Unix time | `u64`, `u64` |
| PSP model, firmware version | `u32`, `u32` |
| zero-based part index, part count | `u32`, `u32` |
| app version | `u8` byte length, then UTF-8 bytes |
| file segment | `u8` name length, `u8` flags, `u16` reserved, `u32` original size, `u32` segment offset, `u32` segment size, `u32` CRC, UTF-8 name, exact segment bytes |

Entry flag bit 0 marks a segmented file. The ordered segments cover every byte
from offset zero through the original file size. There are at most five source
files. File names are logical basenames, not Memory Stick paths. Every segment
has its own checksum; the decoder emits a file only after all of its segments
are present, contiguous, non-overlapping, and verified.

## Decoder allocation report

If **Save error reports** is On, a failed aligned firmware-codec allocation
adds a `media-allocation-v1` section to the ordinary last-error file and its
Diagnostic QR export. Enable the setting before reproducing the failure.
The section is a bounded numeric snapshot taken before failed-open cleanup:

- `missing` identifies buffers that were required but not allocated.
- `request`, `alignment` and `errno` describe the first refused heap fallback.
  Zero `errno` means the allocator supplied no error code, not that it succeeded.
- Heap `lower-bound` is the early boot probe's conservative result, not the
  heap's maximum. `arena`, `free` and `top` are allocator observations, not a
  promise that a new aligned block will fit. The top chunk is not necessarily
  the largest free chunk.
- Pool `bytes`, `used` and `flags` distinguish an absent pool (bit 0), a pool
  claimed by the optional decoder (bit 1), quarantine (bit 2), and a pool
  outside the Media Engine's addressable region (bit 3).
- `devkit`, picture `size`, remaining page `budget`, and system free/largest
  blocks help separate firmware layout, page pressure and allocator refusal.
  System free memory is a different pool from the C heap and must not be
  interpreted as the amount `memalign` could use.

| Missing-mask bit | Buffer |
|---|---|
| 0 | MPEG workspace |
| 1 | Media Engine DDR arena |
| 2, 3 | Output surfaces 0, 1 |
| 4 | Compressed-packet staging |
| 5 | Video access-unit control |
| 6 | SPS/PPS parameter sets |
| 7 | Audio codec control |
| 8 | Audio PCM |
| 9 | Audio output queue |
| 10 | Compressed-audio staging |
| 11 | Pending compressed audio |

No new memory probe, per-frame tracing or file write occurs for this section.
The extra fields contain no addresses, media contents, URLs or credentials.
The surrounding general error report can include the failing page URL and
error text; review those before posting publicly. Never attach saved-login,
cookie or session files. The separate sign-in-only report below intentionally
excludes general error logs.

## YouTube sign-in report

**Menu → Settings → Device & storage → YouTube sign-in diagnostics** is a
session-only, default-off switch. It records counts in RAM, and its report
contains no credentials. While
enabled, **Diagnostic QR** exports only `tilefinch-youtube-signin.txt` and
ordinary public app/firmware metadata. It excludes every general log source,
including Wi-Fi, validation and crash logs. Turning the switch off clears
both the counters and any previously prepared QR bundle. No diagnostic file
or profile setting is written by the switch.

The report formatter accepts only fixed numeric, boolean and enum fields. It
cannot accept an error string, account name, password, cookie value or hash,
token, URL, header or response body. It reports the last native feed phase,
handoff and feed HTTP status, transport/origin checks, session-cookie
eligibility, server-confirmed authentication, script degradation and admission
failures, cookie refusals/evictions and the recovery count. Parsed result
count and pending publication distinguish an empty feed from results waiting
for the user's Refresh action. No titles or video IDs appear.

Version 2 separates bootstrap, API and optional history HTTP statuses, fixed
failure categories, configuration/signature/request admission, feed
construction and publication outcomes. Authentication is independent of
display success. Build/publication outcomes are 0 not attempted, 1 pending, 2
successful, 3 refused/failed, and (publication only) 4 waiting for Refresh.
Clock availability is a boolean, not a timestamp or proof that the clock is
correctly set. Optional history outcomes are 0 not attempted, 1 pending, 2
complete, 3 timed out, and 4 failed. Failure category 11 means the session
changed before construction or publication completed. A definite server
sign-out revokes confirmation; transport, parsing, allocation and display
failures do not.

Version 4 adds Google refusing the browser during sign-in.
`accounts-refusals` counts the top-level navigations that ended on such an
answer while the switch was on. `refusal-class` names the last one: 1
Google's sign-in rejection page ("Couldn't sign you in"), 2 its
unusual-traffic interstitial, 3 HTTP 429 from the account site.
`refusal-reason` is the reason number the rejection page's address carried,
at most four digits, or 0 when there was none. Nothing else from that
address is recorded: no account hint, no other parameter.

Version 5 adds one line, `post-chain=`, for the redirect chain of the last
form submitted on the account pages. It is twelve numbers, kept short so the
report normally fits one QR code (one with many other non-zero fields, after
a script stop or a feed failure, takes two); `tools/decode_diagnostic_qr.py`
prints them with their names. In order:

1. `class`: 0 the chain finished, 1 timeout, 2 name lookup failed, 3 could
   not connect, 4 TLS failure, 5 HTTP/2 failure, 6 broken transfer, 7 a
   redirect or transport rule, 8 cancelled, 9 anything else.
2. `hops`: redirects followed.
3. `failed-hop`: the hop that failed, counting the submission itself as 0.
4. `failed-host`: that hop's host as a class: 1 the Google account site, 2 a
   YouTube host, 3 YouTube's account host, 4 another Google host, 5 any
   other, 0 none.
5. `limit`: the rule for class 7 and the kind of timeout for class 1: 1 the
   request deadline, 2 too many redirects, 3 a redirect loop, 4 a redirect
   to an address the redirect rules refuse, 5 more or larger cookies
   than the transport keeps (128 fields over the whole chain), 6 a request the transport
   cannot carry, 7 no data for the stall allowance, 8 a response over the
   size limit, 9 a redirect address longer than a redirect may name
   (8,191 bytes while opening a page, 2,047 for any other request), 10 a
   chain that ended on an address longer than a page may have (2,047 bytes;
   the redirects before it may be longer).
6. `hop-header-bytes` and 7. `hop-set-cookies`: the last response of the hop
   that failed, 0 when it never answered.
8. `max-header-bytes` and 9. `chain-set-cookies`: the largest response
   header block and the cookies received over the whole chain.
10. `first-byte-ms`: from the submission to that last response's headers.
11. `elapsed-ms`: from the submission to the end of the chain.
12. `timeout-ms`: the deadline they ran against.

These are counts, classes and durations: no address, host name, header or
cookie is recorded. A chain that ends by handing over to the native YouTube
page is dropped at the YouTube response's headers so that page can open
instead. It reads as finished (class 0, no failed hop, host or limit) with
the counts it reached: its redirects, the YouTube response's headers and
cookies, and the time to them. `post-http-status` is then that response's
status; the native page's own request reports in `handoff-http-status`.

The last completed feed hop also reports a numeric transport error, timeout
flag, TLS verification result and whether that result was available. These
fields contain no URL, certificate identity, cookie or response text. An
unavailable TLS result is unknown, not evidence that verification succeeded.

**Menu → Page tools → Continue to YouTube**
([user guide](USER_GUIDE.md#home-search-and-sign-in)) uses the stable account
GET entry to complete the current handoff, even if cookies from an earlier
session remain. The native provider then verifies the session. It never
repeats a password submission or a single-use handoff URL, and never runs the
full YouTube application.

Sign-in stages: 0 idle; 1 account page; 2 stable-entry recovery; 3 session
handoff; 4 native verification; 5 verified; 6 failed; 7 cancelled; 8 manual
retry needed because entered text must be preserved. A successful handoff or
cookie receipt is not a verified login. Recovery retries only the stable GET
entry once; it never resubmits a password or repeats single-use parameters.

## Recovering logs on a computer

Use any QR scanner to extract the text from each photograph, saving one page
per text file if convenient. Then run:

```sh
python3 tools/decode_diagnostic_qr.py page-1.txt page-2.txt \
  --output recovered-tilefinch-report
```

The decoder reorders pages, validates all checksums and bounds, decompresses
all parts, reconstructs each complete log, and writes the original files. It
does not read images itself; that remains the phone or desktop QR scanner's
job.
