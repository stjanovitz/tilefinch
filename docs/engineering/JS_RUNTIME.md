# JavaScript runtime

This page is for engineers changing the JavaScript runtime: the realms a page
can own, the task scheduler and page clock, the input events pages receive,
how platform objects and DOM nodes keep their identity, computed-style
retention, and the representations the vendored engine adds. The rules are
summarized in [Architecture](../ARCHITECTURE.md#javascript); this page holds
the mechanism. Script admission and source memory are in
[Script memory](SCRIPT_MEMORY.md), and the engine's carried changes are listed
in [Vendored QuickJS](../../third_party/quickjs/README.md).

## Bootstrap

The committed bootstrap bytecode is stripped of source text, line tables, and
local-variable names, which QuickJS would otherwise copy into every realm's
heap; the generated manifest and the regeneration rule are in
[src/bootstrap/README.md](../../src/bootstrap/README.md#regenerating).
`TILEFINCH_TRACE_JS_STARTUP=1` prints the heap each module's evaluation
retained, by QuickJS category, so growth is attributed before the footprint
guard test trips.

## Realms

### Shared workers

`SharedWorker` exists so feature detection sees a current browser, and every
construction throws a `NotSupportedError` `DOMException` before allocating
anything. Some sites read a missing `window.SharedWorker` as an old browser
and load much larger legacy bundles; sites that really use shared workers
catch the throw and take their fallback, as in browsers without it. The shell
is defined eagerly in `compat.js`, not behind the lazy Worker group, so a page
that only probes the constructor does not pay for that group. Like the
platform, the interface is exposed to windows only.

### Dedicated workers

A Web Worker is concurrency in the page's programming model, not in the
engine: `postMessage` crosses a structured-clone boundary into another
context, and the worker's turn runs when the single event loop reaches its
task. Its global inherits from `DedicatedWorkerGlobalScope.prototype`; names
it does not define resolve, read-only, through a fallback to the owner's
platform implementations, so each realm stays small instead of instantiating
the platform again. Values a worker API returns are created in the worker's
realm, so `instanceof` and brand checks hold there.

### Loaded child documents

A loaded child document is parsed, styled, laid out, and scripted as its own
document (`SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME`) and shown in its parent as an
opaque pixel snapshot at the iframe's box; the parent never reads the child's
DOM. A child mutation marks the snapshot dirty for the owner tick to refresh
(retrying after a refused allocation), and trusted input in a child notifies
user activation on that realm and every ancestor before dispatch. A frame
whose element has no box in its parent's complete layout (`display:none`, on
it or an ancestor) is laid out at its default size, so its script sees the
same viewport, but no pixels are drawn; the owner tick draws it once the
element has a box. A child realm returns its allocator's cached free blocks to
the page after its bootstrap and again once its document has loaded, and, like
every realm, allocates its mutation journal only at its first connected
mutation.

### Same-origin frames and WindowProxy

An `<iframe>` that is not connected has no content navigable:
`contentWindow` and `contentDocument` are null until it is inserted. Every
connected `<iframe>` and `<frame>` is a child navigable, loaded or not:
`window.length`, `window[i]`, and `window[name]` answer the same WindowProxy
`contentWindow` returns, but a WindowProxy and its realm are created only
when read, so hidden frames no script touches (such as consent-management
locator frames) cost no realm.

A same-origin frame evaluates in a persistent QuickJS context whose global
`this` is the child WindowProxy, and QuickJS itself owns global and lexical
bindings, so no source-scanning or declaration-copying pass runs per
evaluation. The realm's own `eval` is the frame's `window.eval` for every
caller: in frame code `eval(source)` is a direct eval that sees the calling
function's bindings, and a parent's `contentWindow.eval(source)` is an
indirect eval in the frame realm under the frame's dynamic-code policy
(reading it prepares the realm's intrinsics).

The frame's global inherits the embedding page's global names from a native
object (`frame_realm_inherit_page_global` in
`src/js_runtime/host_primitives.inc`), not a Proxy, so a name neither global
defines is unresolvable: reading it throws a `ReferenceError` and it is absent
from `in`. The WindowProxy reports the page window's names, as taken when
author code starts, as the frame's own (`Object.getOwnPropertyNames`,
descriptors); they are the page's objects, not per-frame copies. The installer
lives in `frames.js`, which `platform.js` calls with its private helpers and
deletes.

### Retirement

Frames and workers share one retirement boundary. A failed setup or
construction releases its quota slot; eviction or termination retires the
realm before releasing its native reference, so retained functions keep their
storage alive but cannot run, pending jobs are discarded during normal bounded
draining, settled fetches remove their abort listeners, and microtasks and
delayed import retries check that the worker is still active.

## Scheduler and page clock

*Timers* (`setTimeout` and `setInterval`, in pages and workers alike) run the
HTML timer algorithm, with a nesting level inherited from the scheduling
timer and the 4 ms clamp beyond the fifth level, for intervals and mixed
chains too. *Platform tasks* (IndexedDB steps, promise-returning platform
operations, performance-observer delivery, worker and `MessagePort` messages)
share the bounded queue and quota but never enter the timer algorithm, so a
page's timer chain can neither delay browser bookkeeping nor be reset by it.

Each runtime advance moves the timer and animation-frame clock by the real
time since the previous advance, at most one 16 ms tick, so a faster loop
cannot run timers early and a long frame lets them fall behind instead of
bunching. Frames still begin with the vblank wait even when page work is
runnable, because replacing it with a short yield starves the lower-priority
I/O threads on the device; that yield is only an opt-in validation setting
(boot key `page_task_yield=1`, read by `psp_loop_page_work_waiting` in
`src/psp_script_main.c`).

### Animation frames and visibility

Animation-frame callbacks run at most once per presented tick and skip
elapsed frames rather than replaying a backlog. Native surfaces, the media
player, and suspend drive Page Visibility at a task checkpoint, preserving a
suspend/resume pair even when both edges arrive before JavaScript resumes.

### Message ports

A `MessagePort` delivers one message per task through the listener microtask
checkpoint and schedules the next only after that one finishes, which keeps an
endpoint's messages in order even when a listener posts back synchronously.
Author-dispatched events remain synchronous.

## Input events

Page input events describe the press the user made, in browser order, and are
all trusted. `src/bootstrap/scheduler.js` builds these sequences;
`src/controller.c` orders them around the native defaults.

- **X on a highlighted element** is the Enter key of a TV or console browser:
  `keydown` and `keypress` (key `Enter`, keyCode 13) at the focused element,
  then the keyboard-style `click` (a `PointerEvent` with `pointerId` -1, an
  empty `pointerType` and `detail` 0, no coordinates), then `keyup` after the
  press's defaults such as a form's `submit`. A cancelled `keydown` or
  `keypress` cancels the activation. A highlighted element that cannot take
  focus receives the click, and the keys go to the body rather than to a field
  focused earlier.
- **Enter in a text field** (Start, or the keyboard's Done on a form's only
  field) is the same press around implicit submission; outside a form it is
  the keys alone.
- **X with the analog cursor showing** is a mouse button instead:
  `pointerdown`/`mousedown` on the press and `pointerup`/`mouseup` on the
  release at the cursor (`pointerType` `mouse`), then the pointer's `click`
  with `detail` 1, and no key events.
- **On-screen keyboard text** arrives as a phone keyboard's does. The keyboard
  session's Done commits one composition: `keydown` (key `Unidentified`,
  keyCode 229), `compositionstart`, `beforeinput` (`insertCompositionText`,
  `isComposing`, not cancelable), `compositionupdate`, the change,
  `compositionend`, `input`, `keyup`. Done with the text unchanged fires
  nothing.
