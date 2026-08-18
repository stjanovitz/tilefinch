# URL, request, and cookie security model

This is the current bounded policy for the engine laboratory. It is designed to
fail closed where the project lacks the data or algorithms needed for a full
browser decision. It is not a claim of WHATWG URL, Fetch, cookie, CSP, or
browser-security conformance.

## Security defaults and compatibility controls

Tilefinch uses HTTPS-first top-level navigation and never silently falls back
to HTTP after a failed upgrade. HTTPS pages block active mixed content;
eligible HTTP images and media are upgraded once and then blocked if HTTPS
fails. Public pages cannot reach loopback, link-local, or private-network
peers, while a user may still enter a local address as a top-level page. A
bounded session HSTS table remembers verified HTTPS policy without writing to
the Memory Stick, and ordinary unpartitioned third-party cookies are blocked
by default.

Some old sites need exceptions. Mixed-content and third-party-cookie grants
are explicit, bounded per-site compatibility controls; mixed-content grants
expire when Tilefinch exits. JavaScript can be disabled globally or for the
current site. These controls improve compatibility and recovery but do not
turn the shared process into a security sandbox. The sections below define the
precise request, redirect, cookie, CSP, SRI, iframe, cache-provenance, and
exception rules, along with the boundaries Tilefinch does not claim to cross.

## Canonical HTTP(S) URLs

`TilefinchUrl` is the shared parser for absolute HTTP and HTTPS page URLs. It
provides one basis for origin comparison, URL resolution, downgrade checks,
history/network normalization, and cache keys.

The canonical form lowercases scheme and host, removes a default port, resolves
dot segments, and supplies `/` for an empty path. A request/cache key also
removes the fragment because fragments are not sent in an HTTP request.
User-information is rejected instead of being silently converted into page
identity or transport credentials.

The engine nevertheless retains the normalized logical request URL, including
its fragment, separately from that fragmentless wire key. HTTP trace v9+
records both identities: transport matching uses the wire URL, while replay also
requires the exact normalized logical URL. Consequently two navigations that
would send the same HTTP request but name different fragments cannot alias the
same current-format replay record.

Inputs are bounded to 2,047 serialized bytes and must already be ASCII. Raw
Unicode hostnames, whitespace, control bytes, non-HTTP schemes, and malformed
ports/authorities are rejected. A caller that accepts internationalized names
must perform standards-correct Unicode and IDNA processing before calling this
layer. The current normalizer is deliberately smaller than the WHATWG URL
algorithm.

## Request context

`TilefinchRequestContext` is the immutable input to request-policy decisions. It
records the target, initiator, top-level URL, method, mode, credentials mode,
destination, top-level-navigation state, and user activation. From that value
the engine derives same-origin/same-site state, whether credentials may be
sent, Lax-cookie eligibility, and `Sec-Fetch-Site`, `Sec-Fetch-Mode`, and
`Sec-Fetch-Dest` values.

The three credential modes are `omit`, `same-origin`, and `include`. `omit`
disables cookie transport rather than merely supplying an empty Cookie header;
`same-origin` is evaluated against the credential origin. Explicit Cookie
fields are browser-owned and cannot enter through page-controlled extra
headers.
If a `same-origin` redirect chain crosses that origin boundary, its credential
state becomes sticky-tainted: the target hop and all later hops are
credentialless, even if the chain returns to the original origin. Response
cookies are retained only from hops that were credential-eligible before their
`Location` was evaluated.

Redirect response bodies are not representations of the requested resource.
Once a bounded redirect response has complete headers, both synchronous and
pumpable transports stop that body and advance the hop under the same absolute
deadline. A transport setup or header-allocation failure aborts the request;
it never continues with a partially changed URL, method, referrer, identity
profile, or policy-header set.

Redirect detection distinguishes an absent `Location` field from a present
field with an empty value. The latter is a valid relative reference and is
resolved against the current logical URL; as with any `Location` lacking a
fragment delimiter, redirect processing inherits the current fragment. It is
therefore followed under the ordinary five-hop bound rather than being treated
as a response without a redirect target.

Response `Referrer-Policy` is security metadata, not merely a page-visible
header. Every response block resets and fills a dedicated bounded policy
field plus an explicit header-present bit. This storage is independent of the
bounded `response_headers` snapshot, so an earlier collection of large ordinary
headers cannot silently discard a later policy. The normalized policy value,
presence, and metadata-validity state are also bound by v9 capture/replay.

