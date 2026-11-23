## What changes, and why

<!-- Two or three sentences. Name the behaviour a user or a developer sees, not the files. -->

## Kind of change

- [ ] Fix
- [ ] Feature
- [ ] Refactor (behaviour unchanged)
- [ ] Docs, CI or repo plumbing
- [ ] Packaging (.deb, .rpm, AppImage, Flatpak, dish-bin, the package repositories)
- [ ] Wire protocol or pairing flow (`[wire-coordinated]` in CHANGELOG; the satellite and the sibling clients need the matching change)

## Shape of the code (CONTRIBUTING, "Shape of the code")

- [ ] Immutable by default; one flow per function; named steps instead of comments.
- [ ] Every `switch` over an enum is exhaustive, with no `default`.
- [ ] No singleton, no mutable static, no discarded result, no warning or lint suppression.
- [ ] Every design value reaches QML through `Theme` or `Tokens`, and every word through a catalogue entry.

## Tests

- [ ] Red first: a test failed before the change and passes after it.
- [ ] Every new flow has a test, including the empty, error and loading shapes.
- [ ] A new user-facing string has its catalogue entry in all six catalogues (`scripts/check-translations.sh`).
- [ ] `scripts/ci-local.sh` passes (version mirrors, format, debug build and tests, qmllint, literal scanner, translations, clang-tidy, release build and tests, hardening).
- [ ] A packaging change ran its lane: `scripts/package-in-container.sh deb|rpm|arch|repos`.
- [ ] Manual check on Linux: <!-- distro, package, display server, which controller and satellite -->

## Bookkeeping

- [ ] `CHANGELOG.md` has a line under `[Unreleased]`.
- [ ] `tests/PARITY.md` is updated if a row changed.
- [ ] `docs/ARCHITECTURE.md`, `docs/QML_CONTRACT.md` or `DESIGN.md` is updated if a rule, an `App` member or a token changed.
- [ ] A dependency change updates `scripts/tools.sh` (or `CMakeLists.txt`), `THIRD_PARTY.md` and `assets/licenses/licenses.json` together.
