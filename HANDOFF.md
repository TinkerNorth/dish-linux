# dish-linux — Continuation Prompt

Use this file as the handoff prompt when resuming work on the `dish-linux`
client. It captures the current state of the tree, how to verify it, and the
outstanding follow-ups.

---

## Context

`dish-linux` is the Linux-native port of the Dish wireless gamepad client. It
mirrors `dish-android` and `dish-mac` in protocol, theme, and latency strategy:

- **Language / UI:** C++17 + Qt6 Widgets.
- **Input:** SDL2 on a dedicated polling thread (hot-plug aware).
- **Crypto:** libsodium ChaCha20-Poly1305 over UDP, DSCP EF on the hot path.
- **Server parity:** XUSB report layout matching `satellite`.
- **License:** LGPL-3.0 (`LICENSE`, `COPYING.GPL3`, source headers).
- **Config:** XDG Base Directory spec.

Repository layout lives under `/home/samus/TinkerNorth/dish-linux/` and is
**not** a git repo (per the original instruction — do not init or commit).

---

## What's Done

### Build / tooling
- `CMakeLists.txt` with split warning surface:
  - `dish_warnings` — `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wformat=2`.
  - `dish_strict` — adds `-Werror`, attached only to first-party targets so
    Catch2's headers don't fail the build.
- `scripts/build.sh` — `debug | release | test | clean` wrapper around
  CMake/Ninja.
- `.clang-format` (LLVM-derived, 4-space, 100-col).
- `.clang-tidy` (C++17 modernize / readability / bugprone / performance).
- `.editorconfig`, `.gitignore`.
- `.github/workflows/linux-ci.yml` — installs Qt6/SDL2/libsodium, runs
  `clang-format --dry-run --Werror`, debug build + `ctest`, `clang-tidy`,
  release build, uploads the binary as a CI artifact.
- `.github/dependabot.yml`, `.github/pull_request_template.md`,
  `.github/ISSUE_TEMPLATE/{bug_report,feature_request}.md`.

### Source tree (`src/`)
- `Models/` — `DiscoveredServer`, `PairResponse`, `ConnectResponse`,
  `ConnectionSummary`, `ControllerSlot`, `RememberedWifi`, JSON round-trips.
- `Util/` — `AtomicCounter`, `Hex`, `Endian` (big-endian put/read helpers),
  XDG path helpers.
- `Network/` — `BeaconParser`, `BeaconListener`, `WifiConnectionManager`
  (NetworkManager via D-Bus), `ConnectionStore` (XDG persistence),
  `PairingClient`, `ConnectClient`, `RoutingTable` (lock-free hot path),
  `ReportSender` (libsodium AEAD + DSCP EF).
- `Input/` — `SDLGamepadBridge` (dedicated thread), `GamepadInputProcessor`
  (XUSB packing, telemetry counters, Y-axis inversion).
- `UI/` — `Theme` (verbatim Amber/Dark palette + global QSS),
  `MainWindow` (status header, scrollable `SlotCard` list, 1 Hz telemetry
  footer, Manage button), `ConnectionsDialog`, `PairingDialog`, `SlotCard`.
- `AppModel.{h,cpp}` — central coordinator wiring Store → Manager → Hub →
  Input. Public accessor renamed `slots()` → `slotList()` to avoid the Qt
  `slots` macro collision.
- `main.cpp` — `sodium_init`, `QApplication`, `applyDishTheme`, `AppModel`,
  `MainWindow`, `model.start()`.

### Tests (`tests/`, Catch2 v3, 29 cases / all green)
- `test_atomic_counter.cpp` — init / next / reset + 8-thread × 25k contention.
- `test_hex.cpp` — round-trip, mixed case, rejects odd-length / non-hex.
- `test_endian.cpp` — `putU{16,32,64}Be` / `readU*Be` round-trips.
- `test_models.cpp` — `DiscoveredServer.id`, defaults, JSON round-trip;
  `PairResponse`, `ConnectResponse`, `RememberedWifi` list round-trip.
