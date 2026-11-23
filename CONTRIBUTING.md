# Contributing to Dish for Linux

Thanks for your interest in improving the Linux client. This document captures
the conventions that are not obvious from reading the code: the gates, where
code goes, the shape of the code, and what CI enforces.

## Code of conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). By
participating, you agree to uphold it. Report unacceptable behavior to
`security@tinkernorth.com`.

## Getting set up

```bash
# 1) Install the toolchain CI uses (apt; --ci-qt adds CI's exact Qt 6.12.1)
scripts/install-deps.sh
# 2) Generate compile_commands.json and run the test suite (debug preset, build/)
scripts/build.sh debug test
# 3) Point git at the in-tree pre-commit hook
scripts/setup-hooks.sh
# 4) Before pushing: every CI gate, in CI's order
scripts/ci-local.sh
```

`CMakePresets.json` is the single source of configure truth: the `debug`,
`release` and `package` presets are what `linux-ci.yml`, `codeql.yml` and
`release.yml` drive, and the `scripts/` wrappers drive the same ones. The
debug preset writes to `build/` (CI's tree name).

What a build needs, which `scripts/install-deps.sh` installs from apt and
`CMakeLists.txt` checks for:

- a C++20 compiler (CI builds with gcc and clang), CMake 3.21 or newer, Ninja
  and pkg-config;
- Qt 6.7 or newer: Core, Gui, Network, DBus, Svg, Quick, Qml, QuickControls2
  and Concurrent, plus Linguist tools for the translation catalogues;
- libsodium, SDL2 2.24 or newer, Opus and OpenSSL (libcrypto);
- Catch2 v3 for the tests, fetched at the pinned commit when the system has
  none;
- optionally `rsvg-convert`, which renders the launcher icons below 512x512
  from the SVG. Without it the build says so and installs only the scalable
  and 512x512 icons, which is enough for a working menu entry.

`scripts/tools.sh` is the single source of every pinned version: Qt 6.12.1
and aqtinstall for CI and the AppImage, clang-format, SDL2, sentry-native and
sentry-cli, the AppImage tools with their SHA-256s, the KDE Flatpak runtime
and the package containers. The workflows, the composite actions and the
scripts source it; the copies that cannot (the Flatpak manifests,
`CMakeLists.txt`'s FetchContent pin, the documents) are held to it by
`scripts/check-version-consistency.sh`. A bump is one edit there plus the
copies that check names.

The pre-commit hook runs `clang-format -i` (autofix, re-stages) and
`clang-tidy -p build` (advisory) on staged C++ files. It skips the tools that
are not installed; CI re-runs `clang-format --dry-run --Werror` and
`clang-tidy` in strict mode, so anything that slips locally fails the PR.

### Running CI locally

`scripts/ci-local.sh` runs the gates of `linux-ci.yml`'s `ci` job in the same
order, through the same scripts the workflow steps call
(`check-version-consistency.sh`, `check-format.sh`, `check-qml.sh`,
`qml-lint-literals.sh`, `check-translations.sh`, `check-tidy.sh`,
`check-hardening.sh`), so a green run there is a green run in CI. A gate
whose tool is missing FAILS; pass `--allow-missing` to skip it, and know that
you did. A clang-format other than the pinned one fails too, because its
verdict is not CI's.

| Flag | Adds or changes |
|---|---|
| `--quick` | skips clang-tidy and the Release leg (fastest loop) |
| `--compiler gcc\|clang` | sets CC/CXX; one run covers one leg of CI's compiler matrix |
| `--with-sanitizers` | the ASan+UBSan and TSan legs and the lock laws under each (`scripts/sanitize.sh`) |
| `--with-lock-stats` | the lock-statistics build (`scripts/lock-stats.sh`) |
| `--with-coverage` | the lcov build (`scripts/coverage.sh`) |
| `--with-sentry` | the build with the Sentry SDK and a DSN that resolves nowhere (`scripts/check-sentry-build.sh`) |
| `--with-package` | the `.deb` and `.rpm` lanes in their containers (`scripts/package-in-container.sh`; needs Docker) |
| `--allow-missing` | a missing tool becomes a notice instead of a failure |

The script's header lists what stays CI-only and why.

### Running the package lanes locally

The `.deb`, `.rpm`, Arch and repository lanes run in containers in CI, and
`scripts/package-in-container.sh` runs each one locally in the same image
(the tags in `scripts/tools.sh`), through the scripts CI calls
(`package-deps.sh`, `build-deb.sh` / `build-rpm.sh`, `test-package.sh`,
`check-arch-package.sh`, `check-package-repos.sh`):

```bash
scripts/package-in-container.sh deb                  # Debian 13.7: build, lintian, install, launch
scripts/package-in-container.sh rpm                  # Fedora 44: build, install, launch
scripts/package-in-container.sh repos <dir>          # APT and DNF repositories from the .deb
                                                     # and .rpm in <dir>, built and read back
scripts/package-in-container.sh arch dist/Dish-X.Y.Z-x86_64.AppImage
                                                     # dish-bin, its pacman repository, pacman -S
```

The tree goes in as a copy, so nothing root-owned lands in the checkout; the
artifacts come back under `dist/container/<lane>/`. The install-and-launch
check (`scripts/launch-check.sh`) is the one gate that can catch a missing
runtime dependency: every other check passes against a build tree, which is
exactly where a QML module resolves from the Qt install rather than from a
`Depends:` line. It also fails unless the app logs
`controller database: bundled dbVersion N with M records` at start.

## Where code goes

The app is a unidirectional-dataflow core with a Qt Quick projection on top.
Before writing a class, pick the primitive that matches what it actually does:
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
[`docs/QML_UI_KIT.md`](docs/QML_UI_KIT.md). In particular, a page may not
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

Six catalogues in `translations/`: English, Bosnian, German, Spanish, French
and Brazilian Portuguese. They compile into the binary, and at startup the app
walks `QLocale::uiLanguages()`, so the desktop's preferred interface language
wins over the regional format setting. A new user-facing string needs a catalogue
entry in the same commit: `scripts/check-translations.sh` re-runs `lupdate` in
CI and fails on any diff. Run it locally and commit the result.

It needs Qt 6.9 or newer; CI builds with Qt 6.12.1 (`scripts/tools.sh`).
Before 6.9, `lupdate` drops the namespace from a class whose definition and
member bodies sit in different files: `dish::net::WifiConnectionManager` comes
back out as `WifiConnectionManager`, which is not the context `moc` hands
`tr()` at run time, so the entry it writes could never be looked up. The script
refuses to run on an older one rather than let that land.

Coverage is **enforced**. `check-translations.sh` fails when any catalogue has
an unfinished entry, because a missing translation is not a blank, it is
English shown to someone who does not read English, and nothing else in the
suite can see that. Extracting a string and translating it are one commit, not
two.

English is a real catalogue rather than the untranslated fallback, because a
`%n` message carries one source string but needs one plural form per category
and Bosnian has three. Its non-plural entries are filled from their own source
text by `scripts/seed-source-language.py`, which the gate runs for you, so the
count means the same thing in all six catalogues, and the only English entries
a person writes are the plural forms that genuinely need a decision.

## License headers

Every source file (`*.h`, `*.hpp`, `*.cpp`) starts with:

```cpp
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
```

New files must include both lines. Don't introduce code under a different
license: the project is LGPL-3.0-or-later end-to-end (`LICENSE`,
`COPYING.GPL3`, source headers).

## Style

- C++20, four-space indent, 100-column soft limit. `.clang-format` is
  authoritative; run `clang-format -i` if you're unsure.
- Warnings are enforced as errors on first-party targets (`dish_strict`).
  See `CMakeLists.txt` for the exact set; in short:
  `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Wconversion -Wsign-conversion -Wdouble-promotion -Wformat=2`.
- Match the surrounding style. Headers go in the order: project, Qt, libs,
  std, separated by blank lines (see `src/AppModel.h` for the pattern).

### Zero warnings

Every compiler warning on a first-party target fails the build, under gcc and
clang, Debug and Release, and clang-tidy, qmllint and the QML literal scanner
gate at zero too. A warning or a finding is fixed at its cause, never
silenced: no `NOLINT`, no `#pragma` diagnostic push, no `-Wno-*`, no qmllint
disable comment, no wider exclude, and no lowered severity. A deprecation is a
warning like any other: move to the replacement API. A new Qt, compiler or
formatter that raises a new finding raises a real one.

Two exclusions exist, both around code that is not ours, and both are drawn
by target rather than by markers in the source: the vendored ENet library
builds without `dish_strict`, and clang-tidy does not read `src/UI/` and
`src/qml/`, whose translation units compile moc and qmltyperegistrar output
(see [clang-tidy](#clang-tidy)). ThreadSanitizer reads
`tests/tsan.suppressions`, which names only uninstrumented libraries (Qt,
glib, libstdc++'s out-of-line waits) as `called_from_lib`, never a race in
this code.

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

## Tests

| Where | Covers | Runs in |
|---|---|---|
| `tests/` (`DishTests`, Catch2) | the core, the composers, the sources against real loopback listeners, the QML-facing models | every lane: Debug and Release, gcc and clang, both sanitizers, coverage |
| `tests/test_lock_laws.cpp` (`DishLockLawTests`) | that no hot-path thread takes a tracked lock | the `DISH_LOCK_STATS` lanes, plain and under both sanitizers |
| `tests/test_measurements_report.py` | the release measurements report | the gcc `ci` leg |
| `tools/bench` | the input path's latency and its locks | `ctest`'s `--smoke` in the Release leg; the full run in `measurements.yml` |
| `scripts/check-version-consistency.sh` | every fact the documents repeat from code: the version, the Qt and SDL2 floors, the pins, the KDE runtime branch, the containers, the protocol number | `version-consistency.yml`, first in `scripts/ci-local.sh` |
| `scripts/launch-check.sh` | the shipped package or bundle starts, loads its QML, its SVGs and its controller database, and exits 0 on SIGTERM | the `.deb`, `.rpm` and AppImage lanes |

[`tests/PARITY.md`](tests/PARITY.md) is the cross-client parity ledger: a
behaviour that differs from a sibling, or is deferred, is a row there.

What CI cannot reach (a real window manager, a real pad, a real satellite) is
covered by
[`docs/QML_MANUAL_SMOKE_CHECKLIST.md`](docs/QML_MANUAL_SMOKE_CHECKLIST.md),
which is run by hand before a release.

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

- All changes land on `main` via pull request: no direct pushes. Branch
  protection requires the CI checks named in the PR template.
- Commits and PR descriptions name no tool. No `Co-Authored-By` trailer for an
  assistant, no "Generated with" footer: the author of record is the person
  who opened the PR, whatever helped write it.
- Use the PR template (`.github/pull_request_template.md`) to describe the
  change, the tests that failed before it, the manual test matrix you ran,
  and call out anything that touches the wire protocol. Anything user-facing
  adds a line under `## [Unreleased]` in `CHANGELOG.md`, and a change of a
  row in `tests/PARITY.md` lands in the same change.
- Keep commits focused; squash noisy fixup commits before review. `main`
  takes squash merges only.

## What CI runs

`linux-ci.yml`, ten jobs:

- `ci` ("Build, test, lint, format"), once per compiler (gcc and clang):
  `scripts/check-format.sh`, the Debug build and `ctest`,
  `scripts/check-qml.sh`, `scripts/qml-lint-literals.sh`,
  `scripts/check-translations.sh`, `scripts/check-tidy.sh`, then a Release
  build with the bench that also runs `ctest`, the measurements report tests,
  and `scripts/check-hardening.sh` (RELRO, BIND_NOW, a non-executable stack,
  PIE). The lint steps run on the gcc leg only; the clang leg exists because
  `-Wconversion`, `-Wshadow` and `-Wold-style-cast` diverge materially between
  the two compilers.
- `sanitize`: the suite under ASan+UBSan and under TSan
  (`scripts/sanitize.sh`). TSan is not optional here: long-lived threads share
  atomics and mutexes, and nothing else in the pipeline would see a race.
- `lock-stats`: the `DISH_LOCK_STATS` build, the lock laws and the whole
  suite in it (`scripts/lock-stats.sh`).
- `lock-laws-sanitize`: the lock laws under each sanitizer.
- `package` ("Package (.deb) and launch it"): in a `debian:13.7` container,
  `scripts/package-deps.sh deb`, `scripts/build-deb.sh`, then
  `scripts/test-package.sh deb`: `desktop-file-validate`,
  `appstreamcli validate`, `lintian --fail-on error`, the payload, and the
  install and launch.
- `rpm` ("Package (.rpm) and launch it"): the same in a `fedora:44`
  container: the payload, the udev reload scriptlet, the install and launch
  (with `QT_FORCE_STDERR_LOGGING=1`, because Fedora's Qt logs to journald when
  stderr is not a terminal).
- `release-dry-run`: the shared `_release_harden.yml` in dry-run mode over
  those two packages: Grype, the SBOMs, `SHA256SUMS`, and every file signed
  into a Sigstore bundle with a throwaway key and verified. Nothing leaves the
  runner.
- `repos`: the APT and DNF repositories built from those two packages by the
  scripts `apt-publish` and `rpm-publish` run, signed with a throwaway key
  and read back as apt and dnf would (`scripts/check-package-repos.sh`).
  Nothing is published.
- `coverage`: lcov over the Debug suite (`scripts/coverage.sh`), summarised
  into the job page.
- `sentry-sdk`: the Release build with the Sentry SDK and a DSN that resolves
  nowhere (`scripts/check-sentry-build.sh`), the one lane that compiles the
  crash-reporting path before a tag does.

`appimage-ci.yml`, on every pull request: the AppImage on `ubuntu-22.04`
(the glibc 2.35 floor) through `.github/actions/setup-appimage-host` and
`scripts/build-appimage.sh`, whose smoke test runs `scripts/launch-check.sh`
on the unpacked bundle; then `arch`, the `dish-bin` package built from that
AppImage in an `archlinux:base-devel` container, signed into a pacman
repository with a throwaway key and installed from it
(`scripts/check-arch-package.sh`).

`flatpak-ci.yml`, on every pull request: `scripts/build-flatpak.sh` on the KDE
runtime `scripts/tools.sh` names, then the checks that the DSN, the SDK and
the debug files the symbol upload reads are there.

`version-consistency.yml`: `scripts/check-version-consistency.sh`.
`CMakeLists.txt`'s `project(Dish VERSION ...)` is the version's source; the
mirrors are `packaging/com.tinkernorth.Dish.metainfo.xml`'s `<release>` and
the `CHANGELOG.md` heading. It also holds the Qt floor in `docs/PACKAGING.md`,
`README.md`, `THIRD_PARTY.md` and `assets/licenses/licenses.json`, the SDL2
floor, the pins in `scripts/tools.sh` against their copies, the KDE runtime
branch, the container images and the protocol number.

`measurements.yml`: the input-path bench on pull requests that touch the hot
path, and on every release; see [`tools/bench/README.md`](tools/bench/README.md).

Security gates (also blocking):

- `security.yml`, through the shared `_security.yml`:
  `scripts/check-action-pins.sh`, the allowlist gate
  (`scripts/security-allowlist.py`), OSV-Scanner through `scripts/osv-scan.sh`
  over the tree and the components `scripts/osv-lockfile.sh` names by
  upstream commit, and gitleaks over the full history. Dependency review is
  off (`dependency_review_enabled: false`) until the repository's Dependency
  graph setting is on.
- `codeql.yml`: CodeQL `cpp` analysis (security-extended and
  security-and-quality query packs), blocking, with the results uploaded to
  code scanning.

## Security

### Adding a vulnerability allowlist entry

Open a PR that adds an entry to [`.security/allowlist.yaml`](.security/allowlist.yaml)
(see the schema in the file). Required fields: `cve`, `reason`, `owner`,
`expires`. CI rejects the PR if any field is missing or `expires` is in
the past. Renew or remove on or before `expires`; there is no silent
suppression.

### Running security checks locally

`scripts/ci-local.sh` runs them after the version check, through the scripts
`_security.yml` runs, with OSV-Scanner and gitleaks at the versions
`scripts/tools.sh` pins (fetched into `build/tools/`, Linux x86_64 and macOS
arm64). By hand, from the repository root:

```bash
sh scripts/check-action-pins.sh                   # every uses: pinned to a SHA with # vX.Y.Z
python3 scripts/security-allowlist.py check        # Python 3.11+ with PyYAML
OSV_SCANNER=build/tools/osv-scanner \
  bash scripts/osv-scan.sh --lockfile-script scripts/osv-lockfile.sh -- .
build/tools/gitleaks git --config=.gitleaks.toml --redact --exit-code=1 .
```

### Verifying a release artifact

Each GitHub Release ships the `.deb`, the `.rpm`, the AppImage (with its
`.zsync` and `.sha256`) and the Flatpak bundle, each under its versioned name
and its stable name, `latest.json`, `SHA256SUMS`, the SPDX and CycloneDX
SBOMs, one cosign keyless Sigstore bundle `<file>.sigstore.json` per file,
`dish-linux.intoto.jsonl` (SLSA L3 provenance) and the release measurements.
The signing runs in `_release_harden.yml`, so that workflow at the tag is the
certificate identity:

```bash
TAG=X.Y.Z
cosign verify-blob \
  --bundle SHA256SUMS.sigstore.json \
  --certificate-identity "https://github.com/TinkerNorth/dish-linux/.github/workflows/_release_harden.yml@refs/tags/$TAG" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS
```

Releases up to 2.8.1 carry `.sig` and `.crt` instead of bundles;
[`SECURITY.md`](SECURITY.md) has the full recipe, the older form, and the
provenance check (both run against 2.8.1).

## Cutting a release

`release.yml` runs on a bare `MAJOR.MINOR.PATCH` tag (no `v` prefix, e.g.
`1.2.3`; `1.2.3-rc.1` publishes as a prerelease) or a `workflow_dispatch` with
a tag input. In order:

1. `gates` re-runs the security gates against the tagged commit, and
   `required-secrets` checks the cosign keyless flow can authenticate and
   reports whether the GPG and Sentry secrets are set.
2. `deb`, `rpm`, `appimage` and `flatpak` build each package through the
   scripts the pull-request lanes run, with the `SENTRY_DSN` secret, upload
   each build's debug symbols to Sentry (best effort), and install and launch
   the `.deb` and `.rpm` (the AppImage launches inside its build script).
   `manifest` writes `latest.json` and fails if the tag is not
   `project(Dish VERSION)`.
3. `harden` (the shared `_release_harden.yml`) scans, writes the SBOMs and
   `SHA256SUMS`, and signs everything; `provenance` adds the SLSA file.
4. `draft` uploads it all as a draft release, `measurements` attaches the
   bench's numbers, and `publish` flips the draft to published, so
   `releases/latest` never points at a half-uploaded release.
5. `sentry-release` creates the Sentry release. On a stable tag, `apt-publish`,
   `rpm-publish` and `arch-publish` then regenerate the signed APT, DNF and
   pacman repositories on `gh-pages`. They skip, with a warning, when
   `GPG_PRIVATE_KEY` or `GPG_KEY_ID` is not set.

Repository secrets and variables: `SENTRY_DSN`, `SENTRY_AUTH_TOKEN` (without
them a release ships with no crash reporting and no symbols), the variables
`SENTRY_ORG` and `SENTRY_PROJECT` (optional; the DSN's numeric ids stand in),
and `GPG_PRIVATE_KEY` and `GPG_KEY_ID` for the package repositories
([`packaging/repo/README.md`](packaging/repo/README.md) has the key and the
Pages setup).

The procedure:

1. **Open the release PR** ("Release X.Y.Z"): `project(Dish VERSION ...)` in
   `CMakeLists.txt`, `## [Unreleased]` turned into the `## [X.Y.Z] - YYYY-MM-DD`
   heading, and a `<release version="X.Y.Z">` with a description paragraph in
   `packaging/com.tinkernorth.Dish.metainfo.xml` (`appstreamcli validate
   --no-net` must pass). `version-consistency.yml` fails a PR that moves one
   without the others. Update `PRIVACY.md` first if anything about data
   collection changed.
2. **Mind the fleet order.** The website (`https://dish.tinkernorth.com`) is
   tagged first. The clients are tagged once
   `https://dish.tinkernorth.com/controllers/v1/manifest.json` serves the
   database the release bundles, so a client's first controller-database
   check finds what it expects.
3. **Merge by squash, then tag the squash commit** with a lightweight tag:
   `git tag X.Y.Z <sha> && git push origin X.Y.Z`. Dish for Windows and Dish
   for Linux share one version number when they release together.
4. **Check the published release.** Download it, run the verification recipe
   above, and install the `.deb` or `.rpm` and launch it.

A failed release run publishes nothing. To re-run a release, dispatch it on
the tag ref, `gh workflow run release.yml --ref X.Y.Z -f tag=X.Y.Z`: the
signing certificate names the ref the run is on, so a dispatch from `main`
would sign as `@refs/heads/main`, and `_release_harden.yml` refuses to sign
off a tag. A dispatch on the tag runs the tag's own `release.yml`, and the
repository jobs only run for a tag push, so a release whose workflow needed a
fix ships as the next patch version.
Every tag-time job has a pull-request twin (`package`, `rpm`, `repos`,
`appimage`, `arch`, `flatpak`, `sentry-sdk`), so the tag should not be the
first time any of them runs. `linux-ci.yml`'s `release-dry-run` runs the
shared hardening over the pull request's `.deb` and `.rpm` with a throwaway
key. What a pull request cannot exercise: the keyless signing through Fulcio
and Rekor, the code-scanning upload and the SLSA provenance (they need the
tag's OIDC identity), the Sentry uploads (they need the secrets), and the
`gh-pages` push.

## Touching the hot path

Three threads carry every report and every feedback message: the SDL input
thread, a Direct pad's raw-HID read thread, and each session's receive thread.
None of them takes a lock, allocates per report, or waits on the network, the
GUI or a pad, and a mic capture thread sends its frames without a lock too. If
you are modifying `SDLGamepadBridge`, `GamepadInputProcessor`,
`SlotRouter`, `UsbGamepadManager`'s publisher or its output worker, or
`SatelliteClient`:

- State another thread changes reaches these threads as an immutable snapshot
  (`SlotRouter`'s table, read inside a grace epoch; the bridge's inputs through
  `LatestValue`) or through a seqlock or a single-consumer queue
  (`core/thread/`). Nothing on the path takes a `TrackedMutex`.
- One shape of waiting is allowed, and only between producers: a turn flag
  that several producers of one pad's feedback take for a fixed handful of
  stores, with no call, allocation or syscall inside, spinning with a yield
  only when two producers meet (`OutputMailbox::post`,
  `SurfaceLedger::rememberWith`). Its consumer or reader never takes it, so
  the thread that drives the pad never waits on a producer, and no producer
  waits on a reader. It is not a `TrackedMutex`, so the lock laws do not
  count it; a new one names itself here and gets a test that its reader
  takes no turn.
- Buffers live on the stack or in fixed records; nothing on the path allocates.
- No `QObject::connect` cross-thread signals on the send path.
- Both rules are enforced, not hoped for: `tests/test_lock_laws.cpp`
  (`DishLockLawTests`, built with `-DDISH_LOCK_STATS=ON`) fails when one of these
  threads takes any tracked lock, and `tools/bench` measures the path and its
  locks on every run. The measurements workflow runs both on a pull request
  that touches the path and on every release, and fails when a lock-statistics
  run shows one of these roles taking a lock. Run both after a change here; see
  [`tools/bench/README.md`](tools/bench/README.md).
- No standalone `std::atomic_thread_fence`: GCC refuses it under
  ThreadSanitizer, the arbiter of every lock-free structure here. Order with
  acquire and release on the atomics themselves, or seq_cst operations.
- A thread that waits on a pad, the network or a joining thread does it off
  these three: a Direct pad's OUT reports go through its output worker, and a
  session client the GUI lets go of closes on the `ClientReaper`.
- Preserve `IP_TOS = 0xB8` (DSCP EF) and `MSG_NOSIGNAL` on every send.

## Touching the wire protocol

Dish for Linux offers protocol version 4 and still settles down to 1 against
an older satellite. Dish for Linux, Dish for Windows, Dish for macOS, Dish for
iOS and Dish for Android all talk to the same Satellite server and must
produce byte-identical traffic. The contract
is specified in ONE place,
[`satellite/docs/contract.md`](https://github.com/TinkerNorth/satellite/blob/main/docs/contract.md);
this repo's [`docs/contract.md`](docs/contract.md) maps each concept to the
file implementing it, and [`src/core/model/Protocol.h`](src/core/model/Protocol.h)
is the client-side mirror of the constants.

- AEAD: ChaCha20-Poly1305 IETF, 12-byte big-endian nonce derived from a
  monotonic counter.
- Packet layout: `token(4) | counter(4) | ciphertext+tag`, with the 4-byte
  token as AAD.
- XUSB report: 12 bytes, little-endian.
- INPUT (0x0001): ctrlIdx and the XUSB report, and on a session settled at
  protocol 4 the extended-buttons word (u32 LE) behind it. The frame shape
  keys on the SETTLED version; the receiver reads it by length.
- Ports: discovery UDP 9879, pairing and REST HTTPS 9443, streaming UDP 9876.

Any change here is `[wire-coordinated]` in `CHANGELOG.md` and must be
coordinated with `satellite`, `dish-android`, `dish-ios`, `dish-mac` and
`dish-windows` in the same release cycle.

## clang-tidy

**The non-UI scope is clean, and CI gates it at zero.** `scripts/check-tidy.sh` runs
clang-tidy with `--warnings-as-errors='*'` over every `src/**.cpp` and
`src/**.h` outside `src/UI/` and `src/qml/`, so a new finding fails the build.

`.clang-tidy` itself keeps `WarningsAsErrors: ''` because it is the
fleet-canonical config shared with `dish-android`, `dish-mac` and
`dish-windows`, whose scopes are not clean. The gate lives in the workflow, not
in the config, so this repo can hold a higher bar without forking the file.

`src/UI/` and `src/qml/` are excluded: Qt's MOC- and qmltyperegistrar-generated
code triggers a long tail of false positives that no source change can fix.

Reproduce locally after a debug build:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
scripts/check-tidy.sh
```

It reads the same `build/` the other gates use: CMakeLists exports the compile
database globally, so a second tree would only re-derive the same `src/`
entries and cost another full build.

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

- `-portability-avoid-pragma-once`: the project uses `#pragma once` everywhere
  by convention.

## Reporting bugs

Use the issue templates under `.github/ISSUE_TEMPLATE/`. Include the distro,
the package format (`.deb`, `.rpm`, AppImage, Flatpak, `dish-bin`), the app
version, the controller, and the file the Diagnostics page's "Save
diagnostics" button writes. For a crash, a `journalctl --user -e` excerpt, and the
backtrace in `$XDG_STATE_HOME/dish/crash.log` if there is one.
