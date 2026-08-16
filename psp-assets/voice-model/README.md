# Offline voice model

These are the validated `14M-extra-wide` and `10M-small` English search tiers
imported from the PSP Search Speech Prototype. They share one PocketSphinx US
English acoustic model and provide bounded 7,763-entry and 1,463-entry search
language models. They are intentionally a search/dictation convenience, not
open-vocabulary speech recognition: words outside the selected dictionary
cannot be returned.

The acoustic model is approximately 6.3 MiB on storage. Tilefinch compiles a
small exact dictionary-to-context sidecar for each vocabulary; CMake checks
the expected size and SHA-256 so a stale model and map cannot be packaged.
The fixed loader also uses compact language-model tables, verified
three-state channel storage, checked 16-bit fixed-search IDs, and selectable
exact sendump residency.

This data is not part of the default browser zip or either A/B browser slot.
`tilefinch-voice-component-package` stages it with the two exact sidecars, an
ABI marker, and its Alpha Cephei/CMUdict notices into a deterministic 9.1 MB
TFVP. The in-app Experimental menu downloads that package only after explicit
confirmation and installs it transactionally under
`components/voice-en-us/`. Installed signed metadata supplies the component's
anti-rollback floor; no component files are read during ordinary boot.
The package carries the Alpha Cephei model license and the complete CMUdict
license and provenance notice under `LICENSES/`; the packaging target fails
if any required payload or license file is absent.

In the standalone PPSSPP lab the extra-wide decoder reached 6.80 MiB of live
heap and a 6.91 MiB allocator arena, down from 12.82 MiB for the preceding
fixed/direct representation. The small tier measured 4.88 MiB for both live
heap and allocator arena. The strict 192-row reservations round these to
8 MiB and 6 MiB respectively; larger row sets add their exact storage and
still require a separate 2 MiB selection margin. A 33-fixture host gate
produced identical final hypotheses and scores against the
general-representation oracle. The strict memory profile trades CPU and
Memory Stick traffic for RAM; physical PSP latency still needs calibration.
Tilefinch uses the complete 384-row acoustic table by default. The
off-by-default **Adaptive voice memory** option permits 256- and 192-row
fallbacks under measured pressure. It chooses the largest extra-wide
configuration that fits current total and contiguous heap before considering
the small tier, loads only one decoder lazily, and evicts the resident decoder
before page navigation or at a memory-pressure low-water mark.

The acoustic model is copyright (c) 2015 Alpha Cephei Inc. and distributed
under the following terms:

> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that source redistributions retain the
> copyright notice, conditions, and disclaimer, and binary redistributions
> reproduce them in accompanying documentation.
>
> THIS SOFTWARE IS PROVIDED BY ALPHA CEPHEI INC. "AS IS". ANY EXPRESS OR
> IMPLIED WARRANTIES, INCLUDING MERCHANTABILITY AND FITNESS FOR A PARTICULAR
> PURPOSE, ARE DISCLAIMED. IN NO EVENT SHALL ALPHA CEPHEI INC. OR ITS
> EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
> EXEMPLARY, OR CONSEQUENTIAL DAMAGES, HOWEVER CAUSED.

PocketSphinx itself is copyright (c) 1999–2016 Carnegie Mellon University and
is distributed under its BSD-style license. Its full license is retained in
[`third_party/POCKETSPHINX_LICENSE.txt`](../../third_party/POCKETSPHINX_LICENSE.txt).
