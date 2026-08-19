# Third-party notices

Tilefinch's own source is MIT licensed ([LICENSE](LICENSE)). That grant covers
this repository's code and does not extend to the components below, each of
which keeps its own terms.

Most build dependencies are downloaded at configure time, hash-pinned in
`CMakeLists.txt` and `cmake/`; their complete license texts live inside the
fetched checkouts under `<build>/_deps/<name>-src/`. The bounded font subsets,
voice models, and Public Suffix List snapshot are checked in because they are
runtime assets or immutable engine data.

## Engine and networking

- **Lexbor 3.0.0** (tag `v3.0.0`): Apache License 2.0. The checkout contains
  the complete `LICENSE` and `NOTICE` files; both must accompany binary
  distributions.
- **Bellard QuickJS**, commit `04be246001599f5995fa2f2d8c91a0f198d3f34c`
  (upstream VERSION 2026-06-04): MIT License. This is the backend used by
  every checked-in preset, including the PSP build; the build applies the
  repository's bounded-lifetime and responsiveness patches (`patches/`) to the
  hash-pinned source. The checkout contains the complete `LICENSE` file.
- **QuickJS-NG 0.15.0**, commit `433941b99fb3c5e7f98b7ebd78727972bcf467ee`:
  MIT License (per upstream). Selectable via
  `PSP_BROWSER_USE_BELLARD_QUICKJS=OFF`; no checked-in preset uses it, so it
  is not part of release binaries.
- **stb**, commit `31c1ad37456438565541f4919958214b6e762fb4`: dual
  public-domain / MIT single-file libraries. The build uses `stb_truetype.h`
  v1.26 and `stb_image.h`, both with budget-allocation hooks. Shipping the
  MIT text satisfies either alternative.
- **NanoSVG**, commit `239e102ec2c691f2902e20ace2ed36ee4a35cfe6`: zlib
  License. The build uses its parser and rasterizer with budget-allocation
  hooks.
- **libwebp 1.6.0**: BSD-3-Clause-style license plus Google's additional
  patent grant. Tilefinch links only the static decode library, with encoders,
  tools, animation helpers, SIMD, and threading disabled. Decode output is a
  caller-owned viewport-sized buffer and upstream scratch is pre-admitted
  against the resource budget. Complete `COPYING` and `PATENTS` texts are
  checked in under `third_party/notices/libwebp/`.
- **FreeType 2.14.3**: dual-licensed FreeType License (FTL) or GPLv2.
  **Tilefinch elects the FTL** for all use and distribution of FreeType; the
  GPLv2 alternative is not exercised. The FTL's credit clause requires the
  notice "Portions of this software are copyright © The FreeType Project
  (www.freetype.org). All rights reserved." in distributions, and this file
  carries it: Portions of this software are copyright © The FreeType Project
  (www.freetype.org). All rights reserved. The checkout also bundles
  zlib-licensed gzip code and Old-MIT HarfBuzz-derived autofit sources; carry
  those notices too. Complete texts are in the checkout's `LICENSE.TXT` and
  `docs/FTL.TXT`.
- **Public Suffix List** snapshot `b9a86cf0cd115f1e60b5815533f3fcfd2f9e8f4b`:
  MPL-2.0 data encoded as a checked-in DAFSA. The fixed-set decoder in
  `src/public_suffix.c` was adapted from libpsl/Chromium under its
  BSD-3-Clause license. Source, generation details, hashes, and both license
  texts are under `third_party/public_suffix/`; MPL-2.0 §3.2 requires telling
  binary recipients how to obtain the data's source form (the README there
  records the upstream commit and hash).
- **libcurl 8.21.0**: curl license (MIT-style). The default PSP release
  cross-builds this hash-pinned official source archive inside the project;
  `TILEFINCH_PSP_TRANSPORT_MODE=LEGACY` is a non-release escape hatch.
- **mbed TLS 3.6.6 LTS**: dual Apache-2.0 / GPL-2.0-or-later; Tilefinch
  elects Apache-2.0. The default PSP release cross-builds the hash-pinned
  official source archive with a narrow PSP entropy/time portability patch
  (`patches/mbedtls-3.6.6-psp.patch`), plus an Allegrex bignum
  multiply-accumulate core (`patches/mbedtls-3.6.6-psp-bnmul.patch`) that
  adds a `maddu`-based `MULADDC` block to `library/bn_mul.h` beside the
  stock MIPS32 one. Both patches are applied to the extracted tree; the
  bignum block is compiled only when
  `TILEFINCH_PSP_ALLEGREX_BIGNUM_ASM=ON` defines
  `TILEFINCH_ALLEGREX_MULADDC`, and upstream's own MIPS32 block is left
  byte-for-byte intact as the fallback. Neither patch changes
  verification, cipher selection, or any security behaviour.