- **Desktop lab text** is a plain commit (keydown 229, `textInput`, cancelable
  `beforeinput` `insertText`, `input`, keyup), and Backspace is its own key
  (keyCode 8, `deleteContentBackward`; its keydown can cancel the deletion).

Each edit leaves the caret after the text and fires a trusted
`selectionchange`, bubbling from an `input` or `textarea` and at the document
for contenteditable.

## Gamepad polling

Identical physical samples do not cross into QuickJS, and the Gamepad API's
button and axis objects are reused across polls; only the sequence
`getGamepads()` returns is fresh, as Web IDL requires
(`src/bootstrap/platform.js`).

## User activation

Trusted input opens a five-second transient window
(`SCRIPT_TRANSIENT_USER_ACTIVATION_MS`) and sets a sticky "has been active"
bit, both exposed through `navigator.userActivation`. Activation-gated native
operations (fullscreen, resuming game audio, page controls, starting
multiplayer) ask the realm whether the window is open, and fullscreen consumes
it. A script-initiated navigation snapshots the state when it is requested,
because the host may act on it a turn later; that snapshot, not the presence
of a referrer, is what request context and Fetch Metadata report as
user-activated.

## Platform object identity

### Web IDL receiver checks

Web IDL makes every operation and attribute throw a `TypeError` for a
receiver that does not implement the interface, and pages (sign-in security
checks among them) call prototype members on `{}` to tell a browser's own
functions from patched ones. One check is not written per member. The engine
holds two object flags for this: after the eager bootstrap, and before any
page script, the host marks the prototype of every constructor on the global
as an interface prototype and each function among its members as an interface
member (`runtime_mark_platform_interfaces` in
`src/js_runtime/runtime_creation.inc`, QuickJS `JS_MarkPlatformInterfaces`).
Calling a marked member with a receiver that has no interface prototype on its
prototype chain, or that is one, throws "Illegal invocation" before the member
runs. The same pass gives each interface prototype its class string
(`@@toStringTag`).

