# Security

This page is for PSP owners and security researchers. Tilefinch runs on a
mid-2000s handheld with no OS sandbox, no protected storage, and homebrew
custom firmware. This page states plainly what the project's security
machinery protects, what it does not, and how to report a problem. The
engineering detail is in [docs/SECURE_UPDATES.md](docs/SECURE_UPDATES.md) and
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
  verifies package and file integrity and keeps A/B trial rollback,
  but it does not authenticate the publisher, so the configured endpoint must
  be trusted.
- **A bad update cannot brick the browser.** Updates install into the inactive
  A/B slot, boot as a supervised trial, and roll back automatically to the
  previous known-good version if the new one does not prove itself healthy.
  Interruption, cancellation, power loss, or a full Memory Stick leaves the
  previous browser launchable.
- **Browsing fails closed.** HTTPS connections verify certificates and
  hostnames. In particular:
  - every request's cookies, CORS mode and origin headers come from one
    place, so no part of the browser can assemble them inconsistently;
  - a security header that is malformed, repeated, cut short or oversized is
    rejected rather than partly trusted;
  - the supported part of a page's Content Security Policy and framing rules
    is applied before resources are fetched or frames are created;
  - scripts, stylesheets and images must have the right type and
    cross-origin permission, and are cached separately for each site that
    uses them;
  - allowing mixed content for one site lasts only until you exit Tilefinch.

  [The security model](docs/SECURITY_MODEL.md) describes each of these
  rules in detail.
- **Web pages cannot reach the local network.** Top-level navigation is
  HTTPS-first and never silently downgrades to HTTP, and an in-memory HSTS
  table remembers verified HTTPS policy for the session. Pages loaded
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
- **Desktop-browser guarantees the engine does not yet make.** Tilefinch has
  no process isolation and no complete CSP, as the README's
  [Known limits](README.md#known-limits) say. It also has no complete CORS
  fetch policy, no certificate viewer, no permissions system, and no cache
  partitioning beyond scripts, stylesheets and images.

## How updates verify

Release metadata and packages are signed with P-256 ECDSA over SHA-256 and
verified against a key chain rooted in the launcher's embedded keys.
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
