# Manifest of every license and notice file the staged PSP install tree
# must contain, relative to the tree root.  This list mirrors the
# "Distributing binaries" checklist in THIRD_PARTY_NOTICES.md item by item;
# update both together so the checklist and the staged tree cannot drift.
# StagePspInstall.cmake includes this file and fails the build if any entry
# is missing after staging.
set(TILEFINCH_NOTICES_MANIFEST
    # Project license and the notices index itself.
    NOTICES/LICENSE
    NOTICES/THIRD_PARTY_NOTICES.md
    # Lexbor: Apache-2.0 LICENSE and NOTICE.
    NOTICES/lexbor/LICENSE
    NOTICES/lexbor/NOTICE
    # FreeType: FTL text plus credit line and embedded zlib/HarfBuzz notices.
    NOTICES/freetype/FTL.TXT
    NOTICES/freetype/LICENSE.TXT
    NOTICES/freetype/NOTICE-embedded.txt
    # MIT texts for QuickJS, stb, and the diagnostic QR encoder.
    NOTICES/quickjs/LICENSE
    NOTICES/stb/LICENSE
    NOTICES/qrcodegen/LICENSE
    # NanoSVG zlib text.
    NOTICES/nanosvg/LICENSE.txt
    # libwebp copyright license and additional patent grant.
    NOTICES/libwebp/COPYING
    NOTICES/libwebp/PATENTS
    # Public Suffix List: MPL-2.0 and Chromium BSD-3-Clause texts with the
    # source pointer (README records upstream commit and hash).
    NOTICES/public_suffix/LICENSE.MPL-2.0
    NOTICES/public_suffix/LICENSE.chromium
    NOTICES/public_suffix/README.md
    # Complete PocketSphinx compound license.
    NOTICES/pocketsphinx/POCKETSPHINX_LICENSE.txt
    # Alpha Cephei acoustic-model notice plus the complete CMUdict license and
    # provenance notice for the voice dictionaries and language models.
    NOTICES/voice-model/ALPHA_CEPHEI_LICENSE.txt
    NOTICES/cmudict/LICENSE
    NOTICES/cmudict/NOTICE.md
    # Project-owned curl, nghttp2, and mbed TLS plus SDK zlib.
    NOTICES/curl/COPYING
    NOTICES/curl/NOTICE.md
    NOTICES/nghttp2/COPYING
    NOTICES/mbedtls/LICENSE
    NOTICES/zlib/README
    # Danzeff keyboard layout/control convention.
    NOTICES/danzeff/LICENSE
    # PSPDEV component notices.
    NOTICES/pspdev/newlib/COPYING.NEWLIB
    NOTICES/pspdev/pspsdk/LICENSE
    NOTICES/pspdev/pthread-embedded/COPYING.LIB
    NOTICES/pspdev/pthread-embedded/COPYING.vita
    NOTICES/pspdev/pthread-embedded/README.md
    NOTICES/pspdev/psp-cfw-sdk/gpl-3.0.txt
    NOTICES/pspdev/psp-cfw-sdk/iplsdk/LICENSE
    # OFL requires font licenses to travel with the font files themselves;
    # DejaVu's Vera terms have the same notice requirement.  Unifont glyphs
    # are compiled into the EBOOT, so its OFL text rides here too.
    slot-a/fonts/LICENSE-DejaVu.txt
    slot-a/fonts/LICENSE-TilefinchSans.txt
    slot-a/fonts/LICENSE-Unifont.txt
    # Duplicate font licenses in NOTICES so the tree has one complete set.
    NOTICES/fonts/LICENSE-DejaVu.txt
    NOTICES/fonts/LICENSE-TilefinchSans.txt
    NOTICES/fonts/LICENSE-Unifont.txt
)