What stays as the bootstrap wrote it: promise-returning operations, which
reject instead; the `on*` handler attributes of Window, Document and the
element interfaces, which share their accessors and answer as
`[LegacyLenientThis]` does; and a value iterator's `keys`, `values` and
`entries`. The check accepts an object that merely inherits from an interface
prototype (`Object.create(Node.prototype)`), and a Proxy; the members' own
private checks still apply there.

A lazy module's interfaces and members are marked when it loads, and only
what the module itself defined: before it runs, the host snapshots the
global's constructors and the functions on the marked prototypes
(`JS_SnapshotPlatformInterfaces`), and afterwards marks only what is new
(`runtime_platform_window_open` and `_close`). A page's class on the global,
or a method or native a page put on an interface
(`NodeList.prototype.each = Array.prototype.map`), was there before and stays
as it is, however it was compiled: a large ES module and restored bytecode
keep no source text, so a function's form says nothing about its owner. A
module whose interfaces a factory builds after it loads (the CSSOM rules) has
its ensure native run that step in the same window.

### Interface snapshot

When a realm's start-up script ends, `runtime_snapshot_interfaces`
(`src/js_runtime/runtime_creation.inc`) has the engine mark the interfaces
as they are: every class prototype, every function the global object
holds with its prototype and their methods, and the namespace objects
`Math`, `JSON`, `Reflect`, `Atomics`, `Intl`, `WebAssembly`, `CSS` and
`console` with the constructors they hold and those constructors'
prototypes. A namespace a lazy module publishes (`Intl`) is marked when that
module loads, read as the browser's own binding of the name
(`JS_GetBrowserGlobalProperty`); a page's object under one of those names
(`console = logger`, or an `Intl` the page bound before the module loaded)
is the page's and stays unmarked. Marking sets one bit per object
and allocates nothing. From then on:

- The first change page code makes to a marked object first copies that
  object's own properties and prototype to a private twin. The page keeps
  using the object it changed, so its patches and polyfills keep working.
