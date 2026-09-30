# Contributing to Dish Linux

Thanks for your interest in improving the Linux client! This document
captures the conventions that aren't obvious from skimming the code.

## Getting set up

```bash
# 1) Install the toolchain CI uses (apt; --ci-qt adds CI's exact Qt 6.9.3)
scripts/install-deps.sh
# 2) Generate compile_commands.json + run the test suite (debug preset -> build/)
scripts/build.sh debug test
# 3) Point git at the in-tree pre-commit hook
scripts/setup-hooks.sh
# 4) Before pushing: every CI gate, in CI's order
scripts/ci-local.sh
```

`CMakePresets.json` is the single source of configure truth: the `debug`,
`release` and `package` presets are what `linux-ci.yml`, `codeql.yml` and
`release.yml` drive, and the `scripts/` wrappers drive the same ones. The
debug preset writes to `build/` (CI's tree name; this repo used
`build-debug/` before the presets existed).

The pre-commit hook runs `clang-format -i` (autofix, re-stages) and
`clang-tidy -p build` (advisory) on staged C++ files. It skips
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
[`docs/QML_CONTRACT.md`](docs/QML_CONTRACT.md). `App` is a `Dish.Chrome`
singleton, so `qmllint` does check every reference to it and a typo fails CI;
the document is the readable index of the same surface, and the place a
reviewer looks to see whether a new property belongs there at all. Add new
surface there in the same commit.

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

Coverage is **enforced**. `check-translations.sh` fails when any catalogue has
an unfinished entry, because a missing translation is not a blank — it is
English shown to someone who does not read English, and nothing else in the
suite can see that. Extracting a string and translating it are one commit, not
two.

English is a real catalogue rather than the untranslated fallback, because a
`%n` message carries one source string but needs one plural form per category
and Bosnian has three. Its non-plural entries are filled from their own source
text by `scripts/seed-source-language.py`, which the gate runs for you — so the
count means the same thing in all six catalogues, and the only English entries
a person writes are the plural forms that genuinely need a decision.

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

- C++20, four-space indent, 100-column soft limit. `.clang-format` is
  authoritative — run `clang-format -i` if you're unsure.
- Warnings are enforced as errors on first-party targets (`dish_strict`).
  See `CMakeLists.txt` for the exact set; in short:
  `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Wconversion -Wsign-conversion -Wdouble-promotion -Wformat=2`.
- Match the surrounding style. Headers go in the order: project, Qt, libs,
  std, separated by blank lines (see `src/AppModel.h` for the pattern).

### Shape of the code

These rules are enforced in review, not by a gate. They come from the
Parchment library and are adapted where C++ or Qt make the literal form worse
than the thing it is meant to achieve. They apply to `src/` and `tests/`
alike, and they are the same rules dish-windows follows, so a change ported
between the two desktop clients keeps its shape.

- **As immutable and as static as possible.** `const` on every local and
  parameter that is not reassigned, `constexpr` for a value known at compile
  time, and file-static (or an anonymous namespace) for anything the rest of
  the translation unit does not need. A value that never changes is a named
  constant, never a literal in the middle of a function. A type that holds no
  state is a set of free functions, not a class.

- **Split values into simple, named steps.** One operation per line, with the
  result in a named `const` that says what it is, even when that reads longer:

  ```cpp
  const bool isAnotherSlot = entry.first != deviceId;
  const bool isAPlaceholder = other.transitioning || other.needsReplug;
  const bool isTheSameModel = other.vendorId == device.vendorId &&
                              other.productId == device.productId;
  return isAnotherSlot && isAPlaceholder && isTheSameModel;
  ```

  not one five-term boolean. The names are the documentation, the debugger can
  show each value, and a test can pin each step. A named `const bool` costs
  nothing at runtime.

- **One function, one flow.** When a function would hold two algorithms chosen
  by a condition, the condition dispatches to two named things that each do
  one thing, and the dispatcher does nothing else. A `switch` over an enum
  with no `default` is the preferred form, because the compiler then checks
  that every case is handled. A guard clause is not an algorithm: do not
  invent indirection where there is only one flow.

  A function that stays long because splitting it would make it worse says so
  at the top, in one or two lines. A long function with no such note is one
  nobody has looked at.

- **A chain of `if`s over one byte is a table.** A per-bit or per-index
  mapping belongs in a `constexpr` array the code reads, not in a switch the
  reader has to diff against its twin. The point is that two mappings of the
  same thing cannot drift apart.

- **A callback with a body gets a name.** A lambda is fine as a one-expression
  forward, and fine as an argument to an algorithm that consumes it
  immediately (`std::sort`, `std::find_if`, `std::visit`). A lambda that
  carries an algorithm becomes a named function, so it can be found, read and
  tested on its own. A callback that is *stored* rather than called
  immediately -- a `QObject::connect` slot, a thread body, a
  `std::function` member -- prefers a named member function and a pointer to
  it:

  ```cpp
  QObject::connect(aliveTimer_, &QTimer::timeout, this, &WifiConnection::onAliveTick);
  ```

  This is Parchment's "no anonymous methods" narrowed to what C++ can
  express.

