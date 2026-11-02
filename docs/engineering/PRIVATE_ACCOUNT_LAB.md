# Private account lab

Two host tools let a maintainer sign the interactive lab into a dedicated,
disposable test account without putting account values in chat, command
scripts, shell arguments or captures. Use them only with such an account,
never a main account. Build the release host lab first.

## Rules

- Never pass passwords, verification codes, session cookies or authenticated
  URLs as arguments, in command files, or in captures.
- Cookies and session data stay in RAM for the process. Nothing is copied
  from another browser or migrated to another process; the only export is the
  explicit `export-youtube-session` action described below.
- Raw page text, titles, payloads and screenshots stay on the local machine.
  Only the redacted `inspect` artifacts may be shared, with the account
  owner's permission.
- These host checks do not qualify the PSP sign-in journey.
- This is not a hardened credential vault: the browser and the intended site
  hold submitted values in memory, Python does not guarantee erasure of string
  copies, and OS swap and image-viewer caches are outside the tools'
  protection. After testing, sign out or revoke the test session from the
  account's security settings.

## Interactive launcher

```sh
python3 scripts/run-private-interactive-lab.py --url https://example.test/login
```

Run it yourself in a local interactive terminal. The launcher refuses piped
input, discards the child's stdout and stderr, removes inherited
tracing/cache settings, disables core dumps, automatic screenshots, HTTP
captures and persistent bytecode caching, and uses a private temporary
directory. Its native status pipe carries numeric page/focus/task information
and the parsed top-level origin, never DOM text, URL paths or query
parameters. Check that origin before entering a value; non-HTTPS credential
entry is refused.

| Command | Effect |
| --- | --- |
| `view` | open a temporary 480×272 image of the page |
| `focus-id ID`, `focus-next`, `tap X Y` | focus a field |
| `secret` | replace the focused field with locally masked input (spaces and Unicode preserved) |
| `activate` | press the focused button |
| `pump 32 16` | advance ordinary browser turns, including background resources |
| `tick` | advance runtime/media only; for isolated execution experiments, not a live sign-in |
| `signin` | run the scripted sign-in flow (below) |
| `quit` | tear down the browser and delete the temporary files |

Ordinary `type`, script evaluation and diagnostic commands are unavailable.
An abrupt process failure may leave the temporary directory behind; delete it
by hand.

`signin` (or `--signin` at launch) drives the sign-in flow of the one
identity provider the tool supports (its accounts origin is fixed in the
script). On macOS it opens native masked dialogs, fills only unambiguous
rendered username/password fields on that exact HTTPS origin, submits each
step once through normal focus and activation, and advances page work. It
pauses on a missing password step, a changed origin or a refused action. It
does not solve verification prompts or claim authentication succeeded.

## Sign-in keeper

```sh
python3 scripts/run-private-signin-session.py [--service youtube]
```

The keeper retains the account values in RAM across browser restarts, so
repeated sign-in debugging needs one entry. It starts the same private
browser and attempts the sign-in flow once; further attempts are separate
control actions, never an automatic retry loop. `--service youtube` enters
through that provider's sign-in page instead of the account page.

A test driver controls a running keeper through its owner-only socket, which
the keeper prints:

```sh
python3 scripts/run-private-signin-session.py \
  --control /path/printed/by/keeper/control.sock --action status
```

Accepted actions (the `ACTIONS` set in the script):

- `status`, `attempt`, `restart`, `advance`, `view`, `view-below`,
  `view-above`, `inspect` and `stop`;
- fixed native provider pages: `youtube-home`, `youtube-subscriptions`,
  `youtube-watch-later`, `youtube-playlists` and `youtube-test-video`;
- provider account actions for scenario testing: `youtube-like`,
  `youtube-unlike`, `youtube-watch-later-add`, `youtube-watch-later-remove`,
  `youtube-subscribe`, `youtube-unsubscribe`, `youtube-signout`,
  `youtube-account` and `youtube-refresh`;
- `export-youtube-session`, for an explicitly authorized USB hardware test:
  it writes `youtube-device-session.bin` into the keeper's owner-only working
  directory with the shipping login serializer (no password, no other site's
  cookies) and returns only whether it was accepted.

Status carries a fixed phase name,
numeric page/focus/task information and the origin, never form values,
cookies, DOM text, URL paths or error strings. Actions run serially. `view`
opens an image on the local machine and returns nothing over the socket. No
network listener is opened. The socket is not a security boundary against
other processes of the same OS user.

| Option | Effect |
| --- | --- |
| `--password-attempt-limit N` | possible password submissions per keeper session (default 3; `0` removes the cap) |
| `--confirm-first-attempt` | local confirmation dialog before the first attempt |
| `--confirm-each-attempt` | local confirmation dialog (Cancel by default) before every attempt |
| `--outcome-classifier PATH` | copy an owner-only diagnostic filter into the session (below) |

The keeper counts a possible password submission before filling the password
field, including failed or uncertain delivery. Repeated failures can lock the
account: inspect the result and change the implementation before retrying an
unchanged failure. `restart` discards cookies and images, not the retained
account values; Ctrl-C or `stop` terminates the browser, clears the retained
values and deletes the temporary directory.

After sign-in, the provider-page actions open native provider pages in the
same session. An authenticated feed is published only
after the provider API confirms signed-in status. The synthetic gate for that
UI is `build-preset-release/tilefinch-browser-engine-tests --youtube-home-only`,
which needs no account or external request.

## Redacted inspection

`inspect` writes `redacted.png` and `redacted.ppm.txt` beside the socket.
Wait for `busy=false` and `inspection_ready=true` before reading them. Views
capture the rendered page vertically (up to 4,352 pixels); the diagnostic copy
removes text belonging to input, textarea and editable controls before
painting or writing files, and matching field values in other text are
removed too. The text report gives captured and complete page heights, fixed
runtime counters and exception categories, response lengths, scheduler
flags, navigation status counters, and opt-in RAM-only XHR, promise-outcome
and `JSON.parse` histograms. None of these retain URLs, bodies, headers,
field values, exception messages or stacks. The files are owner-only, outside
the repository, and removed when the keeper exits. This redaction is a
diagnostic for a test account, not a guarantee against a page that reflects
credentials through images or fragmented text.

`--outcome-classifier PATH` copies an owner-only (0600), non-symlink function
expression of at most 16 KiB into the session. It receives
`(value, originalParse, event)` and may report only numeric categories 0–12
(or 13-bit stage masks for snapshot events 2 and 3); invalid returns and
exceptions become "unrecognized" and never reach the page or the diagnostics.
It must contain diagnostic code only, never account inputs, and is selected
only on request.
