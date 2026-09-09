# Release 0.1.17 maintenance checks

Reviewed 2026-09-08; release sequence 18.

- Refreshed the Public Suffix List to
  `3955e3ec29b94c3cca7bd4509c5f14a7c0959e26`. The generated graph adds ten
  read-only bytes and no allocation or executable code.
- Revalidated the 25 retained CA anchors against current Mozilla/GTS
  snapshots and authoritative Microsoft, GlobalSign, and GTS repositories.
  The legacy GTS R1 fingerprint is still published as `oldr1.pem`; retain
  that device-qualified variant. No trust anchors or verification policy changed.
- Native Mbed TLS audited 426 top-site, resource, and update origins:
  334 verified, 84 unreachable, eight apex hostname mismatches, and zero
  certificate-policy/trust failures. Unreachable hosts are not counted as
  verified. All three update origins verified.
- Rechecked the [curl 8.21.0 advisories](https://curl.se/docs/vuln-8.21.0.html)
  and PSP configuration. The affected cookie, LDAP, alternative-TLS,
  native-CA, and Negotiate paths remain disabled; no accepting HTTP/2 push
  callback is installed. Keep the qualified transport pin.
- The [Mbed TLS advisory list](https://mbed-tls.readthedocs.io/en/latest/security-advisories/)
  remains covered by the pinned 3.6.7 July maintenance fixes. nghttp2 1.70.0
  is newer but does not identify a security fix; retain qualified 1.69.0.
  FreeType 2.14.3 and Lexbor 3.0.0 remain current checked releases.
- Bellard QuickJS upstream still matches the vendored base
  `04be246001599f5995fa2f2d8c91a0f198d3f34c`; no upstream delta remains.

## Acceptance evidence and limits

The optimized host suite passed all 151 enabled tests. The external update-root
proof skipped without its prerequisite; the opt-in device-cost test was disabled.
Ordinary and validation named PSP targets passed their executable-size gates.

Physical PSP checks exercised the responsive Wikipedia search form using actual
pointer, Danzeff typing, and submit events. The loaded nonempty `psp` query showed
search results; Budget refusals were zero and teardown retained no ownership.
Earlier empty-query and missed-input attempts were rejected, not counted as passes.

YouTube's natural activation and repeated-seek verifier passed with the 360p
firmware decoder: autoplay reached 14.4 seconds, preview advanced from 24.4 to
44.4 seconds, and playback reached 85.0 seconds after commit. It displayed 1,651
pictures, with zero state-machine mismatches, zero Budget refusals, and clean
teardown. Final measured audio/video skew was 95 ms; this is not a zero-skew claim.

A late dynamic-script request can still time out, and long runtime slices remain.
The long article check retained working cursor/native-menu interaction and clean
exit. This release accepts that known limitation; it does not claim complete
initialization of every optional third-party script or uniformly short frame times.
