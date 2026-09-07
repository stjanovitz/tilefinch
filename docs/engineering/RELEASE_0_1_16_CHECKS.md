# Release 0.1.16 maintenance checks

Reviewed 2026-09-06 before packaging; release sequence 17.

- Public Suffix List refreshed to `f540a06159213b6e8ed9c87d2dd3a52373637e92`.
  The generated graph grows by 616 read-only bytes; allocation behavior is
  unchanged. See the snapshot's third-party README for reproducible inputs.
- The compact CA bundle adds QuoVadis Root CA 2 G3 and DigiCert TLS RSA4096
  Root G5, downloaded from the CA repositories and independently matched to
  the current Mozilla snapshot. No certificate checks or hash policy change.
  See `certs/README.md` and its fingerprint gate.
  Native Mbed TLS audited 426 current top-site, resource, and update origins:
  334 verified, 85 unreachable, seven apex hostname mismatches, and zero
  remaining certificate-policy/trust failures. Unreachable hosts are not
  counted as verified. All three update-service origins verified.
- [curl 8.21.0 advisories](https://curl.se/docs/vuln-8.21.0.html) were reviewed
  against the PSP configuration. The listed cookie, LDAP, WolfSSL, OpenSSL,
  native-CA, and Negotiate paths are not enabled in this build. The HTTP/2
  push issue requires an accepting `CURLMOPT_PUSHFUNCTION`; the transport
  installs none. Keep the device-qualified transport pin for this release.
- [Mbed TLS advisories](https://mbed-tls.readthedocs.io/en/latest/security-advisories/)
  list the July fixes in the already-pinned 3.6.7 maintenance release.
- [nghttp2 1.70.0](https://github.com/nghttp2/nghttp2/releases/tag/v1.70.0)
  is newer than the 1.69.0 pin; its notes do not identify a security fix.
  Keep the qualified library rather than changing it during packaging.
- [FreeType](https://freetype.org/) 2.14.3 and
  [Lexbor](https://github.com/lexbor/lexbor/releases) 3.0.0 match the checked
  upstream releases.
- Bellard QuickJS upstream HEAD remains
  `04be246001599f5995fa2f2d8c91a0f198d3f34c`, the vendored upstream base:
  there is no newer upstream commit delta to review. Tilefinch's local
  engine changes remain covered by the optimized host tests.

Device acceptance of the one-minute navigation ceiling completed the large
article in 35.887 seconds. Native-menu/toolbar/cursor and firmware playback
plus committed-seek checks completed with clean PSPLink exit. This is not a
claim that every page phase is short; remaining long slices need more work.
The current live section-navigation fixture landed on an infobox label, so
that run is not recorded as section-expansion acceptance.
