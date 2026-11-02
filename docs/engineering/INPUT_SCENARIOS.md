# Input scenarios

Pass criteria for the committed scripts in `tests/input-scripts/`. The script
language, runners and outcomes are in
[Input script reference](INPUT_SCRIPT_REFERENCE.md); the records each
criterion reads are in [Validation records](VALIDATION_RECORDS.md). A
completed script is never success by itself: every scenario names the
receiver output that proves it.

Scenarios whose name ends in `-live`, or that start at a live provider page,
depend on external services. They are diagnostics, not stable goldens; their
live evidence stays private.

## Native HOME and menus

### cursor-latency

`cursor-latency` qualifies native HOME at an explicit 333 MHz (also when the
clock override is unset/zero). Continuous movement segments exclude the
script's deliberate idle gaps and cursor-edge clamps. Both sample cadence and
sample-to-accepted-presentation latency must average at most 20 ms and peak at
most 34 ms; every sampled move must receive a presentation with no coalescing.
Explicit other clocks, such as `TILEFINCH_PPSSPP_CPU_MHZ=111`, are pressure
profiles: timings are reported without claiming stock-clock qualification,
while presentation accounting and the receiver golden remain mandatory.
`tilefinch-cursor-work` splits composition from publication (including vblank)
to diagnose missed refreshes without weakening the stock-clock gate. Each run
keeps its requested emulator configuration beside its log.

### menu-tour

