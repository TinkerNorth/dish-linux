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

- Action-pin lint: every `uses:` line must reference a 40-character SHA.
- Allowlist expiry: `.security/allowlist.yaml` entries must be unexpired.
- Dependency review: GitHub advisory database (PR only).
- OSV-Scanner: vendored components and manifest dependencies, scoped per
  ecosystem (see each repo's `security.yml`).
- Gitleaks: secret scanning over the worktree.
- CodeQL: `cpp` for `satellite`, `dish-windows` and `dish-linux`, `swift`
  for `dish-mac` and `dish-ios`, `java-kotlin` and `cpp` for `dish-android`.

**On every tagged release** (also blocking):

- A re-run of every PR-time gate against the tagged commit.
- Required-secrets gate: refuses to publish if the platform signing secret
  is missing for a tag (Windows Authenticode, Apple Developer ID and
  notarization, Android keystore). Linux binaries carry no platform
  signature, so for `dish-linux` the gate enforces that the cosign keyless
  flow can authenticate.
- Artifact-level vulnerability scan: Anchore Grype, fails on CRITICAL and
  HIGH.
- SBOM generation: Syft, both SPDX-JSON and CycloneDX-JSON.
- `SHA256SUMS` over every artifact, its signatures and the SBOMs.
- Cosign keyless signing: every artifact and `SHA256SUMS` get a `.sig` and
  a `.crt`, anchored in the Sigstore transparency log.
- SLSA L3 build provenance: `slsa-framework/slsa-github-generator` emits
  `<repo>.intoto.jsonl`.

A known-vulnerable dependency, a missing signature or a tampered binary fails
the release before any artifact lands on the GitHub Release page.

---

## Verifying a release artifact (consumer recipe)

The commands below are for Dish for Linux and were run against release
2.8.1. The other repos follow the same steps with their own file names and
signing workflow; each repo's own `SECURITY.md` carries its recipe.

### 0) Pre-requisites

```bash
# cosign (2.x or 3.x; both verify these signatures)
brew install cosign            # macOS
go install github.com/sigstore/cosign/v3/cmd/cosign@latest

# slsa-verifier (for the SLSA provenance step)
go install github.com/slsa-framework/slsa-verifier/v2/cli/slsa-verifier@latest
```

### 1) Download every file from the Release

For tag `X.Y.Z`:

```bash
gh release download X.Y.Z -R TinkerNorth/dish-linux -D ./release
cd release
ls
```

You should see, for 2.8.1:

```
dish_2.8.1_amd64.deb          dish-amd64.deb          # the .deb, versioned and stable name
dish-2.8.1-1.x86_64.rpm       dish-x86_64.rpm         # the .rpm
Dish-2.8.1-x86_64.AppImage    Dish-x86_64.AppImage    # the AppImage, plus .zsync and .sha256
Dish-2.8.1-x86_64.flatpak     Dish-x86_64.flatpak     # the Flatpak bundle
latest.json                                           # the in-app update manifest
*.sig  *.crt                                          # a cosign signature and certificate per file
SHA256SUMS  SHA256SUMS.sig  SHA256SUMS.crt
dish.sbom.spdx.json  dish.sbom.cdx.json
dish-linux.intoto.jsonl                               # SLSA L3 provenance
measurements-2.8.1.md  measurements-2.8.1.json        # attached after hardening, not in SHA256SUMS
```

### 2) Verify the checksum bundle

```bash
sha256sum -c SHA256SUMS         # Linux
shasum -a 256 -c SHA256SUMS     # macOS
```

Every line must say `OK` (13 lines for 2.8.1). A failure here means the
artifact was modified after release.

### 3) Verify cosign signed `SHA256SUMS` from the right workflow

The signing step runs in the reusable `_release_harden.yml` that
`release.yml` calls, so that workflow is the certificate identity:

```bash
cosign verify-blob \
  --certificate SHA256SUMS.crt \
  --signature   SHA256SUMS.sig \
  --certificate-identity-regexp '^https://github\.com/TinkerNorth/dish-linux/\.github/workflows/_release_harden\.yml@refs/tags/[0-9]+\.[0-9]+\.[0-9]+$' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
  SHA256SUMS
```

Output ends with `Verified OK`. The identity binds the signature to that
workflow on `TinkerNorth/dish-linux` at a release tag; the Sigstore
transparency log (`https://search.sigstore.dev/`) carries the same record.

To verify each package individually (not just `SHA256SUMS`):

```bash
for f in *.deb *.rpm *.AppImage *.flatpak; do
  [ -f "$f" ] || continue
  cosign verify-blob \
    --certificate "$f.crt" \
    --signature   "$f.sig" \
    --certificate-identity-regexp '^https://github\.com/TinkerNorth/dish-linux/\.github/workflows/_release_harden\.yml@refs/tags/[0-9]+\.[0-9]+\.[0-9]+$' \
    --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
    "$f"
done
```

### 4) Verify the SLSA L3 provenance

```bash
slsa-verifier verify-artifact \
  --provenance-path dish-linux.intoto.jsonl \
  --source-uri      github.com/TinkerNorth/dish-linux \
  --source-tag      X.Y.Z \
  dish_X.Y.Z_amd64.deb
```

This is meant to end with `PASSED: SLSA verification passed`. It does not
today: see [Known gaps](#known-gaps).

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

- **The SLSA provenance does not verify with slsa-verifier.** Against 2.8.1,
  step 4 fails with `invalid ref: "f7dd8c54c2067bafc12ca7a55595d5ee9b75204a":
  unexpected ref type: ""`. `release.yml` calls the SLSA generator by commit
  SHA, and slsa-verifier accepts only a generator called at a release tag.
  The provenance file is genuine and signed; until the call changes, steps 2
  and 3 are the verification that passes.
- **Vendored-header scanners.** `satellite/lib/` and
  `satellite/vigem/include/` are not understood by ecosystem scanners. We
  feed OSV-Scanner a synthetic `osv-scanner.toml` derived from
  [`lib/VENDORED.md` in TinkerNorth/satellite](https://github.com/TinkerNorth/satellite/blob/main/lib/VENDORED.md),
  and the `vendored-freshness` CI job fails if any `Last-vendored:` date is
  more than 90 days old. dish-linux and dish-windows vendor ENet under
  `third_party/enet/`, which OSV-Scanner cannot see either; its pinned commit
  is recorded in `THIRD_PARTY.md`.
- **macOS virtual pads are entitlement-gated.** The macOS satellite backend
  synthesizes virtual DualShock 4 controllers via `IOHIDUserDevice`, which
  requires the `com.apple.developer.hid.virtual.device` entitlement.
  Production (signed) builds carry it; CI artifacts and local unentitled
  builds detect the missing entitlement at run time and fall back to an
  inert backend: the protocol stack runs, but every controller descriptor
  applies as `backendUnavailable`. Don't open a vulnerability report for the
  absence of virtual-gamepad creation in an unentitled build.
