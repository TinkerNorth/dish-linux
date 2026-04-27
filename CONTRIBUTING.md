# Contributing to Dish Linux

Thanks for your interest in improving the Linux client! This document
captures the conventions that aren't obvious from skimming the code.

## Getting set up

```bash
# 1) Install build deps for your distro (see README "Install build dependencies")
# 2) Generate compile_commands.json + run the test suite
scripts/build.sh debug test
# 3) Point git at the in-tree pre-commit hook
scripts/setup-hooks.sh
```

The pre-commit hook runs `clang-format -i` (autofix, re-stages) and
`clang-tidy -p build-debug` (advisory) on staged C++ files. It skips
gracefully if the tools aren't installed — CI re-runs `clang-format
--dry-run --Werror` and `clang-tidy` in strict mode, so anything that
slips locally fails the PR.

## License headers

Every source file (`*.h`, `*.hpp`, `*.cpp`) starts with:

```cpp
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
```

New files must include both lines. Don't introduce code under a different
license — the project is LGPL-3.0-or-later end-to-end (`LICENSE`,
`COPYING.GPL3`, source headers).

## Style

- C++17, four-space indent, 100-column soft limit. `.clang-format` is
  authoritative — run `clang-format -i` if you're unsure.
- Warnings are enforced as errors on first-party targets (`dish_strict`).
  See `CMakeLists.txt` for the exact set; in short:
  `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Wconversion -Wsign-conversion -Wdouble-promotion -Wformat=2`.
- Match the surrounding style. Headers go in the order: project, Qt, libs,
  std, separated by blank lines (see `src/AppModel.h` for the pattern).

## Branching & PRs

- All changes land on `main` via pull request — no direct pushes.
- Use the PR template (`.github/pull_request_template.md`) to describe
  the change, the manual test matrix you ran, and call out anything that
  touches the wire protocol.
- Keep commits focused; squash noisy fixup commits before review.

## What CI runs

Build + style:

- `linux-ci.yml`: `clang-format --dry-run --Werror`, Debug build + `ctest`
  (Catch2 suite under `tests/`), `clang-tidy -p build-debug` over `src/`,
  Release build that uploads `dish` as a CI artifact.

Security gates (also blocking):

- `security.yml`: action-pin lint, vulnerability allowlist expiry,
  OSV-Scanner against the worktree, gitleaks secret scan, GitHub
  `dependency-review-action`.
- `codeql.yml`: CodeQL `cpp` analysis (security-extended +
  security-and-quality query packs).

Reproduce build steps locally with `scripts/build.sh debug test`.

## Security

### Adding a vulnerability allowlist entry

Open a PR that adds an entry to [`.security/allowlist.yaml`](.security/allowlist.yaml)
(see the schema in the file). Required fields: `cve`, `reason`, `owner`,
`expires`. CI rejects the PR if any field is missing or `expires` is in
the past. Renew or remove on or before `expires` — there is no silent
suppression.

### Running security checks locally

```bash
# Action-pin lint (40-char SHA enforcement on every uses: line)
grep -REn '^\s*uses:' .github/workflows/ \
  | grep -vE '@[0-9a-f]{40}\b' \
  || echo "all pinned"

# Allowlist expiry
python3 - <<'PY'
import datetime, yaml, sys
data = yaml.safe_load(open('.security/allowlist.yaml').read()) or {}
for e in data.get('exceptions', []) or []:
    if datetime.date.fromisoformat(str(e['expires'])) < datetime.date.today():
        print('EXPIRED:', e); sys.exit(1)
PY

# OSV-Scanner
osv-scanner --recursive --skip-git .

# Gitleaks
gitleaks detect --no-banner --redact --source .
```

### Verifying a release artifact

Each GitHub Release ships the `.deb` + `.AppImage`, `*.sig`/`*.crt`
(cosign keyless), `SHA256SUMS` + `SHA256SUMS.sig`/`*.crt`, the SPDX
+ CycloneDX SBOMs, and `dish-linux.intoto.jsonl` (SLSA L3 provenance).

```bash
sha256sum -c SHA256SUMS

cosign verify-blob \
  --certificate SHA256SUMS.crt \
  --signature   SHA256SUMS.sig \
  --certificate-identity-regexp '^https://github\.com/TinkerNorth/dish-linux/\.github/workflows/release\.yml@refs/tags/v.*$' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
  SHA256SUMS

slsa-verifier verify-artifact \
  --provenance-path dish-linux.intoto.jsonl \
  --source-uri      github.com/TinkerNorth/dish-linux \
  --source-tag      vX.Y.Z \
  dish_X.Y.Z_amd64.deb
```

The full cross-repo verification recipe lives in
[`SECURITY.md`](SECURITY.md).

## Touching the hot path

The SDL gamepad thread runs at controller polling rate and must never
block on the UI thread or take a heap allocation. If you're modifying
`SDLGamepadBridge`, `GamepadInputProcessor`, or `SatelliteClient::sendReport`:

- No `QObject::connect` cross-thread signals on the send path.
- No `std::mutex` longer than the existing routing-table lookup.
- No allocations per packet — use the preallocated buffer.
- Preserve `IP_TOS = 0xB8` (DSCP EF) and `MSG_NOSIGNAL` on every send.

## Touching the wire protocol

The Linux, macOS, and Android clients all talk to the same `satellite`
server and must produce byte-identical traffic:

- AEAD: ChaCha20-Poly1305 IETF, 12-byte big-endian nonce derived from a
  monotonic counter.
- Packet layout: `token(4) | counter(4) | ciphertext+tag`, with the
  4-byte token as AAD.
- XUSB report: 12 bytes, little-endian.
- Ports: discovery UDP 9879, pairing TCP 9878, HTTP TCP 9877,
  streaming UDP 9876.

Any change here must be coordinated with `dish-android`, `dish-mac`, and
`satellite` in the same PR / release cycle.

## clang-tidy triage

`.clang-tidy`'s `WarningsAsErrors: ''` is intentional: clang-tidy is run
in CI as an advisory linter, not a gate. The remaining warnings on the
non-UI sources are all stylistic and tracked here for future cleanup
PRs:

| Check                                       | Notes                                          |
| ------------------------------------------- | ---------------------------------------------- |
| `modernize-use-nodiscard`                   | Add `[[nodiscard]]` to value-returning getters |
| `modernize-use-scoped-lock`                 | `std::lock_guard` → `std::scoped_lock`         |
| `readability-braces-around-statements`      | Single-statement `if`/`for` bodies             |
| `readability-identifier-naming`             | `Theme::` constexpr need the `k` prefix        |
| `cppcoreguidelines-avoid-c-arrays`          | Mostly fixed-size buffers — review case-by-case |
| `cppcoreguidelines-special-member-functions` | Rule-of-five on classes that own resources    |
| `performance-enum-size`                     | Underlying type narrower than `int`            |

Suppressions intentionally enabled in `.clang-tidy`:

- `-portability-avoid-pragma-once` — the project uses `#pragma once`
  everywhere by convention.

Anything new should land at zero net additional warnings on the
non-UI scope (`find src -name '*.cpp' ! -path 'src/UI/*' | xargs
clang-tidy -p build-debug --quiet`). UI files are excluded from CI's
clang-tidy step because Qt's MOC-generated code triggers a long tail
of false positives.

## Reporting bugs

Use the issue templates under `.github/ISSUE_TEMPLATE/`. Include the
distro + Qt/SDL/libsodium versions (`scripts/build.sh debug` prints
them at the top of the configure step) and a `journalctl --user -e`
excerpt if the app crashed.