Page-controlled extra headers are parsed once at the native boundary. All
`Sec-*` and `Proxy-*` names, connection/framing fields, cookies, client hints,
user-agent/referrer/origin metadata, method-override headers, and other
Fetch-forbidden transport fields are rejected. Browser metadata must use the
typed request fields. `Authorization` and ordinary application `X-*` headers
remain intentionally available and are stripped when redirect policy requires
it. Proxy CONNECT response headers are suppressed by the host transport.

Informational 1xx fields are a separate response block. Their cookies,
metadata, and headers are discarded; only the final response can reach the
stream consumer or influence an explicit redirect hop.

Top-level navigation, frames, scripts, styles, images, fetch/XHR, forms, and
preloads should create a context instead of independently comparing host
strings. Response cookies are associated with the URL of the redirect hop that
supplied them, not unconditionally with the final URL. Redirects remain bounded
to HTTP(S) and a maximum of five hops in the host transport.

An HTTPS document cannot open an HTTP channel through scripts, stylesheets,
frames, fonts, fetch/XHR, or other active content. Eligible image and media
requests are upgraded once to HTTPS before transport and are blocked if that
request fails; they never fall back to HTTP. The shared transport applies the
same rule to the initial request and every explicit redirect hop, so a loader
cannot omit it and an HTTPS response cannot redirect silently to HTTP. A
bounded 16-site compatibility table can explicitly permit mixed content for a
top-level site. It is off by default, available from the Privacy settings, and
exists only for the running browser session: Tilefinch neither writes grants
to the Memory Stick nor restores legacy persisted grants after restart. HTTP
loopback remains potentially trustworthy for local development.

Ordinary top-level HTTP navigation is HTTPS-first. Tilefinch upgrades the
initial request, does not silently retry HTTP after a TLS/network failure, and
rejects a later HTTPS-to-HTTP redirect without the same explicit compatibility
grant. Literal private addresses, localhost, single-label LAN names, and
`.local` names remain available for explicitly entered local navigation. A
16-entry, least-recently-used HSTS table honors `max-age` and
`includeSubDomains` from verified HTTPS responses. The table is intentionally
session-only: it adds no Memory Stick reads or writes and is rebuilt from
responses after restart.

Page-originated traffic from a public origin also enables Private Network
Access protection. Both the socket-open callback and libcurl's pre-transfer
peer callback reject loopback, IPv4 RFC1918/link-local, carrier-grade NAT
(100.64/10) and benchmark (198.18/15) address space, IPv6 loopback,
unique-local/link-local, unspecified, and mapped-private peers. The check runs
against every resolved redirect hop and reused connection, so changing DNS
answers cannot bypass it. Such requests bypass ambient proxies because a
proxy-resolved peer cannot be verified locally. Explicit top-level LAN
navigation and pages already identified by a local URL may use that address
space. This is a fail-closed address-space boundary, not full Fetch PNA
preflight support.

## Cookie policy

The browser session stores a fixed number of budget-owned cookies. Contextual
read and write APIs apply credential policy, host/domain and path matching,
expiry, `Secure`, `HttpOnly`, `SameSite` Default/Lax/Strict/None,
`Partitioned`, and the `__Secure-` and `__Host-` prefix requirements.
`SameSite=None` and partitioned cookies require `Secure`; insecure origins
cannot set Secure cookies. Script access excludes HttpOnly values. Cookie names
and serialized fields reject control, non-ASCII, and invalid token bytes.
The jar holds 32 cookies overall and at most eight for one exact domain;
insertion evicts the oldest applicable entry so one origin cannot permanently
deny storage to every other site. Paths through 319 bytes remain inline.
Exceptional paths through 2,047 bytes use an on-demand session pool capped at
8 KiB, so accepting a long URL does not enlarge every fixed PSP cookie entry.
Unpartitioned cookies from an embedded cross-site response are neither stored
nor sent by default, including ordinary `SameSite=None` cookies. `Partitioned`
cookies remain available under their top-level-site partition. A separate
bounded 16-site compatibility table may permit ordinary third-party cookies
for a selected top-level site; top-level cross-site navigation is first-party
and continues to use the normal SameSite rules.

