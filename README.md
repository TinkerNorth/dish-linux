# Dish Linux

[![Linux CI](https://github.com/TinkerNorth/dish-linux/actions/workflows/linux-ci.yml/badge.svg)](https://github.com/TinkerNorth/dish-linux/actions/workflows/linux-ci.yml)

Turns the gamepads attached to a Linux machine into wireless controllers for
another machine on the same network. Dish finds
[Satellite](https://github.com/TinkerNorth/satellite) servers on the LAN, pairs
with a PIN over HTTPS, and streams encrypted controller input over UDP; the
satellite plugs a matching virtual pad into the host, so games there see a real
controller.

**Dish needs a Satellite server running on your LAN.** It is one half of a pair
and does nothing on its own. It is the Linux sibling of `dish-android`,
`dish-mac` and `dish-windows`; all four speak the same protocol to the same
server and look identical to it.

Physical controllers only. There is no on-screen touch gamepad; that belongs to
`dish-android`, where the form factor makes sense.

## What it does

- LAN discovery over mDNS (`_satellite._udp.local.`) with a UDP broadcast
  fallback for older satellites
- PIN pairing over HTTPS against the satellite's self-signed certificate,
  pinned trust-on-first-use so a swapped certificate aborts the request
- ChaCha20-Poly1305 input streaming over UDP, sent straight off the input
  thread
- SDL2 for every pad, plus an opt-in USB-direct hidraw path for DualSense,
  DualShock 4, Switch Pro, 8BitDo and Steam Controller class pads
- Motion, battery and touchpad forwarded up; rumble and light bar driven back
  down by the host
- Several satellites side by side, with per-slot controller binding
- Per-device deadzones, button remapping, and a guided setup wizard
- Holds the display awake while a slot is streaming, releases it on the last
  unbind
- Light and dark themes that follow the desktop, six UI languages

## Install and run

You need a 64-bit Linux desktop with Qt 6.7 or newer, a gamepad, and a
reachable Satellite server. See [`docs/PACKAGING.md`](docs/PACKAGING.md) for the
per-distro Qt situation — notably, Ubuntu 24.04 LTS ships Qt 6.4 and needs a
Flatpak build or a backport.

Settings persist under `~/.config/TinkerNorth/Dish.conf`; a crash writes a
backtrace to `$XDG_STATE_HOME/dish/crash.log`.

### USB-direct needs a udev rule

`/dev/hidraw*` is root-only by default, so the opt-in USB-direct path needs one
rule installed before it can claim a pad:

```sh
sudo install -m 644 packaging/udev/70-dish-hidraw.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

`cmake --install` places it for you. Without it every claim fails with a
permission error and Dish keeps the pad on the SDL path, which still works —
it is just rate-capped. Dish never asks for root.

### Updates

Dish does not update itself. Your package manager owns the binary, so the
updater checks and stops: about 15 seconds after launch and every four hours
after that it asks GitHub for `latest.json`, and if there is a newer release it
shows a pill linking to the release page. Nothing is downloaded and nothing is
applied. *Check for updates automatically* in Settings stops every
update-related network request when off. What the check sends is spelled out in
[`PRIVACY.md`](PRIVACY.md).

## Build from source

- GCC 12+ or Clang 15+, CMake 3.21+, Ninja
- Qt 6.7+ (Core, Gui, Network, DBus, Svg, Quick, Qml, QuickControls2; Linguist
  tools for the translation catalogues)
- libsodium, SDL2, Catch2 v3

On Debian and Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-svg-dev \
  qt6-tools-dev qt6-l10n-tools \
  libsodium-dev libsdl2-dev libdbus-1-dev catch2
```

Then:

```sh
scripts/build.sh release
./build-release/dish
```

`scripts/build.sh debug` builds into `build-debug/` instead, and
`scripts/build.sh test` runs ctest after the build. `CONTRIBUTING.md` has the
long-form CMake invocation and the hook, format and lint setup.

## How it works

The app is a unidirectional-dataflow core with a Qt Quick projection on top.
Sources of truth own state, pure composers derive from it, QML binds and
renders, and QML sends commands back. `src/qml/` could be deleted and replaced
with a different front end without touching anything below it.

```
src/qml/          Qt Quick UI: AppViewModel facade, role models, theme bridges
src/composer/     Composers (pure derive), Controllers (effects), Coordinators
src/source/       StateSources and IO gateways: discovery, HTTP, USB, stores
src/repository/   Durable keyed storage over QSettings
src/core/         Pure, Qt-free: reducers and FSMs, wire crypto, input math
src/architecture/ The kernel: Observable, StateSource, Composer, Controller, Repository
src/Input/        SDL bridge, XUSB packing, output command queue
src/Network/      POSIX UDP session, REST client, pairing, connection pool
src/UI/           Theme palettes, font probes, crash handler, license manifest
src/update/       The update check. No download, no staging, no apply.
```

The window manager draws the decorations: Dish paints no title bar of its own,
so the shell's rail and header are the whole chrome.

The input hot path is the deliberate exception and is not routed through the
kernel. An SDL controller event runs `GamepadInputProcessor` and then
`SatelliteClient::sendReport` inline on the SDL thread: pack the XUSB report,
encrypt it, and call `sendto()` on a raw POSIX socket. No queue, no Qt event
hop, no cross-thread signal. The UI thread never appears on the path; it only
reads counters the input thread publishes lock-free. The USB-direct read loop
feeds the same `publish` entry point on its own thread.

Layer rules, the state-capture doctrine (`AsyncState<T>` versus a reducer FSM),
the UI binding contract and the hardening roadmap are in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), the kernel primitives in
[`src/architecture/README.md`](src/architecture/README.md), the QML surface in
[`docs/QML_CONTRACT.md`](docs/QML_CONTRACT.md) and
[`docs/QML_UI_KIT.md`](docs/QML_UI_KIT.md), the design tokens in
[`DESIGN.md`](DESIGN.md), and how a build becomes an installed Dish in
[`docs/PACKAGING.md`](docs/PACKAGING.md).

### Desktop integration

Three portal-backed facts, each with a documented fallback so a minimal desktop
degrades rather than breaks: light/dark from the XDG appearance portal (falling
back to dark), reduced motion from the XDG settings portal then `kdeglobals`
(falling back to motion allowed), and keep-awake from
`org.freedesktop.ScreenSaver.Inhibit` (falling back to a silent no-op).
Bluetooth presence and power come from sysfs and BlueZ directly, because the
wizard needs to tell "no adapter" from "adapter off".

## Protocol

Ports, byte layouts and JSON shapes match the other Dish clients so all four
are interchangeable to a satellite. The authoritative contract lives in
[`satellite/docs/contract.md`](https://github.com/TinkerNorth/satellite/blob/main/docs/contract.md);
the client-side mirror is
[`src/core/model/Protocol.h`](src/core/model/Protocol.h).

| | |
|---|---|
| Protocol version | 1 |
| Discovery | UDP 9879 broadcast beacons, plus mDNS `_satellite._udp.local.` |
| Pairing and REST API | HTTPS 9443, self-signed certificate, TOFU-pinned |
| Streaming | UDP 9876 |
| REST auth | `X-Device-Id` + `X-Hmac-Proof` = hex(HMAC-SHA256(pairingKey, `"satellite-proof:"` + deviceId)) |
| Topology | REST only: `PUT /api/connections` upserts the whole desired controller set |
| Session key | HKDF-SHA256(ikm = pairingKey, salt = sessionSalt, info = `"satellite-session-v1"` \|\| token) |
| AEAD | ChaCha20-Poly1305 IETF |
| Nonce | direction(1) \| 0x00 x7 \| counter(4 BE) |
| AAD | token (4 bytes, BE) |
| Packet | `token(4) \| counter(4 BE) \| ciphertext+tag` |
| Up | INPUT 0x0001, HEARTBEAT 0x0002, MOTION 0x000A, BATTERY 0x000B, TOUCHPAD 0x000C |
| Down | HEARTBEAT_ACK 0x0003, RUMBLE 0x0009, LIGHTBAR 0x000D, SESSION_CLOSE 0x000F |
| Input report | 12 bytes XUSB, little-endian |
| Heartbeat | every 2 s; not responding at 2 misses, dead at 5 |

## Translations

Six catalogues in `translations/`: English, Bosnian, German, Spanish, French
and Brazilian Portuguese. They compile to `.qm` files embedded in the binary at
`:/i18n/`, and the app picks one at startup by walking `QLocale::uiLanguages()`
so the desktop's preferred UI language wins over the regional format setting.

English is a real catalogue rather than the untranslated fallback: a `%n`
message carries one source string but needs one form per plural category, and
Bosnian has three. Vocabulary is sourced from `dish-android`, whose catalogues
are older and reviewed.

`scripts/check-translations.sh` re-runs `lupdate` in CI and fails on any diff,
so a new user-facing string cannot land without its catalogue entry. Coverage
is reported but never enforced; translating a string is a separate act from
extracting it.

## Testing

```sh
scripts/build.sh test
# or, against an existing build tree
ctest --test-dir build-debug --output-on-failure
```

One `DishTests` executable links the `dish_core` library. It covers the pure
core exhaustively, with no mocks and no sockets: the reducer FSMs (USB path
switching, pairing, session lifecycle, capture mode, apply sequencing, the
update check), `AsyncState` transitions, the wire encoders and decoders against
interop vectors shared with the satellite and dish-android, session crypto,
XUSB mapping and deadzones, HID report parsing and transport classification,
the beacon and mDNS parsers, TOFU pinning, and every repository against a
shared contract. The design system is tested too: palette completeness, WCAG
contrast ratios in both themes, font-family probes, and placeholder integrity
plus plural-form order across all six translation catalogues.

What CI cannot reach — a real window manager, a real pad, a real satellite — is
covered by [`docs/QML_MANUAL_SMOKE_CHECKLIST.md`](docs/QML_MANUAL_SMOKE_CHECKLIST.md),
which is run by hand before a release.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the workflow, the LGPL header
policy, hook setup and review expectations. Changes land on `main` through a
pull request; `Linux CI`, `Security` and `CodeQL` run on every one.

## Security

Vulnerability disclosure: [`SECURITY.md`](SECURITY.md). Dish is LAN-only and
talks to no TinkerNorth-operated server.

## License

LGPL-3.0-or-later. See [`LICENSE`](LICENSE) for the LGPL and
[`COPYING.GPL3`](COPYING.GPL3) for the GPL v3 it incorporates by reference.
