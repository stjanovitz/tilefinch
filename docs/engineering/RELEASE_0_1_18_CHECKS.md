# Release 0.1.18 maintenance checks

Reviewed 2026-09-09; release sequence 19.

- Re-fetched the Public Suffix List: upstream remains
  `3955e3ec29b94c3cca7bd4509c5f14a7c0959e26`, with source SHA-256
  `a26f7d7e334778ed69216cedb5451ef82031feba6615c12039783cdd94e1fcae`.
  The committed graph requires no change.
- Revalidated all 25 retained CA fingerprints: 21 match the freshly downloaded
  Mozilla bundle; four compatibility variants match the current authoritative
  GTS, GlobalSign, and Microsoft downloads. No trust bytes or policy changed.
- The native Mbed TLS census covered 426 top-site, resource, and update origins:
  334 verified, 83 unreachable, nine apex hostname mismatches, and zero
  certificate-policy/trust failures. Unreachable hosts are not counted as passes.
- Rechecked [curl 8.21.0 advisories](https://curl.se/docs/vuln-8.21.0.html)
  against the PSP configuration. Affected cookie, LDAP, alternate-TLS,
  native-CA, and Negotiate paths remain disabled; no accepting HTTP/2 push
  callback is installed. Retain the qualified transport pin.
- The [Mbed TLS advisory list](https://mbed-tls.readthedocs.io/en/latest/security-advisories/)
  remains covered by the pinned 3.6.7 maintenance release. nghttp2 1.70.0
  is newer than the qualified 1.69.0 but identifies no security release reason
  to change that pin. FreeType 2.14.3 and Lexbor 3.0.0 remain current checked
  releases. Bellard QuickJS upstream still matches the vendored base
  `04be246001599f5995fa2f2d8c91a0f198d3f34c`; no upstream delta remains.

## Acceptance evidence

The optimized host suite passed all 151 enabled tests, including the fidelity
floor. The external signed-update proof requires the release artifacts and is
verified separately; the opt-in device-cost test remains disabled.

A release-review regression exposed retained style entries beyond invalidation
holes. Retirement now clears the entire bounded probe, including duplicate keys.
The new normal/pseudo-element regression fails before the fix and passes after it.

Physical PSP acceptance used the freshly rebuilt in-memory validation browser.
The Wikipedia journey navigated by pointer, entered a nonempty `psp` search with
Danzeff, and loaded its result page. It completed with zero Budget refusals and
clean teardown. A preliminary launch configured to exit after its initial report
did not execute input and is explicitly excluded from acceptance.

YouTube's natural activation/repeated-seek verifier passed with the 360p firmware
decoder: autoplay reached 14.3 seconds; three preview presses advanced the target
from 24.4 to 44.4 seconds; playback reached 86.6 seconds after commit. The run
displayed 1,698 frames, with zero state-machine mismatches, zero Budget refusals,
and clean teardown. Final measured audio/video skew was 82 ms, not zero.
The description/comment reuse and fragment-scroll changes are covered by the
optimized host journeys; no separate physical comment-expansion claim is made.

These checks do not claim uniformly short navigation slices, complete optional
third-party initialization, or that every unreachable census origin works.