Live fetches do not enable libcurl's cookie engine. When a request supplies a
browser session and immutable request context, the transport takes a bounded
cookie-only snapshot, derives the Cookie field independently for every
explicit redirect URL/method, and applies intermediate Set-Cookie fields to
that private overlay. This preserves host/path ordering, SameSite,
Partitioned, and credential policy while leaving the caller's session
unchanged until it accepts the completed `FetchResult`. Callers without a
session may send their bounded literal Cookie field on the initial hop only;
redirects then fail closed without cookies.

Cookie Domain admission, SameSite comparison, and partition keys share one
bundled Public Suffix List snapshot. Its immutable ASCII/punycode DAFSA is
52,676 bytes in the linked image, uses no heap, and includes both ICANN and
PRIVATE rules. Parent-domain cookies are accepted only when the request host
domain-matches the attribute and the attribute is not a public suffix. Invalid
or unclassifiable parent scopes fail closed. An attribute exactly matching a
public-suffix request host is retained as host-only, as required by RFC 6265;
`__Host-*` still rejects the presence of any Domain attribute.

Site identity is scheme plus registrable domain and deliberately excludes the
port. IP literals, single-label hosts, and public suffixes without a
registrable label retain their canonical host. The snapshot source, hash,
licenses, and deterministic refresh command are recorded in
`third_party/public_suffix/README.md`; updating it is security maintenance,
not an acceptance-site compatibility tweak.

The host transport exposes at most `FETCH_RESPONSE_COOKIE_CAPACITY` (currently
32) `Set-Cookie` fields in one bounded redirect transaction, and each
serialized field must fit its 4,095-byte storage ceiling. Cookie values and
their response-URL attributions are allocated from the shared budget only when
received; `FetchResult` retains only the fixed pointer inventory, avoiding a
large worst-case payload in every result and scheduler/stack frame. If a final
response carries more than 32 individually valid fields, Tilefinch accepts the
page, retains the first 32, and marks the cookie set as truncated so capture and
replay preserve the same outcome. The same overflow on a redirect hop fails
closed: a partial intermediate set could otherwise hide a deletion before the
next request. An oversized field or a cookie-allocation failure remains fatal
on every hop rather than exposing partially interpreted security attributes.
This remains deliberately stricter than a desktop browser.

## Content blocking

Content blocking is a compatibility and performance feature, not a security
boundary. It runs before initial transport and every redirect, but an allowed
page can use an unlisted origin, inline content, first-party endpoints, or any
filter syntax outside Tilefinch's documented subset. Top-level navigations are
never blocked. Custom rules and per-site exceptions are user-controlled data
on the Memory Stick; list replacement validates all bounds before changing the
active policy. The browser does not download filter updates automatically.

## Memory Stick site data

The optional HTTP cache and `localStorage` persistence settings are off by
default, and a disabled category is not read during boot. Their separately
bounded files are stored unencrypted on the removable Memory Stick, so cached
page content and local-storage values must be treated as visible to anyone who
can read that stick. Cookies and `sessionStorage` are never serialized. The
Site Data panel provides independent clear actions for cache, cookies, local
storage, and session storage. Imports treat the files as untrusted: lengths,
record counts, normalized HTTP(S) cache keys and origins, response metadata,
aggregate bytes, and a whole-payload checksum are checked before transactional
replacement of live state. The checksum detects damage, not deliberate
tampering. A person who can rewrite the Memory Stick can therefore replace
cached same-origin content or local-storage values; the PSP has no protected
device secret with which to authenticate either file. Persistent site data
should be disabled or cleared when that physical attacker is in scope.
Imported response bodies are always stale until HTTP revalidation. Primary and
backup generations are category-specific, so a cache-only load never commits
storage and a local-storage-only load never commits cache data.

The browser preference profile uses the same primary/backup generation model.
Each accepted generation must end in an exact byte-count and checksum footer;
a newline-aligned prefix is incomplete, so a torn primary falls back to its
backup instead of accepting enabled defaults for omitted late policy records.
The checksum is a damage detector, not authentication against a person who can
rewrite the Memory Stick.

Optional tab hibernation is also off by default. Its single bounded session
file contains URL/title history plus scroll and focus facts, unencrypted. It
uses a versioned envelope, exact length checks, and a checksum before replacing
resident state; the checksum detects damage rather than a physical attacker.
Tilefinch never reads a stale hibernation file during boot. Successful wake,
disabling the option, and controlled exit remove it; abrupt power loss may
leave it until the next hibernation replaces it or the user deletes it.

