# Challenge lab probes

Hand-run probes behind
[Managed challenges as a standards qualification](../../../docs/engineering/LAB_USAGE.md#managed-challenges-as-a-standards-qualification).
No automated test runs them: the live ones depend on a third-party service
whose program changes, and that section explains why its program and answers
must never become fixtures. What lives here is only our side: pages that report
which standard Web APIs Tilefinch exposes, and command scripts that drive the
interactive lab through a live observation.

Run a page with the host lab. A command script prints each `js` result as a
`loop-js ok=yes value=...` line and ends with the lab's `status` report; a page
without one shows its answer in its `<pre id="result">` (use `--output FILE`
to capture the frame):

    build-preset-release/psp-browser-interactive-lab \
        --fixture tests/fixtures/challenge-lab/challenge-api-probe.html \
        --commands tests/fixtures/challenge-lab/challenge-api-probe.commands

    build-preset-release/psp-browser-interactive-lab \
        --url https://site.example/ \
        --commands tests/fixtures/challenge-lab/challenge-live-wait.commands

## Offline API probes

| File | Reports |
|---|---|
| `challenge-api-probe.html` + `.commands` | The shape of `performance`, `screen`, `navigator`, `document`, `MessageEvent`, `PerformanceObserver` and `Worker` as a page sees them, collected into `challengeProbe`; the command script prints each group as JSON. |
| `challenge-native-api-probe.html` | Whether `CompressionStream`, `DecompressionStream`, `WebAssembly` and the Streams constructors exist, and which compression formats are accepted. |
| `challenge-compression-probe.html` | A Compression Streams round trip. |
| `challenge-wasm-api-probe.html` | `WebAssembly.validate/compile/instantiate`, synchronous `Module`, and `CompileError` on an invalid module. Prints `WASM-API-OK`. |

## Live observations (network required)

| File | Use |
|---|---|
| `turnstile-standards-probe.html` | Implicit widget rendering with the service's published always-pass test key; shows `passed:<token>` or `failed:<code>`. |
| `turnstile-interactive.html` | Explicit rendering with the published force-interactive test key; shows `complete` or `error:<code>`. |
| `challenge-live-wait.commands` | With `--url`: let a managed challenge run for 600 ticks, then print status. |
| `challenge-live-activate.commands` | The same, after focusing and activating the first control. |
| `challenge-reload.commands` | Wait, print status, reload, print status again: shows whether clearance survives a reload. |

These record what happened. They must not be extended to patch the challenge,
synthesize a response, or make Tilefinch present itself as another browser.
