# Dish for Linux: Privacy Policy

**Effective date:** 2026-08-04.
**Hosted copy:** [`https://dish.tinkernorth.com/privacy/dish-linux/`](https://dish.tinkernorth.com/privacy/dish-linux/).
The hosted copy at that URL is the canonical version; this file mirrors it
in-repo so the code and the policy ship together. The app links to the hosted
URL from Help.

This document describes what data the Dish Linux client collects, why, how
long it is retained, and the choices you have over it. The product as a whole
spans several repositories (`satellite`, `dish-android`, `dish-windows`,
`dish-linux`, `dish-mac`); this policy is specific to the Linux desktop
client. The server (`satellite`) runs on your own machine and does not transmit
data off your local network. The Android client has a separate policy and
different behaviour, so do not read one as describing the other.

---

## 1. Short version

- Dish for Linux turns a Linux machine into a wireless gamepad for a
  `satellite` server. Controller input goes from your machine, over your own
  network, to your own `satellite` host. It does not stream to any
  TinkerNorth-operated server. TinkerNorth does not operate a server for
  Dish at all.
- **Nothing is transmitted to the authors or to any third party**, except an
  optional update check against GitHub, described in section 2.4, which you
  can turn off. There is no analytics SDK, no telemetry, no advertising
  identifier, no usage reporting, and no automatic error upload in this
  client. The update check asks GitHub for one file and sends no identifier
  with the request.
- Crash diagnostics are written **to your own disk only**, at
  `$XDG_STATE_HOME/dish/crash.log` (`~/.local/state/dish/crash.log` by
  default). They are never uploaded. If you want a maintainer to see one, you
  attach it to an issue yourself.
- Settings, remembered servers, and pairing keys live in your own home
  directory under `~/.config/`. Nothing is synced to a cloud account by the
  app.
- We do not sell, share, or rent your data. We do not show ads. We do not
  profile you.

---

## 2. What data is processed

### 2.1 Stays on your PC

Persisted with `QSettings`, which on Linux writes INI files under
`$XDG_CONFIG_HOME` (`~/.config` by default). The cross-client settings schema
lands in `~/.config/Dish/Dish.conf`. Preferences that only exist on the desktop
clients are written through the app's default organisation name and land in
`~/.config/TinkerNorth/Dish.conf` instead: the navigation preference
`ui_rail_collapsed` and the update settings listed after the table.

| Setting | Holds | Used for |
|---|---|---|
| `satellite_list` | JSON array of remembered satellites: display name, IP, UDP port, HTTPS port, and the server's machine id | Reconnecting to hosts you already paired with |
| `satellite_shared_key:<id>` | **The libsodium-derived pairing key for that satellite**, hex encoded | Deriving the per-session ChaCha20-Poly1305 key for the gamepad wire protocol. This is secret material. Anyone who can read your config directory can read it. |
| `satellite_cert_pin:<ip>` | SHA-256 fingerprint of the satellite's self-signed TLS certificate | Trust-on-first-use pinning, so a later HTTPS call is talking to the same box |
| `deviceId` | A random UUID generated on first run, with the dashes stripped | A stable per-install identifier the satellite uses to recognise this client across restarts and IP changes. It is sent **only** to satellites you pair with, never to us. |
| `deadzone:<deviceId>` | Per-controller stick and trigger deadzone profile | Restoring your calibration |
| `motion_enabled:<slotId>`, `motion_preferences`, `touchpad_mode_preferences` | Per-slot motion and touchpad routing toggles | Restoring your setup |
| `usb_path_choices` | Per `vid:pid` choice between the SDL path and the USB-direct raw-HID path | Restoring your setup |
| `joystick_remaps` | Per-device button, stick, and trigger remapping | Restoring your setup |
| `feature_lightbar_mode` | `followGame` or `off` | Light-bar behaviour |
| `theme_mode` | `system`, `light`, or `dark` | Appearance |
| `onboarding_welcome_completed`, `onboarding_dashboard_hint_dismissed` | Booleans | Not showing first-run screens again |
| `crashlytics_collection_enabled` | Boolean, default `true` | Records the state of the *Share crash reports* toggle. See section 3, which explains why this currently has no external effect. The key name is inherited from the Android client for schema continuity; there is no Crashlytics in this client. |
| `ui_rail_collapsed` | Boolean | Navigation-rail width |

Legacy values `wifi_list` and `wifi_shared_key/<id>` from older builds are
migrated in place on first run so you do not have to re-pair.

These three values live in `~/.config/TinkerNorth/Dish.conf` and cover the
update check described in section 2.4. They record your choice and the app's
own bookkeeping. None of them is transmitted anywhere.

| Setting | Holds | Used for |
|---|---|---|
| `updates_check_enabled` | Boolean, default `true` | The *Check for updates automatically* switch. When off, the app makes no update-related network request at all. |
| `updates_skipped_version` | A version string, default empty | The one version you pressed *Skip this version* on, so it stops being offered |
| `updates_last_check_utc_ms` | Timestamp | Not checking more than once an hour at startup |

There is no downloaded-update folder, because Dish for Linux never downloads or
installs an update; see section 2.4.

The app also **reads** (never writes) the XDG desktop portals for your
light/dark preference and your reduced-motion preference, and reads
`/sys/class/bluetooth` and BlueZ over the system bus to tell "no Bluetooth
adapter" from "adapter switched off".

Held in memory only, never written and never transmitted anywhere except to
the satellite you bound the controller to:

- Gamepad input events: buttons, sticks, triggers, motion, touchpad.
- Battery readings reported by a connected controller.
- Raw HID reports read from a controller in USB-direct mode.

The app reads input from connected game controllers, through SDL2 and, in
USB-direct mode, by reading raw HID input reports itself. It also writes back to
them: rumble and light-bar colour are sent through SDL2. The USB-direct path is
read-only. All of this is device IO on your own machine. It requires no
administrator rights and no driver install.

### 2.2 Sent to your own LAN, not to TinkerNorth

Everything the app puts on the network goes to a `satellite` server you chose,
on an address you can see in the app.

- **Discovery.** When you scan, the app sends a multicast DNS PTR query for
  `_satellite._udp.local.` to `224.0.0.251:5353`, and listens for the legacy
  UDP broadcast beacon on port 9879. The mDNS query contains the service name
  it is asking about and nothing about you. Both stay on your local network.
- **Pairing.** When you pair, the app makes an HTTPS `POST` to
  `/api/pair` on the satellite (port 9443 by default). The satellite presents
  a self-signed certificate, which the app pins on first use and checks on
  every later call. The request carries your device id, a device label, and
  either the PIN shown on the satellite or a PIN the app displays for you to
  type there. The response carries the shared pairing key, which is then
  stored as described above. `DELETE /api/pair` unpairs, which also removes
  the server-side record.
- **Control plane.** Once paired, the app calls the satellite's HTTPS API on
  the same port: `PUT`/`DELETE /api/connections` and
  `/api/connections/<id>/controllers/<slot>` to declare which controllers are
  bound and how they should be emulated, `GET /api/server/capabilities`, and
  `GET /api/catalog` for the localised list of emulatable controller types.
  These carry your device id, an HMAC proof computed from the pairing key, and
  your controller topology.
- **Gamepad stream.** The data plane is raw UDP to the satellite, port 9876
  by default, encrypted and authenticated with ChaCha20-Poly1305 under a
  session key derived from the pairing key (the pairing key itself never
  travels over UDP). Upstream frames carry controller state, motion, battery,
  and touchpad. Downstream frames carry rumble, light-bar colour, heartbeat
  acknowledgements, and session close.

A host firewall may need to allow the app to send and receive on your local
network. That affects your LAN only.

### 2.3 Sent to TinkerNorth or a third party

Nothing goes to TinkerNorth. There is no TinkerNorth server for this app to
talk to.

One thing leaves your network by default, and it goes to GitHub rather than to
us: the update check described in **section 2.4**. It is a plain HTTPS request
for a file on the public releases page, it carries no identifier, and you can
turn it off.

Beyond that there is no analytics library, no crash-reporting service, no
error-tracking SDK, and no advertising identifier. Apart from the updater, the
only component in the app that makes an outbound HTTPS request is the satellite
API client, and it only ever addresses the IP of a satellite you selected.

The other way this app causes a request to a TinkerNorth or third-party
address is if you click a link, which hands the URL to your default browser
and is then between you and that site. Those links are:

| Where | Opens |
|---|---|
| Help, privacy policy | `https://dish.tinkernorth.com/privacy/dish-windows/` |
| Help, project page | `https://github.com/TinkerNorth` |
| Welcome, Connections, and the setup wizard, when you have no server yet | `https://dish.tinkernorth.com/downloads/satellite` |
| Support links | `https://github.com/sponsors/TinkerNorth`, `https://ko-fi.com/tinkernorth`, `https://buymeacoffee.com/tinkernorth` |

The app does not embed a browser and does not pass any identifier along with
these URLs.

### 2.4 Update check (GitHub)

When *Check for updates automatically* is on, which is the default, the app
sends an HTTPS `GET` to `github.com` for the file `latest.json` attached to the
newest release of this project. GitHub answers with a redirect to its own
download CDN, so the request finishes against
`objects.githubusercontent.com`. The file is about a kilobyte of JSON: a version number, a minimum supported
version, and a link to the release notes.

**When it happens.** About 15 seconds after you launch the app, at most once
an hour; every four hours while the app stays open; and whenever you press
*Check for updates* in Settings. Nothing runs while the app is closed. There is
no service, no scheduled task, and no update agent.

**What the request carries.** Your IP address, as GitHub sees it, which is
unavoidable for any HTTPS request. A `User-Agent` header of the form
`Dish/<version> (Linux; x86_64)`. Nothing else. No device id, no account data,
no settings, no usage data, no cookies, no query parameters, and no
conditional-request identifiers. GitHub's own
[privacy statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement)
governs what GitHub logs about the request.

**Nothing is downloaded.** Dish for Linux never downloads or installs its own
update: your package manager owns the binary. If the release is newer, the app
shows a pill linking to the release page and stops. That one `GET` for
`latest.json` is the entire network footprint of the feature.

**Turning it off.** *Check for updates automatically* is the master switch. Off
means the app makes no update-related network request of any kind, arms no
timer, and creates no network stack for it.

The settings behind that switch are in section 2.1.

---

## 3. Crash reports

This is the part most likely to be misread, so it is stated plainly.

**No crash report is uploaded anywhere.** The app installs handlers for the
fatal signals that carry a usable stack. When the process crashes it appends a
backtrace to `$XDG_STATE_HOME/dish/crash.log` (`~/.local/state/dish/crash.log`
by default) and to stderr, then re-raises on the default disposition so your
shell's exit status and any core-dump collector still see the real signal.

The log holds the signal name and a symbolised stack. It does not contain a
memory image. Note that the re-raise means your system's own core-dump
collector may still write a full core — `systemd-coredump`, `apport` and
`abrt` all do by default — and a core IS a memory image, so it can contain
whatever the app held at that moment, including the satellite address and, in
principle, key material. That file belongs to your system's collector, not to
Dish; treat it as sensitive before attaching it to a public issue.

Delete `~/.local/state/dish/` at any time; the app recreates it only if it
crashes again.

**The *Share crash reports* toggle.** Settings has a *Share crash reports*
switch, on by default. Today it records your choice in your config and hands
it to an internal no-op backend that only writes a line to the app's debug log
category `dish.crash`. It does not enable or disable any upload, because
there is no upload path to enable. The switch exists so that the preference,
its default, and the plumbing are in place before a crash backend is chosen;
the in-app description of the switch describes that intended future behaviour
rather than what ships today. If and when a real backend is added, this policy
will be updated first and the change will be called out in the release notes.

---

## 4. System capabilities the app uses

Linux desktop apps do not declare a permission manifest the way Android apps
do, so this is the equivalent list of what the app touches.

| Capability | Why |
|---|---|
| Network sockets: UDP multicast, UDP broadcast listen, UDP unicast, TLS over TCP | Discovery, pairing, control plane, gamepad stream. All to your LAN. |
| HTTPS to `github.com` | The update check, and only the check. Section 2.4. Only while *Check for updates automatically* is on. |
| Game-controller device enumeration and IO | Reading controller input, and writing rumble and light-bar output back to the controller. `/dev/hidraw*` access is used for reading in USB-direct mode, which needs the udev rule the package installs. |
| `org.freedesktop.ScreenSaver.Inhibit` on the session bus | Preventing sleep while a controller is actively streaming, so input latency stays low. Released when streaming stops. |
| XDG desktop portals (appearance, settings) | Following your light/dark and reduced-motion preferences. Read only. |
| `/sys/class/bluetooth` and BlueZ on the system bus | Telling "no Bluetooth adapter" from "adapter switched off" in the setup wizard. Read only. |
| `~/.config/` read and write | Settings and pairing state, as listed in section 2.1. |
| `$XDG_STATE_HOME/dish/` file write | The crash log described in section 3. |
| Default browser launch | Opening the links listed in section 2.3, only when you click one. |

The app runs as a normal user. It never asks for root, installs no service and
no driver, does not read your files, and does not access the microphone,
camera, location, contacts, or clipboard. USB-direct needs read/write on the
hidraw node, which the packaged udev rule grants to your logged-in seat; if
that rule is absent the claim simply fails and the pad keeps streaming over
SDL.

---

## 5. Your choices

- **Forget a satellite.** Removing a satellite deletes its remembered row,
  its stored pairing key, and its certificate pin from your config, and
  unpairs on the server so any live session is closed there too.
- **Turn off crash file writing.** There is no toggle for this today because
  the file never leaves your machine. You can delete
  `~/.local/state/dish/` whenever you like. Your system's own core-dump
  collector is configured separately, outside Dish.
- **Stop the update check.** Settings, Updates, *Check for updates
  automatically*. Off means no update-related request leaves your machine, at
  any time, for any reason.
- **Uninstall.** However you installed it — your package manager, `flatpak
  uninstall`, or removing the files a `cmake --install` placed. Your settings,
  pairings and crash log are deliberately left behind so that reinstalling
  restores your setup; the *Wipe everything* step below removes those too.
- **Wipe everything.** Delete `~/.config/Dish/Dish.conf`,
  `~/.config/TinkerNorth/Dish.conf` and `~/.local/state/dish/`. That removes
  every remembered server, pairing key, certificate pin, preference, update
  setting, and crash artifact. There is no server-side record to delete,
  because there is no TinkerNorth server.
- **Verify any of this.** The client is free software under
  [LGPL-3.0-or-later](LICENSE). Every claim above is checkable in this
  repository, and you can build the binary yourself. See
  [`CONTRIBUTING.md`](CONTRIBUTING.md).

---

## 6. Children's privacy

Dish is suitable for general audiences. The app collects nothing from anyone,
of any age, so there is no children's data for us to hold. If you believe
that is wrong in some way we have not anticipated, contact
`privacy@tinkernorth.com`.

---

## 7. International transfers

None. No personal data leaves your machine to us, so there is nothing to
transfer across a border.

---

## 8. Changes to this policy

We will update the *Effective date* at the top whenever this policy changes.
Material changes, in particular the addition of any crash-reporting or
analytics backend, will be made here before the code ships and will be called
out in that release's notes in [`CHANGELOG.md`](CHANGELOG.md). Previous
versions remain in the git history of this file.

---

## 9. Contact

- Privacy questions: `privacy@tinkernorth.com`
- Security disclosures: see [`SECURITY.md`](SECURITY.md)
- General contact and bug reports: open an issue in this repository