## HTTP trace handling

New captures use HTTP trace format v11. The replay reader intentionally retains
compatibility with v1--v10 for document and generic-fetch fixtures, but typed
script, stylesheet, image, and font records older than v10 cannot prove that a
security-relevant response header was not dropped and therefore fail closed.
V10 records and requires the response-security-header truncation bit; replay
reconstructs it before CORS or typed-resource admission. V11 additionally
binds the once-parsed typed security-header classification
(present/valid/malformed/duplicate/truncated per field), so replay feeds
consumers the same typed metadata live responses produce instead of
re-deriving it from raw header text. Because v8 and older
records contain
only the fragmentless wire identity, they may replay only when the current
normalized logical request has no fragment. V9 additionally binds the exact
normalized logical request URL and the response's dedicated bounded
`Referrer-Policy` metadata; same-wire requests with different fragments fail
closed. V6 fail-closed matching
binds the canonical fragmentless URL and method, request body length and
fingerprint, every non-secret transport-shaping field, credential mode and
immutable request context, and response body length and fingerprint. It also
attributes each `Set-Cookie` field to the eligible response hop that supplied
it and declares whether cookie values are redacted or raw. V7 also binds the
requested Critical-CH token set and its accepting origin, so replay cannot
silently widen high-entropy hint disclosure. V8 also records redirect-origin
taint and binds CORS enforcement, same-origin redirect policy, and whether a
conditional request carries already-validated cached CORS provenance. Because
older traces cannot prove a same-origin final URL did not traverse another
origin, pre-v8 CORS records without explicit taint are rejected rather than
silently treated as safe.

Live low- and high-entropy client hints are sent only to potentially
trustworthy HTTP(S) targets: HTTPS, loopback HTTP, and localhost HTTP. A
`Critical-CH` retry is permitted only when the same final response contains a
well-formed `Accept-CH` list accepting every requested, supported token. The
retry remains same-origin, is bounded to one attempt, and cannot inherit a
request from an untrustworthy redirect hop.

## Page resource and embedding policy

Every page-originated fetch carries one immutable request context containing
the document's final response URL, origin, content-blocking policy, mixed-
content policy, and parsed Content Security Policy. The fetch scheduler copies
that context at admission and revalidates it before transport and after every
redirect. Script, stylesheet and `@import`, image, font, frame, Fetch/XHR/SSE,
form, and base-URL entry points therefore cannot bypass policy by reaching a
lower-level transport helper directly. Browser-owned update and provider
requests use separate privileged paths and never inherit page authority.

The bounded response-header CSP subset intersects up to four policies and
supports `default-src`, `script-src`, `style-src`, `img-src`, `font-src`,
`connect-src`, `frame-src`, `object-src`, `base-uri`, `form-action`, and
`frame-ancestors`, plus `worker-src` with the standard fallback to `script-src`
and then `default-src`. It recognizes `'none'`, `'self'`, wildcard, scheme, bounded
host/port/path sources, `data:` and `blob:` where the selected directive permits
them, plus script/style nonces and SHA-256 hashes. A nonce or hash source makes
`'unsafe-inline'` ineffective for that directive. Authored inline style
attributes are gated; CSSOM `element.style` mutation remains allowed, matching
the browser distinction between inline content and a style API operation.
Security-header truncation, policy overflow, or an invalid bounded policy fails
the affected navigation closed rather than applying a partial policy.

Page-originated `eval`, indirect eval, and the Function/AsyncFunction/
GeneratorFunction constructors are disabled unless every applicable enforced
`script-src`/`default-src` policy contains `'unsafe-eval'`. The gate is inside
the pinned QuickJS interpreter rather than a replaceable JavaScript wrapper.
Trusted bootstrap code runs before the gate is adopted; its temporary native
worker compiler is captured in a private closure and removed from the page
global before author script. Blob workers are then admitted by `worker-src`
and compiled only through that retained host path, so they do not reopen a
general dynamic-compilation primitive.

Users can also disable author JavaScript globally or for the current site.
The per-site deny list is bounded to 16 sites and is consulted before a page
runtime is admitted; changing either control cancels an in-flight navigation
and reloads under the new policy. These controls are an availability and
compatibility escape hatch for problematic pages, not an origin-isolation
boundary and not a substitute for CSP.

