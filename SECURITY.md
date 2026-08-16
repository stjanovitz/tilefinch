# Security

Tilefinch runs on a 2009 handheld with no OS sandbox, no protected storage,
and homebrew custom firmware. This page states plainly what the project's
security machinery does and does not protect, and how to report a problem.
The full engineering detail is in [docs/SECURE_UPDATES.md](docs/SECURE_UPDATES.md)
and [docs/SECURITY_MODEL.md](docs/SECURITY_MODEL.md).

## What is protected

- **Stable and Beta updates cannot be forged.** An attacker who controls the network, a CDN,
  the GitHub account, or a release asset cannot make the PSP install an
  unsigned or downgraded browser. The device trusts only the public signing
  keys embedded in its small stable launcher and the release metadata those
  keys authorize — not TLS, not GitHub, not the download itself.
- **Developer updates are an explicit exception.** A locally configured and
  selected Developer channel accepts unsigned contributor code. It retains
  bounded package/file verification and A/B trial rollback, but it does not
  authenticate the publisher; the configured endpoint must be trusted.
- **A bad update cannot brick the browser.** Updates install into an inactive
  A/B slot, boot as a supervised trial, and automatically roll back to the
  previous known-good version if the new one fails to prove itself healthy.
  Interruption, cancellation, power loss, or a full Memory Stick leaves the
  previous browser launchable.
- **Browsing fails closed.** HTTPS connections verify certificates and
  hostnames; cookies, redirects, and cached data follow the documented model
  in [docs/SECURITY_MODEL.md](docs/SECURITY_MODEL.md). Page requests share one
  immutable origin/policy context and are emitted through one request-authority
  builder rather than caller-assembled Cookie/CORS/Fetch-Metadata fields.
  Security response headers are classified once from complete wire fields into
  typed present/valid/malformed/duplicate/truncated states. The bounded
  response-header CSP and frame-embedding subset is enforced before resources
  are fetched or child DOMs are created. Script, stylesheet, and image responses carry typed authorization
  grants, enforce their applicable MIME and Cross-Origin-Resource-Policy
  decisions, and are cache-partitioned by the top-level site and requesting
  principal. Malformed, duplicate, truncated, or oversized security input is
  rejected rather than partially trusted. A user can temporarily allow mixed
  content for a site, but that compatibility grant exists only in memory for
  the running session and is not restored after restart.
- **Web pages cannot reach the local network.** Top-level navigation is
  HTTPS-first and never silently downgrades to HTTP; a bounded in-memory HSTS
  table remembers verified HTTPS policy for the session. Pages loaded from
  the internet cannot make requests to loopback, link-local, or
  private-network peers (including carrier-grade NAT and IPv4-mapped IPv6
  forms), so a malicious page cannot probe a router's admin interface or
  devices behind the user's NAT. The user typing a local address as a
  top-level page remains allowed.

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
  stylesheets, and images are partitioned, but that protection must
  not be inferred for every cache consumer. Do not use Tilefinch for sensitive
  accounts or transactions.

## How updates verify

Release metadata and packages are signed with P-256 ECDSA over SHA-256,
verified against a bounded key chain rooted in the launcher's embedded keys.
The updater checks the signed manifest, exact package size and hash, and
every extracted file before the new slot may boot; signature verification
rejects expired metadata and older release sequences, so a replayed old
release cannot masquerade as an update. An ordinary boot of the known-good
slot adds no verification work; full verification happens when entering or
retrying a trial. Holding L at startup always boots the previous known-good
version when one exists.

## Reporting a vulnerability

Please use GitHub's private vulnerability-reporting form:

https://github.com/stjanovitz/tilefinch/security/advisories/new

Do not open a public issue for an undisclosed vulnerability. If GitHub's form
is unavailable, contact the repository owner privately through their GitHub
profile. There is no bug bounty; reports that include a minimal reproduction
(page, trace, or package) are the fastest to act on.
