# Release 0.1.19 maintenance checks

Reviewed 2026-09-09; release sequence 20.

## Maintenance

- Re-fetched the official Public Suffix List. Its version comments now appear
  in the response (SHA-256
  `4b673689999dbaca60b93fa3e1da5752505ef9717b1c4dc44acbfdafd35679ea`), but
  its rules are unchanged from the 0.1.18 snapshot at
  `3955e3ec29b94c3cca7bd4509c5f14a7c0959e26`. No graph update is necessary.
- Revalidated all 25 retained roots against fresh Mozilla, GTS, GlobalSign
  and Microsoft downloads; every DER certificate matched. No trust changed.
- Re-ran the native-Mbed-TLS census over 426 top-site, resource and update
  origins: 334 verified, 83 unreachable, nine apex hostname mismatches,
  and no certificate-policy/trust failures. Unreachable hosts are not passes.
- Rechecked the upstream advisory/release pages used by the
  [0.1.18 review](RELEASE_0_1_18_CHECKS.md). The curl 8.21.0 findings remain
  outside the enabled PSP paths, and the Mbed TLS, nghttp2, FreeType and
  Lexbor pin decisions are unchanged. QuickJS upstream master still equals
  the vendored base `04be246001599f5995fa2f2d8c91a0f198d3f34c`.

## Acceptance before packaging

The optimized host build and full enabled CTest suite passed. The expanded
hermetic search visual gate checks middle/lower results, pagination and an
exact bottom-to-top revisit. A deliberately hidden sixth result fails that
gate; restoring the fixture passes without changing any reference.

A freshly rebuilt in-memory validation browser exercised the reported
watch-detail → Comments → Play path on a physical PSP. The logged focus
target was the original video card, with the expanded comments document
retained behind it. Firmware module preparation completed, and 360p Main
video plus AAC played through 10.5 seconds. The run displayed 252 frames,
dropped none, reported zero state-machine mismatches/violations and Budget
refusals, and exited cleanly. Final measured A/V skew was 52 ms.
The user's intermittent Loading 22% stall did not reproduce; this release
does not claim to fix it. The same emulator run only reached its documented
missing-mpeg_vsh boundary and is not counted as firmware playback acceptance.

A separate physical run loaded script-free Wikipedia full-text results,
scrolled down with the D-pad and returned upward. Its lower-results capture
was inspected. All 18 attempted images loaded, Budget refusals remained zero,
and teardown was clean. This is a results-scroll check, not a new end-to-end
keyboard-submission qualification.