Framed responses are checked before their DOM or JavaScript runtime is created.
`frame-ancestors` takes precedence over `X-Frame-Options`; otherwise `DENY` and
`SAMEORIGIN` are enforced. Top-level documents ignore ancestor policy. The
current engine has only bounded direct child frames, so this is not a claim of
full nested browsing-context isolation.

External classic/module scripts and linked stylesheets enforce bounded
Subresource Integrity metadata with SHA-256, SHA-384, and SHA-512. The
strongest recognized algorithm wins and any matching digest at that strength
admits the response. Malformed recognized metadata and metadata beyond the
2,048-byte/32-token limits fail closed. Cross-origin integrity requires an
explicit `crossorigin` attribute and a successful CORS response; those
responses bypass caches that cannot retain CORS provenance. Integrity failure
dispatches the resource error path and the bytes are never compiled or added
to the cascade.

Every admitted external script, stylesheet, and image response also produces a
compact native resource grant from the immutable request context and the final
response. The grant records request mode, credentials mode, destination,
opaque-origin state, final same-origin/same-site classification, MIME decision,
and Cross-Origin-Resource-Policy decision. Classic scripts require an
admissible response under their no-CORS context; modules additionally require
CORS and a JavaScript MIME type. `X-Content-Type-Options: nosniff` rejects
classic script and stylesheet responses with an incompatible MIME type, and
`Cross-Origin-Resource-Policy` `same-origin`/`same-site` is enforced against the
final response URL. Duplicate, oversized, unknown, or truncated policy fields
fail closed. A 304 may reuse cached representation bytes, but it rebuilds the
typed grant from the cached normalized grant plus the 304 metadata; restrictive
CORP, `nosniff`, malformed, or truncated revalidation metadata can revoke use.

The in-memory cache retains that grant with its response and matches it only
under the same top-level partition and requesting principal. Multiple
authorized representations of the same URL may coexist; a generic cache
lookup cannot consume them. Compiled classic-script bytecode remains keyed by
the exact response bytes, so partitions may share an immutable compiled
artifact only when their independently authorized bodies are byte-identical.
Module entries additionally retain the immutable top-level site partition;
opaque-origin modules are not placed in the shared module cache because the
serialized `null` origin is not a principal. Child runtimes receive the
top-level document URL at creation and reuse it for fetch, XHR, classic-script,
and module request/cookie/cache contexts rather than substituting their frame
URL. Stylesheet and image consumers
perform the same grant match on reuse, including after a redirect; a unique
opaque initiator cannot be represented by the bounded serialized principal and
therefore receives no shared cache hit. The v1 persistent-session format does
not contain the complete grant, so resource-authorized entries remain
memory-only rather than being serialized as weaker generic entries. Save/load
tests pin that fail-closed boundary. Fonts and other generic consumers are not
covered by this claim.

Native scheduler/document callbacks are captured in a fixed retained registry
after bootstrap hardening and before author script runs. Native code calls
those retained functions directly; it does not repeatedly resolve writable
or enumerable page-global names. Author code may shadow a compatibility name
without replacing the browser's delivery, mutation, frame-lifecycle, timing,
or document-state callback. This reduces both lookup work and the authority of
the shared JavaScript realm, but it is not process isolation.

An iframe `sandbox` attribute starts from the deny-all state. The bounded token
parser recognizes `allow-scripts`, `allow-same-origin`, `allow-forms`,
`allow-popups`, `allow-top-navigation`, and
`allow-top-navigation-by-user-activation`; over-limit input remains deny-all.
Scripts are not given a child runtime without `allow-scripts`, and a frame
without `allow-same-origin` receives a unique opaque origin exposed as
`"null"`. Forms, auxiliary windows, and child-driven top navigation are not
implemented for child contexts, so their allow tokens cannot grant an absent
capability. Changing a remote frame's URL or sandbox policy retires its old
runtime before constructing the replacement.

Cross-context messages are capped at 16 queued records and 64 KiB of JSON each.
`targetOrigin` is either `*` or one canonical HTTP(S) origin; opaque targets can
therefore be addressed only with `*`. Source origin and source/target lifecycle
generations are captured at enqueue, so redirects, detachment, or DOM-handle
reuse cannot relabel or redirect an older message. Cross-origin WindowProxy
objects expose only the small messaging/lifecycle surface rather than falling
through to page globals. Tilefinch does not create auxiliary browsing contexts:
`window.open()` returns `null` and every top/child `opener` is immutable `null`.