- **No singletons.** A stateless helper is a free function in the file that
  owns it. There is no `instance()`, no `Q_GLOBAL_STATIC` and no process-wide
  mutable object here: every collaborator is constructed by `AppModel` and
  handed to whoever needs it, which is also what makes it replaceable in a
  test. The statics that remain are process-wide by definition, and each is
  there because something outside this code demands it:

  - the once-per-process registrations with Qt's type and resource systems,
    and the one-time settings migration, all spelled `std::call_once`;
  - the sequence that keeps this process's D-Bus connection names unique;
  - the hand-off pointer in `src/qml/chrome/ForeignTypes.h`, because a
    `QML_SINGLETON`'s factory is a static function Qt calls, and it can only
    return what was published to it;
  - `main.cpp`'s `QTranslator`, which `QCoreApplication` holds by pointer.

  A function-local `static` of any other kind is a singleton with the
  constructor hidden, and does not belong here.

- **No discarded results.** A value is either used or not produced. A
  `static_cast<void>(x)` or `(void)x` exists only to quiet a warning about
  something that should not be there, so the warning is the thing to fix.

- **Member naming.** Members carry a TRAILING underscore (`host_`, `probes_`,
  `mtx_`), which is what every class in `src/` uses. It is the same "state,
  not scratch" signal Parchment's `m` prefix gives at the point of use.

- **Prefer a test to a comment.** Behaviour that needs explaining gets a test
  named for the behaviour. A comment is the last resort for a constraint that
  genuinely cannot be tested -- a platform quirk, a wire-format byte layout, a
  lock order, a declaration-order dependency -- states why in one or two
  lines, and never narrates what the next line does.

  Exempt, because the constraint is untestable by construction: the pin-map
  headers in `.github/workflows/`, the usage headers in `scripts/`, and the
  manifests under `packaging/`, where a comment explains what a pin or a
  packaging rule is holding back.

### Test-driven, every flow

The Catch2 suite under `tests/` is not a coverage exercise. It is how a flow
is known to work at all.

- **Red first.** Write the failing test, watch it fail for the reason you
  expect, then write the code. A test that has never failed has not been
  shown to test anything; when adding a test for behaviour that already
  exists, break the behaviour on purpose and watch the test catch it.
- **Cover every flow.** Each branch a function can take gets a case named for
  the behaviour it pins, not for the function it calls. `SECTION` is the right
  tool when the cases share a fixture; a separate `TEST_CASE` is right when
  they do not.
- **Assume nothing.** Where behaviour depends on a platform, a library
  version or the wire, prove it with a probe before writing the code that
  assumes it, and name the probe's finding in the test.
- **Test where the behaviour lives.** Most of `src/core/` and
  `src/composer/` is deliberately Qt-free so it is host-testable without a
  window. Keep it that way: a reducer that needs a `QGuiApplication` to be
  tested is a reducer with a dependency it should not have. What does need
  the network - pairing, the Moonlight host, the update feed - is tested
  against a real listener on loopback, not a mock of the client's own calls.
- **Knowledge written twice is checked by a test.** When the same fact lives
  in two places - an enum and the names QML binds to it, a struct's fields
  and its `operator==`, a table and its twin in another repo - a test walks
  one and requires the other, from the metaobject or the serialized form
  rather than from a third list kept in the test.
- **Tests follow the same shape rules.** A fixture is a named type, not a
  lambda that builds one; a helper with a body gets a name; a test that is
  longer than the thing it tests is usually two tests.

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

`scripts/ci-local.sh` runs those gates in the same order against your worktree
(through the same presets and `scripts/check-format.sh` the workflow calls), so
a green run there means a green run in CI. `--no-tidy` skips the slowest step
for a fast loop; `--with-package` adds the CPack/lintian leg;
`--with-sanitizers` adds the ASan/UBSan and TSan legs; `--compiler gcc|clang`
sets CC/CXX so you can reproduce either side of CI's compiler matrix (one run
covers one compiler). A gate whose tool is missing FAILS rather than printing
a notice — pass `--allow-missing` if you really want to skip it, and know that
you did. `scripts/ci_local.sh` remains as a forwarder for muscle memory.

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
cmake --preset debug
cmake --build --preset debug --parallel
find src -type f \( -name '*.cpp' -o -name '*.h' \) \
  ! -path 'src/UI/*' ! -path 'src/qml/*' -print0 |
  xargs -0 -n1 -P"$(nproc)" clang-tidy -p build --quiet --warnings-as-errors='*'
```

`src/` and `tests/` carry no `NOLINT` of any kind, and a change that adds one
will be asked to fix what the check points at instead. Every finding met so
far had a source answer: two switch arms with the same body become one arm
with both reasons in its comment; a reserved struct tag that is not ours goes
away by including the upstream header, or by keeping the type out of the
header entirely when that header cannot reach the dependency; a demarshalling
operator that returns its own parameter deletes its rvalue overload, so the
dangling case stops compiling; and a pointer handed to a size-aware callee is
spelled with its length at the call. Third-party and generated code stays off
the lint wall by target rather than by markers in the source: see the vendored
ENet library and the qmlcachegen carve-out in `CMakeLists.txt`.

Suppressions intentionally enabled in `.clang-tidy`:

- `-portability-avoid-pragma-once` — the project uses `#pragma once` everywhere
  by convention.

## Reporting bugs

Use the issue templates under `.github/ISSUE_TEMPLATE/`. Include the
distro + Qt/SDL/libsodium versions (`scripts/build.sh debug` prints
them at the top of the configure step) and a `journalctl --user -e`
excerpt if the app crashed.