- **nghttp2 1.69.0**: MIT License. The default PSP build uses its library for
  TLS-negotiated HTTP/2 with HTTP/1.1 fallback. It can be excluded with
  `TILEFINCH_PSP_HTTP2=OFF` for comparison or emergency fallback.
- **zlib**: linked from the PSPDEV SDK under the zlib license.
- **Project Nayuki QR Code generator**, commit
  `2c9044de6b049ca25cb3cd1649ed7e27aa055138`: MIT License. Tilefinch
  vendors the allocation-free C encoder under `third_party/qrcodegen/` for
  the user-triggered diagnostic export screen; the complete license is
  reproduced in the source headers and at
  `third_party/notices/qrcodegen/LICENSE`.
- **OpenSSL** (host builds only): `OpenSSL::Crypto` is linked into host
  laboratory and test binaries. Apache License 2.0. Not part of the PSP
  EBOOT.

## Voice recognition

- **PocketSphinx 5.1.1** (PyPI source release, patched under
  `patches/pocketsphinx/`): a compound BSD-style license — the CMU
  BSD-2-Clause-style grant with a DARPA/NSF acknowledgement paragraph, plus
  BSD-3-Clause (WebRTC VAD), MIT (jsmn), and BSD-2-Clause (json-builder)
  for embedded portions. The complete compound text is checked in at
  `third_party/POCKETSPHINX_LICENSE.txt` (byte-identical to the fetched
  checkout's `LICENSE`) and must accompany binaries; its clause 2 is an
  explicit binary-redistribution notice requirement.
- **Alpha Cephei US English acoustic model** (`psp-assets/voice-model/en-us/`,
  distributed only in the optional signed voice component): BSD-2-Clause,
  "Copyright (c) 2015 Alpha
  Cephei Inc." The license text lives in
  `psp-assets/voice-model/en-us/README`; its binary-redistribution clause
  requires reproducing the notice in accompanying documentation.
- **Search dictionaries and language models**
  (`psp-assets/voice-model/search/` and `extra-wide/`): built by the PSP
  Search Speech Prototype from CMU Sphinx project data. The pronunciation
  dictionaries (`search.dict`, 1,463 and 7,763 entries) are subsets derived
  from the **CMU Pronouncing Dictionary (CMUdict)**, Copyright (C) Carnegie
  Mellon University, distributed under a Simplified BSD (2-clause) license
  whose use "for any research or commercial purpose is completely
  unrestricted" with an attribution request. The `search.lm.bin` language
  models were generated by the prototype over those word lists in the CMU
  Sphinx binary LM format and carry the same attribution. Redistribution of
  these files should reproduce the CMUdict notice alongside the Alpha Cephei
  acoustic-model notice above.

## Fonts

- **DejaVu Fonts 2.37**: Bitstream Vera / Arev license terms, DejaVu changes
  public domain. The configure step downloads the official TTF release with
  its complete `LICENSE`; the five checked-in `DejaVu*-Latin.ttf` files are
  bounded Latin/punctuation subsets of the same release, distributed with
  `fonts/LICENSE-DejaVu.txt`. The Vera license has its own
  binary-redistribution notice requirement.
- **TilefinchSans-Regular.ttf / TilefinchSans-Bold.ttf**: checked-in
  Arial/Helvetica-metric fallback faces under the SIL Open Font License 1.1,
  `fonts/LICENSE-TilefinchSans.txt` (Arimo/Tinos/Cousine and Liberation
  ancestry). Reserved-Font-Name provenance (verified 2026-07-30 against the
  shipped `name` tables with fontTools): the faces are renamed derivatives
  of Liberation Sans 2.1.5 (version string `Version 2.1.5`; copyright
  "Digitized data copyright (c) 2010 Google Corporation" and "Copyright (c)
  2012 Red Hat, Inc."), itself an Arimo derivative. The ancestors' OFL
  declares "Arimo, Tinos and Cousine" (Google) and "Liberation" (Red Hat)
  as Reserved Font Names. Every naming field in both shipped TTFs (name IDs
  1, 3, 4, and 6) reads "Tilefinch Sans" / "TilefinchSans-*" and contains
  none of those Reserved Font Names, satisfying OFL condition 3; the
  ancestor names appear only in the retained attribution fields (the
  trademark notice, ID 7, and the description, ID 10), which the OFL
  requires to be preserved and does not treat as the font's name.
- **GNU Unifont 17.0.04**: a generated bitmap subset provides common
  GB2312/Shift-JIS Japanese and Chinese glyphs plus bounded symbol/emoji
  ranges. `tools/generate_font_fallback.py` selects and block-compresses the
  SHA-256-pinned upstream `unifont_all-17.0.04.hex.gz`; Unifont is
  dual-licensed and this project elects the SIL Open Font License 1.1 option,
  recorded in `fonts/LICENSE-Unifont.txt` and the
  `src/generated/font_fallback_bitmaps.inc` header.

OFL-licensed font files must travel with their license texts, not only with a
notice in this file.

## Platform SDK and tools

- **Danzeff OSK layout and control convention**: the native text-entry mode
  uses the character ordering created by Danzel and Jeff Chen and follows the
  texture-free Danzeff-G rewrite by Geecko. BSD-3-Clause; the complete notice
  is checked in at `third_party/notices/danzeff/LICENSE` and accompanies PSP
  binary distributions.

- **PSPDEV SDK** (v20260701 used for validation builds): PSPSDK, newlib, and
  pthread-embedded carry their own licenses; the Memory Stick validation
  package includes their generated notices under `third-party-licenses/`. The
  stable launcher links the PSP CFW SDK's `SystemCtrlForUser` import stub
  (distributed by PSPDEV under its own GPL-3.0-only and MIT licensing
  metadata); the installed custom firmware supplies the implementation used
  to hand off to a slot EBOOT.
- The PSP package stages the eight public CA root certificates listed in
  `certs/README.md`; they are trust data, not private key material.
- **PPSSPP** 1.20.4 was used only to execute the EBOOT during validation and
  is not redistributed.

## Optional components not in release builds

- **GNU lightning** (LGPL-3.0-or-later): linked only when the experimental
  `PSP_BROWSER_QUICKJS_NATIVE_TRACE=ON` diagnostic is enabled; OFF by default
  and in every preset. Enabling it in a distributed binary would add LGPL
  relinking obligations.
- **FFmpeg** (LGPL-2.1+/GPL by configuration): used by the host media
  laboratory (`PSP_BROWSER_BUILD_HOST_MEDIA_LAB`) for desktop media
  validation; not part of the PSP EBOOT.
- **Apple JavaScriptCore**: macOS-only diagnostic spike, disabled in presets.

## Distributing binaries

A browser release archive must bundle, at minimum: the Lexbor `LICENSE` and
`NOTICE`;
the FreeType FTL text and credit line (plus its embedded zlib/HarfBuzz
notices); the MIT texts for QuickJS, stb, and the QR encoder; the NanoSVG zlib text; the
libwebp copyright license and patent grant;
Public Suffix List MPL-2.0 and Chromium BSD-3-Clause texts with the source
pointer; the complete PocketSphinx compound license; the DejaVu and both OFL
font licenses alongside the font files; the
curl, nghttp2, mbed TLS, and zlib texts for the PSP stack; the PSPDEV
component notices; and the Danzeff BSD-3-Clause notice. Retain all applicable
upstream notices whenever binaries are redistributed.

The optional voice component is distributed independently and must contain
the Alpha Cephei acoustic-model notice plus the verbatim CMUdict Simplified
BSD license and its provenance notice. The component packager embeds those
files as `LICENSES/ALPHA_CEPHEI_LICENSE.txt`,
`LICENSES/CMUDICT_LICENSE.txt`, and `LICENSES/CMUDICT_NOTICE.md`. The browser
install tree also carries copies of these component notices proactively, even
though it deliberately omits the model and dictionaries themselves.

The `tilefinch-psp-install-tree` target now stages all of this
automatically. The exact upstream texts are checked in under
`third_party/notices/` (copied from the hash-pinned `_deps` checkouts and
the PSPDEV SDK's `psp/share/licenses/` tree); `cmake/StagePspInstall.cmake`
copies them, together with this file, the project `LICENSE`,
`third_party/public_suffix/`, `third_party/POCKETSPHINX_LICENSE.txt`, and
the Alpha Cephei model notice and complete CMUdict license, into a `NOTICES/`
directory at the root of the staged install tree, and stages the DejaVu/OFL
font license texts
beside the font files in `slot-a/fonts/`. After staging, the script checks
the tree against `cmake/PspNoticesManifest.cmake`, a manifest that mirrors
the checklist above item by item, and fails the build if any listed file is
missing — keep the manifest and this checklist in sync when either changes.
The verbatim CMUdict license is checked in at
`third_party/notices/cmudict/LICENSE`; the staged-install manifest and the
voice-component packager both enforce its presence.