- `test_beacon_parser.cpp` — valid beacon, wrong service, bad JSON, empty
  name, observed-IP override.
- `test_gamepad_input_processor.cpp` — `scaleAxis`/`scaleTrigger` clamp,
  `publish` fan-out, `zeroAndSendAll`, `drainTelemetry` reset semantics.

### Verified
- `scripts/build.sh debug test` → `100% tests passed, 0 tests failed out of 29`
  on Ubuntu with GCC 15.2, Qt 6, SDL2 2.32.10, libsodium 1.0.18.
- Release build links cleanly against the same dependencies.

---

## What's Left

### Tooling (small)
- [x] `scripts/setup-hooks.sh` + `.githooks/pre-commit` — runs
      `clang-format --dry-run --Werror` (strict) and `clang-tidy -p
      build-debug` (advisory, matches CI semantics) on staged C++.

### Packaging / distribution
- [x] `packaging/dish.desktop` (XDG entry, categories, `StartupWMClass`).
      `main.cpp` calls `setDesktopFileName("dish")` so Wayland resolves
      window grouping to the entry.
- [ ] Application icon at multiple sizes under `packaging/icons/hicolor/`.
      (Skipped — needs actual artwork.)
- [x] README section: per-distro install (`apt`, `dnf`, `pacman`) of
      build deps + how to drop the `.desktop` file under
      `~/.local/share/applications/`.
- [ ] (Optional) Flatpak / AppImage manifest.

### Verification
- [x] Binary runs from a non-snap shell (the symbol-lookup error we hit
      was the snap-confined VS Code terminal leaking
      `/snap/core20/.../libpthread.so.0`, not the binary).
- [ ] Live integration test against a running `satellite`:
  - mDNS discovery → pair → connect → bind a real SDL pad → confirm XUSB
    reports land on the server with correct axis polarity and trigger range.
  - Confirm DSCP EF survives the local network stack
    (`tcpdump -v 'ip and (ip[1] & 0xfc) >> 2 == 46'`).
- [ ] Soak test: 8-hour run, watch `drainTelemetry` for drops; verify the
      SDL thread never blocks on the UI thread under load.
- [x] `clang-tidy -p build-debug` triaged. Real fixes landed:
      `bugprone-reserved-identifier` on `_SDL_GameController` (now
      NOLINT-justified), `readability-inconsistent-declaration-parameter-name`
      on `rebuildState`. `portability-avoid-pragma-once` silenced in
      `.clang-tidy` (project convention). Remaining ~100 warnings are
      stylistic and catalogued in `CONTRIBUTING.md`.

### Documentation
- [ ] README screenshots (main window, connections dialog, pairing dialog).
- [x] `CONTRIBUTING.md` — LGPL header policy, hook setup, hot-path /
      protocol-parity rules, clang-tidy triage table.
- [x] Architecture diagram in README — high-level tree + an explicit
      hot-path callout (SDL thread → `GamepadInputProcessor` →
      `SatelliteClient` → UDP).

---

## How to Resume

```bash
cd /home/samus/TinkerNorth/dish-linux
# Build deps (Ubuntu 24.04+):
#   sudo apt install qt6-base-dev libsodium-dev libsdl2-dev \
#                    cmake ninja-build pkg-config clang-format clang-tidy
scripts/build.sh debug test     # full build + ctest
scripts/build.sh release        # optimized binary at build-release/dish
```

Pinned constraints (do not relax without asking):
- LGPL-3.0 only.
- Wire-protocol parity with `satellite` (XUSB layout, ChaCha20-Poly1305).
- Input thread must never block on the UI thread — keep the hot path
  routed through `RoutingTable` directly.
- Theme must remain visually identical to `dish-android` / `dish-mac`.
- Do not initialize a git repo or commit — local folder only.
