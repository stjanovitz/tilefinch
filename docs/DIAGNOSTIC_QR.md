# Diagnostic QR reports

Tilefinch can display its existing diagnostic logs as one or more QR codes.
This is a transport view only: building a report does not alter, delete, or
replace any log file, and it creates no new file on the Memory Stick.
It can also include the latest opt-in Wi-Fi failure report directly from RAM.

## User flow

Open **Help & diagnostics → Diagnostic QR**, press **X** to build the report,
then photograph every page. Use D-pad Left/Right or L/R to move between
pages, and D-pad Up/Down to move between report parts. Photograph every page
of every part. The report ID, part count, and page count stay visible so
photographs from different reports are easy to separate. Press Circle when
done; the current compressed part and QR working memory are released
immediately.

The release build includes `data/tilefinch-last-error.txt` when it exists.
Ordinary failures create or replace that file only when **Settings → Device &
storage → Save error reports** is On; the default is Off. A fatal startup
failure still writes one report because this screen may not become reachable. A
validation build additionally flushes its buffered validation log once, in
response to the explicit Build action, and can include the current and prior
validation/crash logs. Missing files are skipped. The last-error log comes
first, followed by current and previous validation/crash logs. A file larger
than one part is divided into exact contiguous segments, so no log bytes are
dropped. Each part contains at most 56 KiB of input and 64 QR pages. Moving to
another part frees the current compressed payload before reading and building
the requested part, keeping RAM bounded regardless of total log size.

With **Wi-Fi diagnostics** enabled, a failed connection retains a bounded
4 KiB report in RAM. Diagnostic QR puts that report first and owns a snapshot
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
  verifies the reassembled stream. The report CRC identifies the bundle and
  verifies the decompressed content.

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

## Recovering logs on a computer

### Credential-free YouTube sign-in report

**Settings → Device & storage → YouTube sign-in diagnostics** is a
session-only, default-off switch. It records bounded counts in RAM. While
enabled, **Diagnostic QR** exports only `tilefinch-youtube-signin.txt` and
ordinary public app/firmware metadata. It excludes every general log source,
including Wi-Fi, validation and crash logs. Turning the switch off clears
both the counters and any previously prepared QR bundle. No diagnostic file
or profile setting is written by the switch.

The report formatter accepts only fixed numeric, boolean and enum fields.
It cannot accept an error string, account name, password, cookie value or
hash, token, URL, header or response body. It reports the last native feed
phase, handoff and feed HTTP status, transport/origin checks, session-cookie eligibility,
server-confirmed authentication, script degradation and admission failures,
cookie refusals/evictions and the bounded recovery count.
Parsed result count and pending publication distinguish an empty feed from
results waiting for the user's Refresh action. No titles or video IDs appear.

Version 2 separates bootstrap, API and optional history HTTP statuses, fixed
failure categories, configuration/signature/request admission, feed construction
and publication outcomes. Authentication is independent of display success.
Build/publication outcomes are 0 not attempted, 1 pending, 2 successful,
3 refused/failed, and (publication only) 4 waiting for Refresh. Clock availability
is a boolean, not a timestamp or proof that the clock is correctly set.
Optional history outcomes are 0 not attempted, 1 pending, 2 complete,
3 timed out, and 4 failed. Failure category 11 means the session changed
before construction or publication completed. A definite server sign-out
revokes confirmation; transport, parsing, allocation and display failures do not.

The last completed feed hop also reports a numeric transport error, timeout
flag, TLS verification result and whether that result was available. These
fields contain no URL, certificate identity, cookie or response text. An
unavailable TLS result is unknown, not evidence that verification succeeded.

If an account step finishes without returning to the provider, **Page tools →
Continue to YouTube** uses the stable account GET entry to complete the
current handoff, even if cookies from an earlier session remain. The native
provider then verifies the session. It never repeats a password submission
or a single-use handoff URL, and never executes the full YouTube application.

Sign-in stages: 0 idle; 1 account page; 2 stable-entry recovery; 3 session
handoff; 4 native verification; 5 verified; 6 failed; 7 cancelled; 8 manual
retry needed because entered text must be preserved. A successful handoff or
cookie receipt is not a verified login. Recovery retries only the stable GET
entry once; it never resubmits a password or repeats single-use parameters.

### Decode a report

Use any QR scanner to extract the text from each photograph, saving one page
per text file if convenient. Then run:

```sh
python3 tools/decode_diagnostic_qr.py page-1.txt page-2.txt \
  --output recovered-tilefinch-report
```

The decoder reorders pages, validates all checksums and bounds, decompresses
all parts, reconstructs each complete log, and writes the original files. It deliberately does not do
camera/image recognition; that remains the phone or desktop QR scanner's job.