This is intentionally not complete CSP. Meta-delivered policies,
`Content-Security-Policy-Report-Only`, reporting, `strict-dynamic`, CSP
`sandbox`, Trusted Types, the element/attribute directive
families, and COOP/COEP process isolation are not implemented. The PSP has no
process sandbox, so CSP reduces page authority inside one engine process; it
does not contain a memory-safety flaw in the engine.

The transport-shaping match includes the selected diagnostic browser profile;
a capture made with Mobile Safari diagnostics cannot replay as the ordinary
PSP profile even when the caller supplied an identical `User-Agent`. Required
current-format fields, counts, and numbered entries are parsed strictly and without
duplicates, and the response body is verified on the same open file descriptor
before any consumer callback or deferred handoff.

Captured response `Set-Cookie` values are redacted by default. The trace keeps
cookie names, attributes, and value byte lengths so replay still exercises
policy and request shape without retaining live session secrets. The trace
format records whether values are `redacted` or `raw`.
The fixed redacted seed format supports inline cookie paths only. If a live jar
contains an exceptional long-path cookie, capture fails before publishing an
authority file rather than truncating it into a non-replayable or differently
scoped seed.

Raw values are captured only when the environment variable is exactly
`TILEFINCH_TRACE_RAW_COOKIES=1`. A raw trace is a session secret: use it only in an
isolated, access-controlled diagnostic and never commit or share it. Request
traces store cookie byte counts and selected state indicators rather than the
request Cookie header itself. Extra application headers are represented by
lowercased names and value byte lengths; raw values such as bearer tokens are
not retained. Consequently a trace proves matching request shape, not exact
authentication-secret equality.

## Explicitly incomplete areas

Reader mode is not a sanitizer or a content-security boundary. It hides page
regions with user-origin CSS only; hidden nodes remain in the DOM, their
scripts retain the same authority, and toggling the mode off reveals them
again. Site-specific rules are therefore presentation hints, never evidence
that advertising, tracking, or hostile content was removed. Network blocking
is a separate pre-transport policy described in `CONTENT_BLOCKING.md`.

Offline Reader snapshots cross a separate persistence boundary: only escaped
body text, a bounded title, and the source URL are serialized. Live DOM
objects, scripts, forms, cookies, and event state never enter the snapshot.
YouTube downloads accept direct media URLs only after the existing provider
host policy validates them; internal enqueue and management routes are also
source-page gated by the PSP frontend. The offline index and payloads are
untrusted Memory Stick input on the next run and are size-checked before use.
See [OFFLINE_LIBRARY.md](OFFLINE_LIBRARY.md).

The engine does not yet provide a complete WHATWG URL parser, complete
Fetch/CORS/CSP or mixed-content policy, all-resource cache partitioning,
certificate UI, permissions, nested browsing-context sandboxing, or origin
process isolation. The implemented CSP, SRI, and frame-policy subset is defined
above. Script, stylesheet, and image cache partitioning and typed response
grants are also defined above; neither should be inferred for other resource
classes.
Unsupported directives must not be inferred from the presence of that subset.

The default PSP release cross-builds hash-pinned curl 8.21.0 and Mbed TLS
3.6.6 LTS; HTTP/2 builds additionally pin nghttp2 1.69.0. Runtime startup
checks that linked provenance and fails closed on drift. Tilefinch still
narrows curl to HTTP(S), disables automatic redirects and curl cookie state,
applies peer and host verification on every easy handle, and runs curl/nghttp2
allocations inside a fixed one-MiB budget pool. The SDK's older transport
remains only as an explicit, non-release `LEGACY` escape hatch. Source,
offline-build, and measured-size details are in
`docs/engineering/PSP_TRANSPORT.md`.

Security-sensitive behavior should keep failing closed as these pieces are
added; acceptance-site compatibility is not permission to weaken these
generic rules.

Optional voice and glyph data are not trusted because they came from the
project's asset repository. Voice (`TFVMv1`/TFVP format 2) and glyph
(`TFGMv1`/TFGF format 3) components have distinct signature domains, package
formats, fixed names, bounds, component IDs, and monotonic sequences. A valid
artifact from one class cannot authorize another. Glyph packs are parsed as
untrusted bounded indexes, payload blocks are read only through the cooperative
provider, and malformed or unavailable data falls back to the embedded glyphs.
Neither optional path uses PSP firmware data or creates a new executable-code
authority.
