# Packaging

How a build becomes an installed Dish, and the two facts about a Linux install
that shape the code: **Dish never updates itself**, and **USB-direct needs a
udev rule**.

## What `cmake --install` lays down

| Path | What | Why it is there |
|---|---|---|
| `${bindir}/dish` | the binary | |
| `${datadir}/applications/com.tinkernorth.Dish.desktop` | desktop entry | the app menu, and the Wayland window icon |
| `${datadir}/icons/hicolor/512x512/apps/com.tinkernorth.Dish.png` | raster icon | menus that do not read the scalable dir |
| `${datadir}/icons/hicolor/scalable/apps/com.tinkernorth.Dish.svg` | scalable icon | |
| `${sysconfdir}/udev/rules.d/70-dish-hidraw.rules` | hidraw access | without it every USB-direct claim fails `PermissionDenied` |

The reverse-DNS names are load-bearing, not style: Wayland matches a window to
its launcher by desktop-file id, which `main()` sets with
`QGuiApplication::setDesktopFileName`, and Flatpak requires the app id for both
the entry and the icon.

Everything else — the brand glyphs, Inter, the license manifest, the compiled
translation catalogues — is inside the binary as Qt resources, so there is no
runtime data directory to get out of sync with the executable.

## The udev rule is not optional

`/dev/hidraw*` is root-only by default. Dish's USB-direct path opens the node
read/write to claim a pad, so without
[`packaging/udev/70-dish-hidraw.rules`](../packaging/udev/70-dish-hidraw.rules)
the claim fails and the FSM keeps the pad on the SDL path. That is a *degraded*
outcome, not a broken one — the pad still streams, just rate-capped — and it is
why `HidrawGateway::claim` distinguishes `PermissionDenied` from `Busy`: the two
need different copy, and only one of them is fixable by the user.

Dish never asks for root and never installs the rule itself. A package installs
it; a from-source build installs it with:

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

A packager who does not want the check at all can ship with
`updates_check_enabled=false` seeded in the default config; the store reads it
at construction and the checker arms no timer.

## Qt version floor

Qt 6.7. The QML module uses `qt_standard_project_setup(REQUIRES 6.7)` and the
kit is written against that type surface.

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
