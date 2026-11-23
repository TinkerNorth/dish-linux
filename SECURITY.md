# Security policy: TinkerNorth

This file covers the six TinkerNorth repositories that ship the
wireless-gamepad product end to end, plus the website the clients read their
controller database from:

- [`satellite`](https://github.com/TinkerNorth/satellite): Satellite, the server (Windows, Linux, macOS)
- [`dish-android`](https://github.com/TinkerNorth/dish-android): Dish for Android
- [`dish-ios`](https://github.com/TinkerNorth/dish-ios): Dish for iOS (SwiftUI)
- [`dish-mac`](https://github.com/TinkerNorth/dish-mac): Dish for macOS (SwiftUI)
- [`dish-windows`](https://github.com/TinkerNorth/dish-windows): Dish for Windows (Qt 6, SDL2)
- [`dish-linux`](https://github.com/TinkerNorth/dish-linux): Dish for Linux (Qt 6, SDL2)
- [`https://dish.tinkernorth.com`](https://dish.tinkernorth.com): the website, which serves the controller database

Each repo has its own `CONTRIBUTING.md#security` section with its
ecosystem's local commands. This file is the single source of truth for
**(1) reporting a vulnerability**, **(2) what we do with the report**, and
**(3) how a downstream consumer verifies a Dish for Linux release
artifact**.

---

## Reporting a vulnerability

**Do not file a public issue for a suspected vulnerability.**

Use one of:

1. **GitHub private vulnerability reporting.** Open the repo, click
   *Security*, then *Report a vulnerability*. This is preferred because it
   creates a tracked advisory and a private discussion thread.
2. **Email**: `security@tinkernorth.com` (PGP key on request). Include the
   repo, the version (commit SHA or release tag), reproduction steps, and
   the impact.

Please do **not** test exploits against infrastructure you don't own. The
on-LAN threat model already covers an attacker with packet-injection ability
on the local network: that is the documented design boundary, not a bug.

### Response SLA

| Severity | Triage acknowledgement | Initial assessment | Fix target |
|---|---|---|---|
| Critical (CVSS >= 9.0) | 1 business day | 3 business days | 14 days, coordinated disclosure |
| High (CVSS 7.0 to 8.9) | 2 business days | 5 business days | 30 days |
| Medium / Low           | 5 business days | 10 business days | next minor release |

If we miss the SLA, you may publish 90 days after the original report date
regardless. We'd rather know than not know.

### Scope

In scope:

- All six repos above, and the controller database the website serves at
  `https://dish.tinkernorth.com/controllers/v1/`.
- Release artifacts attached to GitHub Releases for any of the six repos,
  and the APT, DNF and pacman repositories at
  `https://tinkernorth.github.io/dish-linux/`.
- The wire protocol (protocol version 4; the single source of truth is
  [`satellite/docs/contract.md`](https://github.com/TinkerNorth/satellite/blob/main/docs/contract.md)):
  the HTTPS `:9443` control plane (TOFU-pinned self-signed TLS, HMAC
  proof-of-key headers, declarative session PUT) and the UDP `:9876` data
  plane (ChaCha20-Poly1305-IETF under per-session HKDF keys, per-direction
  counters, replay guard).
- The pairing flow (PIN paths A and B on `:9443`) and the loopback-only admin
  HTTP/SSE web UI exposed by `satellite`.

Out of scope:

- Anything that requires the attacker to already have local privileges on
  the user's machine (root, Administrator, the ability to drop binaries in
  `%APPDATA%` or `~/.local`, and so on).
- The vendored ViGEmBus driver itself: file with
  [nefarius/ViGEmBus](https://github.com/nefarius/ViGEmBus).
- DoS via raw network flooding. UDP without a rate limit is a known trade-off
  for hot-path latency; mitigations belong in the network fabric, not the
  protocol.

---

## Supported versions

| Repo | Supported | Notes |
|---|---|---|
| `satellite` | latest minor on `main`; previous minor for 90 days | Windows is the canonical target; Linux is supported; macOS virtual pads require the virtual-HID entitlement (unentitled builds fall back to an inert backend) |
| `dish-android` | latest minor on `main`; previous minor for 90 days | minSdk 24 |
| `dish-ios` | latest minor on `main`; previous minor for 90 days | iOS 26.0+ |
| `dish-mac` | latest minor on `main`; previous minor for 90 days | macOS 13+ |
| `dish-windows` | latest minor on `main`; previous minor for 90 days | 64-bit Windows 10 1809+ or Windows 11 |
| `dish-linux` | latest minor on `main`; previous minor for 90 days | `.deb` for Debian 13+, `.rpm` built on Fedora 44, AppImage for glibc 2.35+ (Ubuntu 22.04 and newer), Flatpak on the KDE runtime |

Patch releases (`X.Y.Z+1`) are issued on demand for the latest minor; the
previous minor only receives backports for high and critical fixes. Tags are
bare `X.Y.Z` in every repo, with no `v` prefix.

---

## How CI prevents vulnerable code from shipping

Each repo runs the same shape of gates.

**On every PR** (blocking):

The jobs live in the shared `_security.yml`, byte-identical in every repo.

- Action-pin lint: every `uses:` line must reference a 40-character SHA with
  a `# vX.Y.Z` comment.
- Allowlist expiry: `.security/allowlist.yaml` entries must be complete,
  unexpired and at most 90 days out; the allowlist is the only place a
  vulnerability is accepted.
- Dependency review: GitHub advisory database (PR only). Off in every repo
  until its Dependency graph setting is switched on.
- OSV-Scanner 2.7.1: manifest dependencies, and code vendored or built from
  source named by its upstream commit (for `dish-linux`: ENet, SDL2,
  libsodium, sentry-native and Catch2, through `scripts/osv-lockfile.sh`).
  "No packages found" fails.
- Gitleaks: secret scanning over the full history.
- CodeQL: `cpp` for `satellite`, `dish-windows` and `dish-linux`, `swift`
  for `dish-mac` and `dish-ios`, `java-kotlin` and `cpp` for `dish-android`.

**On every tagged release** (also blocking):

The hardening lives in the shared `_release_harden.yml`; every pull request
runs it in dry-run mode on that pull request's own packages, signed with a
throwaway key.

- A re-run of every PR-time gate against the tagged commit.
- Required-secrets gate: refuses to publish if the platform signing secret
  is missing for a tag (Windows Authenticode, Apple Developer ID and
  notarization, Android keystore). Linux binaries carry no platform
  signature, so for `dish-linux` the gate enforces that the cosign keyless
  flow can authenticate.
- Artifact-level vulnerability scan: Grype 0.122.1, fails on CRITICAL and
  HIGH.
- SBOM generation: Syft 1.57.2, both SPDX-JSON and CycloneDX-JSON.
- `SHA256SUMS` over every artifact and the SBOMs.
- Cosign 3.1.4 keyless signing: every artifact and `SHA256SUMS` get one
  Sigstore bundle, `<file>.sigstore.json`, anchored in the transparency log.
  Releases signed before this (Dish for Linux up to 2.8.1) carry a `.sig` and
  a `.crt` instead.
- SLSA L3 build provenance: `slsa-framework/slsa-github-generator` emits
  `<repo>.intoto.jsonl`.

A known-vulnerable dependency, a missing signature or a tampered binary fails
the release before any artifact lands on the GitHub Release page.

---

## Verifying a release artifact (consumer recipe)

The commands below are for Dish for Linux. The other repos follow the same
steps with their own file names; each repo's own `SECURITY.md` carries its
recipe.

### 0) Pre-requisites

```bash
# cosign 3.x
brew install cosign            # macOS
go install github.com/sigstore/cosign/v3/cmd/cosign@latest

```

### 1) Download every file from the Release

```bash
TAG=X.Y.Z
gh release download "$TAG" -R TinkerNorth/dish-linux -D "dish-linux-$TAG"
cd "dish-linux-$TAG"
```

A release carries the `.deb` (`dish_X.Y.Z_amd64.deb` and `dish-amd64.deb`),
the `.rpm` (`dish-X.Y.Z-1.x86_64.rpm` and `dish-x86_64.rpm`), the AppImage
(`Dish-X.Y.Z-x86_64.AppImage` and `Dish-x86_64.AppImage`, with `.zsync` and
`.sha256`), the Flatpak bundle (`Dish-X.Y.Z-x86_64.flatpak` and
`Dish-x86_64.flatpak`), `latest.json`, `SHA256SUMS`,
`dish.sbom.spdx.json` and `dish.sbom.cdx.json`, one `<file>.sigstore.json` per
file, `dish-linux.intoto.jsonl` (SLSA L3 provenance), and
`measurements-X.Y.Z.md` and `.json`, attached after hardening and so not in
`SHA256SUMS`.

### 2) Verify that this repository's release workflow signed `SHA256SUMS`, on this tag

```bash
cosign verify-blob \
  --bundle SHA256SUMS.sigstore.json \
  --certificate-identity "https://github.com/TinkerNorth/dish-linux/.github/workflows/_release_harden.yml@refs/tags/$TAG" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  SHA256SUMS
```

Output ends with `Verified OK`. The identity is exact and names
`_release_harden.yml`, because the Fulcio certificate's identity is the
workflow that ran the signing step (its Build Config URI extension names the
calling `release.yml`); the tag is bare `X.Y.Z`. The Sigstore transparency log
(`https://search.sigstore.dev/`) carries the same record.

### 3) Verify every file against it

```bash
sha256sum --ignore-missing -c SHA256SUMS         # Linux
shasum -a 256 --ignore-missing -c SHA256SUMS     # macOS
```

Every line must say `OK`. A failure means the file was modified after
release. One file's own bundle verifies the same way as step 2:

```bash
cosign verify-blob \
  --bundle dish_X.Y.Z_amd64.deb.sigstore.json \
  --certificate-identity "https://github.com/TinkerNorth/dish-linux/.github/workflows/_release_harden.yml@refs/tags/$TAG" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  dish_X.Y.Z_amd64.deb
```

### Releases up to 2.8.1: `.sig` and `.crt`

Releases signed before the bundles carry a detached signature and a
certificate per file. cosign 3 still verifies them with the older flags (it
prints two deprecation lines). Run against 2.8.1, these print `Verified OK`:

```bash
TAG=2.8.1
cosign verify-blob \
  --certificate SHA256SUMS.crt --signature SHA256SUMS.sig \
  --certificate-identity "https://github.com/TinkerNorth/dish-linux/.github/workflows/_release_harden.yml@refs/tags/$TAG" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  SHA256SUMS
shasum -a 256 -c SHA256SUMS    # 13 lines, all OK for 2.8.1
```

### 4) The SLSA L3 provenance

`dish-linux.intoto.jsonl` is a Sigstore bundle whose statement names every
released file by SHA-256. It is signed by the SLSA generator at the exact
commit the workflow pins, which `slsa-verifier` refuses ("invalid ref") because
that is not a release tag, so verify it with cosign, naming the generator's
commit from the provenance's `builder.id`:

```bash
cosign verify-blob-attestation \
  --bundle dish-linux.intoto.jsonl --type slsaprovenance \
  --certificate-identity "https://github.com/slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml@f7dd8c54c2067bafc12ca7a55595d5ee9b75204a" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  dish_X.Y.Z_amd64.deb
```

(`Verified OK` against dish-linux 2.8.1. The commit is the generator v2.1.0 the
workflow pins; use the one in the provenance you are checking.)

### 5) (Optional) Inspect the SBOM

```bash
# Top-level summary
jq '.packages | length' dish.sbom.spdx.json

# Or diff against the last release
diff <(jq -S . prev-release/dish.sbom.spdx.json) \
     <(jq -S . release/dish.sbom.spdx.json) \
  | less
```

---

## Known gaps

- **Vendored-header scanners.** `satellite/lib/` and
  `satellite/vigem/include/` are not understood by ecosystem scanners. We
  feed OSV-Scanner a synthetic `osv-scanner.toml` derived from
  [`lib/VENDORED.md` in TinkerNorth/satellite](https://github.com/TinkerNorth/satellite/blob/main/lib/VENDORED.md),
  and the `vendored-freshness` CI job fails if any `Last-vendored:` date is
  more than 90 days old. dish-linux names its vendored ENet and the sources it
  builds (SDL2, libsodium, sentry-native, Catch2) by upstream commit the same
  way, through `scripts/osv-lockfile.sh`.
- **macOS virtual pads are entitlement-gated.** The macOS satellite backend
  synthesizes virtual DualShock 4 controllers via `IOHIDUserDevice`, which
  requires the `com.apple.developer.hid.virtual.device` entitlement.
  Production (signed) builds carry it; CI artifacts and local unentitled
  builds detect the missing entitlement at run time and fall back to an
  inert backend: the protocol stack runs, but every controller descriptor
  applies as `backendUnavailable`. Don't open a vulnerability report for the
  absence of virtual-gamepad creation in an unentitled build.
