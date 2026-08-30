# Basic view

Basic view is Tilefinch's bounded, reversible fallback for a page whose
server-rendered content is useful but whose application enhancement failed.
It is intentionally narrower than a second browser engine: the authored DOM
stays connected, while a compact native extraction supplies a predictable
presentation when the user chooses it.

Open **Menu → Page tools → Basic view** to switch between Raw and Basic. On a
page where scripts stopped before leaving useful actions, Tilefinch may offer
the same complete Basic extraction automatically after author work settles.

The extracted tree retains, in document order:

- headings, paragraphs, lists, links and common inline semantics;
- images, figures and captions;
- tables and code blocks;
- details/summary content;
- explicitly authored, labeled `GET` or search forms.

Basic view does not infer behavior from button text, classes, test IDs or
click-handler-shaped markup. `POST` forms, unowned buttons, disabled controls,
downloads and opaque application commands never become new actions. An
admitted search form is limited to authored text/search fields, bounded
select/textarea values, hidden GET parameters and explicit submit controls.
Multiple-selection controls are refused rather than collapsed to a different
single-value submission contract.
Inherited disabled state is honored when form wrappers are flattened, and
authored read-only fields remain read-only.

## Bounds and transactions

One preparation inspects at most 4,096 source nodes, emits at most 512 semantic
nodes and retains at most 256 KiB of escaped markup. It retains at most eight
forms and 32 controls. Scratch and clone ownership are charged to the page's
existing `Budget`.

The automatic admission seam builds one bounded temporary extraction and
commits that same buffer only after every completeness check passes. A node,
byte, form, control, select-option or anchor refusal installs nothing. A failed
DOM refresh removes the clone and its body marker before returning. Raw DOM,
form state and navigation history are never replaced.

Preparation and presentation are a two-phase transaction. Tilefinch first
retains the exact native root, then applies the Basic stylesheet. Only after
that stylesheet and relayout succeed does it activate the view and retire the
current page's author realms. This prevents delayed scripts from rewriting the
trusted-looking Basic links or forms, while a failed presentation leaves the
original raw page and JavaScript realm untouched. The configured JavaScript
policy is unchanged for the next navigation.

Preparation itself does not change pixels. Tilefinch offers Basic view only
after author work has settled, current-page script degradation is proven, and
the raw result is blank or has no positive-size action target. A useful raw
page therefore stays visible until the user explicitly switches views.

## Switching and fragments

The normal text-anchor handoff preserves the nearest source position when
switching between Raw and Basic. Extracted IDs and legacy named anchors use a
bounded `tilefinch-extracted-` prefix. Fragment URLs stay exactly as authored;
same-document navigation prefers the painted prefixed target while an
extracted view is active, then falls back to the raw source ID or legacy named
anchor. The raw source IDs remain unchanged for switching back.

Reader and Basic share one extracted-presentation slot. Tilefinch never keeps
two proportional clones of the same page. Reader remains the focused article
or listing view; Basic is the broader action-preserving fallback. Either view
can be turned off to return to Raw. To replace one extracted kind with the
other on the same document, reload first; this keeps clone ownership
proportional and prevents stale JavaScript node references.

Returning from Basic to Raw does not resurrect the retired author realm. Reload
the page to run its scripts again. This is intentional: Basic is the bounded,
script-free recovery surface, not an alternate live view of an application.

## Structured data

The first Basic milestone uses only explicit DOM semantics. It does not turn
arbitrary JSON `contentUrl`, `SearchAction`, product metadata or application
state into controls. A later structured-data extension should reuse the
bounded token scanner and admit only explicit Schema.org contexts after each
shape has a task-based fixture and a clear, non-guessing action contract.
