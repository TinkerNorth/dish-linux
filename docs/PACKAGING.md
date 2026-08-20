# Packaging

How a build becomes an installed Dish, and the two facts about a Linux install
that shape the code: **Dish never updates itself**, and **USB-direct needs a
udev rule**.

## What `cmake --install` lays down

| Path | What | Why it is there |
|---|---|---|
| `${bindir}/dish` | the binary | |
| `${datadir}/applications/com.tinkernorth.Dish.desktop` | desktop entry | the app menu, and the Wayland window icon |
| `${datadir}/metainfo/com.tinkernorth.Dish.metainfo.xml` | AppStream metadata | GNOME Software and KDE Discover list nothing without it, and Flathub refuses a submission |
| `${datadir}/icons/hicolor/scalable/apps/com.tinkernorth.Dish.svg` | scalable icon | what modern desktops actually draw |
| `${datadir}/icons/hicolor/{16,24,32,48,64,128,256,512}x…/apps/com.tinkernorth.Dish.png` | raster ladder | menus that do not read the scalable dir. Sizes below 512 are rendered from the SVG at build time and need `rsvg-convert`; without it only the scalable and 512 icons install |
| `${prefix}/lib/udev/rules.d/70-dish-hidraw.rules` | hidraw access | without it every USB-direct claim fails `PermissionDenied` |
| `${mandir}/man1/dish.1.gz` | man page | |
| `${datadir}/doc/dish/{LICENSE,COPYING.GPL3,THIRD_PARTY.md,Inter-LICENSE.txt,copyright,changelog.gz}` | licence texts | LGPL-3.0 §4(b) covers Qt, OFL-1.1 §2 covers the four Inter faces compiled in as resources, and Debian Policy 12.5 wants `copyright` under that name. Without these the package is not redistributable |

The reverse-DNS names are load-bearing, not style: Wayland matches a window to
its launcher by desktop-file id, which `main()` sets with
`QGuiApplication::setDesktopFileName`, and Flatpak requires the app id for the
entry, the icon and the AppStream file alike.

Everything else — the brand glyphs, Inter, the license manifest, the compiled
translation catalogues — is inside the binary as Qt resources, so there is no
runtime data directory to get out of sync with the executable.

`DISH_UDEV_RULES_DIR` moves the rule (default `lib/udev/rules.d`, relative to
the prefix) and `DISH_INSTALL_UDEV_RULES=OFF` drops it, which is what the
Flatpak and the AppImage do — neither can own a directory on the host. Configure
warns when the resolved path is not one udev scans, because the failure is
otherwise silent.

## Building the packages

Every artifact is generated from those `install()` rules through CPack, so a
package cannot disagree with `cmake --install` about what ships. That is not
hypothetical: the hand-rolled `.deb` this replaced copied two files and
therefore shipped no icon, no udev rule and no licence text.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr -DDISH_BUILD_TESTS=OFF
cmake --build build-release --parallel
cpack --config build-release/CPackConfig.cmake -G DEB   # or RPM, or TGZ
```

| Format | Built by | Targets | Notes |
|---|---|---|---|
| `.deb` | CPack `DEB`, in a `debian:trixie` container | Debian 13+, derivatives with Qt ≥ 6.7 | `dpkg-shlibdeps` computes the Qt/SDL/sodium versions; the QML modules and the platform plugin are listed by hand in `DISH_DEB_RUNTIME_DEPENDS` because nothing links them |
| `.rpm` | CPack `RPM`, in a `fedora` container | Fedora, RHEL, openSUSE | rpmbuild's soname scanner does the same job |
| AppImage | `scripts/build-appimage.sh` | everything else, including an LTS below the Qt floor | Cannot install a udev rule; carries it at `usr/share/dish/` and prints how to install it |
| Flatpak | `packaging/flatpak/com.tinkernorth.Dish.yml` on `org.kde.Platform//6.9` | old LTS, and anyone who wants the sandbox | Needs `--device=all` for hidraw; there is no hidraw portal |
| `.tar.gz` | CPack `TGZ` | packagers laying the tree down under their own prefix | |

Build each native package on the distro it targets. Both dependency scanners
read the built binary, so a `.deb` built against the aqtinstall Qt would demand
a Qt no Debian ships.