- Browser code continues its lookups in the twin: property reads and method
  calls, the iteration protocol behind `for…of`, spread and destructuring,
  inherited setters and read-only checks, `in`, `for…in` and own-property
  reflection.
- A change browser code makes later (a lazy module adding members) reaches
  the object and its twin, and a function it publishes is marked too. What
  the page has changed since is the exception: the page keeps what it
  made, and the browser's change is made in the twin alone
  (`js_snap_page_changed`). That covers a member the page changed, added or
  deleted (where the twin holds an accessor, the browser's assignment calls
  it on the object), the prototype the page replaced, and new members and
  a new prototype of an object the page made non-extensible (sealed or
  frozen): the twin keeps the browser's own extensibility. So a module that
  replaces its own forwarding member does not take a page's wrapper of it
  away, and a member or an object the page made read-only does not refuse
  the module. A change is applied to the object only after the object's
  own checks pass, so a refused one leaves both views as they were: a
  refused change is never followed into the twin. A prototype change is
  refused when it would close a cycle in the page's chain or in the chain
  browser code walks, which continues from twins.
- A lazy placeholder passes a read on to the module's member in the
  browser's view, for the same reason. An assignment through it is made as
  that member answers it, in the view of the code that assigned: the
  browser's accessor there is called with the original receiver, and
  otherwise the value is defined on the receiver, as an assignment over a
  writable data member defines it. The member is read in the browser's
  view: a prototype's twin, or for a global the browser's own binding. A
  page's assignment through a placeholder, as its first touch or with a
  receiver it chose (`setter.call(Object.prototype, f)`), is therefore the
  page's change alone, and a page's wrapper of the placeholder that calls
  it back ends there, with the snapshot off too: while an assignment
  through a placeholder calls a setter, one that comes back to it takes the
  member as a writable data property.

The global bindings follow the same rule. A global variable that holds an
object when the snapshot is taken (`Map`, `String`, `Math`, `document`,
`fetch`), or that the bootstrap defines later, is pinned. When page code
first names a pinned global, assigns it, declares it (`var`, `function`,
`let`, `class`), redefines it or deletes it, the page gets a variable
reference of its own with the same value, and the reference every bootstrap
function already holds stays the browser's. So `new Map()` or
`globalThis.Map` inside the bootstrap is the browser's `Map` whatever the
page put under that name, and the page's own functions see the page's. A
bootstrap function that names a global which is not a variable yet (a
class a lazy module defines on first use) waits on a reference no page
function shares, and that reference becomes the variable when the
bootstrap defines it. An accessor global the browser defined (a lazy
placeholder, a named frame or element id) is kept the same way when page
code redefines or deletes it: browser code that names it, or reads it from
the global object, still reaches the browser's accessor, so a lazy module
still loads for the browser's closures and they build the browser's
objects. A page's assignment through such an accessor runs its setter and
rebinds nothing.

A browser change to a global the page has changed since is made in the
browser's view alone, as for a member (`js_snap_global_page_changed`): the
page keeps a binding it replaced, redefined or wrapped, a name it deleted
stays deleted, and the browser's closures get the new binding. That covers
a module load removing its placeholders and binding its names. A global
the page only named, or set back to the same value, is unchanged, and a
browser change to it reaches both views. A page's setter is never handed
the browser's value, and the page's binding is never copied into the
browser's view. Between a removal and the next binding the name is unbound
in the browser's view (it reads `undefined`), whatever the page's binding
holds. Only a removal unbinds a name: a shared global the browser sets to
`undefined` stays shared, and the browser's next binding of it reaches
both views. A lazy module still cannot load on a global object the page made
non-extensible, as without the snapshot: the names of its that the page
left alone are removed and cannot be added back.

Three kinds of global stay shared window state, read live by both:
accessor globals the page defined, variables the page created, and
variables that held a primitive or nothing at the snapshot (`name`, a
handler slot set to `null`). Own-property reflection on the global object,
and `in` on it, are always live: they report the page's bindings, also for
a name the browser's view has unbound.