Golden coverage is listed in the
[reference](INPUT_SCRIPT_REFERENCE.md#goldens). The home-menu performance journey runs the tour with
`--data-dir tests/fixtures/ppsspp-menu-tour-data`, three fixed-name
screenshot files and no profile. The screenshot list then scrolls a little
past one screen, so its page-down and page-up presses move the page, and the
repeated ones are no-ops at its ends. Without the seed every one is a no-op.
The receiver trace does not change either way.

### site-restore

`site-restore` idles on native HOME and must be run with
`--data-dir tests/fixtures/ppsspp-site-data`, which seeds a profile with
local-storage persistence and a 1 MB disk cache enabled, one local-storage
snapshot, and a 120 KiB cache snapshot beside the EBOOT. Its golden is
trivial; the gate is the validation log, which must show both deferred
restores completing through the frame-pump policy, the baseline font read
yielding to a restore slice, and no order or wiring violations. The seed is
written by `tilefinch-site-data-fixture --write` with the shipping writers,
and the `tilefinch-site-data-fixture-tests` host test reloads it with the
shipping readers, so a persistence-format change fails on the host instead of
turning the emulator run into a restore with nothing to read.

Validation builds print both frame-pump violation counts at exit and, for
each pump, how many admissions and slices the run saw and how often it was
refused because of another pump rather than a frame fact
(`tilefinch-frame-pump: name=baseline-fonts admissions=1 slices=1 yields=1`).
Admissions and slices show that a site is live, not merely present in the
source; a yield shows that a declared relation arbitrated something. Two
relations rest on the host table test (`tests/test_frame_pumps.c`) alone,
with no emulator evidence: cache restoration waiting for the baseline fonts
(on this boot path, the network-warming fact holds cache restoration back
until long after the single font slice) and page-idle yielding to a download
slice (which needs a live media download).

### navigation-cancel-live

Starts from native HOME, begins a shared-worker navigation, presses Circle
while response/parser work is active, and captures loading, immediate
acknowledgement, and settled cancellation
([device qualification](DEVICE_QUALIFICATION.md#scripted-input-during-work)).

## Page input during work

### runtime-input-live, runtime-cancel-live, wikipedia-navigation-live

`runtime-input-live` and `runtime-cancel-live` use the bounded cooperative
runtime fixture. Their goldens preserve action order/counts and captures but
omit asynchronous cursor positions; the raw trace remains available. Separate
gates require runtime-specific queue, successful presentation, and cancellation
evidence. `wikipedia-navigation-live` uses `-page` input and additionally checks
distinct focus geometry, an authored or browser focus indicator, pending tasks,
and captures for visual inspection. Menu focus has a separate 150 ms
emulator gate; menu activation is timed but still requires full relayout.
Its golden ignores asynchronous action-cursor annotations, not receiver
order or captures. A passing action golden alone is not a latency budget.

### page-response-live, page-focus-live

`page-response-live` exercises timed presses (`tap-after-live`) while
optional page work is still active, then repeats page input later. Use
identical captured responses and an explicit emulator clock for A/Bs.
`page-focus-live` isolates focus from menu presses that would supersede it.
Require one Down response (focus, or accepted completion-time scroll) and two
focus-right receiver receipts, not merely a clean exit.

### wikipedia-native-priority-live

`wikipedia-native-priority-live.txt` exercises Select, Settings navigation,
Triangle, and nub motion during a committed page's reflow. Require
`busy-menu` to show `screen=menu`, `busy-settings` to show `screen=options`,
and `toolbar-hidden` to report `visible=0`; a completed script alone is not
success.
Presentation-only navigation runs on the owner thread's retained-frame
supervisor; page-changing actions and chords remain with the main receiver.
Native menus defer new page runtime/resource/raster work. An in-flight
operation still reaches a safe checkpoint before input can be serviced.
Correlate `max-ack` with `max-gap` and `tilefinch-cooperate-slow-gap`: the
former measures presentation after sampling, not physical press-to-display
latency. Live script steps are themselves checkpoint-driven.

### wikipedia-search-keyboard-live, wikipedia-font-cursor-live

`wikipedia-search-keyboard-live` starts at the English Main Page, hides
the toolbar, opens its responsive search icon, preserves the separate page's
initial text-field focus, enters `psp` through Danzeff, and submits with R+Start.
Check that `search-loaded` is loaded, its URL contains the nonempty `search=psp`
query, and its capture shows search results. Optional resources may still be
pending; a keyboard-close, empty-query page, or action acknowledgement alone
is not success. The initial icon coordinates are tied to the live 480px layout:
inspect the `search-field` capture and focus-target log when the site changes.
Require the text input `ooui-php-1`, not its overlapping label control. This is a
live smoke, not a stable page-layout golden.
`wikipedia-font-cursor-live` alternates nub movement and idle time during
optional-font publication. Check the font adoption/rollback and owner-thread
input-acknowledgement records together with the captures. The fallback
layout must survive preemption, and font adoption must remain retryable.
Both scenarios use the normal receiver; neither injects a search-result URL.

### youtube-scroll-interrupt, youtube-scroll-completion, youtube-bbb

`youtube-scroll-interrupt.txt` opens provider details, returns to results,
and sends Select, Triangle, and focus input while visible tiles are still
being prepared. Use a captured results/watch response set and the same
explicit emulator clock. Require the menu mark, both chrome-toggle response
receipts, a visible final focus, and clean completion. The scenario does not
play media or write account/session state.

`youtube-scroll-completion.txt` uses the same details/Back setup, but measures
scrolling down and back up without opening a menu during painting. Use it
alongside the interruption scenario: that scenario's completion interval
includes the deliberate menu pause and is not a raster-cost measurement.
Changed positions and final frames still publish immediately; subsequent
partial progress is coalesced for 32 ms while the same bounded slices keep
sampling input.

`youtube-bbb.txt` measures first visible scroll movement separately from
complete visible-tile publication. Partial publications use the same display
fence as complete frames; the next bounded raster slice samples input without
an additional entry-vblank wait. Raster time/unit limits are unchanged. Check
both timing fields: a fast checkerboard is not evidence of fast completion.

## Media player

### youtube-playlist-player-journey

`youtube-playlist-player-journey.txt` starts at a native playlist and checks
Play all, Next/Previous, the Queue menu, the Automatic next toggle, selection
of an upcoming entry, and return to the retained playlist. Its companion
predicate waits for actual playback after each selection. The host
`tilefinch-youtube-playlist-tests` also covers repeated IDs, local/service
page boundaries, bounded rewinds, stale selection refusal, allocation
refusals, and unchanged document scroll/focus during native activation.

### player-queue-shortcut

`player-queue-shortcut.txt` is a network/decoder-free qualification of the
same native queue input receiver and busy-player routing. Require
`tilefinch-queue-shortcut: outcome=pass checked-states=4` and clean exit.
It covers normal, resolving, buffering, and failed-player states, preserving
selection and excluding lower-quality retry while the modal is open. The
host UI tests additionally compare retained and uncached 208-pixel menus,
all five upcoming hit targets and their boundaries in landscape and portrait,
and both directions of the Square toggle.

### player-queue-profile

`player-queue-profile.txt` measures bounded batches of native menu preparation:
cache hits, focus, Square, tab switching, cold opening, closing, CPU fallback
copy, full rasterization, and cold/focus paths for the audio and subtitle tabs.
Run with `--measure` and require
`tilefinch-menu-profile: outcome=pass` plus clean exit. Its no-op blit excludes
GE submission and presentation waits, so these are not press-to-visible
latencies. Treat `cpu-valid=0` as unavailable thread CPU timing (as in PPSSPP),
not zero CPU cost. Real-font host checks compare partial and full redraws
pixel-for-pixel in every supported UI language; pending/refused glyphs retain
the full-redraw recovery path.

### youtube-chapter-player-journey

`youtube-chapter-player-journey.txt` checks a native provider detail page's
chapter start, two chapter selections, portrait seeking, an audio-track change,
caption selection, pause/resume, picker cancellation and return to the detail page. It needs
a video with chapters, multiple audio tracks and captions. Its readiness probe
uses `@media-playing`; require every wait to succeed, the expected seek/track
actions, a completed caption load, a clock that advances after resume and
picker cancellation, and a clean terminal
report. It has no screenshot commands or account/session writes. Live service
responses are not stable goldens; navigation-continuity assertions also run in
the host's synthetic provider journeys.

### youtube-player-return-journey

`youtube-player-return-journey.txt` extends this through Back to a local,
scrollable results fixture, including selected-filter preservation. Package
`tests/fixtures/player-journey-results.html` using the authenticated offline
app fixture writer, stage its library on host0, and start at its local app URL.
Boot URLs must remain HTTP(S); a raw host0 path is not a valid boot URL.
Use the fixture's tall spacer to require a nonzero saved scroll position.
Require the before/after marks
to identify `journey-video` with the same scroll and focus, and all six media
waits to succeed. The 360p source rotates directly into portrait crop. Captures
are made only on the page before and after playback, not during ME decoding.
The host search-video journey repeats the control sequence eight times and
compares the immediate compositor output for menu, seek, and track feedback;
this proves next-publication feedback, not physical-device latency.

### media-portrait-seek

`media-portrait-seek.txt` exercises clockwise playback using the same native
button receiver as physical input: forward seek, a 60-second rewind (decoder
reopen), preview/cancel, vertical-nub seek, paused seek and resume. Use a local
silent 90-second MP4 fixture, either a tall 240×426 picture or a 640×360
pillarboxed picture cropped to portrait. The USB-only offline-library fixture
route needs no HTTP server or Wi-Fi. Check every semantic media mark for
`clockwise=1`, the expected `crop` flag, `failed=0`, and completed seek state;
require script completion and clean teardown too.
Their `.until.js` companions use the native `@media-playing` predicate:
visible, playing, non-buffering media with a nonzero clock. This avoids sending
Select to a loading screen when decoder startup outlasts a fixed frame wait.
The ordinary bounded `until-live` exhaustion still applies; a timed-out setup
or a measurement mark reporting `clockwise=0` invalidates the comparison.

### media-portrait-cadence

`media-portrait-cadence.txt` alternates CPU / GE / CPU / GE strided texture
copies in one validation binary. `media-stage-cpu` and `media-stage-ge` change
only that copy, not decoding, rotation or framebuffer ownership. Quiet
`video-measure-start` / `video-measure-end` windows reset and freeze new-picture
scanout cadence and report stage/draw averages plus successful GE copies.
Per-loop detailed timing is disabled inside these video windows; per-picture
cadence and the existing copy/draw counters remain active.
Routine per-frame publication text is suppressed too, with explicit
`suppressed-loops` / `suppressed-publications` totals. Require
`tilefinch-video-timing: buffered=1` and the final timing-log verdict
`dropped=0 forced-flushes=0`; an incomplete or overflowing window is not a
qualified cadence capture. End the window after stopping playback, so draining
its log cannot create a measured hold. Keep startup and steady-play results
separate, and avoid full diagnostic marks during the measured interval.
These control marks do not consume image capture slots. Session counters are
sampled during presentation, not through a retained session pointer in the
input handler. Do not combine seeks, pauses, initialization or diagnostic
flushes with steady-play cadence. A successful GE probe additionally checks
every staged word and the final rendered pixels, including padded geometries.

### youtube-comments-play-live

`youtube-comments-play-live.txt` starts on a provider watch URL, expands
Comments with D-pad input, returns focus to the video card, and activates it.
It is a live diagnostic, not a network-dependent golden. Check that
`comments-link` targets `tilefinch_view=comments`, `comments-open` reports that
committed URL, and `play-target` identifies `class=watch-target` with the
canonical watch URL. The `played` and `progressed` marks alone do not prove
playback: require decoded frames and an advancing media clock in the media
log, with zero state-machine mismatches. PPSSPP can reach the known missing
`flash0:/kd/mpeg_vsh.prx` boundary; this qualifies the input/handoff only,
not firmware playback. Do not bless that failure as a playback golden.
The hermetic `--provider-navigation-only` host test independently covers
Play from both expanded Description and Comments documents, including the
canonical URL, native-player action, and unchanged backing generation.

### youtube-watch-live, youtube-results-live

These drive sustained playback and result activation through the PSPLink
loop ([PSPLink device development](PSPLINK_DEV_LOOP.md)). They have no device
golden because CDN and wireless behavior are external inputs.

### youtube-account-actions-live

`youtube-account-actions-live.txt` starts at a native provider details page
(a watch URL with `tilefinch_view=details`) with an explicitly authorized
test-account session, and applies and then undoes two reversible account
actions. Require the target IDs at each target mark, a confirmed account
state at each completion mark, unchanged page generation, and clean
teardown. Never run this mutation journey automatically on a personal
account. If an intermediate action fails, verify and undo any preceding
change before ending the test. Session transfer and all live evidence
remain private.

## Games

### treadline-action-budget

`treadline-action-budget.txt` exercises Deploy, all nub directions, face-button
aim, cannon, secondary, gadget, command, and the escape chord in an Onslaught
boss scene. Its companion requires successful gameplay and killcam history,
and disables game phase clocks during the displayed-frame measurement. Use the
USB-only installed-package route in [PSPLink dev loop](PSPLINK_DEV_LOOP.md),
with `qualification=input&bridge-profile=off&profile-report=off&seed=12345`
in the package source URL. The `configure`, `heavy-setup`, and `post-play`
capture slots cover the menu, heavy scene, and post-action scene. Keep attribution
runs separate: phase clocks add work
and must not be mistaken for ordinary gameplay timings.

For attribution, leave input profiling enabled after `webgl-measure-start`
and collect `__treadlineDebug.slowProfile()` at the end. Input qualification
retains up to 540 samples, covering the full button workload rather than only
its first 180 frames.
The game's profile row layout is defined by its own source. Run host
CPU-heavy gates separately from USB timing runs so host0 servicing is not a
confounder. `profile-report=off` suppresses the large formatted in-game
report; collect its bounded `__treadlineDebug.aiProfile()` totals in a mark
probe instead. A report-building pause is not a gameplay optimization target.

### treadline-offline-controls

`treadline-offline-controls.txt` enters through native Home
and Library and sends its authored `PspUiInput` through Page controls and the
standard Gamepad publication path. Validation reports the downstream evidence
as `tilefinch-input-gamepad:`; the PPSSPP runner requires every scripted face,
shoulder, D-pad, and analog extreme to appear there. Do not infer delivery
merely from `held-frames`. Seed the isolated emulator with the same sealed
library layout used on-device:

```sh
scripts/run-ppsspp-input-script.sh \
  --script treadline-offline-controls \
  --offline-library build-preset-psp-validation/offline \
  --heap-mb 7 --script-file-kb 512
```

The optional directory is copied into the run's disposable Memory Stick. It
must contain `library.bin` and the referenced payload files; the runner never
falls back to the network to manufacture a missing installed app.

### treadline-menu-focus-probe

`treadline-menu-focus-probe.txt` takes the same installed-package route to
check D-pad focus geometry in the menus: from the title it moves to Quick
Match, down through the setup screen's bottom row and back up to Deploy,
then deploys with Cross. Its marks (`deploy-focus`, `bottom-code-focus`,
`deploy-return-focus`, `deploy-result`) and its device golden
(`treadline-menu-focus-probe.device-golden.txt`) record that every move
lands where a person expects; run it with the same `--offline-library`
options as above. Menu focus timing has its own, longer scripted loop;
keep its measurements separately from the activation golden.