The `.deb` and `.rpm` release jobs **install their own package and launch it**
before uploading. Nothing else can stand in for that: unit tests, lints and a
file-contents check all pass against a build tree, and a build tree is exactly
where a QML module resolves from the Qt install instead of from a `Depends:`
line. dish-windows shipped two releases of a bundle that could not start for
precisely this reason.

Snap is deliberately not offered. `hidraw` would need a store-side auto-connect
grant before a fresh install worked at all, which is a poor fit for an app whose
headline feature is claiming gamepads, and the four formats above already reach
every desktop it would.

## The udev rule is not optional

`/dev/hidraw*` is root-only by default. Dish's USB-direct path opens the node
read/write to claim a pad, so without
[`packaging/udev/70-dish-hidraw.rules`](../packaging/udev/70-dish-hidraw.rules)
the claim fails and the FSM keeps the pad on the SDL path. That is a *degraded*
outcome, not a broken one — the pad still streams, just rate-capped — and it is
why `HidrawGateway::claim` distinguishes `PermissionDenied` from `Busy`: the two
need different copy, and only one of them is fixable by the user.

Dish never asks for root and never installs the rule itself. A package installs
it into `/usr/lib/udev/rules.d` (the vendor directory — `/etc` belongs to the
administrator); a from-source build with a prefix udev does not scan installs it
with:

```sh
sudo install -m 644 packaging/udev/70-dish-hidraw.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

The rule grants the logged-in seat through `uaccess`, with an `input`-group
fallback for headless and non-logind sessions.

## Dish does not update itself

The updater checks and stops. `UpdateChecker` fetches `latest.json`, compares
version triples, and the UI surfaces a pill linking to the release page — there
is no download gateway, no staging directory, and no boot-time apply, because
the package manager owns the binary and a self-applying updater would fight it.

This is the one deliberate divergence from dish-windows, whose installer-based
flow does download and apply. The shared reducer
([`core/reducer/UpdateMachine.h`](../src/core/reducer/UpdateMachine.h)) is
correspondingly smaller here: six reachable phases instead of nine.

`latest.json` is emitted by the `manifest` job in `.github/workflows/release.yml`
and published as a release asset under the fixed name the client fetches. That
job also refuses to run when the git tag and `project(Dish VERSION ...)`
disagree — a manifest naming a version the shipped binary does not report would
re-offer the same update forever. `minimumSupportedVersion` comes from
[`packaging/update-policy.json`](../packaging/update-policy.json), which is
reviewed input rather than something the job derives.

A packager who does not want the check at all can ship with
`updates_check_enabled=false` seeded in the default config; the store reads it
at construction and the checker arms no timer.

## Qt version floor

Qt 6.7. The QML module uses `qt_standard_project_setup(REQUIRES 6.7)` and the
kit is written against that type surface.

CI builds two minors above the floor (`.github/actions/setup-qt`) because the
translation gate needs `lupdate` 6.9 — see CONTRIBUTING.md. The code itself
still builds and tests clean at 6.7, which is what this table is about.

| Distro | Ships | Status |
|---|---|---|
| Debian 13 | 6.8 | fine |
| Fedora 40+ | 6.7 | fine |
| Arch | current | fine |
| Ubuntu 24.04 LTS | 6.4.2 | **too old** — needs a backport, or a Flatpak build |

Flatpak on the KDE runtime is the portable answer and is the recommended way to
ship to older LTS distros. Note that a sandboxed build needs `--device=all` (or
a narrower hidraw grant) for USB-direct, and the udev rule still has to exist on
the host.

## Desktop integration Dish relies on

Three portal-backed facts, each with a documented fallback so a minimal desktop
degrades rather than breaks:

| Feature | Source | Fallback |
|---|---|---|
| light/dark | `QStyleHints::colorScheme()` over the XDG appearance portal | dark |
| reduced motion | XDG settings portal (`org.gnome.desktop.interface enable-animations`), then `kdeglobals` | motion allowed |
| keep-awake | `org.freedesktop.ScreenSaver.Inhibit` on the session bus | the inhibit silently no-ops |

Bluetooth presence and power come from sysfs and BlueZ directly, not a portal,
because the wizard needs to tell "no adapter" from "adapter off" and only the
adapter itself can answer that.
