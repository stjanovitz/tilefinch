# Security

Tilefinch runs on a mid-2000s handheld with no OS sandbox, no protected
storage, and homebrew custom firmware. This page states plainly what the
project's security machinery protects, what it does not, and how to report a
problem. The engineering detail is in
[docs/SECURE_UPDATES.md](docs/SECURE_UPDATES.md) and
[docs/SECURITY_MODEL.md](docs/SECURITY_MODEL.md).

## What is protected

- **Stable and Beta updates cannot be forged.** An attacker who controls the
  network, a CDN, the GitHub account, or a release asset cannot make the PSP
  install an unsigned or downgraded browser. The device trusts only the
  public signing keys embedded in its small, stable launcher and the release
  metadata those keys authorize: not TLS, not GitHub, and not the download
  itself.
- **Developer updates are an explicit exception.** A Developer channel that
  you configure locally and select accepts unsigned contributor code. It still
  verifies bounded package and file integrity and keeps A/B trial rollback,
  but it does not authenticate the publisher, so the configured endpoint must
  be trusted.
- **A bad update cannot brick the browser.** Updates install into the inactive
  A/B slot, boot as a supervised trial, and roll back automatically to the
  previous known-good version if the new one does not prove itself healthy.
  Interruption, cancellation, power loss, or a full Memory Stick leaves the
  previous browser launchable.
- **Browsing fails closed.** HTTPS connections verify certificates and
  hostnames, and cookies, redirects, and cached data follow
  [the security model](docs/SECURITY_MODEL.md). In particular:
  - every page request is built by one request-authority builder from one
    immutable origin and policy context, rather than from Cookie, CORS, and
    Fetch Metadata fields each caller assembles;
  - security response headers are classified once, from the complete wire
    fields, as present, valid, malformed, duplicate, or truncated, and
    malformed, duplicate, truncated, or oversized security input is rejected
    rather than partly trusted;
  - the bounded subset of response-header CSP and frame-embedding policy is
    enforced before resources are fetched or child documents are created;
  - script, stylesheet, and image responses carry typed authorization grants,
    enforce their MIME and Cross-Origin-Resource-Policy rules, and are
    cache-partitioned by top-level site and requesting principal;
  - you can allow mixed content for one site as a compatibility measure, but
    that grant exists only in memory for the running session and is not
    restored after a restart.
- **Web pages cannot reach the local network.** Top-level navigation is
  HTTPS-first and never silently downgrades to HTTP, and a bounded in-memory
  HSTS table remembers verified HTTPS policy for the session. Pages loaded
  from the internet cannot send requests to loopback, link-local, or
  private-network addresses (including carrier-grade NAT and IPv4-mapped IPv6
  forms), so a malicious page cannot probe a router's admin interface or
  other devices behind your NAT. Typing a local address as a top-level page
  yourself still works.

## What is explicitly out of scope

- **Physical access to the Memory Stick.** Anyone who can rewrite the root
  `EBOOT.PBP` owns the trust root. The PSP has no protected storage in which
  homebrew could anchor anything stronger.
- **The trust root itself.** Everything chains from the public keys embedded
  in the stable launcher. The launcher does not update itself in-app; a
  launcher flaw or exhausted key threshold requires a manual reinstall.
- **Desktop-browser guarantees the engine does not yet make.** There is no
  complete CSP, CORS-complete fetch policy, all-resource cache partitioning,
  certificate UI, permissions system, or process isolation. Scripts,
  stylesheets, and images are cache-partitioned, but do not assume that for
  every cache consumer. Do not use Tilefinch for sensitive accounts or
  transactions.

## How updates verify

Release metadata and packages are signed with P-256 ECDSA over SHA-256 and
verified against a bounded key chain rooted in the launcher's embedded keys.
Before the new slot may boot, the updater checks the signed manifest, the
exact package size and hash, and every extracted file. Verification rejects
expired metadata and older release sequences, so a replayed old release
cannot pose as an update. An ordinary boot of the known-good slot adds no
verification work; full verification happens when a trial starts or is
retried. Holding L at startup always boots the previous known-good version
when there is one.

## Reporting a vulnerability

Please use GitHub's private vulnerability-reporting form:

https://github.com/stjanovitz/tilefinch/security/advisories/new

Do not open a public issue for an undisclosed vulnerability. If GitHub's form
is unavailable, contact the repository owner privately through their GitHub
profile. There is no bug bounty; reports that include a minimal reproduction
(page, trace, or package) are the fastest to act on.
