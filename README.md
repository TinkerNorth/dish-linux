# Dish Linux

Native Linux desktop client for the Satellite wireless-gamepad server. Mirrors
the functionality of the Dish Android and Dish Mac clients: LAN discovery, PIN
pairing, encrypted UDP input streaming (ChaCha20-Poly1305), heartbeats, and
multiple parallel server sessions.

## Architecture

```
Qt6 Widgets (MainWindow, ConnectionsDialog, PairingDialog, SlotCard)
  └── AppModel (QObject, UI thread)
        ├── ConnectionHub      ── aggregates live + remembered sessions
        ├── WifiConnectionManager
        │     └── WifiConnection (per-server)
        │           └── SatelliteClient  ── encrypted UDP + heartbeat + ACK loop
        ├── LANDiscovery       ── UDP broadcast listener on :9879
        ├── PairingClient      ── TCP pair handshake on :9878
        ├── HTTPClient         ── POST/DELETE /api/connections on :9877
        └── SDLGamepadBridge   ── SDL_GameController event pump (own thread)
              └── GamepadInputProcessor → SatelliteClient.sendReport()
```

### Hot path (input → wire)

The hot path is the only thing that runs at gamepad polling rate; every
other subsystem is bookkeeping and stays off it.

```
  ┌────────────────┐      ┌───────────────────────┐      ┌──────────────────────┐
  │ SDL gamepad    │ ───► │ GamepadInputProcessor │ ───► │ SatelliteClient      │
  │ thread         │      │  • XUSB packing       │      │  • ChaCha20-Poly1305 │
  │ (own pthread)  │      │  • axis/trigger scale │      │  • IP_TOS = DSCP EF  │
  └────────────────┘      │  • atomic counter     │      │  • raw sendto()      │
                          └───────────────────────┘      └──────────┬───────────┘
                                                                    │
                                                                    ▼
                                                              UDP :9876 → server
```

No queue, no Qt event hop, no cross-thread signal. The UI thread never
appears on this path; it only updates 1 Hz telemetry from counters the
SDL thread publishes lock-free.

## Low-latency strategies (mirrored from Android / Mac)

- **Direct `sendto()` from the SDL gamepad thread.** Each `SDL_CONTROLLER*`
  event fires the native socket send inline — no queue, no Qt event hop, no
  cross-thread signal. Same pattern as `Kotlin → JNI → sendto` on Android and
  `GameController valueChangedHandler → sendto` on macOS.
- **Raw POSIX UDP socket** (not `QUdpSocket`) so we can set `IP_TOS = 0xB8`
  (DSCP EF class, expedited forwarding) and bypass any framework-level
  queueing.
- **libsodium `crypto_aead_chacha20poly1305_ietf`** produces the exact same
  wire format as the Android JNI / CryptoKit.ChaChaPoly used by the other
  clients and the Satellite server.
- **Per-session heartbeat + ACK threads** so the hot input path is never
  contended by book-keeping traffic.
- **Lock-free `AtomicCounter` for the nonce** and a single short-held mutex on
  the routing-table lookup keep the hot path branch-free and allocation-free.
- **`MSG_NOSIGNAL`** on every send so a server disconnect can't kill the
  process.

## Requirements

- A reasonably current Linux distro (Ubuntu 22.04+, Fedora 38+, Arch, …)
- Qt 6.2+
- libsodium 1.0.18+
- SDL2 2.0.18+
- CMake 3.21+, a C++17 compiler (gcc 11+ or clang 14+), pkg-config, Ninja
- A compatible gamepad (Xbox, PlayStation, 8BitDo, …)
- A Satellite server reachable on your LAN

### Install build dependencies

**Debian / Ubuntu (22.04+)**
```bash
sudo apt install -y \
    build-essential cmake ninja-build pkg-config \
    qt6-base-dev libsodium-dev libsdl2-dev \
    clang-format clang-tidy
```

**Fedora (38+)**
```bash
sudo dnf install -y \
    gcc-c++ cmake ninja-build pkgconf-pkg-config \
    qt6-qtbase-devel libsodium-devel SDL2-devel \
    clang-tools-extra
```

