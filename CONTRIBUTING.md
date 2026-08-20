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

## Where code goes

The app is a unidirectional-dataflow core with a Qt Quick projection on top.
Before writing a class, pick the primitive that matches what it actually does —
subclassing the wrong one is the commonest architectural mistake here.

| You have… | Use | Lives in |
|---|---|---|
| a `(state, event) -> result` decision with no IO | a free function | `src/core/reducer/` |
| a domain value reshaped for the UI | a mapper, also a free function | `src/core/` |
| state owned from a socket, timer, cache or setting | `StateSource<S>` | `src/source/` |
| one value purely derived from other Observables | `Composer<Out, Ins...>` | `src/composer/` |
| a side effect driven by a state | `Controller<S>` | `src/composer/` |
| durable keyed storage | `Repository<K,V>` | `src/repository/` |
| an IO or native boundary with no domain state | a `*Gateway` | `src/source/` |
| imperative commands spanning several sources | a `*Coordinator` | `src/composer/` |

The rules, and why each exists, are in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
[`src/architecture/README.md`](src/architecture/README.md). Two that catch people
out: a composer never performs IO (if it needs a socket you are writing a
source), and a coordinator never becomes the source of truth for state another
class already owns (a mirror is a second writer).

`src/core/` and `src/architecture/` are Qt-free where they can be and platform-
free always. Nothing below `src/qml/` may know the UI exists.

## Touching the UI

Every design value reaches its callsite through a `Theme` or `Tokens` name.
`src/qml/kit/` is the one layer that turns tokens into pixels; everything else
composes kit components. The rules are review-blocking and listed in
[`docs/QML_UI_KIT.md`](docs/QML_UI_KIT.md) — in particular, a page may not
declare an inline `component`, and every state a component can be in has to
appear in `KitGallery.qml`.

`scripts/qml-lint-literals.sh` catches a hard-coded `#4FE3FF` or `radius: 8`,
which still renders and silently stops tracking the palette. It errors for
`src/qml/wizard/**` and `src/qml/shared/**` and warns elsewhere.

Anything QML reads or calls on `App` is listed in
[`docs/QML_CONTRACT.md`](docs/QML_CONTRACT.md). That document is a compensating
control, not documentation: `App` is a runtime context property that `qmllint`
cannot see, so a reference to it is checked against that table rather than by
the linter. Add new surface there in the same commit.

## Translations

Six catalogues in `translations/`. A new user-facing string needs a catalogue
entry in the same commit — `scripts/check-translations.sh` re-runs `lupdate` in
CI and fails on any diff. Run it locally and commit the result.

It needs Qt 6.9 or newer, the version CI pins in `.github/actions/setup-qt`.
Before 6.9, `lupdate` drops the namespace from a class whose definition and
member bodies sit in different files: `dish::net::WifiConnectionManager` comes
back out as `WifiConnectionManager`, which is not the context `moc` hands
`tr()` at run time, so the entry it writes could never be looked up. The script
refuses to run on an older one rather than let that land.

Coverage is reported, never enforced: translating a string is a separate act
from extracting it. English is a real catalogue rather than the untranslated
fallback, because a `%n` message carries one source string but needs one plural
form per category and Bosnian has three.

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

Build + style — `linux-ci.yml`, four jobs:

- `ci`, once per compiler (gcc and clang): `clang-format --dry-run --Werror`,
  Debug build + `ctest`, `qmllint` over every tracked QML file,
  `scripts/qml-lint-literals.sh`, `scripts/check-translations.sh`,
  `clang-tidy -p build` over `src/` excluding `src/UI/` and `src/qml/`, then a
  Release build that ALSO runs `ctest` and is checked for RELRO, BIND_NOW, a
  non-executable stack and PIE. The lint steps run on the gcc leg only; the
  clang leg exists because `-Wconversion`, `-Wshadow` and `-Wold-style-cast`
  diverge materially between the two compilers.
- `sanitize`: the suite under ASan+UBSan and under TSan. TSan is not optional
  here — four long-lived threads share atomics and mutexes, and nothing else in
  the pipeline would see a race.