A browser script that runs after the snapshot declares nothing at its top
level (`var`, `function`, `let`, `const`, `class`): the engine refuses it.
Lazy modules and host snippets are a single function expression or call
and publish through `globalThis`.

Browser code is bytecode read with `JS_READ_OBJ_PLATFORM_METHODS`, the
host-only source fallback for it, a worker realm's platform initializer,
and the script snippets the host compiles for its own use (frame
messaging, media updates, viewport resize). Code compiled at run time for
the page (`eval`, `new Function`, an event handler attribute) is page code.
Whose code is running is decided by the nearest frame: a bytecode frame by
its function, a host native installed through `js_rt_install_native` as
browser code (behind the JS profiler's trampoline too), and an engine
native by its caller, so `array.map(f)` called
by the bootstrap reads `constructor` and `Symbol.species` from the twin and
the same native called by the page reads the page's. Loading a module on
first use is bracketed as browser work (`JS_EnterBrowserCode`) even when a
page's read started it; the bracket holds with no frame below it too (a
host native behind the profiler's trampoline called as a promise
handler). Host code that runs outside any script is browser
code; a host instrument that must read or replace what the page sees asks
for the page's view with `JS_SetEmbedderPageView`, and a native a host test
may replace is left to the page with `JS_LeaveGlobalToPage`.

A promise job runs with no frame below it, so what it does outside the
functions it calls (resolving the promise a reaction settles with the
handler's result, the rest of an await's resumption) acts for the code the
job runs for, not for the host that drains the queue
(`js_snap_enter_job`). An await resumes as its async function (as the
code that asked, should the function have no code of its own left). Any
other
job is the page's when its handler, or a thenable's `then`, is page code or
when page code asked for it (called `then`, or resolved the promise with a
thenable), and the browser's only when both are the browser's. In the same
way, what an async function or async generator does outside its frame
after a segment runs (setting up an await, settling its promise, resolving
a request) acts for that function, whoever called it. So a page's patch of
`Promise.prototype.then` (zone.js) is called as often as it is without the
snapshot, also for a page async function an event listener or a timer
calls, and the browser's own promise chains and async functions (the page
calling `ReadableStream.prototype.pipeTo`), and the host's promise work
outside any script, read the browser's view. With the snapshot off none of
this is recorded: a queued job carries no view, and only the one byte in
each pending reaction record remains.

Twins and the page's own variable references are page heap, charged like
any other object, and a refused one fails the page's write or declaration
with an ordinary out-of-memory exception. A page that changes no interface
pays one bit test per object in a property read or write, and about 40
bytes for each pinned global it names.

A function the page supplies does not stand in for a platform member
either. When browser code reads `element.remove`, `this.getAttribute` or an
internal method and finds a function the page put on that element, or
defined in its own subclass (a custom element overriding `focus`), the
lookup continues to the member the browser defined further up the
prototype chain. The page's own call of the same name gets the page's
function. This applies only where the browser defines the name: a method an
interface lacks (`connectedCallback`, `handleEvent`) is the page's, and so
are `toString`, `valueOf`, `then` and the other names only ECMAScript's
prototypes define.

What the snapshot does not cover:

- data properties of plain objects the global object holds (`document`,
  `navigator`) and of elements: a page's changes to those are the page's
  state;
- a non-function value a page defines over a platform accessor on one
  object (`Object.defineProperty(element, 'id', {value: 'x'})`);
- values a page hands to the browser (objects with getters, proxies);
- a non-function value a page steers a bootstrap function into writing
  on a platform interface (see below).

A change browser code makes to a marked object is normally the browser's
own. One kind is not: a bootstrap function that writes a property the page
named on an object the page led it to. The bootstrap was read for every
define, assignment, deletion and prototype change (about 700 sites):

- nearly all run while a module is set up, on objects and names the
  bootstrap chose;
