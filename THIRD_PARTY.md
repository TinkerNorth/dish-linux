# Third-party notices

Dish for Linux is licensed [LGPL-3.0-or-later](LICENSE). This file lists every
third-party component that is linked into `dish`, embedded in it as a resource,
or used only to build and test it. It also states what someone redistributing
the binary has to do.

Nothing is bundled. Qt, SDL2 and libsodium are dynamically linked against the
system copies your distribution provides, so a redistributor of a package built
from this tree is redistributing those libraries under their own distribution's
terms, not through this repository.

The app has an in-app version of this list at Settings, Licenses, rendered from
[`assets/licenses/licenses.json`](assets/licenses/licenses.json). That manifest
and this file describe the same set of components. See
[Keeping this in sync](#keeping-this-in-sync) for the divergences that exist
today.

---

## 1. Summary

| Component | Version | SPDX | How it reaches the user | Attribution obligation |
|---|---|---|---|---|
| [Qt 6](#2-qt-6) | CMake requires >= 6.7 | `LGPL-3.0-only` | Dynamically linked against the system Qt. Nothing is bundled. | Notice, license text, relink freedom. See section 2. |
| [SDL2](#sdl2) | >= 2.0.18 | `Zlib` | Dynamically linked against the system SDL2. | Keep the notice, do not claim authorship |
| [libsodium](#libsodium) | >= 1.0.18 | `ISC` | Dynamically linked against the system libsodium. | Keep the copyright and permission notice |
| [Inter](#4-inter) | 4.001 | `OFL-1.1` | Four `.ttf` faces embedded in `dish` as Qt resources under `:/fonts/`. | Ship the license text with every copy. See section 4. |
| [Catch2](#5-catch2) | 3.x | `BSL-1.0` | Test binary only. Not linked into `dish`. | None for redistributors of the app |
| [ENet (cgutman fork)](#9-enet) | commit `4cde9cc` | `MIT` | Vendored C sources under `third_party/enet/`, compiled into `dish`. | Ship the copyright + permission notice |
| [OpenSSL libcrypto](#10-openssl-libcrypto) | system | `Apache-2.0` | Dynamically linked against the system libcrypto for the Moonlight-host crypto. | Keep the notice; nothing bundled |

Two further items are reuse of published facts rather than of code, and are
covered in [section 6](#6-reused-facts-not-reused-code): SDL's default Switch Pro
motion scaling constants, and the HID input-report byte layouts documented in the
Linux kernel's PlayStation and Nintendo HID drivers.

The Moonlight (GameStream) host protocol support under `src/core/moonlight/` and
`src/source/moonlight/` is an original implementation. Its wire framing, crypto
construction and pairing algorithm were learned from the documentation and the
MIT-licensed host implementation of Wolf (games-on-whales/wolf); that reuse of
adapted logic is recorded in [section 11](#11-wolf-moonlight-protocol-reference).
No GPL-licensed Moonlight code (moonlight-common-c, moonlight-qt, Sunshine,
Apollo) was consulted or copied.

Everything under `resources/brand/` and `packaging/dish.svg` is original
TinkerNorth artwork, covered by this repository's own license. See
[section 8](#8-first-party-artwork).

---

## 2. Qt 6

Upstream: <https://www.qt.io/>. Source: <https://code.qt.io/cgit/qt/qtbase.git/>.

Qt is offered under a commercial license, GPLv2, GPLv3, and LGPLv3. **This
project uses Qt under the GNU Lesser General Public License version 3
(`LGPL-3.0-only`).** No commercial Qt license is used, and no Qt source is
modified. The full LGPLv3 text is in [`LICENSE`](LICENSE); LGPLv3 incorporates
GPLv3 by reference, and that text is in [`COPYING.GPL3`](COPYING.GPL3).

### Modules linked

Declared in [`CMakeLists.txt`](CMakeLists.txt):

- `Qt6::Core`, `Qt6::Gui`, `Qt6::Network`, `Qt6::Svg`
- `Qt6::Qml`, `Qt6::Quick`, `Qt6::QuickControls2`
- `Qt6::DBus`, for the screensaver inhibit, the BlueZ adapter probe and the
  desktop-settings portals

The Qt Quick runtime resolves further Qt libraries and QML plugin modules at
run time (`Qt6QmlModels`, `Qt6QuickControls2Basic`, `Qt6QuickEffects`,
`Qt6QuickLayouts`, `Qt6QuickShapes`, `Qt6QuickTemplates2`, plus the `QtQml` and
`QtQuick` module trees). All are part of Qt and carry the same license, and all
come from the system Qt.

Build-time only, not redistributed: `Qt6::LinguistTools` (`lupdate`, `lrelease`),
`qmllint`, `qmlcachegen`, `qmltyperegistrar`.

Qt modules that are GPL-only rather than LGPL, such as Qt Charts, are not used.

### TLS

`Qt6::Network` reaches the satellite's HTTPS API through whichever TLS backend
the system Qt was configured with, which on essentially every distribution is
OpenSSL. Nothing is bundled here: that OpenSSL is your distribution's, and its
attribution travels with the distribution's own Qt and OpenSSL packages rather
than with this repository.

### Third-party code inside Qt

A Qt build embeds or links further third-party libraries (FreeType, HarfBuzz,
PCRE2, zlib, libpng, libjpeg-turbo, Brotli, double-conversion, md4c, and others,
depending on how it was configured). Their notices belong to the Qt build you
run against, and The Qt Company documents them at
<https://doc.qt.io/qt-6/licenses-used-in-qt.html>. On a distribution package
that attribution travels with the distribution's Qt, not with this repository.

### The LGPL position, stated precisely

`dish` is a "Combined Work" in the sense of LGPLv3 section 4: our own code plus
the Qt libraries. Linking is entirely dynamic, against the system Qt. No Qt code
is statically linked into the binary.

**If you redistribute `dish`, or any build of it, you must:**

1. Give prominent notice with each copy that Qt is used and is covered by the
   LGPL. Shipping this file alongside the binary does that.
2. Ship a copy of the GNU LGPLv3 and the GNU GPLv3 with the binary. Those are
   [`LICENSE`](LICENSE) and [`COPYING.GPL3`](COPYING.GPL3) in this repository.
3. Preserve the copyright notices in the material you redistribute, and include
   a reference to the LGPL in your documentation.
4. Let the recipient replace Qt. Both routes in LGPLv3 section 4(d) are open:
   - Section 4(d)(1): Qt is used through a shared library mechanism, so a
     recipient can point the binary at their own build of a compatible Qt 6
     without touching our code.
   - Section 4(d)(0): the complete corresponding source is this public
     repository under LGPL-3.0-or-later, so a recipient can rebuild and relink
     the whole thing themselves.
5. Not strip or obscure the license notices, and not add terms that restrict
   these rights.

If you fork this project and make the fork's source unavailable, you break this
repository's own license. Do not do that.

Nothing here requires a user of the released binary to do anything. These are
obligations on redistribution.

---

## 3. Linked system libraries

SDL2 and libsodium are found through `pkg-config` and dynamically linked against
whatever the build host provides. Neither is vendored into this tree and neither
is bundled with the binary.

### SDL2

Simple DirectMedia Layer. SPDX `Zlib`. Upstream:
<https://github.com/libsdl-org/SDL>.

Used for controller enumeration, input polling, motion, rumble and light-bar
output, and battery reporting on every path other than the raw-HID USB-direct
path.

```
Simple DirectMedia Layer
Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

### libsodium

libsodium. SPDX `ISC`. Upstream:
<https://libsodium.org/>.

Used for the pairing key derivation, the HKDF-SHA256 per-session key schedule,
and the ChaCha20-Poly1305 AEAD on the UDP data plane.

```
ISC License

Copyright (c) 2013-2026 Frank Denis <j at pureftpd dot org>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```


## 4. Inter

Inter 4.001, by Rasmus Andersson and the Inter Project Authors. SPDX `OFL-1.1`.
Upstream: <https://rsms.me/inter/>.

Four faces are bundled and embedded into `dish` as Qt resources by
`packaging/dish.qrc`: Regular, Medium, SemiBold, Bold. They are loaded at startup
through `QFontDatabase` so the UI matches the design system on machines without
Inter installed.

The full license is at
[`packaging/fonts/Inter-LICENSE.txt`](packaging/fonts/Inter-LICENSE.txt) and is
embedded in the binary alongside the fonts at `:/fonts/Inter-LICENSE.txt`.

```
Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)
```

What the OFL requires here:

- The fonts may be bundled and redistributed with this software, including
  commercially, because they are not being sold on their own (OFL condition 1).
- **Every copy that includes the fonts must include the copyright notice and the
  license text** (OFL condition 2). Keep `Inter-LICENSE.txt` with any
  distribution you make. The `.txt` is embedded in the executable as a resource,
  but a copy as a plain file next to the binary is the safer reading of the
  requirement and is what a redistributor should ship.
- "Inter" is not a Reserved Font Name in this license file, but the faces are
  shipped unmodified, so condition 3 does not bite either way. If you modify
  them, rename them.
- The fonts must stay under the OFL. Being bundled with LGPL software does not
  relicense them, and the OFL does not reach the application code.

---

## 5. Catch2

Catch2 v3.5.4. SPDX `BSL-1.0`. Upstream: <https://github.com/catchorg/Catch2>.

**Test-only. Not linked into `dish`.** It
is resolved by `tests/CMakeLists.txt` through `find_package(Catch2 3)` with a
`FetchContent` fallback pinned to tag `v3.5.4`, and it links only into the
`DishTests` executable, which is built when `DISH_BUILD_TESTS=ON` and is switched
off for the release configuration.

It is listed here because it appears in the in-app licenses manifest. Someone
redistributing the application binary has no Catch2 obligation.

---

## 6. Reused facts, not reused code

Two places in this repository reuse published device-protocol facts. No
third-party source is compiled in either case, and the values were remapped onto
this project's own decoders.

### SDL default motion scaling

`switchGyroToWire` and `switchAccelToWire` in
[`src/core/input/UsbReportParsers.h`](src/core/input/UsbReportParsers.h) follow
SDL's default IMU scaling for controllers whose factory calibration this project
does not read: gyro raw divided by 14.2842 to get degrees per second, accel raw
divided by 4096 to get g. In the source those appear pre-multiplied into the wire
scale as the divisors 28568 and 16384. That is reuse of two constants. SDL's zlib
notice is reproduced in [section 3](#sdl2), and SDL is a linked dependency of this
project in any case.

### Linux kernel HID drivers

The per-model input-report byte layouts in the same header, for DualShock 4,
DualSense and Switch Pro, follow the layouts documented in the upstream Linux
kernel HID drivers `drivers/hid/hid-playstation.c` and `drivers/hid/hid-nintendo.c`.
Only offsets and field meanings were used. The Linux kernel is licensed
`GPL-2.0-only`; upstream is <https://github.com/torvalds/linux>. No kernel code
is compiled into this project, and none of the kernel's rumble output sequences
are used here, because the hidraw path is input-only apart from the Steam
Controller's documented configuration feature reports.

The dish-android repository carries the same attribution for the parsers these
were mirrored from.

---

## 7. First-party artwork

The brand iconography under `resources/brand/` (the dish, satellite, bluetooth,
gear, pad and rail glyph families and their state variants),
`packaging/dish.svg` and `packaging/dish.png` are original TinkerNorth work,
shared with the sibling Dish clients. They are covered by this repository's
license and carry no third-party attribution.

---

## 9. ENet

The cgutman fork of ENet, SPDX `MIT`. Upstream:
<https://github.com/cgutman/enet>, vendored at commit
`4cde9cc3dcc5c30775a80da1de87f39f98672a31` (the commit Wolf and the Moonlight
ecosystem pin). Original ENet by Lee Salzman: <https://github.com/lsalzman/enet>.

The Moonlight control stream runs over this reliable-UDP library. The unmodified
upstream C sources live under [`third_party/enet/`](third_party/enet/) and are
compiled into `dish` as the static `dish_enet` library. Because the sources are
bundled and redistributed inside the binary, the MIT copyright and permission
notice ([`third_party/enet/LICENSE`](third_party/enet/LICENSE)) must travel with
any copy.

```
Copyright (c) 2002-2020 Lee Salzman

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction ... THE SOFTWARE IS PROVIDED "AS IS".
```

The fork adds IPv4/IPv6 dual-stack support over upstream ENet; no local
modifications were made to the vendored sources.

---

## 10. OpenSSL libcrypto

OpenSSL, SPDX `Apache-2.0`. Upstream: <https://www.openssl.org/>.

The Moonlight-host support needs AES-128 (ECB and GCM), RSA sign/verify and
self-signed X.509 generation — primitives libsodium deliberately does not
provide — so `dish` links the system OpenSSL's `libcrypto` for them (only
`libcrypto`; the TLS client itself remains Qt Network's). Nothing is bundled:
this is your distribution's OpenSSL, and its notice travels with that package,
as it already does for the Qt build `Qt6::Network` runs against.

---

## 11. Wolf (Moonlight protocol reference)

Wolf, SPDX `MIT`. Upstream: <https://github.com/games-on-whales/wolf>.

**No Wolf source is compiled into `dish`.** Wolf is an MIT-licensed Moonlight
*host*; its protocol documentation and source were the reference for this
project's own Moonlight *client* implementation under `src/core/moonlight/` and
`src/source/moonlight/`. Adapted logic includes the control-packet AES-GCM IV
construction, the 5-phase PIN pairing algorithm, the CONTROLLER_* wire struct
layouts and the RTSP request/response shapes. These were re-implemented against
Dish's own architecture; the byte-exact test fixtures are derived from Wolf's
protocol docs and its published test vectors.

```
Copyright (c) 2021-2024 Games on Whales

Permission is hereby granted, free of charge ... THE SOFTWARE IS PROVIDED "AS IS".
```

Deliberately NOT consulted, to keep this LGPL-3.0 project clear of GPL-3.0
Moonlight code: moonlight-common-c, moonlight-qt, moonlight-android, Sunshine,
Apollo. Wolf's documentation and MIT source were sufficient.

---

## Keeping this in sync

[`assets/licenses/licenses.json`](assets/licenses/licenses.json) is the manifest
the in-app Licenses screen renders, parsed by `src/UI/licenses/LicenseManifest.*`.
It is hand-authored, not generated, so it can drift. It currently lists Qt 6,
SDL2, libsodium, Catch2 and Inter, which is the same set as this file, with the
same licenses. One difference is worth knowing about:

- The manifest lists Catch2, which is test-only and is not in the shipped
  binary. Showing it to a user is harmless but inaccurate.

When a dependency is added, changed or dropped, update
[`CMakeLists.txt`](CMakeLists.txt), the manifest, and this file together.

---

## Reporting an attribution problem

If something is missing, misattributed, or wrong here, open an issue or email
`security@tinkernorth.com`. See [`SECURITY.md`](SECURITY.md) for disclosure
handling and [`CONTRIBUTING.md`](CONTRIBUTING.md) for the license-header policy
applied to contributions.