- `package`: builds the `.deb` from the install rules in a `debian:trixie`
  container, runs `desktop-file-validate`, `appstreamcli validate` and
  `lintian --fail-on error`, asserts the payload, then **installs the package
  and launches it**. That last step is the only gate that can catch a missing
  runtime dependency: every other check passes against a build tree, which is
  exactly where a QML module resolves from the Qt install rather than from a
  `Depends:` line.
- `coverage`: lcov over the Debug suite, summarised into the job page.

`version-consistency.yml` fails a PR that moves the version in one place and not
the others. `CMakeLists.txt`'s `project(Dish VERSION ...)` is the source; the
mirrors are `packaging/com.tinkernorth.Dish.metainfo.xml`'s `<release>`, the
`CHANGELOG.md` heading, and — for the Qt floor — `docs/PACKAGING.md`,
`README.md`, `THIRD_PARTY.md` and `assets/licenses/licenses.json`.

Security gates (also blocking):

- `security.yml`: action-pin lint, vulnerability allowlist expiry,
  OSV-Scanner against the worktree, gitleaks secret scan, GitHub
  `dependency-review-action`.
- `codeql.yml`: CodeQL `cpp` analysis (security-extended +
  security-and-quality query packs).

`scripts/ci_local.sh` runs those gates in the same order against your worktree,
so a green run there means a green run in CI. `--no-tidy` skips the slowest step
for a fast loop; `--with-package` adds the CPack/lintian leg. A gate whose tool
is missing FAILS rather than printing a notice — pass `--allow-missing` if you
really want to skip it, and know that you did.

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
- Ports: discovery UDP 9879, pairing and REST HTTPS 9443, streaming UDP 9876.

Any change here must be coordinated with `dish-android`, `dish-mac`,
`dish-windows` and `satellite` in the same PR / release cycle. The
authoritative contract is
[`satellite/docs/contract.md`](https://github.com/TinkerNorth/satellite/blob/main/docs/contract.md);
the client-side mirror is [`src/core/model/Protocol.h`](src/core/model/Protocol.h).

## clang-tidy

**The non-UI scope is clean, and CI gates it at zero.** `linux-ci.yml` runs
clang-tidy with `--warnings-as-errors='*'` over every `src/**.cpp` and
`src/**.h` outside `src/UI/` and `src/qml/`, so a new finding fails the build.

`.clang-tidy` itself keeps `WarningsAsErrors: ''` because it is the
fleet-canonical config shared with `dish-android`, `dish-mac` and
`dish-windows`, whose scopes are not clean. The gate lives in the workflow, not
in the config, so this repo can hold a higher bar without forking the file.

`src/UI/` and `src/qml/` are excluded: Qt's MOC- and qmltyperegistrar-generated
code triggers a long tail of false positives that no source change can fix.

Reproduce locally. Reads the same `build/` the other gates use — CMakeLists exports the compile
database globally, so a second tree only re-derives the same `src/` entries and
costs another full build:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDISH_BUILD_TESTS=ON
cmake --build build --parallel
find src -type f \( -name '*.cpp' -o -name '*.h' \) \
  ! -path 'src/UI/*' ! -path 'src/qml/*' -print0 |
  xargs -0 -n1 -P"$(nproc)" clang-tidy -p build --quiet --warnings-as-errors='*'
```

When a check is a genuine false positive — two switch arms that share an answer
for different documented reasons, an SDL struct tag whose leading underscore is
not ours — suppress it with a `NOLINTBEGIN`/`NOLINTEND` pair naming the check
**and** a comment saying why. A bare `NOLINT` with no reason will be asked about
in review.

Suppressions intentionally enabled in `.clang-tidy`:

- `-portability-avoid-pragma-once` — the project uses `#pragma once` everywhere
  by convention.

## Reporting bugs

Use the issue templates under `.github/ISSUE_TEMPLATE/`. Include the
distro + Qt/SDL/libsodium versions (`scripts/build.sh debug` prints
them at the top of the configure step) and a `journalctl --user -e`
excerpt if the app crashed.