- the ones that take the page's names write on an object the browser just
  made or owns per instance (a structured clone, an event, a
  `ValidityState`, the storage, dataset and form proxies), or on a frame's
  scope or a worker's global through their proxies, never on an interface;
- one walked out of its object: IndexedDB injected a generated key along
  the store's key path with inherited reads, so the path
  `__proto__.name` defined `name` on `Object.prototype`. It takes own
  properties only, as the specification does.

To hold that class as a whole, a change browser code makes to an
ECMAScript prototype (`Object.prototype`, `Array.prototype`, an iterator
prototype) counts as the browser's own only while a module loads, made by
the module's own code, or when the host makes it outside any script.
Browser code that page code called during the load (a page accessor or
patched method the module reached) does not count: author frames between
it and the load's bracket make its change a steered one
(`js_snap_author_in_view`). Any other is copied on like a page's
and stays out of the browser's view: the bootstrap has no late changes of
its own to make there. Platform interfaces are not held to this, because
the bootstrap completes some of them late (an event interface's prototype
on its first event, the shared layer prototypes, a shadow root's), so a
bootstrap function steered into writing a page's name on one of those
would still change the browser's view; none was found.

Two rules follow from the console and EventTarget being interfaces:

- The browser's own warnings go to the browser's `console.warn`, not to a
  page's replacement of it; host tests read them through
  `__tilefinchTestConsoleSink(level, ...values)`, which host and validation
  realms hand everything the console receives.
- A Worker, a script-made document (XML included) and its nodes, and a
  detached container define no `addEventListener` or
  `removeEventListener` of their own: they inherit
  `EventTarget.prototype`'s, as in other browsers, so a page's patch of
  those methods sees each of the page's calls because the page calls the
  patched method itself.

The snapshot is engine code, so a change to it changes the engine ID and
ships only with a PGO profile retrained on that engine (see Build below).

Host and validation realms have one seam for tests that must inject a
fault into the browser's script or count its work, which a page's patch can
no longer do: `__tilefinchTestBrowserPatch(target, key, value)` makes the
change as the browser's own and makes the function it is given browser
code. Shipping builds compile it out.

`boot.cfg`'s `interface_snapshot=0` (through
`TILEFINCH_JS_INTERFACE_SNAPSHOT`) and
`ScriptRuntimeOptions.interface_snapshot_disabled` build realms without it,
for comparing the two on one binary and for host tests that inject a fault
by replacing a built-in. `tilefinch-quickjs-interface-snapshot-tests` covers
the engine mechanism and `tilefinch-interface-snapshot-tests` the real
bootstrap.

### WeakMap cycles and namespace mirrors

WeakMap values must not retain their own keys, because this collector cannot
reclaim that ephemeron cycle. Attr and NamedNodeMap caches and generic
EventTarget listener tables therefore live in a private field on their owning
node or target, so dropping the whole graph permits ordinary cycle
collection. Namespace mirrors are the exception. `setAttributeNS`, and
`setAttribute` on a non-HTML element, record the attribute's namespace,
prefix and qualified name as written in a WeakMap keyed by the element,
because the native attribute is written through by qualified name alone and
keeps no namespace or prefix. Each record names its element, so an element
with a nonempty mirror keeps its wrapper until the realm ends; storing that
data natively would remove the cycle.

## DOM handle table

A live connected node keeps its slot even without a wrapper, because
bootstrap state (listeners, observers) is keyed by handle. Such a wrapper can
die and be remade, unless it carries an expando or is a WeakMap key: each
collection holds those of connected nodes, so script reaches that state
through the element. Nothing is held between collections.

