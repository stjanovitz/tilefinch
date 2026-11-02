# Site survey tooling

Scripts for surveying a set of sites on the host lab, PPSSPP and Chromium.
Captured pages are private third-party data: they live under
`scratch/survey/NAME/` (git-ignored) and are never committed.

| Script | Purpose |
| --- | --- |
| `lab.sh capture\|replay\|pspreplay NAME URL` | Host lab load at 480x272 with the shipping defaults (Basic blocking, cookie notices hidden), realistic profile (the PSP app's own limits), 8 MiB document cap. `capture` records `NAME/capture` from the live site; `replay` is response-keyed; `pspreplay` replays the PSP's own capture (`NAME/psp-capture`, from `ppsspp.sh`). |
| `ppsspp.sh capture\|replay NAME URL` | PPSSPP at 111 MHz. `capture` records the PSP's own requests over live WLAN into `NAME/psp-capture`; `replay` serves it keyed. `SCRIPT=site-survey-load` gives heavy pages ~100 s to settle. Needs `PSPDEV` and a validation build. |
| `summarize.py RUN_DIR` | Load, memory, JavaScript, image and relayout summary of a PPSSPP run. |
| `chrome-ref.js URL OUTDIR NAME` | Live Chromium reference (Tilefinch UA, 480x272, mobile). `HIDE_CONSENT=1` hides, never clicks, consent overlays. |
| `chrome-replay.js TRACE URL OUTDIR NAME` | Offline Chromium render of a trace. Records Tilefinch truncated at its quotas poison it, so it is only a fallback. |
| `sheet.py OUT L R left,right ...` | Side-by-side sheet (Tilefinch left, Chrome right) without Pillow. |

Set `NODE_PATH` to a `node_modules` containing Playwright for the Chrome tools.