**Arch / Manjaro**
```bash
sudo pacman -S --needed \
    base-devel cmake ninja pkgconf \
    qt6-base libsodium sdl2 clang
```

## Build & Run

```bash
cd dish-linux
scripts/build.sh release
./build/dish
```

For a debug build with tests:
```bash
scripts/build.sh debug test
```

Or the long form:
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/dish
```

## Install (per-user)

To run Dish from your desktop launcher without packaging it system-wide:

```bash
# 1) Build the release binary
scripts/build.sh release

# 2) Drop the binary somewhere on $PATH
install -Dm755 build/dish ~/.local/bin/dish

# 3) Register the .desktop entry
install -Dm644 packaging/dish.desktop ~/.local/share/applications/dish.desktop
update-desktop-database ~/.local/share/applications 2>/dev/null || true
```

The launcher will pick up `dish` from `~/.local/bin`; make sure that
directory is on your `PATH` (most distros ship it on `PATH` by default for
interactive shells via `~/.profile`). For a system-wide install, run
`sudo cmake --install build` and copy the desktop file to
`/usr/share/applications/`.

## Project Layout

```
dish-linux/
├── CMakeLists.txt
├── scripts/build.sh
└── src/
    ├── main.cpp                # QApplication entry + sodium_init
    ├── AppModel.{h,cpp}        # top-level QObject
    ├── Models/                 # DiscoveredServer, PairResponse, …
    ├── Network/                # sockets, crypto, discovery, pairing, HTTP
    ├── Input/                  # SDL bridge + XUSB mapping
    ├── Util/                   # AtomicCounter, hex, big-endian helpers
    └── UI/                     # Qt widgets + theme
```

## Protocol parity

All message types, byte layouts, port numbers and JSON shapes match the other
Dish clients verbatim so all three can talk to the same server and appear
identical to it:

| Field            | Value            |
| ---------------- | ---------------- |
| Discovery port   | UDP 9879 (listen)|
| Pairing port     | TCP 9878         |
| HTTP API port    | TCP 9877         |
| Streaming port   | UDP 9876         |
| AEAD             | ChaCha20-Poly1305 IETF |
| Nonce            | counter, BE, left-padded to 12 bytes |
| Packet layout    | `token(4) \| counter(4) \| ciphertext+tag` |
| AAD              | token (4 bytes)  |
| XUSB report      | 12 bytes, little-endian |
| Heartbeat period | 2 s              |
| Miss threshold   | 5 consecutive    |

## Testing

```bash
scripts/build.sh debug test
# or
ctest --test-dir build-debug --output-on-failure
```

Unit tests cover the hex/byte-packing utilities, the big-endian helpers, the
XUSB input mapping (axis and trigger scaling, button bitfield, zero-on-disconnect
fan-out), the lock-free atomic counter under contention, the lenient beacon
JSON decoder, and the model codable round-trips. They run in well under a
second and do not open sockets.

## Development

Install the build dependencies for your distro (see *Install build
dependencies* above), then enable the pre-commit hook:

```bash
scripts/build.sh debug          # generates build-debug/compile_commands.json
scripts/setup-hooks.sh          # points core.hooksPath at .githooks/
```

Format / lint manually:
```bash
clang-format -i $(git ls-files 'src/*.cpp' 'src/*.h' 'tests/*.cpp')
clang-tidy -p build-debug $(git ls-files 'src/*.cpp')
```

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the full workflow, header
policy, and review expectations.

## License

Distributed under the terms of the **GNU Lesser General Public License v3.0
or later**. See [`LICENSE`](LICENSE) (LGPL) and [`COPYING.GPL3`](COPYING.GPL3)
(the GPL v3 the LGPL incorporates by reference).

## Contributing

Changes should land on `main` through a pull request. The `Linux CI`
workflow (`.github/workflows/linux-ci.yml`) runs the `clang-format` check, the
debug build + ctest, `clang-tidy`, and a release build on every PR and on
`main` pushes. See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the LGPL
header policy, branching, and hook setup.