The sizes are defined, with their per-slot cost, beside
`SCRIPT_DOM_HANDLE_SLOT_INITIAL_TOP_LEVEL` in `include/tilefinch/js_runtime.h`.
A top-level document's table starts at 1,024 slots and a child frame's at 256,
and the table doubles when every slot is live. The strict profile stops at
8,192 slots. The realistic profile grows whenever the Budget allows up to
16,352 slots, then in 4,096-slot steps that are each admitted only while the
page keeps the script presentation reserve free, up to the index's hard bound
of 65,504. Below 8,192 a full table grows at once, so no page collects garbage
for handles earlier than it would with a table of that size; from 8,192 dead
wrappers are reclaimed first and only a table that is genuinely live grows.
Growth is Budget-admitted like any page allocation, and a refusal is the same
clean `RangeError` a full table reports.

## Computed-style retention

Values are serialized from `ComputedStyle` fields, never from authored text,
so `inherit`, `var()`, relative units, and `!important` are settled first;
geometry properties are used values against the real containing block, and
where the cascade keeps less than CSS allows, the serialization is exact for
what is kept, which is also what is painted. A checked-in dump of every
registered property across a fixture page
(`tests/fixtures/computed-style-dump.*`) turns any change into a reviewable
diff.

Resolved styles are retained between reads and between entries into
JavaScript, keyed on `ComputedStyleInputs` (`src/js_runtime_internal.h`).
Script's own DOM writes drop only the subtrees they can restyle; every other
change that can alter a cascade (native DOM writes, parser input, stylesheet
and viewport changes, the colour scheme, container geometry, the fullscreen
element) moves the host style generation (`document_style_generation()`) or a
keyed input, which empties the cache. Removed nodes are evicted, so a
destroyed node's address cannot answer for a new one, and used geometry is
never retained.

## Engine representations

### Arenas and the QuickJS pool

QuickJS carves every object of up to 504 bytes (objects, shapes, property
arrays, strings, closures, var refs) from its own 4 KiB arenas
(`JS_MALLOC_ARENA_SIZE` and `JS_MALLOC_MAX_SMALL_SIZE` in
`third_party/quickjs/quickjs.c`), whose 8-byte block header is also the
reference count and GC tag. The QuickJS pool (`budget_quickjs_pool_*`,
`include/tilefinch/budget_quickjs.h`) charges only those arenas and the
engine's larger blocks to the Budget as `BUDGET_CATEGORY_JAVASCRIPT`, so a
per-object allocator change belongs in the engine's arena code, not in the
pool.

### Lazy functions

An inner function of a page script or module whose source is at least 128
bytes gets no bytecode when its script compiles; its body is compiled on first
call. The validating preparser follows the real parser's token decisions and
early-error rules, so a script with a syntax error anywhere is still rejected
before any of it runs, and it records which enclosing variables the body may
capture. Anything it does not model falls back to the full parser, and a
first-call compile whose captures disagree with that record fails loudly
rather than reading the wrong binding.

### Compact arrays

A dense `Array` built only from one-character Latin-1 strings, bytes, or
signed 16-bit integers is stored packed in a string-shaped backing store, one
or two bytes per element instead of a boxed value, which keeps a decoder's or
bytecode interpreter's working arrays inside a PSP script heap. The
representation is invisible to the language, and its invariants are the ones
every packed representation here must keep:

- the backing store belongs to the array alone: anything that would hand it
  to JavaScript (`join('')`) returns a copy, and mutation is copy-on-write
  whenever the store is shared or interned;
- a value the representation cannot hold widens it in place or converts the
  array to boxed elements, checked first against the device-profile array
  ceiling, since packed storage admits up to eight times more elements under
  the same byte cap;
- re-packing is only an optimization, and a refused allocation leaves the
  boxed array and the append untouched.

### Build

The PSP image is optimized for size, except QuickJS, which is built at `-O2`
because the larger engine is faster on the device despite the instruction
cache (`cmake/TilefinchDependencies.cmake`). Release builds add
profile-guided optimization; training, the fingerprint that refuses a stale
profile, and the release choice are in
[Release process](../RELEASE_PROCESS.md) and
[Validation records](VALIDATION_RECORDS.md#engine-profile-collection-pgo-under-ppsspp).
